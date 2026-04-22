# Zephyr Unit Test Guide

This document explains how to set up, write, and run unit tests using
Zephyr's **ztest** framework on the **native_sim** target.

---

## 1. Prerequisites

| Tool | Purpose |
|------|---------|
| **Zephyr SDK** | Cross-compiler toolchains (including the native host toolchain) |
| **west** | Zephyr meta-tool — builds, flashes, and runs twister |
| **Python 3.12+** | Required by Zephyr 4.4 and twister |
| **CMake ≥ 3.20** | Build system |
| **QEMU** | Bundled with the Zephyr SDK; runs tests on `qemu_cortex_m3` |

Make sure `west` is installed and the Zephyr workspace is initialised
(`west init` / `west update`).  The `ZEPHYR_BASE` environment variable
must point to the Zephyr tree.

### Python virtual environment

Always activate the venv **before** running any `west` command:

```powershell
& C:\Users\idosh\Documents\zephyr\.venv\Scripts\Activate.ps1
```

### Install Python requirements (one-time)

The venv must have all Zephyr dependencies, including twister's:

```powershell
python -m pip install -r $env:ZEPHYR_BASE\scripts\requirements.txt
```

This installs `natsort`, `junitparser`, `pytest`, and everything else
twister needs.

### QEMU environment variable

On Windows, `native_sim` is not supported (Linux-only). Tests run on
`qemu_cortex_m3` instead. Set `QEMU_BIN_PATH` so twister can find the
QEMU binary.

**Per-session** (must repeat in every new terminal):

```powershell
$env:QEMU_BIN_PATH = "C:\Users\idosh\zephyr-sdk-1.0.1\hosttools\qemu"
```

**Persistent** (set once, survives across all terminals):

```powershell
[Environment]::SetEnvironmentVariable("QEMU_BIN_PATH", "C:\Users\idosh\zephyr-sdk-1.0.1\hosttools\qemu", "User")
```

> **Note:** The project `.env` file lists this variable for reference,
> but neither `west` nor `twister` reads `.env` automatically.

---

## 2. Project Test Layout

```
tests/
├── test_utils/              ← pure utility function tests
│   ├── CMakeLists.txt
│   ├── prj.conf
│   ├── testcase.yaml
│   └── src/
│       └── main.c
├── test_command_parse/      ← UART command parser tests
│   ├── CMakeLists.txt
│   ├── prj.conf
│   ├── testcase.yaml
│   └── src/
│       └── main.c
├── test_config_store/       ← config store integration tests
│   ├── CMakeLists.txt
│   ├── prj.conf
│   ├── testcase.yaml
│   └── src/
│       ├── main.c
│       └── stubs.c          ← stubs for ota_report_module_ready()
└── test_rest_logic/         ← REST request-parsing tests
    ├── CMakeLists.txt
    ├── prj.conf
    ├── testcase.yaml
    └── src/
        └── main.c
```

### What each file does

| File | Role |
|------|------|
| `CMakeLists.txt` | Adds the source file(s) under test and the test `main.c` to the build |
| `prj.conf` | Kconfig — enables `CONFIG_ZTEST=y` plus any subsystem the suite needs |
| `testcase.yaml` | Tells twister the suite name, allowed platforms, and tags |
| `src/main.c` | Contains `ZTEST_SUITE()` and `ZTEST()` definitions |

---

## 3. How to Run Tests

### Run all suites

```powershell
west twister -T tests/ -p qemu_cortex_m3
```

### Run a single suite

```powershell
west twister -T tests/test_utils -p qemu_cortex_m3
```

### Verbose output (useful when debugging failures)

```powershell
west twister -T tests/test_utils -p qemu_cortex_m3 -vv
```

### Where results go

Twister writes results to `twister-out/` in the workspace root.  Each
suite gets a subfolder with the build log, binary, and a
`handler.log` showing pass/fail for every test case.

---

## 4. How to Add a New Test Case (existing suite)

1. Open the suite's `src/main.c`.
2. Add a new `ZTEST()` function in the appropriate suite:

```c
ZTEST(ipv4_validation, test_loopback_address)
{
    zassert_true(is_valid_ipv4("127.0.0.1"));
}
```

3. Rebuild and run:

```bash
west twister -T tests/test_utils -p qemu_cortex_m3
```

That's it — ztest discovers all `ZTEST()` macros automatically at link
time.

---

## 5. How to Add a New Test Suite

1. Create a new folder under `tests/`:

```
tests/test_my_module/
├── CMakeLists.txt
├── prj.conf
├── testcase.yaml
└── src/
    └── main.c
```

2. **`CMakeLists.txt`** — pull in Zephyr and the source files under test:

```cmake
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(test_my_module)

target_sources(app PRIVATE
    src/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/my_module.c
)

target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)
```

3. **`prj.conf`** — enable ztest plus whatever Kconfig the module needs:

```
CONFIG_ZTEST=y
# Add module-specific Kconfig here, e.g.:
# CONFIG_SETTINGS=y
```

4. **`testcase.yaml`**:

```yaml
tests:
  app.my_module:
    platform_allow:
      - native_sim
      - qemu_cortex_m3
    tags: my_module
```

5. **`src/main.c`**:

```c
#include <zephyr/ztest.h>
#include "my_module.h"

ZTEST_SUITE(my_module_tests, NULL, NULL, NULL, NULL, NULL);

ZTEST(my_module_tests, test_something)
{
    zassert_equal(my_function(42), 0);
}
```

---

## 6. Key ztest API Reference

### Suite definition

```c
ZTEST_SUITE(suite_name, predicate, setup, before, after, teardown);
```

| Parameter | Purpose |
|-----------|---------|
| `suite_name` | Identifier for the suite (used in `ZTEST()` calls) |
| `predicate` | Run condition (usually `NULL` = always run) |
| `setup` | One-time setup, returns `void *` fixture (or `NULL`) |
| `before` | Called before **each** test (receives fixture pointer) |
| `after` | Called after **each** test |
| `teardown` | One-time teardown |

### Test definition

```c
ZTEST(suite_name, test_name)
{
    /* test body */
}
```

### Fixture-based tests

```c
ZTEST_F(suite_name, test_name)
{
    /* 'fixture' pointer is available as the implicit first argument */
}
```

### Common assertions

| Macro | Purpose |
|-------|---------|
| `zassert_true(cond)` | Assert condition is true |
| `zassert_false(cond)` | Assert condition is false |
| `zassert_equal(a, b)` | Assert `a == b` |
| `zassert_not_equal(a, b)` | Assert `a != b` |
| `zassert_str_equal(a, b)` | Assert strings are equal |
| `zassert_mem_equal(a, b, n)` | Assert memory regions are equal |
| `zassert_is_null(ptr)` | Assert pointer is NULL |
| `zassert_not_null(ptr)` | Assert pointer is not NULL |

All assertions accept an optional format string + args as trailing
parameters for custom failure messages:

```c
zassert_equal(rc, 0, "Expected 0, got %d", rc);
```

---

## 7. Stubs & Test Isolation

When a module under test calls a function from another module (e.g.
`config_store.c` calls `ota_report_module_ready()`), the test build
would fail to link unless that symbol is provided.

**Solution:** create a `stubs.c` in the test's `src/` folder with a
no-op implementation:

```c
#include "ota.h"

void ota_report_module_ready(enum ota_module mod)
{
    (void)mod;  /* intentionally empty */
}
```

Add it to the test's `CMakeLists.txt`:

```cmake
target_sources(app PRIVATE
    src/main.c
    src/stubs.c
    ...
)
```

> **Rule of thumb:** prefer extracting pure logic into separate files
> (like `command_parse.c`, `rest_logic.c`, `utils.c`) over writing
> stubs.  Stubs are a last resort for unavoidable cross-module
> dependencies.

---

## 8. Troubleshooting

| Symptom | Cause | Fix |
|---------|-------|-----|
| `No SOURCES given to target` | Missing `target_sources()` in CMakeLists.txt | Add the source file(s) under test |
| `undefined reference to …` | Missing dependency in test build | Add a stub or include the source file |
| `CONFIG_ZTEST not set` | Missing `prj.conf` entry | Add `CONFIG_ZTEST=y` |
| Twister hangs or times out | Infinite loop or missing `return` in test | Check test code; add `--timeout 60` to twister |
| `platform not allowed` | `testcase.yaml` restricts platforms | Add `native_sim` to `platform_allow` |
| Tests pass locally but fail in CI | Different Zephyr SDK version | Pin SDK version in CI or use `west.yml` revision |
