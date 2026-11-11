# Gate ports plan: parallel-safe QEMU test runs

Status: plan only, 2026-11-11. Validated and revised: the review findings
are folded in (see "Review resolutions"). Nothing is implemented.
- Track worktree: `~/dev/qemu-stix-gateports`, branch
  `stix-uprev-gateports`, from `d2c6cae28a`.
- The integration branch `stix-uprev-tc` is already at `93438dfa3b`
  (D-2), so **G-0 fast-forwards to it first**.
- Line numbers below are at `93438dfa3b` (after G-0). Where D-2 moved one,
  the old number follows in brackets.
- Accepted-diffs section: **§41**.

Sources:
- a read-only pass over the scripts, `tests/qtest/stix.c`,
  `hw/sparc/grlib_stixspw.c`, `hw/sparc/grlib_stixflash.c`, the baseline
  README and the qt.sh wrapper;
- `stat`/`du` on the existing run dirs, and read-only host queries (`df`,
  `ss`, `netsh`, `netstat.exe`).

A tag of **V** means the fact was read in the code or measured; **I**
means it was inferred and needs a live check.

## Goal

Four tracks (tc, platform, elut, att) can run their gates at the same time
without a global lock:
1. Every QEMU run in slot mode gets its own TCP/UDP ports, run dir and QMP
   socket.
2. No slot-mode run opens the shared `flash.bin`.
3. A counting semaphore caps how many QEMU runs share the CPU at once.
4. A run with no new environment variables (legacy mode) behaves as it
   does today. Its QEMU command line and capture-golden's recorded flags
   stay byte-identical. The only additions are the disk guard (D-G7) and
   the slot-0 lock rule (D-G1).

## Inventory of fixed resources (V)

| Resource | Where | Notes |
|---|---|---|
| TMTC TCP 20000+uid (21000) | model default `tmtc-port=0` → `IO_TCP_SERVER_BASE_PORT + getuid()`: tmtc_io_server.c:339, tmtc_io_server.h:8, grlib_stixspw.c:111/1076 | Every script relies on the default; none passes `tmtc-port` |
| UDP TM/TC | grlib_stixspw.c:1078-1079, `udp-tm-port`/`udp-tc-port` default 0 (off) | Unused by the gates |
| same, computed in scripts | boot-smoke.sh:39, soak.sh:63, tc-smoke.sh:39, nominal-smoke.sh:90 (82), fault-smoke.sh:129, capture-golden.sh:36 | Each script checks with `ss -ltn` and exits 2 if busy (boot:40, soak:64, tc:109, nominal:225 (215), fault:293, capture:37) |
| probe default port | tmtc-probe.py:375 (`20000 + os.getuid()`) | The scripts always pass `--port`. The self-test at :748 only parses "21000"; it binds nothing |
| GS ports 21500/21501/21502 | run-qemu-gs.sh:37-39 (`STIX_GS_*_PORT`) | Interactive, outside the slot range (enforced, D-G1); unchanged |
| qtest: the default port held | stix.c:1669-1679 `test_tmtc_hermetic` binds 20000+uid | **The only cross-run collision in the suite.** While it holds the port, a gate's QEMU fails to bind or its `ss` check exits 2 |
| qtest: fixed UDP 21501 | stix.c:3663 (udp-host-error child) | Harmless: realize fails on the host first, and the TM socket is send-only (no bind) |
| qtest: ephemeral ports | stix.c:126-190 (`stix_hold_*`, `stix_free_tcp_port`), :3189 (`stix_free_udp_port`) | `stix_hold_free_tcp_port` is race-free. `stix_free_*` has a release-then-bind window; G-3 moves it to the slot band |
| qtest: temp flash files | stix.c:97, :352, :529, :613, :1732, :1778 (`g_file_open_tmp`, `g_dir_make_tmp`) | Unique names. Aborted tests leak `/tmp/stix-flash-*` (27 now, 0 KB allocated) |
| qtest: libqtest QMP/qtest sockets | libqtest, named per pid | Parallel-safe |
| 5.2 baseline device | /tmp/qemu-stix-baseline tmtc_io_server.c:153, always 20000+uid, no `tmtc-port` property | Every qtest QEMU under the 5.2 wrapper binds 21000, so a 5.2 suite needs slot 0 |
| Lock | `/tmp/stix-qtest-21000.lock`: qt.sh (both branches); taken by convention around nominal-/fault-smoke (nominal:36-37, fault:82-83) | No script takes it itself |
| Shared run dir | `/tmp/stix-uprev-run`: default of boot-smoke.sh:16, soak.sh:46, tc-smoke.sh:34, capture-golden.sh:29 | QEMU runs with CWD = run dir, using the default `flash-file=flash.bin` |
| Fixed run dirs | `/tmp/stix-nominal-run` (nominal:87 (79)), `/tmp/stix-fault-run` (fault:126), `/tmp/stix-gs-run` (gs:36) | nominal and fault delete and recreate their own flash each run (nominal:234 (224), fault:302) |
| D-3 (in progress, `~/dev/qemu-stix-tc`) | `detector-smoke.sh`: `TMTC_PORT` :113, `ss` :259, `/tmp/stix-detector-run` :109, "caller must hold the 21000 flock" :61 | Untracked there today; converted in G-2b |
| QMP socket | `<run dir>/qmp.sock` (fault:157, :279, :330); `STIX_GS_QMP` (gs:49) | Per run dir already, so it is safe once run dirs are per slot |
| Read-only inputs | `/tmp/stix-baseline-run/ASW_em_baseline.elf` (all scripts); golden files in `docs/stix-port/baseline` (boot-smoke:15) | Shared reads are safe |

Port ranges on this host (V):
- The Linux ephemeral range is 43255-47350 (`ip_local_port_range`, WSL
  mirrored networking).
- The Windows dynamic range is 49152+. The Windows excluded port ranges
  are 5357 and 49684+.
- No Windows TCP listener is in 20000-21999 (`netstat.exe`).

In mirrored mode a Windows-side listener is invisible to `ss`; QEMU's bind
error is the backstop.

## Key findings

- **The shared flash is opened read-write, but SAFE gates do not write it
  today (V).**
  - **The device would show every write at once.** grlib_stixflash.c:573
    opens `flash.bin` with `r+b`, and nothing uses mmap. A page write is
    `fwrite` + `fflush` (:379, :387); an erase is
    `fallocate(PUNCH_HOLE)` (:247) or `fwrite` + `fflush`. All of these
    update mtime and ctime immediately. There is no read-only or snapshot
    property: the only properties are `flash-file` and `inverted-storage`
    (:671-672).
  - **The master was written only at its first boot.**
    `/tmp/stix-uprev-run/flash.bin`:
    - birth 2026-11-10 13:05:53.61, mtime = ctime 13:05:54.78;
    - 32 blocks (16 KB);
    - atime 2026-11-11 14:24, so later gates did open and read it.
    - Every boot-smoke, soak 300 s and tc-smoke run since then left mtime
      unchanged.
    - The 16 KB is first-boot initialisation of an empty flash. The
      README's "about 16 KB after a boot" is that one-time total.
  - **Fresh run dirs show when the FSW does write:**

    | When | Example run dirs | Time after birth |
    |---|---|---|
    | A SAFE boot on an empty flash | p45r-fault, p44-fault | +0.6 s, then nothing |
    | A NOMINAL entry (ContextMgmt) | d2g-nominal, d2-nominal | about +100 s |
    | fault-smoke's link-down (TMBuffer) | d2-fault, d1-fault | about +219 s |

    In every case the allocation stays at 32 blocks (16 KB), because
    erases punch holes.
  - **The consequence.** Concurrent SAFE gates on the master are read-only
    in practice. The private copy (D-G3) protects against future writers:
    a gate that enters NOMINAL or uses link-down, or a changed FSW. It also
    avoids two read-write opens of one file. It does not fix a corruption
    that happens today.
- **The qtest suite is almost hermetic already (V).**
  - Under qtest, `tmtc-server=auto` starts no server
    (grlib_stixspw.c:823-836).
  - Every port-using test takes an ephemeral port.
  - The only fixed resource is `test_tmtc_hermetic`'s hold of 21000.

  The suite therefore needs no port lock, only a CPU slot. The exception
  is a 5.2-wrapper run, which needs slot 0.
- **The probe must be connected before the FSW's first TM (V)**, so
  kernel-allocated ports for the TMTC server are impractical. QEMU would
  have to report the bound port, and early TM would be lost. The scripts
  therefore choose the ports.

## Decisions (D-G)

- **D-G1: slots.** A slot is one lock file, one port band and one run-dir
  suffix. Holding the slot lock owns all three.
  - Slot k uses the band `B_k = 20000 + uid + 100*k` .. `B_k + 99`, and
    the lock `/tmp/stix-qtest-<B_k>.lock`.
  - **Slot 0 is reserved for legacy, manual and 5.2-wrapper use.**
    - Its band is the default port 20000+uid (21000), and its lock is the
      existing `/tmp/stix-qtest-21000.lock`.
    - Legacy runs keep today's convention: the caller holds that lock
      (`flock /tmp/stix-qtest-21000.lock …`).
    - Why the script cannot take it itself: the caller holds it via
      `flock file cmd`, and a second `flock -n` on a new open file
      description would fail against its own parent.
    - `auto` never takes slot 0, so a legacy run that bypasses the
      convention still cannot collide with an auto run.
  - **Auto slots are k = 1..N.** `STIX_SLOTS` = N, default 3 (21100,
    21200, 21300 at uid 1000).
    - The helper refuses (exit 2) any N for which some `[B_k, B_k+99]`
      overlaps the GS ports (`STIX_GS_TM_PORT`, `STIX_GS_UDP_TM_PORT`,
      `STIX_GS_UDP_TC_PORT`, default 21500-21502). At uid 1000 that caps
      N at 4.
    - The range is far below the ephemeral range (43255+).
  - **The CPU cap is N auto runs + 1 legacy run** (slot 0).
  - **Ports within a band:**
    - `B+0` is the TMTC TCP port.
    - `B+1` and `B+2` are reserved for UDP TM and TC, for tracks that need
      them.
    - `B+3..B+99` are for the qtest suite's QEMU-bound ports (D-G4).
    - The GS keeps its own variables.
- **D-G2: `STIX_PORT_BASE`.** It is read by a new sourced helper,
  `scripts/stix/lib/slot.sh`.
  - **Unset: legacy (slot 0).**
    - The port is 20000+uid, and the script takes no lock; the caller
      holds the 21000 lock by convention.
    - The default run dirs are unchanged.
    - **No `-global tmtc-port` is added**, and there is no flash copy and
      no new verdict field.
    - The QEMU command line is byte-identical to today's. The only
      addition is the disk guard (D-G7).
  - **`auto`:** wait for the first free slot in k = 1..N with `flock -n`,
    polled once a second. The wait is bounded by `STIX_SLOT_WAIT`
    (default 1800 s).
    - The lock fd stays open for the script's lifetime, so the kernel
      frees the lock on any exit, including SIGKILL.
    - The fd is inherited by QEMU and the background probe, so the lock
      outlives a killed script until they exit. That is intended: they
      still hold the port.
    - A busy port (the `ss` check) on a held slot means a stray process:
      exit 2 and name the slot.
  - **A number k ≥ 1** (a slot index): use that slot, take its lock with
    `flock -n`, and exit 2 if it is held. This is for tracks that want a
    fixed slot. A request for slot 0 is refused; slot 0 is legacy.
  - In slot mode every script passes
    `-global grlib-stixspw.tmtc-port=$TMTC_PORT` explicitly.
    capture-golden.sh refuses slot mode (D-G3), so its recorded flags never
    change.
  - The helper exports `STIX_SLOT`, `STIX_PORT_BASE` and `TMTC_PORT`, and
    prints one `slot: k base B` line (slot mode only), so logs show which
    slot ran.
- **D-G3: no slot-mode run opens the shared flash.**
  - In slot mode, the default run dir becomes `<default>-s<k>`: for example
    `/tmp/stix-uprev-run-s1`, `/tmp/stix-fault-run-s1`. The slot lock makes
    the dir exclusive.
  - For the boot gates (boot, soak, tc), each slot-mode run refreshes the
    private copy from the master:
    `cp --sparse=always /tmp/stix-uprev-run/flash.bin <dir>/flash.bin`.
    - The copy runs under `flock -s /tmp/stix-uprev-run/.flash.lock` and
      `timeout 30`.
    - `cp` 9.4 uses SEEK_HOLE, and /tmp is ext4 (V).
    - The copy is about 16 KB allocated.
  - **The master must exist:** if it is missing, exit 2. Otherwise QEMU
    would silently create a fresh flash, take the first-boot write path,
    and the golden would diff.
  - **Guards after the copy:**
    - compare `du -k` (allocated) with `ls -l` (apparent);
    - the allocated size must be under 64 MB, and the apparent size must
      equal the master's;
    - otherwise delete the copy and exit 2.

    A non-sparse copy (17.7 GB) would exceed the real free disk (D-G7).
    Every observed flash, writers included, is 16 KB, so the cap has a
    large headroom. It checks the copy before boot only, so a NOMINAL
    entry cannot trip it.
  - **Legacy mode does not copy** (user decision, 2026-11-11). The master
    is not written by today's SAFE gates (Key findings). A copy would need
    a `flash-file=` override or another dir, and that breaks goal 4.
    Legacy boot gates keep using `/tmp/stix-uprev-run/flash.bin` directly,
    under the slot-0 lock convention.
  - Only the flash is copied. The golden files come from the repo, and the
    ELF is read in place.
  - capture-golden.sh, the only intended writer of the master, refuses slot
    mode and takes `flock -x` on `.flash.lock`.
  - nominal, fault and detector already build fresh flash files. Only their
    default dir names change, and only in slot mode.
- **D-G4: the qtest suite needs no fixed port.**
  - **The hermetic test.** `test_tmtc_hermetic` holds a
    `stix_hold_free_tcp_port()` port and passes it as `tmtc-port=<held>`,
    keeping `tmtc-server=auto`.
    - That still proves "auto starts no server under qtest", now without
      touching 21000.
    - The RED on the pre-P2-1 11.1 binary is unchanged: `tmtc-port`
      exists since 8308f50e14, so QEMU dies on the bind of the held port.
    - The test no longer exercises the `tmtc-port=0` default path; §41
      notes it.
  - **On the 5.2 baseline the hermetic test SKIPs explicitly** (user
    decision).
    - Before the hold, the test asks QMP `device-list-properties
      grlib-stixspw` on a probe machine. If `tmtc-port` is absent, it calls
      `g_test_skip("5.2 baseline: no tmtc-port property, the hermetic
      check does not apply")`.
    - It neither fails for the wrong reason (unknown property) nor passes
      vacuously.
    - The wrapper stays as it is: it does not strip `tmtc-port`.
  - **`stix_free_tcp_port` / `stix_free_udp_port` use the slot band.**
    - When `STIX_PORT_BASE` is set (the suite holds a slot), they hand out
      `B+3 .. B+99` round-robin.
    - The slot lock makes those ports exclusive, so the release-then-bind
      race against other suites and probes disappears.
    - When it is unset, they keep the ephemeral `bind(0)` behaviour.
    - `stix_hold_free_tcp_port` (race-free) is unchanged.
  - Leave stix.c:3663 as it is (it binds nothing).
- **D-G5: the CPU cap is the slot count.**
  - The suite, each gate and each probe use about one core: one vCPU TCG
    thread plus the main loop, -m 64M.
  - On 16 cores and 23 GB (V), N=3 auto + 1 legacy is conservative, and
    leaves room for one `make -j8` (I; checked in G-d).
  - **`scripts/stix/qt.sh` (new, committed; user decision).** It is ported
    from the session-scratchpad wrapper
    (`/tmp/claude-1000/-home-laszlo-dev-qemu/c3e3dc05-3735-43c3-8d0b-64157e9da156/scratchpad/glm/qt.sh`).
    - It builds `qemu-system-sparc` and `tests/qtest/stix`, then runs the
      suite or a single test **under a slot lock**. Both branches take a
      slot, so single tests stay inside the CPU cap.
    - The default is `STIX_PORT_BASE=auto`, which exports the base for
      D-G4. It drops the `ss :21000` check.
    - `--baseline` (the 5.2 wrapper as `QTEST_QEMU_BINARY`) takes slot 0
      instead.
  - `STIX_SLOTS` may be raised only after the G-d stress gate passes at
    that count, and only within the GS cap (D-G1).
- **D-G6: flake detection, no auto-retry.**
  - In slot mode, each verdict line gains
    `slot=k busy=<held slots at start> load=<loadavg 1m>`.
  - `busy` is read without acquiring other slots' locks (from
    `/proc/locks`/`lslocks`). A `flock -n` probe would briefly steal free
    slots.
  - Legacy verdict lines are unchanged.
  - The scripts never rerun. A FAIL with busy>0 is first rerun alone, by
    hand. If it passes alone, it counts as a load flake: record it in §41
    and lower the cap. This follows the "pause after failures" rule.
- **D-G7: disk budget (a hard constraint).**
  - **The real limit is the Windows host C: (V).**
    - WSL's `df /` shows 813 GB free (ext4 on a VHDX), but `df /mnt/c`
      shows 19 GB free at 99 % used (2026-11-11).
    - The VHDX (196 GB, on C:) grows when ext4 allocates blocks it has
      never backed, and it **never shrinks**.
    - ext4 uses 143 GB, so about 50 GB of space the VHDX has already
      allocated can be reused first. Every new high-water mark is
      permanent.
  - **The guard:** `stix_disk_guard` in `lib/slot.sh` reads
    `df --output=avail /mnt/c` (KiB).
    - It aborts with exit 2 if the free space is below `STIX_DISK_MIN_GB`
      (default 3).
    - It runs before the flash copy and before QEMU starts, in every mode,
      legacy included. This is the one accepted legacy change, in §41.
    - If `/mnt/c` is absent (not WSL), it falls back to `df /tmp`.
  - **Bounded footprint:**
    - Run dirs are per slot (`-s<k>`) and are reused, never named per run
      (no mktemp dirs).
    - Total gate disk use is therefore at most N slots × (one sparse flash
      of about 16 KB, plus the logs). For example, the d2g-nominal dir is
      440 KB with a 363 KB trace log.
    - The 64 MB allocation guard per copy bounds the worst case at about
      200 MB.
  - **Cleanup:**
    - On PASS, a slot-mode gate deletes the large files from its slot dir
      (`flash.bin` and the `-d`/trace logs) and keeps the small verdict
      logs.
    - On FAIL or ERROR it keeps everything for diagnosis.
    - A one-off cleanup of the existing `/tmp/stix-*` experiment dirs and
      the leaked `/tmp/stix-flash-*` files is listed for the user (G-4).
      It never touches the `/tmp/stix-uprev-run` or
      `/tmp/stix-baseline-run` flash. It frees blocks for reuse, not host
      space.

## Commits

Each commit is test-first. The script tests use the existing fake-QEMU
harness (`scripts/stix/tests/`), which captures QEMU's argv, so they boot
nothing. `--dry-run` exists only in nominal-smoke.sh (:39) and
fault-smoke.sh (:85).

### G-0: fast-forward to `stix-uprev-tc` (93438dfa3b)
- No content; this puts the track on D-2 before any edit. It is verified
  as a fast-forward: one commit.

### G-1: `scripts/stix/lib/slot.sh` and its test
- RED: `scripts/stix/tests/test-slot.sh`. It uses temporary lock dirs
  through `STIX_SLOT_LOCK_DIR`, so the real `/tmp` locks are never
  touched. It covers:
  - unset → base 20000+uid, no lock taken, nothing printed;
  - `auto` with slot 0 held → slot 1 (slot 0 is never taken, even when it
    is free);
  - `auto` with slot 1 held by an outside `flock` → slot 2;
  - all auto slots held → bounded wait, then exit 2;
  - a number with its lock held → exit 2, and the number 0 → exit 2;
  - `STIX_SLOTS` overlapping the GS ports (also with `STIX_GS_*`
    overridden) → exit 2;
  - the lock is released on SIGKILL of a childless holder, and a surviving
    child keeps it;
  - `busy=` counts the held slots without taking any lock (a concurrent
    `flock -n` on a free slot still succeeds);
  - the sparse-copy guard (a fake master): holes copied, `du` vs `ls -l`
    checked, the allocation limit enforced, a non-sparse copy refused, a
    missing master → exit 2;
  - the disk guard, with `STIX_DF_CMD` faked: below 3 GB → exit 2 before
    any copy or QEMU; above → proceed;
  - cleanup on PASS removes flash.bin and the big logs; FAIL keeps them.
- GREEN: the helper only; no script uses it yet. It is a new file, so it
  cannot conflict.

### G-2: the gate scripts use the helper
- RED: extend test-boot-smoke.sh and test-soak.sh, add a fake-QEMU argv
  test for tc-smoke, and add `--dry-run` checks for nominal and fault.
  - **Slot mode:**
    - the QEMU line carries `-global grlib-stixspw.tmtc-port=B`;
    - the probe gets `--port B`;
    - the run dir is `-s<k>`;
    - the flash is a copy: the fake master's mtime, inode and blocks are
      unchanged, and the fake QEMU's CWD `flash.bin` is a different inode.
  - **Legacy mode:** the captured QEMU argv, the probe argv and the verdict
    lines are **identical** to the pre-change output.
- GREEN, per script:
  - one `source` + `stix_slot_setup` hunk that replaces the `TMTC_PORT=`
    line and wraps the `RUN_DIR` default;
  - one slot-mode-only `-global` line in each QEMU invocation;
  - the boot gates also get one `stix_flash_private` call after
    `cd "$RUN_DIR"`;
  - capture-golden gets the slot-mode refusal and the `flock -x`.

  This track does not touch `tmtc-probe.py`.

### G-2b: detector-smoke.sh uses the helper (after D-3 lands)
- detector-smoke.sh is D-3's new gate (in `~/dev/qemu-stix-tc`).
- After D-3 commits and this track merges, apply the same two-line pattern
  and the `-global` line. Add a fake-QEMU argv test (slot dir `-s<k>`,
  `tmtc-port=B`; legacy unchanged).
- Until then, detector-smoke runs only in legacy mode under the slot-0
  lock.

### G-3: qtest off 21000, and suite ports from the slot band
- RED first, deterministic: run `-p /sparc/sparc/stix/tmtc-hermetic` under
  `strace -f -e trace=bind` and assert that no bind to port 21000 appears.
  Today it appears, from the test's hold; that is the RED.
- GREEN:
  - The body of `test_tmtc_hermetic` (stix.c:1665-1695) changes: it gets
    the ephemeral held port and the explicit 5.2 SKIP (D-G4). The test
    registration (:5981 (5278)) is not touched.
  - `stix_free_tcp_port` and `stix_free_udp_port` (:173, :3189) read
    `STIX_PORT_BASE` and use `B+3..B+99` when it is set.
- Checks:
  - (a) the strace check above;
  - (b) the whole suite with 21000 held from the shell: all ok;
  - (c) the whole suite with `STIX_PORT_BASE=21100`: an `strace` bind
    filter shows only 21103-21199 and ephemeral holds;
  - (d) the 5.2 wrapper run under slot 0: `tmtc-hermetic` reports SKIP
    with the reason.

### G-4: scripts/stix/qt.sh and docs
- `scripts/stix/qt.sh`, ported from the scratchpad wrapper (D-G5):
  - build, then the suite or a single test under a slot;
  - `--baseline` takes slot 0;
  - test: a fake-build dry-run in `test-slot.sh`, checking that the slot
    is taken and `STIX_PORT_BASE` exported.
- Docs:
  - baseline README run rules: slots, the slot-0 reservation, the private
    flash in slot mode, the 5.2 wrapper in slot 0, and a fix for the stale
    tc-smoke `--dry-run` note;
  - `accepted-diffs.md` **§41** (text below);
  - the stale "21000" sentences in the nominal and fault headers.

### Proposed §41 text (summary)

- Slots: lock + port band + run dir. Slot 0 (21000) is legacy, manual and
  5.2 only. Auto uses slots 1..N, capped below the GS ports. The cap is N
  auto + 1 legacy.
- Legacy mode is byte-identical on the QEMU line and capture-golden flags.
  The only addition is the disk guard.
- **The master flash is not copied in legacy mode.** Measured: SAFE boots
  on the already-booted master issue no page write or erase (mtime fixed
  since 2026-11-10 13:05:54, 32 blocks). Slot mode uses private sparse
  copies, as insurance against future writers.
- `tmtc-hermetic` holds an ephemeral port: the `tmtc-port=0` default path
  is no longer exercised. On the 5.2 baseline it SKIPs explicitly (no
  `tmtc-port` property).
- Suite QEMU ports come from the slot band B+3..B+99 when a slot is held.
  Without a slot, the old ephemeral release-then-bind window remains.
- Leaked `/tmp/stix-flash-*` files multiply with parallel suites (small,
  out of scope).

## Gates

- **G-a, every commit:** the fake-QEMU script tests, `test-slot.sh`
  (including the disk and GS guards) and the probe self-test.
- **G-b, after G-2/G-3, default behaviour unchanged:** the full Phase 4 set
  in legacy mode, run serially under the 21000 lock:
  - the suite;
  - boot-smoke against the golden;
  - soak 300 s;
  - tc-smoke;
  - nominal-smoke in both modes;
  - fault-smoke.

  All verdicts must match the pre-change run.
- **G-c, slot mode never opens the master:**
  - During one slot-mode boot-smoke, soak and tc-smoke, sample
    `ls -lL /proc/<qemu pid>/fd`. The open `flash.bin` inode must equal
    `stat -c %i <slot dir>/flash.bin` and differ from the master's.
  - Tripwire: `stat -c '%Y %s %b'` of `/tmp/stix-uprev-run/flash.bin` is
    identical before and after G-b and G-d. This is a metadata read only.
    It is a tripwire, not a proof, because today's gates leave it
    unchanged anyway.
- **G-d, concurrency, with `STIX_PORT_BASE=auto`:**
  - (1) two suites at once: both all `ok`;
  - (2) two boot-smokes plus one soak at once: all PASS;
  - (3) three fault-smokes at once: all PASS, three times in a row;
  - (4) a fourth auto run started while three hold slots: it waits and
    runs when a slot frees;
  - (5) one legacy run holding 21000 plus three auto runs: the auto runs
    take slots 1-3, and nothing collides;
  - (6) **the real collision:** suite + boot-smoke --with-tm + tc-smoke at
    once: all ok/PASS;
  - (7) a mix of nominal-smoke + tc-smoke + fault-smoke at once, with a
    concurrent `make -j8`: all PASS.

  Compare the fault-smoke and tc-smoke match latencies and the soak chunks
  per slice with a serial run. A shrink of more than half the window
  margin is a warning in §41. The serial margins (d2-fault) are:
  - boot 0x14a3: about 18 s of 30;
  - TC acks: 0-1 s of 5;
  - ADC events: about 3 s of 10;
  - 0x542b: 60 s of 90;
  - 0x543548: 18 s of 35;
  - tc-smoke: `@3` s.
- **G-f, disk:** record `df /mnt/c` free space before and after G-d, and
  `du -sk /tmp/stix-*-s*` after it. The free space may drop by no more than
  about 0.5 GB, every slot flash must be sparse (`du` ≪ `ls -l`), and no
  slot dir may keep a flash.bin after a PASS.
- **G-e, the wall-clock target:** time the full gate set for two tracks in
  parallel against the serial 25 min. The target is under 30 min for both.

## Merging

- **Implementation starts now in the gateports worktree.**
- `stix-uprev-tc` is checked out in `~/dev/qemu-stix-tc`, with
  uncommitted D-3 work: tmtc-probe.py +122/-7, plus untracked
  detector-smoke.sh and ql-check.py.
- **Merge into `stix-uprev-tc` only after D-3 has committed.** Do the
  merge in that worktree, never over a dirty tree, once G-2 and G-3 pass
  G-a/G-b and before the stress gates, so the other tracks pick it up
  early.
- G-2b (detector-smoke), G-d and G-4 follow as a second small merge.
- This track does not touch tmtc-probe.py. D-3's stix.c additions and the
  hermetic body and port helpers are separate hunks.
- Keep the diffs mechanical. The logic lives in the new `lib/slot.sh`.
  Each script changes only at its `TMTC_PORT=`/`RUN_DIR=` lines and adds
  one slot-mode line per QEMU invocation. In stix.c, only the hermetic
  body and the two `stix_free_*` bodies change; the registration table
  stays untouched.
- Do not reformat, reorder or reflow anything. Each script is one commit
  hunk set, so a conflicting track can take the other side and rerun the
  one-line edits.
- After the merge, the other tracks merge `stix-uprev-tc` (never rebase
  shared history). Tracks that add new gate scripts copy the two-line
  pattern; §41 states it.

## Risks and checks

- **The residual port window (I).**
  - With a slot held, suite ports come from the slot band, so the window
    is gone.
  - A suite run without a slot keeps the old ephemeral window. The probes'
    outbound connects draw from the same 4096-port ephemeral range.
  - A "Bind failed" realize in one tmtc test is tracked under D-G6.
  - A retry inside the helper cannot work: the bind fails in the QEMU
    child, and `qtest_init` aborts.
- **The private flash vs the golden (I).** A slot copy is byte-identical to
  the master, so boot-smoke should not diff. G-b/G-c/G-d(2) confirm it.
  If it diffs anyway, the master needs one final settled boot by
  capture-golden, under the exclusive lock.
- **A sparse copy on another filesystem (I).** The guard refuses an
  allocation above 64 MB, and the copy runs under `timeout 30`.
- **Host disk exhaustion (V: 19 GB free on C:, and the VHDX never
  shrinks).**
  - Possible causes: a runaway `-d`/trace log, a non-sparse flash, or a
    FSW that fills its flash (observed writers stay at 16 KB).
  - Mitigations: the D-G7 floor before every run, the per-copy allocation
    cap, and per-slot reuse.
  - A trace log grows without a cap, so trace-heavy gates (nominal's and
    detector's `-trace stixdet_idefx_cmd`) should be bounded by the run
    duration; check their size in G-f.
- **WSL2 clock steps** already affect wall-clock windows (the soak uses
  /proc/uptime). Concurrency adds no new clock source, but load can delay
  probe sends. D-G6 records `busy`/`load` with each verdict.
- **Windows-side listeners** (mirrored networking) are invisible to `ss`.
  None is in 20000-21999 today (V). QEMU's bind error names the port.
- **Leaked `/tmp/stix-flash-*` qtest files** (already pending cleanup)
  multiply with parallel suites. They are small, so this is out of scope;
  note it in §41.
- **A slot lock held by a hung script or a surviving child** blocks that
  slot until it is killed. The wait is bounded, and the error names the
  slot and its lock file.

## Out of scope

- GS ports (`run-qemu-gs.sh`): interactive; already configurable and
  outside the slot range.
- Kernel-allocated TMTC ports with QEMU reporting the bound port (a model
  change; it loses early TM).
- `-icount`/timing determinism under load.

## Review resolutions

Independent validation, 2026-11-11. All 10 required items are accepted,
together with the slot-band recommendation.

| R# | Resolution |
|---|---|
| R1 | Key findings rewritten: the master is opened `r+b` but is not written by SAFE gates (stat evidence). D-G3 is kept as insurance against future writers |
| R2 | G-c now proves the slot copy is the open fd (`/proc/<pid>/fd` inode); the master stat is a tripwire only |
| R3 | User decision: legacy mode does not copy; the legacy QEMU line and capture-golden flags stay byte-identical (D-G2, D-G3, §41) |
| R4 | `-global tmtc-port` is added in slot mode only; legacy argv is identical. tc, boot and soak are tested via fake-QEMU argv, nominal and fault via `--dry-run` (G-2) |
| R5 | User decision: slot 0 is reserved for legacy, manual and 5.2 use; auto uses slots 1..N; the cap is N auto + 1 legacy (D-G1) |
| R6 | User decision: the 5.2 wrapper runs in slot 0; `tmtc-hermetic` SKIPs explicitly when `tmtc-port` is absent (D-G4, G-3 check (d)) |
| R7 | The G-3 RED is deterministic, via `strace -f -e trace=bind`; the 21000-held suite run is a separate check |
| R8 | G-d adds (6) suite + boot-smoke --with-tm + tc-smoke, and (7) nominal + tc + fault with `make -j8` |
| R9 | User decision: implement now; merge only after D-3 commits; detector-smoke conversion is G-2b |
| R10 | The helper refuses any N whose band overlaps the GS ports (N ≤ 4 at uid 1000 with 100-port bands); test-slot.sh case added |
| Rec. | Suite QEMU ports come from the slot band B+3..B+99 instead of a retry (D-G4, G-3) |
| Rec. | qt.sh is committed as `scripts/stix/qt.sh` under a slot lock; `--baseline` uses slot 0 (D-G5, G-4) |
| Rec. | Facts corrected: ephemeral range 43255-47350, Windows port facts, VHDX reuse, 27 leaked files at 0 KB; `busy=` is read without taking locks; the master-missing guard; the lock fd is inherited by children |
