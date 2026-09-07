# Acknowledgements, AI use, and disclaimer

## How this was built

This project was developed by one person working with two AI systems, across
many sessions. That collaboration is not incidental to the result and it would
be dishonest to leave it out of the record, so here is what it actually looked
like.

The **P0 milestone** — the initial add-on scaffold, adapter enumeration and
selection, the bridge thread, and the first window and present chain — was
drafted by a local coding agent running entirely on the developer's own
hardware: the model file `Qwen3.8-27B-Q6_K.gguf`, an Unsloth GGUF quantisation
(Q6_K) of a Qwen3 model, served by
`llama-server` from the llama.cpp CUDA 12 build shipped with LM Studio
(`llama.cpp-win-x86_64-nvidia-cuda12-avx2-2.29.0`), with a 150,072-token
context, flash attention on, q8_0 KV cache, and the model tensor-split 50/50
across the same two RTX 5060 Ti cards this project measures. Sampling was
Qwen's recommended settings: temperature 1.0, top-k 20, top-p 0.95, min-p 0,
no repetition penalty.

It ran with deliberately limited permissions: read access to GitHub
repositories, a single tool call to write to one repository, and no local
compiler — every build went through GitHub CI. It worked from written task
briefs, one per milestone, which carried acceptance criteria and review
standards; the standards were tightened after it marked an unimplemented
acceptance item as passed on the strength of a green log. That episode is the
origin of the first rule in `METHOD.md`. The briefs are not kept in the working
tree — they were instructions to a tool, not documentation — but they are in this
repository's git history if anyone wants to read them.

**That workflow covered P0 and P1.0 and nothing after.** From P1.1 onward the
briefs were abandoned in favour of working directly.

**Everything from P1.1 onward** was written with Claude (Anthropic). Claude wrote
most of the C++ in the bridge, proposed the instrumentation
design, analysed rig logs, and drafted the documentation. It also produced
several confident, well-formatted, wrong diagnoses along the way — the kind
this project came to call an instrument failure, because a wrong answer in the
right format is harder to catch than an obvious error. The causes of at least
two of the worst incidents were found by the human, against Claude's stated
hypothesis:

- A run whose colours were destroyed was blamed on three separate code changes,
  two of which shipped. The actual cause was a stale ReShade preset enabling a
  motion-flow debug view over the neural output. Found by hand, by checking
  config files.
- A frame-rate regression was twice asserted to be impossible before the rig
  data showed it was real (a mutex held across a GPU fence wait).

Both are documented in the records in this repository rather than quietly
fixed, because how a defect was found is part of what the measurements mean.
The working rule that came out of it: a green log is not a passed test, and
acceptance is checked against the source that implements it.

**The constraints turned out to matter more than the model choice.** A small
local model, with a narrow write surface and no compiler of its own, produced
usable milestone code. The failures it did have were failures of specification
rather than of capacity — it did what the brief said, and where the brief was
wrong or silent, so was the code.

Anyone evaluating this work should weigh it accordingly — the code was written
with substantial AI assistance, and the claims rest on measurements that are
published alongside it so they can be checked rather than trusted.

## Prior work this builds on

None of this would exist without the following, and the debt is real rather
than formal:

- **[crosire/ReShade](https://github.com/crosire/reshade)** — the add-on API
  this entire bridge lives inside. Patrick Mours' add-on architecture is what
  makes a second effect runtime on a second adapter possible at all.
- **[jlrouzies-fr/DLSS5-Feeder](https://github.com/jlrouzies-fr/DLSS5-Feeder)** —
  the approach that showed DLSS Neural Rendering could be driven from outside
  a game's own renderer.
- **[Dagherbou/OptiScaler_DLSSNR](https://github.com/Dagherbou/OptiScaler_DLSSNR)** —
  studied as an alternative foundation and as a reference for the DLSS-NR
  feature and parameter space. **It is not the local arm in any measurement
  here** — see below.
- **[clshortfuse/renodx](https://github.com/clshortfuse/renodx)** — the local-GPU
  neural rendering path used as the comparison arm throughout. Every offload
  figure in this work is measured against it.

**On which local path was measured.** DLSS-NR can be reached on the render device
by more than one route, and the comparison arm in `RESULTS.md` is specifically
**RenoDX under ReShade**. **OptiScaler was not measured**, and nothing in this
repository characterises its performance. That distinction is stated because
naming a project as a comparison arm is a claim about that project, and the
narrower the claim the fairer it is: what was measured is one route on one
machine, not "local neural rendering" as a category.

It should also be said plainly that using an existing local implementation as a
comparison arm is not a criticism of it. This project set out to build its own
path — its own optical flow, its own transport — rather than to improve on
anyone's; a comparison arm is a reference point, and a well-made one is what
makes the offload figure mean anything.
- **LumeniteFX** — the shader suite used in the earlier pipeline stages.
- **[ocornut/Dear ImGui](https://github.com/ocornut/imgui)** — the overlay
  panel. Fetched at configure time; no ImGui source is vendored here.
- **Lossless Scaling** — the prior art that made a dual-GPU split of
  post-processing work look plausible before any of this was attempted.
- **[Unsloth](https://huggingface.co/unsloth)** and the **Qwen** team — the
  quantised local model that drafted P0, and the model behind it.
- **[ggml-org/llama.cpp](https://github.com/ggml-org/llama.cpp)** and **LM
  Studio** — what made running that model on this hardware possible at all.

Thank you to all of them. Mistakes in this repository are ours, not theirs.

## Community testing

**[salient-cyanocitta](https://github.com/salient-cyanocitta)** confirmed this
running on an RTX 4080 SUPER + RTX 5060 Ti pair, closing the open question on
40-series (Ada) hardware in [discussion #3](https://github.com/maohgad-web/Neural-coprocessor/discussions/3).
That report also ruled out PCIe bandwidth as the cause of a smoothness issue
seen at high load, on a machine running both cards at PCIe 4.0 x8. Thank you
for the testing, the log, and the video.

## Please read the code before you run it

This is research code. It creates a second D3D12 device, allocates
cross-adapter shared heaps, loads NVIDIA's NGX libraries from your driver
installation, and runs a neural feature on a second GPU. It has been exercised on
**exactly one machine** — the rig described in `VENDOR_LOCK.md` — with three
titles, only one of which produced the published measurements, for sessions
measured in minutes rather than hours. A second rig is documented there because
it changes how the first one's numbers must be read; **nothing has been run on
it.**

You are encouraged — genuinely, not as a formality — to read the source before
running it, and to check that what it does matches what this documentation
claims it does. The instrumentation is deliberately loud: the log states which
configuration file took effect, which adapter was selected and why, and what is
actually on screen. If a log line and this README disagree, believe the log and
please open an issue.

## What is not distributed here

No NVIDIA binaries are included in this repository or in any release package.
`_nvngx.dll`, `nvngx_dlssnr.dll` and the DLSS-NR weights come from your own
driver installation and are never redistributed here. Do not add them to a
release package to make it work for someone else.

ReShade is not vendored either; CI fetches the headers it needs at build time,
and users install ReShade themselves — with add-on support, which is required.

## Disclaimer

This software is provided as-is, without warranty of any kind, express or
implied. See the LICENSE file, which carries the operative warranty and
liability terms; this section is context, not a substitute for it.

Running this changes how a game's frames are processed and drives an
undocumented path through vendor libraries on your own hardware. It may crash,
produce incorrect output, or behave in ways not observed on the single machine it
was developed on. Stability beyond the durations recorded in the measurements
has not been tested. You run it at your own risk, and the authors accept no
responsibility for any damage, data loss, or consequences arising from its use
or misuse.

Nothing here is affiliated with, endorsed by, or supported by NVIDIA.
