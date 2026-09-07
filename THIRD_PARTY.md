# Third-Party Components

## ReShade add-on API headers

`ext/reshade/` is **not committed to this repository**. It is fetched at
build time by `.github/workflows/build.yml` from the public repository
`crosire/reshade` at the pinned SHA recorded in `VENDOR_LOCK.md`
(Milestone 0: tag `v6.8.0`, SHA
`18deaa52de0c425a78b329e9cb3c497281cd00ec`, add-on API version 20).

The files under that repository's `include/` directory (`reshade.hpp`,
`reshade_api*.hpp`, `reshade_events.hpp`, and the remaining public
add-on headers) are dual-licensed under the **BSD 3-Clause License** or
the **MIT License**. This project **elects the BSD 3-Clause License**,
consistent with ReShade proper.

### BSD 3-Clause (ReShade headers)

```
Copyright (c) 2014-2026 Crosire and contributors.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
```

## Dear ImGui

`imgui.h` and `imconfig.h` are **not committed to this repository**. They are
fetched into the build tree at CMake configure time from `ocornut/imgui` at
commit `3912b3d9a9c1b3f17431aebafd86d2f40ee6e59c` (version **1.92.5**,
`IMGUI_VERSION_NUM` 19250) — the commit ReShade pins at `deps/imgui` for the
SHA recorded in `VENDOR_LOCK.md`.

The version is not a preference. `reshade_overlay.hpp` refuses to compile
against anything but 19250: the ImGui function table is a version-numbered
struct of raw pointers filled in by the ReShade runtime, so a near-miss is a
silent ABI mismatch and the header errors rather than allowing it.

**No ImGui source is compiled and no ImGui library is linked.** Only the
declarations are used; every ImGui call in this add-on resolves through the
function table ReShade supplies at `register_addon` time. The headers do
contain inline code, so the notice below is reproduced with the binary.

### MIT License (Dear ImGui)

```
The MIT License (MIT)

Copyright (c) 2014-2025 Omar Cornut

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## NVIDIA NGX headers

`ext/ngx/` is **not committed to this repository**. It is fetched at build time
by `.github/workflows/build.yml` from the public repository `NVIDIA/DLSS` at
the pinned SHA recorded in `VENDOR_LOCK.md`. Only the `include/` directory is
extracted; no NVIDIA binary is fetched, built, committed or redistributed.

`VENDOR_LOCK.md` previously pointed at this file for these terms and this file
did not carry them. That is corrected here.

The headers are governed by the licence published in that repository. Consult
`LICENSE.txt` at the pinned SHA for the operative text; it is not reproduced
here because the terms are NVIDIA's to state and a stale copy would be worse
than a reference.

**No NVIDIA runtime component is distributed with this project.** `_nvngx.dll`,
`nvngx_dlssnr.dll` and the DLSS-NR weights come from the user's own machine and are located at run time.
. Adding any of them to a release package would be redistribution of NVIDIA binaries and must not be done.

## Provenance

This project was written against the ReShade and NGX headers rather than
derived from any existing implementation. Because much of the code was written
with AI assistance — see `ACKNOWLEDGEMENTS.md` — that claim is checked
mechanically rather than asserted.

`tools/provenance_scan.py` compares this repository's sources against the
projects it could plausibly have been derived from, by tokenising both sides,
discarding comments and string literals, and looking for shared runs of
identical code. Run 2026-09-05 against `Dagherbou/OptiScaler_DLSSNR`,
`jlrouzies-fr/DLSS5-Feeder` and `clshortfuse/renodx` — 1,333 upstream source
files:

| file | tokens | matching windows | runs of 10+ |
|---|---|---|---|
| `adapter.cpp` | 3,054 | 0 (0.00%) | 0 |
| `dllmain.cpp` | 2,591 | 7 (0.27%) | 0 |
| `gpu1_context.cpp` | 50,574 | 88 (0.17%) | 0 |
| `gpu1_context.hpp` | 367 | 0 | 0 |
| `worker.cpp` | 3,877 | 1 (0.03%) | 0 |

**No run of ten or more consecutive matching windows anywhere.** Copied code
produces long runs; there are none. Every surviving match is one of two things:
filling a `D3D12_RESOURCE_BARRIER` or `D3D12_RESOURCE_DESC`, where the field
names and order are fixed by Microsoft's headers and there is no other way to
write it; or an NGX function signature, which is NVIDIA's own declaration that
any caller necessarily reproduces.

**What this does and does not establish.** It is evidence that no block of code
was copied from those three projects. It does not prove originality in general:
it covers only the repositories listed, it cannot detect heavily reworded
copying, and it is only as current as the last run. It is published so the
check can be repeated by anyone rather than taken on trust.

## LumeniteFX

LumeniteFX is **not contained in, referenced by, or distributed with this
repository**. It is a runtime asset: the stock, unmodified pack is
installed by hand into the target game's ReShade shader path on the rig,
exactly as any ReShade user would install a technique pack, and it
executes inside the second effect runtime at run time. ReShade compiles
`.fx` shaders at runtime, so LumeniteFX is never a build input and CI
never sees it.

The shader P0's result depends on is `lumenite_QuantMotion.fx`
(version 2026.06.16). `VENDOR_LOCK.md` records the shader provenance and
explains how to identify the right pack — LumeniteFX dates each shader
independently and publishes no single pack version number, so there is no
release string to pin here.

~~`assets/gpu1.ini` in this repository names the LumeniteFX technique
`Lumenite_QuantMotion` as a configuration string.~~ **No longer true as of
2026-09-05, and the reason matters.** The shipped `assets/gpu1.ini` is now empty
of techniques, and `build.yml` **fails the build** if it names any — that preset
had been copied out of a working game folder and shipped a motion-flow debug view
over the neural output on every fresh install.

So this repository contains **no reference to LumeniteFX at all** in anything it
ships. The technique name appears only in `VENDOR_LOCK.md` and the milestone
record, describing the configuration under which P0 was verified. Even then it
was a reference to an externally installed asset rather than a copy of it or a
derivative of it.

Nothing in this repository is a fork of LumeniteFX or of any other
third-party repository.
