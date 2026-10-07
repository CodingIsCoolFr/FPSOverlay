#include "app/updater.h"
#include "app/log.h"
#include "app/version.h"
#include "platform/win_util.h"

#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>
#include <bcrypt.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace fs = std::filesystem;

namespace updater {
namespace {

constexpr char kAssetName[] = "FPSOverlay.zip";
constexpr unsigned long long kMaxDownload = 128ull << 20;

std::mutex g_mutex;
Status g_status;                    // guarded by g_mutex
std::atomic<bool> g_busy{ false };

void SetStatus(State s, const std::string& version = {}, const std::string& error = {}, float progress = 0.f)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_status.state = s;
    g_status.version = version;
    g_status.error = error;
    g_status.progress = progress;
}

std::wstring UpdateDir() { return win::ExeDir() + L"update\\"; }

std::string Narrow(const std::wstring& w) { return win::ToUtf8(w); }

// ── HTTPS ───────────────────────────────────────────────────────────────────

struct Handle {
    HINTERNET h = nullptr;
    ~Handle() { if (h) WinHttpCloseHandle(h); }
};

// GET over HTTPS. Redirects (github.com sends downloads on to its file CDN) are followed, never
// to plain HTTP. The body goes to `file` when given, else to `body`.
bool HttpGet(const std::wstring& url, const wchar_t* accept, std::string* body, HANDLE file, unsigned long long sizeHint,
             std::string& err)
{
    wchar_t host[256] = {}, path[2048] = {}, extra[2048] = {};
    URL_COMPONENTS uc = { sizeof(uc) };
    uc.lpszHostName = host;
    uc.dwHostNameLength = ARRAYSIZE(host);
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = ARRAYSIZE(path);
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = ARRAYSIZE(extra);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) {
        err = "Bad download address.";
        return false;
    }
    const std::wstring object = std::wstring(path, uc.dwUrlPathLength) + std::wstring(extra, uc.dwExtraInfoLength);

    Handle session, conn, req;
    session.h = WinHttpOpen(L"FPSOverlay/" APP_STR(APP_VERSION_MAJOR) L"." APP_STR(APP_VERSION_MINOR) L"." APP_STR(APP_VERSION_PATCH),
                            WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.h) {
        err = "Network unavailable.";
        return false;
    }
    WinHttpSetTimeouts(session.h, 10000, 10000, 15000, 30000);
    conn.h = WinHttpConnect(session.h, host, uc.nPort, 0);
    req.h = conn.h ? WinHttpOpenRequest(conn.h, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                   : nullptr;
    if (!req.h) {
        err = "Network unavailable.";
        return false;
    }
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(req.h, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
    const std::wstring headers = std::wstring(L"Accept: ") + accept + L"\r\n";
    if (!WinHttpSendRequest(req.h, headers.c_str(), (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req.h, nullptr)) {
        err = "GitHub could not be reached (error " + std::to_string(GetLastError()) + ").";
        return false;
    }
    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status,
                        &size, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        err = "GitHub answered HTTP " + std::to_string(status) + ".";
        return false;
    }

    std::vector<char> buf(64 * 1024);
    unsigned long long total = 0;
    for (;;) {
        DWORD got = 0;
        if (!WinHttpReadData(req.h, buf.data(), (DWORD)buf.size(), &got)) {
            err = "The download was interrupted.";
            return false;
        }
        if (got == 0) break;
        total += got;
        if (total > kMaxDownload) {
            err = "The download is too large.";
            return false;
        }
        if (file) {
            DWORD written = 0;
            if (!WriteFile(file, buf.data(), got, &written, nullptr) || written != got) {
                err = "The update could not be saved.";
                return false;
            }
            if (sizeHint) {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_status.progress = (float)std::min(1.0, (double)total / (double)sizeHint);
            }
        } else if (body) {
            body->append(buf.data(), got);
        }
    }
    return true;
}

// ── Files ───────────────────────────────────────────────────────────────────

std::string Sha256OfFile(const std::wstring& path)
{
    std::string hex;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) return hex;
    if (BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0))) {
        HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            std::vector<unsigned char> buf(1 << 16);
            DWORD n = 0;
            bool ok = true;
            while (ok && ReadFile(f, buf.data(), (DWORD)buf.size(), &n, nullptr) && n > 0)
                ok = BCRYPT_SUCCESS(BCryptHashData(hash, buf.data(), n, 0));
            CloseHandle(f);
            unsigned char digest[32];
            if (ok && BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0))) {
                static const char kHex[] = "0123456789abcdef";
                for (unsigned char b : digest) {
                    hex.push_back(kHex[b >> 4]);
                    hex.push_back(kHex[b & 15]);
                }
            }
        }
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return hex;
}

std::string FileVersionOf(const std::wstring& path)
{
    DWORD unused = 0;
    const DWORD n = GetFileVersionInfoSizeW(path.c_str(), &unused);
    if (!n) return {};
    std::vector<unsigned char> buf(n);
    VS_FIXEDFILEINFO* fi = nullptr;
    UINT len = 0;
    if (!GetFileVersionInfoW(path.c_str(), 0, n, buf.data()) || !VerQueryValueW(buf.data(), L"\\", (void**)&fi, &len) || !fi)
        return {};
    char v[48];
    snprintf(v, sizeof(v), "%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS), HIWORD(fi->dwFileVersionLS));
    return v;
}

bool RunHidden(std::wstring cmd, const std::wstring& dir, DWORD timeoutMs, DWORD& exitCode)
{
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi))
        return false;
    const bool done = WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_OBJECT_0;
    if (!done) TerminateProcess(pi.hProcess, 1);
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return done;
}

// Starts exe with args in dir. An elevated parent starts an elevated child without a prompt; if
// the parent is not elevated and the new exe asks for admin, Windows shows the usual prompt.
bool StartProcess(const std::wstring& exe, const std::wstring& args, const std::wstring& dir, DWORD& err)
{
    std::wstring cmd = L"\"" + exe + L"\"" + args;
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return true;
    }
    err = GetLastError();
    if (err != ERROR_ELEVATION_REQUIRED) return false;
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.fMask = SEE_MASK_NOASYNC;
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = args.c_str();
    sei.lpDirectory = dir.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&sei)) return true;
    err = GetLastError();
    return false;
}

// Removes the update folder and the *.old files an install leaves next to the exe. The
// installer process may still be exiting right after an update, hence the retries.
void CleanupLeftovers()
{
    for (int attempt = 0; attempt < 20; ++attempt) {
        bool left = false;
        std::error_code ec;
        for (const std::wstring& dir : { win::ExeDir(), win::ExeDir() + L"locales\\" }) {
            for (const auto& e : fs::directory_iterator(dir, ec)) {
                if (e.path().extension() == L".old" && !DeleteFileW(e.path().c_str())) left = true;
            }
        }
        if (fs::exists(UpdateDir(), ec)) {
            fs::remove_all(UpdateDir(), ec);
            if (ec) left = true;
        }
        if (!left) return;
        Sleep(500);
    }
    logx::Warn("Update: leftovers could not be removed yet");
}

// ── Check, download, unpack ─────────────────────────────────────────────────

void Worker(bool download)
{
    CleanupLeftovers();

    std::string body, err;
    const std::wstring api = L"https://api.github.com/repos/" + win::ToWide(APP_UPDATE_REPO) + L"/releases/latest";
    if (!HttpGet(api, L"application/vnd.github+json", &body, nullptr, 0, err)) {
        logx::Warn("Update check: %s", err.c_str());
        SetStatus(State::Failed, {}, err);
        return;
    }
    ReleaseInfo rel;
    if (!ParseRelease(body, kAssetName, rel)) {
        SetStatus(State::Failed, {}, "The release information could not be read.");
        return;
    }
    if (CompareVersions(rel.tag, APP_VERSION) <= 0) {
        logx::Info("Update check: %s is the newest", APP_VERSION);
        SetStatus(State::UpToDate);
        return;
    }
    const std::string version = (rel.tag[0] == 'v' || rel.tag[0] == 'V') ? rel.tag.substr(1) : rel.tag;
    logx::Info("Update check: %s is available", version.c_str());
    if (!download) {
        SetStatus(State::Available, version);
        return;
    }
    if (rel.assetUrl.empty() || rel.sha256.size() != 64) {
        SetStatus(State::Failed, version, "This release has no verified download.");
        return;
    }

    const std::wstring dir = UpdateDir();
    const std::wstring zip = dir + L"FPSOverlay.zip";
    const std::wstring part = zip + L".part";
    CreateDirectoryW(dir.c_str(), nullptr);
    HANDLE f = CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        SetStatus(State::Failed, version, "The app folder is not writable, so the update cannot be installed here.");
        return;
    }
    SetStatus(State::Downloading, version);
    const bool got = HttpGet(win::ToWide(rel.assetUrl), L"application/octet-stream", nullptr, f, rel.size, err);
    CloseHandle(f);
    if (!got) {
        DeleteFileW(part.c_str());
        SetStatus(State::Failed, version, err);
        return;
    }
    if (Sha256OfFile(part) != rel.sha256) {
        DeleteFileW(part.c_str());
        logx::Warn("Update: SHA-256 mismatch for %s", version.c_str());
        SetStatus(State::Failed, version, "The download did not match its SHA-256 fingerprint, so it was thrown away.");
        return;
    }
    MoveFileExW(part.c_str(), zip.c_str(), MOVEFILE_REPLACE_EXISTING);

    // Windows 10 1803 and later ship bsdtar, which reads zip files.
    wchar_t sys[MAX_PATH] = {};
    GetSystemDirectoryW(sys, MAX_PATH);
    DWORD code = 1;
    const std::wstring tar = L"\"" + std::wstring(sys) + L"\\tar.exe\" -xf \"" + zip + L"\" -C \"" + dir.substr(0, dir.size() - 1) + L"\"";
    if (!RunHidden(tar, dir, 60000, code) || code != 0) {
        SetStatus(State::Failed, version, "The update could not be unpacked.");
        return;
    }
    const std::wstring exe = dir + L"FPSOverlay\\FPSOverlay.exe";
    const std::string exeVersion = FileVersionOf(exe);
    if (exeVersion.empty() || CompareVersions(exeVersion, version) != 0) {
        SetStatus(State::Failed, version, "The downloaded app does not carry the expected version.");
        return;
    }
    logx::Info("Update: %s downloaded and verified", version.c_str());
    SetStatus(State::Ready, version);
}

} // namespace

bool Enabled() { return APP_UPDATE_REPO[0] != '\0'; }

Status GetStatus()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    Status s = g_status;
    if (!Enabled()) s.state = State::Off;
    else if (s.state == State::Off) s.state = State::Idle;
    return s;
}

std::wstring ReleasePageUrl()
{
    return L"https://github.com/" + win::ToWide(APP_UPDATE_REPO) + L"/releases/latest";
}

void CheckAsync(bool download)
{
    if (!Enabled()) return;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_status.state == State::Ready) return;     // already waiting to install
    }
    if (g_busy.exchange(true)) return;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_status.state = State::Checking;
        g_status.error.clear();
    }
    std::thread([download] {
        Worker(download);
        g_busy = false;
    }).detach();
}

bool LaunchInstaller(bool startInTray, std::string& err)
{
    if (GetStatus().state != State::Ready) {
        err = "No update is ready.";
        return false;
    }
    std::wstring appDir = win::ExeDir();
    appDir.pop_back();      // a quoted path must not end in a backslash
    const std::wstring stage = UpdateDir() + L"FPSOverlay";
    const std::wstring args = L" --finish-update " + std::to_wstring(GetCurrentProcessId()) + L" \"" + appDir + L"\"" +
                              (startInTray ? L" --tray" : L"");
    DWORD code = 0;
    if (!StartProcess(stage + L"\\FPSOverlay.exe", args, stage, code)) {
        err = "The installer could not start (error " + std::to_string(code) + ").";
        SetStatus(State::Failed, GetStatus().version, err);
        return false;
    }
    logx::Info("Update: installer started, exiting");
    return true;
}

int FinishUpdateIfAsked(int argc, wchar_t** argv)
{
    int at = -1;
    bool tray = false;
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--finish-update") == 0 && i + 2 < argc) at = i;
        if (_wcsicmp(argv[i], L"--tray") == 0) tray = true;
    }
    if (at < 0) return -1;
    const DWORD oldPid = (DWORD)_wtoi(argv[at + 1]);
    const fs::path appDir = argv[at + 2];
    const fs::path stage = win::ExeDir();

    // The old version is shutting down; give it time to stop its capture and save settings.
    if (HANDLE old = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, oldPid)) {
        if (WaitForSingleObject(old, 30000) == WAIT_TIMEOUT) {
            TerminateProcess(old, 1);
            WaitForSingleObject(old, 5000);
        }
        CloseHandle(old);
    }
    logx::Init((appDir / L"FPSOverlay.log").wstring());
    logx::Info("Installing %s %s into %s", APP_NAME, APP_VERSION, Narrow(appDir.wstring()).c_str());

    // Antivirus scanners briefly lock new files, so every step retries for a few seconds.
    auto retry = [](auto&& step) {
        for (int i = 0; i < 20; ++i) {
            if (step()) return true;
            Sleep(250);
        }
        return false;
    };
    std::vector<std::pair<fs::path, bool>> done;    // target, an .old copy exists
    bool ok = true;
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(stage, ec)) {
        if (!e.is_regular_file()) continue;
        const fs::path target = appDir / fs::relative(e.path(), stage, ec);
        fs::path backup = target;
        backup += L".old";
        fs::create_directories(target.parent_path(), ec);
        const bool existed = fs::exists(target);
        if (existed && !retry([&] { return MoveFileExW(target.c_str(), backup.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE; })) {
            logx::Error("Update: %s is in use", Narrow(target.wstring()).c_str());
            ok = false;
            break;
        }
        done.emplace_back(target, existed);
        if (!retry([&] { return CopyFileW(e.path().c_str(), target.c_str(), FALSE) != FALSE; })) {
            logx::Error("Update: %s could not be written (error %lu)", Narrow(target.wstring()).c_str(), GetLastError());
            ok = false;
            break;
        }
    }
    if (!ok) {
        for (auto it = done.rbegin(); it != done.rend(); ++it) {
            DeleteFileW(it->first.c_str());
            if (it->second) {
                fs::path backup = it->first;
                backup += L".old";
                MoveFileExW(backup.c_str(), it->first.c_str(), MOVEFILE_REPLACE_EXISTING);
            }
        }
        logx::Error("Update failed; the previous version was restored");
    } else {
        logx::Info("Update: %zu files installed", done.size());
    }

    // Start whichever version is now in place.
    DWORD code = 0;
    if (!StartProcess((appDir / L"FPSOverlay.exe").wstring(),
                      std::wstring(tray ? L" --tray" : L"") + (ok ? L" --updated" : L" --update-failed"), appDir.wstring(), code))
        logx::Error("Update: the app could not be restarted (error %lu)", code);
    logx::Shutdown();
    return ok ? 0 : 1;
}

} // namespace updater
