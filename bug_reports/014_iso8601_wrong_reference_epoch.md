# Bug Report #014: Incorrect UNIX Epoch Value Used for ISO 8601 Test Assertions

**Date:** 2026-04-24
**Severity:** Minor (test correctness only — production code was correct)
**Status:** Resolved
**Component:** tests/test_time_service, include/time_service.h, docs
**Board:** qemu_x86 (tests)
**Zephyr Version:** 4.4.0-rc3
**Reporter / Developer:** AI-Agent

---

## Summary

Several ISO 8601 test assertions, docstring examples, and the time
service guide all claimed that UNIX epoch `1718035200` corresponds
to `2024-06-10T20:00:00Z`.  That epoch value actually corresponds to
`2024-06-10T16:00:00Z`.  The correct epoch for `20:00:00Z` on the same
date is `1718049600`.

The bug was caught when the test assertion
`zassert_str_equal(buf, "2024-06-10T20:00:00Z")` failed against the
(correct) output `2024-06-10T16:00:00Z`.

---

## Symptoms

```
Assertion failed at .../src/main.c:38:
  time_service_tests_test_format_known_date:
  buf not equal to "2024-06-10T20:00:00Z"
FAIL - test_format_known_date in 0.002 seconds
```

Two test cases (`test_format_known_date`, `test_parse_known_date`) failed
even though the conversion math was correct.

---

## Root Cause

A `date +%s --date='2024-06-10T20:00:00Z'` conversion was done in the
wrong timezone: `1718035200` is `20:00:00` in CEST (UTC+4), not in UTC.
The error propagated into:

- `tests/test_time_service/src/main.c` (two test bodies + one comment)
- `include/time_service.h` (docstring example)
- `docs/zephyr_time_service_guide.md` (two example snippets)

---

## Fix Applied

Replace every `1718035200 → 2024-06-10T20:00:00Z` claim with the correct
epoch.  The firmware code itself was always right — only the reference
values in tests/docs were wrong.

```diff
- /* 2024-06-10T20:00:00Z = 1718035200 */
- zassert_equal(ts, 1718035200);
+ /* 2024-06-10T20:00:00Z = 1718049600 */
+ zassert_equal(ts, 1718049600);
```

---

## Verification

`python -c "import datetime,calendar;
  print(calendar.timegm(datetime.datetime(2024,6,10,20,0,0).timetuple()))"`
outputs `1718049600`.

`west twister -T tests/test_time_service -p qemu_x86` passes 15/15.

---

## Lessons Learned

Always derive example epoch values from `date -u` (UTC) or
`calendar.timegm(...)` (which treats the tuple as UTC) — never from
naive `datetime.timestamp()` or `date +%s` in a local-time shell, which
silently apply the machine's timezone.
