# SPDX-License-Identifier: GPL-3.0-or-later
CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined
CPPFLAGS += -Iinclude -Icore -Icore/mfi -Icore/iap2/include
BUILD   := build

TESTS := test_mfi2c test_iap2_core test_iap2_identity test_iap2_carplay test_iap2_mfi_auth
BINS  := $(addprefix $(BUILD)/,$(TESTS))

all: $(BINS)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/test_mfi2c: core/mfi/mfi2c.c tests/test_mfi2c.c | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $^

$(BUILD)/test_iap2_core: core/iap2/iap2_core.c tests/unit/test_iap2_core.c | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $^

$(BUILD)/test_iap2_identity: core/iap2/iap2_identity.c tests/unit/test_iap2_identity.c | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $^

$(BUILD)/test_iap2_carplay: core/iap2/iap2_carplay.c tests/unit/test_iap2_carplay.c | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $^

$(BUILD)/test_iap2_mfi_auth: core/iap2/iap2_mfi_auth.c tests/unit/test_iap2_mfi_auth.c | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $^

check: all
	@for t in $(BINS); do echo "== $$t =="; ./$$t; done
	./check-boundary.sh

clean:
	rm -rf $(BUILD)

.PHONY: all check clean
