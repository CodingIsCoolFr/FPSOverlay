// Optional release check against GitHub (APP_UPDATE_REPO). Off when the repo is empty.
#pragma once

#include <cstdlib>
#include <string>

namespace updater {

bool Enabled();
void CheckAsync();                  // no-op while a check is running
bool Busy();
std::string NewerVersion();         // "2.1.0" when a newer release exists, else empty
std::wstring ReleasePageUrl();

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

} // namespace updater
