# The STIX platform controller

The platform controller is the spacecraft side of the STIX test bench. It
owns the power, the time codes, the OBT and the hardware-state view, and it
serves one REST + SSE API. The plan and its decisions are in
`2026-11-11-platform-plan.md`. This page grows with each commit; PL-7
completes it.

Run it natively:

```
python3 scripts/stix/platform/stixplat.py --config my.toml
python3 scripts/stix/platform/stixplat.py --self-test     # offline, no QEMU
```

It binds 127.0.0.1 only. There is no authentication; see "Security".

## API v1 (provisional until PL-7)

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

## Security

There is no authentication, so the controller is localhost-only:

- a `Host` header other than `localhost`, `127.0.0.1` or `[::1]` is
  refused;
- a POST needs `application/json` and `X-Stix-Platform: 1`;
- there are no CORS headers;
- the QMP socket is in the run dir with mode 0600;
- `[api] bind` must be loopback (`127.0.0.1`, `::1` or `localhost`).
  Any other address is refused unless `[api] container = true`, which is
  only for the container of PL-7. That container is published on
  127.0.0.1 only;
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
- `uart.raw`, `qemu.log`, `qmp.sock` and `log.jsonl`.

The controller refuses the gates' run dirs (`/tmp/stix-*-run`). It also
refuses a filesystem without sparse files or hole punching, and a QEMU
binary older than 21f898484f: that binary would write a plain 16.5 GB
flash.

Copy a flash only with `cp --sparse=always`.

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
