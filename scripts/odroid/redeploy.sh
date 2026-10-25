#!/bin/sh
# redeploy.sh -- pull, rebuild, install and restart, in one go, on the ODROID.
#
#   sh scripts/odroid/redeploy.sh            # git pull, build, install, restart
#   sh scripts/odroid/redeploy.sh --no-pull  # build what is checked out now
#
# Run as root from anywhere; it finds the repo from its own path. Stops at the
# first step that fails, and keeps each step's full output in /tmp/redeploy-*.log
# so a failure can be read (or pasted) without re-running.

set -eu

REPO=$(cd "$(dirname "$0")/../.." && pwd)
TARBALL="$REPO/work/rb-odroid.tgz"
PULL=1
[ "${1:-}" = "--no-pull" ] && PULL=0

[ "$(id -u)" = "0" ] || { echo "redeploy: run as root" >&2; exit 1; }

step() {
    name=$1; shift
    log="/tmp/redeploy-$name.log"
    printf '== %-8s ' "$name"
    if "$@" > "$log" 2>&1; then
        echo "ok"
    else
        echo "FAILED -- last lines of $log:"
        tail -20 "$log" | sed 's/^/   /'
        exit 1
    fi
}

cd "$REPO"
if [ "$PULL" = "1" ]; then
    step pull git pull --ff-only
fi
echo "   at $(git log --oneline -1)"
step build sh scripts/odroid/build-on-odroid.sh chroot
step install sh scripts/device/install.sh "$TARBALL"
step restart systemctl restart rblive4

# rbp takes a few seconds to come up and open the audio card.
sleep 8
if systemctl is-active --quiet rblive4; then
    echo "== rblive4 is running"
    sed -n 's/^\(dev\|card\|map\)=/   \1=/p' /run/rblive4/audio.state 2>/dev/null | head -3
else
    echo "== rblive4 is NOT running:"
    systemctl status rblive4 --no-pager 2>&1 | tail -10 | sed 's/^/   /'
    exit 1
fi
