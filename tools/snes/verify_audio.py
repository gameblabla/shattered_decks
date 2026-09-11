#!/usr/bin/env python3
"""Verify cartridge audio with the accurate Mednafen core and real SPC RAM/PCM.

See audio-verification.md for building the capture-enabled emulator and probe.
"""
import argparse
from array import array
import math
import os
from pathlib import Path
import re
import subprocess
import sys
import wave

ROOT = Path(__file__).resolve().parents[2]
TRACKS = ('titlealt', 'altbattle', 'overworld', 'victory', 'fail',
          'titlealt', 'altbattle', 'overworld')
TRACK_IDS = (0, 2, 1, 3, 4, 0, 2, 1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--emulator', type=Path, required=True)
    parser.add_argument('--build', type=Path, default=ROOT / 'build/snes/audio-probe',
                        help='audio probe build directory')
    args = parser.parse_args()
    emulator = args.emulator.resolve()
    build = args.build.resolve()
    out = build / 'verify'
    out.mkdir(parents=True, exist_ok=True)
    symbols = (build / 'waifusnes.sym').read_text()
    address = int(re.search(r'^([0-9a-fA-F]+) audio_probe_stamp$', symbols,
                            re.MULTILINE)[1], 16) & 0x1FFFF
    layout = (ROOT / 'src/snes/snes_audio_layout.h').read_text()
    for phase, track in enumerate(TRACKS):
        name = out / ('%d-%s' % (phase, track))
        inputs = name.with_suffix('.txt')
        inputs.write_text('0 0 0 0\n' + ''.join(
            '%d %d 128 0\n' % (n * 400, n * 400 + 10)
            for n in range(1, phase + 1)))
        pcm = name.with_suffix('.pcm')
        aram_path = name.with_suffix('.aram')
        for path in (pcm, aram_path):
            path.unlink(missing_ok=True)
        env = dict(os.environ, SNES_AUDIO_PCM=str(pcm),
                   SNES_AUDIO_ARAM=str(aram_path))
        result = subprocess.run(
            [str(emulator), 'script', str(build / 'waifusnes.sfc'),
             str(inputs), str(phase * 400 + 300), str(name.with_suffix('.ppm')),
             str(name.with_suffix('.wram'))], cwd=out, env=env,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
        name.with_suffix('.log').write_bytes(result.stdout)
        if result.returncode:
            raise AssertionError('emulator failed: ' + str(name))
        ram = name.with_suffix('.wram').read_bytes()
        words = [int.from_bytes(ram[i:i + 2], 'little')
                 for i in range(address, address + 8, 2)]
        assert words[0] == 0x4155 and words[1] == phase, words
        assert words[2] > 100 and words[3] == TRACK_IDS[phase], words
        aram = aram_path.read_bytes()
        snapshot = (ROOT / ('src/snes/assets/audio/%s.spc' % track)).read_bytes()[256:]
        end = int(re.search(r'SNES_AUDIO_%s_END 0x([0-9A-F]+)' % track.upper(),
                            layout)[1], 16)
        assert aram[0x200:0xC10] == snapshot[0x200:0xC10], 'resident code corrupted'
        assert aram[0xC10:end] == snapshot[0xC10:end], 'wrong music in SPC RAM'
        assert aram[0xE700:0xE800] == snapshot[0xE700:0xE800], 'wrong DSP directory'
        finite = int(track in ('victory', 'fail'))
        assert aram[0x67] == finite, (
            'wrong end mode for %s: got %d expected %d' %
            (track, aram[0x67], finite))
        if phase:
            sfx = (ROOT / 'src/snes/assets/audio/altbattle.spc').read_bytes()[256:]
            assert aram[0xB400:0xE700] == sfx[0xB400:0xE700], 'SFX bank corrupted'
        samples = array('h', pcm.read_bytes()[-48000 * 4:])
        if sys.byteorder != 'little':
            samples.byteswap()
        rms = math.sqrt(sum(x * x for x in samples) / len(samples))
        assert rms > 10, 'silent playback: RMS %.2f' % rms
        with wave.open(str(name.with_suffix('.wav')), 'wb') as wav:
            wav.setparams((2, 2, 48000, 0, 'NONE', 'not compressed'))
            wav.writeframes(pcm.read_bytes())
        pcm.unlink()
        print('ok phase %d %-10s exact SPC music/directory, PCM RMS %.1f' %
              (phase, track, rms), flush=True)


if __name__ == '__main__':
    main()
