// PawnIO: the signed kernel driver LibreHardwareMonitor uses to read CPU temperature, power
// and motherboard fans. Optional: without it everything else keeps working.
#pragma once

#include <cstdint>
#include <string>

namespace pawnio {

struct Version {
    uint32_t ms = 0, ls = 0;            // VS_FIXEDFILEINFO style: major.minor / patch.build
    bool valid() const { return ms || ls; }
    std::string ToString() const;
};

int  Compare(const Version& a, const Version& b);

bool     IsInstalled();
Version  InstalledVersion();
Version  BundledVersion();              // the installer embedded in our exe

enum class InstallResult { Ok, Failed, Missing };
// Runs the embedded installer silently and waits for it. Blocks for several seconds:
// call it from a worker thread.
InstallResult RunInstaller(std::string& detail);

} // namespace pawnio
