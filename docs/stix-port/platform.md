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
| `debug.qmp` | `command`, `arguments` | one raw `qom-get`, `qom-set` or `query-status`; refused (403) unless `[debug] raw_qmp = true` (dev and gate use only) |
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
  of these change records. Instead, every `[clock] snapshot_s` of bench
  time (default 10 s, 0 = off) a `state` record with path `snapshot`
  carries every leaf, so the log alone can rebuild them.
- Every `debug.qmp` call is logged as `cmd` and `result` records, like
  any other verb.
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

## The run dir

The run dir holds:

- `flash.bin` (made by QEMU, sparse);
- `uart.raw`, `qemu.log`, `qmp.sock` and `log.jsonl`.

The controller refuses the gates' run dirs (`/tmp/stix-*-run`). It also
refuses a filesystem without sparse files or hole punching, and a QEMU
binary older than 21f898484f: that binary would write a plain 16.5 GB
flash.

Copy a flash only with `cp --sparse=always`.
