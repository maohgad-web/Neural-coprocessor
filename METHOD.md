# Method

The working rules this project accumulated, each one because breaking it cost
something. They are written as rules rather than advice because that is how they
were learned.

Nothing here is specific to graphics. It is specific to measuring a system you
cannot see inside, using instruments you also wrote.

---

## The failure this project is actually organised against

Not the crash. The crash is free — it announces itself, it stops the run, and it
is fixed before anything is believed.

The expensive failure is **a confident, correctly-formatted, wrong answer.** A
probe that returns a precise hexadecimal result to a question nobody asked. A
log whose calls are in order, whose codes are real, whose numbers are correctly
reported, and whose conclusion is false. Nothing about it looks wrong, so it is
believed, and everything built on it inherits the error silently.

That happened here, more than once, and it is the reason for every rule below.

---

## 1 · A green log is not a passed task

A run that produced no errors is evidence of nothing until the checker that
would have produced the error has itself been observed to fire.

An instrument that has never failed is not known to work. A clean run from an
unproven checker and a clean run from a checker that silently returns `true` are
the same log file.

**Acceptance for an instrument is a clean run plus one deliberately failed run
per fault class, each producing its own named diagnosis.** In this project that
meant a fault-injection switch — `pitch`, `alias`, `magic`, `drop`, `stale` —
read from a file beside the DLL, absent meaning clean, so the shipped default is
a normal run and a missing file is never an error. (`tear` is specified in the
instrument design and was never implemented; it is listed there and not here,
because a fault class that exists only in a document is not a fault class.)

The switch stays in the shipped code. The cost is a branch. The benefit is that
the instrument can be re-proven on any future machine, driver or milestone
without rebuilding it.

**Where this project actually stands against its own rule, stated precisely
because a vague version is worth nothing.** `pitch`, `alias` and `magic` were
each injected on the rig and each produced its own named diagnosis and nothing
else. `drop` and `stale` were never injected; the counters they would have driven
(`dropped`, `reordered`, `overrun`) were instead driven non-zero by real
conditions during a two-pass run and have since returned to zero — which
exercises them, but by accident rather than by design, and does not prove that
the injector itself works.

The narrower thing still owed: none of the five has been re-proven since the
consumer was rewritten to skip superseded frames. The checker code did not
change, but the cadence it runs at did, and "the checker worked against the
previous consumer" is not the same claim.

**The instructive part is how nearly this section said something false.** One
record in this repository stated that fault injection had never been run; another,
written earlier and marked verified, recorded three of the faults tripping. The
first was believed because it was read most recently, and a paragraph of
methodology was written on top of it before the contradiction was noticed by the
person who had actually run them. Two sources, one wrong, and the wrong one was
the more recent — **recency is not authority, and a document that is confident is
not thereby a primary source.**

Two corollaries that were learned the hard way:

- **An unknown fault name must be refused loudly.** Accepting a fault the build
  cannot inject produces a run with all-zero counters, which the
  fault-injection warning then reports as a checker that failed to trip — a
  false accusation against working code.
- **A rejected sample must not be allowed to manufacture a second fault.** A
  corrupted seal at frame N leaves `last_seen` at N−1, so frame N+1 computes a
  gap of 2 and reports a drop that never happened: one injected fault, two
  counters, and the producer blamed for a consumer-side error. One gap check is
  suppressed after a rejection, and the suppression is itself counted.

---

## 2 · Pre-fill every output with a sentinel — and prefer a differential when you can afford one

Before a call that is supposed to write a value, write a value into it that the
call cannot plausibly produce. Without this, a call that fails to write anything
is indistinguishable from a call that wrote a zero, and zero is a legitimate
answer to most questions. For a scalar output that is cheap enough that there is
no threshold below which it is not worth doing.

**For a buffer, the sentinel is not enough, and this project learned that three
separate times.** A sentinel asks *does this value still equal the value I
chose?* — an absolute test. Over millions of pixels, real content eventually
equals the fill by coincidence. Once it did on a single mid-grey pixel and
produced `PROBE FAILED` on a run where nothing had failed. Once it produced a
warning that NVIDIA's model had not written pixels it had in fact written. The
false positive even *reproduced*, which made it look like a mechanism, because
two of the three runs being compared were byte-identical outputs and so had to
agree.

The general replacement is the **differential test**: run the operation twice,
identical in every respect except the pre-fill, and compare the two results to
each other.

```
fill destination with 0x00 → run → R1
fill destination with 0xFF → run → R2
unwritten == count(R1 != R2)
```

A pixel the operation wrote holds the same produced value in both runs and
matches. A pixel it did not write holds `0x00` in one and `0xFF` in the other and
**cannot** match. **No false positive is possible** — there is no value a correct
result could take that fakes a miss — and it is format-independent, which is
exactly where the absolute test failed, because on a packed format the compared
bytes are not the compared quantity.

It costs one extra run. Keep the absolute sentinel only where a second run is
impossible — a one-shot capture of a live frame — and there, count the sentinel
in a control buffer too and subtract. **An absolute test with a control is sound;
an absolute test alone is not.**

---

## 3 · Run a control at the same setting

The comparisons that carry the conclusion in `RESULTS.md` are paired runs
differing in exactly one thing. Not "a baseline taken earlier" — a control taken
at the same resolution, the same DLSS mode, the same scene, on the same day.

**Not everything in that document meets the standard, and where it does not, it
says so.** The local arm's figures come from an on-screen overlay reading; the
offload arm's come from thousands of frames of logged timestamps. Two parameters
between the arms could not be matched at all. Those are stated as limitations
rather than smoothed over, because the alternative — quietly holding weaker
comparisons to the same language as strong ones — is how a document stops being
worth checking.

The reason the paired ones matter is in the next rule.

---

## 4 · A difference between runs that differ in more than one variable is not a finding

This project spent launches relearning it.

A 29% frame-rate gap between two runs was attributed to a mechanism that is real,
documented, and was not the cause. The runs differed in resolution, present rate
and scene as well as in the variable of interest. Two clean runs with one
variable moving showed the opposite sign.

Three things about that are worth more than the correction itself:

- **The rule that would have caught it was already written**, in this project's
  own instrument document, by the same person who then spent the launches. A
  rule in a document does not fire on its own. The moment to apply it is when a
  number looks interesting — which is exactly the moment it feels least
  necessary.
- **The mechanism being real made the story more convincing, not less.** A
  plausible cause is the most dangerous thing to have available when you are
  looking at a number you like.
- **The operator called it before the data did** — *"personally I think we are
  chasing a ghost."* When the person running the machine says a measurement is
  not worth taking, that is evidence, not an objection to be answered.

A related instance: a display reported 1920×1200 when asked for 1920×1080, and a
run was taken in that stretched non-native mode before anyone noticed. **Check
the value the instrument logs against the value you intended, not against what
you typed into a settings dialog.**

---

## 5 · Vary one field per row

When probing an API whose contract is unknown, change one parameter per attempt
and record the result code for each. A batch of changes that finally works tells
you almost nothing, and the temptation to stop there and move on is strong.

The result-code ladder in `ARCHITECTURE.md` — five distinct failure codes, each
mapped to a specific cause — exists only because each one was reached by a
single-field change from a known state.

---

## 6 · Split every HRESULT

Two calls on one line share a return value and lose which of them failed. A
compound expression that succeeds hides which branch produced the success.

Log the code, the call, and the arguments that mattered, per call. The verbosity
is the point: the instrumentation in this project is deliberately loud because
the alternative is a quiet failure that reads as a result.

---

## 7 · A fallback that changes what is being measured is not a fallback

If a probe tries several configurations and takes the first that returns
success, the substitution must be carried into the verdict. Otherwise the
downstream number is a correct measurement of a different question.

The concrete form here: a verdict line prints `eligible=YES/NO`, and the
downstream result is **defined as meaningless unless it reads YES**. Not
"suspect" — meaningless. A softer word invites someone to use it anyway.

---

## 8 · Distinguish invariant from perishable, and re-check the perishable

Some facts are properties of the design and stay true: which module owns a
parameter block, what a shared heap requires, the order of a fence signal
against the copies it guards.

Some are properties of this driver, this Windows build, this game version, this
day: a specific result code, a preset name, a DLL version, the number of
adapters Windows reports as owning outputs.

Filing a perishable fact as an invariant is how a project ends up confidently
asserting something that stopped being true two driver releases ago. Every
version-dependent fact in this repository is recorded with the version it was
observed under, and the ones that are known to move are marked as such.

---

## 9 · Do not enable the D3D12 debug layer to investigate a cross-adapter problem

It changes allocation behaviour, timing, and in this project's experience the
success or failure of the exact calls under investigation. It is a fine tool for
finding a bug in your own code and a poor one for characterising a vendor
library's contract.

Stated as a rule because it was reached for repeatedly and wasted time each
time.

---

## 10 · Retract in place, struck through

When something in the record turns out to be wrong, the wrong statement stays,
struck through, with the correction next to it and the reason it was wrong.

Deleting it produces a document that has always been right, which is both false
and less useful — the reasoning that produced the error is usually more
instructive than the corrected fact, and a reader deciding whether to trust the
document needs to see how it behaves when it is wrong.

Every correction in this repository was made this way. Several of them removed
caveats rather than adding them.

---

## 11 · Measurement is bounded; the shipping default is not

While anything is being measured, the run has a frame bound, so every run ends
the same way and comparison is possible. The shipped configuration is unbounded,
because a person using the tool is not taking a measurement.

These are different builds of the same code with a different value in a settings
file, and confusing them produces runs of unequal length being compared against
each other.

---

## 12 · Stop measuring things that are not worth measuring

The last rule, and the one that had to come from the operator rather than from
the record.

A result observed directly, twice, by independent means, does not need a third
instrument built for it. *"I saw the neural stage change the image, and it cost
45% of the frame rate"* is two confirmations. Constructing a formal test for it
would have been rigour spent where there was no doubt, and not spent where there
was.

Rigour is a budget. Spend it on the claims that carry the conclusion.

---

## What this method does not cover

Stated so it is not mistaken for complete.

- **Image quality.** Nothing in this repository assesses how the output looks.
  That needs exposure-normalised comparison and a protocol this project does not
  have.
- **Photon-to-photon latency.** Timings here are submit-to-consume.
- **Long-run stability.** The longest clean run recorded is about five and a half
  minutes, at one and two passes. The one session that ended badly was later
  explained — see `RESULTS.md` — and is the origin of a rule this document did
  not have: **the observation that could hurt someone is never the one to defer.**
  It was filed as out of scope for two days on the grounds that the architecture
  claim did not depend on it, which was true and beside the point.
- **Generality across hardware.** Every number was taken on the machines
  described in `RESULTS.md`. The architectural claims should transfer; the
  figures are not asserted to.
