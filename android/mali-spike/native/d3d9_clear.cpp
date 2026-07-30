// GeneralsX @feature android-port 30/07/2026
// Minimal D3D9 -> DXVK Native -> Mali Vulkan device/present gate.

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <d3d9.h>

#include <android/log.h>
#include <cstdlib>

namespace {

  void logResult(const char* step, HRESULT hr) {
    __android_log_print(
      hr == D3D_OK ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR,
      "generalsx-mali",
      "%s: 0x%08x",
      step,
      static_cast<unsigned int>(hr));
  }

}

extern "C" int SDL_main(int argc, char** argv) {
  setenv("DXVK_LOG_PATH", "/data/data/me.generalsx.malispike/files", 1);
  setenv("DXVK_LOG_LEVEL", "debug", 1);
  setenv("DXVK_STATE_CACHE_PATH", "/data/data/me.generalsx.malispike/cache", 1);

  if (!SDL_Init(SDL_INIT_VIDEO)) {
    __android_log_print(ANDROID_LOG_ERROR, "generalsx-mali",
      "SDL_Init failed: %s", SDL_GetError());
    return 1;
  }

  SDL_Window* window = SDL_CreateWindow(
    "GeneralsX Mali Spike", 1280, 720,
    SDL_WINDOW_VULKAN | SDL_WINDOW_FULLSCREEN);
  if (!window) {
    __android_log_print(ANDROID_LOG_ERROR, "generalsx-mali",
      "SDL_CreateWindow failed: %s", SDL_GetError());
    SDL_Quit();
    return 2;
  }

  IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!d3d) {
    __android_log_print(ANDROID_LOG_ERROR, "generalsx-mali",
      "Direct3DCreate9 returned null");
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 3;
  }

  HRESULT hr = D3D_OK;
  const UINT adapterCount = d3d->GetAdapterCount();
  __android_log_print(ANDROID_LOG_INFO, "generalsx-mali",
    "D3D9 adapters: %u", adapterCount);

  if (adapterCount) {
    D3DADAPTER_IDENTIFIER9 identifier = { };
    hr = d3d->GetAdapterIdentifier(0, 0, &identifier);
    logResult("GetAdapterIdentifier", hr);
    if (hr == D3D_OK)
      __android_log_print(ANDROID_LOG_INFO, "generalsx-mali",
        "Adapter: %s (vendor 0x%04x device 0x%04x)",
        identifier.Description, identifier.VendorId, identifier.DeviceId);

    D3DDISPLAYMODE displayMode = { };
    hr = d3d->GetAdapterDisplayMode(0, &displayMode);
    logResult("GetAdapterDisplayMode", hr);
    if (hr == D3D_OK) {
      const D3DFORMAT compressedFormats[] = {
        D3DFMT_DXT1,
        D3DFMT_DXT3,
        D3DFMT_DXT5,
      };
      const char* compressedNames[] = {
        "DXT1",
        "DXT3",
        "DXT5",
      };

      for (unsigned int i = 0; i < 3; i++) {
        const HRESULT formatHr = d3d->CheckDeviceFormat(
          0,
          D3DDEVTYPE_HAL,
          displayMode.Format,
          0,
          D3DRTYPE_TEXTURE,
          compressedFormats[i]);
        __android_log_print(
          formatHr == D3D_OK ? ANDROID_LOG_WARN : ANDROID_LOG_INFO,
          "generalsx-mali",
          "%s texture support: 0x%08x (%s)",
          compressedNames[i],
          static_cast<unsigned int>(formatHr),
          formatHr == D3D_OK
            ? "native BC unexpectedly exposed"
            : "software decode path required");
      }
    }
  }

  D3DPRESENT_PARAMETERS present = { };
  int drawableWidth = 0;
  int drawableHeight = 0;
  SDL_GetWindowSizeInPixels(window, &drawableWidth, &drawableHeight);
  present.Windowed = TRUE;
  present.SwapEffect = D3DSWAPEFFECT_DISCARD;
  present.hDeviceWindow = reinterpret_cast<HWND>(window);
  present.BackBufferFormat = D3DFMT_UNKNOWN;
  present.BackBufferWidth = static_cast<UINT>(drawableWidth);
  present.BackBufferHeight = static_cast<UINT>(drawableHeight);
  present.PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;

  IDirect3DDevice9* device = nullptr;
  hr = D3DERR_NOTAVAILABLE;
  const DWORD behaviorFlags[] = {
    D3DCREATE_HARDWARE_VERTEXPROCESSING,
    D3DCREATE_MIXED_VERTEXPROCESSING,
    D3DCREATE_SOFTWARE_VERTEXPROCESSING,
  };

  for (DWORD behavior : behaviorFlags) {
    hr = d3d->CreateDevice(
      D3DADAPTER_DEFAULT,
      D3DDEVTYPE_HAL,
      reinterpret_cast<HWND>(window),
      behavior,
      &present,
      &device);
    __android_log_print(ANDROID_LOG_INFO, "generalsx-mali",
      "CreateDevice behavior 0x%08x: 0x%08x",
      static_cast<unsigned int>(behavior),
      static_cast<unsigned int>(hr));
    if (hr == D3D_OK)
      break;
  }

  if (hr == D3D_OK) {
    bool running = true;
    uint32_t frames = 0;
    const Uint64 start = SDL_GetTicks();

    while (running && frames < 600) {
      SDL_Event event;
      while (SDL_PollEvent(&event))
        running &= event.type != SDL_EVENT_QUIT;

      const D3DCOLOR color = (frames / 120) % 2
        ? D3DCOLOR_XRGB(24, 96, 180)
        : D3DCOLOR_XRGB(180, 72, 24);
      hr = device->Clear(0, nullptr, D3DCLEAR_TARGET, color, 1.0f, 0);
      if (hr == D3D_OK)
        hr = device->Present(nullptr, nullptr, nullptr, nullptr);
      if (hr != D3D_OK) {
        logResult("Clear/Present", hr);
        break;
      }
      frames++;
    }

    const Uint64 elapsed = SDL_GetTicks() - start;
    const double fps = elapsed
      ? (1000.0 * static_cast<double>(frames)) / static_cast<double>(elapsed)
      : 0.0;
    __android_log_print(ANDROID_LOG_INFO, "generalsx-mali",
      "Rendered %u frames at %.1f FPS through native Mali Vulkan (%dx%d)",
      frames, fps, drawableWidth, drawableHeight);
    device->Release();
  }

  d3d->Release();
  SDL_DestroyWindow(window);
  SDL_Quit();
  return hr == D3D_OK ? 0 : 4;
}
