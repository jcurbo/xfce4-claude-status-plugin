# XFCE Claude Status Panel Plugin

PLUGIN_NAME = claude-status
PLUGIN_LIB = lib$(PLUGIN_NAME).so

# Paths
PREFIX ?= /usr
LIBDIR ?= $(PREFIX)/lib/x86_64-linux-gnu
PLUGIN_DIR = $(LIBDIR)/xfce4/panel/plugins
DESKTOP_DIR = $(PREFIX)/share/xfce4/panel/plugins

# Compiler flags
CC = gcc
PKG_CONFIG = pkg-config
PACKAGES = libxfce4panel-2.0 libxfce4ui-2 gtk+-3.0 json-glib-1.0 libsoup-3.0

CFLAGS = -Wall -fPIC -shared $(shell $(PKG_CONFIG) --cflags $(PACKAGES))
LDFLAGS = $(shell $(PKG_CONFIG) --libs $(PACKAGES))

.PHONY: all clean install uninstall

all: $(PLUGIN_LIB)

$(PLUGIN_LIB): $(PLUGIN_NAME).c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

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
	rm -f $(PLUGIN_LIB) $(PLUGIN_NAME).desktop
