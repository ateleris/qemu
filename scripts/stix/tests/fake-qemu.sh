#!/usr/bin/env bash
# Fake QEMU for the gate-script tests: parses -serial file:X and, per
# FAKE_QEMU_MODE, either writes the golden UART log to X (good), writes
# nothing (silent), exits 1 immediately (crash), appends a line every
# 0.5 s until killed or FAKE_QEMU_RUN_SECONDS elapse (heartbeat), or
# appends for FAKE_QEMU_FREEZE_AFTER seconds and then stays alive but
# silent — a hung guest (freeze). For pfm-boot-smoke: pfm writes one
# watchdog RELOAD into the -trace file and stays alive (the positive
# run's live trace), pfm-reset writes a watchdog "count expired" reset
# and exits at once (the latency-off negative). For elut-smoke: elut
# writes the golden boot UART — run 1 with the 4 "problem erasing LUT
# type=3" lines, run 2 without the "could not find LUT (type=3" line
# (the seeded flash) — and, when the argv carries
# grlib-stixctrl.det-replay-file=, the D-7 trace lines on stdout per
# FAKE_QEMU_ELUT_TRACE: good (pre-scan + start + eof; the word/event
# counts derived from the replay file's size), fallback (also one
# stixdet_elut_fallback line), vacuous (pre-scan only: the replay
# never started, the rows were never classified).
#
# FAKE_QEMU_ARGV_FILE, when set, gets one record per invocation appended:
# "argv:" plus the arguments one per line, "cwd: <dir>" and
# "flash-inode: <inode of ./flash.bin, or none>", then "end". A
# -trace ...,file=X argument creates X empty.
set -u
MODE="${FAKE_QEMU_MODE:-good}"
SELF="$(readlink -f "$0")"
if [ -n "${FAKE_QEMU_ARGV_FILE:-}" ]; then
    {
        echo "argv:"
        printf '%s\n' "$@"
        echo "cwd: $(pwd)"
        echo "flash-inode: $(stat -c %i flash.bin 2>/dev/null || echo none)"
        echo "end"
    } >> "$FAKE_QEMU_ARGV_FILE"
fi
SERIAL_OUT=""
TRACE_FILE=""
prev=""
for a in "$@"; do
    if [ "$prev" = "-serial" ]; then
        SERIAL_OUT="${a#file:}"
    fi
    # -trace ...,file=X: create X empty, like QEMU with no event fired
    if [ "$prev" = "-trace" ] && [ "${a#*file=}" != "$a" ]; then
        TRACE_FILE="${a#*file=}"
        : > "$TRACE_FILE"
    fi
    prev="$a"
done
[ -n "$SERIAL_OUT" ] || { echo "fake-qemu: no -serial file:X arg" >&2; exit 3; }

case "$MODE" in
    good)
        cp "$(dirname "$SELF")/../../../docs/stix-port/baseline/uart.golden.log" "$SERIAL_OUT"
        sleep "${FAKE_QEMU_RUN_SECONDS:-30}"
        ;;
    silent)
        sleep "${FAKE_QEMU_RUN_SECONDS:-30}"
        ;;
    heartbeat)
        end=$((SECONDS + ${FAKE_QEMU_RUN_SECONDS:-30}))
        while [ "$SECONDS" -lt "$end" ]; do
            echo "fake-qemu heartbeat $SECONDS" >> "$SERIAL_OUT"
            sleep 0.5
        done
        ;;
    freeze)
        end=$((SECONDS + ${FAKE_QEMU_FREEZE_AFTER:-2}))
        while [ "$SECONDS" -lt "$end" ]; do
            echo "fake-qemu heartbeat $SECONDS" >> "$SERIAL_OUT"
            sleep 0.5
        done
        sleep "${FAKE_QEMU_RUN_SECONDS:-30}"
        ;;
    crash)
        echo "fake-qemu: simulated crash" >&2
        exit 1
        ;;
    pfm)
        # one live watchdog RELOAD (cmd=1), as the FSW feeds it
        echo "stixctrl_watchdog_cmd watchdog cmd=1 reload=536870912" \
            "count=536870912 running=1" >> "$TRACE_FILE"
        sleep "${FAKE_QEMU_RUN_SECONDS:-30}"
        ;;
    pfm-reset)
        # the latency-off negative: the watchdog expires, QEMU exits
        echo "stixctrl_watchdog_reset watchdog resets the IDPU:" \
            "count expired" >> "$TRACE_FILE"
        ;;
    elut)
        GOLDEN="$(dirname "$SELF")/../../../docs/stix-port/baseline/uart.golden.log"
        case "$SERIAL_OUT" in
            *elut1.raw)
                cp "$GOLDEN" "$SERIAL_OUT"
                i=0
                while [ "$i" -lt 4 ]; do
                    echo "00:00:01.0  LUTMgmt       *E problem erasing LUT type=3, copy $i" \
                        >> "$SERIAL_OUT"
                    i=$((i + 1))
                done
                ;;
            *elut2.raw)
                # the seeded flash: the type=3 LUT is found at boot
                grep -v 'could not find LUT (type=3' "$GOLDEN" \
                    > "$SERIAL_OUT"
                REPLAY_FILE=""
                for a in "$@"; do
                    case "$a" in
                        grlib-stixctrl.det-replay-file=*)
                            REPLAY_FILE="${a#*=}" ;;
                    esac
                done
                # the canned counts follow the replay file the gate
                # wrote (one 0xE setup word, the rest events), so a
                # DET7_COUNT change cannot drift the fake
                NWORDS=0
                NEVENTS=0
                if [ -f "$REPLAY_FILE" ]; then
                    NWORDS=$(( $(stat -c %s "$REPLAY_FILE") / 4 ))
                    NEVENTS=$(( NWORDS - 1 ))
                fi
                case "${FAKE_QEMU_ELUT_TRACE:-good}" in
                    good | fallback | vacuous)
                        # the realize pre-scan always traces
                        [ -n "$REPLAY_FILE" ] && echo \
"stixdet_replay_scan replay pre-scan '$REPLAY_FILE': $NWORDS words, $NEVENTS events (fpga_det = dss_det), duration 0 ns, scanned in 0 ns"
                        if [ -n "$REPLAY_FILE" ] &&
                                [ "${FAKE_QEMU_ELUT_TRACE:-good}" != vacuous ]; then
                            echo \
"stixdet_replay_state replay state=start (nominal): t_tick=0 cursor=0 events_fed=0"
                            echo \
"stixdet_replay_state replay state=eof (end of file): t_tick=1 cursor=$NEVENTS events_fed=$NEVENTS"
                        fi
                        if [ "${FAKE_QEMU_ELUT_TRACE:-good}" = fallback ]; then
                            echo \
"stixdet_elut_fallback ELUT fallback: fpga_det 0 pix 0 degenerate row (constant 0), linear binning in use"
                        fi
                        ;;
                    *)
                        echo "fake-qemu: unknown FAKE_QEMU_ELUT_TRACE" \
                            "'${FAKE_QEMU_ELUT_TRACE:-}'" >&2
                        exit 3
                        ;;
                esac
                ;;
            *)
                cp "$GOLDEN" "$SERIAL_OUT"
                ;;
        esac
        sleep "${FAKE_QEMU_RUN_SECONDS:-30}"
        ;;
    l4)
        # The l4-replay wrapper's boots.  The collect boot leaves a
        # small synthetic flash image in ./flash.bin (l4-flash-truth
        # .py's builder: 13 RAW_COUNTS files, 25 archived chunks at
        # SCET 1000001+i, the first model chunk the FSW drops) — the
        # UART rcr / Flush DET stream prints are TRACE calls, compiled
        # out in these builds, so the UART carries only the banner and
        # the one "Skipped first Det data chunk" TRACE_ERROR — and
        # fills the -trace file with FAKE_QEMU_L4_CHUNKS stixdet_chunk
        # lines at SCET 1000001+i (archived[k] = model[k]: the FSW stores
        # each chunk with the previous chunk's end, so the last model
        # chunk is never archived; the truth the canned dumps follow).  The
        # red/green request boots write only the boot banner.
        N="${FAKE_QEMU_L4_CHUNKS:-26}"
        case "$SERIAL_OUT" in
            *uart.collect.raw*)
                {
                    echo "00:00:00.150  Main             init: SAFE mode"
                    echo "00:01:16.0  ArchiveManager   *E Skipped first Det data chunk"
                } > "$SERIAL_OUT"
                if ! python3 - "$(dirname "$SELF")/../l4-flash-truth.py" "$N" <<'EOF'
import importlib.util
import sys

spec = importlib.util.spec_from_file_location("l4ft", sys.argv[1])
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
nf, nc = m.synthetic_collect_flash("flash.bin", nmodel=int(sys.argv[2]))
if (nf, nc) != (13, 25):
    sys.exit("fake-qemu: synthetic flash has %d files / %d chunks, "
             "want 13/25 (FAKE_QEMU_L4_CHUNKS must stay 26)" % (nf, nc))
EOF
                then
                    exit 3
                fi
                ;;
            *)
                echo "00:00:00.150  Main             init: SAFE mode" \
                    > "$SERIAL_OUT"
                ;;
        esac
        if [ -n "$TRACE_FILE" ] && [ -f "$TRACE_FILE" ]; then
            i=0
            while [ "$i" -lt "$N" ]; do
                printf '2026-11-13T10:00:00.000000Z stixdet_chunk chunk: nom_bank 0, 49 blocks, scet 0x%08x:0x0000, int_ticks=10 eacc_events=1800 rotb_count=%d at vclock_ns=%d\n' \
                    "$((1000001 + i))" "$i" $((i * 1000000000)) \
                    >> "$TRACE_FILE"
                i=$((i + 1))
            done
        fi
        sleep "${FAKE_QEMU_RUN_SECONDS:-30}"
        ;;
    *)
        echo "fake-qemu: unknown FAKE_QEMU_MODE '$MODE'" >&2
        exit 3
        ;;
esac
