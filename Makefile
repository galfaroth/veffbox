# SPDX-License-Identifier: GPL-3.0-only
# Host build of the portable core: every host/test_*.c is one test program linked with all of core/.
# The FM-1 image is built by tools/fm1_build.sh on the Linux build host.
CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-function
CORE = $(wildcard core/*.c)
TESTS = $(patsubst %.c,%,$(wildcard host/test_*.c))


.PHONY: test tables clean shots
MNM ?= ..
test: $(TESTS)
	@mkdir -p host/out
	@for t in $(TESTS); do ./$$t || exit 1; done

host/test_%: host/test_%.c $(CORE) $(wildcard core/*.h)
	$(CC) $(CFLAGS) -o $@ $< $(CORE)

# the FM-1 screens as PNG (host/shots/), drawn by the firmware's own UI code with MNM-1's gfx.c and font
shots: host/shot.c fw/vb_app.c $(CORE)
	$(CC) -std=c11 -O1 -w -I$(MNM)/firmware/src -I$(MNM)/build/gen -o host/shot host/shot.c $(CORE) -lz
	@mkdir -p host/shots
	./host/shot

tables:
	python3 tools/gen_tables.py > core/vb_tables.h

clean:
	rm -rf $(TESTS) host/out host/shot host/shots
