// Unit tests for the pure logic: INI handling, config round trip, frame statistics and
// sensor selection. Plain asserts, no framework; exit code = number of failures.

#include "app/config.h"
#include "app/ini.h"
#include "app/updater.h"
#include "capture/frame_stats.h"
#include "sensors/cpu_direct.h"
#include "sensors/lhm_select.h"

#include <windows.h>

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

int main()
{
    TestVersions();
    TestCpuTempDecode();
    TestReleaseJson();
    TestIni();
    TestConfig();
    TestFrameStats();
    TestLhmSelect();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures;
}
