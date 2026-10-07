#include "sensors/lhm_select.h"

#include <algorithm>

namespace {

std::string Lower(const std::string& s)
{
    std::string o(s);
    for (char& c : o)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return o;
}

bool Has(const std::string& lowerHaystack, const char* needle)
{
    return lowerHaystack.find(needle) != std::string::npos;
}

bool IsGpuType(const std::string& hwType) { return hwType.rfind("Gpu", 0) == 0; }
bool IsCpuType(const std::string& hwType) { return hwType == "Cpu"; }

int CpuTempScore(const LhmSensor& s)
{
    const std::string n = Lower(s.name);
    if (Has(n, "distance to tjmax")) return -1;          // headroom, not a temperature
    if (n == "core (tctl/tdie)") return 100;             // AMD Ryzen, what Ryzen Master shows
    if (n == "cpu package") return 95;                   // Intel
    if (n == "package") return 90;
    if ((Has(n, "tctl") || Has(n, "tdie")) && !Has(n, "ccd")) return 85;
    if (n == "core max") return 70;
    if (n == "core average") return 60;
    if (Has(n, "ccd")) return 50;
    if (n == "soc") return 40;                           // AMD handheld APUs
    return 10;
}

int CpuPowerScore(const LhmSensor& s)
{
    const std::string n = Lower(s.name);
    if (n == "cpu package") return 100;
    if (n == "package") return 90;
    if (Has(n, "package")) return 80;
    if (Has(n, "ppt")) return 70;
    if (n == "cpu cores") return 20;
    return 10;
}

int VramTotalScore(const std::string& n)
{
    if (Has(n, "shared")) return -1;
    if (n == "gpu memory total") return 100;
    if (Has(n, "dedicated memory total")) return 80;
    if (Has(n, "memory total")) return 50;
    return -1;
}

int VramUsedScore(const std::string& n)
{
    if (Has(n, "shared")) return -1;
    if (n == "gpu memory used") return 100;
    if (Has(n, "dedicated memory used")) return 80;
    if (Has(n, "memory used")) return 50;
    return -1;
}

void Consider(int& slot, int& best, int idx, int score)
{
    if (score < 0) return;
    if (slot < 0 || score > best) { slot = idx; best = score; }
}

} // namespace

LhmPicks SelectLhmSensors(const std::vector<LhmSensor>& sensors, const std::string& cpuTempPref,
                          const std::string& cpuFanPref)
{
    LhmPicks p;
    int bestTemp = -1, bestPower = -1;
    std::vector<std::pair<int, int>> tempChoices;    // score, index

    struct GpuScores { int temp = -1, hotspot = -1, power = -1, core = -1, mem = -1, fan = -1, load = -1, used = -1, total = -1; };
    std::vector<GpuScores> gs;

    for (int i = 0; i < (int)sensors.size(); ++i) {
        const LhmSensor& s = sensors[i];
        const std::string n = Lower(s.name);

        if (IsCpuType(s.hwType)) {
            if (s.type == "Temperature") {
                const int sc = CpuTempScore(s);
                if (sc >= 0) tempChoices.emplace_back(sc, i);
                Consider(p.cpuTemp, bestTemp, i, sc);
            } else if (s.type == "Power") {
                Consider(p.cpuPower, bestPower, i, CpuPowerScore(s));
            }
            continue;
        }

        if (IsGpuType(s.hwType)) {
            size_t g = 0;
            while (g < p.gpus.size() && p.gpus[g].hardware != s.hardware) ++g;
            if (g == p.gpus.size()) {
                LhmGpuPick pick;
                pick.hardware = s.hardware;
                pick.hwName = s.hwName;
                pick.hwType = s.hwType;
                p.gpus.push_back(pick);
                gs.emplace_back();
            }
            LhmGpuPick& pk = p.gpus[g];
            GpuScores& sc = gs[g];
            const bool hot = Has(n, "hot spot") || Has(n, "hotspot");
            if (s.type == "Temperature") {
                if (hot) Consider(pk.hotspot, sc.hotspot, i, 100);
                else if (Has(n, "memory")) {}
                else if (n == "gpu core") Consider(pk.temp, sc.temp, i, 100);
                else if (Has(n, "core") || Has(n, "edge")) Consider(pk.temp, sc.temp, i, 80);
                else Consider(pk.temp, sc.temp, i, 10);
            } else if (s.type == "Power") {
                if (n == "gpu package") Consider(pk.power, sc.power, i, 100);
                else if (n == "gpu power" || Has(n, "ppt")) Consider(pk.power, sc.power, i, 90);
                else if (Has(n, "board")) Consider(pk.power, sc.power, i, 85);
                else if (n == "gpu core") Consider(pk.power, sc.power, i, 50);
                else Consider(pk.power, sc.power, i, 10);
            } else if (s.type == "Clock") {
                if (Has(n, "memory")) Consider(pk.memClock, sc.mem, i, 100);
                else if (n == "gpu core") Consider(pk.coreClock, sc.core, i, 100);
                else if (Has(n, "core") || Has(n, "shader")) Consider(pk.coreClock, sc.core, i, 80);
            } else if (s.type == "Fan") {
                Consider(pk.fan, sc.fan, i, n == "gpu fan" ? 100 : 50 - (int)std::min<size_t>(n.size(), 40));
            } else if (s.type == "Load") {
                if (n == "gpu core") Consider(pk.load, sc.load, i, 100);
                else if (n == "d3d 3d") Consider(pk.load, sc.load, i, 80);
            } else if (s.type == "SmallData" || s.type == "Data") {
                Consider(pk.vramUsed, sc.used, i, VramUsedScore(n));
                Consider(pk.vramTotal, sc.total, i, VramTotalScore(n));
            }
            continue;
        }

        if (s.type == "Fan") p.fanChoices.push_back(i);
    }

    std::stable_sort(tempChoices.begin(), tempChoices.end(),
                     [](const auto& a, const auto& b) { return a.first > b.first; });
    for (const auto& c : tempChoices) p.cpuTempChoices.push_back(c.second);

    if (!cpuTempPref.empty()) {
        for (int idx : p.cpuTempChoices)
            if (sensors[idx].id == cpuTempPref) { p.cpuTemp = idx; break; }
    }

    // CPU fan: an explicit choice, else a fan the board config names after the CPU.
    for (int idx : p.fanChoices) {
        if (!cpuFanPref.empty() && sensors[idx].id == cpuFanPref) { p.cpuFan = idx; break; }
    }
    if (p.cpuFan < 0 && cpuFanPref.empty()) {
        for (int idx : p.fanChoices)
            if (Has(Lower(sensors[idx].name), "cpu")) { p.cpuFan = idx; break; }
    }
    return p;
}

int MatchLhmGpu(const LhmPicks& picks, const std::string& adapterName, unsigned vendorId)
{
    if (picks.gpus.empty()) return -1;
    const std::string a = Lower(adapterName);
    for (size_t i = 0; i < picks.gpus.size(); ++i)
        if (Lower(picks.gpus[i].hwName) == a) return (int)i;
    for (size_t i = 0; i < picks.gpus.size(); ++i) {
        const std::string h = Lower(picks.gpus[i].hwName);
        if (!h.empty() && !a.empty() && (a.find(h) != std::string::npos || h.find(a) != std::string::npos))
            return (int)i;
    }
    // Names differ (driver vs. LHM naming): accept the only GPU of the same vendor.
    const char* type = vendorId == 0x10DE ? "GpuNvidia" : vendorId == 0x1002 ? "GpuAmd"
                     : vendorId == 0x8086 ? "GpuIntel" : nullptr;
    if (!type) return -1;
    int found = -1;
    for (size_t i = 0; i < picks.gpus.size(); ++i) {
        if (picks.gpus[i].hwType != type) continue;
        if (found >= 0) return -1;      // ambiguous
        found = (int)i;
    }
    return found;
}
