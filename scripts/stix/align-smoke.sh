#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# ALIGN smoke: the ELUT-alignment gate (stimulus plan A3) and, with
# --adc, the ADC-faithful sky legs (C3). The elut-smoke skeleton: the
# run-dir guard, the slot library, --dry-run, --check-guard,
# --negative, SKIP 77 without the ELF. No flight CSV is needed: the
# gate writes its own synthetic .dssevs into its run dir (never a user
# file) and derives its own CSV from it.
#
# Align legs (the default, one boot, fresh flash): the gate writes a
# synthetic .dssevs with one 0xE setup word per dss_det, base
# 1800 + 10*d (distinct, so the derived CSV's rows differ across
# subcol and check-dump's D-Eg sensitivity stays meaningful), then
# ~6200 events at 1 per 100 ms tick on fpga_det 0, pix 0, adc 2050
# (both binnings put 2050 in ebin 3: the fallback floor(250*32/2296)=3;
# the derived edges e[2] = 2016, e[3] = 2087). QEMU runs with
# -global grlib-stixctrl.det-replay-file= (realize-time) and the
# stixdet_align / stixdet_elut_fallback / stixdet_replay_* / chunk
# traces on its log.
#   leg 1: CONF @35, S20 @40, det-align=true @45, det-source=replay-dss
#          @50, NOMINAL @55 (T_nom); @120 qom-get det-align-status must
#          return the fresh-flash line (fallback=384, honoured=0,
#          mismatch_cells=0); the stixdet_elut_fallback trace present;
#          CONF @130 (QL flush). ql-check --ssid32-subcols 0: the
#          counts at subcol 0 byte 4 (ebin 3 + 1) >= 0x30 (~320 events
#          in the first 8 QL periods at 10 events/s), other samples
#          byte 4 < 0x10.
#   leg 2: host: elut-derive.py derive --dssevs <synthetic> --out
#          <run>/derived.csv; elut-upload.py encode (96 x TC(237,7) at
#          150 + 0.75 k, each TM(1,1)/TM(1,7)@5; TC(237,8) at 227);
#          NOMINAL @240; @300 det-align-status = fallback=0,
#          honoured=384, mismatch_cells=0, calibration=dss-linear; no
#          stixdet_elut_fallback line after the leg-2 replay start, and
#          stixdet_align present; pmemsave @310 -> check-dump --csv
#          derived.csv exit 0; CONF @330; SSID 32 again at subcol 0
#          byte 4, the same ebin 3 (the observable bin did not move
#          when the ELUT arrived). The first ROTB chunk of leg 2
#          (traced stixdet_chunk) carries the replay counts in its
#          EACC (add_counts' inj_nom): its eacc_events total is the
#          integration's 1-per-tick events (200 +- phase). The
#          per-cell chunk decode of the det-inject-chunk qtest is not
#          reachable from the host (the ROTB ring is host RAM behind
#          an IO window); the cell-level proof lives in the qtests.
#   --negative: skip the derive/encode/upload -> leg 2 must fail
#          exactly the align-leg2 check (the status stays fallback=384)
#          and nothing else: the replay's fallback bins 2050 at ebin 3
#          as well, so leg 1 and the leg-2 SSID 32 still pass.
#
# ADC legs (--adc, four boots, a fresh flash each; the same distinct
# baselines from a 32-setup-word .dssevs, det-source=synthetic (the
# default) with det-quiet-cps=200, CONF @35, S20 @40):
#   leg 3 adc-reference: mode off; NOMINAL @50, CONF @150; subcol 0
#          byte 19 (ebin 18 + 1, the quiet template's 45 % line,
#          ~2880 counts in the 8 periods) is captured as the reference
#          bound, and the chunk trace shows the EACC total flowing.
#   leg 4 adc-mismatch: det-adc-mode=true, and a deliberately wrong
#          ELUT uploaded first (the CSV derived from base 0 - shifted:
#          the drawn ebin-18 adc range, 3092..3164 for base 1800, lands
#          in ebin 24 under the base-0 row); NOMINAL @135, CONF @235.
#          det-align-status has mismatch_cells >= 1 and
#          calibration=dss-linear; SSID 32 byte 19 <= 1/8 of the
#          reference (the wider base-0 bin keeps the flat floor) while byte 25
#          (ebin 24) stays within 5 sigma of the leg-3 reference (the
#          whole line moved, not lost - a mass-losing fold must fail);
#          the chunk's EACC total still flows.
#   leg 5 adc-aligned (run twice): the A3 derive/encode chain first:
#          mismatch_cells = 0; SSID 32 byte 19 within 5 sigma of the
#          leg-3 reference; the chunk total flows; the two runs' SSID
#          32 ebin bytes are identical (the fold's guest-time
#          determinism; the same seed boots each run).
#   --negative: skip leg 5's upload -> leg 5 fails exactly adc-aligned
#          (the status stays unaligned; with the degenerate rows the
#          fold keeps the mass at its own bin, so the byte19 log is the
#          record, not the witness).
#
# Clocks: no check compares times across clocks; the probe's monotonic
# stamps ([m=...]) separate the legs' log parts, and the trace checks
# read the model's vclock stamps only.
#
# Ports, run dir and lock (scripts/stix/lib/slot.sh): STIX_PORT_BASE
# unset is legacy (port 20000+uid; the caller holds the
# /tmp/stix-qtest-21000 flock), auto or k >= 1 takes a slot.
#
# Usage: align-smoke.sh <qemu-binary> [--adc] [--negative] [--dry-run]
#                       [--check-guard]
#   STIX_ASW_ELF         as in tc-smoke.sh; absent: SKIP, exit 77
#   STIX_ALIGN_RUN_DIR   run dir (default /tmp/stix-align-run; the mode
#                        suffixes -neg, -adc, -adcneg). Canonicalised;
#                        the shared uprev/baseline/GS run dirs, their
#                        parents and children and / are refused.
#   STIX_PORT_BASE       the slot, see above
#   STIX_TMTC_PROBE      the probe script (tests substitute a fake)
#   --dry-run prints the QEMU and probe command lines, boots nothing.
#   --check-guard only runs the run-dir guard and exits 0/2.
#
# Exit codes: 0 pass (a --negative passes when it failed exactly as
#             planned), 1 a failed check, 2 environment problem,
#             77 SKIP.
set -euo pipefail

usage="usage: align-smoke.sh <qemu-binary> [--adc] [--negative] \
[--dry-run] [--check-guard]"
QEMU_BIN_ARG="${1:?$usage}"
shift
ADC=0
NEGATIVE=0
DRY_RUN=0
CHECK_GUARD=0
while [ $# -gt 0 ]; do
    case "$1" in
        --adc)
            [ "$ADC" -eq 0 ] || { echo "--adc given twice"; exit 2; }
            ADC=1 ;;
        --negative)
            [ "$NEGATIVE" -eq 0 ] || { echo "--negative given twice"; exit 2; }
            NEGATIVE=1 ;;
        --dry-run)
            [ "$DRY_RUN" -eq 0 ] || { echo "--dry-run given twice"; exit 2; }
            DRY_RUN=1 ;;
        --check-guard)
            [ "$CHECK_GUARD" -eq 0 ] || {
                echo "--check-guard given twice"; exit 2; }
            CHECK_GUARD=1 ;;
        *) echo "unknown argument: $1"; echo "$usage"; exit 2 ;;
    esac
    shift
done

. "$(dirname "${BASH_SOURCE[0]}")/lib/paths.sh"
ASW_ELF="${STIX_ASW_ELF:-$STIX_DEFAULT_ELF}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TOOL="$REPO_ROOT/scripts/stix/elut-upload.py"
DERIVE="$REPO_ROOT/scripts/stix/elut-derive.py"
QLCHECK="$REPO_ROOT/scripts/stix/ql-check.py"
PROBE="${STIX_TMTC_PROBE:-$REPO_ROOT/scripts/stix/tmtc-probe.py}"
RUN_DIR="${STIX_ALIGN_RUN_DIR:-/tmp/stix-align-run}"
MODE_SFX=""
[ "$ADC" -eq 1 ] && MODE_SFX="-adc"
[ "$NEGATIVE" -eq 1 ] && MODE_SFX="$MODE_SFX-neg"

. "$REPO_ROOT/scripts/stix/lib/slot.sh"
# dry-run, check-guard and the SKIP boot nothing: they peek at the slot
if [ "$DRY_RUN" -eq 1 ] || [ "$CHECK_GUARD" -eq 1 ] || \
        [ ! -f "$ASW_ELF" ]; then
    stix_slot_setup peek
else
    stix_slot_setup
fi
RUN_DIR="$(stix_slot_dir "$RUN_DIR$MODE_SFX")"

# The shared flashes must never see this gate (a NOMINAL entry writes a
# flash context): canonical first, then refuse /, the shared run dirs,
# anything inside them and any parent of them.
RUN_DIR="$(realpath -m -- "$RUN_DIR")"
refuse_run_dir() {
    echo "refusing run dir $RUN_DIR ($1)"
    echo "this gate writes a flash context, so it needs its own run"
    echo "dir away from the shared ones"
    exit 2
}
[ "$RUN_DIR" != / ] || refuse_run_dir "it is the root directory"
for shared in "$STIX_UPREV_DIR" /tmp/stix-uprev-run \
              /tmp/stix-baseline-run /tmp/stix-gs-run; do
    shared="$(realpath -m -- "$shared")"
    case "$RUN_DIR" in
        "$shared" | "$shared"/*) refuse_run_dir "it is $shared or inside it" ;;
    esac
    case "$shared" in
        "$RUN_DIR"/*) refuse_run_dir "it is a parent of $shared" ;;
    esac
done
QMP_SOCK="$RUN_DIR/qmp.sock"
if [ "${#QMP_SOCK}" -ge 108 ]; then
    echo "refusing run dir $RUN_DIR (the QMP socket path $QMP_SOCK is" \
        "${#QMP_SOCK} bytes; a unix socket path must stay under 108)"
    exit 2
fi

if [ "$CHECK_GUARD" -eq 1 ]; then
    echo "GUARD OK: run dir $RUN_DIR (canonical)"
    exit 0
fi

QEMU_BIN="$(realpath -e "$QEMU_BIN_ARG")" || {
    echo "qemu binary not found: $QEMU_BIN_ARG"; exit 2; }
[ -x "$QEMU_BIN" ] || { echo "not executable: $QEMU_BIN"; exit 2; }

if [ ! -f "$ASW_ELF" ]; then
    echo "SKIP: ASW ELF absent ($ASW_ELF); set STIX_ASW_ELF to enable"
    exit 77
fi

# ---- the gate's constants ------------------------------------------------
ELUT_ADDR=3758096384    # 0xE0000000, stixctrl.sram
ELUT_SIZE=1572864       # 12 pix x 32 fpga_det x 4096 adc
BASE_LO=1800            # the synthetic baselines: 1800 + 10 * dss_det
BASE_STEP=10
ALIGN_ADC=2050          # the replay events' adc (ebin 3 both ways)
ALIGN_EVENTS=6200       # > 1650: neither leg's NOMINAL exhausts them
# the expected det-align-status lines (A1's format; the calibration is
# dss-linear because the gate's own .dssevs realizes the 0xE baselines)
STATUS_FRESH="mode=on,rows=384,fallback=384,honoured=0,mismatch_cells=0,unassigned_cells=0,calibration=dss-linear,preimage_miss=0"
STATUS_ALIGNED="mode=on,rows=384,fallback=0,honoured=384,mismatch_cells=0,unassigned_cells=0,calibration=dss-linear,preimage_miss=0"
# the ADC legs set det-adc-mode, not det-align: the same aligned rows
# with mode=off (a mode=on match there would never fire, and the
# negative's "not aligned" verdict would pass vacuously)
STATUS_ADC_ALIGNED="mode=off,rows=384,fallback=0,honoured=384,mismatch_cells=0,unassigned_cells=0,calibration=dss-linear,preimage_miss=0"

qom_set() {  # path property json-value (detector-smoke's form)
    printf '{"execute": "qom-set", "arguments": %s' \
        "{\"path\": \"$1\", \"property\": \"$2\", \"value\": $3}}"
}
qom_get() {  # path property
    printf '{"execute": "qom-get", "arguments": %s' \
        "{\"path\": \"$1\", \"property\": \"$2\"}}"
}
pmemsave() {  # name -> a --qmp-cmd JSON writing the ELUT dump
    printf '{"execute": "pmemsave", "arguments": %s}' \
        "$(printf '{"val": %s, "size": %s, "filename": "%s"}' \
            "$ELUT_ADDR" "$ELUT_SIZE" "$RUN_DIR/elut-dump.$1.bin")"
}

# the mode TCs, seqs 1..6, rebuilt with elut-upload.py (CRC included)
mapfile -t TC < <(python3 - "$TOOL" <<'EOF'
import importlib.util, sys
spec = importlib.util.spec_from_file_location("elut_upload", sys.argv[1])
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
for seq, svc, sub, app in ((1, 236, 1, "0400"),
                           (2, 236, 6, "07040000"),
                           (3, 236, 1, "0500"),
                           (4, 236, 1, "0400"),
                           (5, 236, 1, "0500"),
                           (6, 236, 1, "0400")):
    print(m.unit_hex(m.tc_unit(seq, svc, sub, bytes.fromhex(app))))
EOF
)
TC_CONF="${TC[0]}"      # CONFIGURATION, seq 1
TC_S20="${TC[1]}"       # S20 = 0, seq 2
TC_NOM="${TC[2]}"       # NOMINAL, seq 3
TC_CONF2="${TC[3]}"     # CONF (ends leg 1 / the ADC NOMINAL), seq 4
TC_NOM2="${TC[4]}"      # NOMINAL (leg 2), seq 5
TC_CONF3="${TC[5]}"     # CONF (ends leg 2), seq 6

mkdir -p "$RUN_DIR"
cd "$RUN_DIR"

# the synthetic .dssevs files (never a user file; D-Ed)
ALIGN_DSSEVS="$RUN_DIR/dssevs.align"
ADC_DSSEVS="$RUN_DIR/dssevs.adc"
DERIVED_CSV="$RUN_DIR/derived.csv"
WRONG_CSV="$RUN_DIR/wrong-base0.csv"
ALIGNED_CSV="$RUN_DIR/aligned.csv"

write_align_dssevs() {
    python3 - "$ALIGN_DSSEVS" "$ALIGN_EVENTS" "$ALIGN_ADC" \
        "$BASE_LO" "$BASE_STEP" <<'EOF'
import struct, sys
path, n, adc, base0, step = (sys.argv[1], int(sys.argv[2]),
                             int(sys.argv[3]), int(sys.argv[4]),
                             int(sys.argv[5]))
with open(path, "wb") as f:
    for d in range(32):
        f.write(struct.pack(">I", ((base0 + step * d) << 20)
                            | (0xE << 16) | (d << 11)))
    for _ in range(n):
        # fpga_det 0, pix 0, wait 0: the event lands at this tick ...
        f.write(struct.pack(">I", adc << 20))
        # ... then exactly 100.0 ms of wait-only padding
        # (2442 x 2047 + 1226 = 5e6 units of 20 ns), so the next
        # event is one 100 ms tick later
        for _ in range(2442):
            f.write(struct.pack(">I", (0xF << 16) | 2047))
        f.write(struct.pack(">I", (0xF << 16) | 1226))
EOF
}

write_adc_dssevs() {
    python3 - "$ADC_DSSEVS" "$BASE_LO" "$BASE_STEP" <<'EOF'
import struct, sys
path, base0, step = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
with open(path, "wb") as f:
    for d in range(32):
        f.write(struct.pack(">I", ((base0 + step * d) << 20)
                            | (0xE << 16) | (d << 11)))
EOF
}

encode_args() {  # csv t0 -> probe args in ELUT_ARGS, record in ELUT_RECORD
    local csv="$1" t0="$2" out err
    out="$(mktemp)"; err="$(mktemp)"
    if ! python3 "$TOOL" encode --csv "$csv" --lut-id 1 \
            --first-seq 0x010 --t0 "$t0" --pace 0.75 --emit probe-args \
            > "$out" 2> "$err"; then
        cat "$err"; rm -f "$out" "$err"
        echo "ALIGN-SMOKE: ERROR (elut-upload.py encode failed on $csv)"
        exit 2
    fi
    mapfile -t ELUT_ARGS < "$out"
    ELUT_RECORD="$(cat "$err")"
    rm -f "$out" "$err"
}

# the base-0 JSON of the C3 wrong CSV (decision D6's shift witness)
BASE0="{"
for d in $(seq 0 31); do
    [ "$d" -eq 0 ] || BASE0="$BASE0, "
    BASE0="$BASE0\"$d\": 0"
done
BASE0="$BASE0}"

fresh_flash() {
    rm -f flash.bin
    truncate -s 17716740096 flash.bin
}

PROBE_PID=""
QEMU_PID=""
cleanup() {
    if [ -n "$QEMU_PID" ]; then
        kill "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    if [ -n "$PROBE_PID" ]; then
        kill "$PROBE_PID" 2>/dev/null || true
        wait "$PROBE_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

VERDICT_MODE=""
[ "$NEGATIVE" -eq 0 ] || VERDICT_MODE=" (negative)"
[ "$ADC" -eq 0 ] || VERDICT_MODE=" (adc$VERDICT_MODE)"

fail() {  # reason
    echo "ALIGN-SMOKE$VERDICT_MODE: FAIL ($*)${STIX_SLOT_TAG}"
    exit 1
}

qemu_args() {  # tag dssevs [ctrl-global...]
    local tag="$1" dssevs="$2"; shift 2
    QEMU_ARGS=(
        -M leon3_generic -m 64M -no-reboot
        -display none -monitor none
        "${STIX_SLOT_QEMU_ARGS[@]}"
        -qmp "unix:$QMP_SOCK,server=on,wait=off"
        -serial "file:$RUN_DIR/uart.$tag.raw"
        -kernel "$ASW_ELF"
        -global "grlib-stixctrl.det-replay-file=$dssevs"
        -trace stixspw_rx_truncated
        -trace stixdet_align
        -trace stixdet_elut_fallback
        -trace stixdet_replay_scan
        -trace stixdet_replay_state
        -trace stixdet_chunk
    )
    local g
    for g in "$@"; do
        QEMU_ARGS+=(-global "grlib-stixctrl.$g")
    done
}

# One boot: the probe first (it retries until QEMU binds the port), then
# QEMU; the probe's verdict, with the reason, as in elut-smoke.
boot() {  # tag seconds probe-args...
    local tag="$1" secs="$2" rc alive qrc why log
    log="probe.$tag.log"
    shift 2
    rm -f "uart.$tag.raw" "uart.$tag.txt" "qemu-stdout.$tag.log" "$log"
    stix_disk_guard
    rm -f qmp.sock
    timeout "$((secs + 30))" python3 -u "$PROBE" "$@" > "$log" 2>&1 &
    PROBE_PID=$!
    timeout "$((secs + 30))" "$QEMU_BIN" "${QEMU_ARGS[@]}" \
        > "qemu-stdout.$tag.log" 2>&1 &
    QEMU_PID=$!
    set +e
    wait "$PROBE_PID"
    rc=$?
    PROBE_PID=""
    kill -0 "$QEMU_PID" 2>/dev/null
    alive=$?
    kill "$QEMU_PID" 2>/dev/null
    wait "$QEMU_PID" 2>/dev/null
    qrc=$?
    QEMU_PID=""
    set -e
    if [ "$alive" -ne 0 ]; then
        tail -5 "qemu-stdout.$tag.log"
        echo "ALIGN-SMOKE$VERDICT_MODE: ERROR (boot $tag: QEMU died or" \
            "never started; qemu rc=$qrc)${STIX_SLOT_TAG}"
        exit 2
    fi
    if [ "$rc" -eq 124 ]; then
        tail -5 "$log"
        echo "ALIGN-SMOKE$VERDICT_MODE: ERROR (boot $tag: the probe hit" \
            "its own timeout)${STIX_SLOT_TAG}"
        exit 2
    fi
    if grep -q "could not connect" "$log"; then
        tail -5 "$log"
        echo "ALIGN-SMOKE$VERDICT_MODE: ERROR (boot $tag: the probe" \
            "never connected)${STIX_SLOT_TAG}"
        exit 2
    fi
    if [ "$rc" -ne 0 ]; then
        why="$(grep -m1 -o 'FORBIDDEN TM: .*' "$log" | cut -d' ' -f3-)" ||
            true
        if [ -n "$why" ]; then
            why="forbidden: $why"
        else
            why="$(grep -m1 -o 'EXPECTATION NOT MATCHED: .*' "$log" |
                cut -d' ' -f4-)" || true
            if [ -n "$why" ]; then
                why="not matched: $why"
            elif grep -m1 -q '^QMP ERROR' "$log"; then
                why="$(grep -m1 '^QMP ERROR' "$log")"
            elif grep -m1 -q '^RETURN MISMATCH' "$log"; then
                why="$(grep -m1 '^RETURN MISMATCH' "$log" | cut -c1-160)"
            elif grep -q '^CONNECTION CLOSED' "$log"; then
                why="connection closed"
            else
                why="probe rc=$rc; see $log"
            fi
        fi
        tail -5 "$log"
        fail "boot $tag: $why"
    fi
    tr -d '\000' < "uart.$tag.raw" > "uart.$tag.txt"
    if grep -q 'stixspw_rx_truncated' "qemu-stdout.$tag.log"; then
        grep -m1 'stixspw_rx_truncated' "qemu-stdout.$tag.log"
        fail "boot $tag: a TC unit was truncated at SpW RX"
    fi
}

uart_ok() {  # tag [applied]: the boot's UART rules
    local tag="$1" applied="${2:-0}" n
    grep -q "init: SAFE mode" "uart.$tag.txt" ||
        fail "boot $tag UART: no 'init: SAFE mode' banner"
    grep -q "could not find LUT (type=3, id=0" "uart.$tag.txt" ||
        fail "boot $tag UART: no fresh-flash 'could not find LUT (type=3"
    for t in "problem writing LUT" "problem reading LUT (type=3" \
             "ELUT update got cancelled" "DetectorsApi_setElut(" \
             "storing new context0 failed" "storing new context1 failed"; do
        if grep -qF -- "$t" "uart.$tag.txt"; then
            grep -m1 -F -- "$t" "uart.$tag.txt"
            fail "boot $tag UART: '$t' present"
        fi
    done
    n="$(grep -c 'problem erasing LUT type=3,' "uart.$tag.txt" || true)"
    if [ "$applied" -eq 1 ] && [ "$n" -ne 4 ]; then
        fail "boot $tag UART: $n 'problem erasing LUT type=3' lines, " \
            "want 4 (one per copy)"
    fi
    if [ "$applied" -eq 0 ] && [ "$n" -ne 0 ]; then
        fail "boot $tag UART: $n 'problem erasing LUT type=3' lines" \
            "without TC(237,8)"
    fi
}

# the probe log's det-align-status return (the last qom-get of it)
align_status() {  # probe-log
    grep -F '"property": "det-align-status' "$1" | tail -1 ||
        true
}

ssid32_products() {  # dump tag: the SSID 32 packets of a flush dump
    # the SSID byte follows the 6-byte primary header, the 4-byte data
    # field header and the 6-byte time: hex column 33; SSID 32 = 0x20
    awk '{ if (substr($0, 33, 2) == "20")
               { n++; print > (sprintf("%s.ssid32.%d.hex", tag, n)) } }' \
        tag="$2" "$1"
}

dump_ebin_bytes() {  # dump out: the 32 samples' ebin bytes, one hex line
    python3 - "$QLCHECK" "$1" "$2" <<'EOF'
import importlib.util, sys
spec = importlib.util.spec_from_file_location("ql_check", sys.argv[1])
q = importlib.util.module_from_spec(spec)
spec.loader.exec_module(q)
with open(sys.argv[2]) as f:
    prods = q.reassemble(q.read_dump(f)).get(32, [])
if len(prods) != 1:
    raise SystemExit("SSID 32: %d products, want 1" % len(prods))
with open(sys.argv[3], "w") as f:
    for s in prods[0]["samples"]:
        f.write("".join("%02x" % c for c in s["ebin"]))
    f.write("\n")
EOF
}

chunk_total() {  # qemu-log: the first chunk's eacc_events, or ""
    grep -o 'eacc_events=[0-9]*' "$1" 2>/dev/null | head -1 |
        cut -d= -f2 || true
}

build_adc_args() {  # leg 4|5: the probe args in ADC4_ARGS / ADC5_ARGS
                    # (the leg's upload groups, the qom-get, NOMINAL
                    # @135, CONF @235 + the exit flush)
    local leg="$1"
    local args=(
        --port "$TMTC_PORT" --seconds 290 --window 20 --hold
        --qmp "$QMP_SOCK"
        --dump-apid "0x5DC:$RUN_DIR/ql.dump.$leg.hex"
        --forbid "0x5A1:1:2"
        --forbid "0x5A1:1:8"
        --send-at 35 --send-hex "$TC_CONF"
        --expect "0x5A1:1:1:1DACC001@12"
        --expect "0x5A7:5:1:00210204@12"
        --expect "0x5A1:1:7:1DACC001@12"
        --forbid "0x5A7:5:1:00210402"
        --send-at 40 --send-hex "$TC_S20"
        --expect "0x5A1:1:1:1DACC002@8"
        --expect "0x5A1:1:7:1DACC002@8"
    )
    if [ "$leg" -eq 4 ]; then
        args+=(${ELUT4_ARGS[@]+"${ELUT4_ARGS[@]}"})
    else
        args+=(${ELUT5_ARGS[@]+"${ELUT5_ARGS[@]}"})
    fi
    args+=(
        --send-at 135 --send-hex "$TC_NOM"
        --expect "0x5A1:1:1:1DACC003"
        --expect "0x5A7:5:1:00210405@45"
        --expect "0x5A1:1:7:1DACC003@45"
        --expect "0x5A7:5:2:542c01@45"
        --forbid "0x5A7:5:2:542c03"
        --forbid "0x5A7:5:1:00210502"
        --forbid "0x5A7:5:1:142e"
        --forbid "0x5A7:5:3:9449"
        --send-at 225 --qmp-cmd "$(qom_get /machine/stixctrl \
            det-align-status)"
        --send-at 235 --send-hex "$TC_CONF2"
        --expect "0x5A1:1:1:1DACC004"
        --expect "0x5A7:5:1:00210504@10"
        --expect "0x5A1:1:7:1DACC004@10"
        --forbid "0x5A7:5:1:00210402"
        --expect "0x5DC:21:6:1e@15"
        --expect "0x5DC:21:6:1f@15"
        --expect "0x5DC:21:6:20@15"
        --expect "0x5DC:21:6:21@15"
        --expect "0x5DC:21:6:22@15"
        --hold
    )
    if [ "$leg" -eq 4 ]; then
        ADC4_ARGS=("${args[@]}")
    else
        ADC5_ARGS=("${args[@]}")
    fi
}

build_adc3_args() {  # the reference leg: mode off, NOMINAL @50,
                     # CONF @150 + the exit flush
    ADC3_ARGS=(
        --port "$TMTC_PORT" --seconds 200 --window 20 --hold
        --qmp "$QMP_SOCK"
        --dump-apid "0x5DC:$RUN_DIR/ql.dump.adc3.hex"
        --forbid "0x5A1:1:2"
        --forbid "0x5A1:1:8"
        --send-at 35 --send-hex "$TC_CONF"
        --expect "0x5A1:1:1:1DACC001@12"
        --expect "0x5A7:5:1:00210204@12"
        --expect "0x5A1:1:7:1DACC001@12"
        --forbid "0x5A7:5:1:00210402"
        --send-at 40 --send-hex "$TC_S20"
        --expect "0x5A1:1:1:1DACC002@8"
        --expect "0x5A1:1:7:1DACC002@8"
        --send-at 50 --send-hex "$TC_NOM"
        --expect "0x5A1:1:1:1DACC003"
        --expect "0x5A7:5:1:00210405@45"
        --expect "0x5A1:1:7:1DACC003@45"
        --expect "0x5A7:5:2:542c01@45"
        --forbid "0x5A7:5:2:542c03"
        --forbid "0x5A7:5:1:00210502"
        --forbid "0x5A7:5:1:142e"
        --forbid "0x5A7:5:3:9449"
        --send-at 150 --send-hex "$TC_CONF2"
        --expect "0x5A1:1:1:1DACC004"
        --expect "0x5A7:5:1:00210504@10"
        --expect "0x5A1:1:7:1DACC004@10"
        --forbid "0x5A7:5:1:00210402"
        --expect "0x5DC:21:6:1e@15"
        --expect "0x5DC:21:6:1f@15"
        --expect "0x5DC:21:6:20@15"
        --expect "0x5DC:21:6:21@15"
        --expect "0x5DC:21:6:22@15"
        --hold
    )
}

if [ "$ADC" -eq 1 ]; then
    # ---- the C3 ADC legs -------------------------------------------------
    write_adc_dssevs
    build_adc3_args
    FAILED=()
    add_fail() { FAILED+=("$1"); }
    # the two uploads (host-only, before any boot; the negative skips
    # leg 5's)
    python3 "$DERIVE" derive --base "$BASE0" --out "$WRONG_CSV" \
        > derive.wrong.log 2>&1 ||
        { cat derive.wrong.log;
          echo "ALIGN-SMOKE: ERROR (the base-0 derive failed)"; exit 2; }
    encode_args "$WRONG_CSV" 50
    ELUT4_ARGS=("${ELUT_ARGS[@]}")
    python3 "$DERIVE" derive --dssevs "$ADC_DSSEVS" \
        --out "$ALIGNED_CSV" > derive.aligned.log 2>&1 ||
        { cat derive.aligned.log;
          echo "ALIGN-SMOKE: ERROR (the aligned derive failed)"; exit 2; }
    encode_args "$ALIGNED_CSV" 50
    ELUT5_ARGS=("${ELUT_ARGS[@]}")
    [ "$NEGATIVE" -eq 0 ] || ELUT5_ARGS=()

    if [ "$DRY_RUN" -eq 1 ]; then
        echo "DRY RUN — the commands align-smoke would run" \
            "(adc$VERDICT_MODE):"
        echo "# wrong CSV: $(head -1 derive.wrong.log)"
        echo "# aligned CSV: $(head -1 derive.aligned.log)"
        i=0
        for leg in 3 4 5 5; do
            tag="adc$leg"
            [ "$leg" -ne 5 ] || tag="adc5$([ "$i" -eq 3 ] && echo b ||
                echo a)"
            case "$leg" in
                3) secs=200; build_adc3_args; pa=("${ADC3_ARGS[@]}")
                   qemu_args "$tag" "$ADC_DSSEVS" det-quiet-cps=200 ;;
                4) secs=290; build_adc_args 4; pa=("${ADC4_ARGS[@]}")
                   qemu_args "$tag" "$ADC_DSSEVS" det-quiet-cps=200 \
                       det-adc-mode=true ;;
                *) secs=290; build_adc_args 5; pa=("${ADC5_ARGS[@]}")
                   qemu_args "$tag" "$ADC_DSSEVS" det-quiet-cps=200 \
                       det-adc-mode=true ;;
            esac
            echo "# boot $tag"
            printf '(cd %s && timeout %s %s ' "$RUN_DIR" "$((secs + 30))" \
                "$QEMU_BIN"
            printf '%q ' "${QEMU_ARGS[@]}"
            printf '> qemu-stdout.%s.log 2>&1)\n' "$tag"
            printf 'python3 -u %s ' "$PROBE"
            printf '%q ' "${pa[@]}"
            printf '\n'
            i=$((i + 1))
        done
        exit 0
    fi

    # leg 3: the reference (mode off)
    fresh_flash
    rm -f ql.dump.adc3.hex
    qemu_args adc3 "$ADC_DSSEVS" det-quiet-cps=200
    boot adc3 200 "${ADC3_ARGS[@]}"
    uart_ok adc3 0
    ssid32_products ql.dump.adc3.hex adc3
    set +e
    REF_OUT="$(python3 - "$QLCHECK" adc3.ssid32.1.hex <<'EOF'
import importlib.util, sys
spec = importlib.util.spec_from_file_location("ql_check", sys.argv[1])
q = importlib.util.module_from_spec(spec)
spec.loader.exec_module(q)
with open(sys.argv[2]) as f:
    prods = q.reassemble(q.read_dump(f)).get(32, [])
if len(prods) != 1:
    raise SystemExit("no single SSID 32 dump")
tgt = [s for s in prods[0]["samples"] if s["subcol"] == 0]
if len(tgt) != 1:
    raise SystemExit("no subcol 0 sample")
lo, hi = q.s0_decode(tgt[0]["ebin"][18])
lo19, hi19 = q.s0_decode(tgt[0]["ebin"][19])
print("%d %d %d %d" % (lo, hi, lo19, hi19))
EOF
)"
    REF_RC=$?
    set -e
    REF_LO=""
    REF_HI=""
    [ "$REF_RC" -ne 0 ] || read -r REF_LO REF_HI REF19_LO REF19_HI <<< "$REF_OUT"
    C_MIN=""
    if [ -n "$REF_LO" ] && [ "$REF_LO" -ge 256 ]; then
        # the leg-3 count lies in [REF_LO, REF_HI); its 5-sigma band is
        # [REF_LO - 5*sqrt(REF_LO), REF_HI + 5*sqrt(REF_LO)) — the S0
        # window the ebin-24 arrival (leg 4) and the return (leg 5) of
        # the same mass must fall into
        C_MIN="$(python3 -c '
import importlib.util, sys
spec = importlib.util.spec_from_file_location("ql_check", sys.argv[1])
q = importlib.util.module_from_spec(spec)
spec.loader.exec_module(q)
lo = int(sys.argv[2])
print(q.s0_compress(max(0, int(lo - 5 * lo ** 0.5))))
' "$QLCHECK" "$REF_LO")"
        C_MAX="$(python3 -c '
import importlib.util, sys
spec = importlib.util.spec_from_file_location("ql_check", sys.argv[1])
q = importlib.util.module_from_spec(spec)
spec.loader.exec_module(q)
hi = int(sys.argv[3])
print(q.s0_compress(int(hi + 5 * int(sys.argv[2]) ** 0.5)))
' "$QLCHECK" "$REF_LO" "$REF_HI")"
    fi
    # leg 4's ebin-24 window: under the base-0 row ebin 24 is adc
    # [3072, 3200), the preimage of all of ebin 18 [3092, 3164) and the
    # lower 36/71 of ebin 19 [3164, 3235) (the template's 10 % Ba-133
    # line); the 20-adc ebin-17 sliver is flat floor, inside 5 sigma
    C4_MIN=""
    C4_MAX=""
    if [ -n "$C_MIN" ]; then
        read -r C4_MIN C4_MAX <<< "$(python3 -c '
import importlib.util, sys
spec = importlib.util.spec_from_file_location("ql_check", sys.argv[1])
q = importlib.util.module_from_spec(spec)
spec.loader.exec_module(q)
lo = int(sys.argv[2]) + int(sys.argv[4]) * 36 // 71
hi = int(sys.argv[3]) + -(-int(sys.argv[5]) * 36 // 71)
print(q.s0_compress(max(0, int(lo - 5 * lo ** 0.5))),
      q.s0_compress(int(hi + 5 * lo ** 0.5)))
' "$QLCHECK" "$REF_LO" "$REF_HI" "$REF19_LO" "$REF19_HI")"
    fi
    CT3="$(chunk_total qemu-stdout.adc3.log)"
    # leg 4's ceiling at ebin 18: the line has left, at most 1/8 of the
    # reference (the base-0 bin is 128 adc wide, so the flat floor of
    # ~1.8 template bins stays there; the line alone is >= REF_LO)
    C_GONE=""
    [ -z "$REF_LO" ] || C_GONE="$(python3 -c '
import importlib.util, sys
spec = importlib.util.spec_from_file_location("ql_check", sys.argv[1])
q = importlib.util.module_from_spec(spec)
spec.loader.exec_module(q)
print(q.s0_compress(int(sys.argv[2]) // 8))
' "$QLCHECK" "$REF_LO")"
    if [ -z "$C_MIN" ]; then
        echo "CHECK adc-reference: FAIL (no SSID 32 dump, or the subcol 0" \
            "ebin 18 reference below the floor: ${REF_LO:-none})"
        add_fail adc-reference
    elif [ -z "$CT3" ] || [ "$CT3" -eq 0 ]; then
        echo "CHECK adc-reference: FAIL (no chunk EACC total in the trace)"
        add_fail adc-reference
    else
        echo "adc-reference: subcol 0 ebin 18 decoded lower bound" \
            "$REF_LO counts; the 5-sigma S0 window [$C_MIN, $C_MAX]"
        echo "CHECK adc-reference: ok (byte 19 >= 0x30, the chunk EACC" \
            "total flows)"
    fi

    # leg 4: the mismatch (the fold moves the line to ebin 24); the
    # wrong (base-0) CSV and its upload groups were built above
    build_adc_args 4
    fresh_flash
    rm -f ql.dump.4.hex
    qemu_args adc4 "$ADC_DSSEVS" det-quiet-cps=200 det-adc-mode=true
    boot adc4 290 "${ADC4_ARGS[@]}"
    uart_ok adc4 1      # leg 4 always uploads (--negative skips leg 5's)
    ssid32_products ql.dump.4.hex adc4
    RET="$(align_status probe.adc4.log)"
    MISMATCH="$(printf '%s' "$RET" |
        sed -n 's/.*mismatch_cells=\([0-9]*\).*/\1/p')" || true
    ADC4_OK=1
    if [ -z "$C_MIN" ]; then
        ADC4_OK=0; ADC4_WHY="no leg-3 reference to compare against"
    elif ! printf '%s' "$RET" | grep -qF 'calibration=dss-linear'; then
        ADC4_OK=0; ADC4_WHY="no det-align-status return"
    elif [ -z "$MISMATCH" ] || [ "$MISMATCH" -lt 1 ]; then
        ADC4_OK=0; ADC4_WHY="mismatch_cells=${MISMATCH:-none}, want >= 1"
    elif [ ! -s adc4.ssid32.1.hex ]; then
        ADC4_OK=0; ADC4_WHY="no SSID 32 dump in the flush"
    else
        set +e
        # --ssid32-byte takes the 0-based ebin: ebin 18 is sample
        # byte 19 (byte 0 is the detector byte), ebin 24 byte 25
        python3 "$QLCHECK" adc4.ssid32.1.hex --ssid32-byte 0 18 0 \
            "$C_GONE" > byte19.adc4.log 2>&1
        B19=$?
        python3 "$QLCHECK" adc4.ssid32.1.hex --ssid32-byte 0 24 \
            "$C4_MIN" "$C4_MAX" > byte25.adc4.log 2>&1
        B25=$?
        set -e
        grep -hE '^QL-CHECK' byte19.adc4.log byte25.adc4.log || true
        CT="$(chunk_total qemu-stdout.adc4.log)"
        if [ "$B19" -ne 0 ] || [ "$B25" -ne 0 ]; then
            ADC4_OK=0
            ADC4_WHY="byte19 rc=$B19, byte25 rc=$B25 (see the byte logs)"
        elif [ -z "$CT" ] || [ "$CT" -eq 0 ]; then
            ADC4_OK=0; ADC4_WHY="the chunk EACC total stopped flowing"
        fi
    fi
    if [ "$ADC4_OK" -eq 1 ]; then
        echo "CHECK adc-mismatch: ok (mismatch_cells=$MISMATCH," \
            "calibration=dss-linear, the line left byte 19 (<= 1/8 of" \
            "the reference), arrived at byte 25 = ebin 24 within 5 sigma" \
            "of its preimage (ebin 18 + 36/71 of ebin 19)," \
            "the chunk total flows)"
    else
        echo "CHECK adc-mismatch: FAIL ($ADC4_WHY)"
        add_fail adc-mismatch
    fi

    # leg 5, twice: the aligned ELUT (the fold is the identity again);
    # the aligned CSV and its upload groups were built above (and
    # emptied for the negative)
    build_adc_args 5
    for run in a b; do
        if [ "$run" = b ] && [ "$NEGATIVE" -eq 1 ]; then
            break           # the negative fails on one leg-5 boot
        fi
        fresh_flash
        rm -f ql.dump.5.hex
        qemu_args "adc5$run" "$ADC_DSSEVS" det-quiet-cps=200 \
            det-adc-mode=true
        boot "adc5$run" 290 "${ADC5_ARGS[@]}"
        uart_ok "adc5$run" "$([ "$NEGATIVE" -eq 0 ] && echo 1 || echo 0)"
        ssid32_products ql.dump.5.hex "adc5$run"
        RET="$(align_status "probe.adc5$run.log")"
        ALIGNED_OK=1
        if [ -z "$C_MIN" ]; then
            ALIGNED_OK=0
            ALIGNED_WHY="no leg-3 reference to compare against"
        elif [ "$NEGATIVE" -eq 0 ]; then
            printf '%s' "$RET" | grep -qF "$STATUS_ADC_ALIGNED" || {
                ALIGNED_OK=0
                ALIGNED_WHY="the status is not the aligned line"
            }
        else
            printf '%s' "$RET" | grep -qF "$STATUS_ADC_ALIGNED" && {
                ALIGNED_OK=0
                ALIGNED_WHY="the negative aligned the status"
            }
        fi
        if [ ! -s "adc5$run.ssid32.1.hex" ]; then
            ALIGNED_OK=0
            ALIGNED_WHY="no SSID 32 dump in the flush"
        else
            set +e
            python3 "$QLCHECK" "adc5$run.ssid32.1.hex" \
                --ssid32-byte 0 18 "$C_MIN" "$C_MAX" \
                > "byte19.adc5$run.log" 2>&1
            B19=$?
            set -e
            grep -E '^QL-CHECK' "byte19.adc5$run.log" || true
            CT="$(chunk_total "qemu-stdout.adc5$run.log")"
            if [ "$NEGATIVE" -eq 0 ]; then
                if [ "$B19" -ne 0 ]; then
                    ALIGNED_OK=0
                    ALIGNED_WHY="byte19 rc=$B19 (see byte19.adc5$run.log)"
                elif [ -z "$CT" ] || [ "$CT" -eq 0 ]; then
                    ALIGNED_OK=0
                    ALIGNED_WHY="the chunk EACC total stopped flowing"
                fi
            else
                # the negative's verdict is the status line: with the
                # mode on and no upload the rows are degenerate, so the
                # fold keeps the mass at its own bin (byte 19 high)
                # while the status still reports fallback=384
                if printf '%s' "$RET" | grep -qF "$STATUS_ADC_ALIGNED"; then
                    ALIGNED_OK=0
                    ALIGNED_WHY="the negative aligned the status"
                fi
            fi
        fi
        if [ "$ALIGNED_OK" -eq 1 ]; then
            if [ "$NEGATIVE" -eq 0 ]; then
                echo "CHECK adc-aligned (run $run): ok (mismatch_cells=0," \
                    "byte 19 back at the reference, the chunk total flows)"
            else
                echo "CHECK adc-aligned: FAIL as planned (no upload: the" \
                    "status stays unaligned; byte19.adc5$run.log holds" \
                    "the fold's bin for the record)"
                add_fail adc-aligned
            fi
        else
            echo "CHECK adc-aligned (run $run): FAIL ($ALIGNED_WHY)"
            add_fail adc-aligned
        fi
    done
    if [ "$NEGATIVE" -eq 0 ]; then
        if [ -s adc5a.ssid32.1.hex ] && [ -s adc5b.ssid32.1.hex ]; then
            dump_ebin_bytes adc5a.ssid32.1.hex adc5a.bytes &&
                dump_ebin_bytes adc5b.ssid32.1.hex adc5b.bytes || true
            if cmp -s adc5a.bytes adc5b.bytes; then
                echo "CHECK adc-twice: ok (the two runs' SSID 32 ebin" \
                    "bytes are identical)"
            else
                echo "CHECK adc-twice: FAIL (the two runs' SSID 32 ebin" \
                    "bytes differ)"
                add_fail adc-twice
            fi
        else
            echo "CHECK adc-twice: FAIL (a run produced no SSID 32 dump)"
            add_fail adc-twice
        fi
    fi

    GOT="${FAILED[*]:-}"
    if [ "$NEGATIVE" -eq 1 ]; then
        if [ "$GOT" = "adc-aligned" ]; then
            echo "ALIGN-SMOKE (adc negative): PASS (failed exactly as" \
                "planned: no upload, the line stays misbinned)" \
                "${STIX_SLOT_TAG}"
            exit 0
        fi
        echo "ALIGN-SMOKE (adc negative): FAIL (failed checks:" \
            "'${GOT}', want 'adc-aligned')${STIX_SLOT_TAG}"
        exit 1
    fi
    if [ -n "$GOT" ]; then
        echo "ALIGN-SMOKE (adc): FAIL (failed checks: $GOT)${STIX_SLOT_TAG}"
        exit 1
    fi
    echo "ALIGN-SMOKE (adc): PASS${STIX_SLOT_TAG}"
    exit 0
fi

# ---- the A3 align legs (one boot) ----------------------------------------
write_align_dssevs
if [ "$NEGATIVE" -eq 0 ]; then
    python3 "$DERIVE" derive --dssevs "$ALIGN_DSSEVS" \
        --out "$DERIVED_CSV" > derive.align.log 2>&1 ||
        { cat derive.align.log;
          echo "ALIGN-SMOKE: ERROR (the derive failed)"; exit 2; }
    encode_args "$DERIVED_CSV" 150
else
    ELUT_ARGS=()
fi

# the boot's probe args; the negative skips the upload groups (the
# pmemsave too: no derived CSV exists to check against)
PMEM_ARGS=()
[ "$NEGATIVE" -eq 1 ] || PMEM_ARGS=(--send-at 310 --qmp-cmd "$(pmemsave leg2)")
ALIGN_ARGS=(
    --port "$TMTC_PORT" --seconds 400 --window 20 --hold
    --qmp "$QMP_SOCK"
    --dump-apid "0x5DC:$RUN_DIR/ql.dump.align.hex"
    --expect "0x5A7:5:3:8030@50"
    --expect "0x5A7:5:1:002200000008000000@50"
    --expect "0x5A7:5:1:14a3@30"
    --forbid "0x5A1:1:2"
    --forbid "0x5A1:1:8"
    --send-at 35 --send-hex "$TC_CONF"
    --expect "0x5A1:1:1:1DACC001@12"
    --expect "0x5A7:5:1:00210204@12"
    --expect "0x5A1:1:7:1DACC001@12"
    --forbid "0x5A7:5:1:00210402"
    --send-at 40 --send-hex "$TC_S20"
    --expect "0x5A1:1:1:1DACC002@8"
    --expect "0x5A1:1:7:1DACC002@8"
    --send-at 45 --qmp-cmd "$(qom_set /machine/stixctrl det-align true)"
    --send-at 50 --qmp-cmd "$(qom_set /machine/stixctrl det-source \
        '"replay-dss"')"
    --send-at 55 --send-hex "$TC_NOM"
    --expect "0x5A1:1:1:1DACC003"
    --expect "0x5A7:5:1:00210405@45"
    --expect "0x5A1:1:7:1DACC003@45"
    --expect "0x5A7:5:2:542c01@45"
    --forbid "0x5A7:5:2:542c03"
    --forbid "0x5A7:5:1:00210502"
    --forbid "0x5A7:5:1:142e"
    --forbid "0x5A7:5:3:9449"
    --send-at 120 --qmp-cmd "$(qom_get /machine/stixctrl \
        det-align-status)"
    --expect-return "$STATUS_FRESH"
    --send-at 130 --send-hex "$TC_CONF2"
    --expect "0x5A1:1:1:1DACC004"
    --expect "0x5A7:5:1:00210504@10"
    --expect "0x5A1:1:7:1DACC004@10"
    --forbid "0x5A7:5:1:00210402"
    --expect "0x5DC:21:6:1e@15"
    --expect "0x5DC:21:6:1f@15"
    --expect "0x5DC:21:6:20@15"
    --expect "0x5DC:21:6:21@15"
    --expect "0x5DC:21:6:22@15"
    ${ELUT_ARGS[@]+"${ELUT_ARGS[@]}"}
    --send-at 240 --send-hex "$TC_NOM2"
    --expect "0x5A1:1:1:1DACC005"
    --expect "0x5A7:5:1:00210405@45"
    --expect "0x5A1:1:7:1DACC005@45"
    --forbid "0x5A7:5:2:542c03"
    --forbid "0x5A7:5:1:00210502"
    --send-at 300 --qmp-cmd "$(qom_get /machine/stixctrl \
        det-align-status)"
    ${PMEM_ARGS[@]+"${PMEM_ARGS[@]}"}
    --send-at 330 --send-hex "$TC_CONF3"
    --expect "0x5A1:1:1:1DACC006"
    --expect "0x5A7:5:1:00210504@10"
    --expect "0x5A1:1:7:1DACC006@10"
    --forbid "0x5A7:5:1:00210402"
    --expect "0x5DC:21:6:1e@15"
    --expect "0x5DC:21:6:1f@15"
    --expect "0x5DC:21:6:20@15"
    --expect "0x5DC:21:6:21@15"
    --expect "0x5DC:21:6:22@15"
    --hold
)

if [ "$DRY_RUN" -eq 1 ]; then
    echo "DRY RUN — the commands align-smoke would run$VERDICT_MODE:"
    [ "$NEGATIVE" -eq 1 ] || echo "# $ELUT_RECORD"
    qemu_args align "$ALIGN_DSSEVS"
    printf '(cd %s && timeout %s %s ' "$RUN_DIR" 430 "$QEMU_BIN"
    printf '%q ' "${QEMU_ARGS[@]}"
    printf '> qemu-stdout.align.log 2>&1)\n'
    printf 'python3 -u %s ' "$PROBE"
    printf '%q ' "${ALIGN_ARGS[@]}"
    printf '\n'
    exit 0
fi

# a fresh private flash: an empty sparse flash.bin of the full size
rm -f flash.bin ql.dump.align.hex
truncate -s 17716740096 flash.bin
qemu_args align "$ALIGN_DSSEVS"
boot align 400 "${ALIGN_ARGS[@]}"
uart_ok align "$([ "$NEGATIVE" -eq 0 ] && echo 1 || echo 0)"
if [ "$NEGATIVE" -eq 0 ]; then
    echo "$ELUT_RECORD"
fi

# split the boot's log at the CONFIGURATION window between the two
# NOMINAL periods (the replay starts once per boot, so not at a replay
# start): the first stixdet_align line after a > 30 s guest gap opens
# leg 2; the QL dump splits by SSID 32 product
awk '/^stixdet_align / { v = $0; sub(/.*vclock_ns=/, "", v); v = v / 1e9
                         if (p != "" && v - p > 30) leg2 = 1; p = v }
     { if (leg2) print > "align.leg2.log"; else print > "align.leg1.log" }' \
    qemu-stdout.align.log
: >> align.leg2.log             # present (maybe empty) for the checks
ssid32_products ql.dump.align.hex align

FAILED=()
add_fail() { FAILED+=("$1"); }

# leg 1: the classification ran while nothing was honoured
if grep -q "replay pre-scan" align.leg1.log &&
        grep -q "ELUT fallback:" align.leg1.log &&
        [ -s align.ssid32.1.hex ]; then
    set +e
    python3 "$QLCHECK" align.ssid32.1.hex --ssid32-subcols 0 \
        > ql.leg1.log 2>&1
    QL1=$?
    set -e
    grep -E '^QL-CHECK' ql.leg1.log || true
    if [ "$QL1" -eq 0 ]; then
        echo "CHECK align-leg1: ok (the fallback trace present, the" \
            "replay counts at subcol 0 ebin 3)"
    else
        echo "CHECK align-leg1: FAIL (ql-check rc=$QL1; see ql.leg1.log)"
        add_fail align-leg1
    fi
else
    echo "CHECK align-leg1: FAIL (no pre-scan, no fallback line or no" \
        "SSID 32 dump in leg 1)"
    add_fail align-leg1
fi

# leg 2: the alignment
LEG2_OK=1
LEG2_WHY=""
RET="$(align_status probe.align.log)"
if ! printf '%s' "$RET" | grep -qF "$STATUS_ALIGNED"; then
    LEG2_OK=0
    LEG2_WHY="the det-align-status return is not the aligned line"
fi
if grep -q "ELUT fallback:" align.leg2.log; then
    LEG2_OK=0
    LEG2_WHY="${LEG2_WHY:+$LEG2_WHY; }a fallback line after the upload"
fi
if ! grep -q "stixdet_align" align.leg2.log; then
    LEG2_OK=0
    LEG2_WHY="${LEG2_WHY:+$LEG2_WHY; }no stixdet_align line in leg 2"
fi
# the second leg-2 chunk: the first also carries the EACC the bank
# accumulated between leg 1's last chunk and the CONFIGURATION pause
CHUNK_N="$(grep -o 'eacc_events=[0-9]*' align.leg2.log | sed -n 2p |
    cut -d= -f2)" || true
if [ -z "$CHUNK_N" ] || [ "$CHUNK_N" -lt 150 ] || [ "$CHUNK_N" -gt 250 ]; then
    LEG2_OK=0
    LEG2_WHY="${LEG2_WHY:+$LEG2_WHY; }the leg-2 chunk EACC total ${CHUNK_N:-is missing}, want 150..250 (1 event per tick)"
fi
if [ "$NEGATIVE" -eq 0 ]; then
    if [ -s align.ssid32.2.hex ]; then
        set +e
        python3 "$QLCHECK" align.ssid32.2.hex --ssid32-subcols 0 \
            > ql.leg2.log 2>&1
        QL2=$?
        set -e
        grep -E '^QL-CHECK' ql.leg2.log || true
        if [ "$QL2" -ne 0 ]; then
            LEG2_OK=0
            LEG2_WHY="${LEG2_WHY:+$LEG2_WHY; }ql-check rc=$QL2 (see ql.leg2.log)"
        fi
    else
        LEG2_OK=0
        LEG2_WHY="${LEG2_WHY:+$LEG2_WHY; }no leg-2 SSID 32 dump"
    fi
    echo "check-dump leg2 (the derived CSV against the uploaded ELUT):"
    set +e
    python3 "$TOOL" check-dump --csv "$DERIVED_CSV" \
        --dump "$RUN_DIR/elut-dump.leg2.bin" | sed 's/^/  /'
    CD_RC="${PIPESTATUS[0]}"
    set -e
    if [ "$CD_RC" -ne 0 ]; then
        LEG2_OK=0
        LEG2_WHY="${LEG2_WHY:+$LEG2_WHY; }check-dump rc=$CD_RC"
    fi
fi
if [ "$LEG2_OK" -eq 1 ]; then
    echo "CHECK align-leg2: ok (fallback=0, honoured=384," \
        "mismatch_cells=0, no fallback trace, check-dump PASS, the" \
        "SSID 32 bin unchanged, the chunk EACC at the aligned bin)"
else
    echo "CHECK align-leg2: FAIL ($LEG2_WHY)"
    add_fail align-leg2
fi

GOT="${FAILED[*]:-}"
if [ "$NEGATIVE" -eq 1 ]; then
    if [ "$GOT" = "align-leg2" ]; then
        echo "ALIGN-SMOKE (negative): PASS (failed exactly as planned:" \
            "no upload, the classification still reports fallback=384)" \
            "${STIX_SLOT_TAG}"
        exit 0
    fi
    echo "ALIGN-SMOKE (negative): FAIL (failed checks: '${GOT}', want" \
        "'align-leg2')${STIX_SLOT_TAG}"
    exit 1
fi
if [ -n "$GOT" ]; then
    echo "ALIGN-SMOKE: FAIL (failed checks: $GOT)${STIX_SLOT_TAG}"
    exit 1
fi
echo "ALIGN-SMOKE: PASS${STIX_SLOT_TAG}"
exit 0
