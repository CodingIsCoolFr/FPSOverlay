// FPS Overlay — entry point. Everything else lives in App.

#include "app/app.h"

#include <windows.h>
#include <shellapi.h>

#include <memory>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    auto app = std::make_unique<App>();
    const int code = app->Run(instance, argc, argv);
    app.reset();
    LocalFree(argv);
    return code;
}
