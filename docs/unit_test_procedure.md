# Unit Test Procedure

Standard operating procedure for running and maintaining unit tests.

---

## 0. Environment Setup (one-time)

### Activate the Python venv

```powershell
& C:\Users\idosh\Documents\zephyr\.venv\Scripts\Activate.ps1
```

Run this in every new terminal session before any `west` command.

### Install Python requirements

```powershell
python -m pip install -r $env:ZEPHYR_BASE\scripts\requirements.txt
```

This installs all twister dependencies (`natsort`, `junitparser`, `pytest`, etc.).

### Set QEMU path (Windows)

**Per-session** (must repeat in every new terminal):

```powershell
$env:QEMU_BIN_PATH = "C:\Users\idosh\zephyr-sdk-1.0.1\hosttools\qemu"
```

**Persistent** (set once, survives across all terminals):

```powershell
[Environment]::SetEnvironmentVariable("QEMU_BIN_PATH", "C:\Users\idosh\zephyr-sdk-1.0.1\hosttools\qemu", "User")
```

> **Note:** The `.env` file lists this variable for reference, but
> `west`/`twister` do **not** load `.env` automatically.

On Linux, use `native_sim` instead of `qemu_cortex_m3` (no QEMU needed).

---

## 1. When to Run Tests

- **Before every commit** — ensures you don't push broken code.
- **Before merging to `main`** — gate for pull-request acceptance.
- **After adding or modifying any function** with test coverage.
- **After changing Kconfig or CMakeLists.txt** — build changes can
  break test builds even if source code is unchanged.

---

## 2. Full Test Run

Run all suites on the host (no hardware required):

```powershell
west twister -T tests/ -p qemu_cortex_m3
```

**Expected output** (all passing):

```
INFO - Total test suites: 4, total test cases: XX
...
INFO - 4 of 4 test configurations passed (100.00%)
INFO - 0 test configurations failed
INFO - 0 test configurations skipped
```

> On Linux, replace `qemu_cortex_m3` with `native_sim` for faster execution.

---

## 3. Single-Suite Run

For faster iteration while developing:

```powershell
# Only the utils suite
west twister -T tests/test_utils -p qemu_cortex_m3

# Only the command parser suite
west twister -T tests/test_command_parse -p qemu_cortex_m3

# Only the config store suite
west twister -T tests/test_config_store -p qemu_cortex_m3

# Only the REST logic suite
west twister -T tests/test_rest_logic -p qemu_cortex_m3
```

---

## 4. Reading Results

### Console output

Twister prints a summary table at the end showing PASS / FAIL / SKIP
for each suite.

### Detailed reports

| Location | Contents |
|----------|----------|
| `twister-out/` | Root output directory |
| `twister-out/<suite>/handler.log` | Per-test pass/fail log |
| `twister-out/<suite>/build.log` | Full CMake + compiler output |
| `twister-out/testplan.json` | Machine-readable results |

### Interpreting status

| Status | Meaning |
|--------|---------|
| **PASS** | All assertions succeeded |
| **FAIL** | At least one `zassert_*` failed — check `handler.log` |
| **SKIP** | Platform not allowed or predicate returned false |
| **ERROR** | Build or runtime crash — check `build.log` |

---

## 5. What to Do on Failure

1. **Re-run with verbose output:**

```powershell
west twister -T tests/test_utils -p qemu_cortex_m3 -vv
```

2. **Check `handler.log`** for the failing assertion line number.

3. **Reproduce locally** by building and running the test binary
   directly:

```powershell
west build -b qemu_cortex_m3 -d build_test tests/test_utils
```

4. **Fix the code or the test**, then re-run.

---

## 6. Adding Coverage for New Code

Checklist when you add a new function:

- [ ] Is the function pure logic (no hardware deps)?
  - **Yes** → add it to the appropriate existing test suite.
  - **No** → extract the pure logic into a testable function first.
- [ ] Did you add at least:
  - One **happy-path** test case?
  - One **error / boundary** test case?
  - One **NULL / empty input** test case (where applicable)?
- [ ] Did you run the full test suite and confirm all tests pass?
- [ ] If you created a new module, did you create a new test suite?
  (See the [Unit Test Guide](zephyr_unit_test_guide.md), section 5.)

---

## 7. CI Integration (Future)

When CI is configured, add this step to the pipeline:

```yaml
- name: Run unit tests
  run: west twister -T tests/ -p native_sim --outdir twister-out
```

Fail the pipeline if twister exits with a non-zero code.

Upload `twister-out/testplan.json` as a build artifact for traceability.
