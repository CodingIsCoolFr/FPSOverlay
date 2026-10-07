#include "sensors/gpu_adapters.h"
#include "platform/win_util.h"

#include <dxgi1_4.h>

#include <cstdio>

namespace {

// Minimal D3DKMT declarations (gdi32.dll exports); avoids pulling in the WDK-style headers.
using KmtHandle = UINT;
struct KmtOpenAdapterFromLuid { LUID AdapterLuid; KmtHandle hAdapter; };
struct KmtQueryAdapterInfo { KmtHandle hAdapter; UINT Type; void* pPrivateDriverData; UINT PrivateDriverDataSize; };
struct KmtAdapterAddress { UINT BusNumber; UINT DeviceNumber; UINT FunctionNumber; };
struct KmtCloseAdapter { KmtHandle hAdapter; };
constexpr UINT kKmtQaiAdapterAddress = 6;

using OpenFromLuidFn = LONG(APIENTRY*)(KmtOpenAdapterFromLuid*);
using QueryInfoFn = LONG(APIENTRY*)(KmtQueryAdapterInfo*);
using CloseFn = LONG(APIENTRY*)(KmtCloseAdapter*);

bool QueryPciAddress(const LUID& luid, UINT& bus, UINT& dev, UINT& fn)
{
    static HMODULE gdi = LoadLibraryExW(L"gdi32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!gdi) return false;
    static auto open = reinterpret_cast<OpenFromLuidFn>(GetProcAddress(gdi, "D3DKMTOpenAdapterFromLuid"));
    static auto query = reinterpret_cast<QueryInfoFn>(GetProcAddress(gdi, "D3DKMTQueryAdapterInfo"));
    static auto close = reinterpret_cast<CloseFn>(GetProcAddress(gdi, "D3DKMTCloseAdapter"));
    if (!open || !query || !close) return false;

    KmtOpenAdapterFromLuid o = {};
    o.AdapterLuid = luid;
    if (open(&o) != 0) return false;
    KmtAdapterAddress addr = {};
    KmtQueryAdapterInfo q = {};
    q.hAdapter = o.hAdapter;
    q.Type = kKmtQaiAdapterAddress;
    q.pPrivateDriverData = &addr;
    q.PrivateDriverDataSize = sizeof(addr);
    const bool ok = query(&q) == 0;
    KmtCloseAdapter c = { o.hAdapter };
    close(&c);
    if (!ok) return false;
    bus = addr.BusNumber;
    dev = addr.DeviceNumber;
    fn = addr.FunctionNumber;
    return true;
}

} // namespace

std::string LuidKey(const LUID& luid)
{
    char buf[48];
    snprintf(buf, sizeof(buf), "luid:%08lx:%08lx", (unsigned long)luid.HighPart, (unsigned long)luid.LowPart);
    return buf;
}

std::vector<AdapterEntry> EnumerateGpuAdapters()
{
    std::vector<AdapterEntry> out;
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))))
        return out;

    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d = {};
        adapter->GetDesc1(&d);
        adapter->Release();
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if (d.VendorId == 0x1414) continue;   // Microsoft Basic Render Driver

        bool dup = false;
        for (const auto& e : out)
            if (e.luid.LowPart == d.AdapterLuid.LowPart && e.luid.HighPart == d.AdapterLuid.HighPart) dup = true;
        if (dup) continue;

        AdapterEntry e;
        e.luid = d.AdapterLuid;
        e.info.name = win::ToUtf8(d.Description);
        e.info.vendorId = d.VendorId;
        e.info.dedicatedBytes = d.DedicatedVideoMemory;
        e.info.integrated = d.DedicatedVideoMemory < (768ull << 20);
        e.hasPci = QueryPciAddress(d.AdapterLuid, e.pciBus, e.pciDevice, e.pciFunction);
        // LUIDs change on every boot, so the saved key is the name plus the PCI slot.
        char slot[32] = "";
        if (e.hasPci) snprintf(slot, sizeof(slot), "|pci:%02x:%02x.%x", e.pciBus, e.pciDevice, e.pciFunction);
        e.info.key = e.info.name + slot;
        out.push_back(std::move(e));
    }
    factory->Release();
    return out;
}

int PickAdapter(const std::vector<AdapterEntry>& list, const std::string& key)
{
    if (list.empty()) return -1;
    if (!key.empty()) {
        for (size_t i = 0; i < list.size(); ++i)
            if (list[i].info.key == key) return (int)i;
        // Card moved to another slot: fall back to the same model name.
        const std::string name = key.substr(0, key.find('|'));
        for (size_t i = 0; i < list.size(); ++i)
            if (list[i].info.name == name) return (int)i;
    }
    int best = 0;
    for (size_t i = 1; i < list.size(); ++i)
        if (list[i].info.dedicatedBytes > list[best].info.dedicatedBytes) best = (int)i;
    return best;
}
