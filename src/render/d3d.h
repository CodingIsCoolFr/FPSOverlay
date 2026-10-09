// One Direct3D 11 device shared by every window, and a swap chain per window.
#pragma once

#include <d3d11.h>
#include <dxgi1_3.h>

#include <string>

class D3D {
public:
    bool Init(std::string& error);
    void Shutdown();
    ID3D11Device*        Device() const { return device_; }
    ID3D11DeviceContext* Context() const { return context_; }
    IDXGIFactory2*       Factory() const { return factory_; }

private:
    ID3D11Device*        device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    IDXGIFactory2*       factory_ = nullptr;
};

class SwapTarget {
public:
    enum class Mode {
        Flip,           // normal opaque window (settings)
        Transparent,    // per-pixel alpha through DWM (HUD); blt model, the only one DWM glass accepts
    };

    bool Create(const D3D& d3d, HWND hwnd, UINT width, UINT height, Mode mode);
    void Destroy();
    bool Resize(UINT width, UINT height);
    void Bind(const float clear[4]);
    // Returns false while the window is occluded (minimised, behind an exclusive-fullscreen game);
    // the caller should then skip rendering until Occluded() turns false again.
    bool Present(bool vsync);
    bool Occluded();

    UINT Width() const { return width_; }
    UINT Height() const { return height_; }
    bool Valid() const { return swap_ != nullptr; }

    // Flip mode keeps up to kFramesInFlight presented frames queued for the display. The
    // waitable is signalled once for each queued frame the GPU has processed; with vsync that is
    // the refresh that shows it. Present with vsync, render while QueueHasRoom(), and otherwise
    // wait on FrameWaitable() (then call FrameDone()): exactly one frame per refresh, and Present
    // never blocks. One frame in flight left no slack: whenever the GPU took a refresh longer to
    // show a frame (a busy game), the display showed the last one twice.
    static constexpr int kFramesInFlight = 2;
    HANDLE FrameWaitable() const { return waitable_; }
    void FrameDone();               // a wait on FrameWaitable() returned
    bool QueueHasRoom();            // counts frames done without waiting; always true in blt mode

private:
    void CreateView();
    void ForgetQueuedFrames();

    const D3D*              d3d_ = nullptr;
    IDXGISwapChain1*        swap_ = nullptr;
    ID3D11RenderTargetView* rtv_ = nullptr;
    HANDLE                  waitable_ = nullptr;
    int                     inFlight_ = 0;  // presented, not yet processed (flip mode)
    UINT                    flags_ = 0;     // swap chain flags, repeated on ResizeBuffers
    UINT width_ = 0, height_ = 0;
    Mode mode_ = Mode::Flip;
    bool occluded_ = false;
};
