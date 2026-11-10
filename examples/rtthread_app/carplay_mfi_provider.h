/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Shared AirPlay MFiSAP provider lifecycle.
 *
 * The AirPlay /auth-setup handler reads a process-wide provider installed by
 * core/airplay/airplay_mfi_sap.c.  Both transports -- Bluetooth iAP2 and the
 * wired USB carkit stream -- talk to the same MFi coprocessor, so the
 * provider must be installed for either mode and must stay installed while
 * an AirPlay session can reach /auth-setup.
 *
 * Install before a transport starts its session; release only after its
 * session (and therefore its AirPlay connection) has stopped.  Both calls
 * serialize on an RT-Thread mutex and refcount installations of the same
 * coprocessor handle, so starting wired and wireless sessions does not make
 * one release pull the provider out from under the other.
 */
#ifndef CARPLAY_MFI_PROVIDER_H
#define CARPLAY_MFI_PROVIDER_H

#include "rtcp/mfi.h"

/* Returns RTCP_OK when the provider is installed.  Installing the same
 * handle twice increments a reference count.  A NULL or different handle is
 * rejected while another handle owns the provider. */
int carplay_mfi_provider_install(const struct rtcp_mfi *mfi);

/* Drop one reference.  Clears the core provider when the last user of this
 * handle releases it.  Releasing a handle that is not installed is a no-op. */
void carplay_mfi_provider_release(const struct rtcp_mfi *mfi);

#endif /* CARPLAY_MFI_PROVIDER_H */
