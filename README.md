# AERA Recovery

AERA is a native Android recovery environment with an LVGL interface, device-aware OTA updates, networking, and runtime plugins.

- Website: https://aera-recovery.com/
- Project: https://github.com/AERA-Recovery
- Community: https://t.me/aera_recovery_project

AERA builds on Android Recovery and inherited work from TWRP and OrangeFox. Original copyright and license notices are preserved in the relevant source files.

## Root Manager module selection

Enable Root Manager and optionally restrict its bundled kernel modules in the
device's BoardConfig:

```make
AERA_ENABLE_ROOT_MANAGER := 1
AERA_ROOT_MODULE_KERNELS := 6.12
```

This includes only modules whose KMI kernel version is exactly `6.12`, from
all three providers. The bundled catalog is filtered to match. Leave the flag
unset, or use `all`, to retain every available version. Multiple versions can
be listed, for example `6.1 6.12`. This only controls the recovery's bundled
root modules; it does not select or change the device kernel. Unknown versions
fail the build. Changing the selection removes stale modules on incremental
builds, and disabling Root Manager still removes all provider assets.
