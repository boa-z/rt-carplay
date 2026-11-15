#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent plistlib fixtures/decoding for internal two-video session tests."""
from pathlib import Path
import plistlib
import subprocess
import sys

FIXTURE = Path(__file__).parent / 'support/video_session_fixtures.h'


def streams(*values):
    return {'streams': [{'type': kind, 'streamConnectionID': seed} for kind, seed in values]}


def generated():
    cases = {
        'initial': {'features': ['iAPChannel', 'viewAreas', 'altScreen']},
        'pair': streams((110, 200), (111, 201)),
        'alt': streams((111, 201)),
        'main': streams((110, 200)),
        'replaced_alt': streams((111, 202)),
        'duplicate': streams((111, 203), (111, 204)),
        'collision': streams((110, 205), (111, 205)),
        'live_collision': streams((111, 200)),
        'fresh_pair': streams((110, 210), (111, 211)),
        'other_pair': streams((110, 220), (111, 221)),
        'stale_alt_teardown': streams((111, 199)),
        'alt_type_teardown': {'streams': [{'type': 111}]},
        'video_audio_collision': {'streams': [
            {'type': 110, 'streamConnectionID': 230},
            {'type': 111, 'streamConnectionID': 231},
            {'type': 100, 'streamConnectionID': 231, 'audioFormat': 2048}]},
        'pair_audio': {'streams': [
            {'type': 110, 'streamConnectionID': 230},
            {'type': 111, 'streamConnectionID': 231},
            {'type': 100, 'streamConnectionID': 232, 'audioFormat': 2048}]},
    }
    lines = ['/* SPDX-License-Identifier: GPL-3.0-or-later */',
             '/* Generated independently by tests/check_video_session_wire.py --write. */']
    for name, value in cases.items():
        data = plistlib.dumps(value, fmt=plistlib.FMT_BINARY, sort_keys=False)
        lines.append('static const uint8_t k_video_' + name + '[] = {')
        for offset in range(0, len(data), 16):
            lines.append('    ' + ','.join('0x{:02x}'.format(b) for b in data[offset:offset+16]) + ',')
        lines.append('};')
    return '\n'.join(lines) + '\n'


def check(executable):
    assert FIXTURE.read_text() == generated(), 'Regenerate intentional fixture changes with --write'
    output = subprocess.check_output([str(Path(executable).resolve()), '--dump'], text=True)
    decoded = {}
    for line in output.splitlines():
        if line.startswith('VIDEOSETUP '):
            _, name, data = line.split()
            assert name not in decoded
            decoded[name] = plistlib.loads(bytes.fromhex(data))
    assert decoded['initial']['enabledFeatures'] == ['iAPChannel', 'viewAreas']
    assert decoded['pair']['streams'] == [
        {'type': 110, 'dataPort': 42110}, {'type': 111, 'dataPort': 42111}]
    assert decoded['legacy']['streams'] == []
    print('Two-video session: independent fixtures, default feature bytes and distinct port responses PASS')


if __name__ == '__main__':
    if len(sys.argv) == 2 and sys.argv[1] == '--write':
        FIXTURE.write_text(generated())
    else:
        check(sys.argv[1])
