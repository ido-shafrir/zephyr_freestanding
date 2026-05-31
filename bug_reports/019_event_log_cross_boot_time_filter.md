# Bug Report #019: `event_log <seconds>` Returns Cross-Boot Entries / Hides Pre-Sync Diagnostics

**Date:** 2026-04-24
**Severity:** Minor (observability)
**Status:** Resolved (five iterations — see "Fix History")
**Component:** `src/event_log.c` (walker filter, boot counter),
               `src/command_uart.c` (dump + `event_log_boot` command),
               `src/config_store.c` (`mcu/bootCount` persistent counter)
**Board:** nucleo_h753zi (STM32H753ZI)
**Zephyr Version:** 4.4.0-rc3
**Reporter / Developer:** (redacted)

---

## Summary

The UART command `event_log 60` ("show me the last 60 seconds") was
returning entries from prior boots (and/or hiding important
current-boot pre-sync diagnostics) because the walker filtered by
`entry.timestamp`, which is **per-boot uptime seconds** — not a
monotonic clock across reboots.

Symptoms reported by an operator:

```
---- Sent: "event_log 60" ----
[8s]   INFO NETWORK: DHCP lease: 10.100.110.91        <-- prior boot
[31s]  ERR  OTA: health check timeout ...              <-- prior boot
[606s 2026-04-24T17:23:00Z] DEBUG SYSTEM: sntp sync ok...  <-- two boots ago
... 27 entries total, spanning multiple boots ...
--- 27 event(s) ---
```

The command was supposed to print events from the last minute; it
printed a mix of 5 different boots and ignored both the 60 s window
and the intended "recent diagnostic" semantics.

---

## Root Cause

The old API + walker looked like this:

```c
int event_log_read(uint32_t since_epoch, event_log_walk_cb cb, void *ud);

/* walker: */
if (entry.timestamp < ctx->since_epoch) {
    return 0; /* skip */
}
```

`entry.timestamp` is `k_uptime_get() / 1000`, i.e. seconds since the
current boot. It is:

- **Not monotonic across reboots** — every reset starts again at 0.
- **Independent of wall-clock time**, which does survive reboots
  (from the SNTP sync onwards; see #017 for the lifecycle).

The UART handler computed `since = now_uptime - 60`, then called
`event_log_read(since, ...)`. Two failure modes resulted:

1. **Immediately after reboot**, `now_uptime < 60` so `since = 0`,
   which disables filtering — the walker returns everything, which
   often includes entries from prior boots whose uptime is small.
2. **Long-running devices** returned a grab-bag of entries whose
   per-boot uptimes happened to land in `[since, now]` — a nonsense
   window that mixes "t=10 s of boot N-3" with "t=10 s of boot N-1".

Then we attempted a half-fix that picked `wall_clock` when available
and fell back to `entry.timestamp` with a **single cutoff**. That
*reversed* the problem: once the device was sync'd and the caller
passed a wall-clock cutoff (~1.7e9), every pre-sync entry (with
`wall_clock == 0` and a small uptime) was excluded — exactly the
events operators need to see (boot banner, early DHCP/SNTP
failures):

```
[0s]  INFO BOOT:    system boot fw=0.1.4                  <-- hidden
[5s]  WARN SYSTEM:  sntp sync failed rc=-1 server=...     <-- hidden
[10s] INFO NETWORK: DHCP lease: 10.100.110.91             <-- hidden
[10s 2026-04-24T18:07:03Z] INFO SYSTEM: time synced       <-- shown
```

---

## Impact

- Operators got misleading results from what's supposed to be the
  primary firmware-debug command (`event_log <seconds>`).
- The exact boot-diagnostic events the command is most often used
  to inspect — pre-sync SNTP / DHCP / boot banner — were either
  drowned in prior-boot noise or hidden entirely, depending on
  sync state.
- No correctness impact on stored data; the log itself was fine.

---

## Fix History

This bug was closed out in five iterations; each revealed a case the
previous one couldn't cover. The sections below document all five for
posterity — the final, currently-shipping design is **iteration 5
(asymmetric boot_id + cross-boot pre-sync inclusion via `min_in_window`)**.

### Iteration 1 — Single wall/uptime cutoff (rejected)

First patch picked `wall_clock` when non-zero and fell back to
`entry.timestamp` with a single `since_epoch`. Once the device sync'd
the caller's cutoff was a wall-clock value (~1.7e9), and every
pre-sync entry (wall_clock=0, small uptime) was filtered out:

```
[0s]  INFO BOOT:    system boot fw=0.1.4                  <-- hidden
[5s]  WARN SYSTEM:  sntp sync failed rc=-1 server=...     <-- hidden
[10s] INFO NETWORK: DHCP lease: 10.100.110.91             <-- hidden
[10s 2026-04-24T18:07:03Z] INFO SYSTEM: time synced       <-- shown
```

### Iteration 2 — Two-axis filter (rejected on hardware)

API bumped to `event_log_read(since_wall, since_uptime, cb, ud)`. The
walker picked an axis per-entry based on whether `wall_clock != 0`.
Tests passed in qemu, but on real hardware the bug resurfaced:

```
---- Sent: "event_log 11" ----
[31s] ERR OTA: health check timeout ...                <-- prior boot
[11s] INFO NETWORK: DHCP lease: 10.100.110.91          <-- prior boot
[11s] INFO NETWORK: DHCP lease: 10.100.110.91          <-- prior boot
[11s] INFO NETWORK: DHCP lease: 10.100.110.91          <-- prior boot
[10s] INFO NETWORK: DHCP lease: 10.100.110.91          <-- prior boot
[11s] INFO NETWORK: DHCP lease: 10.100.110.91          <-- prior boot
[11s 2026-04-24T19:20:22Z] INFO SYSTEM: time synced    <-- shown, current
```

Two problems remained:

1. **Prior-boot pre-sync leakage.** The uptime cutoff cannot
   distinguish "current boot, uptime=11" from "previous boot,
   uptime=11". They alias.
2. **Boot banner clipped.** On a fresh boot, `now_up ≈ 12 s` and
   `seconds = 11`, so `since_uptime = 1`. The banner at `timestamp=0`
   failed the strict `<` comparison. A user asking for "last 11 s"
   would never see `t=0`.

No tuning of a pure time cutoff can separate two boots' uptimes —
uptime is not a total order across reboots. The only way is a
**boot identifier**.

### Iteration 3 — `boot_id` + wall clock (regressed sync'd cross-boot)

Each entry now carries a persistent `boot_id` loaded from a new
`mcu/bootCount` setting in config_store, incremented on every
`event_log_init()`. The walker filter is three-axis:

```c
int event_log_read(uint32_t since_wall,
                   uint32_t since_uptime,
                   uint32_t boot_id,
                   event_log_walk_cb cb,
                   void *user_data);

/* walker (simplified): */
if (boot_id != 0 && entry.boot_id != boot_id)      return skip;
if (entry.wall_clock != 0) {
    if (since_wall != 0 && entry.wall_clock < since_wall) return skip;
} else if (boot_id == 0) {
    /* Only consult uptime if the caller did NOT pin a boot. */
    if (since_uptime != 0 && entry.timestamp < since_uptime) return skip;
}
emit;
```

The UART `event_log [seconds]` handler now pins the current
`event_log_get_boot_id()` and relies purely on the wall axis for its
time window. Pre-sync entries of the current boot are always emitted
(the boot_id match alone proves they belong), so the boot banner
(`t=0`) and every early DHCP/SNTP message is always visible.

A new UART command `event_log_boot <id> [seconds]` dumps an arbitrary
historic boot; the current boot's id is printed in every line of
every dump (`[boot N][...]`) so operators can note historic ids.

#### Entry layout change (76 → 80 bytes)

`event_entry_t` gained `uint32_t boot_id`, bumping the record from
76 to 80 bytes. The FCB magic was bumped accordingly from `0x45564C48`
("EVLH") to `0x45564C49` ("EVLI") to force a one-time partition
reformat when upgraded firmware runs for the first time. The existing
stale-magic recovery path (flash_area_erase + retry) in
`event_log_init()` handles this transparently; operators will see
**one** warning after upgrade:

```
<wrn> event_log: fcb_init failed: -22 — wiping event-log partition and retrying
<wrn> event_log: event-log partition reformatted after stale magic
```

After reformat, logging resumes normally.

### Iteration 4 — asymmetric `boot_id` (regressed cross-boot pre-sync)

Iteration 3 was validated on hardware for the common cases — the boot
banner shows, pre-sync noise from prior boots is gone, `event_log_boot
<id>` replays historic boots. But the same hardware session revealed a
new regression: after a reboot, `event_log 300` on boot 2 **omitted**
boot 1's last sync'd entry even though it was well inside the 300-second
wall window:

```
---- boot 1 ----
[boot 1][12s 2026-04-24T20:01:39Z] INFO SYSTEM: time synced via SNTP

---- reboot; wall clock is 2026-04-24T20:03:30Z, ~111 s later ----
---- Sent: "event_log 300" on boot 2 ----
[boot 2][0s]  INFO BOOT: system boot fw=v0.1.4
[boot 2][5s]  INFO NETWORK: DHCP lease: 10.100.110.91
[boot 2][12s 2026-04-24T20:03:30Z] INFO SYSTEM: time synced via SNTP
--- 3 event(s) ---
```

The `2026-04-24T20:01:39Z` line from boot 1 is exactly what the
operator is asking about — it's in the 5-minute wall-clock window,
and its timestamp is authoritative across reboots. The iteration-3
strict `boot_id` filter was too broad: it rejected the entry solely
because `entry.boot_id != current_boot_id`.

Root cause: **sync'd entries and pre-sync entries have different
identity guarantees** and the iteration-3 walker treated them
identically:

- A sync'd entry's `wall_clock` is a cross-boot-comparable UNIX
  timestamp; two boots' sync'd entries can be freely interleaved on
  the wall timeline.
- A pre-sync entry's `timestamp` is uptime-seconds since *this* boot
  — aliasing across reboots, which is why iteration 2 leaked prior-
  boot DHCP/OTA lines. Only `boot_id` can disambiguate these.

Fix: make the filter asymmetric. The walker now applies `boot_id`
only to pre-sync entries in the default mode; sync'd entries are
gated purely by the wall-clock window regardless of which boot wrote
them. A new `bool boot_id_strict` flag restores the iteration-3
behaviour (strict boot_id on every entry) for the `event_log_boot <id>`
full-replay use case.

API bumped to 6 args:

```c
int event_log_read(uint32_t since_wall,
                   uint32_t since_uptime,
                   uint32_t boot_id,
                   bool     boot_id_strict,
                   event_log_walk_cb cb,
                   void *user_data);

/* walker (simplified): */
if (boot_id_strict && boot_id != 0 && entry.boot_id != boot_id)
    return skip;

if (entry.wall_clock != 0) {
    /* Sync'd — wall axis only. boot_id NOT consulted in default
     * mode because the timestamp uniquely places the entry on the
     * wall timeline regardless of boot. */
    if (since_wall != 0 && entry.wall_clock < since_wall) return skip;
} else {
    /* Pre-sync — boot_id + uptime axes. */
    if (boot_id != 0 && entry.boot_id != boot_id) return skip;
    if (since_uptime != 0 && entry.timestamp < since_uptime) return skip;
}
emit;
```

UART callers:

```c
/* event_log [seconds] — default dump. Pre-sync gated to this boot,
 * sync'd entries gated by wall-window only (any boot). */
event_log_read(since_wall, 0, current_boot_id,
               /*boot_id_strict=*/false, cb, NULL);

/* event_log_boot <id> — full replay of one boot, no leak from
 * adjacent boots' sync'd entries. */
event_log_read(since_wall, 0, requested_id,
               /*boot_id_strict=*/true, cb, NULL);
```

#### Result on the failing session

`event_log 300` on boot 2 now returns:

```
[boot 1][12s 2026-04-24T20:01:39Z] INFO SYSTEM: time synced via SNTP
[boot 2][0s]  INFO BOOT: system boot fw=v0.1.4
[boot 2][5s]  INFO NETWORK: DHCP lease: 10.100.110.91
[boot 2][12s 2026-04-24T20:03:30Z] INFO SYSTEM: time synced via SNTP
--- 4 event(s) (this boot=2; wall window 300s) ---
```

The boot-1 SNTP line is back — it's part of the wall timeline the
operator is asking about. Prior-boot pre-sync noise (which
iteration 2 leaked) is still suppressed because its `boot_id`
mismatches the current boot. `event_log_boot 2` still replays boot 2
in full (strict mode) without boot 1's sync'd line leaking in.

### Iteration 5 — cross-boot pre-sync via `min_in_window` (current, shipping)

Iteration 4 brought back prior-boot **sync'd** entries that fall
inside the wall window, but it still suppressed prior-boot
**pre-sync** entries unconditionally. Hardware testing across a
short reboot loop showed the gap:

```
---- Sent: "event_log 600" on boot 5 ----
[boot 3][7s 2026-04-25T04:23:50Z] INFO SYSTEM: time synced via SNTP
[boot 4][5s 2026-04-25T04:24:04Z] INFO SYSTEM: time synced via SNTP
[boot 5][0s] INFO BOOT: system boot fw=0.1.4
[boot 5][5s] WARN SYSTEM: sntp sync failed rc=-1 server=216.239.35.0
[boot 5][5s] INFO NETWORK: DHCP lease: 10.100.110.91
[boot 5][5s 2026-04-25T04:24:54Z] INFO SYSTEM: time synced via SNTP
--- 6 event(s) (this boot=5; wall window 600s) ---
```

Boot 4's banner / DHCP lease / SNTP-failed lines are missing even
though boot 4 itself was *inside* the 600 s window — it booted,
crashed/restarted, and only its `time synced` line survived the
filter. Same for boot 3. The operator wants the full pre-sync
context of every reboot that happened inside the window, not just
the post-sync survivor.

But naively dropping the boot_id gate on pre-sync re-introduces
iteration 2's leak: a boot that started long *before* the window
and is still in the FCB ring would dump its banner/DHCP lines too
(uptime aliases across boots).

**Fix:** anchor pre-sync emission on `min_in_window` — the smallest
`boot_id` whose sync'd entries fell inside the wall window. A
pre-sync entry is emitted iff it belongs to the current boot **or**
its `boot_id > min_in_window` (i.e. a boot strictly younger than the
oldest one whose sync'd entries already qualified). Boots older
than `min_in_window` had their pre-sync context occur *before* the
window and are correctly suppressed.

Implementation: a small two-pass FCB walk inside `event_log_read()`
in default (non-strict) mode. Pass 1 finds `min_in_window` over the
sync'd entries; pass 2 applies the asymmetric filter using the rule
above. Strict mode (`event_log_boot <id>`) is unchanged — it still
replays a single boot's full log without cross-boot bleed.

```c
/* walker (simplified, default mode): */
if (entry.wall_clock != 0) {
    if (since_wall != 0 && entry.wall_clock < since_wall) skip;
} else {
    bool in_scope =
        (boot_id != 0 && entry.boot_id == boot_id) ||      /* current boot  */
        (min_valid && entry.boot_id > min_in_window) ||    /* younger boot  */
        (!min_valid && boot_id == 0 && since_wall == 0);   /* dump-all path */
    if (!in_scope) skip;
    if (since_uptime != 0 && entry.timestamp < since_uptime) skip;
}
emit;
```

API signature is unchanged from iteration 4; only the internal
walker grew the pre-pass and the in-scope rule.

#### Result on the failing session

`event_log 600` on boot 5 now returns:

```
[boot 3][7s 2026-04-25T04:23:50Z] INFO SYSTEM: time synced via SNTP
[boot 4][0s] INFO BOOT: system boot fw=0.1.4
[boot 4][3s] INFO NETWORK: DHCP lease: 10.100.110.91
[boot 4][5s 2026-04-25T04:24:04Z] INFO SYSTEM: time synced via SNTP
[boot 5][0s] INFO BOOT: system boot fw=0.1.4
[boot 5][5s] WARN SYSTEM: sntp sync failed rc=-1 server=216.239.35.0
[boot 5][5s] INFO NETWORK: DHCP lease: 10.100.110.91
[boot 5][5s 2026-04-25T04:24:54Z] INFO SYSTEM: time synced via SNTP
--- 8 event(s) (this boot=5; wall window 600s) ---
```

`min_in_window = 3` (the smallest boot whose sync'd entry passed
the wall window). Boot 3's pre-sync is suppressed (it occurred
before the window). Boot 4's full pre-sync context is emitted
because `4 > 3`. Boot 5's pre-sync is emitted as the current boot.
A hypothetical pre-window boot 2 still in the ring would have its
pre-sync suppressed because `2 < 3`.

---

## Fix (currently shipping — iteration 5)

### Public API

```c
int      event_log_read(uint32_t since_wall,
                        uint32_t since_uptime,
                        uint32_t boot_id,
                        bool     boot_id_strict,
                        event_log_walk_cb cb,
                        void *user_data);

uint32_t event_log_get_boot_id(void);   /* set once by event_log_init */

uint32_t config_store_get_boot_count(void);
int      config_store_set_boot_count(uint32_t count);
```

### UART handler

```c
/* event_log [seconds] — default dump, asymmetric filter. */
uint32_t boot_id = event_log_get_boot_id();
uint32_t since_wall = 0;
if (seconds != 0 && time_service_is_synced()) {
    uint32_t now_wall = (uint32_t)time_service_get();
    since_wall = (seconds > now_wall) ? 0 : now_wall - seconds;
}
event_log_read(since_wall, 0, boot_id, /*strict=*/false,
               uart_event_walk_cb, NULL);

/* event_log_boot <id> [seconds] — strict replay of one boot. */
event_log_read(since_wall, 0, requested_id, /*strict=*/true,
               uart_event_walk_cb, NULL);
```

---

## Files Touched

- `include/event_log.h` — `event_entry_t` gains `boot_id`
  (`76 → 80 bytes`); `event_log_read` signature bumped to 6 args
  (adds `bool boot_id_strict`); new `event_log_get_boot_id()`.
- `src/event_log.c` — magic bumped to `0x45564C49`; `current_boot_id`
  loaded and incremented via config_store in `event_log_init()`;
  walker is two-pass in default mode (pass 1 finds `min_in_window`,
  pass 2 applies asymmetric pre-sync rule based on
  `boot_id > min_in_window` OR current-boot match); strict mode
  filters every entry by exact boot match; write path stamps
  every entry.
- `include/config_store.h`, `src/config_store.c` — new
  `mcu/bootCount` setting; `config_store_get/set_boot_count`.
- `src/command_uart.c` — `cmd_event_log_dump` passes
  `boot_id_strict=false`; `cmd_event_log_boot_dump` passes
  `boot_id_strict=true`; output format includes `[boot %u]`.
- `src/command_parse.c`, `include/command_parse.h` — new
  `CMD_EVENT_LOG_BOOT` command id.
- `tests/test_event_log/src/main.c` — all `event_log_read` call
  sites migrated to 6-arg form; new `test_read_boot_id_filter`,
  `test_entry_carries_boot_id`, and
  `test_nonstrict_sync_bypasses_boot_id` tests;
  `event_log_read_suite` gained a `before` hook so new test writes
  don't contaminate `test_read_empty_log`.
- `tests/test_event_log/src/stubs.c` — stub
  `config_store_get_boot_count` / `_set_boot_count`; toggleable
  `time_service_is_synced` / `_get` driven by new
  `test_time_service_set(synced, epoch)` helper so tests can exercise
  sync'd-entry semantics.
- `docs/zephyr_event_logging_guide.md` — rewrote `event_log_read`
  section with asymmetric four-parameter semantics, updated worked
  example showing cross-boot sync'd inclusion, updated UART command
  table with `event_log_boot`.

Firmware builds clean for `nucleo_h753zi` with sysbuild; 37/37
event_log test cases pass on `qemu_x86`.

---

## Alternatives Considered

1. **Index by write-order only, ignore timestamps.**  Rejected —
   loses the primary UX of "last N seconds".
2. **Persist wall-clock time across reboots via RTC (VBAT).**
   Orthogonal improvement that would make the *single-axis* filter
   work again. Parked pending VBAT backup hardware on the board
   (nucleo SB156 solder bridge + battery) — discussed in the
   time_service guide. Even with an RTC, a boot_id is still useful
   to disambiguate very-short reboots (< RTC resolution).
3. **Reverse-walk + "cling" to pre-sync entries.**  Walk
   newest→oldest; when a sync'd entry passes `since_wall`, keep
   emitting pre-sync entries until the next out-of-window sync'd
   entry. Rejected because:
   - FCB does not expose a reverse walk; we'd need a second pass or
     an in-memory buffer.
   - On a never-sync'd device, the "cling" has no anchor and
     `event_log 60` degenerates to "return every entry".
   - The three-axis design gives predictable output on every path.
     Reverse-cling can be layered on later as a separate mode
     (`event_log 60 trace`) without breaking the default.
4. **"Boot id" only, no wall filter.** Rejected — cannot express
   "last minute" on a long-running device whose current boot is
   weeks old.

---

## Worked Example (asymmetric boot_id + `min_in_window`)

Device has gone through five boots (current `boot_id == 5`); the FCB
ring still holds entries from boots 3 and 4 (boots 1 and 2 have
been overwritten). Operator runs `event_log 600` — UART handler
passes `since_wall = now_wall - 600`, `boot_id = 5`,
`boot_id_strict = false`.

Pass 1 builds `min_in_window = 3` (smallest boot_id whose sync'd
entries passed the wall window).

Log contents (oldest first):

| # | boot_id | `timestamp` | `wall_clock`  | emitted | rule |
|---|---------|-------------|---------------|---------|------|
| 1 | 3       | 0           | 0             | ❌      | pre-sync; boot 3 == min_in_window |
| 2 | 3       | 7           | 1_714_019_030 | ✅      | sync'd, in wall window |
| 3 | 4       | 0           | 0             | ✅      | pre-sync; 4 > min_in_window (3) |
| 4 | 4       | 3           | 0             | ✅      | pre-sync; 4 > min |
| 5 | 4       | 5           | 1_714_019_044 | ✅      | sync'd, in wall window |
| 6 | 5       | 0           | 0             | ✅      | pre-sync, current boot |
| 7 | 5       | 5           | 0             | ✅      | pre-sync, current boot |
| 8 | 5       | 5           | 0             | ✅      | pre-sync, current boot |
| 9 | 5       | 5           | 1_714_019_094 | ✅      | sync'd, in wall window |

Result: 8 events. Boot 3's sync'd anchor is included but its
pre-sync banner is not (it occurred before the 600 s window). Boot 4
contributes its full pre-sync context because it's a younger boot
whose own start fell inside the window. Boot 5 contributes
everything as the current boot.

If the operator instead runs `event_log_boot 4`, only entries 3, 4,
and 5 are emitted (strict).

### Special-case inputs

| `since_wall` | `since_uptime` | `boot_id` | `strict` | Meaning                                                              |
|-------------:|---------------:|----------:|:--------:|----------------------------------------------------------------------|
| 0            | 0              | 0         | false    | Dump everything, every boot.                                         |
| 0            | 0              | X         | true     | Full log of boot X only.                                             |
| W > 0        | 0              | X         | false    | Sync'd entries in wall window + current-boot pre-sync + pre-sync of every boot whose `boot_id > min_in_window`. |
| W > 0        | 0              | X         | true     | Boot X's pre-sync + boot X's sync'd entries in window.               |
| W > 0        | 0              | 0         | false    | Sync'd entries in wall window + pre-sync of every boot with `boot_id > min_in_window`. |

Full guide: [docs/zephyr_event_logging_guide.md](../docs/zephyr_event_logging_guide.md).

---

## Lessons Learned

- A per-boot uptime value cannot be used alone (or with a
  wall-clock fallback) as an identity across reboots. If you need
  to attribute records to the boot session that produced them,
  store an explicit identifier.
- Tests passing in a simulator do not guarantee the fix works on
  hardware. The two-axis design looked correct locally because
  qemu has no FCB persistence across test runs — prior-boot
  leakage is impossible there. Always validate cross-boot
  semantics on real flash (or with a test that pre-populates the
  log with two distinct boot's worth of entries).
- `<` vs `<=` matters at the boundary. A `seconds = 11` request
  one second after the event occurred should still include that
  event; be explicit about whether the cutoff is inclusive and
  test t=0 cases.
- Exposing the identity field in the UI (printing `[boot %u]` in
  every dump line) gives the operator a cheap handle for follow-up
  queries (`event_log_boot N`), which is worth the 10 characters
  of output width.
- Entries with different identity guarantees need different filter
  rules. Sync'd entries (wall_clock != 0) carry a cross-boot
  timestamp and should not be gated by `boot_id` in the common
  "last N seconds" case — doing so (iteration 3) hides exactly the
  cross-boot context the operator is asking about. Pre-sync entries
  (uptime only) must still be gated by `boot_id`. An explicit
  "strict" flag keeps the one-boot replay use case available.
- A boolean current-vs-not gate on pre-sync is too coarse when
  several boots fit inside the window. Anchoring pre-sync on
  `min_in_window` (the smallest boot whose sync'd entries
  qualified) lets *every* boot that started inside the wall
  window contribute its pre-sync banner / DHCP / SNTP-failed
  diagnostics, while still suppressing pre-sync from boots that
  started before the window.

