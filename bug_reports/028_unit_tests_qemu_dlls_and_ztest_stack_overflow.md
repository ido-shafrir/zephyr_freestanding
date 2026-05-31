# Bug Report #028: Unit Tests Unrunnable on Windows (QEMU Missing DLLs + ZTest Stack Overflow)

**Date:** 2026-04-30
**Severity:** Medium (CI / dev workflow blocker; no production firmware impact)
**Status:** Resolved
**Component:** Test suite `prj.conf`, Zephyr SDK host QEMU install
**Board:** N/A (host-side test runner: `qemu_cortex_m3`)
**Zephyr Version:** 4.4.0+ (SDK 1.0.1)

---

## Summary

`west twister -T tests/ -p qemu_cortex_m3` fails end-to-end on a fresh
Windows dev machine for two unrelated reasons:

1. **Host-side:** `qemu-system-arm.exe` exits with `0xc0000135`
   (STATUS_DLL_NOT_FOUND) because MinGW dependencies
   (`libgcc_s_seh-1.dll`, `libwinpthread-1.dll`) are not bundled in
   the SDK. Twister then crashes with `TypeError: 'NoneType' object
   cannot be interpreted as an integer`.

2. **Firmware-side:** Once QEMU works, test suites with large
   stack-allocated structs trigger a kernel panic:
   ```
   ASSERTION FAIL [!k_is_pre_kernel()] @ zephyr/include/zephyr/kernel.h:838
   ```
   This is Zephyr's stack-corruption signature when the ZTest thread
   overflows.

---

## Symptoms

### Issue 1 — QEMU DLL failure

```
QEMU (0) complete with failed (timeout) after 120.00 seconds
ERROR - General exception: 'NoneType' object cannot be interpreted as an integer
  File ".../twister/handlers.py", line 1392, in handle
      os.close(self.pipe_handle)
```

Direct verification:
```powershell
& "C:\Users\<user>\zephyr-sdk-1.0.1\hosttools\qemu\qemu-system-arm.exe" --version
# $LASTEXITCODE = -1073741515 (0xC0000135 = STATUS_DLL_NOT_FOUND)
```

### Issue 2 — ZTest stack overflow

```
START - test_large_struct
ASSERTION FAIL [!k_is_pre_kernel()] ...
E: >>> ZEPHYR FATAL ERROR 4: Kernel panic on CPU 0
```

---

## Root Cause

### Issue 1

Zephyr SDK's Windows QEMU build links against MinGW-w64's
`libgcc_s_seh-1.dll` and `libwinpthread-1.dll` (transitive through
pixman/jpeg). Neither is part of Windows or bundled in the SDK.

When QEMU dies before creating the named pipe, Twister's
`QEMUHandler.handle()` calls `os.close(None)` → `TypeError`.

### Issue 2

The default ZTest stack is 1024 B on `qemu_cortex_m3`. Test cases that
allocate large structs on the stack (~300–500 B) plus the call chain
leaves no headroom → stack overflow → `_kernel.cpus[].current`
corrupted → misleading "k_is_pre_kernel" assertion.

---

## Fix

### Fix 1 — Bundle missing MinGW DLLs

Copy from Git for Windows (which ships MinGW-w64):

```powershell
Copy-Item "C:\Program Files\Git\mingw64\bin\libgcc_s_seh-1.dll" `
          "C:\Users\<user>\zephyr-sdk-1.0.1\hosttools\qemu\" -Force
Copy-Item "C:\Program Files\Git\mingw64\bin\libwinpthread-1.dll" `
          "C:\Users\<user>\zephyr-sdk-1.0.1\hosttools\qemu\" -Force
```

Verify:
```powershell
& ".../qemu-system-arm.exe" --version
# QEMU emulator version 10.0.2
```

### Fix 2 — Bump ZTest stack for affected suites

In the test's `prj.conf`:

```kconfig
CONFIG_ZTEST=y
# Large struct on test thread overflows default 1024 B stack.
CONFIG_ZTEST_STACK_SIZE=2048
CONFIG_MAIN_STACK_SIZE=2048
```

---

## Lessons Learned

1. **On Windows, verify `qemu-system-arm.exe --version` works before
   running twister.** The DLL issue is invisible in the build step.
2. **ZTest stack overflow manifests as "k_is_pre_kernel" assertion** —
   not as a stack-overflow fault. Misleading unless you know the
   pattern.
3. **Large struct on test stack?** Bump `CONFIG_ZTEST_STACK_SIZE`.
   The default 1024 B is conservative.
4. **`qemu-img.exe` working doesn't mean `qemu-system-arm.exe` works**
   — they have different transitive dependencies.

---

## References

- Zephyr SDK host tools: `zephyr-sdk-*/hosttools/qemu/`
- Twister source: `scripts/pylib/twister/handlers.py` (QEMU pipe
  handler).
- Git for Windows MinGW DLLs: `C:\Program Files\Git\mingw64\bin\`.
