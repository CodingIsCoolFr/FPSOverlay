// Updates from GitHub releases (APP_UPDATE_REPO). Off when the repo is empty.
//
// 1. CheckAsync asks GitHub for the latest release. When it is newer and `download` is set, the
//    release zip goes to <exe folder>\update\, its SHA-256 is checked against the digest GitHub
//    publishes for the asset, it is unpacked, and the unpacked exe must carry the release's
//    version. State becomes Ready.
// 2. The app calls LaunchInstaller when no game is running and then exits. The new exe, started
//    from the update folder with --finish-update, waits for the old process to end, copies its
//    files over the old ones (keeping *.old copies to roll back to), and starts the new version.
// 3. The new version calls CleanupAfterUpdate, which removes the *.old files and the update folder.
#pragma once

#include <cstdlib>
#include <string>

namespace updater {

enum class State { Off, Idle, Checking, UpToDate, Available, Downloading, Ready, Failed };

struct Status {
    State state = State::Off;
    std::string version;        // the newer release ("2.1.0"), empty if none was found
    float progress = 0.f;       // 0..1 while downloading
    std::string error;          // English detail when Failed
};

bool Enabled();
Status GetStatus();
void CheckAsync(bool download);     // no-op while a check or download runs
std::wstring ReleasePageUrl();

// State must be Ready. Starts the installer process; the caller must exit right after.
bool LaunchInstaller(bool startInTray, std::string& err);

// Runs the --finish-update step when the command line asks for it and returns the process exit
// code; returns -1 for a normal start.
int FinishUpdateIfAsked(int argc, wchar_t** argv);

// Removes what an update left behind. Retries in the background for a few seconds, since the
// installer process may still be exiting.
void CleanupAfterUpdate();

// Compares dotted versions, ignoring a leading "v" and any "-suffix": <0, 0, >0.
inline int CompareVersions(const std::string& a, const std::string& b)
{
    auto parts = [](const std::string& s, int out[4]) {
        size_t i = 0;
        while (i < s.size() && (s[i] == 'v' || s[i] == 'V' || s[i] == ' ')) ++i;
        for (int k = 0; k < 4; ++k) {
            out[k] = 0;
            if (i < s.size() && s[i] >= '0' && s[i] <= '9') {
                out[k] = atoi(s.c_str() + i);
                while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
            }
            if (i < s.size() && s[i] == '.') ++i;
            else break;
        }
    };
    int x[4] = {}, y[4] = {};
    parts(a, x);
    parts(b, y);
    for (int k = 0; k < 4; ++k)
        if (x[k] != y[k]) return x[k] < y[k] ? -1 : 1;
    return 0;
}

// Finds the asset `assetName` in a GitHub "latest release" JSON body. Pure, unit-tested.
struct ReleaseInfo {
    std::string tag;            // "v2.1.0"
    std::string assetUrl;       // browser_download_url
    std::string sha256;         // lowercase hex from the asset's "digest", empty if missing
    unsigned long long size = 0;
};
bool ParseRelease(const std::string& json, const std::string& assetName, ReleaseInfo& out);

} // namespace updater
