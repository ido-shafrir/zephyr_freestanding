# Bug Report #005: Sysbuild OTA Build Failures — DTS Label Conflict & Missing imgtool Dependencies

**Date:** 2026-04-13  
**Severity:** Medium  
**Status:** Resolved  
**Component:** Build system (DTS overlay, Python environment)  
**Board:** nucleo_h753zi (STM32H753ZI)  
**Zephyr Version:** 4.4.0-rc3  

---

## Summary

First attempt to build with MCUboot via `west build -b nucleo_h753zi --sysbuild --pristine`
failed with two independent errors:

1. **DTS label conflict** — duplicate `slot1_partition` label between the
   base board DTS and our overlay.
2. **Missing Python `click` module** — MCUboot's `imgtool.py` key extraction
   step failed because `click` was not installed in the venv.

---

## Issue 1: DTS Label Conflict

### Symptoms

```
devicetree error: Label 'slot1_partition' appears on
/soc/flash-controller@52002000/flash@8000000/partitions/partition@100000 and
on /soc/flash-controller@52002000/flash@8000000/partitions/partition@60000
```

Build aborted during CMake configure for the ICB-FW application image.

### Root Cause

The base board DTS (`nucleo_h753zi.dts`) already defines flash partitions:

| Node | Label | Size |
|------|-------|------|
| `partition@0` | `boot_partition` | 128 KB |
| `partition@20000` | `slot0_partition` | 256 KB |
| `partition@60000` | `slot1_partition` | 384 KB |
| `partition@c0000` | `storage_partition` | 256 KB |

Our overlay added **new** partition nodes at different addresses (`partition@100000`
for slot1, `partition@1e0000` for scratch) but reused the same DTS labels
(`slot0_partition`, `slot1_partition`). Zephyr's devicetree compiler treats
labels as globally unique — a label cannot appear on two different nodes.

### Fix

Use `/delete-node/` directives to remove the base board's conflicting partition
nodes before defining our larger layout. The `boot_partition` (128 KB at `@0`)
stays as-is since our layout uses the same size and address.

```dts
&flash0 {
    partitions {
        /delete-node/ partition@20000;  /* old slot0: 256 KB */
        /delete-node/ partition@60000;  /* old slot1: 384 KB */
        /delete-node/ partition@c0000;  /* old storage: 256 KB */

        slot0_partition: partition@20000 {
            label = "image-0";
            reg = <0x00020000 DT_SIZE_K(896)>;
        };

        slot1_partition: partition@100000 {
            label = "image-1";
            reg = <0x00100000 DT_SIZE_K(896)>;
        };

        scratch_partition: partition@1e0000 {
            label = "image-scratch";
            reg = <0x001E0000 DT_SIZE_K(128)>;
        };
    };
};
```

The `chosen { zephyr,code-partition = &slot0_partition; }` node from the base
DTS is kept as-is (it already points to `&slot0_partition`, which we redefine
at the same address but with a larger size).

---

## Issue 2: Missing Python `click` Module

### Symptoms

After fixing the DTS issue, the MCUboot build step failed:

```
FAILED: zephyr/autogen-pubkey.c
imgtool.py getpub -k root-rsa-2048.pem > autogen-pubkey.c
ModuleNotFoundError: No module named 'click'
```

The MCUboot build uses `imgtool.py` to extract the public key from the signing
key PEM file. `imgtool` depends on the `click` CLI framework.

### Root Cause

The project's Python virtual environment (`.venv`) did not have `click` or
`imgtool` installed. These are not part of Zephyr's base `requirements.txt` —
they are MCUboot-specific dependencies that are only needed when building with
`--sysbuild` and MCUboot enabled.

### Fix

Install the required packages into the venv:

```bash
python -m pip install click imgtool
```

This also pulls in transitive dependencies: `cryptography`, `cbor2`, `cffi`.

---

## Files Changed

| File | Change |
|------|--------|
| `boards/nucleo_h753zi.overlay` | Replaced full partition redefinition with `/delete-node/` + re-add pattern |

## Environment Changes

| Change | Details |
|--------|---------|
| Python venv | Installed `click`, `imgtool`, `cryptography`, `cbor2`, `cffi` |

---

## Lessons Learned

1. **Always check the base board DTS for existing partitions.** Most Zephyr
   board DTS files for MCUboot-capable boards already define `boot_partition`,
   `slot0_partition`, `slot1_partition`. Overlays must use `/delete-node/`
   before redefining nodes at different addresses with the same labels.

2. **DTS labels are globally unique.** You cannot have two nodes with the
   same label, even if they are at different addresses. The devicetree
   compiler enforces this strictly.

3. **MCUboot builds require `imgtool` Python dependencies.** When adding
   MCUboot via sysbuild for the first time, install `click` and `imgtool`
   into the build venv. Add this to the project setup docs / README.

4. **Build with `--pristine` after DTS changes.** The devicetree is generated
   at CMake configure time. Incremental builds may cache stale DTS output.
