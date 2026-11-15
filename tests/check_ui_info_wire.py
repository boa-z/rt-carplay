#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent plistlib checks for experimental single-Main-Display UI info.

Only synthetic metadata is used. This does not supply a renderer or prove
real-iPhone interoperability. Panel field names follow LIVI; see NOTICE.
"""
from copy import deepcopy
from pathlib import Path
import plistlib
import subprocess
import sys
import tempfile

exe = str(Path(sys.argv[1]).resolve())
result = subprocess.run([exe, '--ui-wire'], check=True, capture_output=True, text=True)
raw = bytes.fromhex(next(line.split()[1] for line in result.stdout.splitlines()
                         if line.startswith('UI_INFO ')))
info = plistlib.loads(raw)
uuid = 'b7e6c5a0-1111-4000-8000-000000000001'
assert info['uiSyncInfo'] == {}
assert len(info['displays']) == 1 and info['displays'][0]['type'] == 110
assert info['displays'][0]['uuid'] == uuid
assert info['displayPanels'] == [{
    'uid': 'synthetic-main-panel', 'widthPixels': 1280, 'heightPixels': 480,
    'widthPhysical': 300, 'heightPhysical': 120, 'properties': [],
    'initialVideoStreams': [{'uuid': uuid, 'originXPixels': 0, 'originYPixels': 0, 'zIndex': 0}]}]
assert 'hevcInfo' not in info
assert 'displayPlugins' not in info['displayPanels'][0]
combinations = set()
for line in result.stdout.splitlines():
    if line.startswith('UI_FEATURES '):
        _, mask, encoded = line.split()
        mask = int(mask)
        expected = ['iAPChannel', 'viewAreas']
        if mask & 1:
            expected.append('vehicleStateProtocol')
        if mask & 2:
            expected.append('uiSync')
        assert plistlib.loads(bytes.fromhex(encoded)) == {
            'timingPort': 49152, 'eventPort': 49153, 'enabledFeatures': expected}
        combinations.add(mask)
assert combinations == {0, 1, 2, 3}

cases = [('original', raw, True)]
for order in (False, True):
    cases.append(('plistlib-' + str(order), plistlib.dumps(info, fmt=plistlib.FMT_BINARY,
                                                        sort_keys=order), True))


def change(name, mutate):
    value = deepcopy(info)
    mutate(value)
    cases.append((name, plistlib.dumps(value, fmt=plistlib.FMT_BINARY), False))


for key in ('uiSyncInfo', 'displayPanels', 'displays'):
    change('missing-' + key, lambda v, k=key: v.pop(k))
for value in ([], {'unsupported': True}, True):
    change('ui-type-' + str(value), lambda v, x=value: v.update(uiSyncInfo=x))
for key in ('displays', 'displayPanels'):
    change('empty-' + key, lambda v, k=key: v.update({k: []}))
    change('extra-' + key, lambda v, k=key: v[k].append(deepcopy(v[k][0])))
for container in ('displays', 'displayPanels'):
    for key in ('widthPixels', 'heightPixels', 'widthPhysical', 'heightPhysical'):
        for value in (0, 1.0, True, 65535):
            change(container + key + str(value),
                   lambda v, c=container, k=key, x=value: v[c][0].update({k: x}))
change('wrong-display-uuid', lambda v: v['displays'][0].update(uuid='other'))
change('wrong-display-type', lambda v: v['displays'][0].update(type=111))
change('wrong-panel-uid', lambda v: v['displayPanels'][0].update(uid='other'))
change('nul-panel-uid', lambda v: v['displayPanels'][0].update(uid='synthetic-main-panel\0suffix'))
change('invented-instruments', lambda v: v['displayPanels'][0].update(properties=['showsInstruments']))
change('invented-plugin', lambda v: v['displayPanels'][0].update(displayPlugins=[{'uid': 'fake'}]))
change('extra-stream', lambda v: v['displayPanels'][0]['initialVideoStreams'].append({}))
change('absent-stream', lambda v: v['displayPanels'][0].update(initialVideoStreams=[]))
for key, value in [('uuid', 'other'), ('originXPixels', 1), ('originYPixels', 1), ('zIndex', 1), ('type', 111)]:
    change('stream-' + key, lambda v, k=key, x=value: v['displayPanels'][0]['initialVideoStreams'][0].update({k: x}))


def duplicate_key(data, wanted):
    """Introduce duplicate references in an independently encoded object table."""
    out = bytearray(data)
    ow, rw = data[-26:-24]
    count = int.from_bytes(data[-24:-16], 'big')
    table = int.from_bytes(data[-8:], 'big')
    offsets = [int.from_bytes(data[table + i*ow:table + (i+1)*ow], 'big') for i in range(count)]

    def size(at):
        length = data[at] & 15
        at += 1
        if length == 15:
            width = 1 << (data[at] & 15)
            length = int.from_bytes(data[at+1:at+1+width], 'big')
            at += 1 + width
        return length, at

    for pos in offsets:
        if data[pos] >> 4 != 13:
            continue
        length, refs = size(pos)
        names = []
        for i in range(length):
            key = offsets[int.from_bytes(data[refs+i*rw:refs+(i+1)*rw], 'big')]
            assert data[key] >> 4 == 5
            n, start = size(key)
            names.append(data[start:start+n].decode('ascii'))
        # widthPixels also appears in unrelated view-area dictionaries. Target
        # the actual panel whose geometry this validator is responsible for.
        if wanted == 'widthPixels' and 'uid' not in names:
            continue
        if wanted == 'originXPixels' and 'zIndex' not in names:
            continue
        if wanted in names:
            source = names.index(wanted)
            target = (source+1) % length
            out[refs+target*rw:refs+(target+1)*rw] = data[refs+source*rw:refs+(source+1)*rw]
            return bytes(out)
    raise AssertionError(wanted)


for key in ('uiSyncInfo', 'displayPanels', 'displays', 'uid', 'widthPixels', 'initialVideoStreams', 'originXPixels'):
    cases.append(('duplicate-' + key, duplicate_key(raw, key), False))
with tempfile.TemporaryDirectory(prefix='rtcp-ui-info-') as folder:
    for index, (name, data, accepted) in enumerate(cases):
        path = Path(folder) / (str(index) + '.plist')
        path.write_bytes(data)
        tested = subprocess.run([exe, '--ui-match', str(path), 'yes' if accepted else 'no'], capture_output=True)
        assert tested.returncode == 0, (name, tested.stderr.decode(errors='replace'))
print('UI /info: independent plistlib single-panel, geometry, type, duplicate and stream checks passed ({} cases)'.format(len(cases)))
