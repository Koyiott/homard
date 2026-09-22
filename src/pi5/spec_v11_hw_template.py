#!/usr/bin/env python3
"""
spec_v11_hw_template.py — template the Hamming weight HW(V) of a target cache
line using the SAME Phase-2 speculative HD(G,V) gadget (v11 ARM_I), but with the
attacker line G FIXED to 0x00.

Since HD(G,V) = HW(G XOR V) and G = 0, the leakage collapses to HW(V):
    VDD2 ~ alpha * HW(V)            (core/activation cross-term, monotonic)
    VDDQ ~ DBI(V) shape            (I/O burst, V-inverted ∧ shape)
So with G pinned to zero the very same gadget that recovered HD(G,V) now profiles
the Hamming weight of ANY target line.

Protocol (Phase-1 template style, multi-line VDDQ amplifier / single worker):
  * arm = ARM_K (spec, reads 16 V cache lines/trigger), G = 0x00 x64 fixed.
  * sweep V over 9 redundant-byte HW classes {0,64,...,512} (byte = (1<<k)-1);
    SET_V replicates the pattern across all 16 amplifier lines.
  * many reps, pattern order randomized per rep; per-rep baseline removed offline.
  * record every PMIC sample (DDR_VDDQ, DDR_VDD2) as one CSV row.
CSV columns match analyze_multi.load(): rep,num_ones,DDR_VDDQ_A_curr_mA,DDR_VDD2_A_curr_mA
Run ON the Pi as root.
"""
import csv, fcntl, math, os, random, subprocess, sys, time

DEV    = "/dev/spec_power_p2_v11"
MODULE = "spec_power_p2_v11"
KO     = f"/home/pi/spectre_multi/phase2/{MODULE}.ko"
MAGIC  = 0xF1
LINE   = 64
def _IOW(n,sz): return 0x40000000|(sz<<16)|(MAGIC<<8)|n
def _IO(n):     return (MAGIC<<8)|n
SET_V=_IOW(1,LINE); SET_G=_IOW(2,LINE); START=_IO(3); STOP=_IO(4); SET_ARM=_IOW(21,4)

ARM_I    = 9                       # best speculative gadget (both-spec, civac-every)
ARM_J    = 10                      # V-first variant (spec loads V then G)
ARM_K    = 11                      # multi-line VDDQ amplifier (16 V-lines, spec)
ARM_SEL  = ARM_K                   # which gadget to use for HW(V) templating
G_FIX    = 0x00                    # G pinned to zero -> HD(G,V) = HW(V)
# Redundant-byte V: all 64 bytes identical = (1<<k)-1, byte HW k=0..8.
# vline(64*k) emits exactly that (bit k spread across all 64 bytes first).
# => line HW in {0,64,...,512}, 9 classes, each a repeated byte.
HW_CLASSES = list(range(0, 513, 64))   # 9 classes: 0,64,...,512 (redundant byte)
REPS     = 60
SAMPLE_S = 2.0
SAMPLE_DT= 0.10
INTER    = 0.15
WARMUP_S = 4.0
COOL_C   = 54
HOT_C    = 64
COOL_MAX_WAIT = 300
SEED     = 0x4857B11E
N_WORKERS= 1                       # single worker (realistic, per user)
OUT_DIR  = "/home/pi/spectre_multi/phase2/data/V11/HWTPL"
CONTAM   = ["spec_power_p2_v10","spec_power_p2_min13","spec_power_p2_min12",
            "spec_power_p2_min11","spec_power_p2_min10"]

def ioctl_op(c):    fd=os.open(DEV,os.O_RDWR); fcntl.ioctl(fd,c,0); os.close(fd)
def ioctl_buf(c,b): fd=os.open(DEV,os.O_RDWR); fcntl.ioctl(fd,c,b); os.close(fd)
def ioctl_u32(c,v): fd=os.open(DEV,os.O_RDWR); fcntl.ioctl(fd,c,v); os.close(fd)
def rep_line(b):    return bytes([b]*LINE)

def vline(h):
    """64-byte line with exactly h bits set, spread evenly over bytes & bit positions."""
    b = bytearray(LINE)
    for j in range(h):
        b[j % LINE] |= (1 << ((j // LINE) % 8))
    return bytes(b)

def popcnt_line(bs): return sum(bin(x).count("1") for x in bs)

def temp():
    o=subprocess.run(["vcgencmd","measure_temp"],capture_output=True,text=True).stdout
    try: return float(o.split("=")[1].split("'")[0])
    except: return float("nan")
def throttled():
    return subprocess.run(["vcgencmd","get_throttled"],capture_output=True,text=True).stdout.strip()
def contaminated():
    for m in CONTAM:
        if os.path.exists("/sys/module/"+m): return m
    return None
def cool_to(thr, max_wait=COOL_MAX_WAIT):
    t=temp(); t0=time.time()
    while not math.isnan(t) and t>thr:
        if time.time()-t0 > max_wait:
            print(f"  [cool] gave up after {max_wait}s at {t:.1f}C; proceeding", flush=True); break
        print(f"  [cool] {t:.1f}C > {thr}C, waiting...", flush=True); time.sleep(20); t=temp()
    print(f"  [cool] {t:.1f}C ok", flush=True)

def read_vdd2_vddq():
    o=subprocess.run(["vcgencmd","pmic_read_adc"],capture_output=True,text=True).stdout
    v2=vq=float("nan")
    for L in o.splitlines():
        if "DDR_VDD2_A current" in L:
            try: v2=float(L.split("=")[1].rstrip("A"))*1000.0
            except: pass
        elif "DDR_VDDQ_A current" in L:
            try: vq=float(L.split("=")[1].rstrip("A"))*1000.0
            except: pass
    return v2,vq

def measure(v_bytes):
    """STOP, install V, START, collect PMIC samples for SAMPLE_S; return list of (vddq,vdd2)."""
    ioctl_op(STOP); ioctl_buf(SET_V, v_bytes); ioctl_op(START)
    out=[]; t0=time.time()
    while time.time()-t0 < SAMPLE_S:
        v2,vq=read_vdd2_vddq()
        if not (math.isnan(v2) or math.isnan(vq)): out.append((vq,v2))
        time.sleep(SAMPLE_DT)
    ioctl_op(STOP)
    return out

def system_init():
    for mod in CONTAM+[MODULE]:
        subprocess.run(["sudo","rmmod",mod],capture_output=True)
    time.sleep(0.2)
    r=subprocess.run(["sudo","insmod",KO,f"n_workers={N_WORKERS}"],capture_output=True,text=True)
    if r.returncode!=0: sys.stderr.write(f"insmod failed: {r.stderr}\n"); sys.exit(1)
    for _ in range(20):
        if os.path.exists(DEV): break
        time.sleep(0.1)
    ioctl_op(STOP); time.sleep(0.1)
    ioctl_u32(SET_ARM, ARM_SEL)          # speculative gadget (ARM_SEL)
    ioctl_buf(SET_G, rep_line(G_FIX))    # G pinned to 0 -> HD = HW(V)
    print(f"  [init] ARM{ARM_SEL}(spec) armed, G=0x{G_FIX:02X} x64 fixed (single worker)", flush=True)

def main():
    global REPS, SAMPLE_S, N_WORKERS
    if len(sys.argv) > 1: REPS = int(sys.argv[1])         # quick smoke: pass small REPS
    if len(sys.argv) > 2: SAMPLE_S = float(sys.argv[2])
    if len(sys.argv) > 3: N_WORKERS = int(sys.argv[3])
    if len(sys.argv) > 4:
        global_arm = int(sys.argv[4]); globals()['ARM_SEL'] = global_arm
    if len(sys.argv) > 5:                                   # HW step: 64=>9 classes, 32=>17
        globals()['HW_CLASSES'] = list(range(0, 513, int(sys.argv[5])))
    os.makedirs(OUT_DIR, exist_ok=True)
    stamp=time.strftime("%Y%m%d_%H%M%S")
    out=os.path.join(OUT_DIR, f"hwtpl_v11_{stamp}.csv")
    print(f"  out={out}\n  classes={HW_CLASSES}  reps={REPS}  sample={SAMPLE_S}s", flush=True)
    system_init()
    # verify patterns
    for h in HW_CLASSES:
        assert popcnt_line(vline(h))==h, f"pattern HW mismatch at {h}"
    fp=open(out,"w",newline=""); w=csv.writer(fp)
    w.writerow(["rep","num_ones","DDR_VDDQ_A_curr_mA","DDR_VDD2_A_curr_mA","t_s","temp_C"]); fp.flush()

    # warmup (hammer HW=256 a few seconds)
    cool_to(COOL_C)
    ioctl_op(STOP); ioctl_buf(SET_V, vline(256)); ioctl_op(START); tw=time.time()
    while time.time()-tw < WARMUP_S: time.sleep(0.3)
    ioctl_op(STOP)

    t0=time.time(); rng=random.Random(SEED); nsamp=0
    for rep in range(REPS):
        tc=temp()
        if tc>HOT_C:
            print(f"  [rep {rep}] {tc:.1f}C>{HOT_C} -> cool", flush=True); cool_to(COOL_C)
        order=HW_CLASSES[:]; rng.shuffle(order)
        for h in order:
            c=contaminated()
            if c:
                print(f"  !! CONTAMINATION {c} at rep {rep} -> ABORT", flush=True)
                ioctl_op(STOP); fp.close(); sys.exit(2)
            samples=measure(vline(h)); t=time.time()-t0; tt=temp()
            for (vq,v2) in samples:
                w.writerow([rep,h,f"{vq:.4f}",f"{v2:.4f}",f"{t:.2f}",f"{tt:.1f}"]); nsamp+=1
            fp.flush(); time.sleep(INTER)
        if rep%5==0 or rep==REPS-1:
            print(f"  rep {rep:3d}/{REPS}  T={temp():.1f}C  nsamp={nsamp}  "
                  f"t={(time.time()-t0)/60:.1f}m", flush=True)
    ioctl_op(STOP); fp.close()
    print(f"\nDone. {nsamp} samples -> {out}", flush=True)
    print(f"  throttled={throttled()}", flush=True)

if __name__=="__main__":
    main()
