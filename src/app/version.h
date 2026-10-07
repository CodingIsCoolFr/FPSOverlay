// Product identity. Everything user-visible about the app's name and version lives here,
// and resource.rc reads it too, so a rename or version bump is a one-file change.
#pragma once

#define APP_NAME            "FPS Overlay"
#define APP_NAME_W          L"FPS Overlay"
#define APP_ID_W            L"FPSOverlay"          // window classes, mutex, ETW session, task name

#define APP_VERSION_MAJOR   2
#define APP_VERSION_MINOR   0
#define APP_VERSION_PATCH   2

#define APP_STR2(x) #x
#define APP_STR(x)  APP_STR2(x)
#define APP_VERSION         APP_STR(APP_VERSION_MAJOR) "." APP_STR(APP_VERSION_MINOR) "." APP_STR(APP_VERSION_PATCH)

// GitHub "owner/repo" whose latest release is checked for updates.
// Empty string = the update check is off and no network request is ever made.
#define APP_UPDATE_REPO     "CodingIsCoolFr/FPSOverlay"
