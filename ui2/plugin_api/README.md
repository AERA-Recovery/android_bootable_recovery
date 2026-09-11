# AERA Host API 2

Host API 2 lets a plugin with a previously unknown ID install and run without
rebuilding recovery. AERA remains the only UI and privileged process. The
plugin is an isolated ARM64 worker that sends bounded declarative UI messages
over a `SOCK_SEQPACKET` channel inherited as file descriptor 4.

## Manifest contract

An API 2 manifest uses `type: "ui-runtime"`, `entry: "main"`,
`min_host_api: 2`, `protocol_version: 2`, and
`executable: "usr/bin/aera-plugin"`. It must request `display` and
`touch-input`. Supported privileged permissions are:

- `settings-backup` / `settings-restore` for AERA Recovery preferences
- `android-settings-backup` / `android-settings-restore` for Android user
  0's SettingsProvider `system`, `secure`, and `global` databases, plus
  the LineageSettingsProvider database when the installed ROM supplies it

Access is denied unless the permission is declared and the user confirms the
individual request in trusted AERA UI. The Android settings operations use wire
operation IDs 3 and 4. They include ROM customization values such as Infinity-X
and Lineage settings but deliberately exclude lock credentials, accounts, app data,
SettingsProvider SSAIDs, and arbitrary `/data` access. Restore validates and
stages the complete snapshot before transactionally replacing live files.

API 1 manifests and their built-in scene routing remain compatible.

## Protocol and limits

`protocol.hpp` is the canonical wire definition. Every packet is exactly 1144
bytes, includes the `A2PI` magic and contains only fixed-size strings. The
worker starts with `HELLO`, placing its minimum protocol version in `value` and
maximum in `flags`. AERA replies with `HELLO_ACK` and `LIFECYCLE/RESUME`.

The initial declarative surface supports a page title/body, up to 12 action
buttons, status updates, lifecycle events and mediated operations. AERA rejects
bad sequences, duplicate action IDs, unknown flags, oversized packets and more
than 128 worker messages per second.

The host expands a hash-verified runtime into private RAM, launches it under a
dedicated UID with no capabilities, storage, devices or network, and limits it
to 256 MiB RAM, 64 file descriptors, 32 processes and 16 MiB output files.
Leaving the scene, protocol failure or worker exit closes the channel and kills
the complete cgroup.

`AERA-settings-backup-plugin` is the reference implementation. It proves a new
ID can render and request backup/restore without being added to recovery's
launcher source.
