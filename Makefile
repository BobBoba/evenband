CC      ?= cc
CFLAGS  ?= -O2 -g
WARN    := -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion
BUNDLE  := build/evenband.lv2
LV2DIR  ?= $(HOME)/.lv2
DESTDIR ?=

# The user interfaces are optional: each one is built when its libraries are found.
HAVE_X11  := $(shell pkg-config --exists x11 cairo 2>/dev/null && echo 1)
HAVE_GTK3 := $(shell pkg-config --exists gtk+-3.0 cairo 2>/dev/null && echo 1)
UI_FLAGS  := $(if $(HAVE_X11),--x11) $(if $(HAVE_GTK3),--gtk3)
UI_LIBS   := $(if $(HAVE_X11),$(BUNDLE)/evenband_ui_x11.so) $(if $(HAVE_GTK3),$(BUNDLE)/evenband_ui_gtk3.so)
UI_CORE   := src/eb_ui_core.c src/eb_presets.c src/eb_spawn.c

all: $(BUNDLE)/evenband.so $(UI_LIBS) $(BUNDLE)/evenband.ttl $(BUNDLE)/manifest.ttl

build/eb_ports.h: lv2/gen_ttl.py
	mkdir -p build
	python3 -I lv2/gen_ttl.py header > $@

$(BUNDLE)/evenband.so: src/eb_lv2.c src/eb_dsp.c src/eb_dsp.h build/eb_ports.h
	mkdir -p $(BUNDLE)
	$(CC) $(CFLAGS) $(WARN) -Ibuild -fPIC -shared -fvisibility=hidden $(LDFLAGS) -o $@ src/eb_lv2.c src/eb_dsp.c -lm

$(BUNDLE)/evenband_ui_x11.so: src/eb_ui_x11.c $(UI_CORE) src/eb_ui_core.h src/eb_presets.h src/eb_spawn.h build/eb_ports.h
	mkdir -p $(BUNDLE)
	$(CC) $(CFLAGS) $(WARN) -Ibuild -fPIC -shared -fvisibility=hidden $$(pkg-config --cflags x11 cairo) $(LDFLAGS) -o $@ src/eb_ui_x11.c $(UI_CORE) $$(pkg-config --libs x11 cairo) -lm

$(BUNDLE)/evenband_ui_gtk3.so: src/eb_ui_gtk3.c $(UI_CORE) src/eb_ui_core.h src/eb_presets.h src/eb_spawn.h build/eb_ports.h
	mkdir -p $(BUNDLE)
	$(CC) $(CFLAGS) $(WARN) -Ibuild -fPIC -shared -fvisibility=hidden $$(pkg-config --cflags gtk+-3.0 cairo) $(LDFLAGS) -o $@ src/eb_ui_gtk3.c $(UI_CORE) $$(pkg-config --libs gtk+-3.0 cairo) -lm

$(BUNDLE)/evenband.ttl: lv2/gen_ttl.py
	mkdir -p $(BUNDLE)
	python3 -I lv2/gen_ttl.py ttl $(UI_FLAGS) > $@

$(BUNDLE)/manifest.ttl: lv2/gen_ttl.py
	mkdir -p $(BUNDLE)
	python3 -I lv2/gen_ttl.py manifest $(UI_FLAGS) > $@

build/test_dsp: tests/test_dsp.c src/eb_dsp.c src/eb_dsp.h
	mkdir -p build
	$(CC) $(CFLAGS) $(WARN) $(LDFLAGS) -o $@ tests/test_dsp.c src/eb_dsp.c -lm

build/test_ui: tests/test_ui.c $(UI_CORE) src/eb_ui_core.h src/eb_presets.h src/eb_spawn.h build/eb_ports.h
	$(CC) $(CFLAGS) $(WARN) -Isrc -Ibuild $$(pkg-config --cflags cairo) $(LDFLAGS) -o $@ tests/test_ui.c $(UI_CORE) $$(pkg-config --libs cairo) -lm

# Developer tools: load a user interface like a host does (see tests/ui_harness.c).
build/ui_harness_x11: tests/ui_harness.c build/eb_ports.h
	$(CC) $(CFLAGS) -Wall -Ibuild $$(pkg-config --cflags x11) -o $@ tests/ui_harness.c $$(pkg-config --libs x11) -ldl

build/ui_harness_gtk3: tests/ui_harness.c build/eb_ports.h
	$(CC) $(CFLAGS) -Wall -DHARNESS_GTK3 -Ibuild $$(pkg-config --cflags gtk+-3.0) -o $@ tests/ui_harness.c $$(pkg-config --libs gtk+-3.0) -ldl

test: build/test_dsp build/test_ui
	./build/test_dsp
	./build/test_ui
	python3 -I tests/test_amx2lv2.py

install: all
	install -d $(DESTDIR)$(LV2DIR)/evenband.lv2
	install -m 755 $(BUNDLE)/*.so $(DESTDIR)$(LV2DIR)/evenband.lv2/
	install -m 644 $(BUNDLE)/*.ttl $(DESTDIR)$(LV2DIR)/evenband.lv2/

# Convert your own BreakawayOne presets (local use only, never commit the result).
AMX_DIR ?= $(HOME)/.local/share/wineprefixes/breakawayone/drive_c/users/$(USER)/AppData/Local/BreakawayOne/proc_presets
PRESET_BUNDLE ?= $(HOME)/.lv2/evenband-presets.lv2
presets:
	python3 -I tools/amx2lv2.py "$(AMX_DIR)" "$(PRESET_BUNDLE)"

clean:
	rm -rf build

.PHONY: all test install presets clean
