# Bug Reports

These bug reports originate from downstream (forked) projects that use this
repository as their base. They are collected here because the issues and
lessons learned are relevant to anyone building on the same Zephyr
foundations (UART command interface, networking, etc.).

Each report documents the symptoms, root cause, and fix so that upstream
users can avoid the same pitfalls.

## Index

| # | Title | Severity | Status |
|---|-------|----------|--------|
| 001 | [W5500 DHCP Failure — No Lease Acquired](001_w5500_dhcp_failure.md) | High | Resolved |
| 002 | [UART Commands Dispatched Twice](002_uart_double_command_dispatch.md) | Low | Resolved |
| 003 | [HTTP POST Body Missing](003_http_post_body_missing.md) | High | Resolved |
| 004 | [HTTP Unreachable After IP Change](004_http_unreachable_after_ip_change.md) | High | Resolved |
| 005 | [Sysbuild OTA Build Failures](005_sysbuild_ota_build_failures.md) | Medium | Resolved |
| 006 | [MCUboot No Bootable Image](006_mcuboot_no_bootable_image.md) | High | Resolved |
| 007 | [MCUmgr Silently Disabled](007_mcumgr_silently_disabled.md) | Minor | Resolved |
| 008 | [Settings NVS Backend Fails — UINT16 Sector Size Limit](008_settings_nvs_uint16_limit.md) | High | Resolved |
| 010 | [FCB `flash_area_get_sectors` Returns `-ENOMEM` With Undersized Array](010_fcb_sector_array_undersized.md) | Medium | Resolved |
| 011 | [`event_log_write` Called With printf-Style Format Arguments](011_event_log_write_format_args.md) | Low | Resolved |
| 013 | [Time Service Test Hangs on `qemu_x86` Due to Full Networking Stack](013_time_service_test_hang_networking_stack.md) | Medium | Resolved |
| 014 | [Incorrect UNIX Epoch Used for ISO 8601 Test Assertions](014_iso8601_wrong_reference_epoch.md) | Low | Resolved |
| 015 | [Event Log Offline After OTA — FCB Magic Mismatch](015_event_log_fcb_stale_magic_after_ota.md) | High | Resolved |
| 016 | [HTTP/2 Server Loops on `eventfd failed (-12)` — ZVFS Pool Too Small](016_http_server_eventfd_pool_exhausted.md) | High | Resolved |
| 017 | [SNTP First Sync Races DHCP Lease — UDP Dropped](017_time_service_ota_coupling_and_dhcp_race.md) | High | Resolved |
| 018 | [OTA `module_names[]` Missing Entries — `MISSING: error` In Logs](018_ota_module_names_missing_entry.md) | Medium | Resolved |
| 019 | [`event_log <seconds>` Cross-Boot Filtering Hides Pre-Sync Diagnostics](019_event_log_cross_boot_time_filter.md) | Medium | Resolved |
| 020 | [Build Fails With `undefined reference to z_impl_sys_rand_get` Until RNG Enabled](020_link_failure_when_rng_disabled.md) | Major | Resolved |
| 021 | [`BOARD_ROOT` in App CMakeLists Ignored by Sysbuild](021_board_root_ignored_by_sysbuild.md) | Minor | Worked-around |
| 024 | [RTT Shell and Log Backends Both Default to Channel 0 → BUILD_ASSERT](024_rtt_shell_log_channel_collision.md) | Medium | Resolved |
| 025 | [MCUboot Swap Mode Silently Overridden by Sysbuild Kconfig](025_mcuboot_swap_mode_overridden_by_sysbuild.md) | High | Resolved |
| 026 | [New Defaults Don't Take Effect on Deployed Devices (ZMS Persists Prior Values)](026_config_store_defaults_not_applied_to_deployed_units.md) | Low | Documented |
| 027 | [Event Log Unfilterable When Wall Clock Never Syncs](027_log_filtering_without_wall_clock.md) | Medium | Resolved |
| 028 | [Unit Tests Unrunnable on Windows (QEMU DLLs + ZTest Stack Overflow)](028_unit_tests_qemu_dlls_and_ztest_stack_overflow.md) | Medium | Resolved |
| 033 | [POST /api/mcu/reboot — MCU Resets Before HTTP Response Sent](033_reboot_endpoint_response_race.md) | High | Resolved |
| 035 | [STM32 EXTI Line Conflicts — One Port Per Pin Causes Silent Interrupt Loss](035_stm32_exti_line_conflicts.md) | High | Resolved |

> **Numbering note:** Reports 009, 012, 022–023, 029–032, and 034 are
> intentionally absent — those numbers were assigned in downstream projects
> to issues that are hardware-specific or business-logic-specific and not
> relevant to this generic template. The gaps are preserved so future bug
> imports keep numbers stable.
