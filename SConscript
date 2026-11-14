# SPDX-License-Identifier: GPL-3.0-or-later
#
# RT-Thread / Luban-Lite build integration for rt-carplay.
#
# The integrating application calls this script from its own SConscript:
#
#     SConscript('<path-to-rt-carplay>/SConscript')
#
# Include this directory (not a subdirectory) so Glob() paths stay relative.
# mbedTLS 2.x is expected to provide ECP/X25519, AES-CTR, SHA-1/256/512,
# MPI, ChaCha20-Poly1305 and HKDF; see port/luban-lite/README.md.

import os
from building import *

cwd = GetCurrentDir()

src = Glob('runtime/*.c')
cpppath = [cwd + '/include']
ccflags = []

# Force-include the logging provider so include/rtcp/log.h stays platform-free.
# LOCAL_CCFLAGS is a single flag string; DefineGroup() rewrites it.
if GetDepend('LPKG_RT_CARPLAY_LOG'):
    ccflags = '-include ' + os.path.join(
        cwd, 'port/rtthread/include/rtcp_log_rtthread.h')

# Protocol core: hardware and OS independent.
for sub in ('audio', 'airplay', 'iap2', 'discovery', 'input', 'media', 'mfi', 'usb'):
    src += Glob('core/%s/*.c' % sub)
    inc = os.path.join(cwd, 'core', sub, 'include')
    if os.path.isdir(inc):
        cpppath.append(inc)
    else:
        cpppath.append(os.path.join(cwd, 'core', sub))

# Software Ed25519 identity (vendored orlp/ed25519, zlib license).
if GetDepend('LPKG_RT_CARPLAY_ED25519'):
    src += Glob('core/crypto/*.c')
    src += Glob('third_party/ed25519/*.c')
    cpppath.append(cwd + '/core/crypto/include')
    cpppath.append(cwd + '/third_party/ed25519')

if GetDepend('LPKG_RT_CARPLAY_AUDIO_CODECS'):
    cpppath += [cwd + '/third_party/audio/faad2/include', cwd + '/third_party/audio/opus/include', cwd + '/third_party/audio/speexdsp/include']
    # Group-local definition: do not change the SDK's unrelated audio packages.
    ccflags = (ccflags or '') + ' -DRTCP_AUDIO_CODECS'

# RT-Thread OS and socket seams.
src += Glob('port/rtthread/*.c')
cpppath.append(cwd + '/port/rtthread/include')

# Optional generic example, independent of the retired board template.
if GetDepend('LPKG_RT_CARPLAY_HEADLESS_EXAMPLE'):
    src += Glob('examples/rtthread_headless/*.c')
    cpppath.append(cwd + '/examples/rtthread_headless')

group = DefineGroup('rt-carplay', src, depend=['LPKG_USING_RT_CARPLAY'],
                    CPPPATH=cpppath, LOCAL_CCFLAGS=ccflags)

if GetDepend('LPKG_RT_CARPLAY_AUDIO_CODECS'):
    group = group + SConscript(os.path.join(cwd, 'third_party', 'audio', 'SConscript'))

Return('group')
