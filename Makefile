# XFCE Claude Status Panel Plugin

PLUGIN_NAME = claude-status
PLUGIN_LIB = lib$(PLUGIN_NAME).so
RUST_LIB = core/target/release/libclaude_status_core.a

# Paths
PREFIX ?= /usr
LIBDIR ?= $(PREFIX)/lib/x86_64-linux-gnu
PLUGIN_DIR = $(LIBDIR)/xfce4/panel/plugins
DESKTOP_DIR = $(PREFIX)/share/xfce4/panel/plugins

# Compiler flags
CC = gcc
PKG_CONFIG = pkg-config
PACKAGES = libxfce4panel-2.0 libxfce4ui-2 gtk+-3.0

CFLAGS = -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Wno-unused-parameter \
         -fPIC -shared $(shell $(PKG_CONFIG) --cflags $(PACKAGES)) \
         -I.

# Link against Rust static library and its dependencies
LDFLAGS = $(shell $(PKG_CONFIG) --libs $(PACKAGES)) \
          $(RUST_LIB) -lpthread -ldl -lm

.PHONY: all clean install uninstall check rust-lib

all: rust-lib $(PLUGIN_LIB)

# Build Rust library first
rust-lib:
	cd core && cargo build --release
	cp core/target/release/claude_status_core.h .

$(PLUGIN_LIB): $(PLUGIN_NAME).c claude_status_core.h $(RUST_LIB)
	$(CC) $(CFLAGS) -o $@ $(PLUGIN_NAME).c $(LDFLAGS)

$(PLUGIN_NAME).desktop: $(PLUGIN_NAME).desktop.in
	sed 's|@PLUGIN_DIR@|$(PLUGIN_DIR)|g' $< > $@

install: $(PLUGIN_LIB) $(PLUGIN_NAME).desktop
	install -d $(DESTDIR)$(PLUGIN_DIR)
	install -m 755 $(PLUGIN_LIB) $(DESTDIR)$(PLUGIN_DIR)/
	install -d $(DESTDIR)$(DESKTOP_DIR)
	install -m 644 $(PLUGIN_NAME).desktop $(DESTDIR)$(DESKTOP_DIR)/

uninstall:
	rm -f $(DESTDIR)$(PLUGIN_DIR)/$(PLUGIN_LIB)
	rm -f $(DESTDIR)$(DESKTOP_DIR)/$(PLUGIN_NAME).desktop

clean:
	rm -f $(PLUGIN_LIB) $(PLUGIN_NAME).desktop claude_status_core.h
	cd core && cargo clean

check: $(PLUGIN_NAME).c
	cppcheck --enable=all --suppress=missingIncludeSystem $<
