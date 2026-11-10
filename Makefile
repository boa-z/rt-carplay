# SPDX-License-Identifier: GPL-3.0-or-later
CC       ?= cc
AR       ?= ar
CFLAGS   ?= -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined
CPPFLAGS += -Iinclude -Icore -Icore/mfi \
            -Icore/iap2/include -Icore/airplay/include -Icore/crypto/include \
            -Icore/media/include -Icore/input/include -Icore/discovery/include \
            -Icore/usb/include \
            -Ithird_party/ed25519 -Iport/linux/include -Itests/support
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
# Vendored portable Ed25519 (zlib); used only by core/crypto.
ED25519_SRCS := $(wildcard third_party/ed25519/*.c)
ED25519_OBJS := $(ED25519_SRCS:%.c=$(BUILD)/%.o)
LIB       := $(BUILD)/librtcp_core.a

PORT_LINUX_SRCS := $(wildcard port/linux/*.c)
PORT_LINUX_OBJS := $(PORT_LINUX_SRCS:%.c=$(BUILD)/%.o)
PORT_LINUX_LIB  := $(BUILD)/librtcp_port_linux.a

TESTS := test_mfi2c \
         test_iap2_core test_iap2_identity test_iap2_carplay test_iap2_mfi_auth \
         test_iap2_parameter test_iap2_session \
         test_airplay_bplist test_airplay_info_plist test_airplay_rtsp_codec test_airplay_tlv8 \
         test_airplay_screen_codec test_airplay_timing test_airplay_hid_plist \
         test_airplay_control_stream test_airplay_screen_stream \
         test_airplay_audio_stream test_airplay_mfi_sap \
         test_airplay_srp6a test_airplay_pair_setup test_airplay_pair_verify \
         test_airplay_rtsp_pairing test_airplay_rtsp_session \
         test_airplay_pairing_store \
         test_airplay_control_cipher test_airplay_control_port \
         test_airplay_timing_port test_airplay_event_port \
         test_airplay_screen_server test_airplay_rtsp_server \
         test_airplay_pairing_e2e test_airplay_ed25519 \
         test_discovery test_mdns_responder test_mdns_browser test_carplay_ctrl \
         test_usb_device test_usbmux test_usbmux_host test_lockdown test_plist \
         test_lockdown_pair test_lockdown_tls test_lockdown_session \
         test_port_linux test_input_bridge \
         test_media_timestamp

# These tests drive the poll seams through the POSIX port.
PORT_TESTS := test_airplay_control_port test_airplay_timing_port \
              test_airplay_event_port test_airplay_screen_server \
              test_airplay_audio_stream test_airplay_rtsp_server \
              test_airplay_pairing_e2e
BINS := $(addprefix $(BUILD)/,$(TESTS))
TEST_SUPPORT_OBJ := $(BUILD)/tests/support/airplay_srp6a_test_client.o

SIM := $(BUILD)/rt-carplay-sim
SIM_SRCS := $(wildcard examples/linux_sim/*.c)

all: $(BINS) $(SIM)

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

# The vendored Ed25519 field arithmetic relies on two's-complement signed
# shifts; that is well-defined under -fwrapv and not a UBSan finding in our
# code, so the host sanitizers are disabled for it only.
$(BUILD)/third_party/%.o: third_party/%.c
	@mkdir -p $(dir $@)
	$(CC) $(filter-out -fsanitize=%,$(CFLAGS)) -fwrapv $(CPPFLAGS) -c $< -o $@

$(LIB): $(CORE_OBJS) $(ED25519_OBJS)
	$(AR) rcs $@ $^

$(BUILD)/test_mfi2c: tests/test_mfi2c.c $(LIB) $(MBEDTLS_LIB)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(LIB) $(MBEDTLS_LIB) $(LDLIBS)

$(PORT_LINUX_LIB): $(PORT_LINUX_OBJS)
	$(AR) rcs $@ $^

$(SIM): $(SIM_SRCS) $(LIB) $(PORT_LINUX_LIB) $(MBEDTLS_LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(SIM_SRCS) $(LIB) $(PORT_LINUX_LIB) $(MBEDTLS_LIB) $(LDLIBS)

$(BUILD)/test_%: tests/unit/test_%.c $(LIB) $(MBEDTLS_LIB) $(TEST_SUPPORT_OBJ)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(LIB) $(TEST_SUPPORT_OBJ) $(MBEDTLS_LIB) $(LDLIBS)

# Explicit rules win over the pattern rule above.
define PORT_TEST_RULE
$(BUILD)/$(1): tests/unit/$(1).c $(LIB) $(PORT_LINUX_LIB) $(MBEDTLS_LIB) $(TEST_SUPPORT_OBJ)
	$$(CC) $$(CFLAGS) $$(CPPFLAGS) -o $$@ $$< $(LIB) $(PORT_LINUX_LIB) $(TEST_SUPPORT_OBJ) $(MBEDTLS_LIB) $$(LDLIBS)
endef
$(foreach t,$(PORT_TESTS),$(eval $(call PORT_TEST_RULE,$(t))))

$(BUILD)/test_port_linux: tests/unit/test_port_linux.c $(PORT_LINUX_LIB)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(PORT_LINUX_LIB) -lpthread

# The TLS and session tests run an in-process mbedTLS server on other
# threads.
$(BUILD)/test_lockdown_tls: tests/unit/test_lockdown_tls.c $(LIB) $(MBEDTLS_LIB) $(TEST_SUPPORT_OBJ)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(LIB) $(TEST_SUPPORT_OBJ) $(MBEDTLS_LIB) -lpthread

$(BUILD)/test_lockdown_session: tests/unit/test_lockdown_session.c $(LIB) $(MBEDTLS_LIB) $(TEST_SUPPORT_OBJ)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(LIB) $(TEST_SUPPORT_OBJ) $(MBEDTLS_LIB) -lpthread

check: all
	@status=0; \
	for t in $(BINS); do \
	    echo "== $$t =="; \
	    ./$$t || status=1; \
	done; \
	./check-boundary.sh || status=1; \
	exit $$status

clean:
	rm -rf $(BUILD)

.PHONY: all check clean
