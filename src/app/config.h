// User settings: one struct, defaults inline, stored as config.ini (UTF-8).
#pragma once

#include <string>

class Ini;

namespace cfg {

enum class Layout : int { Vertical = 0, Horizontal = 1, Bar = 2, Count };

enum class Anchor : int { TopLeft = 0, TopCenter, TopRight, BottomLeft, BottomCenter, BottomRight, Count };

enum class Group : int { Frames = 0, Gpu, Cpu, Memory, Info, Count };

// Everything the HUD can show. The order here is the order inside each group on the HUD.
enum class Metric : int {
    Fps = 0, FrameTime, Low1, Low01, FrameGraph, Api,
    GpuLoad, GpuTemp, GpuHotspot, GpuPower, GpuClock, GpuMemClock, GpuFan, Vram,
    CpuLoad, CpuTemp, CpuPower, CpuClock, CpuFan,
    Ram,
    Process, Time,
    Count
};

struct MetricInfo {
    Metric      id;
    Group       group;
    const char* key;        // config.ini key
    const char* label;      // settings label (English source; translated through locale::T)
    const char* hint;       // settings tooltip (English source), may be null
    bool        defaultOn;
};

const MetricInfo& Info(Metric m);
const char* GroupLabel(Group g);

struct Hotkey {
    unsigned vk = 0;        // 0 = not bound
    unsigned mods = 0;      // MOD_ALT / MOD_CONTROL / MOD_SHIFT / MOD_WIN
    bool operator==(const Hotkey& o) const { return vk == o.vk && mods == o.mods; }
    bool operator!=(const Hotkey& o) const { return !(*this == o); }
};

enum class HotkeyAction : int { ToggleHud = 0, ResetStats, OpenSettings, Exit, Count };

struct Config {
    bool show[(int)Metric::Count] = {};

    // HUD look and placement
    Layout layout = Layout::Vertical;
    Anchor anchor = Anchor::TopLeft;
    std::string monitor;            // "\\.\DISPLAY1"; empty = primary monitor
    bool customPos = false;         // true after the user drags the HUD
    int  posX = 0, posY = 0;        // HUD top-left relative to the monitor's top-left, physical px
    int  margin = 12;               // distance from the screen edge for anchored positions, in DIPs
    int  scale = 100;               // 50..250 %
    int  bgOpacity = 80;            // 0..100 %
    int  textOpacity = 100;         // 20..100 %
    int  accent = 0;                // index into theme::kAccents
    bool colorValues = true;        // green / yellow / red by threshold
    bool hideFromCapture = false;   // exclude the HUD from screenshots and recordings
    bool showLabels = true;         // "GPU" / "CPU" captions on the HUD
    bool hideWhenIdle = false;      // hide the HUD while no game is drawing frames

    // Timing
    int hudFps = 30;                // HUD redraw rate
    int statsIntervalMs = 500;      // how often the numbers change
    int lowsWindowSec = 10;         // rolling window for 1% / 0.1% lows
    int sensorIntervalMs = 1000;    // hardware sensor polling

    // Units
    bool fahrenheit = false;
    bool time24h = true;
    bool timeSeconds = false;

    // Sensors
    std::string gpu;                // GpuAdapter::key (name + PCI slot); empty = automatic
    std::string cpuTempSensor;      // LibreHardwareMonitor identifier; empty = automatic
    std::string cpuFanSensor;       // LibreHardwareMonitor identifier; empty = automatic

    Hotkey hotkeys[(int)HotkeyAction::Count];

    // General
    std::string language = "en-US";
    int  settingsPage = 0;
    bool firstRunDone = false;

    Config();
};

void Load(Config& c, const Ini& ini);
void Store(const Config& c, Ini& ini);

// File I/O. Save writes a temp file and renames it over the old one, so a crash or power
// loss mid-save never leaves a truncated config behind.
bool LoadFile(const std::wstring& path, Config& c);
bool SaveFile(const std::wstring& path, const Config& c);

std::string HotkeyToString(const Hotkey& h);      // "45,2"
Hotkey      HotkeyFromString(const std::string& s);

} // namespace cfg
