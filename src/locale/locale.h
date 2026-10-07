// Translations. English source strings are the keys; locales/<code>.json maps them to the
// language in use, and anything missing falls back to English.
#pragma once

namespace locale {

// Loads locales/<code>.json from next to the exe (or from the repo when run from build\<Config>).
// "en-US" or an unknown code leaves everything in English.
void Load(const char* langCode);

const char* Current();
bool IsRtl();           // Arabic, Persian, Urdu, Hebrew...

// Translated text, ready to draw: right-to-left text comes back shaped and in visual order.
// The pointer stays valid until the next Load().
const char* T(const char* english);

// printf with a translated format. The result lives in a small rotating buffer: use it at once.
// A translation whose placeholders do not match the English format is ignored (it would read
// the wrong arguments).
const char* TF(const char* englishFmt, ...);

} // namespace locale
