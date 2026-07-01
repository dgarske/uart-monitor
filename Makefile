# Makefile
#
# Copyright (C) 2025 wolfSSL Inc.
#
# This file is part of uart-monitor.
#
# uart-monitor is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 3 of the License, or
# (at your option) any later version.
#
# uart-monitor is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335, USA

CC      ?= gcc
CFLAGS  = -Wall -Wextra -Werror -pedantic -std=c11 -O2 -pthread
LDFLAGS = -pthread
PREFIX  ?= $(HOME)/.local

SRCDIR  = src
BUILDDIR= build
TARGET  = uart-monitor

UNAME_S := $(shell uname -s)

# Platform-neutral sources plus an OS-selected backend pair. The Makefile
# picks the matching platform files (rather than a wildcard) so the wrong
# OS's backend is never compiled.
COMMON_SRCS = main.c monitor.c serial.c control.c log.c util.c \
              identify.c identify_worker.c

ifeq ($(UNAME_S),Darwin)
  CFLAGS  += -D_DARWIN_C_SOURCE
  LDFLAGS += -framework IOKit -framework CoreFoundation
  PLATFORM_SRCS = hotplug_macos.c identify_macos.c
  TEST_PLATFORM_OBJ = $(BUILDDIR)/identify_macos.o
  # macOS: openpty is in libSystem; IOKit/CoreFoundation for identify.
  TEST_LIBS = -framework IOKit -framework CoreFoundation
else
  CFLAGS  += -D_GNU_SOURCE
  PLATFORM_SRCS = hotplug_linux.c identify_linux.c
  TEST_PLATFORM_OBJ = $(BUILDDIR)/identify_linux.o
  # Linux: openpty is in libutil.
  TEST_LIBS = -lutil
endif

SRCS    = $(addprefix $(SRCDIR)/, $(COMMON_SRCS) $(PLATFORM_SRCS))
OBJS    = $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(SRCS))

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

$(BUILDDIR)/%.o: $(SRCDIR)/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

clean:
	rm -rf $(BUILDDIR) $(TARGET)

LAUNCHD_LABEL = com.wolfssl.uart-monitor
LAUNCHD_DIR   = $(HOME)/Library/LaunchAgents
LAUNCHD_PLIST = $(LAUNCHD_DIR)/$(LAUNCHD_LABEL).plist

install: $(TARGET)
	install -d $(PREFIX)/bin
	install -m 755 $(TARGET) $(PREFIX)/bin/
ifeq ($(UNAME_S),Darwin)
	install -d $(LAUNCHD_DIR)
	sed 's|__UART_MONITOR_BIN__|$(PREFIX)/bin/$(TARGET)|' \
	    launchd/$(LAUNCHD_LABEL).plist > $(LAUNCHD_PLIST)
	@echo ""
	@echo "Installed uart-monitor to $(PREFIX)/bin/"
	@echo "Enable with:"
	@echo "  launchctl bootstrap gui/$$(id -u) $(LAUNCHD_PLIST)"
	@echo "Disable with:"
	@echo "  launchctl bootout gui/$$(id -u) $(LAUNCHD_PLIST)"
else
	install -d $(HOME)/.config/systemd/user
	install -m 644 systemd/uart-monitor.service $(HOME)/.config/systemd/user/
	systemctl --user daemon-reload
	@echo ""
	@echo "Installed uart-monitor to $(PREFIX)/bin/"
	@echo "Enable with: systemctl --user enable --now uart-monitor"
endif

uninstall:
	rm -f $(PREFIX)/bin/$(TARGET)
ifeq ($(UNAME_S),Darwin)
	-launchctl bootout gui/$$(id -u) $(LAUNCHD_PLIST) 2>/dev/null || true
	rm -f $(LAUNCHD_PLIST)
else
	systemctl --user stop uart-monitor 2>/dev/null || true
	systemctl --user disable uart-monitor 2>/dev/null || true
	rm -f $(HOME)/.config/systemd/user/uart-monitor.service
	systemctl --user daemon-reload
endif

# Tests. Shared objects plus the OS-selected identify backend; TEST_LIBS
# provides openpty (libutil on Linux) and IOKit/CoreFoundation on macOS.
TESTS   = tests/test_serial tests/test_monitor tests/test_identify
TEST_COMMON = $(BUILDDIR)/util.o $(BUILDDIR)/identify.o $(BUILDDIR)/serial.o \
              $(BUILDDIR)/log.o $(BUILDDIR)/control.o $(TEST_PLATFORM_OBJ)

tests/test_serial: tests/test_serial.c $(TEST_COMMON)
	$(CC) $(CFLAGS) -o $@ $^ $(TEST_LIBS)

tests/test_monitor: tests/test_monitor.c $(TEST_COMMON)
	$(CC) $(CFLAGS) -o $@ $^ $(TEST_LIBS)

tests/test_identify: tests/test_identify.c $(TEST_COMMON)
	$(CC) $(CFLAGS) -o $@ $^ $(TEST_LIBS)

test: $(TARGET) $(TESTS)
	@echo "=== Running tests ==="
	@for t in $(TESTS); do echo "--- $$t ---"; ./$$t || exit 1; done
	@echo "=== All tests passed ==="

.PHONY: all clean install uninstall test
