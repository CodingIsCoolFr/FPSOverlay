// Decides which process the HUD measures.
//
// Rule: the foreground app if it is drawing frames. When focus moves to something that is
// not drawing (Discord on a second monitor, the desktop, this app's settings), the last game
// stays selected as long as it is still running and presenting.
#pragma once

#include <windows.h>

#include <string>

class FrameCapture;
enum class PresentSource : int;

class TargetTracker {
public:
    void Update(const FrameCapture& capture);   // call a few times per second

    DWORD Pid() const { return pid_; }
    const std::string& DisplayName() const { return name_; }  // "Game Title" or "game.exe"
    const std::string& ExeName() const { return exe_; }
    // "DX12", "DX11", "Vulkan", "OpenGL", "DX9" or empty while unknown.
    const std::string& ApiLabel() const { return api_; }

    void Refine(PresentSource source);  // fills ApiLabel once the present source is known

private:
    static DWORD ForegroundPid();
    void SetTarget(DWORD pid);

    DWORD pid_ = 0;
    std::string name_;
    std::string exe_;
    std::string api_;
    int apiForSource_ = -2;
};
