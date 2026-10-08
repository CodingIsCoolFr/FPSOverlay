// Product identity. Everything user-visible about the app's name and version lives here,
// and resource.rc reads it too, so a rename or version bump is a one-file change.
#pragma once

#define APP_NAME            "FPS Overlay"
#define APP_NAME_W          L"FPS Overlay"
// Window classes, the single-instance mutex, the ETW session and the autostart task are named
// after APP_ID_W. A test build (cl /DFPSO_TEST_INSTANCE) gets its own names, so it can run beside
// an installed copy without taking over its FPS capture.
#ifdef FPSO_TEST_INSTANCE
#define APP_ID_W            L"FPSOverlayTest"
#else
#define APP_ID_W            L"FPSOverlay"
#endif

#define APP_VERSION_MAJOR   2
#define APP_VERSION_MINOR   0
#define APP_VERSION_PATCH   8

#define APP_STR2(x) #x
#define APP_STR(x)  APP_STR2(x)
#define APP_VERSION         APP_STR(APP_VERSION_MAJOR) "." APP_STR(APP_VERSION_MINOR) "." APP_STR(APP_VERSION_PATCH)

// GitHub "owner/repo" whose latest release is checked for updates.
// Empty string = the update check is off and no network request is ever made.
#define APP_UPDATE_REPO     "CodingIsCoolFr/FPSOverlay"

// Where the Support on Ko-fi buttons go.
#define APP_SUPPORT_URL_W   L"https://ko-fi.com/codingiscool"
