#!/usr/bin/env python3
"""Drive the bundled headless Hatari MCP build from a script.

The MCP server has no key-injection tool, so the port is verified the way the
other blind targets are: the build stamps its state into a fixed RAM probe and
the harness reads it back with dump_ram, while screenshot proves what reached
the screen.  Scripted input is delivered by poking the probe's input queue.

    python3 tools/atarist/hatari_mcp.py run --image build/atarist/waifu.st \
        --machine ste --seconds 6 --shot /tmp/shot.png
"""

import argparse
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
HATARI = os.path.join(ROOT, "AtariST", "hatari-headless-mcp")
TOS = os.path.join(ROOT, "AtariST", "etos512us.img")


class Hatari(object):
    def __init__(self, hatari=HATARI, verbose=False):
        self.verbose = verbose
        self.proc = subprocess.Popen(
            [hatari, "--mcp-server"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, text=True, bufsize=1,
            cwd=os.path.dirname(hatari))
        self._id = 0
        self._rpc("initialize", {"protocolVersion": "2024-11-05",
                                 "capabilities": {},
                                 "clientInfo": {"name": "waifu", "version": "1"}})
        self._notify("notifications/initialized", {})

    def _write(self, msg):
        self.proc.stdin.write(json.dumps(msg) + "\n")
        self.proc.stdin.flush()

    def _notify(self, method, params):
        self._write({"jsonrpc": "2.0", "method": method, "params": params})

    def _rpc(self, method, params):
        self._id += 1
        self._write({"jsonrpc": "2.0", "id": self._id,
                     "method": method, "params": params})
        while True:
            line = self.proc.stdout.readline()
            if not line:
                raise RuntimeError("hatari MCP server closed the connection")
            msg = json.loads(line)
            if msg.get("id") == self._id:
                if "error" in msg:
                    raise RuntimeError("%s: %s" % (method, msg["error"]))
                return msg.get("result")

    def tool(self, name, **args):
        res = self._rpc("tools/call", {"name": name, "arguments": args})
        text = "".join(c.get("text", "") for c in res.get("content", []))
        if self.verbose:
            sys.stderr.write("[%s] %s\n" % (name, text.strip()[:400]))
        return text

    # ---- convenience -------------------------------------------------------
    def start(self, path, machine="ste", tos=TOS, mount=None, fast=False,
              autostart=None):
        args = {"action": "start", "path": os.path.abspath(path),
                "machine": machine, "tos": os.path.abspath(tos)}
        if mount:
            args["mount"] = os.path.abspath(mount)
        if autostart:
            args["autostart"] = autostart
        if fast:
            args["fast_forward"] = True
        return self.tool("emu", **args)

    def status(self):
        return self.tool("emu", action="status")

    def screenshot(self, path):
        return self.tool("screenshot", path=os.path.abspath(path))

    def dump(self, address, length, path):
        """Read memory.  dump_ram pauses the emulator to take a consistent
        snapshot and does NOT resume it, so every reader has to say `continue`
        afterwards -- otherwise a second sample looks identical to the first
        and every blind check reports a frozen game."""
        out = self.tool("dump_ram", address=address, length=length,
                        path=os.path.abspath(path))
        self.tool("emu", action="continue")
        return out

    def poke(self, address, data):
        """Write exact bytes at `address`.  `data` is bytes or a hex string.

        THE BYTES MUST BE SPACE SEPARATED.  The MCP `reasm` tool parses a bare
        hex run as a number, so "0011" writes one byte and a payload with a
        zero in it silently shifts everything after it -- which is how the
        scripted-input queue arrived as garbage and every duel measurement
        looked idle.  Byte pairs separated by spaces are taken literally.
        """
        if isinstance(data, str):
            data = bytes.fromhex(data.replace(" ", ""))
        hexbytes = " ".join("%02x" % b for b in data)
        out = self.tool("reasm", address=address, bytes=hexbytes)
        self.tool("emu", action="continue")
        return out

    def debug(self, command):
        out = self.tool("debug", command=command)
        self.tool("emu", action="continue")
        return out

    def close(self):
        try:
            self.tool("emu", action="stop")
        except Exception:
            pass
        self.proc.kill()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("action", choices=["run"])
    ap.add_argument("--image", required=True)
    ap.add_argument("--machine", default="st")
    ap.add_argument("--tos", default=TOS)
    ap.add_argument("--mount")
    ap.add_argument("--seconds", type=float, default=8.0)
    ap.add_argument("--shot")
    ap.add_argument("--shot-every", type=float, default=0.0)
    ap.add_argument("--dump", help="ADDR:LEN:PATH")
    ap.add_argument("--fast", action="store_true")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()

    h = Hatari(verbose=a.verbose)
    try:
        print(h.start(a.image, machine=a.machine, tos=a.tos, mount=a.mount,
                      fast=a.fast).strip())
        if a.shot_every > 0 and a.shot:
            base, ext = os.path.splitext(a.shot)
            n, t = 0, 0.0
            while t < a.seconds:
                time.sleep(a.shot_every)
                t += a.shot_every
                n += 1
                h.screenshot("%s_%02d%s" % (base, n, ext))
        else:
            time.sleep(a.seconds)
        print(h.status().strip())
        if a.shot:
            print(h.screenshot(a.shot).strip())
        if a.dump:
            addr, length, path = a.dump.split(":", 2)
            print(h.dump(int(addr, 0), int(length, 0), path).strip())
    finally:
        h.close()


if __name__ == "__main__":
    main()
