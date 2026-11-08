#include <assert.h>
#include <stdio.h>

#include "iap2_mfi_auth.h"

int main(void)
{
    iap2_mfi_auth_t auth;
    iap2_mfi_auth_reset(&auth);
    assert(iap2_mfi_auth_stage(&auth) == IAP2_MFI_AUTH_IDLE);
    assert(!iap2_mfi_auth_is_authenticated(&auth));
    assert(!iap2_mfi_auth_mark_authenticated(&auth));
    assert(iap2_mfi_auth_can_respond_to_challenge(&auth));
    assert(iap2_mfi_auth_mark_challenge_response_queued(&auth));
    assert(iap2_mfi_auth_stage(&auth) == IAP2_MFI_AUTH_CHALLENGE_RESPONSE_PENDING);
    assert(!iap2_mfi_auth_can_respond_to_challenge(&auth));
    assert(!iap2_mfi_auth_mark_authenticated(&auth));
    assert(iap2_mfi_auth_mark_challenge_response_acked(&auth));
    assert(iap2_mfi_auth_stage(&auth) == IAP2_MFI_AUTH_CHALLENGE_RESPONSE_ACKED);
    assert(iap2_mfi_auth_can_respond_to_challenge(&auth));
    assert(iap2_mfi_auth_mark_challenge_response_queued(&auth));
    assert(!iap2_mfi_auth_mark_authenticated(&auth));
    assert(iap2_mfi_auth_mark_challenge_response_acked(&auth));
    assert(iap2_mfi_auth_mark_authenticated(&auth));
    assert(iap2_mfi_auth_is_authenticated(&auth));
    assert(!iap2_mfi_auth_can_respond_to_challenge(&auth));

    iap2_mfi_auth_mark_failed(&auth);
    assert(!iap2_mfi_auth_is_authenticated(&auth));
    assert(!iap2_mfi_auth_mark_challenge_response_acked(&auth)); /* A late ACK cannot revive a failed exchange. */
    assert(!iap2_mfi_auth_mark_authenticated(&auth));
    assert(!iap2_mfi_auth_can_respond_to_challenge(&auth));
    iap2_mfi_auth_reset(&auth);
    assert(iap2_mfi_auth_can_respond_to_challenge(&auth));
    assert(iap2_mfi_auth_stage_name(iap2_mfi_auth_stage(&auth))[0] == 'i');

    puts("iap2_mfi_auth host test passed");
    return 0;
}
