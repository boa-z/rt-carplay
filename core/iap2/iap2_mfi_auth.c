/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "iap2_mfi_auth.h"

#include <stddef.h>

void iap2_mfi_auth_reset(iap2_mfi_auth_t *auth)
{
    if (auth != NULL) auth->stage = IAP2_MFI_AUTH_IDLE;
}

bool iap2_mfi_auth_can_respond_to_challenge(const iap2_mfi_auth_t *auth)
{
    return auth != NULL && (auth->stage == IAP2_MFI_AUTH_IDLE ||
                            auth->stage == IAP2_MFI_AUTH_CHALLENGE_RESPONSE_ACKED);
}

bool iap2_mfi_auth_mark_challenge_response_acked(iap2_mfi_auth_t *auth)
{
    if (auth == NULL || auth->stage != IAP2_MFI_AUTH_CHALLENGE_RESPONSE_PENDING) return false;
    auth->stage = IAP2_MFI_AUTH_CHALLENGE_RESPONSE_ACKED;
    return true;
}

bool iap2_mfi_auth_mark_challenge_response_queued(iap2_mfi_auth_t *auth)
{
    if (!iap2_mfi_auth_can_respond_to_challenge(auth)) return false;
    auth->stage = IAP2_MFI_AUTH_CHALLENGE_RESPONSE_PENDING;
    return true;
}

bool iap2_mfi_auth_mark_authenticated(iap2_mfi_auth_t *auth)
{
    if (auth == NULL || auth->stage != IAP2_MFI_AUTH_CHALLENGE_RESPONSE_ACKED) {
        return false;
    }
    auth->stage = IAP2_MFI_AUTHENTICATED;
    return true;
}

void iap2_mfi_auth_mark_failed(iap2_mfi_auth_t *auth)
{
    if (auth != NULL) auth->stage = IAP2_MFI_AUTH_FAILED;
}

bool iap2_mfi_auth_is_authenticated(const iap2_mfi_auth_t *auth)
{
    return auth != NULL && auth->stage == IAP2_MFI_AUTHENTICATED;
}

iap2_mfi_auth_stage_t iap2_mfi_auth_stage(const iap2_mfi_auth_t *auth)
{
    return auth != NULL ? auth->stage : IAP2_MFI_AUTH_FAILED;
}

const char *iap2_mfi_auth_stage_name(iap2_mfi_auth_stage_t stage)
{
    switch (stage) {
    case IAP2_MFI_AUTH_IDLE: return "idle";
    case IAP2_MFI_AUTH_CHALLENGE_RESPONSE_PENDING: return "challenge_response_pending";
    case IAP2_MFI_AUTH_CHALLENGE_RESPONSE_ACKED: return "challenge_response_acked";
    case IAP2_MFI_AUTHENTICATED: return "authenticated";
    case IAP2_MFI_AUTH_FAILED: return "failed";
    default: return "invalid";
    }
}
