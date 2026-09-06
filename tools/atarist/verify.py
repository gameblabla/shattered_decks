#!/usr/bin/env python3
"""Boot the Atari ST floppy headlessly and read the port's probe back.

The headless Hatari MCP build cannot press a key, so a blind run is judged the
way the other blind targets are judged: the game stamps `AtaristProbe` into RAM
every frame and this locates it by scanning a dump for the magic.  A screenshot
is taken alongside, because a probe that says "frame 300" and a black screen is
still a failure.

    python3 tools/atarist/verify.py --image build/atarist/waifu.st
    python3 tools/atarist/verify.py --image ... --script "0:60,START:2,A:30"
"""

import argparse
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hatari_mcp import Hatari, TOS  # noqa: E402

MAGIC = 0x57465354      # 'WFST'
MAGIC_END = 0x454E4421  # 'END!'
SCRIPT_SLOTS = 128

BUTTONS = {
    "UP": 0x0001, "DOWN": 0x0002, "LEFT": 0x0004, "RIGHT": 0x0008,
    "A": 0x0010, "B": 0x0020, "START": 0x0040, "TAB": 0x0080, "QUIT": 0x0100,
    "0": 0x0000, "NONE": 0x0000,
}

STAGES = ["ENTRY", "VIDEO", "INPUT", "AUDIO", "ASSETS", "LOOP",
          "TITLE", "DUEL", "STORY", "EXIT"]
STATUS = ["OK", "BADSTATE", "STUCK", "NOMEM", "NOFILE"]

# struct AtaristProbe, big endian, up to (but not including) the script array.
HEAD_FMT = ">IHH II HH HHHHH hh HH HHH HHHH HH"
HEAD_SIZE = struct.calcsize(HEAD_FMT)
HEAD_FIELDS = [
    "magic", "version", "stage", "frame", "vbl", "frame_vbls", "status",
    "scene", "menu_cursor", "duels", "wins_player", "wins_com",
    "lp_player", "lp_com", "turns", "phase",
    "is_ste", "has_blitter", "machine_ram_kb",
    "script_len", "script_pos", "script_hold", "mark",
    "worst_vbls", "spare",
]
PROBE_SIZE = HEAD_SIZE + SCRIPT_SLOTS * 4 + 4

# Byte offset of a named head field.  Computed rather than written down: the
# script_len poke used to be spelled HEAD_SIZE - 8, and adding two fields to
# the probe silently redirected it at `mark`, so the scripted input stopped
# arriving and every measurement looked idle.
def head_offset(name):
    return struct.calcsize(">" + "".join(
        HEAD_FMT.replace(" ", "")[1:][:HEAD_FIELDS.index(name)]))


def find_probe(ram, base=0):
    """Return (address, decoded dict) for the first valid probe in `ram`."""
    needle = struct.pack(">I", MAGIC)
    start = 0
    while True:
        i = ram.find(needle, start)
        if i < 0:
            return None, None
        start = i + 4
        if i + PROBE_SIZE > len(ram):
            continue
        if struct.unpack_from(">I", ram, i + PROBE_SIZE - 4)[0] != MAGIC_END:
            continue
        values = struct.unpack_from(HEAD_FMT, ram, i)
        probe = dict(zip(HEAD_FIELDS, values))
        probe["_addr"] = base + i
        return base + i, probe


def parse_script(text):
    """"START:2,A:30" -> [(mask, frames), ...] packed as (frames<<16)|mask."""
    out = []
    for item in text.split(","):
        item = item.strip()
        if not item:
            continue
        name, _, frames = item.partition(":")
        frames = int(frames or 1)
        mask = 0
        for part in name.split("+"):
            part = part.strip().upper()
            if part not in BUTTONS:
                raise SystemExit("unknown button %r" % part)
            mask |= BUTTONS[part]
        out.append((frames << 16) | mask)
    if len(out) > SCRIPT_SLOTS:
        raise SystemExit("script longer than %d entries" % SCRIPT_SLOTS)
    return out


def describe(p):
    stage = STAGES[p["stage"]] if p["stage"] < len(STAGES) else p["stage"]
    status = STATUS[p["status"]] if p["status"] < len(STATUS) else p["status"]
    return ("probe @ 0x%06x  stage=%s status=%s frame=%d vbl=%d "
            "frame_vbls=%d worst=%d scene=%d mark=%d spare=%d ste=%d blitter=%d ram=%dK"
            % (p["_addr"], stage, status, p["frame"], p["vbl"],
               p["frame_vbls"], p["worst_vbls"], p["scene"], p["mark"],
               p["spare"], p["is_ste"], p["has_blitter"], p["machine_ram_kb"]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", required=True)
    ap.add_argument("--machine", default="st")
    ap.add_argument("--tos", default=TOS)
    ap.add_argument("--boot-seconds", type=float, default=6.0)
    ap.add_argument("--run-seconds", type=float, default=4.0)
    ap.add_argument("--script", default="")
    ap.add_argument("--shot", default="build/atarist/shot.png")
    ap.add_argument("--dump", default="")
    ap.add_argument("--ram-kb", type=int, default=512)
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()

    scratch = os.environ.get("TMPDIR", "/tmp")
    dump_path = a.dump or os.path.join(scratch, "atarist_ram.bin")
    os.makedirs(os.path.dirname(os.path.abspath(a.shot)), exist_ok=True)

    h = Hatari(verbose=a.verbose)
    rc = 0
    try:
        h.start(a.image, machine=a.machine, tos=a.tos)
        time.sleep(a.boot_seconds)

        h.dump(0, a.ram_kb * 1024, dump_path)
        with open(dump_path, "rb") as f:
            ram = f.read()
        addr, probe = find_probe(ram)
        if addr is None:
            print("FAIL: no probe in RAM -- the game never reached its entry")
            h.screenshot(a.shot)
            return 2
        print(describe(probe))

        if a.script:
            entries = parse_script(a.script)
            payload = b"".join(struct.pack(">I", e) for e in entries)
            script_addr = addr + HEAD_SIZE
            h.poke(script_addr, payload)
            # script_len last, so the game never reads a half-written queue.
            h.poke(addr + head_offset("script_len"),
                   struct.pack(">H", len(entries)))
            print("script: %d entries at 0x%06x" % (len(entries), script_addr))

        first_frame = probe["frame"]
        time.sleep(a.run_seconds)

        h.dump(0, a.ram_kb * 1024, dump_path)
        with open(dump_path, "rb") as f:
            ram = f.read()
        addr2, probe2 = find_probe(ram)
        if addr2 is None:
            print("FAIL: the probe vanished between samples")
            rc = 2
        else:
            print(describe(probe2))
            if probe2["frame"] <= first_frame:
                print("FAIL: frame counter stalled (%d -> %d)"
                      % (first_frame, probe2["frame"]))
                rc = 1
            if probe2["status"] != 0:
                print("FAIL: probe status %s" % STATUS[probe2["status"]])
                rc = 1
        print(h.screenshot(a.shot).strip())
        if rc == 0:
            print("OK")
    finally:
        h.close()
    return rc


if __name__ == "__main__":
    sys.exit(main())
