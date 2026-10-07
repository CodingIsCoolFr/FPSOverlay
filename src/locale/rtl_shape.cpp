#include "locale/rtl_shape.h"

#include <RTLScript/RTLScript.hpp>

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

// Dear ImGui draws glyphs left to right with no shaping. For Arabic, Persian, Urdu and Hebrew
// this file turns a logical-order string into what ImGui must draw:
//   1. Arabic letters get their joined presentation forms (RTLScript's glyph tables), computed
//      in logical order so each letter sees its real neighbours.
//   2. The line is reordered for a right-to-left paragraph: the order of runs is reversed,
//      right-to-left runs are reversed character by character (with mirrored brackets), and
//      left-to-right runs such as "Ctrl", "PawnIO" or "1% low" keep their own order.
// (RTLScript's own reordering appended every left-to-right word to one end of the line.)

namespace locale {
namespace {

bool IsRtlCodepoint(uint32_t cp)
{
    return (cp >= 0x0590 && cp <= 0x05FF) || (cp >= 0x0600 && cp <= 0x06FF) ||
           (cp >= 0x0750 && cp <= 0x077F) || (cp >= 0x08A0 && cp <= 0x08FF) ||
           (cp >= 0xFB1D && cp <= 0xFB4F) || (cp >= 0xFB50 && cp <= 0xFDFF) || (cp >= 0xFE70 && cp <= 0xFEFF);
}

// Arabic-Indic digits are written left to right inside right-to-left text, like European digits.
bool IsArabicDigit(wchar_t c) { return (c >= 0x0660 && c <= 0x0669) || (c >= 0x06F0 && c <= 0x06F9); }

enum class Dir : unsigned char { R, L, N };

Dir Classify(wchar_t c)
{
    if (IsArabicDigit(c)) return Dir::L;
    if (IsRtlCodepoint((uint32_t)c)) return Dir::R;
    if (iswalnum(c)) return Dir::L;
    return Dir::N;
}

wchar_t Mirror(wchar_t c)
{
    switch (c) {
        case L'(': return L')';
        case L')': return L'(';
        case L'[': return L']';
        case L']': return L'[';
        case L'{': return L'}';
        case L'}': return L'{';
        case L'<': return L'>';
        case L'>': return L'<';
        case 0x00AB: return 0x00BB;     // « »
        case 0x00BB: return 0x00AB;
        default: return c;
    }
}

std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)std::max(n, 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)std::max(n, 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

// Presentation forms in logical order.
std::wstring JoinArabic(const std::wstring& t)
{
    std::wstring out = t;
    auto neighbour = [&](size_t i, int step) -> wchar_t {
        long j = (long)i + step;
        while (j >= 0 && j < (long)t.size() && RTLScript::CheckForAnyArabicDiacritic(t[(size_t)j])) j += step;
        if (j < 0 || j >= (long)t.size()) return RTLScript::NULL_CHAR;
        return RTLScript::IsArabicLetter(t[(size_t)j]) ? t[(size_t)j] : RTLScript::NULL_CHAR;
    };
    for (size_t i = 0; i < t.size(); ++i) {
        if (!RTLScript::IsArabicLetter(t[i])) continue;
        out[i] = RTLScript::GetArCharGlyph(t[i], neighbour(i, -1), neighbour(i, +1));
    }
    // Zero-width marks (Persian ZWNJ in "می‌شود", bidi controls) have done their job by breaking
    // the joins above; ImGui has no glyph for them and would draw a box.
    out.erase(std::remove_if(out.begin(), out.end(), [](wchar_t c) {
                  return (c >= 0x200B && c <= 0x200F) || (c >= 0x202A && c <= 0x202E) || (c >= 0x2066 && c <= 0x2069);
              }),
              out.end());
    return out;
}

// Visual order for one line of a right-to-left paragraph.
std::wstring ReorderLine(const std::wstring& line)
{
    const size_t n = line.size();
    std::vector<Dir> dir(n);
    for (size_t i = 0; i < n; ++i) dir[i] = Classify(line[i]);

    // Neutrals between two left-to-right characters join that run ("Ctrl + right", "1% low");
    // every other neutral follows the paragraph direction.
    for (size_t i = 0; i < n; ++i) {
        if (dir[i] != Dir::N) continue;
        size_t j = i;
        while (j < n && dir[j] == Dir::N) ++j;
        const bool leftL = i > 0 && dir[i - 1] == Dir::L;
        const bool rightL = j < n && dir[j] == Dir::L;
        const Dir d = (leftL && rightL) ? Dir::L : Dir::R;
        for (size_t k = i; k < j; ++k) dir[k] = d;
        i = j - 1;
    }

    // Runs from the end of the line to the start; R runs reversed, L runs kept.
    std::wstring out;
    out.reserve(n);
    size_t end = n;
    while (end > 0) {
        size_t start = end - 1;
        while (start > 0 && dir[start - 1] == dir[end - 1]) --start;
        if (dir[end - 1] == Dir::L) {
            out.append(line, start, end - start);
        } else {
            for (size_t k = end; k > start; --k) out.push_back(Mirror(line[k - 1]));
        }
        end = start;
    }
    return out;
}

bool HasRtl(const std::wstring& w)
{
    for (wchar_t c : w)
        if (IsRtlCodepoint((uint32_t)c)) return true;
    return false;
}

} // namespace

std::string ShapeRtlForImGui(const char* utf8)
{
    if (!utf8 || !utf8[0]) return {};

    // Keep ImGui "##id" suffixes out of the shaping.
    const char* idSep = strstr(utf8, "##");
    const std::string visible = idSep ? std::string(utf8, idSep) : std::string(utf8);
    const std::string idSuffix = idSep ? std::string(idSep) : std::string();

    const std::wstring in = Utf8ToWide(visible);
    if (!HasRtl(in)) return std::string(utf8);

    const std::wstring joined = JoinArabic(in);
    std::wstring out;
    size_t start = 0;
    for (;;) {
        const size_t nl = joined.find(L'\n', start);
        out += ReorderLine(joined.substr(start, nl == std::wstring::npos ? std::wstring::npos : nl - start));
        if (nl == std::wstring::npos) break;
        out.push_back(L'\n');
        start = nl + 1;
    }
    return WideToUtf8(out) + idSuffix;
}

} // namespace locale
