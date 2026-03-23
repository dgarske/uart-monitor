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
CFLAGS  = -Wall -Wextra -Werror -pedantic -std=c11 -D_GNU_SOURCE -O2
LDFLAGS =
PREFIX  ?= $(HOME)/.local

SRCDIR  = src
BUILDDIR= build
TARGET  = uart-monitor

SRCS    = $(wildcard $(SRCDIR)/*.c)
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

install: $(TARGET)
	install -d $(PREFIX)/bin
	install -m 755 $(TARGET) $(PREFIX)/bin/
	install -d $(HOME)/.config/systemd/user
	install -m 644 systemd/uart-monitor.service $(HOME)/.config/systemd/user/
	systemctl --user daemon-reload
	@echo ""
	@echo "Installed uart-monitor to $(PREFIX)/bin/"
	@echo "Enable with: systemctl --user enable --now uart-monitor"

uninstall:
	rm -f $(PREFIX)/bin/$(TARGET)
	systemctl --user stop uart-monitor 2>/dev/null || true
	systemctl --user disable uart-monitor 2>/dev/null || true
	rm -f $(HOME)/.config/systemd/user/uart-monitor.service
	systemctl --user daemon-reload

# Tests (link with -lutil for openpty)
TESTS   = tests/test_serial tests/test_monitor tests/test_identify
TEST_COMMON = $(BUILDDIR)/util.o $(BUILDDIR)/identify.o $(BUILDDIR)/serial.o \
              $(BUILDDIR)/log.o $(BUILDDIR)/hotplug.o $(BUILDDIR)/control.o

tests/test_serial: tests/test_serial.c $(TEST_COMMON)
	$(CC) $(CFLAGS) -o $@ $^ -lutil

tests/test_monitor: tests/test_monitor.c $(TEST_COMMON)
	$(CC) $(CFLAGS) -o $@ $^ -lutil

tests/test_identify: tests/test_identify.c $(TEST_COMMON)
	$(CC) $(CFLAGS) -o $@ $^ -lutil

test: $(TARGET) $(TESTS)
	@echo "=== Running tests ==="
	@for t in $(TESTS); do echo "--- $$t ---"; ./$$t || exit 1; done
	@echo "=== All tests passed ==="

.PHONY: all clean install uninstall test
