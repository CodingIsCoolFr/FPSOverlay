# Notices

FPS Overlay is free software: you can redistribute it and/or modify it under the terms of the
GNU General Public License, version 3, as published by the Free Software Foundation
(see LICENSE.txt). It comes with no warranty.

## Origin

This program began as a modified version of an earlier GPL-3.0 program of the same name,
Copyright (c) 2026 Anees. It was rebuilt from the ground up in October 2026: the source code
and the translations in `locales/` were written anew. This notice is kept because the program
started as that modified version.

## Third-party components

- Dear ImGui — Copyright (c) 2014-2026 Omar Cornut. MIT License (libs/imgui/LICENSE.txt).
- RTLScript — Copyright (c) 2019 iAmir and contributors. MIT License (libs/RTLScript/LICENSE).
  Modified here: three functions that returned pointers to local variables were fixed.
- LibreHardwareMonitor 0.9.6 and its dependencies, bundled in lhwm-wrapper.dll —
  Mozilla Public License 2.0. Source: https://github.com/LibreHardwareMonitor/LibreHardwareMonitor
- PawnIO driver installer (libs/lhwm/PawnIO_setup.exe) — see https://pawnio.eu/ for its license.
- PawnIO.Modules 0.1.6: the signed IntelMSR.bin and AMDFamily17.bin modules (libs/pawnio, embedded
  in FPSOverlay.exe) — GNU Lesser General Public License 2.1 or later (LICENSE-PawnIO-modules.txt).
  Source: https://github.com/namazso/PawnIO.Modules/releases/tag/0.1.6
