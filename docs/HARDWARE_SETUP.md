# Hardware setup and re-measurement

Replaying the figures needs no hardware. This file is for reviewers or readers
who want to re-run the *acquisitions*.

Expect long runs. The blind value-recovery experiments take 12–34 hours per
board; the reverse-engineering probes take seconds to minutes.

---

## Raspberry Pi 5 (HOMARD-Data, Sections 6 and 7)

**Board.** BCM2712, 16-bank LPDDR4X. Results in the paper are from a 16 GB
board; Section 7.3 repeats them on a 2 GB board.

**Sensor.** The on-board DA9091 PMIC aggregates the DRAM rails and exposes
instantaneous consumption through the VideoCore firmware mailbox at
`/dev/vcio`, sampled at ~44 Hz. Access requires membership in the standard
`video` group, which ordinary desktop users hold by default — **no root, no
`CAP_SYS_*`**.

Two rails carry DRAM signal:

| Rail | Voltage | Role |
|---|---|---|
| `DDR_VDD2_A` | ~1.1 V | core array supply — carries the HD(G,V) cross-term; this is the attack rail |
| `DDR_VDDQ_A` | ~0.6 V | DQ I/O supply — carries the static Hamming weight with the DBI fold |

**Build and load.**

```bash
cd src/pi5
./build.sh                      # builds the kernel module against the running kernel
sudo insmod homard_gadget.ko spec_arm=0
```

`spec_arm` selects the gadget of Appendix C:

| `spec_arm` | Gadget | Paper |
|---|---|---|
| `0` | committed `G→V`, explicit `dc civac` flush | Listing 1, Figures 6–8 |
| `14` | Normal non-cacheable, no flush, no barrier | Listing 2, Figure 11 |
| `21` | Spectre-RSB, `G→V` on the squashed path only | Listing 3, Figure 10 |

**Acquire.**

```bash
python3 src/pi5/spec_v11_hw_template.py    # Hamming-weight template (Fig 6/10a/11a)
python3 src/pi5/block_hd_v11ref_sweep.py   # HD sweep (Fig 7)
python3 src/pi5/v11_cpa_realvalue.py       # blind 64-byte CPA (Fig 8/10b/11b)
```

**Thermal note.** Long acquisitions drift. The HD sweep interleaves anchor
traces so a polynomial drift model can be fitted and subtracted; the CPA runs
subtract a `G = 0` anchor every 16th trace and drop traces more than three
median absolute deviations from the local median. Keep the board below ~75 °C
and cool between runs — chained runs at temperature degrade correlation
noticeably.

**Placement.** `G` and `V` must share a DRAM **channel** (PA bit 31 on this
board). Same row is not required; see Table 1 and
[`CROSSBANK_CPA.md`](CROSSBANK_CPA.md).

---

## AMD ZCU102 (HOMARD-RE, Figure 3a/3d)

**Board.** Zynq UltraScale+ MPSoC, DDR4-2400 controller, contiguous 2-bit bank
index. PetaLinux.

**Sensor.** TI INA226 monitors on `VCCO_PSDDR` (designator u93), exposed through
Linux `hwmon` at ~28 Hz and world-readable.

```bash
cd src/zcu102 && make
./zcu102_rowconflict_probe --pairs 500 --out test.csv
python3 zcu102_hd_cpa.py test.csv
```

Detach long runs with `systemd-run`; polling the sensor from a second process
during acquisition perturbs the measurement.

---

## NVIDIA Jetson TX2 (HOMARD-RE, Figures 3c/3f, 4)

**Board.** Tegra186 with two physically separate LPDDR4 chips on a 64-bit
bonded bus (two 32-bit sub-channels) at 1331 MT/s, aggressive DVFS.

**Sensor.** On-board INA3221 on `VDD_SYS_DDR`, exposed through Linux IIO at
1 kHz with a 1 mA LSB.

```bash
cd src/tx2 && make
./tx2_knockknock_full --probes 3000 --reps 3 --out kk_large.csv
./tx2_drama --pairs 3000 --out drama_conflictset.csv
```

**Warm-up.** Allow **90 s** of rail warm-up before acquiring. The 1 kHz sensor
would otherwise alias DVFS transients into the measurement. The other two
platforms run at their default configuration.

**Iteration count.** `N` scales with sensor LSB and noise floor: 1–5×10³ on the
ZCU102 and Pi 5 where the rail reads in mA or mW with a noise floor of tens of
µA, and N = 50 000 on the Jetson where the 1 mA LSB requires longer integration.

---

## Ground-truth physical addresses

Evaluating a recovered mapping needs the physical address of each probed
virtual address. These are obtained once per platform by querying
`/proc/self/pagemap`, which is privileged. This is an **offline evaluation
step only** — it is the same cost prior DRAM reverse-engineering work pays, and
the online measurement loop that HOMARD actually attacks with requires no
privilege at all.
