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

`assets/gpu1.ini` in this repository names the LumeniteFX technique
`Lumenite_QuantMotion` as a configuration string. That is a reference to
an externally installed asset, not a copy of it or a derivative of it.

Nothing in this repository is a fork of LumeniteFX or of any other
third-party repository.
