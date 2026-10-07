// FPSOverlay.Sensors.dll — compiled with /clr. Wraps LibreHardwareMonitor (bundled inside
// lhwm-wrapper.dll) behind a flat C API. Unlike the old wrapper, reading a value never triggers
// a hardware update: the caller updates exactly the hardware it needs, once per tick.

#using <System.dll>
#using "lhwm-wrapper.dll"

#include "lhm_bridge.h"

#include <cmath>
#include <cstring>
#include <limits>

using namespace System;
using namespace System::Collections::Generic;
using namespace LibreHardwareMonitor::Hardware;

namespace {

void CopyString(String^ s, char* dst, int cap)
{
    if (!dst || cap <= 0) return;
    dst[0] = '\0';
    if (s == nullptr) return;
    array<Byte>^ bytes = Text::Encoding::UTF8->GetBytes(s);
    int n = bytes->Length < cap - 1 ? bytes->Length : cap - 1;
    // Never cut a UTF-8 sequence in half.
    while (n > 0 && n < bytes->Length && (bytes[n] & 0xC0) == 0x80) --n;
    for (int i = 0; i < n; ++i) dst[i] = (char)bytes[i];
    dst[n] = '\0';
}

ref class State abstract sealed {
public:
    static Computer^ computer = nullptr;
    static List<IHardware^>^ hardware = gcnew List<IHardware^>();
    static List<ISensor^>^ sensors = gcnew List<ISensor^>();
    static List<int>^ sensorHardware = gcnew List<int>();
    static Object^ lock = gcnew Object();

    static void Flatten(IHardware^ hw)
    {
        int index = hardware->Count;
        hardware->Add(hw);
        for each (ISensor^ s in hw->Sensors) {
            sensors->Add(s);
            sensorHardware->Add(index);
        }
        for each (IHardware^ sub in hw->SubHardware) Flatten(sub);
    }

    static void Reset()
    {
        hardware->Clear();
        sensors->Clear();
        sensorHardware->Clear();
    }
};

} // namespace

extern "C" __declspec(dllexport) int __cdecl lhm_version() { return LHM_BRIDGE_VERSION; }

extern "C" __declspec(dllexport) int __cdecl lhm_open(int flags, char* error, int errorLen)
{
    if (error && errorLen > 0) error[0] = '\0';
    try {
        Threading::Monitor::Enter(State::lock);
        try {
            if (State::computer != nullptr) {
                State::computer->Close();
                State::computer = nullptr;
            }
            State::Reset();

            Computer^ c = gcnew Computer();
            c->IsCpuEnabled = (flags & LHM_HW_CPU) != 0;
            c->IsGpuEnabled = (flags & LHM_HW_GPU) != 0;
            c->IsMotherboardEnabled = (flags & LHM_HW_MOTHERBOARD) != 0;
            c->IsMemoryEnabled = (flags & LHM_HW_MEMORY) != 0;
            c->IsControllerEnabled = (flags & LHM_HW_CONTROLLER) != 0;
            c->IsStorageEnabled = false;
            c->IsNetworkEnabled = false;
            c->IsPsuEnabled = false;
            c->IsBatteryEnabled = false;
            c->Open();

            // Some sensors (GPU engines, SuperIO fans) only appear after the first update.
            for each (IHardware^ hw in c->Hardware) {
                hw->Update();
                for each (IHardware^ sub in hw->SubHardware) sub->Update();
            }
            for each (IHardware^ hw in c->Hardware) State::Flatten(hw);
            State::computer = c;
        } finally {
            Threading::Monitor::Exit(State::lock);
        }
        return 1;
    } catch (Exception^ ex) {
        CopyString(ex->GetType()->Name + ": " + ex->Message, error, errorLen);
        return 0;
    }
}

extern "C" __declspec(dllexport) void __cdecl lhm_close()
{
    try {
        Threading::Monitor::Enter(State::lock);
        try {
            if (State::computer != nullptr) State::computer->Close();
            State::computer = nullptr;
            State::Reset();
        } finally {
            Threading::Monitor::Exit(State::lock);
        }
    } catch (Exception^) {
    }
}

extern "C" __declspec(dllexport) int __cdecl lhm_sensor_count()
{
    return State::sensors->Count;
}

extern "C" __declspec(dllexport) int __cdecl lhm_sensor_info(int index, LhmSensorInfo* out)
{
    if (!out || index < 0 || index >= State::sensors->Count) return 0;
    try {
        ISensor^ s = State::sensors[index];
        IHardware^ hw = State::hardware[State::sensorHardware[index]];
        memset(out, 0, sizeof(*out));
        out->hardware = State::sensorHardware[index];
        CopyString(hw->Name, out->hardwareName, sizeof(out->hardwareName));
        CopyString(hw->HardwareType.ToString(), out->hardwareType, sizeof(out->hardwareType));
        CopyString(s->Name, out->name, sizeof(out->name));
        CopyString(s->SensorType.ToString(), out->type, sizeof(out->type));
        CopyString(s->Identifier->ToString(), out->id, sizeof(out->id));
        return 1;
    } catch (Exception^) {
        return 0;
    }
}

extern "C" __declspec(dllexport) int __cdecl lhm_update(int hardware)
{
    if (hardware < 0 || hardware >= State::hardware->Count) return 0;
    try {
        State::hardware[hardware]->Update();
        return 1;
    } catch (Exception^) {
        return 0;
    }
}

extern "C" __declspec(dllexport) float __cdecl lhm_value(int sensor)
{
    if (sensor < 0 || sensor >= State::sensors->Count) return std::numeric_limits<float>::quiet_NaN();
    try {
        Nullable<float> v = State::sensors[sensor]->Value;
        return v.HasValue ? v.Value : std::numeric_limits<float>::quiet_NaN();
    } catch (Exception^) {
        return std::numeric_limits<float>::quiet_NaN();
    }
}
