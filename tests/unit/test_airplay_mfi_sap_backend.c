/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Compile the real responder with a silent no-op AES-CTR backend, matching
 * the pre-fix SDK contract violation. It must fail before signing/sending. */
#define mbedtls_aes_crypt_ctr mock_aes_ctr
#include "../../core/airplay/airplay_mfi_sap.c"
#undef mbedtls_aes_crypt_ctr
#include <assert.h>
#include <stdio.h>

int mock_aes_ctr(mbedtls_aes_context *ctx, size_t length, size_t *offset,
                 unsigned char counter[16], unsigned char stream[16],
                 const unsigned char *input, unsigned char *output)
{
    (void)ctx; (void)offset; (void)counter; (void)stream;
    memcpy(output, input, length);
    return 0;
}
static int certificate(uint8_t *out, size_t cap, size_t *length)
{
    assert(cap >= 16); memset(out, 0xc3, 16); *length = 16; return RTCP_OK;
}
static int sign_unreachable(const uint8_t *challenge, size_t length,
                            uint8_t *signature, size_t cap, size_t *sig_length)
{
    (void)challenge; (void)length; (void)signature; (void)cap; (void)sig_length;
    assert(!"signing with a broken encryption backend"); return RTCP_EIO;
}
int main(void)
{
    static const struct rtcp_mfi chip = {.sign=sign_unreachable, .read_cert=certificate};
    airplay_mfi_sap_provider_t provider = {.mfi=&chip, .protocol_major=2};
    assert(airplay_mfi_sap_set_provider(&provider));
    unsigned char request[33] = {1, 9}, output[2048];
    memset(output, 0xa5, sizeof(output));
    size_t length = 99;
    assert(airplay_mfi_sap_respond(request, sizeof(request), output,
           sizeof(output), &length) == AIRPLAY_MFI_SAP_CRYPTO_FAILED);
    assert(length == 0);
    for (size_t i=0; i<sizeof(output); ++i) assert(output[i] == 0xa5);
    airplay_mfi_sap_clear_provider();
    puts("MFiSAP rejects a backend reporting successful CTR without encryption");
    return 0;
}
