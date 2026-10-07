#include "render/d3d.h"
#include "app/log.h"

namespace {
template <typename T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }
}

bool D3D::Init(std::string& error)
{
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL got = {};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 3,
                                   D3D11_SDK_VERSION, &device_, &got, &context_);
    if (FAILED(hr)) {
        // Remote desktop sessions and broken drivers: fall back to WARP so the UI still works.
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, 3, D3D11_SDK_VERSION,
                               &device_, &got, &context_);
    }
    if (FAILED(hr)) {
        error = "Direct3D 11 could not be started (0x" + std::to_string((unsigned long)hr) + ").";
        return false;
    }

    IDXGIDevice* dxgiDevice = nullptr;
    IDXGIAdapter* adapter = nullptr;
    if (SUCCEEDED(device_->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice))) &&
        SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
        adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory_));
    }
    SafeRelease(adapter);
    SafeRelease(dxgiDevice);
    if (!factory_) {
        error = "DXGI 1.2 is not available.";
        Shutdown();
        return false;
    }
    return true;
}

void D3D::Shutdown()
{
    SafeRelease(factory_);
    SafeRelease(context_);
    SafeRelease(device_);
}

bool SwapTarget::Create(const D3D& d3d, HWND hwnd, UINT width, UINT height, Mode mode)
{
    Destroy();
    d3d_ = &d3d;
    mode_ = mode;
    width_ = width ? width : 1;
    height_ = height ? height : 1;

    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = width_;
    sd.Height = height_;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    if (mode == Mode::Flip) {
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    } else {
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferCount = 1;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    }
    sd.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;

    HRESULT hr = d3d.Factory()->CreateSwapChainForHwnd(d3d.Device(), hwnd, &sd, nullptr, nullptr, &swap_);
    if (FAILED(hr) && mode == Mode::Flip) {
        // Very old systems: plain blt model.
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        sd.BufferCount = 1;
        hr = d3d.Factory()->CreateSwapChainForHwnd(d3d.Device(), hwnd, &sd, nullptr, nullptr, &swap_);
    }
    if (FAILED(hr)) {
        logx::Error("CreateSwapChainForHwnd failed 0x%08lx", (unsigned long)hr);
        swap_ = nullptr;
        return false;
    }
    d3d.Factory()->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    CreateView();
    return rtv_ != nullptr;
}

void SwapTarget::CreateView()
{
    SafeRelease(rtv_);
    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(swap_->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back)))) {
        d3d_->Device()->CreateRenderTargetView(back, nullptr, &rtv_);
        back->Release();
    }
}

void SwapTarget::Destroy()
{
    SafeRelease(rtv_);
    SafeRelease(swap_);
    occluded_ = false;
}

bool SwapTarget::Resize(UINT width, UINT height)
{
    if (!swap_) return false;
    width = width ? width : 1;
    height = height ? height : 1;
    if (width == width_ && height == height_) return true;
    SafeRelease(rtv_);
    d3d_->Context()->OMSetRenderTargets(0, nullptr, nullptr);
    d3d_->Context()->Flush();
    if (FAILED(swap_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0))) {
        logx::Warn("ResizeBuffers(%u, %u) failed", width, height);
        CreateView();
        return false;
    }
    width_ = width;
    height_ = height;
    CreateView();
    return rtv_ != nullptr;
}

void SwapTarget::Bind(const float clear[4])
{
    if (!rtv_) return;
    ID3D11DeviceContext* ctx = d3d_->Context();
    ctx->OMSetRenderTargets(1, &rtv_, nullptr);
    ctx->ClearRenderTargetView(rtv_, clear);
    D3D11_VIEWPORT vp = { 0.f, 0.f, (float)width_, (float)height_, 0.f, 1.f };
    ctx->RSSetViewports(1, &vp);
}

bool SwapTarget::Present(bool vsync)
{
    if (!swap_) return false;
    const HRESULT hr = swap_->Present(vsync ? 1 : 0, 0);
    occluded_ = (hr == DXGI_STATUS_OCCLUDED);
    return !occluded_;
}

bool SwapTarget::Occluded()
{
    if (!occluded_ || !swap_) return false;
    occluded_ = swap_->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED;
    return occluded_;
}
