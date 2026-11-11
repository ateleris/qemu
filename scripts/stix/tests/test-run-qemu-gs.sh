#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Tests for scripts/stix/gs/run-qemu-gs.sh STIX_GS_QMP (P4-6, PL-2): the
# script is run against a stub "QEMU" that records its argv, never a real
# QEMU (no flash, no ports). The unix form must produce the same -qmp
# argument as before PL-2; the tcp form (PL-2, platform plan D-PLj)
# accepts only tcp:127.0.0.1:PORT and tcp:localhost:PORT.
#
# Usage: test-run-qemu-gs.sh [SCRIPT]   (default: the repo's script)
set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
GS="${1:-$REPO_ROOT/scripts/stix/gs/run-qemu-gs.sh}"
PASS=0
FAIL=0
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

report() { # name ok|<failure detail>
    if [ "$2" = "ok" ]; then
        echo "ok   - $1"
        PASS=$((PASS + 1))
    else
        echo "FAIL - $1: $2"
        FAIL=$((FAIL + 1))
    fi
}

# the stub: one argv element per line, then stay up past the script's
# 1 s startup check
cat > "$TMP/stub-qemu" <<'STUB'
#!/bin/sh
for a in "$@"; do printf '%s\n' "$a"; done > "$STUB_ARGV"
# one line the script's log filter keeps, so its pipeline ends with 0
echo "stixtmtc_stub argv recorded" >&2
sleep 2
STUB
chmod +x "$TMP/stub-qemu"

# run_gs QMP -> rc in $RC, the argv in $TMP/argv, stderr in $TMP/err
run_gs() {
    rm -f "$TMP/argv"
    STUB_ARGV="$TMP/argv" STIX_GS_RUN_DIR="$TMP/run" STIX_GS_QMP="$1" \
        STIX_ASW_ELF=/dev/null sh "$GS" "$TMP/stub-qemu" \
        > /dev/null 2> "$TMP/err"
    RC=$?
}

# the argument after -qmp, or "" when there is none
qmp_arg() {
    awk 'p { print; exit } $0 == "-qmp" { p = 1 }' "$TMP/argv" 2>/dev/null
}

run_gs ""
if [ "$RC" = 0 ] && ! grep -qx -- -qmp "$TMP/argv"; then
    report "unset: no -qmp" ok
else
    report "unset: no -qmp" "rc=$RC argv=$(tr '\n' ' ' < "$TMP/argv")"
fi

run_gs "$TMP/qmp.sock"
got="$(qmp_arg)"
want="unix:$TMP/qmp.sock,server=on,wait=off"
[ "$RC" = 0 ] && [ "$got" = "$want" ] && report "unix form unchanged" ok \
    || report "unix form unchanged" "rc=$RC got '$got'"

for addr in tcp:127.0.0.1:21519 tcp:localhost:21519; do
    run_gs "$addr"
    got="$(qmp_arg)"
    [ "$RC" = 0 ] && [ "$got" = "$addr,server=on,wait=off" ] \
        && report "accept $addr" ok \
        || report "accept $addr" "rc=$RC got '$got' $(cat "$TMP/err")"
done

for addr in tcp:0.0.0.0:21519 tcp:192.168.1.2:21519 tcp:127.0.0.1: \
            tcp:127.0.0.1:x tcp:127.0.0.1:1,y relative.sock /a,b; do
    run_gs "$addr"
    [ "$RC" = 2 ] && [ ! -e "$TMP/argv" ] && report "refuse $addr" ok \
        || report "refuse $addr" "rc=$RC"
done

echo "passed $PASS, failed $FAIL"
[ "$FAIL" = 0 ]
