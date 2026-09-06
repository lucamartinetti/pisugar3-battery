#!/usr/bin/env bash
# Cross-build pisugar3_battery.ko against a Raspberry Pi's kernel headers and
# install it, with the overlays, on that Pi over SSH.
#
#   tools/deploy.sh [user@]host [overlay] [--persist]
#
# The headers are fetched from the Raspberry Pi apt archive for whatever
# kernel the Pi is running, and cached under ~/.cache/pisugar3-rpi-headers.
# Their kbuild helpers are arm64 binaries, so the build host needs binfmt_misc
# and QEMU_LD_PREFIX set to an arm64 sysroot; see README.md.
#
# With an overlay name the script also loads it at runtime (dtoverlay) so the
# driver probes without a reboot; that is undone by `dtoverlay -r <name>` or
# the next boot. With --persist as well it adds the dtoverlay= line to
# config.txt, so the driver comes back after a reboot.

set -euo pipefail

HOST=${1:?usage: $0 [user@]host [overlay]}
OVERLAY=${2:-}
PERSIST=${3:-}
REPO=$(cd "$(dirname "$0")/.." && pwd)
CACHE=${PISUGAR3_HEADERS:-$HOME/.cache/pisugar3-rpi-headers}
POOL=https://archive.raspberrypi.com/debian/pool/main/l/linux
BOOT=/boot/firmware

say() { printf '\033[1m%s\033[0m\n' "$*"; }

KREL=$(ssh "$HOST" uname -r)          # 6.18.39+rpt-rpi-v8
say "$HOST runs $KREL"

# 6.18.39+rpt-rpi-v8 -> version 6.18.39+rpt, flavour rpi-v8, package 6.18.39-1+rpt1
VER=${KREL%%-rpi-*}                    # 6.18.39+rpt
UPS=${VER%%+*}-1+rpt1                  # 6.18.39-1+rpt1
KDIR=$CACHE/hdr/usr/src/linux-headers-$KREL

if [ ! -d "$KDIR" ]; then
    say "fetching headers for $KREL"
    mkdir -p "$CACHE/debs" "$CACHE/hdr"
    for f in "linux-headers-${VER}-common-rpi_${UPS}_all.deb" \
             "linux-headers-${KREL}_${UPS}_arm64.deb" \
             "linux-kbuild-${VER}_${UPS}_arm64.deb"; do
        [ -f "$CACHE/debs/$f" ] || curl -sfL -o "$CACHE/debs/$f" "$POOL/$f" \
            || { echo "could not fetch $POOL/$f" >&2; exit 1; }
        bsdtar -xOf "$CACHE/debs/$f" 'data.tar*' | bsdtar -x -C "$CACHE/hdr"
    done
    # The per-flavour stub includes the common tree by absolute path.
    printf 'KBUILD_OUTPUT=%s\ninclude %s/Makefile\n' "$KDIR" \
        "$CACHE/hdr/usr/src/linux-headers-${VER}-common-rpi" > "$KDIR/Makefile"
fi

say "building"
make -C "$REPO" modules dtbo KDIR="$KDIR" ARCH=arm64 \
    CROSS_COMPILE=aarch64-linux-gnu- CC=aarch64-linux-gnu-gcc >/dev/null

say "installing on $HOST"
STAGE=$(ssh "$HOST" mktemp -d)
scp -q "$REPO"/pisugar3_battery.ko "$REPO"/overlays/*.dtbo "$HOST:$STAGE/"
ssh "$HOST" "sudo -n sh -e -c '
    install -D -m 644 $STAGE/pisugar3_battery.ko /lib/modules/$KREL/updates/pisugar3_battery.ko
    depmod -a $KREL
    install -m 644 $STAGE/*.dtbo $BOOT/overlays/
    rm -rf $STAGE'"

if [ -n "$OVERLAY" ]; then
    say "loading overlay $OVERLAY"
    ssh "$HOST" "sudo -n sh -e -c '
        dtoverlay -l | grep -q \" $OVERLAY\$\" || dtoverlay $OVERLAY'"
    if [ "$PERSIST" = "--persist" ]; then
        say "adding dtoverlay=$OVERLAY to $BOOT/config.txt"
        ssh "$HOST" "sudo -n sh -e -c '
            grep -q \"^dtoverlay=$OVERLAY\" $BOOT/config.txt || echo dtoverlay=$OVERLAY >> $BOOT/config.txt'"
    fi
    sleep 2
    ssh "$HOST" 'ls /sys/class/power_supply/; dmesg | grep -i pisugar | tail -3'
fi
