// Reads the parts of GitHub's "latest release" JSON the updater needs. Pure C++, unit-tested.
#include "app/updater.h"

#include <cstdlib>
#include <string>

namespace updater {
namespace {

class JsonWalker {
public:
    JsonWalker(const std::string& s) : p_(s.data()), e_(s.data() + s.size()) {}

    bool Release(const std::string& assetName, ReleaseInfo& out)
    {
        Ws();
        if (!Eat('{')) return false;
        if (Eat('}')) return false;
        for (;;) {
            std::string key;
            if (!String(&key) || !Colon()) return false;
            if (key == "tag_name" && Peek('"')) {
                if (!String(&out.tag)) return false;
            } else if (key == "assets" && Peek('[')) {
                if (!Assets(assetName, out)) return false;
            } else if (!Skip()) {
                return false;
            }
            if (Eat(',')) continue;
            return Eat('}');
        }
    }

private:
    bool Assets(const std::string& assetName, ReleaseInfo& out)
    {
        Eat('[');
        if (Eat(']')) return true;
        for (;;) {
            Ws();
            if (!Eat('{')) return false;
            std::string name, url, digest;
            unsigned long long size = 0;
            if (!Eat('}')) {
                for (;;) {
                    std::string key;
                    if (!String(&key) || !Colon()) return false;
                    bool ok;
                    if (key == "name" && Peek('"')) ok = String(&name);
                    else if (key == "browser_download_url" && Peek('"')) ok = String(&url);
                    else if (key == "digest" && Peek('"')) ok = String(&digest);
                    else if (key == "size" && (Ws(), p_ < e_ && *p_ >= '0' && *p_ <= '9')) {
                        size = strtoull(p_, nullptr, 10);
                        ok = Skip();
                    } else ok = Skip();
                    if (!ok) return false;
                    if (Eat(',')) continue;
                    if (!Eat('}')) return false;
                    break;
                }
            }
            if (name == assetName && out.assetUrl.empty()) {
                out.assetUrl = url;
                out.size = size;
                if (digest.rfind("sha256:", 0) == 0) {
                    out.sha256 = digest.substr(7);
                    for (char& c : out.sha256)
                        if (c >= 'A' && c <= 'F') c = (char)(c - 'A' + 'a');
                }
            }
            if (Eat(',')) continue;
            return Eat(']');
        }
    }

    void Ws() { while (p_ < e_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\r' || *p_ == '\n')) ++p_; }
    bool Peek(char c) { Ws(); return p_ < e_ && *p_ == c; }
    bool Eat(char c)
    {
        if (!Peek(c)) return false;
        ++p_;
        return true;
    }
    bool Colon() { return Eat(':'); }

    // Strings the updater uses are ASCII; other \u escapes become '?'.
    bool String(std::string* out)
    {
        if (!Eat('"')) return false;
        if (out) out->clear();
        while (p_ < e_ && *p_ != '"') {
            char c = *p_++;
            if (c == '\\') {
                if (p_ >= e_) return false;
                const char esc = *p_++;
                if (esc == 'u') {
                    if (e_ - p_ < 4) return false;
                    p_ += 4;
                    c = '?';
                } else {
                    c = esc == 'n' ? '\n' : esc == 't' ? '\t' : esc == 'r' ? '\r' : esc == 'b' ? '\b' : esc == 'f' ? '\f' : esc;
                }
            }
            if (out) out->push_back(c);
        }
        return Eat('"');
    }

    bool Skip(int depth = 0)
    {
        if (depth > 64) return false;
        Ws();
        if (p_ >= e_) return false;
        if (*p_ == '"') return String(nullptr);
        if (*p_ == '{' || *p_ == '[') {
            const char close = *p_ == '{' ? '}' : ']';
            const bool object = *p_ == '{';
            ++p_;
            if (Eat(close)) return true;
            for (;;) {
                if (object && (!String(nullptr) || !Colon())) return false;
                if (!Skip(depth + 1)) return false;
                if (Eat(',')) continue;
                return Eat(close);
            }
        }
        const char* start = p_;     // number, true, false, null
        while (p_ < e_ && *p_ != ',' && *p_ != '}' && *p_ != ']' && *p_ != ' ' && *p_ != '\r' && *p_ != '\n' && *p_ != '\t') ++p_;
        return p_ > start;
    }

    const char* p_;
    const char* e_;
};

} // namespace

bool ParseRelease(const std::string& json, const std::string& assetName, ReleaseInfo& out)
{
    out = ReleaseInfo();
    return JsonWalker(json).Release(assetName, out) && !out.tag.empty();
}

} // namespace updater
