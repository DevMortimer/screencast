CC      := gcc
TARGET  := screencast
SRCDIR  := src
OBJDIR  := build

SRCS    := $(wildcard $(SRCDIR)/*.c)
OBJS    := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(SRCS))

PKG     := libavformat libavcodec libavdevice libswscale libswresample libavutil x11 xtst xext

CFLAGS  := $(shell pkg-config --cflags $(PKG)) -pthread -O2 -Wall -Wextra -std=c11
LDFLAGS := $(shell pkg-config --libs   $(PKG)) -pthread -lm -lXinerama

all: $(OBJDIR) $(TARGET)

$(OBJDIR):
	mkdir -p $@

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

$(OBJDIR)/%.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf $(OBJDIR) $(TARGET)

.PHONY: all clean
