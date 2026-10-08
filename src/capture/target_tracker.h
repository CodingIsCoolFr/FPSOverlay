// Decides which process the HUD measures.
//
// Rule: the foreground app if it is drawing frames. When focus moves to something that is
// not drawing (Discord on a second monitor, the desktop, this app's settings), or to an app
// that draws but is not a game (a browser, a tool), the last game stays selected as long as it
// is still running and presenting. When the target is not a drawing game, a game drawing
// anywhere else is picked up even if it never had focus (a VR game, a game started before
// this app, a game left running behind a chat window).
#pragma once

#include "capture/game_detect.h"

#include <windows.h>

#include <string>
#include <unordered_map>

class FrameCapture;
enum class PresentSource : int;

class TargetTracker {
public:
    void Update(const FrameCapture& capture, const games::Choices& choices);   // call a few times per second

    DWORD Pid() const { return pid_; }
    const std::string& DisplayName() const { return name_; }  // "Game Title" or "game.exe"
    const std::string& ExeName() const { return exe_; }
    // "DX12", "DX11", "Vulkan", "OpenGL", "DX9" or empty while unknown.
    const std::string& ApiLabel() const { return api_; }
    // The target counts as a game (see games::Classify; an unknown app counts while it fills
    // its screen).
    bool IsGame() const { return isGame_; }
    // The target's main window (the one it had in front, else its biggest), or null.
    HWND Window() const { return window_; }

    void Refine(PresentSource source);  // fills ApiLabel once the present source is known

    // The process behind the foreground window (the app inside a Store app's frame window), or 0
    // for the desktop, the taskbar and this app's own windows. `window` receives the window.
    static DWORD ForegroundPid(HWND* window = nullptr);

private:
    struct Candidate {
        DWORD pid = 0;
        std::wstring path;
        std::string exe;
        bool fullscreen = false;
    };
    bool Judge(const Candidate& c, const games::Choices& choices) const;
    const Candidate& Lookup(DWORD pid);     // path and exe name, looked up once per process
    void SetTarget(DWORD pid);

    DWORD pid_ = 0;
    std::string name_;
    std::string exe_;
    std::wstring path_;
    std::string api_;
    int apiForSource_ = -2;
    bool isGame_ = false;
    bool fullscreen_ = false;   // the target's window filled its screen when last in front
    HWND window_ = nullptr;
    Candidate fg_;              // the foreground app, looked up once per new foreground process
    std::unordered_map<DWORD, Candidate> known_;    // processes seen drawing
};
