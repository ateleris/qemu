#!/bin/sh
# Start the STIX emulator for the ATS ground station: TM server on
# 127.0.0.1:${STIX_GS_TM_PORT:-21500} (not the default 21000, which the
# qtests and gate scripts use), run dir /tmp/stix-gs-run with its own
# sparse flash.bin (created on first run, a few KB on disk).
#
# Usage: run-qemu-gs.sh [--udp] <qemu-system-sparc>
#   --udp            direct UDP for the ground station (D-B, no bridge):
#                    TM datagrams to udp-tm-host:21501, TC bound on
#                    21502/udp; the TCP server still runs on 21500.
#                    Mutually exclusive with stix_gs_bridge.py: both
#                    bind 127.0.0.1:21502, and both would feed TM to
#                    21501. Stop the bridge first.
#   STIX_ASW_ELF     FSW ELF (default: the normal build ASW_em_baseline.elf)
#   STIX_GS_RUN_DIR  run dir (default /tmp/stix-gs-run)
#   STIX_GS_TM_PORT  TM TCP port (default 21500)
#   STIX_GS_UDP_TM_PORT  TM UDP destination (default 21501)
#   STIX_GS_UDP_TC_PORT  TC UDP bind (default 21502)
#   STIX_GS_QMP      QMP unix socket path for fault injection (default
#                    unset: no QMP). Absolute, no comma, under 108 bytes.
#                    See docs/stix-port/ground-station.md "Fault injection".
# FSW console: tail -f $STIX_GS_RUN_DIR/uart.raw | tr -d '\000'
set -eu
UDP=0
while [ $# -gt 0 ] && [ "$1" = "--udp" ]; do
    [ "$UDP" = 0 ] || { echo "run-qemu-gs.sh: --udp given twice" >&2; exit 2; }
    UDP=1
    shift
done
[ $# -eq 1 ] || {
    echo "usage: run-qemu-gs.sh [--udp] <qemu-system-sparc>" >&2
    exit 2
}
QEMU="$1"
. "$(dirname "${BASH_SOURCE[0]}")/../lib/paths.sh"
ELF="${STIX_ASW_ELF:-$STIX_DEFAULT_ELF}"
RUN="${STIX_GS_RUN_DIR:-/tmp/stix-gs-run}"
PORT="${STIX_GS_TM_PORT:-21500}"
UDP_TM_PORT="${STIX_GS_UDP_TM_PORT:-21501}"
UDP_TC_PORT="${STIX_GS_UDP_TC_PORT:-21502}"
for p in "$UDP_TM_PORT" "$UDP_TC_PORT"; do
    # numeric only: they are interpolated unquoted into UDP_ARGS below
    case "$p" in
        '' | *[!0-9]*)
            echo "run-qemu-gs.sh: STIX_GS_UDP_*_PORT must be numeric" >&2
            exit 2
            ;;
    esac
done
QMP="${STIX_GS_QMP:-}"
case "$QMP" in
    '' | /*) ;;
    *) echo "run-qemu-gs.sh: STIX_GS_QMP must be an absolute path" >&2; exit 2 ;;
esac
case "$QMP" in
    *,*) echo "run-qemu-gs.sh: STIX_GS_QMP must not contain a comma" >&2; exit 2 ;;
esac
# sun_path is 108 bytes including the NUL
[ "${#QMP}" -lt 108 ] || {
    echo "run-qemu-gs.sh: STIX_GS_QMP must be under 108 bytes" >&2; exit 2; }
[ "$RUN" = /tmp/stix-baseline-run ] && { echo "refusing the frozen v11.1 run dir"; exit 2; }
mkdir -p "$RUN" && cd "$RUN"
# Since P2-2 the model's device prints are trace events on stderr, not
# console prints. Enable the TMTC events plus the TM send/receive markers
# that used to be the "Send packet len"/"recv_packet" stdout lines, and
# keep those lines plus QEMU's own error/warning reports so the log stays
# small but still shows connections, packets and realize errors
# (see docs/stix-port/accepted-diffs.md).
UDP_ARGS=""
[ "$UDP" = 0 ] || UDP_ARGS="-global grlib-stixspw.udp-tm-port=$UDP_TM_PORT \
-global grlib-stixspw.udp-tc-port=$UDP_TC_PORT"
"$QEMU" -M leon3_generic -m 64M -no-reboot -display none -monitor none \
    -serial file:"$RUN/uart.raw" -kernel "$ELF" \
    -global grlib-stixspw.tmtc-port="$PORT" \
    $UDP_ARGS \
    ${QMP:+-qmp} ${QMP:+"unix:$QMP,server=on,wait=off"} \
    -trace 'stixtmtc*' -trace stixspw_tx_send \
    -trace stixspw_recv_packet_callback 2>&1 \
  | grep --line-buffered -E "^stix|^qemu-system-sparc:" \
  > "$RUN/qemu.log" &
QPID=$!
# A realize failure (a busy 21502, a bad ELF) exits at once, but behind
# this pipe the terminal would show nothing (P2-8 review M2): a moment
# after the start, check the pipeline and surface the log if it died.
sleep 1
if ! kill -0 "$QPID" 2>/dev/null; then
    echo "run-qemu-gs.sh: QEMU exited during startup; $RUN/qemu.log says:" >&2
    tail -n 20 "$RUN/qemu.log" >&2 || true
    wait "$QPID" || true
    exit 1
fi
wait "$QPID"
