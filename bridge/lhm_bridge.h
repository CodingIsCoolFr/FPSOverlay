// C interface of FPSOverlay.Sensors.dll, the C++/CLI bridge to LibreHardwareMonitor.
// The main exe stays fully native and only loads this DLL (and with it the .NET runtime)
// on the sensor thread, when a reading actually needs LibreHardwareMonitor.
#pragma once

#define LHM_HW_CPU          0x01
#define LHM_HW_GPU          0x02
#define LHM_HW_MOTHERBOARD  0x04
#define LHM_HW_MEMORY       0x08
#define LHM_HW_CONTROLLER   0x10

#define LHM_BRIDGE_VERSION  1

struct LhmSensorInfo {
    int  hardware;              // index for lhm_update()
    char hardwareName[128];
    char hardwareType[32];      // "Cpu", "GpuNvidia", "GpuAmd", "GpuIntel", "SuperIO", ...
    char name[96];              // "CPU Package"
    char type[24];              // "Temperature", "Power", "Fan", "Clock", "Load", "SmallData", ...
    char id[160];               // "/intelcpu/0/temperature/26"
};

#ifdef __cplusplus
extern "C" {
#endif

typedef int   (__cdecl *lhm_version_fn)(void);
typedef int   (__cdecl *lhm_open_fn)(int hardwareFlags, char* error, int errorLen);
typedef void  (__cdecl *lhm_close_fn)(void);
typedef int   (__cdecl *lhm_sensor_count_fn)(void);
typedef int   (__cdecl *lhm_sensor_info_fn)(int index, struct LhmSensorInfo* out);
typedef int   (__cdecl *lhm_update_fn)(int hardware);
typedef float (__cdecl *lhm_value_fn)(int sensor);     // NaN when the sensor has no value

#ifdef __cplusplus
}
#endif
