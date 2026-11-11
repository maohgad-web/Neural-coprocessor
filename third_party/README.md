# third_party

These are the only third-party dependencies this PR adds to the repository. The
repository's `ext/` directory is deliberately git-ignored (CI fetches ReShade and
NGX headers there at pinned SHAs), so anything the build needs beyond those two
must live here.

## safetyhook/ — SafetyHook + amalgamated Zydis

- `safetyhook.cpp`, `safetyhook.hpp`: [SafetyHook](https://github.com/cursey/safetyhook), MIT.
- `Zydis.c`, `Zydis.h`: amalgamated Zydis decoder bundled with SafetyHook, MIT.

Used only by `src/ngx_hook.cpp` and `src/ffx_hook.cpp` to install the
trampolines on the provider's exported functions. Both files are compiled with
`/std:c++latest` and `ZYDIS_STATIC_BUILD`; `Zydis.c` is compiled as C, which is
why `CMakeLists.txt` enables the C language.

## xess_fg/ — Intel XeSS-FG / XeLL headers

- `xefg_swapchain.h`, `xefg_swapchain_d3d12.h`, `xefg_swapchain_debug.h`,
  `xell.h`, `xell_d3d12.h`: Intel XeSS SDK headers, MIT.

Used by `src/xess_fg.cpp` and `src/display_ref.cpp` (the provider adapter that
gives the T5 author-chain handover its `mgpu_xefg_*` entry points). The runtime
DLLs (`libxess_fg.dll`, `libxell.dll`) are not vendored; they are loaded from
`mgpu\` beside the add-on and from the game's executable directory respectively.
