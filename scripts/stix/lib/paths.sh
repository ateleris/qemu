# SPDX-License-Identifier: GPL-2.0-or-later
#
# Persistent locations for STIX gate state. Sourced by the gate scripts and
# by slot.sh.
#
# A WSL restart clears /tmp. On 2026-11-12 one took out the gate ELF and the
# master flash. The two files that must persist between runs therefore live
# under $HOME. Per-run scratch dirs (STIX_*_RUN_DIR, the slot dirs' private
# flash copies) can stay in /tmp, because every gate recreates them.
#
#   STIX_ELF_DIR    ELFs, with sha256 files and PROVENANCE.md
#                   (default ~/dev/stix-elf)
#   STIX_STATE_DIR  persistent gate state (default ~/dev/stix-state)
#
# Derived:
#   STIX_DEFAULT_ELF  the AX gate ELF the golden was captured from
#                     (sha256 in docs/stix-port/baseline/run-flags.txt)
#   STIX_UPREV_DIR    the master flash.bin plus the legacy (slot 0) run dir
#   STIX_LEGACY_DIRS  the old /tmp locations, still refused as run dirs
#
# Only defaults are set here: a value already in the environment wins, so
# sourcing this file more than once is harmless.

STIX_ELF_DIR="${STIX_ELF_DIR:-$HOME/dev/stix-elf}"
STIX_STATE_DIR="${STIX_STATE_DIR:-$HOME/dev/stix-state}"
STIX_DEFAULT_ELF="${STIX_DEFAULT_ELF:-$STIX_ELF_DIR/ASW_em_baseline.elf}"
STIX_UPREV_DIR="${STIX_UPREV_DIR:-$STIX_STATE_DIR/uprev-run}"
STIX_LEGACY_DIRS="/tmp/stix-uprev-run /tmp/stix-baseline-run"
