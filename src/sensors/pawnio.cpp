#include "sensors/pawnio.h"
#include "app/log.h"
#include "platform/win_util.h"

#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <vector>

namespace pawnio {
namespace {

const wchar_t* kUninstallKey = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\PawnIO";

bool ReadRegString(const wchar_t* value, std::wstring& out)
{
    wchar_t buf[1024];
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, kUninstallKey, value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
                     nullptr, buf, &size) != ERROR_SUCCESS)
        return false;
    out = buf;
    return !out.empty();
}

Version FileVersion(const std::wstring& path)
{
    Version v;
    DWORD dummy = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &dummy);
    if (!size) return v;
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return v;
    VS_FIXEDFILEINFO* ffi = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&ffi), &len) || !ffi || len < sizeof(*ffi))
        return v;
    v.ms = ffi->dwFileVersionMS;
    v.ls = ffi->dwFileVersionLS;
    return v;
}

Version ParseVersionString(const std::wstring& s)
{
    unsigned a = 0, b = 0, c = 0, d = 0;
    const wchar_t* p = s.c_str();
    while (*p == L'v' || *p == L'V' || *p == L' ') ++p;
    const int n = swscanf_s(p, L"%u.%u.%u.%u", &a, &b, &c, &d);
    Version v;
    if (n >= 1) {
        v.ms = (a << 16) | (b & 0xFFFF);
        v.ls = (c << 16) | (d & 0xFFFF);
    }
    return v;
}

// "C:\path\x.exe",0  /  "C:\path\x.exe" -arg  /  C:\path\x.exe
std::wstring FirstPath(const std::wstring& raw)
{
    std::wstring s = raw;
    while (!s.empty() && (s[0] == L' ' || s[0] == L'\t')) s.erase(0, 1);
    if (!s.empty() && s[0] == L'"') {
        const size_t q = s.find(L'"', 1);
        return q == std::wstring::npos ? s.substr(1) : s.substr(1, q - 1);
    }
    const size_t cut = s.find_first_of(L", ");
    return cut == std::wstring::npos ? s : s.substr(0, cut);
}

// The official, signed installer ships as its own file next to the app. It used to be embedded in
// the exe and copied to %TEMP% to run, which is what droppers do, and antivirus engines noticed.
std::wstring InstallerPath() { return win::ExeDir() + L"PawnIO_setup.exe"; }

} // namespace

std::string Version::ToString() const
{
    char buf[48];
    snprintf(buf, sizeof(buf), "%u.%u.%u", ms >> 16, ms & 0xFFFF, ls >> 16);
    return buf;
}

int Compare(const Version& a, const Version& b)
{
    if (a.ms != b.ms) return a.ms < b.ms ? -1 : 1;
    if (a.ls != b.ls) return a.ls < b.ls ? -1 : 1;
    return 0;
}

bool IsInstalled()
{
    std::wstring v;
    return ReadRegString(L"DisplayVersion", v);
}

Version InstalledVersion()
{
    std::wstring s;
    if (ReadRegString(L"DisplayIcon", s)) {
        Version v = FileVersion(FirstPath(s));
        if (v.valid()) return v;
    }
    if (ReadRegString(L"InstallLocation", s)) {
        if (!s.empty() && s.back() != L'\\') s += L'\\';
        Version v = FileVersion(s + L"PawnIO.exe");
        if (v.valid()) return v;
    }
    if (ReadRegString(L"UninstallString", s)) {
        Version v = FileVersion(FirstPath(s));
        if (v.valid()) return v;
    }
    if (ReadRegString(L"DisplayVersion", s)) return ParseVersionString(s);
    return {};
}

Version BundledVersion()
{
    static Version cached;
    static bool done = false;
    if (done) return cached;
    done = true;
    cached = FileVersion(InstallerPath());
    return cached;
}

InstallResult RunInstaller(std::string& detail)
{
    const std::wstring setup = InstallerPath();
    if (GetFileAttributesW(setup.c_str()) == INVALID_FILE_ATTRIBUTES) {
        detail = "The PawnIO installer is not bundled in this build.";
        return InstallResult::Missing;
    }

    const std::wstring dir = win::ExeDir();
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.lpVerb = win::IsElevated() ? L"open" : L"runas";
    sei.lpFile = setup.c_str();
    sei.lpParameters = L"-install";
    sei.lpDirectory = dir.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) {
        detail = "The installer could not be started (error " + std::to_string(GetLastError()) + ").";
        return InstallResult::Failed;
    }
    WaitForSingleObject(sei.hProcess, 5 * 60 * 1000);
    DWORD code = (DWORD)-1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);

    logx::Info("PawnIO installer exit code %lu", code);
    if (code != 0) {
        detail = "The installer reported error " + std::to_string(code) +
                 ". Removing the old PawnIO in Windows Settings > Apps and trying again usually fixes it.";
        return InstallResult::Failed;
    }
    return InstallResult::Ok;
}

} // namespace pawnio
