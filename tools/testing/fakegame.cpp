// A stand-in game for testing "Hide when no game is running": a small window that presents at
// vsync through DXGI, never takes focus, minimizes itself at 6 s, restores at 9 s, exits at 14 s.
//
// Build (x64 Native Tools prompt), into build\ so nothing runs from %TEMP%:
//   cl /nologo /EHsc /O2 tools\testing\fakegame.cpp user32.lib /Fe:build\fakegame\fakegame.exe
// Then give a FPSO_TEST_INSTANCE copy of the app `[Games] games=fakegame.exe` in its config.ini,
// start it with --tray, start fakegame.exe, and read the "Game check" lines in its log.
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <cstdio>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, m, w, l);
}

int wmain()
{
    WNDCLASSW wc = {};
    wc.lpfnWndProc = Proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"FakeGameWnd";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(WS_EX_NOACTIVATE, wc.lpszClassName, L"Fake game (FPS Overlay test)", WS_OVERLAPPEDWINDOW,
                                40, 40, 360, 220, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGISwapChain* sc = nullptr;
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                             &sd, &sc, &dev, nullptr, &ctx))) {
        printf("d3d failed\n");
        return 1;
    }
    ID3D11Texture2D* bb = nullptr;
    sc->GetBuffer(0, IID_PPV_ARGS(&bb));
    ID3D11RenderTargetView* rtv = nullptr;
    dev->CreateRenderTargetView(bb, nullptr, &rtv);
    bb->Release();

    const ULONGLONG start = GetTickCount64();
    int phase = 0;
    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        const ULONGLONG t = GetTickCount64() - start;
        if (t > 14000) break;
        if (phase == 0 && t > 6000) { ShowWindow(hwnd, SW_SHOWMINNOACTIVE); phase = 1; printf("%5.1fs minimized\n", t / 1000.0); }
        if (phase == 1 && t > 9000) { ShowWindow(hwnd, SW_SHOWNOACTIVATE); phase = 2; printf("%5.1fs restored\n", t / 1000.0); }
        const float c[4] = { 0.1f, 0.3f + 0.2f * (float)((t / 500) % 2), 0.5f, 1.f };
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->ClearRenderTargetView(rtv, c);
        sc->Present(1, 0);
        if (phase == 1) Sleep(16);      // a minimized window's Present returns at once
    }
    rtv->Release();
    sc->Release();
    ctx->Release();
    dev->Release();
    DestroyWindow(hwnd);
    return 0;
}
