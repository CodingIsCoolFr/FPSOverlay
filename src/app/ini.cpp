#include "app/ini.h"

#include <cstdlib>

namespace {

std::string_view Trim(std::string_view s)
{
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

bool EqualsNoCase(std::string_view a, std::string_view b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

} // namespace

void Ini::Parse(std::string_view text)
{
    sections_.clear();
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF)
        text.remove_prefix(3);

    Section* cur = nullptr;
    while (!text.empty()) {
        const size_t nl = text.find('\n');
        std::string_view line = Trim(text.substr(0, nl));
        text = (nl == std::string_view::npos) ? std::string_view() : text.substr(nl + 1);

        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line.front() == '[') {
            const size_t close = line.find(']');
            if (close == std::string_view::npos) continue;
            const std::string_view name = Trim(line.substr(1, close - 1));
            cur = nullptr;
            for (auto& s : sections_)
                if (EqualsNoCase(s.name, name)) cur = &s;
            if (!cur) {
                sections_.push_back({ std::string(name), {} });
                cur = &sections_.back();
            }
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string_view::npos || !cur) continue;
        const std::string_view key = Trim(line.substr(0, eq));
        const std::string_view val = Trim(line.substr(eq + 1));
        if (key.empty()) continue;
        bool replaced = false;
        for (auto& kv : cur->kv) {
            if (EqualsNoCase(kv.first, key)) { kv.second = std::string(val); replaced = true; break; }
        }
        if (!replaced) cur->kv.emplace_back(std::string(key), std::string(val));
    }
}

std::string Ini::Serialize() const
{
    std::string out;
    for (size_t i = 0; i < sections_.size(); ++i) {
        if (i) out += "\r\n";
        out += "[" + sections_[i].name + "]\r\n";
        for (const auto& kv : sections_[i].kv)
            out += kv.first + "=" + kv.second + "\r\n";
    }
    return out;
}

const std::string* Ini::Find(std::string_view section, std::string_view key) const
{
    for (const auto& s : sections_) {
        if (!EqualsNoCase(s.name, section)) continue;
        for (const auto& kv : s.kv)
            if (EqualsNoCase(kv.first, key)) return &kv.second;
    }
    return nullptr;
}

bool Ini::Has(std::string_view section, std::string_view key) const { return Find(section, key) != nullptr; }

std::string Ini::Get(std::string_view section, std::string_view key, std::string_view def) const
{
    const std::string* v = Find(section, key);
    return v ? *v : std::string(def);
}

int Ini::GetInt(std::string_view section, std::string_view key, int def) const
{
    const std::string* v = Find(section, key);
    if (!v || v->empty()) return def;
    char* end = nullptr;
    const long n = strtol(v->c_str(), &end, 10);
    return (end && end != v->c_str()) ? (int)n : def;
}

bool Ini::GetBool(std::string_view section, std::string_view key, bool def) const
{
    const std::string* v = Find(section, key);
    if (!v || v->empty()) return def;
    if (EqualsNoCase(*v, "1") || EqualsNoCase(*v, "true") || EqualsNoCase(*v, "yes") || EqualsNoCase(*v, "on"))
        return true;
    if (EqualsNoCase(*v, "0") || EqualsNoCase(*v, "false") || EqualsNoCase(*v, "no") || EqualsNoCase(*v, "off"))
        return false;
    return def;
}

void Ini::Set(std::string_view section, std::string_view key, std::string_view value)
{
    Section* sec = nullptr;
    for (auto& s : sections_)
        if (EqualsNoCase(s.name, section)) sec = &s;
    if (!sec) {
        sections_.push_back({ std::string(section), {} });
        sec = &sections_.back();
    }
    // Values are single-line by construction; strip anything that would break the format.
    std::string v(value);
    for (char& c : v)
        if (c == '\r' || c == '\n') c = ' ';
    for (auto& kv : sec->kv) {
        if (EqualsNoCase(kv.first, key)) { kv.second = std::move(v); return; }
    }
    sec->kv.emplace_back(std::string(key), std::move(v));
}

void Ini::SetInt(std::string_view section, std::string_view key, int value)
{
    Set(section, key, std::to_string(value));
}

void Ini::SetBool(std::string_view section, std::string_view key, bool value)
{
    Set(section, key, value ? "1" : "0");
}
