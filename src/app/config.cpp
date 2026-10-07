#include "app/config.h"
#include "app/ini.h"

#include <windows.h>

#include <cstdio>
#include <vector>

namespace cfg {

namespace {

const MetricInfo kMetrics[] = {
    { Metric::Fps,         Group::Frames, "fps",         "Frame rate",            "Average frames per second of the game in front.", true },
    { Metric::FrameTime,   Group::Frames, "frametime",   "Frame time",            "Average time per frame in milliseconds.", true },
    { Metric::Low1,        Group::Frames, "low1",        "1% low",                "Average FPS of the slowest 1% of frames. Shows stutter that the average hides.", true },
    { Metric::Low01,       Group::Frames, "low01",       "0.1% low",              "Average FPS of the slowest 0.1% of frames. Catches rare hitches.", false },
    { Metric::FrameGraph,  Group::Frames, "framegraph",  "Frame time graph",      "Live graph of recent frame times. Spikes are stutters.", true },
    { Metric::Api,         Group::Frames, "api",         "Graphics API",          "DirectX 12, DirectX 11, Vulkan, OpenGL or DirectX 9.", true },

    { Metric::GpuLoad,     Group::Gpu,    "gpuload",     "GPU usage",             nullptr, true },
    { Metric::GpuTemp,     Group::Gpu,    "gputemp",     "GPU temperature",       nullptr, true },
    { Metric::GpuHotspot,  Group::Gpu,    "gpuhotspot",  "GPU hot spot",          "Hottest point on the GPU die. Uses a slower sensor path.", false },
    { Metric::GpuPower,    Group::Gpu,    "gpupower",    "GPU power",             nullptr, false },
    { Metric::GpuClock,    Group::Gpu,    "gpuclock",    "GPU clock",             nullptr, false },
    { Metric::GpuMemClock, Group::Gpu,    "gpumemclock", "GPU memory clock",      nullptr, false },
    { Metric::GpuFan,      Group::Gpu,    "gpufan",      "GPU fan",               nullptr, false },
    { Metric::Vram,        Group::Gpu,    "vram",        "Video memory",          nullptr, true },

    { Metric::CpuLoad,     Group::Cpu,    "cpuload",     "CPU usage",             nullptr, true },
    { Metric::CpuTemp,     Group::Cpu,    "cputemp",     "CPU temperature",       "Needs the PawnIO driver.", true },
    { Metric::CpuPower,    Group::Cpu,    "cpupower",    "CPU power",             "Needs the PawnIO driver.", false },
    { Metric::CpuClock,    Group::Cpu,    "cpuclock",    "CPU clock",             "Effective clock, the same number Task Manager shows.", false },
    { Metric::CpuFan,      Group::Cpu,    "cpufan",      "CPU fan",               "Needs the PawnIO driver and a supported motherboard.", false },

    { Metric::Ram,         Group::Memory, "ram",         "System memory",         nullptr, true },

    { Metric::Process,     Group::Info,   "process",     "Game name",             "Name of the program being measured.", true },
    { Metric::Time,        Group::Info,   "time",        "Clock",                 "Current local time.", false },
};
static_assert(sizeof(kMetrics) / sizeof(kMetrics[0]) == (size_t)Metric::Count, "metric table out of sync");

const char* kGroupLabels[] = { "Frames", "Graphics card", "Processor", "Memory", "Info" };
static_assert(sizeof(kGroupLabels) / sizeof(kGroupLabels[0]) == (size_t)Group::Count, "group table out of sync");

const char* kHotkeyKeys[] = { "toggle", "reset", "settings", "exit" };
static_assert(sizeof(kHotkeyKeys) / sizeof(kHotkeyKeys[0]) == (size_t)HotkeyAction::Count, "hotkey table out of sync");

const char* kLayoutNames[] = { "vertical", "horizontal", "bar" };
const char* kAnchorNames[] = { "top-left", "top-center", "top-right", "bottom-left", "bottom-center", "bottom-right" };

template <typename E, size_t N>
E ParseEnum(const std::string& s, const char* (&names)[N], E def)
{
    for (size_t i = 0; i < N; ++i)
        if (_stricmp(s.c_str(), names[i]) == 0) return (E)i;
    return def;
}

int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

int Snap(int v, std::initializer_list<int> allowed, int def)
{
    for (int a : allowed)
        if (a == v) return v;
    return def;
}

} // namespace

const MetricInfo& Info(Metric m) { return kMetrics[(int)m]; }
const char* GroupLabel(Group g) { return kGroupLabels[(int)g]; }

Config::Config()
{
    for (const auto& m : kMetrics) show[(int)m.id] = m.defaultOn;
    hotkeys[(int)HotkeyAction::ToggleHud] = { VK_INSERT, 0 };
}

std::string HotkeyToString(const Hotkey& h)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%u,%u", h.vk, h.mods);
    return buf;
}

Hotkey HotkeyFromString(const std::string& s)
{
    Hotkey h;
    unsigned vk = 0, mods = 0;
    if (sscanf_s(s.c_str(), "%u,%u", &vk, &mods) >= 1 && vk < 256) {
        h.vk = vk;
        h.mods = mods & (MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN);
    }
    return h;
}

void Load(Config& c, const Ini& ini)
{
    c = Config();

    for (const auto& m : kMetrics)
        c.show[(int)m.id] = ini.GetBool("Metrics", m.key, m.defaultOn);

    c.layout = ParseEnum(ini.Get("Hud", "layout"), kLayoutNames, Layout::Vertical);
    c.anchor = ParseEnum(ini.Get("Hud", "anchor"), kAnchorNames, Anchor::TopLeft);
    c.monitor = ini.Get("Hud", "monitor");
    c.customPos = ini.GetBool("Hud", "customPos", false);
    c.posX = ini.GetInt("Hud", "posX", 0);
    c.posY = ini.GetInt("Hud", "posY", 0);
    c.margin = Clamp(ini.GetInt("Hud", "margin", c.margin), 0, 200);
    c.scale = Clamp(ini.GetInt("Hud", "scale", c.scale), 50, 250);
    c.bgOpacity = Clamp(ini.GetInt("Hud", "bgOpacity", c.bgOpacity), 0, 100);
    c.textOpacity = Clamp(ini.GetInt("Hud", "textOpacity", c.textOpacity), 20, 100);
    c.accent = Clamp(ini.GetInt("Hud", "accent", c.accent), 0, 7);
    c.colorValues = ini.GetBool("Hud", "colorValues", c.colorValues);
    c.hideFromCapture = ini.GetBool("Hud", "hideFromCapture", c.hideFromCapture);
    c.showLabels = ini.GetBool("Hud", "showLabels", c.showLabels);
    c.hideWhenIdle = ini.GetBool("Hud", "hideWhenIdle", c.hideWhenIdle);

    c.hudFps = Snap(ini.GetInt("Timing", "hudFps", c.hudFps), { 15, 30, 60, 120 }, 30);
    c.statsIntervalMs = Snap(ini.GetInt("Timing", "statsIntervalMs", c.statsIntervalMs), { 250, 500, 1000 }, 500);
    c.lowsWindowSec = Snap(ini.GetInt("Timing", "lowsWindowSec", c.lowsWindowSec), { 5, 10, 30, 60 }, 10);
    c.sensorIntervalMs = Snap(ini.GetInt("Timing", "sensorIntervalMs", c.sensorIntervalMs), { 500, 1000, 2000 }, 1000);

    c.fahrenheit = ini.GetBool("Units", "fahrenheit", c.fahrenheit);
    c.time24h = ini.GetBool("Units", "time24h", c.time24h);
    c.timeSeconds = ini.GetBool("Units", "timeSeconds", c.timeSeconds);

    c.gpu = ini.Get("Sensors", "gpu");
    c.cpuTempSensor = ini.Get("Sensors", "cpuTempSensor");
    c.cpuFanSensor = ini.Get("Sensors", "cpuFanSensor");

    for (int i = 0; i < (int)HotkeyAction::Count; ++i) {
        if (ini.Has("Hotkeys", kHotkeyKeys[i]))
            c.hotkeys[i] = HotkeyFromString(ini.Get("Hotkeys", kHotkeyKeys[i]));
    }

    c.language = ini.Get("General", "language", "en-US");
    if (c.language.empty() || c.language.size() > 16) c.language = "en-US";
    c.settingsPage = Clamp(ini.GetInt("General", "settingsPage", 0), 0, 15);
    c.firstRunDone = ini.GetBool("General", "firstRunDone", false);
}

void Store(const Config& c, Ini& ini)
{
    ini.SetInt("General", "configVersion", 2);
    ini.Set("General", "language", c.language);
    ini.SetInt("General", "settingsPage", c.settingsPage);
    ini.SetBool("General", "firstRunDone", c.firstRunDone);

    for (const auto& m : kMetrics)
        ini.SetBool("Metrics", m.key, c.show[(int)m.id]);

    ini.Set("Hud", "layout", kLayoutNames[(int)c.layout]);
    ini.Set("Hud", "anchor", kAnchorNames[(int)c.anchor]);
    ini.Set("Hud", "monitor", c.monitor);
    ini.SetBool("Hud", "customPos", c.customPos);
    ini.SetInt("Hud", "posX", c.posX);
    ini.SetInt("Hud", "posY", c.posY);
    ini.SetInt("Hud", "margin", c.margin);
    ini.SetInt("Hud", "scale", c.scale);
    ini.SetInt("Hud", "bgOpacity", c.bgOpacity);
    ini.SetInt("Hud", "textOpacity", c.textOpacity);
    ini.SetInt("Hud", "accent", c.accent);
    ini.SetBool("Hud", "colorValues", c.colorValues);
    ini.SetBool("Hud", "hideFromCapture", c.hideFromCapture);
    ini.SetBool("Hud", "showLabels", c.showLabels);
    ini.SetBool("Hud", "hideWhenIdle", c.hideWhenIdle);

    ini.SetInt("Timing", "hudFps", c.hudFps);
    ini.SetInt("Timing", "statsIntervalMs", c.statsIntervalMs);
    ini.SetInt("Timing", "lowsWindowSec", c.lowsWindowSec);
    ini.SetInt("Timing", "sensorIntervalMs", c.sensorIntervalMs);

    ini.SetBool("Units", "fahrenheit", c.fahrenheit);
    ini.SetBool("Units", "time24h", c.time24h);
    ini.SetBool("Units", "timeSeconds", c.timeSeconds);

    ini.Set("Sensors", "gpu", c.gpu);
    ini.Set("Sensors", "cpuTempSensor", c.cpuTempSensor);
    ini.Set("Sensors", "cpuFanSensor", c.cpuFanSensor);

    for (int i = 0; i < (int)HotkeyAction::Count; ++i)
        ini.Set("Hotkeys", kHotkeyKeys[i], HotkeyToString(c.hotkeys[i]));
}

bool LoadFile(const std::wstring& path, Config& c)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        c = Config();
        return false;
    }
    LARGE_INTEGER size = {};
    GetFileSizeEx(h, &size);
    std::string text;
    if (size.QuadPart > 0 && size.QuadPart < 4 * 1024 * 1024) {
        text.resize((size_t)size.QuadPart);
        DWORD read = 0;
        if (!ReadFile(h, text.data(), (DWORD)text.size(), &read, nullptr)) read = 0;
        text.resize(read);
    }
    CloseHandle(h);

    Ini ini;
    ini.Parse(text);
    Load(c, ini);
    return true;
}

bool SaveFile(const std::wstring& path, const Config& c)
{
    // Start from the existing file so unknown keys (newer versions, hand edits) survive.
    Ini ini;
    {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER size = {};
            GetFileSizeEx(h, &size);
            if (size.QuadPart > 0 && size.QuadPart < 4 * 1024 * 1024) {
                std::string text((size_t)size.QuadPart, '\0');
                DWORD read = 0;
                if (ReadFile(h, text.data(), (DWORD)text.size(), &read, nullptr)) {
                    text.resize(read);
                    ini.Parse(text);
                }
            }
            CloseHandle(h);
        }
    }
    Store(c, ini);
    const std::string text = ini.Serialize();

    const std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(h, text.data(), (DWORD)text.size(), &written, nullptr) && written == text.size();
    FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

} // namespace cfg
