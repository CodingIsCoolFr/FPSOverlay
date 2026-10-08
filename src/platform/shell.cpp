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
#include <taskschd.h>

#include <cstdio>

#pragma comment(lib, "taskschd.lib")

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

// installer/FPSOverlay.iss removes the task by this name on uninstall. A test build gets its
// own, so trying it out never replaces the real one.
#ifdef FPSO_TEST_INSTANCE
const wchar_t kTaskName[] = APP_NAME_W L" (test)";
#else
const wchar_t kTaskName[] = APP_NAME_W;
#endif

// COM for the calling thread, which is usually a plain worker thread. On a thread that already
// has COM (the UI thread), CoInitializeEx fails harmlessly and COM stays as it was.
struct ComScope {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ComScope() { if (SUCCEEDED(hr)) CoUninitialize(); }
};

// Task Scheduler's root folder, through its own API. The app used to run schtasks.exe hidden
// with an XML file in %TEMP%, which is also how malware installs itself, and antivirus engines
// treated it that way.
ITaskFolder* RootFolder(HRESULT& hr)
{
    ITaskService* svc = nullptr;
    ITaskFolder* root = nullptr;
    hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&svc));
    if (SUCCEEDED(hr)) {
        VARIANT empty;
        VariantInit(&empty);
        hr = svc->Connect(empty, empty, empty, empty);
        if (SUCCEEDED(hr)) {
            BSTR path = SysAllocString(L"\\");
            hr = svc->GetFolder(path, &root);
            SysFreeString(path);
        }
        svc->Release();
    }
    return SUCCEEDED(hr) ? root : nullptr;
}

std::string HrText(HRESULT hr)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "0x%08lX", (unsigned long)hr);
    return buf;
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
    ComScope com;
    HRESULT hr = S_OK;
    ITaskFolder* root = RootFolder(hr);
    if (!root) return false;
    BSTR name = SysAllocString(kTaskName);
    IRegisteredTask* task = nullptr;
    const bool found = SUCCEEDED(root->GetTask(name, &task));
    SysFreeString(name);
    if (task) task->Release();
    root->Release();
    return found;
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

    ComScope com;
    HRESULT hr = S_OK;
    if (ITaskFolder* root = RootFolder(hr)) {
        BSTR name = SysAllocString(kTaskName);
        BSTR def = SysAllocString(xml.c_str());
        VARIANT empty;
        VariantInit(&empty);
        IRegisteredTask* task = nullptr;
        hr = root->RegisterTask(name, def, TASK_CREATE_OR_UPDATE, empty, empty, TASK_LOGON_INTERACTIVE_TOKEN, empty, &task);
        SysFreeString(def);
        SysFreeString(name);
        if (task) task->Release();
        root->Release();
    }
    if (FAILED(hr)) {
        error = "Task Scheduler refused the task (code " + HrText(hr) + ").";
        logx::Warn("RegisterTask failed: %s", HrText(hr).c_str());
        return false;
    }
    return true;
}

bool Disable(std::string& error)
{
    ComScope com;
    HRESULT hr = S_OK;
    if (ITaskFolder* root = RootFolder(hr)) {
        BSTR name = SysAllocString(kTaskName);
        hr = root->DeleteTask(name, 0);
        SysFreeString(name);
        root->Release();
    }
    if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) && IsEnabled()) {
        error = "Task Scheduler could not remove the task (code " + HrText(hr) + ").";
        return false;
    }
    return true;
}

} // namespace autostart
