# SPDX-License-Identifier: GPL-3.0-or-later
CC       ?= cc
AR       ?= ar
CFLAGS   ?= -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -MMD -MP
CPPFLAGS += -Iinclude -Icore -Icore/mfi \
            -Icore/iap2/include -Icore/airplay/include -Icore/crypto/include \
            -Icore/media/include -Icore/input/include -Icore/discovery/include \
            -Icore/usb/include -Icore/audio/include -Iport/rtthread/include \
            -Ithird_party/ed25519 -Iport/linux/include -Itests/support
AUDIO_CODECS ?= 1
ifeq ($(AUDIO_CODECS),1)
CPPFLAGS += -DRTCP_AUDIO_CODECS -Ithird_party/audio/faad2/include -Ithird_party/audio/opus/include -Ithird_party/audio/speexdsp/include
AUDIO_LIBS = $(BUILD)/audio-deps/librtcp_faad.a $(BUILD)/audio-deps/librtcp_opus.a $(BUILD)/audio-deps/librtcp_speexdsp.a
LDLIBS += $(AUDIO_LIBS)
endif
LDLIBS   += -lm
BUILD    ?= build

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

TESTS := test_rtthread_clock test_audio_input test_audio_sink test_audio_engine test_mfi2c test_pairing_files \
         test_iap2_core test_iap2_identity test_iap2_carplay test_iap2_mfi_auth \
         test_iap2_parameter test_iap2_session test_iap2_stream \
         test_airplay_bplist test_airplay_info_plist test_airplay_rtsp_codec test_airplay_tlv8 \
         test_airplay_screen_codec test_airplay_timing test_airplay_hid_plist \
         test_airplay_control_stream test_airplay_screen_stream \
         test_airplay_audio_stream test_airplay_mfi_sap test_airplay_mfi_sap_backend \
         test_airplay_srp6a test_airplay_pair_setup test_airplay_pair_verify \
         test_airplay_rtsp_pairing test_airplay_rtsp_session test_airplay_iap_tunnel \
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

$(BUILD)/audio-deps/librtcp_faad.a $(BUILD)/audio-deps/librtcp_opus.a $(BUILD)/audio-deps/librtcp_speexdsp.a &: third_party/audio/CMakeLists.txt $(wildcard third_party/audio/faad2/libfaad/*.[ch] third_party/audio/opus/*/*.[ch] third_party/audio/opus/*/*/*.[ch] third_party/audio/speexdsp/libspeexdsp/*.[ch])
	cmake -S third_party/audio -B $(BUILD)/audio-deps
	cmake --build $(BUILD)/audio-deps -j8
	touch $(AUDIO_LIBS)

$(BINS) $(SIM): $(AUDIO_LIBS)

$(MBEDTLS_LIB):
	@if [ ! -f "$(MBEDTLS_HDR)" ]; then \
	    echo "mbedTLS not found at '$(MBEDTLS_DIR)'." >&2; \
	    echo "  run scripts/fetch-mbedtls.sh or set RTCP_MBEDTLS_DIR." >&2; \
	    exit 2; \
	fi
	./scripts/build-mbedtls-host.sh $(MBEDTLS_DIR) $(abspath $(BUILD)/mbedtls)

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

# This test includes the platform sources to intercept SDK filesystem calls.
# Track them explicitly so editing a seam cannot reuse a stale test binary.
$(BUILD)/test_pairing_files: tests/unit/test_pairing_files.c \
    examples/rtthread_app/pairing_files.c examples/rtthread_app/pairing_files.h \
    examples/rtthread_app/pairing_manager.c examples/rtthread_app/pairing_manager.h \
    examples/rtthread_app/usb_pair_cache.c examples/rtthread_app/usb_pair_cache.h

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

# Optional SDK-port regression using official CherryUSB headers (no SDK build).
# make check-lubanlite-usb CHERRYUSB_ROOT=/path/to/CherryUSB
.PHONY: check-lubanlite-usb
check-lubanlite-usb:
	@test -f "$(CHERRYUSB_ROOT)/core/usbh_core.h" || (echo "Set CHERRYUSB_ROOT to official CherryUSB"; exit 1)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -DLPKG_RT_CARPLAY_LUBANLITE_USB \
	  -Itests/support/lubanlite_usb -Iport/luban-lite/include \
	  -I$(CHERRYUSB_ROOT)/core -I$(CHERRYUSB_ROOT)/common \
	  -I$(CHERRYUSB_ROOT)/class/hub -ffunction-sections -fdata-sections \
	  tests/unit/test_lubanlite_usb_dma.c core/usb/usb_device.c -Wl,--gc-sections -o $(BUILD)/test_lubanlite_usb_dma
	$(BUILD)/test_lubanlite_usb_dma

# Actual video sink with a bounded MPP model and the SDK's public headers.
.PHONY: check-lubanlite-video
check-lubanlite-video:
	@test -f "$(SDK_ROOT)/packages/artinchip/mpp/include/mpp_decoder.h" || (echo "Set SDK_ROOT to Luban-Lite SDK"; exit 1)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -Itests/support/lubanlite_video \
	  -I$(SDK_ROOT)/packages/artinchip/mpp/include \
	  -I$(SDK_ROOT)/bsp/artinchip/include/uapi \
	  -ffunction-sections -fdata-sections tests/unit/test_lubanlite_video.c \
	  -Wl,--gc-sections -o $(BUILD)/test_lubanlite_video
	$(BUILD)/test_lubanlite_video
	$(BUILD)/test_lubanlite_video queue

.PHONY: check-lubanlite-touch
check-lubanlite-touch:
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -Itests/support/lubanlite_touch \
	  tests/unit/test_lubanlite_touch.c -o $(BUILD)/test_lubanlite_touch
	$(BUILD)/test_lubanlite_touch

# Dual-mode demo lifecycle with actual SDK LVGL public headers.
.PHONY: check-lubanlite-demo
check-lubanlite-demo:
	@test -n "$(SDK_ROOT)" || (echo 'set SDK_ROOT to Luban-Lite checkout'; exit 1)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -DLV_CONF_SKIP -DRTCARPLAY_IAP2 -DLPKG_RT_CARPLAY_LUBANLITE_WIFI \
	  -Itests/support/lubanlite_demo -Iport/luban-lite/include \
	  -I$(SDK_ROOT)/packages/artinchip/lvgl-ui/lvgl_v9/lvgl \
	  -ffunction-sections -fdata-sections tests/unit/test_lubanlite_demo.c examples/rtthread_app/pairing_manager.c \
	  -Wl,--gc-sections -o $(BUILD)/test_lubanlite_demo
	$(BUILD)/test_lubanlite_demo

.PHONY: check-lubanlite-bonds
check-lubanlite-bonds:
	@test -n "$(SDK_ROOT)" || (echo 'set SDK_ROOT to Luban-Lite checkout'; exit 1)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -DCONFIG_X86 -DCONFIG_64BIT -DRT_IPC_FLAG_FIFO=0 \
	  -Itests/support/lubanlite_demo \
	  -I$(SDK_ROOT)/packages/third-party/zephyr-bluetooth/include \
	  -I$(SDK_ROOT)/packages/third-party/zephyr-bluetooth/porting \
	  tests/unit/test_lubanlite_bt_settings.c \
	  -Wl,-T,tests/support/lubanlite_demo/settings_sections.ld -o $(BUILD)/test_lubanlite_bt_settings
	$(BUILD)/test_lubanlite_bt_settings
	$(BUILD)/test_lubanlite_bt_settings negative-errno
	SDK_ROOT=$(SDK_ROOT) python3 tests/run_lubanlite_br_restore.py

.PHONY: check-rtthread-multicast
check-rtthread-multicast:
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -D_DEFAULT_SOURCE -DRT_USING_SAL -DNETDEV_IPV6=1 \
	  -Dclosesocket=close -include unistd.h -Itests/support/lubanlite_demo \
	  -Iport/rtthread/include tests/unit/test_rtthread_multicast.c port/rtthread/rtcp_net_rtthread.c \
	  -o $(BUILD)/test_rtthread_multicast
	$(BUILD)/test_rtthread_multicast

.PHONY: check-lubanlite-handoff
check-lubanlite-handoff:
	python3 tests/run_lubanlite_handoff.py

.PHONY: check-lubanlite-reconnect
check-lubanlite-reconnect:
	python3 tests/run_lubanlite_reconnect.py

.PHONY: check-lubanlite-capabilities
check-lubanlite-capabilities:
	python3 tests/run_lubanlite_capabilities.py $(SDK_ROOT)

# Actual board sink with a pthread-backed RT-Thread/device seam.
$(BUILD)/test_audio_sink: tests/unit/test_audio_sink.c examples/rtthread_app/audio_sink_lubanlite.c tests/support/lubanlite_audio/rtthread_host.c $(LIB) $(MBEDTLS_LIB)
	$(CC) $(CFLAGS) $(CPPFLAGS) -Itests/support/lubanlite_audio -o $@ $< tests/support/lubanlite_audio/rtthread_host.c $(LIB) $(MBEDTLS_LIB) $(LDLIBS) -lpthread

$(BUILD)/test_audio_input: tests/unit/test_audio_input.c examples/rtthread_app/audio_input_lubanlite.c tests/support/lubanlite_audio/rtthread_host.c $(LIB) $(PORT_LINUX_LIB) $(MBEDTLS_LIB)
	$(CC) $(CFLAGS) $(CPPFLAGS) -Itests/support/lubanlite_audio -Iport/luban-lite/include -o $@ $< tests/support/lubanlite_audio/rtthread_host.c $(LIB) $(PORT_LINUX_LIB) $(MBEDTLS_LIB) $(LDLIBS) -lpthread

# Track header and included platform-source dependencies for incremental tests.
-include $(wildcard $(BUILD)/core/*/*.d $(BUILD)/port/*/*.d $(BUILD)/tests/support/*.d $(BUILD)/*.d)

$(BUILD)/test_rtthread_clock: tests/unit/test_rtthread_clock.c port/rtthread/rtcp_os_rtthread.c $(LIB) $(MBEDTLS_LIB)
	$(CC) $(CFLAGS) $(CPPFLAGS) -Itests/support/rtthread_clock -o $@ $< $(LIB) $(MBEDTLS_LIB) $(LDLIBS) -lpthread

.PHONY: check-audio-discovery
check-audio-discovery: $(SIM)
	python3 tests/run_audio_discovery.py $(SIM)
