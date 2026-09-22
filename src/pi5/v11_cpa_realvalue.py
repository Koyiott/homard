#!/usr/bin/env python3
"""v11_cpa_realvalue.py — FULL-VALUE CPA on a REAL, NON-REDUNDANT kernel secret.

Difference vs v11_cpa_n64.py (the 30/30 single-byte run):
  * V = a fixed 64-byte secret with DISTINCT bytes (a real impactful kernel
    value), set ONCE.  NO in-cache-line redundancy ("no R amplification").
  * G = 64 INDEPENDENT random bytes per trace (each DQ lane / byte position
    gets its own fresh random g_i).  The full 64-byte G is logged per trace so
    the analyser can run per-byte-position CPA / coupling-aware recovery on
    HW(g_i ^ v_i) and recover EVERY byte from ONE trace set.

Independent per-lane random G ("the right model-agnostic data to collect"):
  The DDR_VDD2 bus-switching leakage is  sum_i HW(g_i ^ v_i)  over the 64
  lane/beat positions, plus second-order DQ-coupling cross-terms between
  neighbouring lanes.  Randomising every lane INDEPENDENTLY makes the cross-term
  VARIANCE beatable by more traces; it does NOT by itself remove the structural
  BIAS of a SIGNED / non-HW-monotone coupling kernel (project notes: V-dependent
  coupling, quadratic-in-(g^v) at R^2~0.97).  That bias is removed OFFLINE by the
  coupling-aware analyser (cpa_realvalue_analysis.py), which re-uses this same
  logged G.  random64 is the single-pass acquisition that supports any such model.

Gadget: v11 ARM_REF (arm 0) = dc civac G+V ; arch LDP G(64B) ; arch LDP V(64B).
G@+0x0, V@+0x40 on the SAME DRAM bank+row (proven same 4 KiB page).

Robustness for a long remote run:
  * --resume appends to an existing CSV, replaying the RNG so g continues the
    same stream (run detached via systemd-run so an SSH drop does not kill it).
  * Thermal gate: if T >= --temp-max STOP the gadget and idle (no DRAM activity)
    until T <= --temp-resume; NaN temp is treated as "still hot" (never an early
    resume).  A finite-temp self-check warns loudly if the sensor is dead so the
    operator knows thermal protection is inert.
  * A 'seg' column increments on every cooldown resume so the analyser can
    detrend per-segment across baseline steps.
"""
import fcntl, os, struct, subprocess, time, sys, math, argparse, csv, random, json

DEV    = "/dev/spec_power_p2_v11"
MODULE = "spec_power_p2_v11"
KO     = os.environ.get("V11_KO", "/home/pi/spectre_multi/phase2/spec_power_p2_v11.ko")
MAGIC  = 0xF1
LINE_BYTES = 64

def _IOW(n, sz): return 0x40000000 | (sz << 16) | (MAGIC << 8) | n
def _IO(n):      return (MAGIC << 8) | n
def _IOWR(n, sz): return 0xC0000000 | (sz << 16) | (MAGIC << 8) | n
SET_V   = _IOW(1, LINE_BYTES)
SET_G   = _IOW(2, LINE_BYTES)
START   = _IO(3)
STOP    = _IO(4)
SET_ARM = _IOW(21, 4)
GET_PA  = _IOWR(30, 16)   # struct spec_pa_query {u64 offset, phys_addr}
SET_ANCHOR_K = _IOW(32, 4)  # ARM_T ALU-delay window length
_ANCHOR_K = None            # set from --anchor-k; passed to insmod + ioctl

# --- ioctl helpers: copied VERBATIM from the validated v11_cpa_n64.py -------
def ioctl_op(c):
    fd = os.open(DEV, os.O_RDWR); fcntl.ioctl(fd, c, 0); os.close(fd)
def ioctl_buf(c, buf):
    fd = os.open(DEV, os.O_RDWR); fcntl.ioctl(fd, c, buf); os.close(fd)
def ioctl_u32(c, v):
    # v11 driver reads SET_ARM arg as (unsigned int)arg; pass the int directly.
    fd = os.open(DEV, os.O_RDWR); fcntl.ioctl(fd, c, v); os.close(fd)
def ioctl_getpa(off):
    fd = os.open(DEV, os.O_RDWR); b = struct.pack("QQ", off, 0)
    r = fcntl.ioctl(fd, GET_PA, b); os.close(fd)
    return struct.unpack("QQ", r)[1]

def read_pmic():
    o = subprocess.run(["vcgencmd","pmic_read_adc"], capture_output=True, text=True).stdout
    out = {}
    for L in o.splitlines():
        if "_A current" in L:
            name = L.split()[0]
            try:
                amps = float(L.strip().split("=")[1].rstrip("A"))
                out[name.replace("_A","") + "_A_curr_mA"] = amps * 1000.0
            except Exception: pass
    return out

def measure_temp():
    o = subprocess.run(["vcgencmd","measure_temp"], capture_output=True, text=True).stdout
    try: return float(o.split("=")[1].rstrip("'C\n"))
    except Exception: return float('nan')

def mstd(xs):
    if not xs: return float('nan'), float('nan')
    m = sum(xs)/len(xs)
    if len(xs) < 2: return m, 0.0
    v = sum((x-m)**2 for x in xs)/(len(xs)-1)
    return m, math.sqrt(v)

def _load_module():
    for mod in ["spec_power_p2_min13","spec_power_p2_min12","spec_power_p2_min11", MODULE]:
        subprocess.run(["sudo","rmmod",mod], capture_output=True)
    time.sleep(0.4)
    ins = ["sudo","insmod",KO,"n_workers=1"]
    if _ANCHOR_K is not None: ins.append(f"anchor_k={_ANCHOR_K}")
    r = subprocess.run(ins, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(f"insmod failed: {r.stderr}\n"); sys.exit(1)
    for _ in range(20):
        if os.path.exists(DEV): break
        time.sleep(0.1)

def _quick_gamma(vbyte=0xFF, dur=2.0, arm=0):
    """Amplified leakage swing (V=vbyte x64): gamma = [VDD2(G=0) - VDD2(G=0xFF)]/512.
    A fast proxy (~mA scale) for the per-bit leakage amplitude of the current
    allocation (DDR_VDD2 couples ~4x more strongly when PA bit31=1)."""
    ioctl_buf(SET_V, bytes([vbyte]) * LINE_BYTES); ioctl_u32(SET_ARM, arm)
    def m(g):
        ioctl_buf(SET_G, bytes([g]) * LINE_BYTES); ioctl_op(START)
        xs = []; t1 = time.time()
        while time.time() - t1 < dur:
            d = read_pmic()
            if d.get("DDR_VDD2_A_curr_mA") is not None: xs.append(d["DDR_VDD2_A_curr_mA"])
            time.sleep(0.08)
        ioctl_op(STOP)
        return sum(xs) / len(xs) if xs else float('nan')
    return (m(0x00) - m(0xFF)) / 512.0 * 1000.0   # uA/bit

def secure_strong_layout(min_gamma, tries, require_bit31=False, arm=0):
    """Reload until kbuf_cache lands in a STRONG-coupling DRAM channel, judged by
    the measured amplified swing gamma>=min_gamma (machine-agnostic).  On the 16GB
    Pi the strong channel happens to be PA bit31=1; on other machines (e.g. 2GB)
    the channel-select bit differs, so gamma is the portable criterion.
    Cannot revisit a past allocation -> accept-or-reload; fall back to last."""
    last = None
    for k in range(tries):
        _load_module()
        pa0 = ioctl_getpa(0); b31 = (pa0 >> 31) & 1; g = _quick_gamma(arm=arm)
        last = (pa0, g, b31)
        print(f"  [layout {k}] PA=0x{pa0:x} bit31={b31} gamma={g:.1f} uA/bit", flush=True)
        if g >= min_gamma and (not require_bit31 or b31 == 1):
            print(f"  [layout] ACCEPTED strong layout: gamma={g:.1f} uA/bit (bit31={b31})", flush=True)
            return pa0, g
    print(f"  [layout] WARN: no layout met gamma>={min_gamma}{' & bit31=1' if require_bit31 else ''} "
          f"in {tries} tries; using last (gamma={last[1]:.1f} bit31={last[2]})", flush=True)
    return last[0], last[1]

def parse_secret(s):
    s = s.strip().lower().replace("0x","")
    b = bytes.fromhex(s)
    if len(b) != LINE_BYTES:
        sys.exit(f"--secret-hex must be {LINE_BYTES} bytes ({2*LINE_BYTES} hex chars); got {len(b)}")
    return b

def gen_g(rng, mode):
    """Return a 64-byte G for one trace.  Always consumes a DETERMINISTIC number
    of rng draws per call (64) so --resume can replay the stream exactly."""
    if mode == "random64":
        return bytes(rng.randrange(256) for _ in range(LINE_BYTES))
    if mode in ("checker_even", "checker_odd"):
        par = 0 if mode == "checker_even" else 1
        g = bytearray(LINE_BYTES)
        for i in range(LINE_BYTES):
            r = rng.randrange(256)               # always draw to keep stream aligned
            if i % 2 == par: g[i] = r
        return bytes(g)
    sys.exit(f"unknown --g-mode {mode}")

def last_index(path):
    """Return (last_order_idx, last_seg) from an existing CSV, or (-1, 0)."""
    last, seg = -1, 0
    try:
        with open(path) as f:
            r = csv.DictReader(f)
            for row in r:
                last = int(row["order_idx"]); seg = int(row.get("seg", 0))
    except Exception:
        return -1, 0
    return last, seg

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--secret-hex", required=True, help="64-byte secret, 128 hex chars")
    ap.add_argument("--n-traces", type=int, default=40000)
    ap.add_argument("--out", required=True)
    ap.add_argument("--seed", type=int, default=0xC0FFEE)
    ap.add_argument("--anchor-every", type=int, default=16)
    ap.add_argument("--sample-dur", type=float, default=1.0)
    ap.add_argument("--sample-interval", type=float, default=0.10)
    ap.add_argument("--warmup", type=float, default=8.0)
    ap.add_argument("--g-mode", default="random64",
                    choices=["random64","checker_even","checker_odd"])
    ap.add_argument("--temp-max", type=float, default=60.0, help="pause gadget above this")
    ap.add_argument("--temp-resume", type=float, default=50.0, help="resume once cooled below")
    ap.add_argument("--cool-poll", type=float, default=5.0)
    ap.add_argument("--cool-max-wait", type=float, default=900.0, help="max seconds to wait per cooldown")
    ap.add_argument("--resume", action="store_true", help="append to existing --out, replaying RNG")
    ap.add_argument("--strong-layout", action="store_true",
                    help="reload until kbuf_cache lands in a strong-coupling channel (by measured gamma)")
    ap.add_argument("--min-gamma", type=float, default=14.0, help="min amplified gamma (uA/bit) to accept a layout")
    ap.add_argument("--layout-tries", type=int, default=30)
    ap.add_argument("--require-bit31", action="store_true",
                    help="also require PA bit31=1 (16GB Pi only; do NOT use on 2GB machines)")
    ap.add_argument("--arm", type=int, default=0,
                    help="gadget arm (SPEC_SET_ARM): 0=ARM_REF, 9=ARM_I(spec), etc. (test other gadgets)")
    ap.add_argument("--anchor-k", type=int, default=None,
                    help="ARM_T ALU-delay window length (insmod anchor_k=K + SET_ANCHOR_K)")
    args = ap.parse_args()

    global _ANCHOR_K
    _ANCHOR_K = args.anchor_k
    secret = parse_secret(args.secret_hex)
    anchor_g = bytes(LINE_BYTES)  # all-zero anchor (HW(0^V)=const drift reference)

    start_idx, seg = 0, 0
    if args.resume and os.path.exists(args.out):
        last, last_seg = last_index(args.out)
        start_idx = last + 1
        seg = last_seg + 1   # module reload shifts the baseline -> new detrend segment
        print(f"  [resume] existing CSV ends at idx={last} seg={last_seg}; continuing from {start_idx} as seg={seg}", flush=True)

    print(f"  REAL-VALUE CPA  V=64 distinct bytes (non-redundant)  N={args.n_traces}", flush=True)
    print(f"  secret={secret.hex()}", flush=True)
    print(f"  g-mode={args.g_mode}  anchor_every={args.anchor_every}  sample={args.sample_dur}s  start_idx={start_idx}", flush=True)

    # load module, securing the strong DRAM channel (PA bit31=1) when requested
    achieved_pa, achieved_gamma = None, None
    if args.strong_layout:
        achieved_pa, achieved_gamma = secure_strong_layout(args.min_gamma, args.layout_tries,
                                                           require_bit31=args.require_bit31, arm=args.arm)
    else:
        _load_module(); achieved_pa = ioctl_getpa(0)

    ioctl_buf(SET_V, secret)
    ioctl_u32(SET_ARM, args.arm)  # 0=ARM_REF (gadget_ref: LDP+cacheable+civac, G,G,V,V)
    if args.anchor_k is not None:
        ioctl_u32(SET_ANCHOR_K, args.anchor_k)
    print(f"  [init] secret sealed; ARM_REF  PA=0x{achieved_pa:x} bit31={(achieved_pa>>31)&1} "
          f"gamma={achieved_gamma}", flush=True)

    meta = dict(secret_hex=secret.hex(), n_traces=args.n_traces, seed=args.seed,
                anchor_every=args.anchor_every, sample_dur=args.sample_dur,
                g_mode=args.g_mode, gadget="v11_ARM_REF", line_bytes=LINE_BYTES,
                temp_max=args.temp_max, temp_resume=args.temp_resume,
                pa_cache=hex(achieved_pa) if achieved_pa is not None else None,
                pa_bit31=((achieved_pa >> 31) & 1) if achieved_pa is not None else None,
                layout_gamma=achieved_gamma)
    with open(args.out + ".meta.json", "w") as mf:
        json.dump(meta, mf, indent=2)

    # RNG: replay to start_idx so a resumed run continues the SAME g stream
    rng = random.Random(args.seed)
    for idx in range(start_idx):
        if idx % args.anchor_every != 0:
            gen_g(rng, args.g_mode)

    # thermal self-check: confirm the sensor is alive before trusting the gate
    t_probe = measure_temp()
    if math.isnan(t_probe):
        print("  [WARN] measure_temp() returned NaN at startup — thermal protection may be INERT.", flush=True)
    else:
        print(f"  [thermal] sensor OK, T={t_probe:.1f}C  (gate {args.temp_resume}->{args.temp_max})", flush=True)

    print(f"  [warmup] {args.warmup}s at g=0x00", flush=True)
    ioctl_buf(SET_G, anchor_g); ioctl_op(START)
    time.sleep(args.warmup); ioctl_op(STOP)

    n_cool, nan_temp = 0, 0
    mode_w = "a" if (args.resume and start_idx > 0) else "w"
    with open(args.out, mode_w) as fp:
        w = csv.writer(fp)
        if mode_w == "w":
            w.writerow(["order_idx","time_s","seg","is_anchor","g_hex",
                        "mean_VDD2_mA","std_VDD2_mA","mean_VDDQ_mA","std_VDDQ_mA",
                        "mean_CORE_mA","std_CORE_mA","temp_C","n_samp"])
        t0 = time.time()
        for idx in range(start_idx, args.n_traces):
            is_anchor = (idx % args.anchor_every == 0)

            # thermal gate (checked at anchor cadence; gadget idle while cooling)
            tnow = measure_temp() if (is_anchor or idx % 16 == 0) else float('nan')
            if math.isnan(tnow) and (is_anchor or idx % 16 == 0):
                nan_temp += 1
            if not math.isnan(tnow) and tnow >= args.temp_max:
                ioctl_op(STOP)
                print(f"  [thermal] T={tnow:.1f}C >= {args.temp_max} -> cooling to {args.temp_resume}", flush=True)
                cool_t0 = time.time()
                while True:
                    time.sleep(args.cool_poll)
                    tc = measure_temp()
                    # NaN = "still hot" (never resume on unknown temp)
                    if (not math.isnan(tc)) and tc <= args.temp_resume:
                        print(f"  [thermal] resumed at T={tc:.1f}C", flush=True); break
                    if time.time() - cool_t0 > args.cool_max_wait:
                        print(f"  [thermal] max wait {args.cool_max_wait}s exceeded (T={tc}); resuming", flush=True); break
                n_cool += 1; seg += 1
                tnow = measure_temp()

            g = anchor_g if is_anchor else gen_g(rng, args.g_mode)
            ioctl_op(STOP)
            ioctl_buf(SET_G, g)
            ioctl_op(START)
            vdd2s, vddqs, cores = [], [], []
            t1 = time.time()
            while time.time() - t1 < args.sample_dur:
                d = read_pmic()
                if d.get("DDR_VDD2_A_curr_mA") is not None: vdd2s.append(d["DDR_VDD2_A_curr_mA"])
                if d.get("DDR_VDDQ_A_curr_mA") is not None: vddqs.append(d["DDR_VDDQ_A_curr_mA"])
                if d.get("VDD_CORE_A_curr_mA") is not None: cores.append(d["VDD_CORE_A_curr_mA"])
                time.sleep(args.sample_interval)
            ioctl_op(STOP)
            mv2, sv2 = mstd(vdd2s); mvq, svq = mstd(vddqs); mc, sc = mstd(cores)
            tc = tnow if not math.isnan(tnow) else (measure_temp() if idx % 16 == 0 else float('nan'))
            ts = time.time() - t0
            w.writerow([idx, f"{ts:.2f}", seg, int(is_anchor), g.hex(),
                        f"{mv2:.4f}", f"{sv2:.4f}", f"{mvq:.4f}", f"{svq:.4f}",
                        f"{mc:.4f}", f"{sc:.4f}",
                        f"{tc:.1f}" if not math.isnan(tc) else "", len(vdd2s)])
            fp.flush()
            if idx % 100 == 0:
                tdisp = f"{tc:.1f}" if not math.isnan(tc) else "--"
                done = idx - start_idx + 1
                eta = (ts/max(done,1))*(args.n_traces-idx)/3600
                print(f"  idx={idx}/{args.n_traces}  VDD2={mv2:.2f}  T={tdisp}C  seg={seg} "
                      f"cool={n_cool} nanT={nan_temp}  t={ts/60:.1f}min  ETA={eta:.1f}h", flush=True)
    ioctl_op(STOP)
    if nan_temp > 0:
        print(f"  [WARN] {nan_temp} temp reads were NaN — verify thermal protection held.", flush=True)
    print(f"Done. CSV: {args.out}  (cooldowns={n_cool})", flush=True)

if __name__ == "__main__":
    main()
