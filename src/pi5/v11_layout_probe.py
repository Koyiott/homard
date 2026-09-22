#!/usr/bin/env python3
"""v11_layout_probe.py — find WHY leakage amplitude (gamma) varies across module
reloads.  Each reload gets a fresh kbuf_cache allocation; we log its physical
address (bank/row/col/channel bits) and a fast AMPLIFIED leakage swing
(redundant V=byte x64, so swing = 512 * gamma_per_bit -> mA-scale, measurable in
seconds).  Output: which PA features predict a STRONG layout (high gamma).
"""
import fcntl, os, struct, subprocess, time, sys, math, argparse, csv

DEV="/dev/spec_power_p2_v11"; MODULE="spec_power_p2_v11"
KO=os.environ.get("V11_KO","/home/pi/spectre_multi/phase2/spec_power_p2_v11.ko"); MAGIC=0xF1; LB=64
def _IOW(n,sz): return 0x40000000|(sz<<16)|(MAGIC<<8)|n
def _IO(n): return (MAGIC<<8)|n
def _IOWR(n,sz): return 0xC0000000|(sz<<16)|(MAGIC<<8)|n
SET_V=_IOW(1,LB); SET_G=_IOW(2,LB); START=_IO(3); STOP=_IO(4); SET_ARM=_IOW(21,4); GET_PA=_IOWR(30,16)
def op(c):
    fd=os.open(DEV,os.O_RDWR); fcntl.ioctl(fd,c,0); os.close(fd)
def buf(c,b):
    fd=os.open(DEV,os.O_RDWR); fcntl.ioctl(fd,c,b); os.close(fd)
def u32(c,v):
    fd=os.open(DEV,os.O_RDWR); fcntl.ioctl(fd,c,v); os.close(fd)
def getpa(off):
    fd=os.open(DEV,os.O_RDWR); b=struct.pack("QQ",off,0); r=fcntl.ioctl(fd,GET_PA,b); os.close(fd)
    return struct.unpack("QQ",r)[1]
def vdd2():
    o=subprocess.run(["vcgencmd","pmic_read_adc"],capture_output=True,text=True).stdout
    for L in o.splitlines():
        if "DDR_VDD2_A current" in L:
            return float(L.split("=")[1].rstrip("A"))*1000.0
    return float('nan')
def temp():
    o=subprocess.run(["vcgencmd","measure_temp"],capture_output=True,text=True).stdout
    try: return float(o.split("=")[1].rstrip("'C\n"))
    except: return float('nan')
def bit(pa,i): return (pa>>i)&1
def bank_paper(pa): return ((bit(pa,12)^bit(pa,33))<<3)|((bit(pa,31)^bit(pa,32))<<2)|(bit(pa,31)<<1)|bit(pa,12)
def row_id(pa): return (pa>>14)&((1<<17)-1)
def col_id(pa): return (pa>>6)&0x3f

def meas(gbyte, samples, dur=2.0):
    buf(SET_G, bytes([gbyte])*LB); op(START)
    xs=[]; t1=time.time()
    while time.time()-t1<dur:
        v=vdd2()
        if not math.isnan(v): xs.append(v)
        time.sleep(0.08)
    op(STOP)
    return sum(xs)/len(xs) if xs else float('nan')

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--trials",type=int,default=20)
    ap.add_argument("--vbyte",type=lambda s:int(s,0),default=0xFF)
    ap.add_argument("--dur",type=float,default=2.0)
    ap.add_argument("--out",default="layout_probe.csv")
    ap.add_argument("--pressure",action="store_true",help="touch memory between reloads to vary allocation")
    ap.add_argument("--arm",type=int,default=0,help="gadget arm (SPEC_SET_ARM): 0=ARM_REF, etc.")
    args=ap.parse_args()
    w=csv.writer(open(args.out,"w"))
    w.writerow(["trial","pa0","pa64","bank","row","col0","col64","ch31","ch32","ch33",
                "samepage","vdd2_hw512","vdd2_hw0","swing_mA","gamma_uA_bit","temp_C"])
    print(f"probe: {args.trials} reloads, V=0x{args.vbyte:02X}x64, swing=VDD2(G=0)-VDD2(G=0xFF)", flush=True)
    for tr in range(args.trials):
        for m in ["spec_power_p2_min13","spec_power_p2_min12","spec_power_p2_min11",MODULE]:
            subprocess.run(["sudo","rmmod",m],capture_output=True)
        if args.pressure:
            # dirty some anonymous pages to perturb the page allocator
            _=bytearray(64*1024*1024); _[::4096]=b"\x01"*len(_[::4096])
            del _
        time.sleep(0.3)
        r=subprocess.run(["sudo","insmod",KO,"n_workers=1"],capture_output=True,text=True)
        if r.returncode!=0:
            print("insmod failed",r.stderr); continue
        for _ in range(20):
            if os.path.exists(DEV): break
            time.sleep(0.1)
        pa0=getpa(0); pa64=getpa(64)
        buf(SET_V, bytes([args.vbyte])*LB); u32(SET_ARM,args.arm)
        hi=meas(0x00,None,args.dur)              # HW(0^FF)=8/byte -> 512 total
        lo=meas(0xFF,None,args.dur)              # HW(FF^FF)=0
        swing=hi-lo; gamma=swing/512*1000.0      # uA per bit
        tc=temp()
        sp=1 if (pa0>>12)==(pa64>>12) else 0
        row=[tr,hex(pa0),hex(pa64),bank_paper(pa0),hex(row_id(pa0)),col_id(pa0),col_id(pa64),
             bit(pa0,31),bit(pa0,32),bit(pa0,33),sp,f"{hi:.3f}",f"{lo:.3f}",f"{swing:.3f}",
             f"{gamma:.2f}",f"{tc:.1f}"]
        w.writerow(row)
        print(f"  tr{tr:2d} pa0={hex(pa0)} bank={bank_paper(pa0):2d} ch[{bit(pa0,31)}{bit(pa0,32)}{bit(pa0,33)}] "
              f"row=0x{row_id(pa0):x} swing={swing:5.2f}mA gamma={gamma:5.2f}uA/bit T={tc:.0f}", flush=True)
    op(STOP)
    print("Done.", flush=True)

if __name__=="__main__":
    main()
