CC      := gcc
TARGET  := screencast
SRCDIR  := src
OBJDIR  := build
PROTODIR:= protocols

SRCS    := $(wildcard $(SRCDIR)/*.c)
OBJS    := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(SRCS))

# Vendored Wayland protocol (wlr-screencopy); code generated at build time.
PROTO_XML := $(PROTODIR)/wlr-screencopy-unstable-v1.xml
PROTO_HDR := $(OBJDIR)/wlr-screencopy-unstable-v1-client-protocol.h
PROTO_SRC := $(OBJDIR)/wlr-screencopy-unstable-v1-protocol.c
PROTO_OBJ := $(OBJDIR)/wlr-screencopy-unstable-v1-protocol.o

SCANNER := wayland-scanner

PKG     := libavformat libavcodec libavdevice libswscale libswresample \
           libavutil wayland-client libpipewire-0.3 libspa-0.2

CFLAGS  := $(shell pkg-config --cflags $(PKG)) -I$(OBJDIR) -pthread \
           -O2 -Wall -Wextra -std=c11
LDFLAGS := $(shell pkg-config --libs   $(PKG)) -pthread -lm

all: $(OBJDIR) $(TARGET)

$(OBJDIR):
	mkdir -p $@

# ── generated protocol code ─────────────────────────────────
$(PROTO_HDR): $(PROTO_XML) | $(OBJDIR)
	$(SCANNER) client-header $< $@

$(PROTO_SRC): $(PROTO_XML) | $(OBJDIR)
	$(SCANNER) private-code $< $@

$(PROTO_OBJ): $(PROTO_SRC)
	$(CC) $(CFLAGS) -c -o $@ $<

# wlcap.c is the only source that includes the generated header.
$(OBJDIR)/wlcap.o: $(PROTO_HDR)

$(TARGET): $(OBJS) $(PROTO_OBJ)
	$(CC) -o $@ $^ $(LDFLAGS)

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf $(OBJDIR) $(TARGET)

.PHONY: all clean
