/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdbool.h>

typedef enum {
    IAP2_MFI_AUTH_IDLE = 0,
    IAP2_MFI_AUTH_CHALLENGE_RESPONSE_PENDING,
    IAP2_MFI_AUTH_CHALLENGE_RESPONSE_ACKED,
    IAP2_MFI_AUTHENTICATED,
    IAP2_MFI_AUTH_FAILED,
} iap2_mfi_auth_stage_t;

typedef struct {
    iap2_mfi_auth_stage_t stage;
} iap2_mfi_auth_t;

void iap2_mfi_auth_reset(iap2_mfi_auth_t *auth);
bool iap2_mfi_auth_can_respond_to_challenge(const iap2_mfi_auth_t *auth);
bool iap2_mfi_auth_mark_challenge_response_queued(iap2_mfi_auth_t *auth);
bool iap2_mfi_auth_mark_challenge_response_acked(iap2_mfi_auth_t *auth);
bool iap2_mfi_auth_mark_authenticated(iap2_mfi_auth_t *auth);
void iap2_mfi_auth_mark_failed(iap2_mfi_auth_t *auth);
bool iap2_mfi_auth_is_authenticated(const iap2_mfi_auth_t *auth);
iap2_mfi_auth_stage_t iap2_mfi_auth_stage(const iap2_mfi_auth_t *auth);
const char *iap2_mfi_auth_stage_name(iap2_mfi_auth_stage_t stage);
