# Simple Makefile for Linux/BSD (X11) and macOS (Cocoa).
# Windows: use CMake (MSVC or MinGW), see README.md.

CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
UNAME_S := $(shell uname -s)

SRC = src/render.c src/font.c src/fontdb.c src/theme.c src/ui.c src/textedit.c src/window.c
CPPFLAGS += -Iinclude -Isrc $(shell pkg-config --cflags freetype2)
LDLIBS   += $(shell pkg-config --libs freetype2) -lm

ifeq ($(UNAME_S),Darwin)
  PLATFORM_OBJ = build/platform_cocoa.o
  LDLIBS += -framework Cocoa -framework QuartzCore
else
  PLATFORM_OBJ = build/platform_x11.o
  ifeq ($(shell pkg-config --exists xext && echo yes),yes)
    CPPFLAGS += -DCGUI_HAVE_XSHAPE $(shell pkg-config --cflags x11 xext)
    LDLIBS   += $(shell pkg-config --libs x11 xext)
  else
    CPPFLAGS += $(shell pkg-config --cflags x11)
    LDLIBS   += $(shell pkg-config --libs x11)
  endif
endif

OBJ = $(SRC:src/%.c=build/%.o) $(PLATFORM_OBJ)

all: build/fontview

build:
	mkdir -p build

build/%.o: src/%.c src/cg_internal.h src/platform.h include/cgui.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/platform_cocoa.o: src/platform_cocoa.m src/platform.h include/cgui.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/libcgui.a: $(OBJ)
	$(AR) rcs $@ $^

build/fontview: examples/fontview.c build/libcgui.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $< build/libcgui.a $(LDLIBS) -o $@

clean:
	rm -rf build

.PHONY: all clean
