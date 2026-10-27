#!/bin/sh
# Load RyzenPlatform.kext into the running kernel for this boot only.
# Nothing is copied to /Library/Extensions and nothing is written to the EFI,
# so a reboot is always a clean undo.
#
#   sudo scripts/dev-load.sh          load
#   sudo scripts/dev-load.sh unload   unload
#
# The kext is staged into /tmp and chowned there, so the build tree in
# build/ stays owned by you and `make` keeps working.
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KEXT="$ROOT/build/run/RyzenPlatform.kext"
STAGE_DIR="/tmp/ryzenplatform-dev"
STAGE="$STAGE_DIR/RyzenPlatform.kext"

if [ "$(id -u)" != "0" ]; then
    echo "run me with sudo" >&2
    exit 1
fi

case "${1:-load}" in
    load)
        if [ ! -d "$KEXT" ]; then
            echo "no kext at $KEXT - run make in kext/ first" >&2
            exit 1
        fi
        rm -rf "$STAGE_DIR"
        mkdir -p "$STAGE_DIR"
        cp -R "$KEXT" "$STAGE"
        chown -R root:wheel "$STAGE"
        chmod -R go-w "$STAGE"
        # kmutil wants to rebuild the boot kernel collection for a third-party
        # kext, which needs a reboot. kextutil loads straight into the running
        # kernel, which is what we want for a shadow-mode probe.
        if /usr/bin/kmutil load -z -p "$STAGE"; then
            echo "loaded $STAGE (kmutil)"
        else
            echo "kmutil wants a reboot; falling back to kextutil"
            /usr/bin/kextutil -v 2 -l "$STAGE"
            echo "loaded $STAGE (kextutil, this boot only)"
        fi
        ;;
    unload)
        /usr/bin/kmutil unload -z -p "$STAGE"
        echo "unloaded $STAGE"
        ;;
    *)
        echo "usage: $0 [load|unload]" >&2
        exit 1
        ;;
esac
