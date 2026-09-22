# Requirements

## To replay the figures and tables (what the AEC needs)

| | |
|---|---|
| OS | Any Linux or macOS; verified on Ubuntu 24.04 |
| CPU | Any x86-64 or arm64 |
| RAM | ~200 MB |
| Disk | ~90 MB for the repository |
| Python | 3.8 or newer (verified on 3.12.3) |
| Packages | numpy ≥ 1.24, pandas ≥ 2.0, matplotlib ≥ 3.7, scipy ≥ 1.10 |
| Network | Not required |
| Privileges | None |
| Runtime | ~30 s for the full set |

## To re-measure from scratch (optional)

Only needed to regenerate the raw data rather than replay it.

| Platform | Requirement |
|---|---|
| Raspberry Pi 5 | 16 GB board (2 GB for the Section 7.3 replicate), kernel headers to build the gadget module, membership in `video` for `/dev/vcio` |
| AMD ZCU102 | PetaLinux with INA226 `hwmon` exposed |
| NVIDIA Jetson TX2 | L4T with INA3221 exposed through IIO |

Acquisition times: reverse-engineering probes run in seconds to minutes
(≈500 s on the ZCU102, ≈30 s on the Pi 5); the blind value-recovery runs take
12–34 hours per board. Privileged access to `/proc/self/pagemap` is needed once
per platform for ground-truth evaluation only, never for the attack itself.
