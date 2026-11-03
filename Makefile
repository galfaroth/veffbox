# SPDX-License-Identifier: GPL-3.0-only
# Host build of the portable core. The FM-1 build will come from the MNM-1 platform layer.
CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-function
CORE = core/vb_drum.c core/vb_mod.c

.PHONY: test tables clean
test: host/test_core
	@mkdir -p host/out
	./host/test_core

host/test_core: host/test_core.c $(CORE) core/*.h
	$(CC) $(CFLAGS) -o $@ host/test_core.c $(CORE)

tables:
	python3 tools/gen_tables.py > core/vb_tables.h

clean:
	rm -rf host/test_core host/out
