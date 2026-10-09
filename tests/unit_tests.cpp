// Unit tests for the pure logic: INI handling, config round trip, frame statistics and
// sensor selection. Plain asserts, no framework; exit code = number of failures.

#include "app/config.h"
#include "app/game_visibility.h"
#include "app/ini.h"
#include "app/updater.h"
#include "capture/capture_watchdog.h"
#include "capture/frame_stats.h"
#include "capture/game_detect.h"
#include "sensors/cpu_direct.h"
#include "sensors/lhm_select.h"
#include "ui/scroll_glide.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        ++g_checks;                                                                 \
        if (!(cond)) {                                                              \
            ++g_failures;                                                           \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                  \
        }                                                                           \
    } while (0)

#define CHECK_NEAR(a, b, eps) CHECK(std::fabs((double)(a) - (double)(b)) <= (eps))

// ── INI ─────────────────────────────────────────────────────────────────────

static void TestIni()
{
    Ini ini;
    ini.Parse("\xEF\xBB\xBF; comment\n[Hud]\nscale = 125\r\nlayout=bar\n\n[hud]\nscale=130\n[Other]\nflag=yes\nbad line\n");
    CHECK(ini.GetInt("Hud", "scale", 0) == 130);        // later duplicate wins, sections merge
    CHECK(ini.Get("HUD", "LAYOUT") == "bar");           // case-insensitive
    CHECK(ini.GetBool("Other", "flag", false));
    CHECK(ini.GetInt("Missing", "x", 7) == 7);
    CHECK(!ini.Has("Other", "bad line"));

    ini.Set("New", "key", "multi\nline");
    CHECK(ini.Get("New", "key") == "multi line");

    Ini round;
    round.Parse(ini.Serialize());
    CHECK(round.GetInt("Hud", "scale", 0) == 130);
    CHECK(round.Get("New", "key") == "multi line");
}

// ── Config ──────────────────────────────────────────────────────────────────

static void TestConfig()
{
    cfg::Config c;
    CHECK(c.show[(int)cfg::Metric::Fps]);
    CHECK(!c.show[(int)cfg::Metric::Low01]);
    CHECK(c.hotkeys[(int)cfg::HotkeyAction::ToggleHud].vk == VK_INSERT);

    c.layout = cfg::Layout::Bar;
    c.anchor = cfg::Anchor::BottomRight;
    c.scale = 175;
    c.show[(int)cfg::Metric::Low01] = true;
    c.show[(int)cfg::Metric::Fps] = false;
    c.monitor = "\\\\.\\DISPLAY2";
    c.hotkeys[(int)cfg::HotkeyAction::Exit] = { 'Q', MOD_CONTROL | MOD_SHIFT };
    c.gpu = "NVIDIA GeForce RTX 4080|pci:01:00.0";

    Ini ini;
    cfg::Store(c, ini);
    cfg::Config back;
    cfg::Load(back, ini);
    CHECK(back.layout == cfg::Layout::Bar);
    CHECK(back.anchor == cfg::Anchor::BottomRight);
    CHECK(back.scale == 175);
    CHECK(back.show[(int)cfg::Metric::Low01]);
    CHECK(!back.show[(int)cfg::Metric::Fps]);
    CHECK(back.monitor == "\\\\.\\DISPLAY2");
    CHECK(back.hotkeys[(int)cfg::HotkeyAction::Exit] == (cfg::Hotkey{ 'Q', MOD_CONTROL | MOD_SHIFT }));
    CHECK(back.gpu == c.gpu);

    // Out-of-range values are clamped / snapped instead of trusted.
    Ini bad;
    bad.Parse("[Hud]\nscale=9000\nbgOpacity=-5\nlayout=nonsense\n[Timing]\nhudFps=47\n[Hotkeys]\ntoggle=999,3\n");
    cfg::Config clamped;
    cfg::Load(clamped, bad);
    CHECK(clamped.scale == 250);
    CHECK(clamped.bgOpacity == 0);
    CHECK(clamped.layout == cfg::Layout::Vertical);
    CHECK(clamped.hudFps == 30);
    CHECK(clamped.hotkeys[(int)cfg::HotkeyAction::ToggleHud].vk == 0);

    // Game lists: lower-cased, trimmed, no blanks or duplicates, and they survive a round trip.
    Ini lists;
    lists.Parse("[Games]\ngames= Tool.EXE ; ;tool.exe;emu.exe\nnotGames=Chat.exe\n");
    cfg::Config g;
    cfg::Load(g, lists);
    CHECK(g.gameApps == (std::vector<std::string>{ "tool.exe", "emu.exe" }));
    CHECK(g.notGameApps == (std::vector<std::string>{ "chat.exe" }));
    Ini again;
    cfg::Store(g, again);
    cfg::Config g2;
    cfg::Load(g2, again);
    CHECK(g2.gameApps == g.gameApps && g2.notGameApps == g.notGameApps);
}

// ── Game detection ──────────────────────────────────────────────────────────

static void TestGameDetect()
{
    using games::Verdict;
    const std::vector<std::wstring> known = { L"c:\\games\\indie\\thing.exe", L"d:\\stuff\\minecraft\\javaw.exe" };
    games::Choices none;
    auto classify = [&](const wchar_t* path, const char* exe, const games::Choices& ch) {
        return games::Classify(path, exe, ch, known);
    };

    // Library folders, any case.
    CHECK(classify(L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\VRChat\\VRChat.exe", "VRChat.exe", none) == Verdict::Game);
    CHECK(classify(L"D:\\XboxGames\\Forza\\Content\\forza.exe", "forza.exe", none) == Verdict::Game);
    CHECK(classify(L"C:\\Users\\me\\Desktop\\Games\\Bodycam\\Bodycam.exe", "Bodycam.exe", none) == Verdict::Game);
    // Windows' own game list matches whole paths, case-insensitively.
    CHECK(classify(L"D:\\Stuff\\Minecraft\\JAVAW.exe", "JAVAW.exe", none) == Verdict::Game);
    CHECK(classify(L"D:\\Other\\javaw.exe", "javaw.exe", none) == Verdict::Unknown);
    // Apps that draw frames but are not games, even inside a game folder or on Windows' list.
    CHECK(classify(L"C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe", "chrome.exe", none) == Verdict::NotGame);
    CHECK(classify(L"C:\\Users\\me\\AppData\\Local\\AnthropicClaude\\claude.exe", "Claude.exe", none) == Verdict::NotGame);
    CHECK(classify(L"C:\\Program Files\\Epic Games\\Launcher\\Portal\\Binaries\\Win64\\EpicGamesLauncher.exe",
                   "EpicGamesLauncher.exe", none) == Verdict::NotGame);
    CHECK(classify(L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\wallpaper_engine\\wallpaper64.exe",
                   "wallpaper64.exe", none) == Verdict::NotGame);
    // The Snipping Tool's capture layer fills the screen and draws, but is not a game.
    CHECK(classify(L"C:\\Windows\\SystemApps\\ScreenClipping\\ScreenClippingHost.exe", "ScreenClippingHost.exe", none) ==
          Verdict::NotGame);
    // Unknown programs are left to the full-screen test.
    CHECK(classify(L"C:\\Tools\\CrashDetective.exe", "CrashDetective.exe", none) == Verdict::Unknown);
    CHECK(classify(L"", "", none) == Verdict::Unknown);

    // The user's choice beats everything else.
    games::Choices mine;
    mine.games = { "crashdetective.exe", "chrome.exe" };
    mine.notGames = { "vrchat.exe" };
    CHECK(classify(L"C:\\Tools\\CrashDetective.exe", "CrashDetective.exe", mine) == Verdict::Game);
    CHECK(classify(L"C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe", "chrome.exe", mine) == Verdict::Game);
    CHECK(classify(L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\VRChat\\VRChat.exe", "VRChat.exe", mine) == Verdict::NotGame);

    // Video players are never games. Stremio 5 is stremio-shell-ng.exe, and a film in it fills
    // the screen like a game would.
    CHECK(classify(L"C:\\Users\\me\\AppData\\Local\\Programs\\Stremio\\stremio-shell-ng.exe", "stremio-shell-ng.exe",
                   none) == Verdict::NotGame);
    CHECK(classify(L"C:\\Program Files\\AniSol\\AniSol.exe", "AniSol.exe", none) == Verdict::NotGame);
    // An unknown program with a video engine loaded is a player...
    auto withVideo = [&](const wchar_t* path, const char* exe, const games::Choices& ch) {
        return games::Classify(path, exe, ch, known, true);
    };
    CHECK(withVideo(L"C:\\Tools\\Player\\player.exe", "player.exe", none) == Verdict::NotGame);
    CHECK(classify(L"C:\\Tools\\Player\\player.exe", "player.exe", none) == Verdict::Unknown);
    // ... even on Windows' list, which also holds apps that are not games ...
    CHECK(withVideo(L"D:\\Stuff\\Minecraft\\javaw.exe", "javaw.exe", none) == Verdict::NotGame);
    // ... but a game in a game folder that plays its videos with VLC is still a game,
    CHECK(withVideo(L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\VRChat\\VRChat.exe", "VRChat.exe", none) ==
          Verdict::Game);
    // and the user's choice still wins.
    games::Choices player;
    player.games = { "player.exe" };
    CHECK(withVideo(L"C:\\Tools\\Player\\player.exe", "player.exe", player) == Verdict::Game);

    CHECK(games::IsVideoApp("Stremio-Shell-NG.exe"));
    CHECK(games::IsVideoApp("vlc.exe"));
    CHECK(!games::IsVideoApp("VRChat.exe"));
    CHECK(!games::IsVideoApp("firefox.exe"));
    CHECK(games::IsVideoEngine(L"libmpv-2.dll"));
    CHECK(games::IsVideoEngine(L"LIBVLC.DLL"));
    CHECK(games::IsVideoEngine(L"Windows.Media.Protection.PlayReady.dll"));
    CHECK(!games::IsVideoEngine(L"mfplat.dll"));        // games load Media Foundation for cut-scenes
    CHECK(!games::IsVideoEngine(L"d3d11.dll"));
    CHECK(!games::IsVideoEngine(L"Qt6Multimedia.dll")); // also loaded just for sounds

    // Overlays and frame scalers sit over the game without hiding it.
    CHECK(games::IsSeeThrough("LosslessScaling.exe"));
    CHECK(games::IsSeeThrough("NVIDIA Overlay.exe"));
    CHECK(games::IsSeeThrough("ScreenClippingHost.exe"));
    CHECK(!games::IsSeeThrough("stremio-shell-ng.exe"));
    CHECK(!games::IsSeeThrough("firefox.exe"));

    // Windows' pop-ups: Start and search, the Alt+Tab switcher and the taskbar. A folder window
    // is a real app, although it also belongs to explorer.exe.
    CHECK(games::IsWindowsPopup("SearchHost.exe", L"Windows.UI.Core.CoreWindow"));
    CHECK(games::IsWindowsPopup("StartMenuExperienceHost.exe", L"Windows.UI.Core.CoreWindow"));
    CHECK(games::IsWindowsPopup("explorer.exe", L"XamlExplorerHostIslandWindow"));
    CHECK(games::IsWindowsPopup("explorer.exe", L"Shell_TrayWnd"));
    CHECK(games::IsWindowsPopup("explorer.exe", L"ForegroundStaging"));
    CHECK(!games::IsWindowsPopup("explorer.exe", L"CabinetWClass"));
    CHECK(!games::IsWindowsPopup("firefox.exe", L"MozillaWindowClass"));
    CHECK(!games::IsWindowsPopup("stremio-shell-ng.exe", L"NativeWindowsGuiWindow"));
}

// ── "Hide when no game is running" ──────────────────────────────────────────

static void TestGameVisibility()
{
    using F = GameVisibility::Front;
    GameVisibility v;
    uint64_t t = 0;
    GameVisibility::Inputs in;
    in.target = 100;
    in.isGame = true;
    in.secondsSinceFrame = 0.01;
    auto step = [&](F front, uint64_t ms = 100) {
        in.front = front;
        t += ms;
        return v.Update(in, t);
    };

    // The game runs behind other apps and never had focus (VR). Not alt-tabbed out of, so the
    // HUD shows.
    CHECK(step(F::GameScreen));
    CHECK(step(F::OtherScreen));

    // Playing, then alt-tab to an app on the game's screen: hidden after the 0.4 s grace.
    CHECK(step(F::Game));
    CHECK(step(F::GameScreen));         // still inside the grace period
    CHECK(!step(F::GameScreen, 500));
    CHECK(v.AltTabbedOut());
    CHECK(!step(F::OtherScreen));       // moving on to the other screen does not bring it back
    // Back to the game: shown at once.
    CHECK(step(F::Game));
    CHECK(!v.AltTabbedOut());

    // An app on the other screen (Discord), the settings window, or focus in motion: still shown.
    CHECK(step(F::OtherScreen, 1000));
    CHECK(step(F::Ours, 1000));
    CHECK(step(F::Moving, 1000));
    // After that, clicking an app on the game's screen still counts as leaving it.
    step(F::GameScreen);
    CHECK(!step(F::GameScreen, 500));
    CHECK(step(F::Game));

    // Minimized: hidden; restored: shown.
    in.minimized = true;
    step(F::OtherScreen);
    CHECK(!step(F::OtherScreen, 500));
    in.minimized = false;
    CHECK(step(F::Game));

    // A loading screen: no frames for 2 s keeps the HUD, 3 s and more hides it.
    in.secondsSinceFrame = 2.0;
    CHECK(step(F::Game, 1000));
    in.secondsSinceFrame = 3.5;
    step(F::Game);
    CHECK(!step(F::Game, 500));
    // Coming back needs a recent frame, not just a stale one.
    in.secondsSinceFrame = 0.8;
    CHECK(!step(F::Game));
    in.secondsSinceFrame = 0.02;
    CHECK(step(F::Game));

    // Not a game, or nothing measured: never shown.
    in.isGame = false;
    step(F::Game);
    CHECK(!step(F::Game, 500));
    in.isGame = true;
    in.target = 0;
    CHECK(!step(F::Game, 500));

    // A new target starts clean: being alt-tabbed out of the old game does not carry over.
    in.target = 100;
    step(F::Game);
    step(F::GameScreen);
    CHECK(!step(F::GameScreen, 500));
    in.target = 200;
    CHECK(step(F::GameScreen));
}

static void TestGameVisibilityVideoAndMemory()
{
    using F = GameVisibility::Front;
    GameVisibility v;
    uint64_t t = 0;
    GameVisibility::Inputs in;
    in.target = 100;
    in.isGame = true;
    in.secondsSinceFrame = 0.01;
    auto step = [&](F front, uint64_t ms = 100) {
        in.front = front;
        t += ms;
        return v.Update(in, t);
    };

    // The reported case: a film plays in Stremio over a game that never had focus. With the film
    // under the HUD it hides after the grace period, and comes back as soon as the film is gone.
    CHECK(step(F::GameScreen));
    in.overVideo = true;
    CHECK(step(F::GameScreen));
    CHECK(!step(F::GameScreen, 500));
    CHECK(!step(F::OtherScreen, 1000));     // chatting on the other screen while the film plays
    in.overVideo = false;
    CHECK(step(F::OtherScreen));

    // While the game has focus, a video left under the HUD (a windowed game) does not hide it.
    in.overVideo = true;
    CHECK(step(F::Game));
    CHECK(step(F::Game, 1000));
    in.overVideo = false;

    // Alt-tabbed out, then the tracker briefly measures another app (a hitch in the game's frames)
    // and comes back: still alt-tabbed out. 2.0.10 forgot it here and showed the HUD over Firefox.
    step(F::Game);
    step(F::GameScreen);
    CHECK(!step(F::GameScreen, 500));
    in.target = 300;            // Firefox, in front
    in.isGame = false;
    CHECK(!step(F::Game));
    in.target = 100;
    in.isGame = true;
    CHECK(!step(F::GameScreen));
    CHECK(v.AltTabbedOut());
    CHECK(step(F::Game));       // back in the game

    // Start, search or the Alt+Tab switcher in front is not leaving the game.
    CHECK(step(F::Moving, 1000));
    CHECK(step(F::Game));
    CHECK(!v.AltTabbedOut());

    // On another virtual desktop the game counts as minimized.
    in.minimized = true;
    step(F::OtherScreen);
    CHECK(!step(F::OtherScreen, 500));
}

// ── Capture watchdog ────────────────────────────────────────────────────────

static void TestCaptureWatchdog()
{
    CaptureWatchdog w;
    CaptureWatchdog::Inputs in;
    in.running = true;
    uint64_t t = 0;
    // One check a second, like the app. Healthy: events grow, heartbeats come back.
    auto step = [&](uint64_t newEvents, int missing) {
        in.events += newEvents;
        in.heartbeatsMissing = missing;
        t += 1000;
        return w.Update(in, t);
    };
    for (int i = 0; i < 10; ++i) CHECK(!step(500, 1));

    // A quiet desktop (no presents at all) with the heartbeat fine: nothing to fix.
    for (int i = 0; i < 30; ++i) CHECK(!step(0, 1));

    // The 17:19 failure: the session looks fine but nothing comes out. Heartbeats pile up.
    CHECK(!step(0, 2));
    CHECK(!step(0, 4));
    CHECK(!step(0, 6));
    CHECK(step(0, 8));
    CHECK(w.Restarts() == 1);
    // The restart did not help: the next try waits 10 s, then 20 s.
    for (int i = 0; i < 9; ++i) CHECK(!step(0, 9));
    CHECK(step(0, 9));
    for (int i = 0; i < 19; ++i) CHECK(!step(0, 9));
    CHECK(step(0, 9));
    CHECK(w.Restarts() == 3);
    // Events flow again: the back-off starts over.
    CHECK(!step(300, 0));
    CHECK(w.Restarts() == 0);

    // A game in front but no presents from anyone for 6 s (heartbeat fine): restart.
    in.gameInFront = true;
    in.heartbeatsMissing = 1;
    CaptureWatchdog g;
    in.events = 1;
    t = 0;
    auto gstep = [&](uint64_t newEvents) { in.events += newEvents; t += 1000; return g.Update(in, t); };
    CHECK(!gstep(0));                           // first look: baseline
    CHECK(!gstep(100));
    for (int i = 0; i < 5; ++i) CHECK(!gstep(0));
    CHECK(gstep(0));
    // A loading screen shorter than 6 s does not trigger it.
    CaptureWatchdog l;
    t = 0;
    auto lstep = [&](uint64_t newEvents) { in.events += newEvents; t += 1000; return l.Update(in, t); };
    for (int round = 0; round < 3; ++round) {
        for (int i = 0; i < 4; ++i) CHECK(!lstep(0));
        CHECK(!lstep(200));
    }
    // Alt-tabbed out (game not in front): no presents is fine.
    in.gameInFront = false;
    for (int i = 0; i < 20; ++i) CHECK(!lstep(0));

    // The session ended under us, or never started: restart at once.
    CaptureWatchdog e;
    in.consumerEnded = true;
    CHECK(e.Update(in, 1000));
    CaptureWatchdog n;
    in.consumerEnded = false;
    in.running = false;
    CHECK(n.Update(in, 1000));
    CHECK(!n.Update(in, 2000));                 // and then it backs off
}

// ── Frame statistics ────────────────────────────────────────────────────────

static void TestFrameStats()
{
    // Steady 100 FPS for 10 s.
    FrameRing ring(4096);
    double t = 0;
    for (int i = 0; i < 1000; ++i) { t += 0.010; ring.Push(t, 10.f); }
    FrameStats s = ComputeFrameStats(ring, t, 1.0, 10.0);
    CHECK(s.valid);
    CHECK_NEAR(s.fps, 100.0, 0.5);
    CHECK_NEAR(s.frameTimeMs, 10.0, 0.01);
    CHECK_NEAR(s.low1, 100.0, 0.5);
    CHECK_NEAR(s.low01, 100.0, 0.5);

    // 1000 frames at 10 ms with 10 stutters of 50 ms: the 1% low (10 worst frames) is 20 FPS.
    FrameRing r2(4096);
    t = 0;
    for (int i = 0; i < 1000; ++i) {
        const float ms = (i % 100 == 50) ? 50.f : 10.f;
        t += ms / 1000.0;
        r2.Push(t, ms);
    }
    s = ComputeFrameStats(r2, t, 100.0, 100.0);
    CHECK_NEAR(s.low1, 20.0, 0.01);
    CHECK_NEAR(s.low01, 20.0, 0.01);
    CHECK(s.fps < 100.f && s.fps > 90.f);

    // Only frames inside the window count.
    FrameRing r3(64);
    r3.Push(1.0, 100.f);
    r3.Push(5.0, 10.f);
    r3.Push(5.01, 10.f);
    s = ComputeFrameStats(r3, 5.01, 0.5, 0.5);
    CHECK(s.valid);
    CHECK_NEAR(s.fps, 100.0, 0.01);
    CHECK(s.frames == 2);

    // Ring overwrites the oldest entries.
    FrameRing r4(4);
    for (int i = 1; i <= 6; ++i) r4.Push(i, (float)i);
    CHECK(r4.Size() == 4);
    CHECK(r4.At(0).ms == 3.f);
    CHECK(r4.Back().ms == 6.f);
    std::vector<float> g;
    CopyRecentFrameTimes(r4, 2, g);
    CHECK(g.size() == 2 && g[0] == 5.f && g[1] == 6.f);

    FrameRing empty(8);
    CHECK(!ComputeFrameStats(empty, 1.0, 1.0, 1.0).valid);
}

// ── LibreHardwareMonitor sensor selection ───────────────────────────────────

static LhmSensor S(int hw, const char* hwName, const char* hwType, const char* name, const char* type, const char* id)
{
    LhmSensor s;
    s.hardware = hw; s.hwName = hwName; s.hwType = hwType; s.name = name; s.type = type; s.id = id;
    return s;
}

static void TestLhmSelect()
{
    // Trimmed from a real i9-13900K + RTX 4080 dump.
    std::vector<LhmSensor> v = {
        S(1, "13th Gen Intel Core i9-13900K", "Cpu", "Core Max", "Temperature", "/intelcpu/0/temperature/0"),
        S(1, "13th Gen Intel Core i9-13900K", "Cpu", "P-Core #1 Distance to TjMax", "Temperature", "/intelcpu/0/temperature/27"),
        S(1, "13th Gen Intel Core i9-13900K", "Cpu", "CPU Package", "Temperature", "/intelcpu/0/temperature/26"),
        S(1, "13th Gen Intel Core i9-13900K", "Cpu", "CPU Cores", "Power", "/intelcpu/0/power/1"),
        S(1, "13th Gen Intel Core i9-13900K", "Cpu", "CPU Package", "Power", "/intelcpu/0/power/0"),
        S(2, "NVIDIA GeForce RTX 4080", "GpuNvidia", "GPU Hot Spot", "Temperature", "/gpu-nvidia/0/temperature/2"),
        S(2, "NVIDIA GeForce RTX 4080", "GpuNvidia", "GPU Memory Junction", "Temperature", "/gpu-nvidia/0/temperature/3"),
        S(2, "NVIDIA GeForce RTX 4080", "GpuNvidia", "GPU Core", "Temperature", "/gpu-nvidia/0/temperature/0"),
        S(2, "NVIDIA GeForce RTX 4080", "GpuNvidia", "GPU Memory", "Clock", "/gpu-nvidia/0/clock/4"),
        S(2, "NVIDIA GeForce RTX 4080", "GpuNvidia", "GPU Core", "Clock", "/gpu-nvidia/0/clock/0"),
        S(2, "NVIDIA GeForce RTX 4080", "GpuNvidia", "GPU Fan 1", "Fan", "/gpu-nvidia/0/fan/1"),
        S(2, "NVIDIA GeForce RTX 4080", "GpuNvidia", "D3D 3D", "Load", "/gpu-nvidia/0/load/7"),
        S(2, "NVIDIA GeForce RTX 4080", "GpuNvidia", "GPU Core", "Load", "/gpu-nvidia/0/load/0"),
        S(2, "NVIDIA GeForce RTX 4080", "GpuNvidia", "GPU Package", "Power", "/gpu-nvidia/0/power/0"),
        S(2, "NVIDIA GeForce RTX 4080", "GpuNvidia", "D3D Shared Memory Used", "SmallData", "/gpu-nvidia/0/smalldata/0"),
        S(2, "NVIDIA GeForce RTX 4080", "GpuNvidia", "GPU Memory Used", "SmallData", "/gpu-nvidia/0/smalldata/2"),
        S(2, "NVIDIA GeForce RTX 4080", "GpuNvidia", "GPU Memory Total", "SmallData", "/gpu-nvidia/0/smalldata/3"),
        S(3, "Nuvoton NCT6798D", "SuperIO", "Fan #1", "Fan", "/lpc/nct6798d/0/fan/0"),
        S(3, "Nuvoton NCT6798D", "SuperIO", "CPU Fan", "Fan", "/lpc/nct6798d/0/fan/1"),
    };
    LhmPicks p = SelectLhmSensors(v, "", "");
    CHECK(p.cpuTemp == 2);                  // CPU Package beats Core Max
    CHECK(p.cpuPower == 4);
    CHECK(p.cpuFan == 18);                  // named "CPU Fan"
    CHECK(p.cpuTempChoices.size() == 2);    // TjMax distance excluded
    CHECK(p.fanChoices.size() == 2);
    CHECK(p.gpus.size() == 1);
    const LhmGpuPick& g = p.gpus[0];
    CHECK(g.temp == 7);
    CHECK(g.hotspot == 5);
    CHECK(g.coreClock == 9);
    CHECK(g.memClock == 8);
    CHECK(g.load == 12);
    CHECK(g.power == 13);
    CHECK(g.vramUsed == 15);
    CHECK(g.vramTotal == 16);
    CHECK(MatchLhmGpu(p, "NVIDIA GeForce RTX 4080", 0x10DE) == 0);
    CHECK(MatchLhmGpu(p, "Intel(R) UHD Graphics 770", 0x8086) == -1);

    // A saved preference wins; an unknown preference falls back to automatic.
    p = SelectLhmSensors(v, "/intelcpu/0/temperature/0", "/lpc/nct6798d/0/fan/0");
    CHECK(p.cpuTemp == 0);
    CHECK(p.cpuFan == 17);
    p = SelectLhmSensors(v, "/gone/0", "");
    CHECK(p.cpuTemp == 2);

    // AMD desktop: Tctl/Tdie wins over CCD temperatures.
    std::vector<LhmSensor> amd = {
        S(0, "AMD Ryzen 7 7800X3D", "Cpu", "CCD1 (Tdie)", "Temperature", "/amdcpu/0/temperature/3"),
        S(0, "AMD Ryzen 7 7800X3D", "Cpu", "Core (Tctl/Tdie)", "Temperature", "/amdcpu/0/temperature/2"),
        S(0, "AMD Ryzen 7 7800X3D", "Cpu", "Package", "Power", "/amdcpu/0/power/0"),
    };
    p = SelectLhmSensors(amd, "", "");
    CHECK(p.cpuTemp == 1);
    CHECK(p.cpuPower == 2);
    CHECK(p.cpuFan == -1);

    // Ryzen with a Tctl offset (1800X: +20 °C): the real die temperature wins, Tctl ranks last.
    std::vector<LhmSensor> zen1 = {
        S(0, "AMD Ryzen 7 1800X", "Cpu", "Core (Tctl)", "Temperature", "/amdcpu/0/temperature/0"),
        S(0, "AMD Ryzen 7 1800X", "Cpu", "Core (Tdie)", "Temperature", "/amdcpu/0/temperature/1"),
        S(0, "AMD Ryzen 7 1800X", "Cpu", "CCD1 (Tdie)", "Temperature", "/amdcpu/0/temperature/2"),
    };
    p = SelectLhmSensors(zen1, "", "");
    CHECK(p.cpuTemp == 1);
    CHECK(p.cpuTempChoices.size() == 3 && p.cpuTempChoices.back() == 0);
}

// ── Version comparison (update check) ──────────────────────────────────────

static void TestVersions()
{
    CHECK(updater::CompareVersions("v2.0.1", "2.0.0") > 0);
    CHECK(updater::CompareVersions("2.0.0", "2.0.0") == 0);
    CHECK(updater::CompareVersions("2.0", "2.0.0") == 0);
    CHECK(updater::CompareVersions("1.9.9", "2.0.0") < 0);
    CHECK(updater::CompareVersions("2.10.0", "2.9.0") > 0);           // numeric, not text order
    CHECK(updater::CompareVersions("v2.1.0-beta", "2.0.9") > 0);
    CHECK(updater::CompareVersions("", "0.0.1") < 0);
}

// ── GitHub release JSON (auto-update) ───────────────────────────────────────

static void TestReleaseJson()
{
    const std::string json = R"({
  "url": "https://api.github.com/repos/o/r/releases/1",
  "tag_name": "v2.1.0",
  "name": "FPS Overlay 2.1.0",
  "author": { "login": "o", "id": 1, "site_admin": false },
  "body": "Notes with \"quotes\", a \\ backslash and é.",
  "assets": [
    { "name": "FPSOverlay.zip.sig", "size": 64, "digest": "sha256:1111111111111111111111111111111111111111111111111111111111111111",
      "browser_download_url": "https://github.com/o/r/releases/download/v2.1.0/FPSOverlay.zip.sig" },
    { "url": "https://api.github.com/repos/o/r/releases/assets/2", "id": 2, "name": "FPSOverlay.zip", "label": null,
      "uploader": { "login": "o", "id": 1, "url": "https://api.github.com/users/o" },
      "content_type": "application/x-zip-compressed", "state": "uploaded", "size": 5216662,
      "digest": "sha256:CC6AFC614BBC8A7CCCA05965FE9AFFA7CD3DFD10CF038D80BCE33B6A8C5B34F5", "download_count": 3,
      "browser_download_url": "https://github.com/o/r/releases/download/v2.1.0/FPSOverlay.zip" }
  ],
  "tarball_url": "https://api.github.com/repos/o/r/tarball/v2.1.0"
})";
    updater::ReleaseInfo r;
    CHECK(updater::ParseRelease(json, "FPSOverlay.zip", r));
    CHECK(r.tag == "v2.1.0");
    CHECK(r.assetUrl == "https://github.com/o/r/releases/download/v2.1.0/FPSOverlay.zip");
    CHECK(r.sha256 == "cc6afc614bbc8a7ccca05965fe9affa7cd3dfd10cf038d80bce33b6a8c5b34f5");
    CHECK(r.size == 5216662ull);

    CHECK(updater::ParseRelease(json, "Other.zip", r) && r.assetUrl.empty() && r.sha256.empty());
    CHECK(!updater::ParseRelease(R"({"message": "Not Found"})", "FPSOverlay.zip", r));
    CHECK(!updater::ParseRelease("<html>rate limited</html>", "FPSOverlay.zip", r));
    CHECK(!updater::ParseRelease(R"({"tag_name": "v1", "assets": [)", "FPSOverlay.zip", r));
}

// ── CPU temperature registers ───────────────────────────────────────────────

static void TestCpuTempDecode()
{
    float c = 0.f;
    bool limit = true;
    // Intel: 32 °C below TjMax 100, valid, not at the limit.
    CHECK(cputemp::DecodeIntel((1ull << 31) | (32ull << 16), 100, c, limit) && c == 68.f && !limit);
    // At the limit: readout 0, status bit set.
    CHECK(cputemp::DecodeIntel((1ull << 31) | 1ull, 100, c, limit) && c == 100.f && limit);
    // Bit 31 clear: not a valid reading.
    CHECK(!cputemp::DecodeIntel(32ull << 16, 100, c, limit));

    // AMD: 68 °C is 544 eighths.
    CHECK(cputemp::DecodeAmd(544u << 21, 0.f) == 68.f);
    // RANGE_SEL: the register holds Tctl + 49.
    CHECK(cputemp::DecodeAmd((936u << 21) | 0x80000u, 0.f) == 68.f);
    // TJ_SEL == 3 means the same (LibreHardwareMonitor 0.9.6 misses this case and reads 49 °C high).
    CHECK(cputemp::DecodeAmd((936u << 21) | 0x30000u, 0.f) == 68.f);
    // Ryzen 7 1800X: Tctl 88 with its +20 °C offset is a real 68 °C.
    CHECK(cputemp::DecodeAmd(704u << 21, 20.f) == 68.f);
}

// ── Reset all settings ──────────────────────────────────────────────────────

static void TestFactoryDefaults()
{
    cfg::Config c;
    c.language = "de";
    c.firstRunDone = true;
    c.settingsPage = 4;
    c.accent = 5;
    c.layout = cfg::Layout::Horizontal;
    c.anchor = cfg::Anchor::TopCenter;
    c.customPos = true;
    c.posX = 300;
    c.bgOpacity = 0;
    c.textOpacity = 60;
    c.hideWhenIdle = true;
    c.show[(int)cfg::Metric::Fps] = false;
    c.hotkeys[(int)cfg::HotkeyAction::ToggleHud] = { 'F', MOD_CONTROL };
    c.gameApps.push_back("vrchat.exe");
    c.gpu = "NVIDIA GeForce RTX 4080|PCI 1";
    c.autoUpdate = false;

    const cfg::Config r = cfg::FactoryDefaults(c);
    CHECK(r.language == "de" && r.firstRunDone && r.settingsPage == 4);     // kept
    cfg::Config fresh;
    fresh.language = "de";
    fresh.firstRunDone = true;
    fresh.settingsPage = 4;
    Ini a, b;
    cfg::Store(r, a);
    cfg::Store(fresh, b);
    CHECK(a.Serialize() == b.Serialize());                                  // everything else is the default
    CHECK(r.accent == 0 && !r.customPos && r.show[(int)cfg::Metric::Fps] && r.gameApps.empty() && r.autoUpdate);
    CHECK(r.hotkeys[(int)cfg::HotkeyAction::ToggleHud].vk == VK_INSERT);
}

// ── Scroll glide ────────────────────────────────────────────────────────────

static void TestScrollGlide()
{
    const float dt = 1.f / 240.f;   // one frame at 240 Hz

    // One notch (80 px) from rest: moves on the first frame, never past the target, and rests on it
    // after 150 ms, even though the window sets the same target again every frame.
    ScrollGlide g;
    g.SetTarget(80.f);
    g.Advance(dt);
    CHECK(g.Position() > 3.f);
    bool steady = true;
    for (int i = 0; i < 36; ++i) {
        const float before = g.Position();
        g.SetTarget(80.f);
        g.Advance(dt);
        if (g.Position() < before || g.Position() > 80.f) steady = false;
    }
    CHECK(steady);
    CHECK(!g.Gliding() && g.Position() == 80.f && g.Speed() == 0.f);

    // A spinning wheel, a notch every 70 ms: an even speed and no lurch at each notch. The easing
    // before 2.0.12 swung between 2 and 8 px a frame and jumped about 5 px at every notch.
    ScrollGlide w;
    float target = 0.f, lastStep = -1.f, slowest = 1e9f, fastest = 0.f, biggestChange = 0.f;
    for (int f = 0; f < 8 * 17; ++f) {
        if (f % 17 == 0) w.SetTarget(target += 80.f);
        const float before = w.Position();
        w.Advance(dt);
        const float step = w.Position() - before;
        if (f >= 3 * 17 && f < 7 * 17) {     // up to speed, before the last notch
            slowest = std::min(slowest, step);
            fastest = std::max(fastest, step);
        }
        if (lastStep >= 0.f) biggestChange = std::max(biggestChange, std::fabs(step - lastStep));
        lastStep = step;
    }
    CHECK(slowest > 4.f && fastest < 5.3f);
    CHECK(biggestChange < 0.5f);
    for (int f = 0; f < 37; ++f) w.Advance(dt);
    CHECK(!w.Gliding() && w.Position() == target);

    // Reversing mid-glide heads straight for the new target, without overshooting it.
    ScrollGlide r;
    r.SetTarget(400.f);
    for (int f = 0; f < 12; ++f) r.Advance(dt);
    const float turnedAt = r.Position();
    r.SetTarget(100.f);
    bool inside = true;
    for (int f = 0; f < 37; ++f) {
        r.Advance(dt);
        if (r.Position() < 100.f || r.Position() > turnedAt + 0.001f) inside = false;
    }
    CHECK(inside && r.Position() == 100.f);

    // Moved by something else: stops there.
    r.SetTarget(300.f);
    r.Advance(dt);
    r.Jump(250.f);
    CHECK(!r.Gliding() && r.Position() == 250.f && r.Target() == 250.f && r.Speed() == 0.f);
}

int main()
{
    TestVersions();
    TestCpuTempDecode();
    TestReleaseJson();
    TestIni();
    TestConfig();
    TestFactoryDefaults();
    TestGameDetect();
    TestGameVisibility();
    TestGameVisibilityVideoAndMemory();
    TestCaptureWatchdog();
    TestFrameStats();
    TestLhmSelect();
    TestScrollGlide();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures;
}
