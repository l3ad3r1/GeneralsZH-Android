// GeneralsX @feature android-port 30/07/2026
// D3D8 -> d3d8to9 -> DXVK Native -> Mali Vulkan presentation gate.

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <d3d8.h>

#include <android/log.h>
#include <cstdlib>

extern "C" int SDL_main(int argc, char** argv) {
  setenv("DXVK_LOG_PATH", "/data/data/me.generalsx.malispike/files", 1);
  setenv("DXVK_LOG_LEVEL", "info", 1);
  setenv("DXVK_STATE_CACHE_PATH", "/data/data/me.generalsx.malispike/cache", 1);

  if (!SDL_Init(SDL_INIT_VIDEO))
    return 1;

  SDL_Window* window = SDL_CreateWindow(
    "GeneralsX Mali D3D8 Spike", 1280, 720,
    SDL_WINDOW_VULKAN | SDL_WINDOW_FULLSCREEN);
  if (!window) {
    SDL_Quit();
    return 2;
  }

  IDirect3D8* d3d = Direct3DCreate8(D3D_SDK_VERSION);
  if (!d3d) {
    __android_log_print(ANDROID_LOG_ERROR, "generalsx-mali",
      "Direct3DCreate8 returned null");
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 3;
  }

  D3DDISPLAYMODE displayMode = { };
  HRESULT hr = d3d->GetAdapterDisplayMode(0, &displayMode);
  const D3DFORMAT compressedFormats[] = {
    D3DFMT_DXT1,
    D3DFMT_DXT3,
    D3DFMT_DXT5,
  };
  const char* compressedNames[] = { "DXT1", "DXT3", "DXT5" };
  for (unsigned int i = 0; hr == D3D_OK && i < 3; i++) {
    const HRESULT formatHr = d3d->CheckDeviceFormat(
      0, D3DDEVTYPE_HAL, displayMode.Format, 0,
      D3DRTYPE_TEXTURE, compressedFormats[i]);
    __android_log_print(ANDROID_LOG_INFO, "generalsx-mali",
      "D3D8 %s texture support: 0x%08x (%s)",
      compressedNames[i],
      static_cast<unsigned int>(formatHr),
      formatHr == D3D_OK ? "native BC exposed" : "software decode required");
  }

  int width = 0;
  int height = 0;
  SDL_GetWindowSizeInPixels(window, &width, &height);

  D3DPRESENT_PARAMETERS present = { };
  present.Windowed = TRUE;
  present.SwapEffect = D3DSWAPEFFECT_DISCARD;
  present.hDeviceWindow = reinterpret_cast<HWND>(window);
  present.BackBufferFormat = D3DFMT_UNKNOWN;
  present.BackBufferWidth = static_cast<UINT>(width);
  present.BackBufferHeight = static_cast<UINT>(height);
  present.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;

  IDirect3DDevice8* device = nullptr;
  hr = d3d->CreateDevice(
    D3DADAPTER_DEFAULT,
    D3DDEVTYPE_HAL,
    reinterpret_cast<HWND>(window),
    D3DCREATE_HARDWARE_VERTEXPROCESSING,
    &present,
    &device);
  __android_log_print(ANDROID_LOG_INFO, "generalsx-mali",
    "D3D8 CreateDevice: 0x%08x", static_cast<unsigned int>(hr));

  uint32_t frames = 0;
  const Uint64 start = SDL_GetTicks();
  while (hr == D3D_OK && frames < 600) {
    const D3DCOLOR color = (frames / 120) % 2
      ? D3DCOLOR_XRGB(32, 112, 196)
      : D3DCOLOR_XRGB(196, 80, 32);
    hr = device->Clear(0, nullptr, D3DCLEAR_TARGET, color, 1.0f, 0);
    if (hr == D3D_OK)
      hr = device->Present(nullptr, nullptr, nullptr, nullptr);
    frames++;
  }

  const Uint64 elapsed = SDL_GetTicks() - start;
  const double fps = elapsed
    ? (1000.0 * static_cast<double>(frames)) / static_cast<double>(elapsed)
    : 0.0;
  __android_log_print(ANDROID_LOG_INFO, "generalsx-mali",
    "D3D8 rendered %u frames at %.1f FPS through native Mali Vulkan (%dx%d)",
    frames, fps, width, height);

  if (device)
    device->Release();
  d3d->Release();
  SDL_DestroyWindow(window);
  SDL_Quit();
  return hr == D3D_OK ? 0 : 4;
}
