CC ?= cc
PKG_CONFIG ?= pkg-config
PREFIX ?= /usr/local
APPDIR ?= $(PREFIX)/flitz
LOCALEDIR ?= /usr/share/locale
LANGUAGES = ar ca de es fr hu it ja pt ru zh

CFLAGS ?= -O2 -pipe
GTK_CFLAGS := $(shell $(PKG_CONFIG) --cflags gtk+-3.0 gio-2.0 2>/dev/null)
GTK_LIBS := $(shell $(PKG_CONFIG) --libs gtk+-3.0 gio-2.0 2>/dev/null)

CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Wformat=2 -Wshadow \
          -Wstrict-prototypes -Wmissing-prototypes -Wpointer-arith \
          -Wcast-qual -Wwrite-strings -fstack-protector-strong \
          $(GTK_CFLAGS)
LDLIBS += $(GTK_LIBS)

COMMON_OBJ = build/flitz-common.o
TARGETS = build/flitz-extractor-bin build/flitz-compressor-bin

.PHONY: all clean install check locales

all: check $(TARGETS)

check:
	@command -v $(PKG_CONFIG) >/dev/null 2>&1 || { \
		echo "ERROR: pkg-config is required."; exit 1; }
	@$(PKG_CONFIG) --exists gtk+-3.0 gio-2.0 || { \
		echo "ERROR: GTK3 development files are required."; \
		echo "On Debian/Devuan install: build-essential pkg-config libgtk-3-dev"; \
		echo "On Puppy load the devx SFS and install the GTK3 development package if needed."; \
		exit 1; }

build:
	mkdir -p build

build/flitz-common.o: src/flitz-common.c src/flitz-common.h | check build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/flitz-extractor.o: src/flitz-extractor.c src/flitz-common.h | check build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/flitz-compressor.o: src/flitz-compressor.c src/flitz-common.h | check build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/flitz-extractor-bin: build/flitz-extractor.o $(COMMON_OBJ)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

build/flitz-compressor-bin: build/flitz-compressor.o $(COMMON_OBJ)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

locales:
	python3 tools/build_locales.py

install: all
	install -d "$(DESTDIR)$(APPDIR)" \
	           "$(DESTDIR)$(PREFIX)/bin" \
	           "$(DESTDIR)/usr/share/applications" \
	           "$(DESTDIR)/usr/share/doc/flitz-toolkit"
	install -m 0755 build/flitz-extractor-bin "$(DESTDIR)$(APPDIR)/flitz-extractor-bin"
	install -m 0755 build/flitz-compressor-bin "$(DESTDIR)$(APPDIR)/flitz-compressor-bin"
	install -m 0644 data/Flitz.png "$(DESTDIR)$(APPDIR)/Flitz.png"
	install -m 0644 data/flitz-extractor.desktop "$(DESTDIR)$(APPDIR)/flitz-extractor.desktop"
	install -m 0644 data/flitz-compressor.desktop "$(DESTDIR)$(APPDIR)/flitz-compressor.desktop"
	install -m 0644 data/flitz-extractor.desktop "$(DESTDIR)/usr/share/applications/flitz-extractor.desktop"
	install -m 0644 data/flitz-compressor.desktop "$(DESTDIR)/usr/share/applications/flitz-compressor.desktop"
	install -m 0644 LICENSE "$(DESTDIR)/usr/share/doc/flitz-toolkit/LICENSE"
	install -m 0644 README.md "$(DESTDIR)/usr/share/doc/flitz-toolkit/README.md"
	install -m 0644 README-ES.md "$(DESTDIR)/usr/share/doc/flitz-toolkit/README-ES.md"
	ln -sfn "$(APPDIR)/flitz-extractor-bin" "$(DESTDIR)$(PREFIX)/bin/flitz-extractor"
	ln -sfn "$(APPDIR)/flitz-compressor-bin" "$(DESTDIR)$(PREFIX)/bin/flitz-compressor"
	# Intentional ROX launcher: this link must point to the .desktop file.
	ln -sfn "$(APPDIR)/flitz-extractor.desktop" "$(DESTDIR)$(APPDIR)/flitz-extractor"
	@for lang in $(LANGUAGES); do \
		install -d "$(DESTDIR)$(LOCALEDIR)/$$lang/LC_MESSAGES"; \
		install -m 0644 "locale/$$lang/LC_MESSAGES/flitz-toolkit.mo" \
			"$(DESTDIR)$(LOCALEDIR)/$$lang/LC_MESSAGES/flitz-toolkit.mo"; \
	done

clean:
	rm -rf build package-root *.deb
