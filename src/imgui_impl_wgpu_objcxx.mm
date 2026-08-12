// imgui_impl_wgpu.cpp pulls in Cocoa/Metal headers when built for Dawn on
// macOS, so it must be compiled as Objective-C++. Wrapping it in a .mm picks
// the right language by extension, which survives CMake generator quirks.
#include "imgui_impl_wgpu.cpp"
