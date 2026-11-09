#!/usr/bin/env bash
# Bridge from the v11.1 libqtest harness to the QEMU 5.2 baseline binary:
# translate/strip command-line options the 5.2 binary does not understand.
# libqtest always passes: -qtest ... -qtest-log ... -chardev socket ...
# -object monitor-qmp ... -display none -audio none [-run-with ...] -accel qtest
set -u

raw=("$@")
args=()
i=0
while [ $i -lt ${#raw[@]} ]; do
    a="${raw[$i]}"
    case "$a" in
        -audio|-audiodev|-run-with)
            i=$((i + 2)) # drop the option and its value
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

exec /tmp/qemu-stix-baseline/build/sparc-softmmu/qemu-system-sparc "${args[@]}"
