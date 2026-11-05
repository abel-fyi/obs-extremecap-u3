# OBS plugin for the AVerMedia ExtremeCap U3 (CV710).
#   make           build the plugin
#   make install   copy it into your OBS plugins folder
#   make smoke     build the hardware test (build/smoke SECONDS)

OBS_VERSION ?= 32.2.2
CFLAGS += -O2 -std=c11 -Wall -Wextra -fPIC -D_POSIX_C_SOURCE=200809L -D_DARWIN_C_SOURCE

ifeq ($(shell uname),Darwin)
# macOS: libobs (+ SIMDe) headers fetched for the matching OBS release; libusb static.
LIBUSB   ?= $(shell brew --prefix libusb)
OBS_SRC   = build/obs-studio
CFLAGS   += -I$(OBS_SRC)/libobs -Ibuild/simde -Ibuild/include -I$(LIBUSB)/include/libusb-1.0
USB_LIBS  = $(LIBUSB)/lib/libusb-1.0.a -framework IOKit -framework CoreFoundation -framework Security
PLUGIN    = build/cv710.plugin/Contents/MacOS/cv710
PLUGIN_LDFLAGS = -bundle -undefined dynamic_lookup
DEST      = $(HOME)/Library/Application Support/obs-studio/plugins/cv710.plugin
else
# Linux: distribution packages (e.g. libobs-dev / obs-studio, libusb-1.0-0-dev).
CFLAGS   += $(shell pkg-config --cflags libobs libusb-1.0)
USB_LIBS  = $(shell pkg-config --libs libusb-1.0) -lpthread -lm
PLUGIN    = build/cv710.so
PLUGIN_LDFLAGS = -shared $(shell pkg-config --libs libobs)
DEST      = $(HOME)/.config/obs-studio/plugins/cv710/bin/64bit
endif

$(PLUGIN): src/cv710.c src/cv710.h src/obs-plugin.c $(if $(OBS_SRC),build/include/obsconfig.h)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) src/cv710.c src/obs-plugin.c -o $@ $(PLUGIN_LDFLAGS) $(USB_LIBS)

build/smoke: src/cv710.c src/cv710.h tests/smoke.c
	mkdir -p build
	$(CC) $(CFLAGS) src/cv710.c tests/smoke.c -o $@ $(USB_LIBS)

build/include/obsconfig.h:
	git clone -q --depth 1 --branch $(OBS_VERSION) --filter=blob:none --sparse \
		https://github.com/obsproject/obs-studio.git $(OBS_SRC)
	git -C $(OBS_SRC) sparse-checkout set libobs
	git clone -q --depth 1 --branch v0.8.2 https://github.com/simd-everywhere/simde.git build/simde
	mkdir -p build/include
	printf '#pragma once\n#define OBS_RELEASE_CANDIDATE 0\n#define OBS_BETA 0\n' > $@

smoke: build/smoke

install: $(PLUGIN)
ifeq ($(shell uname),Darwin)
	mkdir -p "$(DEST)/Contents/MacOS"
	cp $(PLUGIN) "$(DEST)/Contents/MacOS/"
else
	mkdir -p "$(DEST)"
	cp $(PLUGIN) "$(DEST)/"
endif

clean:
	rm -rf build

.PHONY: smoke install clean
