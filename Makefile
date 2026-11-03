# SPDX-License-Identifier: GPL-3.0-only
# Host build of the portable core: every host/test_*.c is one test program linked with all of core/.
# The FM-1 image is built by tools/fm1_build.sh on the Linux build host.
CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-function
CORE = $(wildcard core/*.c)
TESTS = $(patsubst %.c,%,$(wildcard host/test_*.c))

.PHONY: test tables clean
test: $(TESTS)
	@mkdir -p host/out
	@for t in $(TESTS); do ./$$t || exit 1; done

host/test_%: host/test_%.c $(CORE) $(wildcard core/*.h)
	$(CC) $(CFLAGS) -o $@ $< $(CORE)

tables:
	python3 tools/gen_tables.py > core/vb_tables.h

clean:
	rm -rf $(TESTS) host/out
