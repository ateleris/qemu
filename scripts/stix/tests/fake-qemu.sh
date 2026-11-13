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
    *)
        echo "fake-qemu: unknown FAKE_QEMU_MODE '$MODE'" >&2
        exit 3
        ;;
esac
