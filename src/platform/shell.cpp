#include "platform/shell.h"
#include "app/log.h"
#include "app/version.h"
#include "platform/win_util.h"

#include <windows.h>
#include <objbase.h>
#include <ole2.h>
#include <exdisp.h>
#include <servprov.h>
#include <shellapi.h>
#include <shldisp.h>
#include <shlobj.h>

namespace shell {
namespace {

template <typename T> void Release(T*& p) { if (p) { p->Release(); p = nullptr; } }

// Raymond Chen's technique: ask the desktop's Explorer to run ShellExecute for us, so the new
// process gets Explorer's (unelevated) token.
bool ExecuteThroughExplorer(const std::wstring& target)
{
    IShellWindows* windows = nullptr;
    IDispatch* desktopDisp = nullptr;
    IServiceProvider* sp = nullptr;
    IShellBrowser* browser = nullptr;
    IShellView* view = nullptr;
    IDispatch* bgDisp = nullptr;
    IShellFolderViewDual* folderView = nullptr;
    IDispatch* appDisp = nullptr;
    IShellDispatch2* shellDisp = nullptr;
    bool ok = false;

    VARIANT empty;
    VariantInit(&empty);
    long hwnd = 0;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&windows))) &&
        windows->FindWindowSW(&empty, &empty, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, &desktopDisp) == S_OK &&
        SUCCEEDED(desktopDisp->QueryInterface(IID_PPV_ARGS(&sp))) &&
        SUCCEEDED(sp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser))) &&
        SUCCEEDED(browser->QueryActiveShellView(&view)) &&
        SUCCEEDED(view->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&bgDisp))) &&
        SUCCEEDED(bgDisp->QueryInterface(IID_PPV_ARGS(&folderView))) &&
        SUCCEEDED(folderView->get_Application(&appDisp)) &&
        SUCCEEDED(appDisp->QueryInterface(IID_PPV_ARGS(&shellDisp)))) {
        BSTR file = SysAllocString(target.c_str());
        VARIANT args, dir, op, show;
        VariantInit(&args);
        VariantInit(&dir);
        VariantInit(&op);
        VariantInit(&show);
        show.vt = VT_I4;
        show.lVal = SW_SHOWNORMAL;
        ok = SUCCEEDED(shellDisp->ShellExecute(file, args, dir, op, show));
        SysFreeString(file);
    }
    Release(shellDisp);
    Release(appDisp);
    Release(folderView);
    Release(bgDisp);
    Release(view);
    Release(browser);
    Release(sp);
    Release(desktopDisp);
    Release(windows);
    return ok;
}

} // namespace

void OpenUnelevated(const std::wstring& target)
{
    if (win::IsElevated() && ExecuteThroughExplorer(target)) return;
    ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void RevealInExplorer(const std::wstring& path)
{
    PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(path.c_str());
    if (pidl) {
        SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
        ILFree(pidl);
        return;
    }
    const size_t slash = path.find_last_of(L'\\');
    OpenUnelevated(slash == std::wstring::npos ? path : path.substr(0, slash));
}

} // namespace shell

// ── Autostart ───────────────────────────────────────────────────────────────

namespace autostart {
namespace {

DWORD RunHidden(const std::wstring& cmdLine)
{
    std::wstring cmd = cmdLine;
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return (DWORD)-1;
    WaitForSingleObject(pi.hProcess, 15000);
    DWORD code = (DWORD)-1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code;
}

std::wstring Schtasks()
{
    wchar_t sys[MAX_PATH] = {};
    GetSystemDirectoryW(sys, MAX_PATH);
    return std::wstring(L"\"") + sys + L"\\schtasks.exe\"";
}

std::wstring XmlEscape(const std::wstring& s)
{
    std::wstring o;
    for (wchar_t c : s) {
        switch (c) {
            case L'&': o += L"&amp;"; break;
            case L'<': o += L"&lt;"; break;
            case L'>': o += L"&gt;"; break;
            case L'"': o += L"&quot;"; break;
            default: o += c; break;
        }
    }
    return o;
}

std::wstring CurrentUser()
{
    wchar_t user[256] = {}, domain[256] = {};
    GetEnvironmentVariableW(L"USERNAME", user, 256);
    GetEnvironmentVariableW(L"USERDOMAIN", domain, 256);
    return domain[0] ? std::wstring(domain) + L"\\" + user : std::wstring(user);
}

} // namespace

bool IsEnabled()
{
    return RunHidden(Schtasks() + L" /Query /TN \"" APP_NAME_W L"\"") == 0;
}

bool Enable(const std::wstring& exePath, std::string& error)
{
    const std::wstring dir = exePath.substr(0, exePath.find_last_of(L'\\'));
    const std::wstring user = XmlEscape(CurrentUser());
    // Defaults that schtasks /Create would get wrong for an overlay: no 3-day time limit,
    // keep running on battery, normal process priority.
    const std::wstring xml =
        L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
        L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
        L"  <RegistrationInfo><Description>Starts " APP_NAME_W L" in the tray when you sign in.</Description></RegistrationInfo>\r\n"
        L"  <Triggers><LogonTrigger><Enabled>true</Enabled><UserId>" + user + L"</UserId><Delay>PT5S</Delay></LogonTrigger></Triggers>\r\n"
        L"  <Principals><Principal id=\"Author\"><UserId>" + user + L"</UserId><LogonType>InteractiveToken</LogonType>"
        L"<RunLevel>HighestAvailable</RunLevel></Principal></Principals>\r\n"
        L"  <Settings>\r\n"
        L"    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\r\n"
        L"    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\r\n"
        L"    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\r\n"
        L"    <AllowHardTerminate>true</AllowHardTerminate>\r\n"
        L"    <StartWhenAvailable>false</StartWhenAvailable>\r\n"
        L"    <RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>\r\n"
        L"    <IdleSettings><StopOnIdleEnd>false</StopOnIdleEnd><RestartOnIdle>false</RestartOnIdle></IdleSettings>\r\n"
        L"    <AllowStartOnDemand>true</AllowStartOnDemand>\r\n"
        L"    <Enabled>true</Enabled>\r\n"
        L"    <Hidden>false</Hidden>\r\n"
        L"    <RunOnlyIfIdle>false</RunOnlyIfIdle>\r\n"
        L"    <WakeToRun>false</WakeToRun>\r\n"
        L"    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>\r\n"
        L"    <Priority>5</Priority>\r\n"
        L"  </Settings>\r\n"
        L"  <Actions Context=\"Author\"><Exec><Command>\"" + XmlEscape(exePath) + L"\"</Command><Arguments>--tray</Arguments>"
        L"<WorkingDirectory>" + XmlEscape(dir) + L"</WorkingDirectory></Exec></Actions>\r\n"
        L"</Task>\r\n";

    wchar_t tmpDir[MAX_PATH + 1] = {};
    GetTempPathW(MAX_PATH, tmpDir);
    const std::wstring xmlPath = std::wstring(tmpDir) + L"FPSOverlay-task.xml";
    HANDLE f = CreateFileW(xmlPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        error = "Could not write the task definition.";
        return false;
    }
    DWORD written = 0;
    const wchar_t bom = 0xFEFF;
    WriteFile(f, &bom, sizeof(bom), &written, nullptr);
    WriteFile(f, xml.data(), (DWORD)(xml.size() * sizeof(wchar_t)), &written, nullptr);
    CloseHandle(f);

    const DWORD code = RunHidden(Schtasks() + L" /Create /F /TN \"" APP_NAME_W L"\" /XML \"" + xmlPath + L"\"");
    DeleteFileW(xmlPath.c_str());
    if (code != 0) {
        error = "Task Scheduler refused the task (code " + std::to_string((long)code) + ").";
        logx::Warn("schtasks /Create failed: %ld", (long)code);
        return false;
    }
    return true;
}

bool Disable(std::string& error)
{
    const DWORD code = RunHidden(Schtasks() + L" /Delete /F /TN \"" APP_NAME_W L"\"");
    if (code != 0 && IsEnabled()) {
        error = "Task Scheduler could not remove the task (code " + std::to_string((long)code) + ").";
        return false;
    }
    return true;
}

} // namespace autostart
