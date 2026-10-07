// Hardware GPU list from DXGI, with PCI addresses so other APIs (NVML, LHM) can be matched
// to the same physical card.
#pragma once

#include "sensors/snapshot.h"

#include <windows.h>

#include <vector>

struct AdapterEntry {
    GpuAdapter info;
    LUID luid = {};
    bool hasPci = false;
    UINT pciBus = 0, pciDevice = 0, pciFunction = 0;
};

std::vector<AdapterEntry> EnumerateGpuAdapters();

std::string LuidKey(const LUID& luid);   // "luid:hhhhhhhh:llllllll" (matches PDH instance names)

// Index of the adapter matching `key`, or the best default (most dedicated memory) when the key
// is empty or not found. -1 if the list is empty.
int PickAdapter(const std::vector<AdapterEntry>& list, const std::string& key);
