/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Host-side mbedTLS configuration for rt-carplay unit tests.
 *
 * mbedTLS 2.28 does not enable ChaCha20-Poly1305 or HKDF in its default
 * configuration, and AirPlay pairing/DataStream needs both.  The header is
 * passed with -DMBEDTLS_CONFIG_FILE so it applies to every mbedTLS source
 * file and to the rt-carplay modules that include mbedTLS headers.
 *
 * Embedded ports normally take the configuration from the RT-Thread SDK
 * (Kconfig / cconfig.h) instead of this file.
 */
#ifndef RTCP_MBEDTLS_HOST_CONFIG_H
#define RTCP_MBEDTLS_HOST_CONFIG_H

#include "mbedtls/config.h"

#define MBEDTLS_CHACHAPOLY_C
#define MBEDTLS_HKDF_C

#endif /* RTCP_MBEDTLS_HOST_CONFIG_H */
