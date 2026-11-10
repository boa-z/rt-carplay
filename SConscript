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

src = []
cpppath = [cwd + '/include']
ccflags = []

# Force-include the logging provider so include/rtcp/log.h stays platform-free.
# LOCAL_CCFLAGS is a single flag string; DefineGroup() rewrites it.
if GetDepend('LPKG_RT_CARPLAY_LOG'):
    ccflags = '-include ' + os.path.join(
        cwd, 'port/rtthread/include/rtcp_log_rtthread.h')

# Protocol core: hardware and OS independent.
for sub in ('airplay', 'iap2', 'discovery', 'input', 'media', 'mfi'):
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

# RT-Thread OS and socket seams.
src += Glob('port/rtthread/*.c')
cpppath.append(cwd + '/port/rtthread/include')

# Luban-Lite board seams.  The include directory is shared by the Wi-Fi,
# Bluetooth and MFi sources (rtcp_lubanlite.h declares all three interfaces).
if (GetDepend('LPKG_RT_CARPLAY_LUBANLITE_WIFI') or
        GetDepend('LPKG_RT_CARPLAY_LUBANLITE_BT') or
        GetDepend('LPKG_RT_CARPLAY_LUBANLITE_MFI')):
    cpppath.append(cwd + '/port/luban-lite/include')

# Luban-Lite Wi-Fi seam (Realtek RTL8733BS SoftAP + vendor IE).
if GetDepend('LPKG_RT_CARPLAY_LUBANLITE_WIFI'):
    src += Glob('port/luban-lite/rtcp_wifi_lubanlite.c')
    src += Glob('port/luban-lite/rtcp_random_lubanlite.c')

    # The Realtek API headers live outside the SDK's global include path;
    # the driver SConscript adds them only to its own group.
    aic_root = os.environ.get('AIC_ROOT', '')
    if aic_root:
        realtek = os.path.join(aic_root, 'bsp', 'peripheral', 'wireless',
                               'realtek')
        for sub in ('api/wifi', 'api', 'include', 'os/os_dep/include',
                    'platform/include', 'platform/rtwlan_bsp'):
            cpppath.append(os.path.join(realtek, sub))

# Luban-Lite Bluetooth seam: Zephyr Classic BR/EDR + RFCOMM for iAP2.
if GetDepend('LPKG_RT_CARPLAY_LUBANLITE_BT'):
    src += Glob('port/luban-lite/rtcp_bt_lubanlite.c')

    # The zephyr-bluetooth package ships no settings core; this file backs
    # CONFIG_BT_SETTINGS with an RT-Thread DFS file so BR/EDR link keys (and
    # therefore the phone's bond) survive a reboot.
    if GetDepend('LPKG_BT_SETTINGS'):
        src += Glob('port/luban-lite/rtcp_bt_settings.c')

    aic_root = os.environ.get('AIC_ROOT', '')
    if aic_root:
        zephyr = os.path.join(aic_root, 'packages', 'third-party',
                              'zephyr-bluetooth')
        cpppath.append(os.path.join(zephyr, 'include'))
        cpppath.append(os.path.join(zephyr, 'porting'))

# Luban-Lite MFi seam: 2.0C authentication coprocessor on the I2C bus.
if GetDepend('LPKG_RT_CARPLAY_LUBANLITE_MFI'):
    src += Glob('port/luban-lite/rtcp_mfi_lubanlite.c')

group = DefineGroup('rt-carplay', src, depend=['LPKG_USING_RT_CARPLAY'],
                    CPPPATH=cpppath, LOCAL_CCFLAGS=ccflags)

Return('group')
