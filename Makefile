NATIVE_DIR := native
TEST_PYTHON ?= .venv/bin/python
PREFIX ?= /usr/local

.PHONY: all clean test sanitize lint ci install uninstall source deb release release-draft verify-release

all:
	$(MAKE) -C $(NATIVE_DIR) all

test: all
	$(MAKE) -C $(NATIVE_DIR) check
	INCOGNITO_TERMINAL_BINARY=$(abspath $(NATIVE_DIR)/build/incognito-terminal) \
		$(TEST_PYTHON) -m unittest discover -s tests -v

sanitize:
	$(MAKE) -C $(NATIVE_DIR) sanitize
	INCOGNITO_TERMINAL_BINARY=$(abspath $(NATIVE_DIR)/build/sanitize/incognito-terminal) \
		$(TEST_PYTHON) -m unittest discover -s tests -v

lint:
	shellcheck -x incognito-terminal native/scripts/build-release.sh native/scripts/incognito-terminal-root
	desktop-file-validate native/data/incognito-terminal.desktop native/data/incognito-terminal-root.desktop
	appstreamcli validate --no-net --pedantic native/data/com.secragon.incognito-terminal.metainfo.xml
	mandoc -T lint native/docs/incognito-terminal.1 native/docs/incognito-terminal-root.1

ci: lint test verify-release

install:
	$(MAKE) -C $(NATIVE_DIR) install PREFIX=$(PREFIX) DESTDIR="$(DESTDIR)"

uninstall:
	$(MAKE) -C $(NATIVE_DIR) uninstall PREFIX=$(PREFIX) DESTDIR="$(DESTDIR)"

clean:
	$(MAKE) -C $(NATIVE_DIR) clean

source deb release release-draft:
	$(MAKE) -C $(NATIVE_DIR) $@

verify-release: release
	cd $(NATIVE_DIR)/dist && sha256sum -c SHA256SUMS
	lintian --fail-on error $(NATIVE_DIR)/dist/$(shell . $(NATIVE_DIR)/packaging/release.conf; printf '%s_%s-%s_%s.deb' "$$PACKAGE" "$$(cat $(NATIVE_DIR)/VERSION)" "$$DEB_REVISION" "$$(dpkg-architecture -qDEB_HOST_ARCH)")
