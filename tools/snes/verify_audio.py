#!/usr/bin/env python3
"""Verify cartridge audio with the accurate Mednafen core and real SPC RAM/PCM.

See audio-verification.md for building the capture-enabled emulator and probe.
Every phase changes the track, then plays two SFX slots (the last phase plays
its two 15 frames apart so the second voice streams over the first); each
effect must show up in the PCM as its own reference decode, and the SPC RAM
must still hold the exact resident code, music, directory and SFX region.
"""
import argparse
from array import array
import hashlib
import json
import math
import os
from pathlib import Path
import re
import subprocess
import sys
import wave

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools' / 'snes'))
import gen_snes_audio  # noqa: E402
import snes_sfx_brr2 as brr2  # noqa: E402

TRACKS = ('titlealt', 'altbattle', 'overworld', 'victory', 'fail',
          'titlealt', 'altbattle', 'overworld')
TRACK_IDS = (0, 2, 1, 3, 4, 0, 2, 1)
PAD_A, PAD_B = 0x80, 0x8000
OUT_RATE = 48000
FRAME = OUT_RATE / 60.0


def sfx_presses(phase):
    """(frame, slot) for the two B presses of a phase."""
    base = phase * 400
    gap = 15 if phase == 7 else 100
    return ((base + 150, (2 * phase) % 10), (base + 150 + gap, (2 * phase + 1) % 10))


# The core clocks its S-DSP at 32040 Hz (bsnes' 24.6 MHz APU crystal), so a
# 16 kHz stream plays 0.125% fast against a 48 kHz capture; a tonal effect
# decorrelates within a tenth of a second unless the reference is stretched
# the same way.
DSP_RATE = 32040


def reference_48k(slot_pcm15):
    """A slot's decoded stream as the capture hears it, mono."""
    from scipy.signal import resample, resample_poly
    divisor = math.gcd(OUT_RATE, brr2.SAMPLE_RATE)
    ref = resample_poly(slot_pcm15.astype(np.float64), OUT_RATE // divisor,
                        brr2.SAMPLE_RATE // divisor)
    return resample(ref, int(round(len(ref) * 32000 / DSP_RATE)))


def best_match(mix, ref, at, window):
    """Peak normalised cross-correlation of ref against mix near sample at."""
    best = 0.0
    n = len(ref)
    rn = ref / (np.linalg.norm(ref) or 1.0)
    for start in range(max(0, at - window), min(len(mix) - n, at + window), 16):
        seg = mix[start:start + n]
        norm = np.linalg.norm(seg)
        if not norm:
            continue
        best = max(best, float(np.dot(seg, rn) / norm))
    return best


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--emulator', type=Path, required=True)
    parser.add_argument('--build', type=Path, default=ROOT / 'build/snes/audio-probe',
                        help='audio probe build directory')
    parser.add_argument('--phases', default=None,
                        help='comma-separated phases to run (default: all)')
    parser.add_argument('--layout', type=Path,
                        default=ROOT / 'src/snes/snes_audio_layout.h')
    parser.add_argument('--timeout', type=int, default=120)
    args = parser.parse_args()
    try:
        phases = ([int(p.strip()) for p in args.phases.split(',') if p.strip()]
                  if args.phases is not None else list(range(len(TRACKS))))
    except ValueError as exc:
        parser.error('--phases must be comma-separated integers')
    if not phases or len(set(phases)) != len(phases) or any(
            phase < 0 or phase >= len(TRACKS) for phase in phases):
        parser.error('--phases must select unique values from 0 through 7')
    emulator = args.emulator.resolve()
    if not emulator.is_file():
        parser.error('missing capture-enabled emulator: %s' % emulator)
    build = args.build.resolve()
    out = build / 'verify'
    out.mkdir(parents=True, exist_ok=True)
    rom = build / 'waifusnes.sfc'
    symbol_path = build / 'waifusnes.sym'
    for required in (rom, symbol_path, args.layout):
        if not required.is_file():
            parser.error('missing required candidate file: %s' % required)
    source_inputs = [ROOT / 'Makefile.snes', ROOT / 'tools/snes/audio_probe.c',
                     ROOT / 'tools/snes/gen_snes_audio.py',
                     ROOT / 'tools/snes/snes_sfx_brr2.py',
                     ROOT / 'src/snes/snes_audio.asm',
                     ROOT / 'src/snes/snes_audio_driver.c',
                     ROOT / 'src/snes/snes_audio.h']
    source_inputs += list((ROOT / 'src/snes/assets/audio').glob('*.spc'))
    newer = [path for path in source_inputs if path.stat().st_mtime_ns > rom.stat().st_mtime_ns]
    if newer:
        parser.error('probe ROM is stale; newer input: %s' % newer[0])
    symbols = symbol_path.read_text()
    address = int(re.search(r'^([0-9a-fA-F]+) audio_probe_stamp$', symbols,
                            re.MULTILINE)[1], 16) & 0x1FFFF
    layout = args.layout.resolve().read_text()
    region = int(re.search(r'SNES_AUDIO_SFX_REGION 0x([0-9A-F]+)', layout)[1], 16)

    # The slot references, decoded exactly as the SPC700 must expand them.
    bank, entries, _ = gen_snes_audio.build_sfx_bank()
    refs = {}
    for slot, (offset, blocks) in enumerate(entries):
        if blocks:
            packed = bank[offset:offset + blocks * brr2.PACKED_BLOCK_BYTES]
            refs[slot] = reference_48k(brr2.decode(packed))

    snapshots = {track: (ROOT / ('src/snes/assets/audio/%s.spc' % track)).read_bytes()
                 for track in set(TRACKS)}
    resident = {data[256 + 0x200:256 + 0xC10] for data in snapshots.values()}
    if len(resident) != 1:
        raise AssertionError('SPC snapshots do not share identical resident code')
    sha256 = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    manifest = {
        'format': 1,
        'complete': False,
        'requested_phases': phases,
        'rom': {'path': str(rom), 'sha256': sha256(rom)},
        'symbols': {'path': str(symbol_path), 'sha256': sha256(symbol_path)},
        'layout': {'path': str(args.layout.resolve()), 'sha256': sha256(args.layout.resolve())},
        'emulator': {'path': str(emulator), 'sha256': sha256(emulator)},
        'snapshots': {track: sha256(ROOT / ('src/snes/assets/audio/%s.spc' % track))
                      for track in sorted(snapshots)},
        'results': [],
    }
    manifest_path = out / 'result-manifest.json'
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + '\n')

    for phase, track in enumerate(TRACKS):
        if phase not in phases:
            continue
        name = out / ('%d-%s' % (phase, track))
        rows = ['0 0 0 0']
        for n in range(1, phase + 1):
            rows.append('%d %d %d 0' % (n * 400, n * 400 + 10, PAD_A))
            for frame, _slot in sfx_presses(n - 1):
                rows.append('%d %d %d 0' % (frame, frame + 4, PAD_B))
        for frame, _slot in sfx_presses(phase):
            rows.append('%d %d %d 0' % (frame, frame + 4, PAD_B))
        rows.sort(key=lambda row: int(row.split()[0]))
        inputs = name.with_suffix('.txt')
        inputs.write_text('\n'.join(rows) + '\n')
        pcm = name.with_suffix('.pcm')
        aram_path = name.with_suffix('.aram')
        for path in (pcm, aram_path):
            path.unlink(missing_ok=True)
        env = dict(os.environ, SNES_AUDIO_PCM=str(pcm),
                   SNES_AUDIO_ARAM=str(aram_path))
        result = subprocess.run(
            [str(emulator), 'script', str(rom),
             str(inputs), str(phase * 400 + 340), str(name.with_suffix('.ppm')),
             str(name.with_suffix('.wram'))], cwd=out, env=env,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=args.timeout)
        name.with_suffix('.log').write_bytes(result.stdout)
        if result.returncode:
            raise AssertionError('emulator failed: ' + str(name))
        ram = name.with_suffix('.wram').read_bytes()
        words = [int.from_bytes(ram[i:i + 2], 'little')
                 for i in range(address, address + 8, 2)]
        assert words[0] == 0x4155 and words[1] == phase, words
        assert words[2] > 100 and words[3] == TRACK_IDS[phase], words
        aram = aram_path.read_bytes()
        snapshot = snapshots[track][256:]
        end = int(re.search(r'SNES_AUDIO_%s_END 0x([0-9A-F]+)' % track.upper(),
                            layout)[1], 16)
        assert aram[0x200:0xC10] == snapshot[0x200:0xC10], 'resident code corrupted'
        assert aram[0xC10:end] == snapshot[0xC10:end], 'wrong music in SPC RAM'
        assert aram[0xE700:0xE800] == snapshot[0xE700:0xE800], 'wrong DSP directory'
        bank_start = region + gen_snes_audio.SFX_VOICES * gen_snes_audio.RING_BYTES
        assert aram[bank_start:0xE700] == snapshot[bank_start:0xE700], 'SFX bank corrupted'
        finite = int(track in ('victory', 'fail'))
        assert aram[0x67] == finite, (
            'wrong end mode for %s: got %d expected %d' %
            (track, aram[0x67], finite))
        # Both streams idle again by the end of the run.
        assert aram[0xD6] == 0 and aram[0xDE] == 0, 'an SFX stream never finished'
        late = aram[0xED] | (aram[0xEE] << 8)
        worst_backlog = aram[0xEF]
        underruns = (aram[0xD7], aram[0xDF])
        min_leads = (aram[0x69], aram[0x6A])
        assert underruns == (0, 0), 'SFX ring underrun: %r' % (underruns,)
        raw = pcm.read_bytes()
        samples = np.frombuffer(raw, dtype='<i2').astype(np.float64)
        mix = samples[0::2] + samples[1::2]
        rms = math.sqrt(float(np.mean(samples[-OUT_RATE * 4:] ** 2)))
        assert rms > 10, 'silent playback: RMS %.2f' % rms
        scores = []
        for index, (frame, slot) in enumerate(sfx_presses(phase)):
            if slot not in refs:
                continue
            at = int(frame * FRAME)
            check_mix = mix
            check_ref = refs[slot]
            # In the overlap phase the second, noise-like effect shares the
            # window with another full-volume one-shot. First differences
            # reject the smooth music/first-effect envelope while retaining
            # the exact second effect's high-frequency signature.
            if phase == 7 and index == 1:
                check_mix = np.diff(check_mix)
                check_ref = np.diff(check_ref)
            score = best_match(check_mix, check_ref, at, int(12 * FRAME))
            scores.append('slot %d %.2f' % (slot, score))
            # The second voice of the overlapping pair plays under the first
            # (and the music), so its share of the mix is smaller.
            floor = 0.35
            # DIRECT_HIT is the noisiest retained 2-bit source (about 11 dB
            # codec SNR); its exact decoded reference remains identifiable
            # under the failure cue, but with a lower normalized share.
            if slot == 8:
                floor = 0.24
            assert score > floor, 'SFX slot %d not heard in phase %d (%.2f)' % (slot, phase, score)
        with wave.open(str(name.with_suffix('.wav')), 'wb') as wav:
            wav.setparams((2, 2, OUT_RATE, 0, 'NONE', 'not compressed'))
            wav.writeframes(raw)
        pcm.unlink()
        manifest['results'].append({
            'phase': phase, 'track': track, 'late_tick_events': late,
            'worst_timer_backlog': worst_backlog,
            'minimum_decode_lead': min_leads, 'ring_underruns': underruns,
            'pcm_rms': rms, 'scores': scores,
            'aram_sha256': sha256(aram_path),
            'wav_sha256': sha256(name.with_suffix('.wav')),
        })
        manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + '\n')
        print('ok phase %d %-10s exact SPC music/directory/SFX, PCM RMS %.1f, %s, '
              '%d late reads, worst backlog %d, min lead %s' %
              (phase, track, rms, ', '.join(scores), late, worst_backlog,
               min_leads), flush=True)
    manifest['complete'] = phases == list(range(len(TRACKS)))
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + '\n')


if __name__ == '__main__':
    main()
