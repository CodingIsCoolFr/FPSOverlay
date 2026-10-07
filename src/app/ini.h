// Minimal UTF-8 INI document: ordered sections and keys, comments dropped on save.
// Pure C++ (no Win32) so it is unit-tested directly.
#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

class Ini {
public:
    void Parse(std::string_view text);
    std::string Serialize() const;

    bool Has(std::string_view section, std::string_view key) const;
    std::string Get(std::string_view section, std::string_view key, std::string_view def = {}) const;
    int  GetInt(std::string_view section, std::string_view key, int def) const;
    bool GetBool(std::string_view section, std::string_view key, bool def) const;

    void Set(std::string_view section, std::string_view key, std::string_view value);
    void SetInt(std::string_view section, std::string_view key, int value);
    void SetBool(std::string_view section, std::string_view key, bool value);

private:
    struct Section {
        std::string name;
        std::vector<std::pair<std::string, std::string>> kv;
    };
    const std::string* Find(std::string_view section, std::string_view key) const;
    std::vector<Section> sections_;
};
