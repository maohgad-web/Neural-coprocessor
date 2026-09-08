# Contributors

Code contributed by people other than the primary author, and what it covers.

## [grtninja](https://github.com/grtninja)

Two fixes to the add-on's own code, reviewed and merged from external pull
requests:

- **Adapter selection.** When the game's swapchain-derived adapter LUID is
  absent from DXGI enumeration, the bridge no longer infers the target from
  output counts - a missing game LUID is now a hard identity gate ahead of
  candidate and tiebreak selection. The selection policy was factored into a
  small pure helper so the boundary is directly regression-tested, with
  table-driven coverage for missing-LUID cases, software filtering, no
  hardware candidates, output tiebreaks, LUID high-half mismatch, and
  enumeration reorder.
- **`mgpu.ini` parsing.** The previous reader accepted a silently clipped
  document past its 8192-byte buffer, so settings after the cutoff read as
  absent with no warning - a real problem once the shipped config itself grew
  past that size. The bounded read is now 64 KiB, a document that doesn't fit
  is rejected outright rather than partially applied, and the line-key parser
  is shared with a portable, CPU-only regression suite covering encoding
  (UTF-8 BOM, UTF-16 rejection, malformed UTF-8, embedded NULs, control
  bytes) and boundary conditions.

Thank you for both - especially for the regression coverage that makes these
correctness properties checkable going forward rather than just asserted.
