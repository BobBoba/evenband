CC      ?= cc
CFLAGS  ?= -O2 -g
WARN    := -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion
BUNDLE  := build/evenband.lv2
LV2DIR  ?= $(HOME)/.lv2
DESTDIR ?=

all: $(BUNDLE)/evenband.so

$(BUNDLE)/evenband.so: src/eb_lv2.c src/eb_dsp.c src/eb_dsp.h lv2/manifest.ttl lv2/gen_ttl.py
	mkdir -p $(BUNDLE)
	$(CC) $(CFLAGS) $(WARN) -fPIC -shared -fvisibility=hidden -o $@ src/eb_lv2.c src/eb_dsp.c -lm
	cp lv2/manifest.ttl $(BUNDLE)/
	python3 -I lv2/gen_ttl.py > $(BUNDLE)/evenband.ttl

build/test_dsp: tests/test_dsp.c src/eb_dsp.c src/eb_dsp.h
	mkdir -p build
	$(CC) $(CFLAGS) $(WARN) -o $@ tests/test_dsp.c src/eb_dsp.c -lm

test: build/test_dsp
	./build/test_dsp
	python3 -I tests/test_amx2lv2.py

install: all
	install -d $(DESTDIR)$(LV2DIR)/evenband.lv2
	install -m 755 $(BUNDLE)/evenband.so $(DESTDIR)$(LV2DIR)/evenband.lv2/
	install -m 644 $(BUNDLE)/manifest.ttl $(BUNDLE)/evenband.ttl $(DESTDIR)$(LV2DIR)/evenband.lv2/

# Convert your own BreakawayOne presets (local use only, never commit the result).
AMX_DIR ?= $(HOME)/.local/share/wineprefixes/breakawayone/drive_c/users/$(USER)/AppData/Local/BreakawayOne/proc_presets
PRESET_BUNDLE ?= $(HOME)/.lv2/evenband-presets.lv2
presets:
	python3 -I tools/amx2lv2.py "$(AMX_DIR)" "$(PRESET_BUNDLE)"

clean:
	rm -rf build

.PHONY: all test install presets clean
