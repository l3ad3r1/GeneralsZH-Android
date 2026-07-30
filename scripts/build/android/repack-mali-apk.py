#!/usr/bin/env python3
"""Replace an Android APK's graphics stack with the native Mali test lane."""

# GeneralsX @build android-port 30/07/2026 Repack legacy DXVK Mali builds.

import argparse
import zipfile
from pathlib import Path


LIB_PREFIX = "lib/arm64-v8a/"
REPLACEMENTS = {
    f"{LIB_PREFIX}libdxvk_d3d8.so": "d3d8",
    f"{LIB_PREFIX}libdxvk_d3d9.so": "d3d9",
    f"{LIB_PREFIX}libSDL3.so": "sdl3",
}
SWIFTSHADER_ENTRY = f"{LIB_PREFIX}libvk_sw.so"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base", required=True, type=Path)
    parser.add_argument("--d3d8", required=True, type=Path)
    parser.add_argument("--d3d9", required=True, type=Path)
    parser.add_argument("--sdl3", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    libraries = {
        "d3d8": args.d3d8,
        "d3d9": args.d3d9,
        "sdl3": args.sdl3,
    }

    for path in (args.base, *libraries.values()):
        if not path.is_file():
            raise SystemExit(f"Input does not exist: {path}")

    replaced = set()
    with zipfile.ZipFile(args.base, "r") as source:
        with zipfile.ZipFile(args.output, "w", allowZip64=True) as destination:
            for entry in source.infolist():
                if entry.filename.startswith("META-INF/"):
                    continue
                if entry.filename == SWIFTSHADER_ENTRY:
                    continue

                replacement = REPLACEMENTS.get(entry.filename)
                if replacement:
                    destination.write(
                        libraries[replacement],
                        entry.filename,
                        compress_type=zipfile.ZIP_STORED,
                    )
                    replaced.add(replacement)
                else:
                    destination.writestr(entry, source.read(entry.filename))

    missing = set(libraries) - replaced
    if missing:
        args.output.unlink(missing_ok=True)
        raise SystemExit(
            "Base APK is missing graphics libraries: " + ", ".join(sorted(missing))
        )


if __name__ == "__main__":
    main()
