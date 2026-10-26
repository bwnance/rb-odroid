#!/bin/sh
# conf-crosscheck.sh — prove the three pairs of things that have to be changed
# together still agree.
#
# Each pair lives in two files, so nothing but a habit holds it in step:
#
#   SHIM_VARS         scripts/device/start-rb.sh exports these names into rbp's
#                     environment, and each one needs an RB_<name> line in
#                     scripts/device/rb.conf
#   RB_CONF_VERSION   scripts/device/rb.conf's schema, which must equal
#                     RB_CONF_SCHEMA in scripts/device/lib.sh
#   ExecStart         the units in scripts/device/*.service name a program under
#                     the deploy root, and install.sh has to copy that program
#                     there
#
# THE FIRST IS THE ONE WITH TEETH. start-rb.sh:289-294 states the rule — "a name
# belongs in this list iff it is a device-level setting worth a line in rb.conf"
# — and the loop under it exports EVERY name in the list UNCONDITIONALLY. So a
# listed name with no line in rb.conf is exported as the empty string, over the
# top of an ad-hoc value, and the setting is unreachable with nothing to say so:
# RB_KEY_DUMP, RB_KEY_REPLAY, RB_KEY_REPLAY_SPEED and RB_SIDE_NUDGE_PCT were in
# exactly that state (found 2026-10-13) while docs/08-controls.md and
# docs/07-touch.md published all four as knobs the operator could set.
#
# THE SECOND FAILS CLOSED ON THE UNIT rather than quietly: lib.sh:48 refuses to
# run against a schema it does not expect, so a half-bump shows up as "the
# launcher is broken". lib.sh:19-24 asks for the two lines to be bumped together
# and nothing checked it until this script.
#
# THE THIRD is the failure install.sh's own comment names: healthwatch.sh was
# missing from the copy list, the unit still started, and its ExecStart pointed
# at a file that was never there. A missing file and a running service look the
# same from `systemctl status`. novnc_patch.py was the second of these.
#
# It fails LOUDLY rather than skipping when it cannot find or parse something. A
# guard that quietly does nothing when a pattern changes is worse than no guard:
# the check stays green and the reader believes the two sides were compared.
#
# Runs on the host (macOS or Linux) with a POSIX shell and awk. No target, no
# Docker, no cross-compiler, no Python. It takes a fraction of a second:
#
#     sh tools/conf-crosscheck.sh
#
# What it does NOT check:
#
#   * That a name in rb.conf is READ by anything. rb.conf's own line 1214 says
#     every value above it is a shipped default; whether a shim or a script looks
#     at one is a question about the C sources, and grep is a poor witness.
#   * That a runtime script's `$HERE/<file>` reference is copied to the deploy
#     root. That direction is real -- novnc_patch.py was one -- but the deploy root
#     also receives the unit files and the two conf drop-ins by a second path in
#     install.sh, so a naive scan is a false-positive machine. The ExecStart check
#     below covers the same hazard from the one side that is unambiguous.
#   * Anything about the unit at all. It compares this checkout's files.

set -u

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
DEV="$ROOT/scripts/device"
START="$DEV/start-rb.sh"
CONF="$DEV/rb.conf"
LIB="$DEV/lib.sh"
INSTALL="$DEV/install.sh"

for f in "$START" "$CONF" "$LIB" "$INSTALL"; do
    if [ ! -f "$f" ]; then
        echo "conf-crosscheck: $f is missing." >&2
        echo "  All four are tracked files; a checkout without them cannot run" >&2
        echo "  this check, and saying so is the point -- do not skip it silently." >&2
        exit 1
    fi
done

# Every failure is collected rather than exiting on the first, because these are
# independent pairs and a reader fixing one wants to see the rest in the same run.
FAILED=0
fail() {
    echo "conf-crosscheck: $*" >&2
    FAILED=1
}

# --- 1. every SHIM_VARS name has an RB_ line in rb.conf ----------------------
#
# The list is a quoted multi-line assignment, so it is read between SHIM_VARS="
# and the closing quote rather than by sourcing start-rb.sh -- sourcing it would
# start reading rb.conf and touching the unit's paths.
names=$(awk '/^SHIM_VARS="$/ { inside=1; next } inside && /^"$/ { inside=0 } inside' "$START" | tr -d '"')
n_names=$(printf '%s\n' "$names" | wc -w | tr -d ' ')

# A sanity check on the extraction itself: a mis-slice that happens to yield a
# handful of names would make the rest of this silently vacuous.
if [ "${n_names:-0}" -lt 20 ]; then
    fail "read only ${n_names:-0} names out of SHIM_VARS -- the list changed shape."
    fail "  Expected the quoted assignment at start-rb.sh:295; nothing was compared."
    exit 1
fi

# Only definitions count, not prose: a comment naming RB_FOO must not satisfy the
# check for FOO. The two forms in the file are `: "${RB_X:=...}"`, which rb.conf
# uses throughout.
defined=$(sed -n 's/^: "${\(RB_[A-Za-z0-9_]*\):=.*/\1/p' "$CONF")
n_defined=$(printf '%s\n' "$defined" | wc -w | tr -d ' ')
if [ "${n_defined:-0}" -lt 20 ]; then
    fail "read only ${n_defined:-0} definitions out of rb.conf -- its shape changed."
    exit 1
fi

missing=""
for v in $names; do
    printf '%s\n' "$defined" | grep -qx "RB_$v" || missing="$missing $v"
done

if [ -n "$missing" ]; then
    fail "these names are exported by start-rb.sh with no line in rb.conf:"
    for v in $missing; do
        fail "    $v   (needs  : \"\${RB_$v:=...}\"  in scripts/device/rb.conf)"
    done
    fail "  Being in SHIM_VARS exports them as the EMPTY string, over the top of"
    fail "  any ad-hoc value, so the setting cannot be reached."
fi

# --- 2. the schema pair -----------------------------------------------------
conf_v=$(sed -n 's/^RB_CONF_VERSION=\([0-9][0-9]*\).*/\1/p' "$CONF")
lib_v=$(sed -n 's/^RB_CONF_SCHEMA=\([0-9][0-9]*\).*/\1/p' "$LIB")

if [ -z "$conf_v" ] || [ -z "$lib_v" ]; then
    fail "could not read the schema pair (rb.conf RB_CONF_VERSION='$conf_v',"
    fail "  lib.sh RB_CONF_SCHEMA='$lib_v') -- one of the two lines was renamed."
elif [ "$conf_v" != "$lib_v" ]; then
    fail "rb.conf declares schema v$conf_v and lib.sh expects v$lib_v."
    fail "  Bump both together (lib.sh:19-24); on the unit this fails closed and"
    fail "  looks like the launcher is broken rather than like a version skew."
fi

# --- 3. every ExecStart under the deploy root is in install.sh's copy list ---
#
# The copy list is the single `for s in ...; do` line. Only ExecStart-family lines
# are read, so a WorkingDirectory= or an [Install] stanza naming the deploy root
# cannot be mistaken for a program the unit runs.
# The copy list is one long line, so it is split to one name per line: membership
# is then an exact line compare, where a word match would let "dev.sh" be
# satisfied by "fix-dev.sh".
copy=$(sed -n 's/^for s in \(.*\); do$/\1/p' "$INSTALL" | tr ' ' '\n' | grep -v '^$')
n_copy=$(printf '%s\n' "$copy" | wc -l | tr -d ' ')
if [ "${n_copy:-0}" -lt 10 ]; then
    fail "read only ${n_copy:-0} names out of install.sh's copy list -- its shape changed."
    exit 1
fi

for unit in "$DEV"/*.service; do
    [ -f "$unit" ] || continue
    for prog in $(grep -E '^Exec(Start|StartPre|Stop|StopPost|Reload)=' "$unit" |
                  grep -o '/opt/rbp/[A-Za-z0-9_.-]*' | sed 's|.*/||' | sort -u); do
        printf '%s\n' "$copy" | grep -qx "$prog" && continue
        if [ -f "$DEV/$prog" ]; then
            fail "$(basename "$unit") runs $prog, which exists but is not in"
            fail "  install.sh's copy list -- the unit would start against a file"
            fail "  that never reached the deploy root. (This is the healthwatch.sh"
            fail "  bug; see the comment above that list.)"
        else
            fail "$(basename "$unit") runs $prog, which is not in scripts/device/"
            fail "  at all -- the unit names a program that does not exist."
        fi
    done
done

# --- verdict ---------------------------------------------------------------
if [ "$FAILED" -ne 0 ]; then
    echo "conf-crosscheck: FAILED. The three pairs are not in step -- see above." >&2
    exit 1
fi

echo "conf-crosscheck: $n_names SHIM_VARS names all defined in rb.conf;"
echo "                 schema v$conf_v agrees with lib.sh; every unit ExecStart"
echo "                 is in install.sh's copy list."
