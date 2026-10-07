// Command-line tools for development and support, built into the release exe:
//   --shots <dir>                  render every settings page and HUD layout to PNG (sample data)
//   --demo-frames <dir>            render an animated HUD sequence to transparent PNGs (sample data)
//   --probe [seconds]              print live sensor readings and every temperature sensor
//   --selftest                     measure test windows with known frame rates (needs admin)
//   --render-test <api> <fps> <s>  test window: api = d3d11 | opengl
#pragma once

#include <string>

namespace devtools {

constexpr int kNotHandled = -100000;

// Returns kNotHandled when no developer flag is present; otherwise the process exit code.
int Run(int argc, wchar_t** argv, const std::wstring& configPath);

} // namespace devtools
