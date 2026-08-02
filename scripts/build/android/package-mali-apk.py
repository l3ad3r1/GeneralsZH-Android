#!/usr/bin/env python3
"""Assemble a Mali/TCL APK by grafting a freshly built libmain.so (and optionally
rebuilt FFmpeg libraries) onto a known-good base APK.

WHY THIS EXISTS
---------------
`gradlew assembleRelease` does NOT produce a runnable Mali APK. It builds
libmain.so correctly, but the graphics/codec runtime it packages is wrong for
this device:

  * it ships DXVK 2.6, which requires Vulkan 1.3 -- the Mali-G57 driver only
    offers 1.1, so the renderer cannot initialise at all; and
  * it omits the five FFmpeg libraries entirely.

A build made that way crashed on launch and replaced a working install once
already (see HANDOVER_AUDIO_VIDEO_STREAMING.md section 8).

So: take ONLY libmain.so from the Gradle output and keep every other native
library from the known-good base APK. Verify before writing.

USAGE
  package-mali-apk.py --base GOOD.apk --libmain path/to/libmain.so \
                      [--ffmpeg-dir DIR] --output OUT.apk [--strip STRIP_TOOL]
"""

import argparse
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

LIB = "lib/arm64-v8a/"
FFMPEG_LIBS = ["libavcodec.so", "libavformat.so", "libavutil.so",
               "libswresample.so", "libswscale.so"]
# DXVK Native 1.9.2b is ~2.6 MB; DXVK 2.6 is ~37 MB. Anything above this is wrong.
DXVK_D3D9_MAX = 5_000_000


def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument("--base", required=True, type=Path,
                   help="known-good Mali APK to graft onto")
    p.add_argument("--libmain", type=Path,
                   help="freshly built libmain.so (unstripped is fine, see --strip)")
    p.add_argument("--ffmpeg-dir", type=Path,
                   help="directory holding rebuilt FFmpeg .so files")
    p.add_argument("--output", required=True, type=Path)
    p.add_argument("--strip", type=Path,
                   help="llvm-strip binary; if given, libmain.so is stripped first")
    p.add_argument("--dxvk-from", type=Path,
                   help="APK to take the Mali DXVK libraries from. Use this when "
                        "--base is a FRESH Gradle APK (so its classes.dex and "
                        "manifest are current) and only the graphics runtime needs "
                        "swapping back to DXVK Native 1.9.2b.")
    p.add_argument("--require-dex-string", action="append", default=[], metavar="TEXT",
                   help="fail unless TEXT appears in the APK's classes.dex. Guards "
                        "against grafting onto a stale base whose Java code predates "
                        "the change being shipped (see the dex note below).")
    return p.parse_args()


def main():
    a = parse_args()
    if not a.base.is_file():
        sys.exit(f"base APK not found: {a.base}")

    replacements = {}
    tmpdir = None

    if a.libmain:
        if not a.libmain.is_file():
            sys.exit(f"libmain.so not found: {a.libmain}")
        src = a.libmain
        if a.strip:
            tmpdir = tempfile.mkdtemp()
            stripped = Path(tmpdir) / "libmain.so"
            subprocess.run([str(a.strip), "--strip-unneeded", str(src), "-o", str(stripped)],
                           check=True)
            print(f"stripped libmain.so: {src.stat().st_size} -> {stripped.stat().st_size} bytes")
            src = stripped
        replacements[LIB + "libmain.so"] = src

    if a.ffmpeg_dir:
        for name in FFMPEG_LIBS:
            p = a.ffmpeg_dir / name
            if not p.is_file():
                sys.exit(f"missing FFmpeg library: {p}")
            replacements[LIB + name] = p

    # Pull the Mali graphics runtime out of a donor APK. This is what lets --base
    # be the current Gradle output instead of an old known-good APK: grafting onto
    # an old base also inherits its classes.dex and AndroidManifest, so any Java
    # change (a new Activity, a manifest entry) is silently dropped while every
    # native check still passes. That happened once with the launcher.
    if a.dxvk_from:
        if not a.dxvk_from.is_file():
            sys.exit(f"--dxvk-from APK not found: {a.dxvk_from}")
        if not tmpdir:
            tmpdir = tempfile.mkdtemp()
        with zipfile.ZipFile(a.dxvk_from) as donor:
            for name in ("libdxvk_d3d8.so", "libdxvk_d3d9.so"):
                entry = LIB + name
                if entry not in donor.namelist():
                    sys.exit(f"--dxvk-from APK has no {entry}")
                out = Path(tmpdir) / name
                out.write_bytes(donor.read(entry))
                replacements[entry] = out
                print(f"dxvk from donor: {name} = {out.stat().st_size} bytes")

    if not replacements:
        sys.exit("nothing to do: pass --libmain and/or --ffmpeg-dir")

    with zipfile.ZipFile(a.base) as zin, \
         zipfile.ZipFile(a.output, "w", allowZip64=True) as zout:
        names = set(zin.namelist())
        for want in replacements:
            if want not in names:
                sys.exit(f"base APK has no entry {want} -- wrong base?")
        for item in zin.infolist():
            if item.filename.startswith("META-INF/"):
                continue  # old signature; re-sign after zipalign
            repl = replacements.get(item.filename)
            data = repl.read_bytes() if repl else zin.read(item.filename)
            zi = zipfile.ZipInfo(item.filename, date_time=item.date_time)
            zi.compress_type = item.compress_type
            zi.external_attr = item.external_attr
            zout.writestr(zi, data, compress_type=item.compress_type)

    # ---- post-write verification (the build guard, enforced in code) ----
    problems = []
    with zipfile.ZipFile(a.output) as z:
        names = set(z.namelist())
        for name in FFMPEG_LIBS:
            if LIB + name not in names:
                problems.append(f"MISSING FFmpeg library: {name}")
        d9 = z.getinfo(LIB + "libdxvk_d3d9.so").file_size
        if d9 > DXVK_D3D9_MAX:
            problems.append(f"libdxvk_d3d9.so is {d9} bytes -- that is DXVK 2.6, "
                            f"which cannot work on a Vulkan 1.1 Mali device")
        main_sz = z.getinfo(LIB + "libmain.so").file_size

        # Shipping the FFmpeg .so files is not enough -- libmain.so has to be
        # BUILT against them. If -DSAGE_ANDROID_FFMPEG_DIR is not passed to
        # CMake, Core/GameEngineDevice quietly takes its "no FFmpeg" branch and
        # compiles FFmpegFileStub.cpp + BinkVideoPlayerStub.cpp instead. The
        # build still reports SUCCESS, the APK still contains all five FFmpeg
        # libraries, and the game still runs -- but with no video and no audio
        # decoding whatsoever. That shipped once; this check is why it cannot
        # ship again.
        #
        # Checked by scanning the .so bytes so no NDK binutils are required:
        # the DT_NEEDED names live in .dynstr as plain NUL-terminated strings,
        # and the stub's log message is a string literal only the stub has.
        main_bytes = z.read(LIB + "libmain.so")
        for name in FFMPEG_LIBS:
            if name.encode() not in main_bytes:
                problems.append(
                    f"libmain.so does not link {name} -- built WITHOUT FFmpeg "
                    f"(is -DSAGE_ANDROID_FFMPEG_DIR set in android/app/build.gradle?)")
        if b"Linux stub (video playback not available)" in main_bytes:
            problems.append(
                "libmain.so contains BinkVideoPlayerStub -- built WITHOUT FFmpeg; "
                "videos and audio will silently not play")

        # Stale-dex guard. Every other check here inspects native code, so an APK
        # grafted onto an old base passes them all while shipping outdated Java.
        if a.require_dex_string:
            dex = b"".join(z.read(n) for n in z.namelist() if n.endswith(".dex"))
            for needed in a.require_dex_string:
                if needed.encode() not in dex:
                    problems.append(
                        f"classes.dex does not contain {needed!r} -- the base APK's "
                        f"Java code is stale. Rebuild, or use --base <fresh APK> "
                        f"with --dxvk-from <old Mali APK>.")

        print(f"verify: libmain.so={main_sz}  libdxvk_d3d9.so={d9}  "
              f"ffmpeg={'all present' if not problems else 'PROBLEM'}")

    if tmpdir:
        shutil.rmtree(tmpdir, ignore_errors=True)

    if problems:
        a.output.unlink(missing_ok=True)
        sys.exit("REFUSING TO EMIT APK:\n  " + "\n  ".join(problems))

    print(f"wrote {a.output} -- now: zipalign -f -P 16 4, then apksigner sign")


if __name__ == "__main__":
    main()
