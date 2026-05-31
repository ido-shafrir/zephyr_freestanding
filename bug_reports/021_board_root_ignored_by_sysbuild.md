# Bug Report #021: `BOARD_ROOT` Set in Application `CMakeLists.txt` Is Ignored by Sysbuild

**Date:** 2026-05-08
**Severity:** Minor (workflow / build ergonomics)
**Status:** Worked-around; documented
**Component:** `CMakeLists.txt` (`list(APPEND BOARD_ROOT ...)` directive),
               sysbuild integration
**Board:** Any out-of-tree board built with `--sysbuild`
**Zephyr Version:** 4.4.0+

---

## Summary

To make a new out-of-tree board discoverable without a per-clone
`west config build.board-root ...`, the application's top-level
`CMakeLists.txt` adds:

```cmake
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_LIST_DIR})
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
```

This works for **non-sysbuild** builds. With `--sysbuild`, however,
the build fails with:

```
No board named '<board>' found. Did you mean: ...
CMake Error at .../zephyr/cmake/modules/boards.cmake:221 (message):
  Invalid BOARD; see above.
```

The **sysbuild template** runs board resolution **before** the
application's own `CMakeLists.txt` is parsed. The `list(APPEND
BOARD_ROOT ...)` line is effective only inside the per-image configure
step — too late for the top-level board lookup.

---

## Root Cause

Sysbuild is its own CMake project rooted at
`zephyr/share/sysbuild/CMakeLists.txt`. Its first action is board
resolution against `BOARD_ROOT` as it is at that point — populated
only from:

- the environment variable `BOARD_ROOT`,
- `-DBOARD_ROOT=...` on the CMake command line, or
- the `BOARD_ROOT` west-config setting / `.west/config`.

Application source `CMakeLists.txt` files are *child* projects under
sysbuild and are not consulted for board lookup.

---

## Workaround

Pass `-DBOARD_ROOT=...` on the command line:

```powershell
west build -b <board> -p always --sysbuild `
  -- "-DBOARD_ROOT=<absolute-path-to-project-root>"
```

Keep the `list(APPEND BOARD_ROOT ...)` in `CMakeLists.txt` because:

1. It still helps the non-sysbuild path (e.g. unit-test builds via
   twister that build the app directly).
2. Removing it means *every* invocation needs the explicit `-D`.

---

## Lessons Learned

- **Sysbuild does not inherit CMake state from the application
  project.** Anything board-related must be visible **before** sysbuild
  runs: env var, `-D...`, or west config.
- A "cleaner" alternative — the Zephyr "module" out-of-tree layout
  (with a `zephyr/module.yml` that declares `boards: <path>`) — would
  let west auto-include the boards via the manifest. More invasive.

---

## Possible Permanent Fixes

1. **`setup-env.ps1` / `setup-env.sh`** that exports `BOARD_ROOT`.
2. **`west config build.cmake-args -- -DBOARD_ROOT=...`** once per
   checkout.
3. **Promote the project into a Zephyr module** by adding
   `zephyr/module.yml` with a `build.boards: boards` setting.

---

## References

- `zephyr/share/sysbuild/CMakeLists.txt` — sysbuild entry point.
- `zephyr/cmake/modules/boards.cmake` — board lookup failure message.
- Bug #005 — sysbuild OTA build failures (same class: app-scope vs
  sysbuild-scope confusion).
