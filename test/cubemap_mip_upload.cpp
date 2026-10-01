#include <windows.h>
#include <d3d9.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <thread>
#include <atomic>

static void check(HRESULT hr, const char* call) {
  if (FAILED(hr)) { std::fprintf(stderr, "%s failed: %08lx\n", call, hr); std::exit(1); }
}
int main(int argc, char** argv) {
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  bool legacy = false;
  bool concurrent = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--legacy") == 0) legacy = true;
    else if (std::strcmp(argv[i], "--concurrent") == 0) concurrent = true;
    else { std::fprintf(stderr, "Unknown argument: %s\n", argv[i]); return 8; }
  }
  auto mod = LoadLibraryW(L"d3d9.dll");
  if (!mod) { std::fprintf(stderr, "LoadLibrary: %lu\n", GetLastError()); return 1; }
  auto create = reinterpret_cast<IDirect3D9* (WINAPI*)(UINT)>(GetProcAddress(mod, "Direct3DCreate9"));
  auto d3d = create(D3D_SDK_VERSION);
  if (!d3d) return 2;
  WNDCLASSW wc = {}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"BridgeMipRegression";
  RegisterClassW(&wc);
  auto hwnd = CreateWindowW(wc.lpszClassName, L"Bridge mip regression", WS_OVERLAPPEDWINDOW, 0, 0, 320, 240, nullptr, nullptr, wc.hInstance, nullptr);
  D3DPRESENT_PARAMETERS pp = {}; pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = hwnd; pp.BackBufferWidth = 320; pp.BackBufferHeight = 240; pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  IDirect3DDevice9* dev = nullptr;
  check(d3d->CreateDevice(0, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED, &pp, &dev), "CreateDevice");
  unsigned uploads = 0;
  if (concurrent) {
    IDirect3DSurface9* rt = nullptr;
    IDirect3DSurface9* staging = nullptr;
    check(dev->CreateRenderTarget(64, 64, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, nullptr), "CreateRenderTarget");
    check(dev->CreateOffscreenPlainSurface(64, 64, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &staging, nullptr), "CreateOffscreenPlainSurface");
    check(dev->ColorFill(rt, nullptr, 0xff123456), "ColorFill");
    std::atomic<bool> start = false;
    std::thread resourceThread([&] {
      while (!start.load()) std::this_thread::yield();
      for (unsigned i = 0; i < 2000; ++i) {
        IDirect3DVertexBuffer9* vb = nullptr;
        check(dev->CreateVertexBuffer(32768, 0, 0, D3DPOOL_MANAGED, &vb, nullptr), "Concurrent CreateVertexBuffer");
        vb->Release();
      }
    });
    std::thread readbackThread([&] {
      while (!start.load()) std::this_thread::yield();
      for (unsigned i = 0; i < 1000; ++i) {
        check(dev->GetRenderTargetData(rt, staging), "Concurrent GetRenderTargetData");
        D3DLOCKED_RECT lock = {};
        check(staging->LockRect(&lock, nullptr, D3DLOCK_READONLY), "Readback LockRect");
        if (*static_cast<DWORD*>(lock.pBits) != 0xff123456) {
          std::fprintf(stderr, "Readback pixel mismatch at iteration %u: %08lx\n", i, *static_cast<DWORD*>(lock.pBits)); std::exit(7);
        }
        check(staging->UnlockRect(), "Readback UnlockRect");
      }
    });
    start.store(true); resourceThread.join(); readbackThread.join();
    staging->Release(); rt->Release();
    std::printf("PASS: 2000 concurrent resource creations and 1000 pixel-verified render-target readbacks.\n");
  }
  for (D3DFORMAT fmt : {D3DFMT_DXT1, D3DFMT_DXT5, D3DFMT_A8R8G8B8}) {
    const bool compressed = fmt != D3DFMT_A8R8G8B8;
    const unsigned block = compressed ? 4 : 1;
    const unsigned bytes = fmt == D3DFMT_DXT1 ? 8 : fmt == D3DFMT_DXT5 ? 16 : 4;
    for (unsigned edge : {16u, 512u}) {
      IDirect3DCubeTexture9* cube = nullptr;
      check(dev->CreateCubeTexture(edge, 0, 0, fmt, D3DPOOL_MANAGED, &cube, nullptr), "CreateCubeTexture");
      const size_t topPitch = ((edge + block - 1) / block) * bytes;
      for (unsigned face = 0; face < 6; ++face) {
        for (unsigned level = 0; level < cube->GetLevelCount(); ++level) {
          D3DSURFACE_DESC desc = {};
          check(cube->GetLevelDesc(level, &desc), "GetLevelDesc");
          const size_t rows = (desc.Height + block - 1) / block;
          const size_t rowBytes = ((desc.Width + block - 1) / block) * bytes;
          const size_t copySize = (legacy ? topPitch : rowBytes) * rows;
          std::vector<unsigned char> source(copySize);
          for (size_t i = 0; i < copySize; ++i) source[i] = static_cast<unsigned char>(i * 17 + face * 29 + level * 7);
          IDirect3DSurface9* surface = nullptr;
          check(cube->GetCubeMapSurface(static_cast<D3DCUBEMAP_FACES>(face), level, &surface), "GetCubeMapSurface");
          if (surface->LockRect(nullptr, nullptr, 0) != D3DERR_INVALIDCALL) return 3;
          D3DLOCKED_RECT lock = {};
          if ((face + level) % 2) check(cube->LockRect(static_cast<D3DCUBEMAP_FACES>(face), level, &lock, nullptr, 0), "Cube LockRect");
          else check(surface->LockRect(&lock, nullptr, 0), "Surface LockRect");
          // The compatibility option must not change the pitch seen by correct callers.
          if (static_cast<size_t>(lock.Pitch) != rowBytes) { std::fprintf(stderr, "Pitch changed: %d vs %zu\n", lock.Pitch, rowBytes); return 4; }
          std::memcpy(lock.pBits, source.data(), copySize);
          if ((face + level) % 2) check(cube->UnlockRect(static_cast<D3DCUBEMAP_FACES>(face), level), "Cube UnlockRect");
          else check(surface->UnlockRect(), "Surface UnlockRect");
          check(surface->LockRect(&lock, nullptr, D3DLOCK_READONLY), "Read-only LockRect");
          if (std::memcmp(lock.pBits, source.data(), rowBytes * rows)) return 5;
          check(surface->UnlockRect(), "Read-only UnlockRect");
          surface->Release(); ++uploads;
        }
      }
      cube->Release();
    }
  }
  IDirect3DQuery9* query = nullptr;
  check(dev->CreateQuery(D3DQUERYTYPE_EVENT, &query), "CreateQuery");
  check(query->Issue(D3DISSUE_END), "Issue");
  HRESULT hr; const DWORD start = GetTickCount();
  while ((hr = query->GetData(nullptr, 0, D3DGETDATA_FLUSH)) == S_FALSE) {
    if (GetTickCount() - start > 15000) return 6;
    Sleep(1);
  }
  check(hr, "GPU completion"); query->Release();
  dev->Release(); d3d->Release(); DestroyWindow(hwnd); FreeLibrary(mod);
  std::printf("PASS: %u cubemap uploads, DXT1/DXT5/ARGB8, all faces and mips; %s copies.\n", uploads, legacy ? "legacy oversized" : "standard");
  return 0;
}
