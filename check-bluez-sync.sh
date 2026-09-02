#!/bin/sh
# =============================================================================
# check-bluez-sync.sh — guard the two copies of the BlueZ transport.
#
# The BlueZ GATT + raw-HCI advertising code exists twice:
#
#   app/tuyaos_demo_wukong_ai/vendor/.../tuyaos_adapter/src/   (TuyaOS TKL)
#   app/agentic_kit/pal/bluez/                                 (agentic-kit)
#
# They are byte-identical and are meant to stay that way. The intended end
# state is one shared copy under app/common/bluez/ with each app supplying only
# its own tuya_bluez_compat.h; until that lands, this script is what stops the
# two from drifting silently.
#
# It exists because the failure it catches is invisible: fix an advertising bug
# in one app, ship it, and six weeks later the other app has the same bug and
# nobody remembers there were two files.
#
# tuya_bluez_compat.h is deliberately NOT compared. That header IS the
# adaptation point -- wukong's routes memory through tkl_system_malloc and logs
# via uni_log, agentic-kit's uses libc. It is supposed to differ.
#
# Usage:
#   ./check-bluez-sync.sh            # report drift, exit 1 if any
#   ./check-bluez-sync.sh --quiet    # only speak up when something drifted
# =============================================================================

SELF=$(readlink -f "$0")
# This script lives inside the wukong repo (the only versioned home the two
# copies share); the app/ directory itself is not a git repository. A compat
# symlink at app/check-bluez-sync.sh preserves the old invocation path.
APP_DIR=$(dirname "$SELF")/..

WUKONG="$APP_DIR/tuyaos_demo_wukong_ai/vendor/gcc-arm-10.3-2021.07-x86_64-arm-none-linux-gnueabihf/tuyaos/tuyaos_adapter/src"
AGENTIC="$APP_DIR/agentic_kit/pal/bluez"

# Everything that must stay identical. tuya_bluez_compat.h is excluded above.
SHARED="tuya_gatt.c tuya_gatt.h tuya_hci.c tuya_hci.h tuya_bluez_def.h"
SHARED_DIRS="gdbus"

QUIET=0
[ "$1" = "--quiet" ] && QUIET=1

# Not an error: either app may legitimately be absent from a given checkout,
# and a missing tree must not fail somebody's build.
if [ ! -d "$WUKONG" ] || [ ! -d "$AGENTIC" ]; then
    [ "$QUIET" -eq 1 ] || echo "check-bluez-sync: only one copy present, nothing to compare"
    exit 0
fi

drift=""

for f in $SHARED; do
    if [ ! -f "$WUKONG/$f" ] || [ ! -f "$AGENTIC/$f" ]; then
        drift="$drift $f(missing)"
        continue
    fi
    diff -q "$WUKONG/$f" "$AGENTIC/$f" >/dev/null 2>&1 || drift="$drift $f"
done

for d in $SHARED_DIRS; do
    if [ ! -d "$WUKONG/$d" ] || [ ! -d "$AGENTIC/$d" ]; then
        drift="$drift $d/(missing)"
        continue
    fi
    diff -rq "$WUKONG/$d" "$AGENTIC/$d" >/dev/null 2>&1 || drift="$drift $d/"
done

if [ -z "$drift" ]; then
    [ "$QUIET" -eq 1 ] || echo "check-bluez-sync: OK — both BlueZ copies are identical"
    exit 0
fi

# Loud on purpose: this is printed in the middle of a build, and a quiet line
# here is a line nobody reads.
echo
echo "############################################################"
echo "## check-bluez-sync: THE TWO BlueZ COPIES HAVE DRIFTED"
echo "##"
for f in $drift; do
    echo "##   $f"
done
echo "##"
echo "##   wukong : $WUKONG"
echo "##   agentic: $AGENTIC"
echo "##"
echo "## Both apps drive the same radio with the same protocol. A fix"
echo "## that landed in one belongs in the other. Diff them with:"
echo "##   diff -ru $WUKONG $AGENTIC"
echo "##"
echo "## (tuya_bluez_compat.h is excluded — it is meant to differ.)"
echo "############################################################"
echo
exit 1
