# ── platform detection ───────────────────────────────────────
UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
    PLATFORM := macos
    CC      := clang
else
    PLATFORM := linux
    CC      := gcc
endif

TARGET  := screencast
SRCDIR  := src
OBJDIR  := build
PROTODIR:= protocols

# Shared sources (both platforms)
SHARED_SRCS := $(SRCDIR)/control.c $(SRCDIR)/encoder.c $(SRCDIR)/composite.c $(SRCDIR)/mixer.c

# Platform-specific sources
ifeq ($(PLATFORM),macos)
    PLATFORM_SRCS := $(SRCDIR)/macos/main.c
    OBJC_SRCS     := $(SRCDIR)/macos/sck_capture.m $(SRCDIR)/macos/avf_camera.m \
                     $(SRCDIR)/macos/avf_mic.m
    SRCS          := $(SHARED_SRCS) $(PLATFORM_SRCS) $(OBJC_SRCS)
else
    PLATFORM_SRCS := $(SRCDIR)/linux/main.c $(SRCDIR)/linux/wlcap.c \
                     $(SRCDIR)/linux/pwcam.c $(SRCDIR)/linux/capture.c \
                     $(SRCDIR)/linux/arbiter.c
    SRCS          := $(SHARED_SRCS) $(PLATFORM_SRCS)
endif

# Object files — handle .c and .m separately
OBJS := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(filter %.c,$(SRCS)))
OBJS += $(patsubst $(SRCDIR)/%.m,$(OBJDIR)/%.o,$(filter %.m,$(SRCS)))

# Vendored Wayland protocol (wlr-screencopy); code generated at build time.
PROTO_XML := $(PROTODIR)/wlr-screencopy-unstable-v1.xml
PROTO_HDR := $(OBJDIR)/wlr-screencopy-unstable-v1-client-protocol.h
PROTO_SRC := $(OBJDIR)/wlr-screencopy-unstable-v1-protocol.c
PROTO_OBJ := $(OBJDIR)/wlr-screencopy-unstable-v1-protocol.o

SCANNER := wayland-scanner

# ── compiler flags ───────────────────────────────────────────
ifeq ($(PLATFORM),macos)
    # Use pkg-config for ffmpeg (libav*) but not for wayland/pipewire
    FFMPEG_PKGS := libavformat libavcodec libavdevice libswscale libswresample libavutil
    CFLAGS  := $(shell pkg-config --cflags $(FFMPEG_PKGS)) -Isrc -Isrc/macos -I$(OBJDIR) -pthread \
               -include src/compat_macos.h \
               -O2 -Wall -Wextra -std=c11
    LDFLAGS := $(shell pkg-config --libs $(FFMPEG_PKGS)) -pthread -lm \
               -framework AVFoundation -framework ScreenCaptureKit \
               -framework CoreMedia -framework CoreVideo -framework CoreAudio \
               -framework Cocoa -framework Accelerate
else
    PKG     := libavformat libavcodec libavdevice libswscale libswresample \
               libavutil wayland-client libpipewire-0.3 libspa-0.2
    CFLAGS  := $(shell pkg-config --cflags $(PKG)) -I$(OBJDIR) -Isrc -Isrc/linux -pthread \
               -O2 -Wall -Wextra -std=c11
    LDFLAGS := $(shell pkg-config --libs   $(PKG)) -pthread -lm
endif

all: $(OBJDIR) $(TARGET)

$(OBJDIR):
	mkdir -p $@ $@/linux $@/macos

# ── generated protocol code (Linux only) ────────────────────
$(PROTO_HDR): $(PROTO_XML) | $(OBJDIR)
	$(SCANNER) client-header $< $@

$(PROTO_SRC): $(PROTO_XML) | $(OBJDIR)
	$(SCANNER) private-code $< $@

$(PROTO_OBJ): $(PROTO_SRC)
	$(CC) $(CFLAGS) -c -o $@ $<

# Collect link dependencies
TARGET_DEPS := $(OBJS)
ifneq ($(PLATFORM),macos)
# wlcap.c is the only source that includes the generated header.
$(OBJDIR)/linux/wlcap.o: $(PROTO_HDR)
TARGET_DEPS += $(PROTO_OBJ)
endif

$(TARGET): $(TARGET_DEPS)
	$(CC) -o $@ $^ $(LDFLAGS)

# Pattern rule for .c -> .o
# The order-only $(OBJDIR) prerequisite only fires when build/ is missing
# entirely, so a tree built before the linux/ + macos/ split has build/ but no
# subdirectories.  mkdir here rather than relying on that.
$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# Pattern rule for .m -> .o (Objective-C on macOS, ARC only where needed)
# sck_capture uses the SCStreamOutput protocol, which requires ARC.
$(OBJDIR)/macos/sck_capture.o: $(SRCDIR)/macos/sck_capture.m | $(OBJDIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fobjc-arc -c -o $@ $<

$(OBJDIR)/macos/%.o: $(SRCDIR)/macos/%.m | $(OBJDIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# ── macOS app bundle ────────────────────────────────────────
# Presenter Overlay is offered in Control Center's Video Effects panel, which
# only lists camera clients macOS can attribute to an app.  The bundle gives
# the recorder that identity; main.c hands `screencast presenter` off to it.
# Ad-hoc signed by default (CODESIGN_ID=-), which means TCC re-asks for
# permissions after a rebuild — an ad-hoc seal names the exact build.  Pass
# CODESIGN_ID=<identity> to sign with a stable certificate instead.
ifeq ($(PLATFORM),macos)
BUNDLE      := Screencast.app
CODESIGN_ID ?= -

bundle: $(TARGET) $(SRCDIR)/macos/Info.plist
	rm -rf $(BUNDLE)
	mkdir -p $(BUNDLE)/Contents/MacOS
	cp $(SRCDIR)/macos/Info.plist $(BUNDLE)/Contents/Info.plist
	cp $(TARGET) $(BUNDLE)/Contents/MacOS/$(TARGET)
	codesign --force --sign "$(CODESIGN_ID)" $(BUNDLE)

# ~/Applications/Screencast.app is where relaunch_as_bundle() looks.
install: bundle
	mkdir -p $(HOME)/.local/bin $(HOME)/Applications
	install -m 755 $(TARGET) $(HOME)/.local/bin/$(TARGET)
	rm -rf $(HOME)/Applications/$(BUNDLE)
	cp -R $(BUNDLE) $(HOME)/Applications/$(BUNDLE)

.PHONY: bundle install
endif

clean:
	rm -rf $(OBJDIR) $(TARGET) $(TEST_BIN) $(BUNDLE)

# ── tests ───────────────────────────────────────────────────
# The capture arbiter is a pure, I/O-free unit — the one testing seam.  Build a
# standalone test binary (no framework, plain-C asserts) and run it.  It links
# only src/arbiter.c, so it needs no camera, PipeWire, or libav.
TEST_BIN  := $(OBJDIR)/test_arbiter
TEST_SRCS := tests/test_arbiter.c src/linux/arbiter.c

# The mixer is the other pure seam: given frames in, it decides what comes out
# and when silence has to stand in for a source.  It needs libav (swresample +
# avutil) but no device, so it runs anywhere the project builds.
MIXER_TEST_BIN  := $(OBJDIR)/test_mixer
MIXER_TEST_SRCS := tests/test_mixer.c src/mixer.c
MIXER_TEST_PKGS := libswresample libavutil

# VideoToolbox hardware-frame probe (macOS only).  Standalone: verifies that
# h264_videotoolbox accepts CVPixelBuffers by reference, both from libav's own
# hardware pool and wrapped from an external source.  Links nothing from src/.
PROBE_BIN := $(OBJDIR)/vt_hwframe_probe

probe: | $(OBJDIR)
ifeq ($(PLATFORM),macos)
	$(CC) $(CFLAGS) -fobjc-arc -o $(PROBE_BIN) tests/vt_hwframe_probe.m \
	    $(LDFLAGS)
	@echo "built $(PROBE_BIN)"
else
	@echo "probe is macOS only"
endif

test-mixer: | $(OBJDIR)
	$(CC) -Isrc -pthread -O2 -Wall -Wextra -std=c11 \
	    $(shell pkg-config --cflags $(MIXER_TEST_PKGS)) \
	    -o $(MIXER_TEST_BIN) $(MIXER_TEST_SRCS) \
	    $(shell pkg-config --libs $(MIXER_TEST_PKGS)) -lm -pthread
	$(MIXER_TEST_BIN)

ifneq ($(PLATFORM),macos)
test: test-mixer | $(OBJDIR)
	$(CC) -Isrc -Isrc/linux -pthread -O2 -Wall -Wextra -std=c11 \
	    -o $(TEST_BIN) $(TEST_SRCS)
	$(TEST_BIN)
else
test: test-mixer
endif

.PHONY: all clean test test-mixer probe
