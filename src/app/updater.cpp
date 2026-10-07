#include "app/updater.h"
#include "app/log.h"
#include "app/version.h"
#include "platform/win_util.h"

#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace updater {
namespace {

std::atomic<bool> g_busy{ false };
std::mutex g_mutex;
std::string g_newer;

std::string FetchLatestTag()
{
    std::string tag;
    HINTERNET session = WinHttpOpen(L"FPSOverlay/" APP_STR(APP_VERSION_MAJOR) L"." APP_STR(APP_VERSION_MINOR),
                                    WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return tag;
    WinHttpSetTimeouts(session, 5000, 5000, 5000, 5000);
    HINTERNET conn = WinHttpConnect(session, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    const std::wstring path = L"/repos/" + win::ToWide(APP_UPDATE_REPO) + L"/releases/latest";
    HINTERNET req = conn ? WinHttpOpenRequest(conn, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                              WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                         : nullptr;
    if (req && WinHttpSendRequest(req, L"Accept: application/vnd.github+json\r\n", (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(req, nullptr)) {
        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status,
                            &size, WINHTTP_NO_HEADER_INDEX);
        std::string body;
        char buf[4096];
        DWORD read = 0;
        while (status == 200 && body.size() < 512 * 1024 && WinHttpReadData(req, buf, sizeof(buf), &read) && read > 0)
            body.append(buf, read);
        const size_t key = body.find("\"tag_name\"");
        if (key != std::string::npos) {
            const size_t colon = body.find(':', key);
            const size_t q1 = colon == std::string::npos ? colon : body.find('"', colon + 1);
            const size_t q2 = q1 == std::string::npos ? q1 : body.find('"', q1 + 1);
            if (q2 != std::string::npos && q2 - q1 < 64) tag = body.substr(q1 + 1, q2 - q1 - 1);
        }
        if (status != 200) logx::Warn("Update check: HTTP %lu", status);
    }
    if (req) WinHttpCloseHandle(req);
    if (conn) WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return tag;
}

} // namespace

bool Enabled() { return APP_UPDATE_REPO[0] != '\0'; }
bool Busy() { return g_busy.load(); }

std::string NewerVersion()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_newer;
}

std::wstring ReleasePageUrl()
{
    return L"https://github.com/" + win::ToWide(APP_UPDATE_REPO) + L"/releases/latest";
}

void CheckAsync()
{
    if (!Enabled() || g_busy.exchange(true)) return;
    std::thread([] {
        const std::string tag = FetchLatestTag();
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_newer = (!tag.empty() && CompareVersions(tag, APP_VERSION) > 0) ? tag : std::string();
        }
        logx::Info("Update check: latest '%s'", tag.c_str());
        g_busy = false;
    }).detach();
}

} // namespace updater
