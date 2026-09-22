# Limitations and scope

What this artifact does and does not establish, stated plainly.

## What replaying establishes

The analysis pipeline is correct and the published numbers follow from the
recorded measurements. Figures 3–7, 12 and Table 2 are recomputed from raw
per-pair or per-trace data, so an analysis error would surface. See
`docs/REPRODUCTION.md` for which items are recomputed and which are redrawn
from cached curves.

## What replaying does not establish

Replaying cannot confirm that the measurements themselves are faithful to the
hardware. That requires the boards. We include the measurement code for all
three platforms and can arrange reviewer access to a Pi 5 rig.

## Limits of the attack itself (from the paper)

- **HOMARD-Data needs a core-array rail.** Where only an I/O rail such as
  `DDR_VDDQ_A` is exposed, only the static Hamming-weight channel is available;
  I/O rails carry no HD cross-term, so value recovery is unavailable. Where no
  DRAM rail reaches userspace, the attack does not apply — many smartphone SoCs
  fall here.
- **G and V must share a DRAM channel.** Row and bank co-location are not
  required, but crossing to a different channel removes the shared DQ bus and
  the attack fails outright (0/64 bytes). See Table 1.
- **Acquisition is slow.** Blind value recovery takes 12–34 hours per 64-byte
  block. This is a profiled, low-bandwidth channel, not a fast exfiltration
  primitive.
- **Channels, ranks and bank groups are untested.** The genericity argument of
  Appendix B predicts that a bank group imposes a distinct column-to-column
  delay and a rank switch a distinct bus turnaround, so each should yield its
  own current level. Our three platforms expose none of these, so the claim
  stands on the argument and not on measurement. Confirming it on a multi-rank,
  bank-grouped DDR4/5 platform is future work.
- **The gadget is a controlled workload.** Section 7.5 discusses how comparable
  access patterns arise in real applications, but the evaluation uses a kernel
  module that loads an attacker block next to a secret block. The artifact does
  not demonstrate an end-to-end attack on a production service.

## Reporting a discrepancy

Please check `docs/REPRODUCTION.md` first — the known deviations are listed
there with their causes. If something outside that list does not reproduce,
include the output of `make check`, the failing step's full stdout, and your
Python and package versions.
