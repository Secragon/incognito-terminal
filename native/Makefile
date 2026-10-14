# Copyright 2026 Secragon Ltd
# SPDX-License-Identifier: Apache-2.0

CC ?= cc
PACKAGE ?= incognito-terminal
VERSION := $(shell sed -n '1p' VERSION)
CFLAGS ?= -O2 -g
CPPFLAGS += -Isrc -Ivendor/libvterm/include -Ivendor/cjson -DCJSON_NESTING_LIMIT=32 \
	-DINCOGNITO_TERMINAL_VERSION='"$(VERSION)"'
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -fstack-protector-strong \
	-ffile-prefix-map=$(CURDIR)=. -fdebug-prefix-map=$(CURDIR)=. \
	-fmacro-prefix-map=$(CURDIR)=.
LDFLAGS += -Wl,-z,relro,-z,now
LDLIBS += -lutil -lm
PREFIX ?= /usr/local
BUILD ?= build
SOURCES := $(wildcard src/*.c) $(wildcard vendor/libvterm/src/*.c) vendor/cjson/cJSON.c
OBJECTS := $(patsubst %.c,$(BUILD)/%.o,$(SOURCES))
TABLES := $(patsubst %.tbl,%.inc,$(wildcard vendor/libvterm/src/encoding/*.tbl))
.PHONY: all check clean distclean test sanitize install uninstall source deb release release-draft
all: $(BUILD)/incognito-terminal
$(BUILD)/incognito-terminal: $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)
$(BUILD)/%.o: %.c src/privacy.h src/history_policies.h VERSION $(TABLES)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@
vendor/libvterm/src/encoding/%.inc: vendor/libvterm/src/encoding/%.tbl
	perl -CSD vendor/libvterm/tbl2inc_c.pl $< > $@
check test: all $(BUILD)/test-native
	$(BUILD)/test-native
$(BUILD)/test-native: tests/native.c $(filter-out $(BUILD)/src/main.o,$(OBJECTS))
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)
sanitize:
	$(MAKE) BUILD=build/sanitize CFLAGS='-O1 -g -std=c11 -Wall -Wextra -Wpedantic -fno-omit-frame-pointer -fsanitize=address,undefined -fno-pie' LDFLAGS='-fsanitize=address,undefined -no-pie' check
install: all
	install -Dm755 $(BUILD)/incognito-terminal $(DESTDIR)$(PREFIX)/bin/incognito-terminal
	install -Dm755 scripts/incognito-terminal-root $(DESTDIR)$(PREFIX)/bin/incognito-terminal-root
	install -Dm644 data/incognito-terminal.desktop $(DESTDIR)$(PREFIX)/share/applications/$(PACKAGE).desktop
	install -Dm644 data/incognito-terminal-root.desktop $(DESTDIR)$(PREFIX)/share/applications/$(PACKAGE)-root.desktop
	install -Dm644 data/com.secragon.incognito-terminal.metainfo.xml $(DESTDIR)$(PREFIX)/share/metainfo/com.secragon.incognito-terminal.metainfo.xml
	install -Dm644 data/incognito-terminal.lintian-overrides $(DESTDIR)$(PREFIX)/share/lintian/overrides/$(PACKAGE)
	install -Dm644 data/icons/hicolor/32x32/apps/incognito-terminal.png $(DESTDIR)$(PREFIX)/share/icons/hicolor/32x32/apps/$(PACKAGE).png
	install -Dm644 data/icons/hicolor/32x32/apps/incognito-terminal-root.png $(DESTDIR)$(PREFIX)/share/icons/hicolor/32x32/apps/$(PACKAGE)-root.png
	install -Dm644 data/icons/hicolor/48x48/apps/incognito-terminal.png $(DESTDIR)$(PREFIX)/share/icons/hicolor/48x48/apps/$(PACKAGE).png
	install -Dm644 data/icons/hicolor/48x48/apps/incognito-terminal-root.png $(DESTDIR)$(PREFIX)/share/icons/hicolor/48x48/apps/$(PACKAGE)-root.png
	install -Dm644 data/icons/hicolor/64x64/apps/incognito-terminal.png $(DESTDIR)$(PREFIX)/share/icons/hicolor/64x64/apps/$(PACKAGE).png
	install -Dm644 data/icons/hicolor/64x64/apps/incognito-terminal-root.png $(DESTDIR)$(PREFIX)/share/icons/hicolor/64x64/apps/$(PACKAGE)-root.png
	install -Dm644 data/icons/hicolor/128x128/apps/incognito-terminal.png $(DESTDIR)$(PREFIX)/share/icons/hicolor/128x128/apps/$(PACKAGE).png
	install -Dm644 data/icons/hicolor/128x128/apps/incognito-terminal-root.png $(DESTDIR)$(PREFIX)/share/icons/hicolor/128x128/apps/$(PACKAGE)-root.png
	install -Dm644 data/icons/hicolor/256x256/apps/incognito-terminal.png $(DESTDIR)$(PREFIX)/share/icons/hicolor/256x256/apps/$(PACKAGE).png
	install -Dm644 data/icons/hicolor/256x256/apps/incognito-terminal-root.png $(DESTDIR)$(PREFIX)/share/icons/hicolor/256x256/apps/$(PACKAGE)-root.png
	install -Dm644 data/icons/hicolor/512x512/apps/incognito-terminal.png $(DESTDIR)$(PREFIX)/share/icons/hicolor/512x512/apps/$(PACKAGE).png
	install -Dm644 data/icons/hicolor/512x512/apps/incognito-terminal-root.png $(DESTDIR)$(PREFIX)/share/icons/hicolor/512x512/apps/$(PACKAGE)-root.png
	install -Dm644 docs/incognito-terminal.1 $(DESTDIR)$(PREFIX)/share/man/man1/incognito-terminal.1
	install -Dm644 docs/incognito-terminal-root.1 $(DESTDIR)$(PREFIX)/share/man/man1/incognito-terminal-root.1
	install -Dm644 README.md $(DESTDIR)$(PREFIX)/share/doc/$(PACKAGE)/README.md
	install -Dm644 NOTICE $(DESTDIR)$(PREFIX)/share/doc/$(PACKAGE)/NOTICE
	install -Dm644 LICENSE $(DESTDIR)$(PREFIX)/share/licenses/$(PACKAGE)/LICENSE
	install -Dm644 vendor/libvterm/LICENSE $(DESTDIR)$(PREFIX)/share/licenses/$(PACKAGE)/libvterm-LICENSE
	install -Dm644 vendor/cjson/LICENSE $(DESTDIR)$(PREFIX)/share/licenses/$(PACKAGE)/cJSON-LICENSE
uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/incognito-terminal \
		$(DESTDIR)$(PREFIX)/bin/incognito-terminal-root \
		$(DESTDIR)$(PREFIX)/share/applications/$(PACKAGE).desktop \
		$(DESTDIR)$(PREFIX)/share/applications/$(PACKAGE)-root.desktop \
		$(DESTDIR)$(PREFIX)/share/metainfo/com.secragon.incognito-terminal.metainfo.xml \
		$(DESTDIR)$(PREFIX)/share/lintian/overrides/$(PACKAGE) \
		$(DESTDIR)$(PREFIX)/share/man/man1/incognito-terminal.1 \
		$(DESTDIR)$(PREFIX)/share/man/man1/incognito-terminal-root.1
	@for size in 32 48 64 128 256 512; do \
		rm -f "$(DESTDIR)$(PREFIX)/share/icons/hicolor/$$size"x"$$size/apps/$(PACKAGE).png" \
			"$(DESTDIR)$(PREFIX)/share/icons/hicolor/$$size"x"$$size/apps/$(PACKAGE)-root.png"; \
	done
	rm -rf $(DESTDIR)$(PREFIX)/share/doc/$(PACKAGE) \
		$(DESTDIR)$(PREFIX)/share/licenses/$(PACKAGE)
clean:
	rm -rf $(BUILD)
distclean: clean
	rm -rf dist
source:
	./scripts/build-release.sh source
deb:
	./scripts/build-release.sh deb
release:
	./scripts/build-release.sh all
release-draft:
	ALLOW_PLACEHOLDERS=1 ./scripts/build-release.sh all
-include $(OBJECTS:.o=.d)
