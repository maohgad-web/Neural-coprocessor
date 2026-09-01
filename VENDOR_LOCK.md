# Vendor Lock — Milestone 0 (Gate P0)

Single source of truth for the versions that make P0 pass. CI and the
rig must agree on every row. T8 fills the rig rows and appends the run
report that closes P0.

## Pinned inputs

| Input | Pin | Notes |
|-|-|-|
| ReShade (add-on API) | **v6.8.0** — SHA `18deaa52de0c425a78b329e9cb3c497281cd00ec`, add-on API version 20 | The tag is `v6.8.0`; bare `v6.8` is not a valid ref. CI fetches `include/` at this SHA (`build.yml`); the rig's ReShade build must be the same release, with add-ons enabled |
| LumeniteFX | _TBD — record the stock, unmodified release installed on the rig_ | Runtime asset only; never enters this repo (see `THIRD_PARTY.md`) |
| GPU driver, GPU 0 | _TBD — record at T8_ | — |
| GPU driver, GPU 1 | _TBD — record at T8_ | — |
| ReBAR state | _TBD — record at T8_ | — |

## P0 run report (written at T8)

| Task | Result | Notes |
|-|-|-|
| T1 — add-on loads under stock ReShade, init line in log | _pending_ | — |
| T2 — adapter table logged, adapter 1 selected by LUID | _pending_ | — |
| T3 — private D3D12 device on adapter 1 | _pending_ | — |
| T4 — bridge thread, window, message pump | _pending_ | — |
| T5 — swapchain + present loop on GPU 1 | _pending_ | — |
| T6 — second effect runtime on adapter 1 | _pending_ | — |
| T7 — LumeniteFX chain live on GPU 1 | _pending_ | — |
| T8 — clean teardown, repeated load/unload | _pending_ | — |
