# Top-level Makefile for lg-magic (kernel module + userspace tools).
#
# Targets:
#   all             - build the kernel module and the userspace tools (default)
#   modules         - build only the kernel module (used by DKMS)
#   tools           - build only the userspace tools
#   check           - build the tools and run the test suite
#   install         - install the lg-magic binary and the udev rule
#   install-firmware- install a calibration blob into /lib/firmware (opt-in)
#   uninstall       - remove what install put in place (never the firmware)
#   clean
#
# dkms.conf at the repo root drives DKMS (PACKAGE_VERSION must stay in sync
# with debian/changelog). DKMS builds the module only; the tools are built
# by the .deb package, not by DKMS.

KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build
PREFIX ?= /usr
DESTDIR ?=

# Calibration blob for the install-firmware target.
CALIB ?= lg_magic_calib.bin

all: modules tools

modules:
	$(MAKE) -C kernel KDIR=$(KDIR)

tools:
	$(MAKE) -C tools

check: tools
	$(MAKE) -C tools check

install: tools
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)/etc/udev/rules.d
	install -m 0755 tools/lg-magic $(DESTDIR)$(PREFIX)/bin/
	install -m 0644 51-lgimu.rules $(DESTDIR)/etc/udev/rules.d/

# Deliberately opt-in: a zeroed blob would pass the kernel's validation and
# silently disable the airmouse, so firmware is never installed by default.
install-firmware:
	install -d $(DESTDIR)/lib/firmware
	install -m 0644 $(CALIB) $(DESTDIR)/lib/firmware/

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/lg-magic
	rm -f $(DESTDIR)/etc/udev/rules.d/51-lgimu.rules

clean:
	$(MAKE) -C kernel clean
	-$(MAKE) -C tools clean || true

.PHONY: all modules tools check install install-firmware uninstall clean
