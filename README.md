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

### EFISP (optional)

Set `AERA_ENABLE_EFISP := 1` in the device configuration to expose the exact,
non-slotted `efisp` partition in Backup and Wipe. It is backed up/restored as a
raw image. Wipe requires a separate warning acknowledgement: Format Data is
needed afterwards before Android can boot, and that format erases internal
storage. EFISP is never included in factory reset and is not preselected.

The same flag enables tapping an ARM64 `.efi` application in File Manager to
flash it directly to EFISP, matching `fastboot flash efisp file.efi`. The image
must fit and have a valid PE32+ EFI-application header. A swipe confirmation is
required and written bytes are read back and compared. This checks integrity,
not device compatibility or authenticity; use a file intended for the device.
No slot switching, filesystem formatting, or automatic data formatting occurs.
The feature is disabled by default; no EFISP fstab entry is required.

With EFISP enabled, startup also records read-only ABL assessments for both slots
(or non-slotted ABL). ABL is read only through its ELF image extent; unchanged
hashes reuse the cached assessment before reboot, shutdown or live mode changes.
If a slot had the EFISP loader at boot and subsequently loses it, a separate
warning offers Cancel or an explicit swipe to continue. Unreadable/unrecognized
replacements are reported as unknown rather than called non-vulnerable. An
intentional change can be acknowledged for that exact image state; any later
change requires a new acknowledgement. No bootloader or EFISP data is altered
by these checks. This is structural loader detection, not signature validation.
External forced resets and power loss cannot be intercepted by recovery UI.
