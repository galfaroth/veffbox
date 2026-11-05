# SPDX-License-Identifier: GPL-3.0-only
# Host build of the portable core: every host/test_*.c is one test program linked with all of core/.
# The FM-1 image is built by tools/fm1_build.sh on the Linux build host.
CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-function
CORE = $(wildcard core/*.c)
TESTS = $(patsubst %.c,%,$(wildcard host/test_*.c))


.PHONY: test tables clean shots emu uitest
MNM ?= ..
test: $(TESTS)
	@mkdir -p host/out
	@for t in $(TESTS); do ./$$t || exit 1; done

host/test_%: host/test_%.c $(CORE) $(wildcard core/*.h)
	$(CC) $(CFLAGS) -o $@ $< $(CORE) -lm

# the FM-1 screens as PNG (host/shots/), drawn by the firmware's own UI code with MNM-1's gfx.c and font
shots: host/shot.c fw/vb_app.c $(CORE)
	$(CC) -std=c11 -O1 -w -I$(MNM)/firmware/src -I$(MNM)/build/gen -o host/shot host/shot.c $(CORE) -lz -lm
	@mkdir -p host/shots
	./host/shot

# the firmware's UI on the host, flash in RAM: steps, OCT, VOICING / BASS, songs, saving, a restart
uitest: host/ui_test.c host/vb_host.h fw/vb_app.c $(CORE)
	$(CC) -std=c11 -O1 -w -I$(MNM)/firmware/src -I$(MNM)/build/gen -o host/ui_test host/ui_test.c $(CORE) -lm
	./host/ui_test

# the web emulator: the firmware as WebAssembly for site/emu/ (emscripten's emcc on the PATH)
EMU_FN = _emu_fb,_emu_flash,_emu_flash_size,_emu_flash_writes,_emu_audio_buf,_emu_init,_emu_frame,_emu_button,_emu_led,_emu_encoder,_emu_key,_emu_master,_emu_audio,_emu_playing
emu: host/emu.c host/vb_host.h fw/vb_app.c $(CORE)
	emcc -std=c11 -O2 -w -I$(MNM)/firmware/src -I$(MNM)/build/gen host/emu.c $(CORE) -o site/emu/veffbox.js \
	  -sMODULARIZE=1 -sEXPORT_NAME=Veffbox -sENVIRONMENT=web -sALLOW_MEMORY_GROWTH=1 \
	  -sEXPORTED_FUNCTIONS=$(EMU_FN) -sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAPU16,HEAPF32

tables:
	python3 tools/gen_tables.py > core/vb_tables.h

clean:
	rm -rf $(TESTS) host/out host/shot host/shots host/ui_test
