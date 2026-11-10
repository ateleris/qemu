#!/usr/bin/env bash
# Bridge from the v11.1 libqtest harness to the QEMU 5.2 baseline binary:
# translate/strip command-line options the 5.2 binary does not understand.
# libqtest always passes: -qtest ... -qtest-log ... -chardev socket ...
# -object monitor-qmp ... -display none -audio none [-run-with ...] -accel qtest
# and the STIX qtest adds: -global grlib-stixflash.flash-file=<path>
# plus, since P2-1, -global grlib-stixspw.tmtc-server=..., and since
# P2-8 the grlib-stixspw.udp-* properties (all stripped below)
#
# Override the baseline binary location with STIX_OLD_QEMU (default:
# /tmp/qemu-stix-baseline/build/sparc-softmmu/qemu-system-sparc).
set -u

OLD_BIN="${STIX_OLD_QEMU:-/tmp/qemu-stix-baseline/build/sparc-softmmu/qemu-system-sparc}"

raw=("$@")
args=()
i=0
while [ "$i" -lt "${#raw[@]}" ]; do
    a="${raw[$i]}"
    case "$a" in
        -audio|-audiodev|-run-with)
            i=$((i + 2)) # drop the option and its value
            ;;
        -global)
            prop="${raw[$((i + 1))]:-}"
            case "$prop" in
                grlib-stixflash.flash-file|grlib-stixflash.flash-file=*|\
grlib-stixspw.tmtc-server|grlib-stixspw.tmtc-server=*|\
grlib-stixspw.udp-tm-host|grlib-stixspw.udp-tm-host=*|\
grlib-stixspw.udp-tm-port|grlib-stixspw.udp-tm-port=*|\
grlib-stixspw.udp-tc-port|grlib-stixspw.udp-tc-port=*|\
grlib-stixspw.udp-spw-header|grlib-stixspw.udp-spw-header=*)
                    i=$((i + 2)) # 5.2 has no such property; drop the pair
                    ;;
                *)
                    args+=("$a" "$prop")
                    i=$((i + 2))
                    ;;
            esac
            ;;
        -object)
            if [ "${raw[$((i + 1))]:-}" = "monitor-qmp,id=qmp0,chardev=char0" ]; then
                args+=("-mon" "chardev=char0,mode=control")
            fi
            i=$((i + 2))
            ;;
        *)
            args+=("$a")
            i=$((i + 1))
            ;;
    esac
done

exec "$OLD_BIN" "${args[@]}"
