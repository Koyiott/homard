#!/usr/bin/env python3
"""block_hd_v11ref_sweep.py — HD (bit-toggling) characterization on the CANONICAL
cachable gadget: v11 ARM_REF (LDP G(64B) then LDP V(64B), civac flush, single
line, G@+0 / V@+64 colocated in the SAME DRAM row, NO R amplification).

This is the same block-randomized + HD=0-anchor design as block_hd_arch_sweep.py
(min13 HAMMER_ARCH), ported to the v11 ioctl interface and the ARM_REF gadget so
the figure matches the gadget actually used for the CPA. A strong DRAM channel is
selected (PA bit31=1) so the slope reproduces the gamma2 of the paper.

Fix V uniform 0x33, sweep G so HD(G,V)=sum_i HW(g_i^v_i) spans {0,8,...,512}
holding HW(G) constant (0x33 and 0xCC both have HW=4), 6 blocks, HD=0 anchors at
{0,16,32,48,64} per block for wall-clock drift correction.
Output: /tmp/block_hd_v11ref_sweep.csv (block_hd_arch_sweep.csv-compatible).
"""
import fcntl, os, struct, subprocess, time, sys, math, random, csv

DEV = "/dev/spec_power_p2_v11"; MODULE = "spec_power_p2_v11"
KO = os.environ.get("V11_KO", "/home/pi/spectre_multi/phase2/armL_build/spec_power_p2_v11.ko")
MAGIC = 0xF1; LINE_BYTES = 64; V_BYTE = 0x33

HD_VALUES = list(range(0, 513, 8))          # 65 points
BLOCKS = 6
ANCHOR_POSITIONS = [0, 16, 32, 48, 64]
BLOCK_SIZE = len(HD_VALUES) + len(ANCHOR_POSITIONS)
SAMPLE_DURATION_S = 3.0
SAMPLE_INTERVAL_S = 0.10
INTER_TRACE_SLEEP = 0.10
WARMUP_DURATION_S = 90.0
WARMUP_HD = 256
BASE_SEED = 0xDEADBEEF
ANCHOR_HD = 0
ARM_REF = 0
MIN_GAMMA = float(os.environ.get("MIN_GAMMA", "16"))   # strong channel
LAYOUT_TRIES = 150
OUT_CSV = "/tmp/block_hd_v11ref_sweep.csv"

def _IOW(n, sz): return 0x40000000 | (sz << 16) | (MAGIC << 8) | n
def _IO(n): return (MAGIC << 8) | n
def _IOWR(n, sz): return 0xC0000000 | (sz << 16) | (MAGIC << 8) | n
SET_V = _IOW(1, LINE_BYTES); SET_G = _IOW(2, LINE_BYTES)
START = _IO(3); STOP = _IO(4); SET_ARM = _IOW(21, 4); GET_PA = _IOWR(30, 16)

def op(c): fd = os.open(DEV, os.O_RDWR); fcntl.ioctl(fd, c, 0); os.close(fd)
def buf(c, b): fd = os.open(DEV, os.O_RDWR); fcntl.ioctl(fd, c, b); os.close(fd)
def u32(c, v): fd = os.open(DEV, os.O_RDWR); fcntl.ioctl(fd, c, v); os.close(fd)
def getpa(off):
    fd = os.open(DEV, os.O_RDWR); b = struct.pack("QQ", off, 0)
    r = fcntl.ioctl(fd, GET_PA, b); os.close(fd); return struct.unpack("QQ", r)[1]
def popc(x): return bin(x & 0xFF).count('1')

def read_pmic():
    o = subprocess.run(["vcgencmd", "pmic_read_adc"], capture_output=True, text=True).stdout
    out = {}
    for L in o.splitlines():
        if "_A current" in L:
            name = L.split()[0]
            try: out[name.replace("_A", "") + "_A_curr_mA"] = float(L.strip().split("=")[1].rstrip("A")) * 1000.0
            except Exception: pass
    return out

REMAINDER_BYTES = {hd: next((b for b in range(256) if popc(b) <= 4 and popc(b ^ V_BYTE) == hd), None) for hd in range(9)}
for hd, b in REMAINDER_BYTES.items(): assert b is not None, f"no byte for HD={hd}"

def make_g_with_hd(hd_total):
    q, r = divmod(hd_total, 8)
    g = bytearray([V_BYTE] * LINE_BYTES)
    for i in range(q): g[i] = 0xCC          # 0x33^0xCC=0xFF -> HD 8, HW(0xCC)=4
    if r > 0: g[q] = REMAINDER_BYTES[r]
    return bytes(g)

def mstd(xs):
    if not xs: return float("nan"), float("nan")
    m = sum(xs) / len(xs)
    if len(xs) < 2: return m, 0.0
    return m, math.sqrt(sum((x - m) ** 2 for x in xs) / (len(xs) - 1))

def measure_one(g_bytes, s):
    op(STOP); buf(SET_G, g_bytes); op(START)
    vdd2, vddq, core = [], [], []
    t0 = time.time()
    while time.time() - t0 < s:
        d = read_pmic()
        if d.get("DDR_VDD2_A_curr_mA") is not None: vdd2.append(d["DDR_VDD2_A_curr_mA"])
        if d.get("DDR_VDDQ_A_curr_mA") is not None: vddq.append(d["DDR_VDDQ_A_curr_mA"])
        if d.get("VDD_CORE_A_curr_mA") is not None: core.append(d["VDD_CORE_A_curr_mA"])
        time.sleep(SAMPLE_INTERVAL_S)
    op(STOP)
    return mstd(vdd2), mstd(vddq), mstd(core)

def load_module():
    subprocess.run(["sudo", "rmmod", MODULE], capture_output=True); time.sleep(0.3)
    r = subprocess.run(["sudo", "insmod", KO, "n_workers=1"], capture_output=True, text=True)
    if r.returncode != 0: sys.stderr.write(f"insmod failed: {r.stderr}\n"); sys.exit(1)
    for _ in range(20):
        if os.path.exists(DEV): break
        time.sleep(0.1)

def quick_gamma():
    u32(SET_ARM, ARM_REF)
    def m(gb):
        buf(SET_V, bytes([0xFF]) * LINE_BYTES); buf(SET_G, bytes([gb]) * LINE_BYTES); op(START)
        xs = []; t = time.time()
        while time.time() - t < 1.2:
            v = read_pmic().get("DDR_VDD2_A_curr_mA")
            if v is not None: xs.append(v)
            time.sleep(0.06)
        op(STOP); return sum(xs) / len(xs) if xs else float("nan")
    return (m(0x00) - m(0xFF)) / 512.0 * 1000.0

def secure_strong():
    last = (None, 0.0, 0)
    for k in range(LAYOUT_TRIES):
        load_module(); pa = getpa(0); b31 = (pa >> 31) & 1; g = quick_gamma()
        print(f"  [layout {k}] PA=0x{pa:x} bit31={b31} gamma={g:.1f}", flush=True)
        last = (pa, g, b31)
        if g >= MIN_GAMMA and b31 == 1:
            print(f"  [layout] ACCEPTED strong: gamma={g:.1f} bit31=1", flush=True); return pa, g
    print(f"  [layout] WARN: using last (gamma={last[1]:.1f} bit31={last[2]})", flush=True)
    return last[0], last[1]

def build_block(b):
    rng = random.Random(BASE_SEED + b); perm = list(HD_VALUES); rng.shuffle(perm)
    anchor = set(ANCHOR_POSITIONS); out = []; pi = 0
    for pos in range(BLOCK_SIZE):
        if pos in anchor: out.append((True, ANCHOR_HD))
        else: out.append((False, perm[pi])); pi += 1
    return out

def main():
    n_traces = BLOCKS * BLOCK_SIZE
    print(f"v11 ARM_REF HD sweep: {len(HD_VALUES)} HDs x {BLOCKS} blocks, {n_traces} traces, "
          f"~{(n_traces*(SAMPLE_DURATION_S+INTER_TRACE_SLEEP)+WARMUP_DURATION_S)/60:.0f} min", flush=True)
    pa, g = secure_strong()
    buf(SET_V, bytes([V_BYTE] * LINE_BYTES)); u32(SET_ARM, ARM_REF)
    print(f"  [init] V=0x{V_BYTE:02X} sealed; ARM_REF; PA=0x{pa:x} gamma={g:.1f}", flush=True)
    print(f"  warmup HD={WARMUP_HD} {WARMUP_DURATION_S:.0f}s", flush=True)
    op(STOP); buf(SET_G, make_g_with_hd(WARMUP_HD)); op(START)
    time.sleep(WARMUP_DURATION_S); op(STOP)
    with open(OUT_CSV, "w") as fp:
        w = csv.writer(fp)
        w.writerow(["block_idx", "position_in_block", "order_idx", "time_s", "is_anchor", "hd_total", "rep",
                    "mean_VDD2_mA", "std_VDD2_mA", "mean_VDDQ_mA", "std_VDDQ_mA", "mean_CORE_mA", "std_CORE_mA"])
        t0 = time.time(); oi = 0
        for b in range(BLOCKS):
            for pos, (is_anc, hd) in enumerate(build_block(b)):
                tt = time.time() - t0
                (mv2, sv2), (mvq, svq), (mc, sc) = measure_one(make_g_with_hd(hd), SAMPLE_DURATION_S)
                rep = -1 if is_anc else b
                w.writerow([b, pos, oi, f"{tt:.3f}", int(is_anc), hd, rep,
                            f"{mv2:.4f}", f"{sv2:.4f}", f"{mvq:.4f}", f"{svq:.4f}", f"{mc:.4f}", f"{sc:.4f}"])
                fp.flush()
                if oi % 32 == 0:
                    print(f"  idx={oi:4d}/{n_traces} b={b} {'ANCH' if is_anc else 'MAIN'} HD={hd:3d} "
                          f"VDD2={mv2:.2f} t={(time.time()-t0)/60:.1f}min", flush=True)
                oi += 1; time.sleep(INTER_TRACE_SLEEP)
    op(STOP)
    print(f"DONE -> {OUT_CSV}", flush=True)

if __name__ == "__main__":
    main()
