#!/usr/bin/env python3
"""Measure real openMSX camera presentation; never builds or loads a savestate.

Run after make -f Makefile.msx2 soak for populated turns, or after a shipping
build for the empty opening. Uses linked symbols from that exact ROM/map pair.
Outputs per-frame CSV, machine/build metadata, raw VRAM and summary JSON.
"""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import re
import statistics
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--seconds',type=float,default=150)
    ap.add_argument('--machine',default='C-BIOS_MSX2')
    ap.add_argument('--openmsx',default='/usr/local/bin/openmsx')
    ap.add_argument('--out',type=Path,default=ROOT/'artifacts/msx2-floor-bench')
    ap.add_argument('--timeout',type=float,default=600)
    args = ap.parse_args()
    out = args.out.resolve(); out.mkdir(parents=True,exist_ok=True)
    rom = ROOT/'src/msx2/out/waifu_msx2.rom'
    mapfile = rom.with_suffix('.map')
    symbols = {name:int(at,16) for at,name in re.findall(r'([0-9A-F]{8})\s+(_[A-Za-z0-9_]+)',mapfile.read_text())}
    wanted = {'camera':'_Msx2_BoardStepCameraMove','arena':'_Msx2_ArenaDrawStep',
              'present':'_Msx2_VideoPresent','pose':'_g_msx2_arena_pose'}
    addr = {key:symbols[name]&65535 for key,name in wanted.items()}
    image = rom.read_bytes()
    # Verify mapped entry bytes in every breakpoint condition: page-0 modal
    # code shares these addresses and must never produce false timing samples.
    def guard(key):
        address = symbols[wanted[key]]
        physical = ((address>>16)*16384 + (address&65535)) if address>>16 else (address-0x4000)
        sig = image[physical:physical+8].hex()
        return f'[binary encode hex [debug read_block memory {addr[key]} 8]] eq "{sig}"'
    metadata = {'rom_sha256':hashlib.sha256(image).hexdigest(),
                'map_sha256':hashlib.sha256(mapfile.read_bytes()).hexdigest(),
                'machine':args.machine,'seconds':args.seconds,'symbols':addr,
                'emulator':subprocess.check_output([args.openmsx,'-v'],text=True).strip(),
                'variant':[p.name for p in rom.parent.glob('.variant-*')],
                'compiler_config':(ROOT/'src/msx2/project_config.js').read_text(),
                'scope':'emulated stock-machine timing; no physical hardware claim'}
    (out/'metadata.json').write_text(json.dumps(metadata,indent=2))
    tcl = r'''
set renderer none
set throttle off
set power on
set camera_scope 0
set pending 0
set frame_id 0
set step_start 0
set arena_ms 0
set csv [open frames.csv w]
puts $csv "frame,pose,start_s,present_s,arena_ms,complete_ms,reg1,reg2,reg8,reg9,ce"
proc on_return {kind action} {
    global bp
    set sp [expr {([reg SP]+2)&65535}]
    set ret [peek16 [reg SP]]
    set bp($kind) [debug set_bp $ret [format {[reg SP] == %d} $sp] [list returned $kind $action]]
}
proc returned {kind action} {
    global bp
    debug remove_bp $bp($kind)
    uplevel #0 $action
}
proc camera_enter {} {
    set ::camera_scope 1
    set ::step_start [machine_info time]
    on_return camera {set ::camera_scope 0}
}
proc arena_enter {} {
    if {!$::camera_scope} {return}
    set ::pending 1
    set ::arena_start [machine_info time]
    set ::pose [debug read memory POSE_ADDRESS]
    on_return arena {set ::arena_ms [expr {1000*([machine_info time]-$::arena_start)}]}
}
proc present_enter {} {
    if {$::pending} {on_return present frame_done}
}
proc frame_done {} {
    set ::pending 0
    set now [machine_info time]
    set ms [expr {1000*($now-$::step_start)}]
    puts $::csv "$::frame_id,$::pose,$::step_start,$now,$::arena_ms,$ms,[debug read {VDP regs} 1],[debug read {VDP regs} 2],[debug read {VDP regs} 8],[debug read {VDP regs} 9],[debug probe read VDP.commandExecuting]"
    flush $::csv
    set f [open [format "frame_%04d_pose_%02d.vram" $::frame_id $::pose] w]
    fconfigure $f -translation binary
    puts -nonewline $f [debug read_block {physical VRAM} 0 131072]
    close $f
    incr ::frame_id
}
after time 6.0 {keymatrixdown 8 1; after time 0.15 {keymatrixup 8 1}}
after time 6.8 {keymatrixdown 8 64; after time 0.15 {keymatrixup 8 64}}
after time 7.6 {keymatrixdown 8 1; after time 0.15 {keymatrixup 8 1}}
'''.replace('POSE_ADDRESS',str(addr['pose']))
    for key in ('camera','arena','present'):
        tcl += f'debug set_bp {addr[key]} {{{guard(key)}}} {{{key}_enter}}\n'
    tcl += f'after time {args.seconds} {{close $::csv; exit 0}}\n'
    script = out/'run.tcl'; script.write_text(tcl)
    with (out/'openmsx.log').open('w') as log:
        subprocess.run([args.openmsx,'-machine',args.machine,'-cart',str(rom),
                        '-romtype','NEO-16','-script',str(script)],cwd=out,
                       env={**os.environ,'SDL_VIDEODRIVER':'dummy','SDL_AUDIODRIVER':'dummy'},
                       stdout=log,stderr=subprocess.STDOUT,check=True,timeout=args.timeout)
    rows = list(csv.DictReader((out/'frames.csv').open()))
    if not rows:
        raise SystemExit('No completed camera frames: inspect openmsx.log; no fps claim possible.')
    for row in rows:
        stem = f"frame_{int(row['frame']):04d}_pose_{int(row['pose']):02d}"
        subprocess.run(['python3',str(ROOT/'tools/msx2/vram_png.py'),
                        str(out/(stem+'.vram')),str(out/(stem+'.png')),
                        '--page',str((int(row['reg2'])>>5)&1)],check=True)
    summary = {'completed_frames':len(rows),'busy_at_presentation':sum(int(r['ce']) for r in rows),'groups':{}}
    for name,group in [('opening',[r for r in rows if 2<=int(r['pose'])<18]),
                       ('turn',[r for r in rows if int(r['pose'])>=18])]:
        if not group: continue
        times = [float(r['complete_ms']) for r in group]
        intervals = [1000*(float(b['present_s'])-float(a['present_s'])) for a,b in zip(rows,rows[1:])
                     if a in group and b in group and 0<=float(b['start_s'])-float(a['present_s'])<.1]
        summary['groups'][name] = {'frames':len(group),'mean_work_ms':statistics.mean(times),
            'worst_work_ms':max(times),'work_rate_fps':1000/statistics.mean(times),
            'continuous_presented_fps':1000/statistics.mean(intervals) if intervals else None,
            'all_work_within_200ms':max(times)<=200}
    (out/'summary.json').write_text(json.dumps(summary,indent=2))
    print(json.dumps(summary,indent=2))

if __name__ == '__main__':
    main()
