# SPDX-License-Identifier: GPL-3.0-or-later
CC       ?= cc
AR       ?= ar
CFLAGS   ?= -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined
CPPFLAGS += -Iinclude -Icore -Icore/mfi \
            -Icore/iap2/include -Icore/airplay/include \
            -Icore/media/include -Icore/input/include
LDLIBS   += -lm
BUILD    := build

# mbedTLS provides ChaCha20-Poly1305, HKDF, bignum and the pairing
# primitives.  Point RTCP_MBEDTLS_DIR at an mbedTLS 2.28 checkout (the
# RT-Thread SDK ships one), or run scripts/fetch-mbedtls.sh.
MBEDTLS_DIR ?= $(if $(RTCP_MBEDTLS_DIR),$(RTCP_MBEDTLS_DIR),third_party/mbedtls)
MBEDTLS_HDR := $(MBEDTLS_DIR)/include/mbedtls/chachapoly.h
MBEDTLS_CPPFLAGS := -I$(MBEDTLS_DIR)/include -Iconfig \
                    -DMBEDTLS_CONFIG_FILE='"mbedtls_rtcp_host.h"'
MBEDTLS_LIB := $(BUILD)/mbedtls/libmbedcrypto.a
CPPFLAGS += $(MBEDTLS_CPPFLAGS)

CORE_SRCS := $(wildcard core/*/*.c)
CORE_OBJS := $(CORE_SRCS:%.c=$(BUILD)/%.o)
LIB       := $(BUILD)/librtcp_core.a

TESTS := test_mfi2c \
         test_iap2_core test_iap2_identity test_iap2_carplay test_iap2_mfi_auth \
         test_airplay_bplist test_airplay_rtsp_codec test_airplay_tlv8 \
         test_airplay_screen_codec test_airplay_timing test_airplay_hid_plist \
         test_airplay_control_stream test_airplay_screen_stream \
         test_input_bridge test_media_timestamp
BINS := $(addprefix $(BUILD)/,$(TESTS))

all: $(BINS)

$(MBEDTLS_LIB):
	@if [ ! -f "$(MBEDTLS_HDR)" ]; then \
	    echo "mbedTLS not found at '$(MBEDTLS_DIR)'." >&2; \
	    echo "  run scripts/fetch-mbedtls.sh or set RTCP_MBEDTLS_DIR." >&2; \
	    exit 2; \
	fi
	./scripts/build-mbedtls-host.sh $(MBEDTLS_DIR)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(LIB): $(CORE_OBJS)
	$(AR) rcs $@ $^

$(BUILD)/test_mfi2c: tests/test_mfi2c.c $(LIB) $(MBEDTLS_LIB)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(LIB) $(MBEDTLS_LIB) $(LDLIBS)

$(BUILD)/test_%: tests/unit/test_%.c $(LIB) $(MBEDTLS_LIB)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(LIB) $(MBEDTLS_LIB) $(LDLIBS)

check: all
	@for t in $(BINS); do echo "== $$t =="; ./$$t; done
	./check-boundary.sh

clean:
	rm -rf $(BUILD)

.PHONY: all check clean
