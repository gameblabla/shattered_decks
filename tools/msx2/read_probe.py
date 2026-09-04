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

import argparse
import json
import os
import re
import struct
import sys

MAGIC = b"M2PB"
EXPECT_VERSION = 2

STATUS = {0: "OK", 1: "STUCK (duel exceeded step watchdog)", 2: "BADSTATE (invariant failed)"}
PHASE = {0: "TURN_START", 1: "MAIN", 2: "BATTLE", 3: "TURN_END", 4: "RESULT"}
SCENE = {0: "TITLE", 1: "DUEL", 2: "STORY"}

# Layout of struct Msx2Probe.  SDCC packs structs without padding on z80, and
# every member here is already naturally ordered, so this is a straight read.
BASE_LAYOUT = "<4sBB" + "HHHHHH" + "BBb" + "hh" + "BB" + "5s5s5s5s" + "HHH" + "BB" + "H" + "BB"
FIELDS = [
    "magic", "version", "status",
    "frame", "steps", "duels_done", "wins_player", "wins_com", "turns",
    "phase", "turn_owner", "result",
    "lp_player", "lp_com",
    "deck_player", "deck_com",
    "field_player", "field_com", "hand_player", "hand_com",
    "data_end", "sp", "ram_free", "scene", "menu_cursor", "checksum", "stage", "pad",
]
REG_FIELDS = [
    "board_mode", "board_view", "draw_page", "show_page", "deal_slot",
    "deal_step", "deal_reveal", "deal_target_mask", "deal_landed0",
    "deal_landed1", "hand_left", "hand_hidden", "camera_active", "gem_visible",
    "gem_x", "gem_y", "full_view_streams", "band_streams", "blank_pairs",
    "page_flips", "story_phase", "save_row", "music_track", "music_segment",
    "music_pointer", "music_frames", "music_loops", "music_errors",
    "initial_seed", "duel_seed", "entropy_sources",
]
REG_LAYOUT = "<" + "BBBBBBBBBB" + "BBBBBB" + "HHHH" + "BB" + "B" + "HHHHB" + "II" + "B"
LAYOUTS = {2: BASE_LAYOUT, 3: BASE_LAYOUT + REG_LAYOUT[1:]}
FIELDS_BY_VERSION = {2: FIELDS, 3: FIELDS + REG_FIELDS}
SIZES = {version: struct.calcsize(layout) for version, layout in LAYOUTS.items()}


def parse(blob, offset, version=None):
    if version is None:
        version = blob[offset + 4]
    layout = LAYOUTS[version]
    values = struct.unpack_from(layout, blob, offset)
    probe = dict(zip(FIELDS_BY_VERSION[version], values))
    for key in ("field_player", "field_com", "hand_player", "hand_com"):
        probe[key] = list(probe[key])
    # The checksum covers everything before the checksum field itself; `stage`
    # and the pad byte follow it and are written asynchronously.
    checksum_at = offset + struct.calcsize(BASE_LAYOUT) - 4
    body = blob[offset:checksum_at]
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
        version = blob[at + 4] if at + 4 < len(blob) else 0
        if version not in SIZES or at + SIZES[version] > len(blob):
            continue
        probe = parse(blob, at, version)
        if probe["version"] not in SIZES:
            continue
        probe["offset"] = at
        found.append(probe)


def probe_address(map_path):
    """Read g_probe's linker-authoritative RAM address when a map exists."""
    if not map_path or not os.path.isfile(map_path):
        return None
    pattern = re.compile(r"^\s*([0-9A-Fa-f]{8})\s+_g_probe\b")
    with open(map_path, encoding="utf-8", errors="replace") as source:
        for line in source:
            match = pattern.match(line)
            if match:
                return int(match.group(1), 16)
    return None


def card(v):
    return "--" if v == 0xFF else str(v)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dump")
    parser.add_argument("--map", dest="map_path")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()

    blob = open(args.dump, "rb").read()
    map_path = args.map_path
    if map_path is None:
        map_path = os.path.splitext(args.dump)[0] + ".map"
    base = probe_address(map_path)
    if base is None:
        candidates = find(blob)
    else:
        candidates = []
        for offset in (base, base + SIZES[EXPECT_VERSION], base + SIZES.get(3, 0)):
            version = blob[offset + 4] if offset + 4 < len(blob) else 0
            if version in SIZES and offset + SIZES[version] <= len(blob) and blob[offset:offset + 4] == MAGIC:
                probe = parse(blob, offset, version)
                if probe["version"] in SIZES:
                    probe["offset"] = offset
                    candidates.append(probe)
    hits = sorted((p for p in candidates if p["checksum_ok"]),
                  key=lambda p: p["frame"], reverse=True)
    if not hits:
        raw = candidates
        if raw:
            sys.exit("%d probe slot(s) found (first at 0x%04X) and none checksums "
                     "-- the struct layout changed: bump MSX2_PROBE_VERSION and "
                     "this script together" % (len(raw), raw[0]["offset"]))
        suffix = " at linker address 0x%04X" % base if base is not None else ""
        sys.exit("no probe found in %s%s: the ROM never reached Msx2_ProbeInit"
                 % (args.dump, suffix))

    p = hits[0]
    if args.json:
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

    if p["version"] >= 3:
        print("regression   view %d mode %d, deal target 0x%02X landed 0x%02X/0x%02X, "
              "streams %d bands %d flips %d"
              % (p["board_view"], p["board_mode"], p["deal_target_mask"],
                 p["deal_landed0"], p["deal_landed1"], p["full_view_streams"],
                 p["band_streams"], p["page_flips"]))
        print("audio        track %d segment %d frame %d loop %d errors %d; entropy 0x%02X"
              % (p["music_track"], p["music_segment"], p["music_frames"],
                 p["music_loops"], p["music_errors"], p["entropy_sources"]))

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
