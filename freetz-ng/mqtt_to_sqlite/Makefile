# Makefile for mqtt_to_sqlite (Ubuntu/Freetz-friendly)
#
# Ubuntu build deps:
#   sudo apt-get install build-essential pkg-config libmosquitto-dev libsqlite3-dev
#
# Usage:
#   make            # build mqtt_to_sqlite
#   make clean
#
# Cross-compile hint for FreetzNG:
#   Adjust CC, PKG_CONFIG_PATH, and LDFLAGS/CFLAGS to point to your toolchain and staged libs.
#   Example (pseudo):
#     make CC=mips-linux-gnu-gcc PKG_CONFIG_PATH=/path/to/staging/usr/lib/pkgconfig \
#          CFLAGS_EXTRA="-I/path/to/staging/usr/include" \
#          LDFLAGS_EXTRA="-L/path/to/staging/usr/lib"
#
# Optional static link (only if your toolchain supports it):
#   make STATIC=1

APP      := mqtt_to_sqlite
SRC      := mqtt_to_sqlite.c
OBJ      := $(SRC:.c=.o)

PKG_LIBS := libmosquitto sqlite3

CC       ?= gcc
CSTD     ?= -std=c11
WARN     ?= -Wall -Wextra -Wpedantic
OPT      ?= -O2
DEFS     ?=

# Allow extra flags from env
CFLAGS_EXTRA ?=
LDFLAGS_EXTRA ?=

# Prefer pkg-config for portable flags
CFLAGS_PKG := $(shell pkg-config --cflags $(PKG_LIBS) 2>/dev/null)
LIBS_PKG   := $(shell pkg-config --libs   $(PKG_LIBS) 2>/dev/null)

# Fallback if pkg-config missing (common on embedded toolchains)
ifeq ($(strip $(CFLAGS_PKG)),)
  CFLAGS_PKG := 
endif
ifeq ($(strip $(LIBS_PKG)),)
  LIBS_PKG := -lmosquitto -lsqlite3
endif

CFLAGS := $(CSTD) $(WARN) $(OPT) $(DEFS) $(CFLAGS_PKG) $(CFLAGS_EXTRA)
LDFLAGS := $(LDFLAGS_EXTRA)
LIBS := $(LIBS_PKG)

# Optional static link
ifeq ($(STATIC),1)
  LDFLAGS += -static
endif

.PHONY: all clean

all: $(APP)

$(APP): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(APP) $(OBJ)

