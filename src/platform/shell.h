// Shell helpers that behave correctly from an elevated process.
#pragma once

#include <string>

namespace shell {

// Opens a URL or file through the desktop shell, so the browser starts with the user's normal
// (non-elevated) rights instead of inheriting administrator rights from this app.
void OpenUnelevated(const std::wstring& target);

// Opens an Explorer window with the given file selected.
void RevealInExplorer(const std::wstring& path);

} // namespace shell

namespace autostart {

// "Start with Windows" through Task Scheduler: an elevated logon task starts the app without a
// UAC prompt, which the Run registry key cannot do for an app that requires administrator rights.
bool IsEnabled();
bool Enable(const std::wstring& exePath, std::string& error);
bool Disable(std::string& error);

} // namespace autostart
