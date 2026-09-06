# Out-of-tree build for pisugar3_battery.ko and the Raspberry Pi overlays.
#
#   make                    against the running kernel
#   make KDIR=<path>        against another kernel's headers
#   make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KDIR=<rpi headers>
#   make dtbo               just the overlays (needs dtc)
#   make check              checkpatch and the binding schema
#   sudo make install       module into /lib/modules/<kver>/updates, overlays
#                           into /boot/firmware/overlays, on a Raspberry Pi
#
# Building on a Pi Zero takes a couple of minutes; cross-building takes
# seconds. Debian-packaged Raspberry Pi headers ship arm64 kbuild helpers, so
# a cross-build on x86 needs binfmt_misc and QEMU_LD_PREFIX pointing at an
# arm64 sysroot - see README.md.

obj-m += pisugar3_battery.o

KDIR ?= /lib/modules/$(shell uname -r)/build
KVER ?= $(shell uname -r)
DTC ?= dtc
CHECKPATCH ?= $(KDIR)/scripts/checkpatch.pl
BOOT ?= /boot/firmware

OVERLAYS := $(patsubst %.dts,%.dtbo,$(wildcard overlays/*.dts))

all: modules dtbo

modules:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

dtbo: $(OVERLAYS)

%.dtbo: %.dts
	$(DTC) -@ -I dts -O dtb -o $@ $<

install: modules dtbo
	install -D -m 644 pisugar3_battery.ko \
		/lib/modules/$(KVER)/updates/pisugar3_battery.ko
	depmod -a $(KVER)
	install -m 644 $(OVERLAYS) $(BOOT)/overlays/

uninstall:
	rm -f /lib/modules/$(KVER)/updates/pisugar3_battery.ko
	depmod -a $(KVER)
	rm -f $(addprefix $(BOOT)/overlays/,$(notdir $(OVERLAYS)))

# The binding refers to the kernel's power-supply.yaml and battery.yaml, so
# checking it needs those two beside it: DT_SCHEMAS is a directory holding
# power/supply/{power-supply,battery}.yaml, as tools/fetch-schemas.sh makes.
DT_SCHEMAS ?= $(HOME)/.cache/pisugar3-dt-schemas
BINDING := Documentation/devicetree/bindings/power/supply/pisugar,pisugar3.yaml

check: dtbo
	$(CHECKPATCH) --no-tree --strict -f pisugar3_battery.c
	install -D -m 644 $(BINDING) $(DT_SCHEMAS)/$(BINDING:Documentation/devicetree/bindings/%=%)
	dt-doc-validate -u $(DT_SCHEMAS) $(DT_SCHEMAS)/$(BINDING:Documentation/devicetree/bindings/%=%)
	dt-extract-example $(BINDING) > .example.dts
	$(DTC) -I dts -O dtb -o .example.dtb .example.dts
	dt-validate -s $(DT_SCHEMAS) .example.dtb
	rm -f .example.dts .example.dtb

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	rm -f $(OVERLAYS) .example.dts .example.dtb

.PHONY: all modules dtbo install uninstall check clean
