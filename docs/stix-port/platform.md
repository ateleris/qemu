# The STIX platform controller

The platform controller is the spacecraft side of the STIX test bench. It
owns the power, the time codes, the OBT and the hardware-state view, and it
serves one REST + SSE API. The plan and its decisions are in
`2026-11-11-platform-plan.md`; this page is the usage and API reference
(completed in PL-7).

## Usage

### Natively (the default dev path; no image space, D-PLu)

```
python3 scripts/stix/platform/stixplat.py --config my.toml
python3 scripts/stix/platform/stixplat.py --self-test     # offline, no QEMU
```

A config is TOML; every key has a default and an unknown key or wrong
type is an error (exit 2). `stixplat/config.py` holds the full list;
`scripts/stix/platform/interactive.toml` and `run-gates.toml` are
worked samples (native copies usually override `qemu.binary` and
`qemu.elf`).

### In the container (PL-7)

The image (`scripts/stix/platform/Dockerfile`) builds QEMU from the
current tree (two stages, both base images pinned by digest; no
`make install`, no `python3-full`) and carries only
`qemu-system-sparc`, its runtime libraries, `python3` and the
controller (`/opt/stix/platform`, the probe at `/opt/stix/tmtc-probe.py`),
as a non-root user behind `tini`. Not in the image: the ELF, the flash
and run data, the config, the scenarios and the IDB JSON — all mounts.
`scripts/stix/platform/run-container.sh` is the only documented way to
run it:

```
scripts/stix/platform/run-container.sh build                     # once, needs network
scripts/stix/platform/run-container.sh                           # the interactive sample
scripts/stix/platform/run-container.sh --config run-gates.toml   # the gate scenarios
scripts/stix/platform/run-container.sh --dry-run                 # print the docker argv
```

| Mount | From | Contents |
|---|---|---|
| `/var/lib/stix` (rw) | the named volume `stix-run`, or `--volume DIR` | the flash, run dirs, reports |
| `/elf` (ro) | `--elf-dir` (default `$STIX_ELF_DIR`) | the FSW ELF |
| `/config` (ro) | `--conf-dir` (default the platform dir) | the TOML config(s) |
| `/scenarios` (ro) | `--scen-dir` (default the shipped gate scenarios) | scenario JSON |
| `/idb` (ro, optional) | `--idb-dir` | the GS's `stix_idb_tlm.json` |

What it runs (and nothing else — 127.0.0.1 publishing, the named
volume, read-only mounts; D-PLj):

```
docker run --rm --name stix-platform \
  -p 127.0.0.1:8080:8080 -p 127.0.0.1:21502:21502/udp \
  --add-host=host.docker.internal:host-gateway \
  -v stix-run:/var/lib/stix -v "$ELF_DIR":/elf:ro -v "$CONF_DIR":/config:ro \
  -v "$SCEN_DIR":/scenarios:ro [-v "$IDB_DIR":/idb:ro] stix-platform \
  [--config /config/<name>.toml]
```

- The flash volume must support sparse files and hole punching: the
  named volume or a Linux ext4/xfs bind mount. **Never `/mnt/c`**
  (drvfs): the controller's sparse check refuses it, and
  `run-container.sh` refuses it before docker runs. A fresh flash
  costs ~16 KB of real blocks; a NOMINAL run with TM backups a few MB
  (D-PLu).
- The volume is persistent: the samples set `fresh = false`
  (interactive). A fresh start is `fresh = true`, or a new volume
  (`docker volume rm stix-run`).
- **Copy a flash sparse only**: `docker cp` does not keep holes —
  `docker run --rm -v stix-run:/v ubuntu tar --sparse -cf - ...`
  instead. A plain copy materialises 16.5 GB (Troubleshooting).
- On a Linux engine, `--network host` is an alternative to the two
  `-p` lines; then use a loopback config (`[api] bind = "127.0.0.1"`,
  `[gs] tm_dests = ["127.0.0.1:21501"]`, `container = false`), or the
  controller binds 0.0.0.0 on every host interface.
- A build peaks at ≈ 2-3.5 GB (builder layers plus cache) and the
  final image is ≤ 250 MB; `run-container.sh build` prints the prune
  and VHDX-compaction hygiene afterwards and runs none of it without
  asking (D-PLu).

### Ports

| Port | Use |
|---|---|
| 8080/tcp | the API and the web UI (published 127.0.0.1:8080) |
| 21511/udp | QEMU sends TM here (the controller binds it) |
| 21512/udp | QEMU binds TC here, 127.0.0.1 only |
| 21502/udp | the controller takes GS TCs here (published 127.0.0.1:21502) |
| 21501/udp | the GS's TM in-port (the controller's TM destination) |

Natively the controller binds 127.0.0.1 on all of them. The gates use
their own slot bands (2152x/2153x, `tests/plat-slot.sh`) and never
these ports, so a gate can run while an interactive controller is up
(D-PLg).

## Ownership (D-PLa)

The ground station is generic and external: the STIX operator's
console. The controller is the spacecraft plus the test bench.

| Concern | Ground station | Platform controller | QEMU model |
|---|---|---|---|
| STIX operator TCs (ZIX…: modes, parameters, FDIR config, dumps) | **owns** | only as labelled `tc.gs.send` scenario steps | executes (FSW) |
| ELUT upload as an operator activity | **owns** | config hook only, labelled GS emulation | executes |
| TM display, operator decoding, archive | **owns** | decodes only what it checks (events, acks, CUC, HK); its log is a test record, not an archive | produces |
| TM transport to the GS | — | **relays transparently** (bytes unchanged, in arrival order) | sends to the controller |
| GS TC transport to STIX | sends | **proxies transparently**: adds the SpW header, drops TCs while the link is down | receives |
| Power (on, off, cycle, boot), pause | — | **owns** | process, `-S`, `cont`, `stop` |
| SpW link up and down | — | **owns** | `link-down` |
| Time codes, OBT | — | **owns** | generates (PL-1) |
| Time sync TC(9,129), keep-alive TC(20,128) | — | **owns**, source 14 | executes |
| Hardware state (truth and presented) | — | **reads, serves and logs** | exposes read-only (PL-2) |
| Faults and value forcing | — | **owns** | hooks (`adc-inject[N]`, …) |
| Scenarios, expectations, reports | — | **owns** | — |

Consequences: the GS also receives the acks for platform TCs it never
sent (destinations 14 and 121, each its own SSC stream inside APID
0x5A1); the relay can fan out, so an optional COSMOS or Yamcs client
can attach later; a GS TC claiming source 14 is refused and logged as
`tc.refused`.

## API v1 (stable)

The path prefix is `/api/v1`. Read endpoints:

| Endpoint | Returns |
|---|---|
| `GET /status` | power, mode, boot_id, time, latest seq, the QMP round trips |
| `GET /time` | sim (bench) time, wall time, vclock, OBT, running/paused |
| `GET /state`, `GET /state/<path>` | the state tree (D-PLo), or one subtree or leaf |
| `GET /log?since=<seq>&kinds=a,b` | a JSONL slice of the record ring |
| `GET /stream?topics=a,b` | SSE: `id` = seq, `event` = topic, `data` = the record line; resume with `Last-Event-ID` |
| `GET /faults` | the fault catalogue and the active faults |
| `GET /verbs` | the command vocabulary with argument schemas |
| `GET /scenarios` | the scenario dir's files: name, variants, description, or the load error |

`GET /` serves the web UI (below).

Every state change is `POST /api/v1/cmd/<verb>` with a JSON object of
arguments, `Content-Type: application/json` and `X-Stix-Platform: 1`.

### Verbs

This table is normative: the self-test checks that `GET /api/v1/verbs`
lists exactly these verbs and arguments.

<!-- verbs:begin -->
| Verb | Arguments | What it does |
|---|---|---|
| `platform.power.on` | | spawn QEMU frozen (`-S`); no instruction runs |
| `platform.power.boot` | | QMP `cont`; its bench time is T0 |
| `platform.power.off` | | QMP `quit`, then SIGTERM, then SIGKILL; the flash stays |
| `platform.power.cycle` | `off_time_s`, `boot` | off, the off-time (wall clock), on, and boot unless `boot` is false |
| `sim.pause` | | QMP `stop`: the virtual clock, the OBT and the codes freeze |
| `sim.resume` | | QMP `cont` |
| `log.mark` | `text`, `data` | a marker record in the log |
| `platform.spw.link` | `state` | `up` or `down` (QEMU `link-down`); while down, GS TCs are dropped and logged, platform TCs are not sent |
| `platform.timecode.enable` | `enabled` | the periodic time codes on or off |
| `platform.timecode.once` | | one correct code at the next rollover (409 while the link is down or TR = 0) |
| `platform.timecode.inject` | `timecnt` | one code now with this TIMECNT (0..63), a wrong-value tool |
| `platform.obt.set` | `coarse`, `fine` | jump the platform OBT (`obt` mode only) |
| `platform.timesync.send` | `coarse` | TC(9,129) timed to the next code; `"auto"` or an explicit coarse (warned when its 6 LSBs miss); 409 while one is pending |
| `platform.keepalive.set` | `enabled`, `period_s` | the TC(20,128) keep-alive, paced on bench time |
| `force.set` | `path`, `value` | force a presented value: `instrument.hk_adc.<ch>` (adc-inject) |
| `force.clear` | `path` | clear a forcing |
| `fault.inject` | `id`, `params`, `trigger`, `seed` | a catalogue fault (`GET /faults`); logged as `fault` with its params and seed |
| `fault.clear` | `id` | clear an active, reversible fault, or disarm an armed one |
| `tc.platform.send` | `hex` | a raw platform TC space packet: source 14, services 9 and 20 only (403 otherwise); TC(9,129) gets 409 while a sync is pending |
| `tc.gs.send` | `service`, `subtype`, `data`, `ack` | GS emulation: an operator TC from source 121, labelled GS-EMULATION; needs `[gs] emulation` (on in run mode by default) |
| `scenario.run` | `file`, `variant` | run a scenario file from the scenario dir (`[run] scenario_dir`) in this controller, one at a time (409); `variant` is required when the scenario declares variants; verdicts are `expect` records, progress is `status.scenario` |
| `scenario.abort` | | abort the running scenario: open checks and steps not started become `error`, the teardown still runs |
| `debug.qmp` | `command`, `arguments` | one raw `qom-get` or `qom-set` on `/machine/stixspw` or `/machine/stixctrl`, or `query-status`; refused (403) unless `[debug] raw_qmp = true`, and in run mode also `[debug] allow_in_run = true`; every call is a `debug` record and sets the sticky `status.debug_used` taint (dev and gate use only) |
<!-- verbs:end -->

### Versioning and stability (D-PLn)

**API v1 is stable** (declared at PL-7). The path prefix is `/api/v1`,
every record carries `"v":1`, and scenario and catalogue files carry
`"api":1`. From here v1 is additive-only: new verbs, fields, kinds and
topics are fine, and clients must ignore unknown fields. A rename, a
removal or a semantic change goes into `/api/v2`, and v1 stays served
for at least one release. The JSONL log follows the same rule; a reader
keys on `v`. `GET /api/v1/verbs` is the normative list, and the
self-test checks that it matches the table above.

### Records

`log.jsonl` and the SSE stream carry the same records, byte for byte:

```
{"v":1,"seq":N,"sim_time":<ns>,"boot_id":B,"vclock_ns":<ns|null>,
 "wall_time":"...Z","source":"ctl|qemu|fsw|tm|tc|scn|api",
 "kind":"cmd|result|state|power|tm|tc|fault|force|expect|mismatch|uart|log|debug",
 "path":"<state path or verb>","data":{...}}
```

- `sim_time` is the bench time. It never goes back across power cycles,
  and the off-time is timed on the wall clock.
- `boot_id` counts every power-on and every guest reset.
- `"sim_est": true` marks a time the controller estimated between two
  `vclock-ns` samples.
- A `state` record is logged when a leaf changes. Volatile leaves (the
  SCET, the watchdog count, the OBT and the last time code) are left out
  of these change records. A snapshot carries every leaf instead: a
  `state` record with path `""` (the state root) and
  `data = {"snapshot": true, "reason": ..., "leaves": {...}}`. It is
  logged at boot, at pause, just before power-off, and every
  `[clock] snapshot_s` of bench time (default 10 s, 0 = off). So the log
  alone can rebuild every leaf.
- Every `debug.qmp` call is logged as `cmd`, `debug` (the command, its
  arguments and its return or error) and `result` records.
- A refused command (bad arguments) is still logged: `cmd`, then a
  `result` with `ok: false`.
- Topics: `cmd` and `result` map to `cmd`; `state` and `power` map to
  `state`; `fault` and `force` map to `fault`; `debug` maps to `log`.
  Every other kind is its own topic.

## The config

TOML via `tomllib` (stdlib, Python ≥ 3.11), hand-edited. Every key has
a default; an unknown key or a wrong type is an error (exit 2), so a
typo never falls back to a default. The full list with its defaults is
`stixplat/config.py`; the worked container samples are
`interactive.toml` (interactive mode) and `run-gates.toml` (run mode
over the gate scenarios). The keys most often set:

| Key | Default | What it is |
|---|---|---|
| `mode` | `"interactive"` | or `"run"` (scenarios, one bench per variant) |
| `run_dir.path`, `.fresh` | `/tmp/stix-plat/interactive`, false | the run dir; `fresh = true` removes the controller's own files in it |
| `qemu.binary`, `.elf` | `build/qemu-system-sparc`, the paths.sh ELF | the binary (binary-guarded) and the FSW ELF (`-kernel`) |
| `qemu.timecode_source` | `"obt"` | `obt` (D-PLb) or `scet` (the pre-PL-1 behaviour, goldens unchanged) |
| `qemu.obt_epoch`, `.obt_continuity` | `0x10000000 << 16`, `"aligned"` | the OBT start (CUC 48-bit) and the cold-cycle alignment |
| `qemu.on_guest_reset` | `"warm"` | `warm` (in-process, BRAM kept) / `cold` / `halt` |
| `ports` | 8080, 21511, 21512, 21502 | api, qemu_tm, qemu_tc, gs_tc |
| `gs.tm_dests`, `.bind` | `["127.0.0.1:21501"]`, `"127.0.0.1"` | the relay's TM destinations and the TC-proxy bind (loopback, or `container = true`) |
| `gs.tc_during_link_down` | `"drop"` | or `"hold"` (at most `hold_max` = 32, the FSW queues 64) |
| `keepalive.enabled/period_s/autostart` | true, 0.125, `"first_tm"` | the TC(20,128) keep-alive (D-PLe) |
| `timesync.autostart` | `"first_tm"` | the automatic TC(9,129) once per boot |
| `api.bind`, `.container` | `"127.0.0.1"`, false | loopback unless the container of PL-7 |
| `clock.snapshot_s` | 10.0 | the full state record cadence (0 = off) |
| `retention.*` | see `config.py` | run mode's run root and cleanup (D-PLl) |
| `faults.catalogue` | `""` (the v1 catalogue) | a JSON catalogue path |
| `run.scenario_dir`, `.scenarios`, `.variants` | `""` (shipped), [], [] | run mode's inputs |
| `debug.raw_qmp`, `.allow_in_run` | false, false | the `debug.qmp` gate |

## Scenarios and `mode = "run"` (PL-5)

A scenario is a JSON file (`api: 1`) in `scripts/stix/platform/scenarios/`
(or `[run] scenario_dir`): JSON plus whole-line `#` comments (the SPDX
tag), as in the fault catalogue. The runner is `stixplat/scenario.py`.

- **Timing**: `at` counts from T0, the sim time after the step that runs
  `platform.power.boot` (else the end of setup); `after` counts from the
  previous step that runs in this variant. All timeouts and windows are
  sim (bench) time, so a frozen bench does not time out. The boot step's
  own checks arm at the step, before the boot verb, so their windows
  start a little before T0 (the boot's QMP round trip).
- **Checks** arm before the step's verb runs, so a reaction that comes
  back during the verb is not lost: `await_tm` (a probe spec with
  `@timeout` or `{tm, timeout_s}`), `await_event` (`{eid, data,
  timeout_s}`, `data` a hex prefix of the parameters), `await_uart`,
  `await_state` (`{path, eq|ne|lt|gt|within|in_set|changed|stable_for,
  timeout_s}`), `assert_no_tm`, `assert_no_event` (armed to the end or
  for `for_s`), `assert_silence` (`{after_s, for_s}`: no TM unit),
  `expect_cuc` (`{apid, within_s|outside_s, timeout_s, offset_s,
  mod_s}`: the APID's next CUC minus the platform OBT minus `offset_s`,
  taken modulo `mod_s` into [-mod_s/2, mod_s/2) when given).
  `assert_consistent` is an error until PL-8.
- **Every variant needs a check**: a variant whose steps arm none is a
  scenario error, not a vacuous pass. Each negative-heavy gate variant
  also ends on a positive liveness check (a heartbeat), so a dead QEMU
  or TM path cannot pass it.
- **Variants**: `variants` names them; a step with `variants` (globs)
  runs only in those. `config` and `variant_config` may set a small
  whitelist (`scenario.CONFIG_ALLOWED`); anything else is a scenario
  error.
- **Infrastructure**: a power-off that no step's verb caused (QEMU
  exited), a 409 from a step verb while the bench is off, a 5xx, and in
  run mode the wall-clock ceiling (3 x `duration_s` + 120 s, monotonic)
  abort the variant as infra (exit 3). A 409 from a step verb on a
  powered bench is the step's fail. The teardown's `platform.power.off`
  on a bench already off is not an error.
- **Verdicts**: pass, fail, timeout (a check), error. Each check and
  step is logged as an `expect` record on `scenario.<name>.s<i>`, the
  end on `scenario.<name>`, with the seq range as evidence.

Interactive: `scenario.run {file, variant}` (the file must lie in the
scenario dir; 409 while one runs) and `scenario.abort`; `/status` shows
`scenario`.

`mode = "run"` (`stixplat.py --config c.toml --scenario f.json
[--variant glob]`): every scenario is validated first (including its
verb names, against `stixplat/verbs.py`), then each variant
runs in its own controller, QEMU and fresh run dir
`<run_root>/runs/<stamp>-<name>[.<variant>]/`. Reports:
`<run_root>/reports/<stamp>/` with `<name>.json` (the result), the log
slice `<name>.jsonl`, `summary.json` and `summary.txt`. Exit 0 all
pass, 1 a fail, 2 a scenario/config error, 3 infrastructure (the worst
wins). Whatever happens in a variant, its bench is powered off and its
controller closed before the next. SIGTERM aborts the running variant
as infra, runs no other and still writes the reports (a second SIGTERM
stops at once). Retention runs at the end.

The gate scenarios: `gate-spw-link` (nominal, fault_no_recovery,
fault_with_recovery), `gate-keepalive` (the NOMINAL-entry re-arm, R1),
`gate-timesync` (aligned, mismatch), the optional `gate-timecode-loss`,
and `gate-negative`, which must fail with exactly one unmatched await
(`tests/run-smoke.sh` checks both).

## The fault catalogue

The catalogue is `scripts/stix/platform/catalogue/faults-v1.json` (or
`[faults] catalogue`): JSON with `"api": 1`, whole-line `#` comments
allowed. A named failure records the *intent* — distinct from forcing
(`force.set/clear`), which sets a presented value directly and is
logged as `force`. Each entry declares:

- `id` (dotted) and `class` (`component|signal|link|memory|timing`) and
  the `target` (a state tree path);
- `mode` (`stuck_at|offset|drop|disconnect|wrong_value|…`) and `params`
  (types, ranges, units and the index space);
- `persistence` (`transient|until_cleared|permanent|until_write`),
  `reversible`, `trigger`, `seed` (for noisy modes);
- `preconditions` (e.g. a TC(236,10) limit tightened first);
- `expected_detection`: TM matchers with a latency the scenario runner
  can assert;
- `changes`: `truth` (a physical failure) or `presented` (a wrong
  reading) — either way the item shows as `fault` in the state tree
  while active.

`fault.inject {id, params, trigger, seed}` and `fault.clear {id}`;
`GET /faults` lists the catalogue and the active faults. Everything is
validated before anything is marked; an apply that fails at its trigger
leaves nothing armed or active; every inject and clear is a `fault` log
record with the resolved params and seed, so a run can be replayed from
its log.

The v1 entries (all on existing model hooks): `spw.link.disconnect`,
`timecode.missing`, `timecode.wrong_value`, `obt.jump`,
`keepalive.missing`, `power.dpu_1v5.undervoltage`,
`thermal.psu.overtemp` and `hk_prot.det_c.overcurrent`. The
attenuator's fault IDs are reserved in the catalogue's comments.

## Timing

- **QEMU's virtual clock is the single time master** (D-PLr). Every
  controller timer — the keep-alive, the time-sync window, scenario
  `at`/`after` and timeouts, the staleness checks — is a bench-time
  deadline. `sim.pause` freezes the vclock, the OBT, the codes, the
  keep-alive and the scenario timeouts together; a host stall cannot
  fake a lapse. The only wall-clock pacing is the power-off time
  between processes.
- `sim_time` is the continuous **bench time**: Σ earlier processes'
  final vclocks + Σ off-times + the current vclock. It never goes back
  across power cycles; `boot_id` counts every power-on and guest
  reset; `vclock_ns` is the raw virtual clock of the current process
  (null while off). A time the controller estimates between 20 Hz
  samples carries `"sim_est": true`.
- **The keep-alive** TC(20,128) goes out every
  `[keepalive] period_s` (default 125 ms = 8 Hz, the ICD's upper
  bound; range 0.125..3600), paced on bench time with no drift; the
  sequence count follows the OBC's 8 Hz counter (`floor(t·8) mod
  2^14`). The FSW's timeout is a fixed 30 s and not a parameter: EID
  0x142E ("Service 20 not received for 30 s"), one per lapse. Every
  NOMINAL entry re-arms the timer, so in NOMINAL the 0x142E comes 30 s
  after the *later* of the last TC(20,128) and the entry; SAFE entry at
  boot arms it too (the boot-armed 0x142E at about +29 s). The
  keep-alive is suppressed while the link is down or STIX is off.
- **A time sync** TC(9,129) is timed to the next time code (D-PLd): the
  controller reads the OBT, and sends coarse+1 either at once (≥ 0.5 s
  before the rollover) or just after it, so the code applies within the
  FSW's 2 s wait. One sync in flight: a second gets 409 (a queued
  TC(9,129) would apply late). The verification is exact: TM(1,7)
  within 3 s, then the first heartbeat's 48-bit CUC equals the OBT
  within 0.25 s plus the measured relay latency, bit 31 clear; a ±1 s
  window would hide the one-code-late apply the FSW heals after 5
  codes. `[timesync] autostart = "first_tm"` (default) sends one sync
  per boot at the first TM.
- **The OBT** (D-PLb): source `obt` by default (in `scet` mode the
  "spacecraft" follows the instrument and no time-sync test means
  anything). The default epoch 0x10000000 has `coarse & 63` = 0, so
  the boot codes match the FSW's SCET LSBs. Across a cold power cycle
  `obt_continuity = "aligned"` (default) rounds the continuation up to
  the next value whose 6 LSBs match the new SCET (a forward jump of at
  most 63 s, logged); `"exact"` keeps the true continuation, and a
  mismatched code is ignored by the FSW until a TC(9,129) (a shift of
  4 s or more is never corrected).

## Security

There is no authentication, so the controller is localhost-only:

- a `Host` header other than `localhost`, `127.0.0.1` or `[::1]` is
  refused;
- a POST needs `application/json` and `X-Stix-Platform: 1`;
- there are no CORS headers;
- the QMP socket is in the run dir with mode 0600;
- `[api] bind` must be loopback (`127.0.0.1`, `::1` or `localhost`).
  Any other address is refused unless `[api] container = true`, which is
  only for the container of PL-7: it binds 0.0.0.0 inside, and
  `run-container.sh` — the only documented way to run that container —
  publishes 8080 and 21502/udp on 127.0.0.1 only;
- request bodies are strict JSON: `NaN` and `Infinity` are refused, and
  a malformed or negative `Content-Length` gives 400;
- a comma in the run dir, the ELF or the binary path is refused, because
  it would split QEMU's option strings.
- the GS side has no authentication either: `[gs] bind` and every
  `[gs] tm_dests` host must be loopback, under the same
  `[api] container` exception. An IPv6 destination is written
  `[::1]:port`;
- only the controller speaks as the platform: a GS TC claiming source
  14 is refused and logged as `tc.refused`, and `[gs] emulation_source`
  may not be 14.

## The run dir

The run dir holds:

- `flash.bin` (made by QEMU, sparse);
- `uart.raw`, `qemu.log`, `qmp.sock`, `log.jsonl` and, with the relay,
  `tm.jsonl`; run mode adds `runs/<stamp>-<scenario>[.<variant>]/` and
  `reports/` under its run root.

The controller refuses the gates' run dirs (`/tmp/stix-*-run` and the
persistent master `~/dev/stix-state/uprev-run`, i.e.
`scripts/stix/lib/paths.sh`'s `STIX_UPREV_DIR` and
`STIX_LEGACY_DIRS`), also their `-s<k>` slot copies and anything inside
them. It also refuses a filesystem without sparse files or hole
punching (the preflight, before QEMU can create the flash), an existing
`flash.bin` whose real blocks reach 1 GiB, and a QEMU binary older than
21f898484f: that binary would write a plain 16.5 GB flash (and still
reads an inverted one as plain storage, so it is refused with an
existing flash too). Run mode additionally refuses to start when the
free space under its run root is below `retention.min_free_mb` (1024).

Copy a flash only sparse: `cp --sparse=always`, or for the container
volume `docker run --rm -v stix-run:/v ubuntu tar --sparse -cf - ...`.
Never a plain `cp`, `docker cp`, or a tar without `--sparse`: the flash
is 16.5 GB apparent, and a materialised copy fills the host disk
permanently (the WSL VHDXs do not shrink when files are deleted,
D-PLu).

## Troubleshooting

### The ground station hookup

- The GS satellite entry is unchanged (`instance/satellites.json`):
  TM in-port **21501**, TC out **127.0.0.1:21502**. The controller
  relays TM there — `[gs] tm_dests`, default `127.0.0.1:21501`,
  `host.docker.internal:21501` in the container via run-container.sh's
  `--add-host=host.docker.internal:host-gateway` (the GS binds its TM
  port on all interfaces, so the host gateway reaches it on a Linux
  engine) — and takes the GS's TCs on the published 21502/udp: the
  controller **replaces QEMU** for the GS, and the GS config is
  unchanged.
- The station's UI fills the TM view only once you **click it open**;
  the history endpoint (`/api/sat/stix/telemetry/history`) has the
  packets regardless, which is the quick check.
- The GS web app's startup takes ~30 s while liboqs-python tries to
  build **liboqs** and fails; harmless (PKI disabled, commit `0635cad`).
  Installing liboqs properly needs **cmake** and ninja (sudo), once.
- **Never run `run-qemu-gs.sh --udp` or `stix_gs_bridge.py` while the
  controller runs.** Both bind 127.0.0.1:21502 — the port the
  controller takes the GS's TCs on, on purpose — and both would also
  feed TM to 21501, so the GS would see every packet twice. Whichever
  starts second fails; stop the other first.
- While the controller runs, QEMU's QMP is held by it: the raw
  `qom_set` snippets of ground-station.md do not work. Use the API
  (`platform.spw.link`, `platform.timecode.*`, `fault.*`, `force.*`),
  or `debug.qmp` with `[debug] raw_qmp = true` (dev and gate use).

### API refusals

- 403 on a POST: the `X-Stix-Platform: 1` header is missing (415:
  `Content-Type: application/json` is missing; 403 again for a `Host`
  that is not `localhost`/`127.0.0.1`/`[::1]` — use 127.0.0.1, not a
  name: the filter is the CSRF/DNS-rebinding guard).
- 409 on `platform.timesync.send`/`tc.platform.send` of TC(9,129): a
  sync is already in flight. 403 from `tc.platform.send`: only
  services 9 and 20, from source 14, are allowed (operator TCs belong
  to the GS; `tc.gs.send` is the scenario escape hatch).
- "run dir does not support sparse files or hole punching": the run
  dir is on a filesystem that cannot hold the sparse flash (a `/mnt/c`
  or network mount). Move it; in the container use the named volume.
- "is a gate's run dir or inside one": `/tmp/stix-*-run` (with the
  slot copies), the master `~/dev/stix-state/uprev-run` and the legacy
  `/tmp` dirs are refused; the gates use `/tmp/stix-plat/<gate>`.
- "QEMU predates 21f898484f": rebuild or point `qemu.binary` at a
  current binary; the controller refuses to start rather than let QEMU
  write a 16.5 GB flash.
- A comma in the run dir, ELF or binary path is refused (it would
  split QEMU's option strings).

### Docker and disk

- `docker port stix-platform` must show only `127.0.0.1` addresses;
  the API is at http://127.0.0.1:8080/ . The controller's own
  `[api] container = true` config is what allows its 0.0.0.0 bind
  inside; anything else published is a mistake.
- A build peaks at ≈ 2-3.5 GB (D-PLu). Freed disk does not come back
  until the VHDXs are compacted by hand (Windows: `wsl --shutdown`,
  then `Optimize-VHD` or `diskpart compact vdisk`); measure free space
  on C: in Windows, not with WSL's `df` (814 GB there is a lie).
- `du flash.bin` (allocated) is the real cost, `ls -lh` (apparent,
  16.5 GB) is not; under the volume:
  `docker run --rm -v stix-run:/v ubuntu du -h /v/flash.bin`. A fresh
  flash is ~16 KB of blocks.
- The container needs no ELF/config/scenario copies: the mounts are
  read-only, and the IDB mount is optional (`/idb`, for PL-8's
  truth-vs-telemetry decoder).

## The web UI (PL-6)

`http://127.0.0.1:8080/` by default (`[ports] api`, interactive mode).
One file,
`scripts/stix/platform/ui/index.html`: plain HTML, CSS and JS, no build,
no CDN. It is only an API client:

- every control is an element with a `data-verb` that posts
  `/api/v1/cmd/<verb>`, with its `data-args` or the inputs of its row
  (`data-only` limits which); the faults panel is generated from
  `GET /faults`, the forcings from the state tree's `instrument.hk_adc.*`
  leaves, the scenario list from `GET /scenarios`, and an "any verb"
  form and the buttons' tooltips from `GET /verbs` (a verb the server
  does not enable is greyed out);
- everything shown comes from `/status` (polled each second),
  `/state` (every 2 s), and the event log and UART tail: first
  `GET /log` (the last 2000 records), then the SSE stream from that seq.
  So a reload loses nothing, and nothing lives only in the page.

The server sends it behind the same Host filter, with
`Content-Security-Policy: default-src 'none'; script-src <hash>;
style-src <hash>; connect-src 'self'; …; frame-ancestors 'none'`: only
the page's own inline script and style run, and only this origin is
reachable. The page never uses `innerHTML`: TM and UART text is set as
text.

Gates without a browser: the self-test parses the page (every
`data-verb` is served and its posted inputs are args of it, the panels
cover the plan's verbs, no browser storage, no external URL, no inline
handler or style attribute, the script parses with `node --check` when
node exists); core-smoke and link-smoke run `stixplat.py --check-ui
http://127.0.0.1:<port>` against their live server.

### Manual browser checklist (optional)

Against a controller with the real ASW (or the self-test's fake QEMU):

1. Load `/`: the devtools console shows no error and no CSP violation;
   the header chips fill in within a second.
2. on, boot: power `frozen`, then `running`; the UART pane shows the
   banner; TM lines stream into the events pane.
3. SpW down, then up: the SpW chip follows; TM stops and resumes.
4. Time codes off, once, on; TC(9,129) send: the time sync chip goes
   `pending`, then `ok`.
5. A fault (for example `spw.link.disconnect`): inject, the row shows
   `active`, a `fault` line appears; clear: `idle`.
6. A forcing on an `instrument.hk_adc.<ch>` leaf: the state table shows
   presented ≠ truth (highlighted); clear.
7. Reload the page mid-run: the events and UART panes come back with the
   same records (same seqs) and keep streaming.
8. Stop the controller: the page shows "stream: reconnecting…" and
   recovers when it is back.
