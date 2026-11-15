#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent plistlib checks of the generated synthetic Vehicle catalogue."""
from copy import deepcopy
from pathlib import Path
import plistlib
import subprocess
import sys
import tempfile

exe = str(Path(sys.argv[1]).resolve())
result = subprocess.run([exe, '--wire'], check=True, capture_output=True, text=True)
wire = next(line.split()[1] for line in result.stdout.splitlines() if line.startswith('VEHICLE_INFO '))
raw = bytes.fromhex(wire)
info = plistlib.loads(raw)
vehicle = info['vehicleStateProtocolInfo']
assert vehicle['protocolVersion'] == '1.0' and vehicle['pluginCount'] == 1
assert vehicle['pluginMapping'] == {'\u8f66\U0001f697': 9}
plugin = vehicle['pluginConfigs'][0]
assert plugin['name'] == '\u8f66\U0001f697' and plugin['pluginID'] == 9
chars = plugin['accessories'][0]['services'][0]['characteristics']
assert chars[0]['initialValue'] == -1.25 and chars[0]['stepValue'] == 0.25
assert chars[0]['minimumValue'] == -2.0 and chars[0]['maximumValue'] == 10.0
assert chars[1]['initialValue'] == 9007199254740993
assert chars[2]['initialValue'] == 'A\0\u8f66\U0001f697'
assert chars[3]['initialValue'] == b'\xff'
assert chars[0]['mutable'] is True and chars[0]['writable'] is False
assert len(info['displays']) == 1 and info['displays'][0]['type'] == 110
assert not {'uiSyncInfo', 'displayPanels', 'hevcInfo'} & info.keys()

cases = [('original', raw, True)]
# Different object order, reference widths and equivalent dictionary ordering.
for order in (False, True):
    cases.append(('plistlib-' + str(order), plistlib.dumps(info, fmt=plistlib.FMT_BINARY, sort_keys=order), True))
for key, value in [('protocolVersion', '2.0'), ('pluginCount', 2)]:
    changed = deepcopy(info)
    changed['vehicleStateProtocolInfo'][key] = value
    cases.append((key, plistlib.dumps(changed, fmt=plistlib.FMT_BINARY), False))
for index, value in [(0, -1), (1, 9007199254740992), (2, 'A\0\u8f66'), (3, b'\x00')]:
    changed = deepcopy(info)
    changed['vehicleStateProtocolInfo']['pluginConfigs'][0]['accessories'][0]['services'][0]['characteristics'][index]['initialValue'] = value
    cases.append(('value-' + str(index), plistlib.dumps(changed, fmt=plistlib.FMT_BINARY), False))
# Create two references to the capability key in the original root dictionary.
duplicate = bytearray(raw)
offwidth, refwidth = raw[-26:-24]
root = int.from_bytes(raw[-16:-8], 'big')
table = int.from_bytes(raw[-8:], 'big')
root_at = int.from_bytes(raw[table + root * offwidth:table + (root + 1) * offwidth], 'big')
count = raw[root_at] & 15
refs_at = root_at + 1
if count == 15:
    width = 1 << (raw[refs_at] & 15)
    count = int.from_bytes(raw[refs_at + 1:refs_at + 1 + width], 'big')
    refs_at += 1 + width
# vehicleStateProtocolInfo is the final emitted root key.
duplicate[refs_at:refs_at + refwidth] = raw[refs_at + (count - 1) * refwidth:refs_at + count * refwidth]
cases.append(('duplicate', bytes(duplicate), False))
with tempfile.TemporaryDirectory(prefix='rtcp-vehicle-info-') as folder:
    for name, data, accepted in cases:
        path = Path(folder) / (name + '.plist')
        path.write_bytes(data)
        subprocess.run([exe, '--match', str(path), 'yes' if accepted else 'no'], check=True, capture_output=True)
print('Vehicle /info: independent plistlib scalar/Unicode/int64 decode and {} metadata cases passed'.format(len(cases)))
