// D3D12 Present hook scaffold. Intentionally empty for now — see header.
//
// A working implementation needs:
//   1. D3D12CreateDevice + a dummy command queue + dummy IDXGISwapChain3.
//   2. Hook vtable slot 8  (Present)            on the swapchain.
//   3. Hook vtable slot 54 (ExecuteCommandLists) on the queue to capture the
//      real engine queue for any later RTV work.
//
// For pure head-tracking we only need (1) + (2). Patches welcome.

#include "D3D12Hook.hpp"
