#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Host test for scripts/stix/aspect-smoke.sh: no QEMU is booted.  Every
# leg's --dry-run argv carries its own knobs, TCs and expectations (the
# AS-5 legs, D-ASh rows 2-7 of docs/stix-port/2026-11-13-aspect-plan.md),
# the nine legs differ from each other, argument errors exit 2, and
# --check-guard refuses the shared run dirs and /.
#
# Usage: scripts/stix/tests/test-aspect-smoke.sh

set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SMOKE="$HERE/../aspect-smoke.sh"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
PASS=0
FAIL=0
# any binary path: dry-run and check-guard never exec it
QEMU="$TMP/qemu-system-sparc"
: > "$QEMU"
chmod +x "$QEMU"

report() { # name ok|<failure detail>
    if [ "$2" = "ok" ]; then
        echo "ok   - $1"
        PASS=$((PASS + 1))
    else
        echo "FAIL - $1: $2"
        FAIL=$((FAIL + 1))
    fi
}

dry() { # leg-flag... -> $out, $rc (slot-independent)
    out="$(STIX_PORT_BASE=auto bash "$SMOKE" "$QEMU" "$@" --dry-run 2>&1)"
    rc=$?
    # the dry run prints shell-quoted argv (\"value\":\ 6\}): drop the
    # backslashes so the checks read plain JSON
    out="${out//\\/}"
}

# want <name> <fixed-string>...: every string occurs in $out
want() {
    local name="$1" s miss=""
    shift
    for s in "$@"; do
        grep -qF -- "$s" <<< "$out" || miss="$miss [$s]"
    done
    if [ "$rc" -ne 0 ]; then
        report "$name" "rc=$rc: $(tail -3 <<< "$out")"
    elif [ -n "$miss" ]; then
        report "$name" "missing:$miss"
    else
        report "$name" ok
    fi
}

# lacks <name> <fixed-string>...: none occurs in $out
lacks() {
    local name="$1" s hit=""
    shift
    for s in "$@"; do
        grep -qF -- "$s" <<< "$out" && hit="$hit [$s]"
    done
    # never vacuous: the dry run itself must have succeeded
    if [ "$rc" -ne 0 ]; then
        report "$name" "rc=$rc"
    else
        report "$name" "$([ -z "$hit" ] && echo ok || echo "present:$hit")"
    fi
}

# TC(237,18) NUMBER_ASPECT_GAPS_ALLOWED = 100 and its TM(237,20) read-back
GAPS_TC="01DD0000000064"
GAPS_RB="0x5A8:237:20:01DD0000000064"
# the averaging-64 request (TC(238,1) level 5: ... 0040 ...) the #1031
# legs use, and the TM(5,2) EID 0x5440 the corrupted estimate raises
AVG64_REQ="0FFFFF0040"
EID5440="0x5A7:5:2:544000000001"

# ---- the legs ------------------------------------------------------
dry
want "ideal: dry run, forbids 0x5440, no knob" \
    "DRY RUN" "--forbid" "0x5A7:5:2:5440" "/tmp/stix-aspect-run-s"
# (its one qom-set is the AS-4 as-sun step, not an experiment knob)
lacks "ideal: no experiment knob, no gap-budget TC" "as-jitter-next-ms" \
    "as-step-next-ms" "as-rate-ppm" "as-drop-next" "$GAPS_TC"
printf '%s' "$out" > "$TMP/ideal"

dry --negative
want "negative: as-enable=false, own run dir" \
    "grlib-stixctrl.as-enable=false" "stix-aspect-run-neg-s"
printf '%s' "$out" > "$TMP/negative"

dry --jitter
want "jitter: as-jitter-next-ms 6, expects EID 0x5440" \
    "as-jitter-next-ms" '"value": 6}' "$EID5440" "stix-aspect-run-jitter-s"
want "jitter: the knob group fires at its own @150" \
    '--send-at 150 --qmp-cmd {"execute": "qom-set", "arguments": {"path": "/machine/stixctrl", "property": "as-jitter-next-ms"'
printf '%s' "$out" > "$TMP/jitter"

dry --step-gap
want "step-gap: as-step-next-ms 6" \
    "as-step-next-ms" '"value": 6}' "stix-aspect-run-step-gap-s"
printf '%s' "$out" > "$TMP/step-gap"

dry --drift
want "drift: as-rate-ppm -2000, gap budget 100 + read-back, avg 64" \
    "as-rate-ppm" '"value": -2000}' "$GAPS_TC" "$GAPS_RB" "$AVG64_REQ" \
    "$EID5440" "stix-aspect-run-drift-s"
want "drift: the rate knob fires at @55, before the @60 S20" \
    '--send-at 55 --qmp-cmd {"execute": "qom-set", "arguments": {"path": "/machine/stixctrl", "property": "as-rate-ppm"'
printf '%s' "$out" > "$TMP/drift"

dry --drop
want "drop: as-drop-next 2" \
    "as-drop-next" '"value": 2}' "stix-aspect-run-drop-s"
lacks "drop: no gap-budget TC" "$GAPS_TC"
printf '%s' "$out" > "$TMP/drop"

dry --avg
want "avg: the AvgSize TC and its acceptance" \
    "19EC0600" "0x5A1:1:7:1DACC00B" "stix-aspect-run-avg-s"
want "avg: the AvgSize TC is sent at @150" "--send-at 150 --send-hex"
printf '%s' "$out" > "$TMP/avg"

dry --step-1031
want "step-1031: gap budget 100 + read-back, avg 64, a second epoch" \
    "$GAPS_TC" "$GAPS_RB" "$AVG64_REQ" "$EID5440" \
    "stix-aspect-run-step-1031-s"
[ "$(grep -o -- "--send-at" <<< "$out" | wc -l)" -gt \
  "$(grep -o -- "--send-at" "$TMP/drift" | wc -l)" ]
report "step-1031: more TC sends than drift (the second epoch)" \
    "$([ $? -eq 0 ] && echo ok || echo "send-at count not above drift's")"
printf '%s' "$out" > "$TMP/step-1031"

dry --step-1031-control
want "step-1031-control: as-step-next-ms -1, own run dir" \
    "as-step-next-ms" '"value": -1}' "stix-aspect-run-step-1031c-s"
printf '%s' "$out" > "$TMP/step-1031c"

# ---- the nine legs differ (after the slot and run-dir names) ---------
norm() {
    sed -E 's/-s[0-9]+\b/-sN/g; s/stix-aspect-run(-[a-z0-9-]+)?-sN/RUN/g;
            s/\b21[0-9]{3}\b/PORT/g; s/slot: [0-9]+/slot: N/;
            s/^DRY RUN .*$//; s/\([a-z0-9-]+\):$//' "$1" | md5sum | cut -c1-32
}
dups="$(for f in "$TMP"/ideal "$TMP"/negative "$TMP"/jitter "$TMP"/step-gap \
        "$TMP"/drift "$TMP"/drop "$TMP"/avg "$TMP"/step-1031 \
        "$TMP"/step-1031c; do norm "$f"; done | sort | uniq -d)"
report "the nine legs' argvs all differ" \
    "$([ -z "$dups" ] && echo ok || echo "identical legs: $dups")"

# ---- argument errors -------------------------------------------------
out="$(STIX_PORT_BASE=auto bash "$SMOKE" "$QEMU" --drift --jitter \
    --dry-run 2>&1)"
rc=$?
[ "$rc" -eq 2 ] && grep -q 'leg given twice' <<< "$out"
report "two legs -> 2" "$([ $? -eq 0 ] && echo ok || echo "rc=$rc: $out")"
out="$(STIX_PORT_BASE=auto bash "$SMOKE" "$QEMU" --wobble 2>&1)"
rc=$?
[ "$rc" -eq 2 ] && grep -q 'unknown argument: --wobble' <<< "$out"
report "unknown argument -> 2" \
    "$([ $? -eq 0 ] && echo ok || echo "rc=$rc: $out")"

# ---- --check-guard ---------------------------------------------------
for d in / /tmp/stix-uprev-run /tmp/stix-uprev-run/x /tmp; do
    out="$(STIX_ASPECT_RUN_DIR="$d" STIX_PORT_BASE=auto \
        bash "$SMOKE" "$QEMU" --check-guard 2>&1)"
    rc=$?
    [ "$rc" -eq 2 ] && grep -q '^refusing run dir' <<< "$out"
    report "check-guard refuses $d" \
        "$([ $? -eq 0 ] && echo ok || echo "rc=$rc: $out")"
done
out="$(STIX_ASPECT_RUN_DIR="$TMP/run" STIX_PORT_BASE=auto \
    bash "$SMOKE" "$QEMU" --drift --check-guard 2>&1)"
rc=$?
report "check-guard accepts a private dir" \
    "$([ "$rc" -eq 0 ] && echo ok || echo "rc=$rc: $out")"

echo "test-aspect-smoke: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
