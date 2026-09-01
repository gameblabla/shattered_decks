#!/usr/bin/env python3
"""Read the MSX2 blind-play probe out of an `openmsx snap` dump.

The bundled openmsx-headless can inject scripted keyboard input and can dump the
whole machine: `openmsx snap rom.rom N` writes a file containing the CPU state,
64 KiB of RAM and 128 KiB of VRAM.  The ROM stamps a magic-tagged struct into
RAM every frame (src/msx2/msx2_probe.c); this tool finds it and prints it.

That is the entire observation channel for milestone 1: the game is played
completely blind, and this is how we see what happened.

Usage:
    tools/msx2/read_probe.py src/msx2/out/waifu_msx2.snap [--json]
"""

import json
import struct
import sys

MAGIC = b"M2PB"
EXPECT_VERSION = 2

STATUS = {0: "OK", 1: "STUCK (duel exceeded step watchdog)", 2: "BADSTATE (invariant failed)"}
PHASE = {0: "TURN_START", 1: "MAIN", 2: "BATTLE", 3: "TURN_END", 4: "RESULT"}
SCENE = {0: "TITLE", 1: "DUEL"}

# Layout of struct Msx2Probe.  SDCC packs structs without padding on z80, and
# every member here is already naturally ordered, so this is a straight read.
LAYOUT = "<4sBB" + "HHHHHH" + "BBb" + "hh" + "BB" + "5s5s5s5s" + "HHH" + "BB" + "H" + "BB"
FIELDS = [
    "magic", "version", "status",
    "frame", "steps", "duels_done", "wins_player", "wins_com", "turns",
    "phase", "turn_owner", "result",
    "lp_player", "lp_com",
    "deck_player", "deck_com",
    "field_player", "field_com", "hand_player", "hand_com",
    "data_end", "sp", "ram_free", "scene", "menu_cursor", "checksum", "stage", "pad",
]
SIZE = struct.calcsize(LAYOUT)


def parse(blob, offset):
    values = struct.unpack_from(LAYOUT, blob, offset)
    probe = dict(zip(FIELDS, values))
    for key in ("field_player", "field_com", "hand_player", "hand_com"):
        probe[key] = list(probe[key])
    # The checksum covers everything before the checksum field itself; `stage`
    # and the pad byte follow it and are written asynchronously.
    body = blob[offset:offset + SIZE - 4]
    probe["checksum_ok"] = (sum(body) & 0xFFFF) == probe["checksum"]
    return probe


def find(blob):
    """Return every probe-looking struct in the dump.

    The ROM alternates between two slots, so a dump taken mid-update still
    contains one intact copy; the caller keeps the newest that checksums.
    """
    found = []
    start = 0
    while True:
        at = blob.find(MAGIC, start)
        if at < 0:
            return found
        start = at + 1
        if at + SIZE > len(blob):
            continue
        probe = parse(blob, at)
        if probe["version"] != EXPECT_VERSION:
            continue
        probe["offset"] = at
        found.append(probe)


def card(v):
    return "--" if v == 0xFF else str(v)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    as_json = "--json" in sys.argv[1:]
    if len(args) != 1:
        sys.exit(__doc__)

    blob = open(args[0], "rb").read()
    hits = sorted((p for p in find(blob) if p["checksum_ok"]),
                  key=lambda p: p["frame"], reverse=True)
    if not hits:
        raw = find(blob)
        if raw:
            sys.exit("%d probe slot(s) found (first at 0x%04X) and none checksums "
                     "-- the struct layout changed: bump MSX2_PROBE_VERSION and "
                     "this script together" % (len(raw), raw[0]["offset"]))
        sys.exit("no probe found in %s: the ROM never reached Msx2_ProbeInit" % args[0])

    p = hits[0]
    if as_json:
        p.pop("magic")
        print(json.dumps(p, indent=2))
        return

    print("probe        @ 0x%04X (RAM)  version %d" % (p["offset"], p["version"]))
    print("status       %s   (main stage %d)" % (STATUS.get(p["status"], "? %d" % p["status"]), p["stage"]))
    print("scene        %s%s"
          % (SCENE.get(p["scene"], "? %d" % p["scene"]),
             "" if p["menu_cursor"] == 0xFF else "   menu row %d" % p["menu_cursor"]))
    print("frame        %d" % p["frame"])
    print("duels done   %d   player %d  /  com %d"
          % (p["duels_done"], p["wins_player"], p["wins_com"]))
    print("current duel turn %d, phase %s, %s to act, steps %d"
          % (p["turns"], PHASE.get(p["phase"], p["phase"]),
             "COM" if p["turn_owner"] else "player", p["steps"]))
    print("LP           player %5d   com %5d" % (p["lp_player"], p["lp_com"]))
    print("deck left    player %5d   com %5d" % (p["deck_player"], p["deck_com"]))
    print("field        player %-20s com %s"
          % (" ".join(card(c) for c in p["field_player"]),
             " ".join(card(c) for c in p["field_com"])))
    print("hand         player %-20s com %s"
          % (" ".join(card(c) for c in p["hand_player"]),
             " ".join(card(c) for c in p["hand_com"])))
    print("RAM          data ends 0x%04X, SP 0x%04X, %d bytes free between them"
          % (p["data_end"], p["sp"], p["ram_free"]))

    if p["status"] != 0:
        sys.exit(1)
    if p["scene"] == 0:
        # Sitting on the title is a legitimate end state for a `shot` run; it is
        # only a failure when the run was supposed to be playing.
        print("\nNOTE: the run ended on the title screen, so no duel was played.")
        return
    if p["duels_done"] == 0 and p["frame"] > 120:
        print("\nWARNING: %d frames and not one duel has finished." % p["frame"])
        sys.exit(1)


if __name__ == "__main__":
    main()
