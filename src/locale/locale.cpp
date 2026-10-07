#include "locale/locale.h"
#include "locale/rtl_shape.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

namespace locale {
namespace {

std::string g_lang = "en-US";
bool g_rtl = false;
std::unordered_map<std::string, std::string> g_strings;   // English -> translation (logical order)
std::unordered_map<std::string, std::string> g_shaped;    // English -> right-to-left text ready to draw

bool LangIsRtl(const std::string& code)
{
    static const char* kRtl[] = { "ar", "fa", "he", "iw", "ur", "ps", "sd", "yi" };
    for (const char* tag : kRtl) {
        const size_t n = strlen(tag);
        if (_strnicmp(code.c_str(), tag, n) == 0 && (code.size() == n || code[n] == '-' || code[n] == '_'))
            return true;
    }
    return false;
}

// ── Minimal JSON reader ─────────────────────────────────────────────────────
// Enough for the locale files: string pairs inside "translations" (at any depth) are collected,
// everything else is parsed and skipped. Malformed input stops the parse; what was read so far
// is kept and the rest falls back to English.
class JsonReader {
public:
    JsonReader(const char* p, const char* end, std::unordered_map<std::string, std::string>& out)
        : p_(p), end_(end), out_(out) {}

    bool Document() { Ws(); return Value(false) && (Ws(), p_ == end_); }

private:
    void Ws() { while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\r' || *p_ == '\n')) ++p_; }

    static void Append(std::string& s, unsigned cp)
    {
        if (cp < 0x80) s.push_back((char)cp);
        else if (cp < 0x800) { s.push_back((char)(0xC0 | (cp >> 6))); s.push_back((char)(0x80 | (cp & 0x3F))); }
        else if (cp < 0x10000) {
            s.push_back((char)(0xE0 | (cp >> 12)));
            s.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back((char)(0x80 | (cp & 0x3F)));
        } else {
            s.push_back((char)(0xF0 | (cp >> 18)));
            s.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
            s.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back((char)(0x80 | (cp & 0x3F)));
        }
    }

    bool Hex4(unsigned& v)
    {
        if (end_ - p_ < 4) return false;
        v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = *p_++;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    bool String(std::string& out)
    {
        out.clear();
        if (p_ >= end_ || *p_ != '"') return false;
        ++p_;
        while (p_ < end_ && *p_ != '"') {
            if (*p_ != '\\') { out.push_back(*p_++); continue; }
            if (++p_ >= end_) return false;
            const char e = *p_++;
            switch (e) {
                case '"': case '\\': case '/': out.push_back(e); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    unsigned cp = 0;
                    if (!Hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF && end_ - p_ >= 6 && p_[0] == '\\' && p_[1] == 'u') {
                        p_ += 2;
                        unsigned lo = 0;
                        if (!Hex4(lo)) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    Append(out, cp);
                    break;
                }
                default: return false;
            }
        }
        if (p_ >= end_) return false;
        ++p_;
        return true;
    }

    bool Value(bool collect)
    {
        Ws();
        if (p_ >= end_) return false;
        if (*p_ == '"') { std::string s; return String(s); }
        if (*p_ == '{') return Object(collect);
        if (*p_ == '[') {
            ++p_;
            Ws();
            if (p_ < end_ && *p_ == ']') { ++p_; return true; }
            for (;;) {
                if (!Value(false)) return false;
                Ws();
                if (p_ < end_ && *p_ == ',') { ++p_; continue; }
                if (p_ < end_ && *p_ == ']') { ++p_; return true; }
                return false;
            }
        }
        // number, true, false, null
        const char* start = p_;
        while (p_ < end_ && *p_ != ',' && *p_ != '}' && *p_ != ']' && *p_ != ' ' && *p_ != '\r' && *p_ != '\n' && *p_ != '\t') ++p_;
        return p_ > start;
    }

    bool Object(bool collect)
    {
        ++p_;   // '{'
        Ws();
        if (p_ < end_ && *p_ == '}') { ++p_; return true; }
        std::string key, value;
        for (;;) {
            Ws();
            if (!String(key)) return false;
            Ws();
            if (p_ >= end_ || *p_ != ':') return false;
            ++p_;
            Ws();
            if (p_ < end_ && *p_ == '"') {
                if (!String(value)) return false;
                if (collect && !value.empty()) out_[key] = value;
            } else if (!Value(collect || key == "translations")) {
                return false;
            }
            Ws();
            if (p_ < end_ && *p_ == ',') { ++p_; continue; }
            if (p_ < end_ && *p_ == '}') { ++p_; return true; }
            return false;
        }
    }

    const char* p_;
    const char* end_;
    std::unordered_map<std::string, std::string>& out_;
};

bool LoadFile(const std::wstring& path)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size = {};
    GetFileSizeEx(h, &size);
    std::string text;
    if (size.QuadPart > 0 && size.QuadPart < 8 * 1024 * 1024) {
        text.resize((size_t)size.QuadPart);
        DWORD read = 0;
        if (!ReadFile(h, text.data(), (DWORD)text.size(), &read, nullptr)) read = 0;
        text.resize(read);
    }
    CloseHandle(h);
    const char* p = text.data();
    const char* end = p + text.size();
    if (text.size() >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;
    JsonReader(p, end, g_strings).Document();
    return !g_strings.empty();
}

std::wstring ExeDir()
{
    wchar_t buf[1024] = {};
    GetModuleFileNameW(nullptr, buf, 1024);
    std::wstring s(buf);
    return s.substr(0, s.find_last_of(L'\\') + 1);
}

std::wstring Widen(const std::string& s)
{
    std::wstring w;
    for (char c : s) w.push_back((wchar_t)(unsigned char)c);   // language codes are ASCII
    return w;
}

// Conversion letters of a printf format: "%.0f of %s" -> "fs".
std::string FormatSignature(const char* f)
{
    std::string sig;
    for (const char* p = f; p && *p; ++p) {
        if (*p != '%') continue;
        ++p;
        if (*p == '%') continue;
        while (*p && strchr("-+ #0123456789.*hlLzjt", *p)) ++p;
        if (!*p) break;
        sig.push_back(*p);
    }
    return sig;
}

const char* Lookup(const char* english)
{
    if (!english) return "";
    const auto it = g_strings.find(english);
    return it == g_strings.end() ? english : it->second.c_str();
}

} // namespace

void Load(const char* langCode)
{
    g_strings.clear();
    g_shaped.clear();
    g_lang = (langCode && langCode[0]) ? langCode : "en-US";
    if (_stricmp(g_lang.c_str(), "en") == 0) g_lang = "en-US";
    g_rtl = LangIsRtl(g_lang);
    if (g_lang == "en-US") return;

    const std::wstring file = L"locales\\" + Widen(g_lang) + L".json";
    if (!LoadFile(ExeDir() + file))
        LoadFile(ExeDir() + L"..\\..\\" + file);    // running from build\<Config>\ inside the repo
}

const char* Current() { return g_lang.c_str(); }
bool IsRtl() { return g_rtl; }

const char* T(const char* english)
{
    const char* text = Lookup(english);
    if (!g_rtl || !english) return text;
    auto it = g_shaped.find(english);
    if (it == g_shaped.end()) it = g_shaped.emplace(english, ShapeRtlForImGui(text)).first;
    return it->second.c_str();
}

const char* TF(const char* englishFmt, ...)
{
    static char bufs[8][1024];
    static std::string shaped[8];
    static int next = 0;
    const int slot = next++ & 7;
    char* buf = bufs[slot];
    const char* fmt = Lookup(englishFmt);
    if (fmt != englishFmt && FormatSignature(fmt) != FormatSignature(englishFmt)) fmt = englishFmt;
    va_list ap;
    va_start(ap, englishFmt);
    vsnprintf(buf, sizeof(bufs[0]), fmt, ap);
    va_end(ap);
    if (!g_rtl) return buf;
    shaped[slot] = ShapeRtlForImGui(buf);
    return shaped[slot].c_str();
}

} // namespace locale
