#!/usr/bin/env bash
# Bridge from the v11.1 libqtest harness to the QEMU 5.2 baseline binary:
# translate/strip command-line options the 5.2 binary does not understand.
# libqtest always passes: -qtest ... -qtest-log ... -chardev socket ...
# -object monitor-qmp ... -display none -audio none [-run-with ...] -accel qtest
# and the STIX qtest adds: -global grlib-stixflash.flash-file=<path>
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
            if [ "${raw[$((i + 1))]:-}" = "grlib-stixflash.flash-file" ] || \
               [[ "${raw[$((i + 1))]:-}" == grlib-stixflash.flash-file=* ]]; then
                i=$((i + 2)) # 5.2 has no such property; drop the pair
            else
                args+=("$a" "${raw[$((i + 1))]:-}")
                i=$((i + 2))
            fi
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
