#!/usr/bin/env python3
"""Cycle profile of the SNES debug ROM over a window of emulated fields.

    python3 tools/snes/prof.py NAME START END [BUTTON@FIELD ...]

runs the debug ROM through the Random Battle menu path (verify.py's script)
with the extra presses, profiles fields START..END with the cycle profiler
in SNES/snes-headless (tools/snes/cycle_profile.patch) and prints the master
cycles per routine, mapped through build/snes/waifusnes_debug.sym.  A field
is 357,366 master cycles; WRAM costs eight a byte, ROM and the DMA registers
six, so the cycles-per-instruction column says what a loop is waiting on.
E.g. `prof.py lift 2400 2440 R@2200 UP@2400` is the first pose of the lift
over the fixture board, `prof.py rest 2500 2599 R@2200` a resting frame."""
import os, subprocess, sys, re, bisect
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools", "snes"))
import verify
EMU = os.path.join(ROOT, "SNES", "snes-headless", "snes-mednafen")
name, start, end = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
script = verify.random_battle_script()
for b in sys.argv[4:]:
    btn, at = b.split("@"); script.append(verify.press(btn, int(at)))
out=os.path.join(ROOT,"build/snes/verify"); os.makedirs(out+"/sav",exist_ok=True)
inp=os.path.join(out,name+".prof.in.txt")
open(inp,"w").write("".join("%d %d %d 0\n"%r for r in script))
prof=os.path.join(out,name+".prof.txt")
env=dict(os.environ, SNES_PROFILE="%d:%d:%s"%(start,end,prof))
r=subprocess.run([EMU,"script",verify.ROM,inp,str(end+5),out+"/"+name+".prof.ppm",out+"/"+name+".prof.wram.bin"],
                 cwd=out, env=env, capture_output=True, text=True, timeout=600)
if r.returncode: print(r.stderr[-2000:]); sys.exit(1)
# symbols
syms=[]
for line in open(ROOT+"/build/snes/waifusnes_debug.sym"):
    m=re.match(r"([0-9a-f]{8}) (\S+)",line)
    if not m: continue
    a=int(m.group(1),16); n=m.group(2)
    if n.startswith("SECTION") or n.startswith("__local") or n.startswith("_") and n[1:2].isdigit(): continue
    syms.append((a,n))
syms.sort(); addrs=[a for a,_ in syms]
def sym(pc):
    i=bisect.bisect_right(addrs,pc)-1
    return syms[i][1] if i>=0 else "?"
tot=0; bysym={}; instr={}
head=None
for line in open(prof):
    if line.startswith("#"): head=line.strip(); continue
    pc,cnt,cyc=line.split(); pc=int(pc,16); cnt=int(cnt); cyc=int(cyc)
    s=sym(pc); tot+=cyc
    d=bysym.setdefault(s,[0,0]); d[0]+=cyc; d[1]+=cnt
print(head, "fields=%d  total master cycles=%d (%.1f fields of 357,366)"%(end-start+1,tot,tot/357366.0))
for s,(cyc,cnt) in sorted(bysym.items(), key=lambda kv:-kv[1][0])[:45]:
    print("%7.2f%% %12d cyc %10d instr %5.2f cyc/instr  %s"%(100.0*cyc/tot,cyc,cnt,cyc/max(cnt,1),s))
