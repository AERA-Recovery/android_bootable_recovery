# AERA PC Connection

The Wi-Fi settings sheet enables a browser workspace at `https://aera.local`.
The sheet also shows an IP address when multicast DNS is unavailable. Another
recovery using that hostname causes a device-specific `.local` name instead.
PC connection is off by default. Auto-enable is saved with the ordinary recovery
preferences and runs after unlock and Wi-Fi connection, not before decryption.

This feature does not require a PC application, wireless ADB or a new device-tree
flag. It uses the existing WLAN build option, OpenSSL and JSON dependencies.

## Connection And Installation

- HTTPS uses a private device authority generated on the phone. The first browser
  visit requires accepting the certificate warning. Its SHA-256 fingerprint is
  displayed in recovery. The Device tab exports only the public authority, not
  a private key. Certificate import is manual: browsers cannot install trusted
  certificates themselves. The device authority remains stable across service
  and recovery restarts; server certificates include the current IP and `aera.local`.
  Without persistent writable data the authority is temporary. A data wipe
  requires approving a new authority.
- A new browser requests approval on the device. Remembering a browser stores
  its key fingerprint in `/data/misc/aera/pc-connection/computers.json`; no
  plaintext browser secret is stored on the phone. If writable persistent data
  is unavailable, approval is session-only.
  Reloading the page resumes an approved browser's current session without a
  second physical approval, even when Remember was not selected. Remember is
  needed only to retain approval after restarting the service or recovery.
- A complete ZIP or IMG is written to the selected mounted storage under
  `AERA/PCTransfers/<random-id>/`. It is not streamed into an installer or partition.
- Size and SHA-256 must match before the browser can confirm installation.
  Verified ZIPs are inspected with the same payload/OTA inspector as the native
  install review. Version, device, Android, patch level, payload layout and full
  partition details are available before confirmation. Firmware ARB downgrade
  blocks and explicit upgrade acknowledgement follow the native review rules.
  Installation then uses the existing recovery ZIP or image backend, including
  recovery/ABL preservation and the backend's image partition/slot rules.
- The first device approval is physical. Subsequent installation confirmations
  and structured AERA installer questions are handled in the authorized browser.
  This does not automate arbitrary third-party installers' hardware-key prompts.
- A queued or running PC installation wakes the display and can replace the
  idle swipe lock. It never bypasses the Android data-decryption screen.
- Browser or Wi-Fi disconnection cannot cancel an already started installation.
  Reconnect to read its progress, log and result. Only pending transfers/jobs can
  be discarded; a running flash cannot be cancelled remotely.
- The browser mirrors structured package/device/author/stage information from
  the native installer when provided. Finished activity remains visible without
  a time limit until changing tabs; active jobs remain visible across tab changes.
- Temporary files are removed after normal completion, failure or cancellation.
  A sudden reboot or power loss can leave a transfer folder for manual removal.

AERA Remote retains the listener on HTTP port 80 when screen mirroring is running.
PC Connection remains on HTTPS port 443. With Remote enabled, ordinary HTTP visits
open mirroring exactly as before; `/pc` explicitly redirects to the PC workspace.
With Remote disabled, the PC listener redirects HTTP visits to HTTPS. Existing
mirroring links with `?code=...` and the explicit `/remote` entry remain supported.
When Remote stops, the PC redirect listener can reclaim port 80. Redirects keep a
recognized advertised hostname, rather than changing the browser's remembered origin.

## Additional Tools

- Root uses the existing KernelSU, KernelSU Next and SukiSU backend. Select A or B,
  inspect its kernel, optionally fetch a compatible release, patch, restore the
  existing rollback backup, or stage a manager APK. The browser path checks KMI
  from the target slot's unpacked/decompressed boot kernel, not the running recovery
  kernel. It refuses an unknown/unsupported target and uses exact matching modules.
  This allows manual inactive-slot patching after an OTA without switching slots
  or rebooting automatically. Root still requires writable unlocked internal storage.
  Devices without the init_boot/ksud backend show an unavailable state.
  The active slot is selected initially. Browser root jobs retain operation guards
  but do not replace the phone's current page with an operation/completion screen.
- Confirmed power actions use the native UI-thread reboot path. Android, recovery,
  bootloader, fastbootd and power-off are blocked while a critical operation is active.
  The HTTP acknowledgement is sent before a reboot can be consumed by the UI.
- Files browse available mounted storage with directory-first ordering and paging.
  Server downloads stream with a fixed-size buffer and single-use tickets. The
  browser saves files up to 64 MiB via an authenticated fetch/Blob, avoiding silent
  native HTTPS-download failures. Larger files stream to a file picker in supported
  browsers, or use a normal browser download requiring a trusted certificate.
  Choosing Upload file immediately verifies and saves the file into the current
  folder without switching to Install; saving does not flash anything. Rename and
  save never overwrite another file. Delete handles files and empty folders only.
  ZIP/IMG files already on mounted storage can be selected for installation or
  image flashing without uploading/copying them. They use the same review and
  confirmation; image target/slot rules still apply. Original files are never
  included in transfer cleanup. File identity/size/timestamps are checked again
  before execution to reject a changed selection.
  Delete does not recursively remove directories or storage roots. The private key
  directory is excluded even if a storage root would contain it.
- Device information includes slots, kernel, memory, battery, uptime and connection.
  Recovery-log, Logcat and kernel-log downloads are authorized snapshots. Logcat
  includes the last 4,000 records across available buffers. Diagnostic command
  capture has a ten-second timeout and four-MiB ceiling; no continuous logging
  daemon is added. Missing log commands return a visible error.

## Protocol

Image targets include native flashable/logical volumes and discovered physical
firmware block devices under by-name. Physical A/B pairs are grouped and use the
active slot unless both slots are explicitly selected. Native raw/sparse image
writers enforce target capacity. Super/COW/mapper devices and personal-data or
calibration partitions are not exposed through the generic firmware fallback.

Schema 1 endpoints use JSON except the raw file upload:

| Endpoint | Purpose |
| --- | --- |
| `GET /api/hello` | Device and protocol information |
| `POST /api/connect` | Request approval or resume a remembered browser |
| `POST /api/connect/status` | Poll the same approval request |
| `GET /api/status` | Storage, image targets, job progress, log and installer question |
| `POST /api/prepare` | Declare filename, byte count, SHA-256, storage and optional image target |
| `PUT /api/upload/<id>` | Transfer exactly the declared number of bytes |
| `POST /api/install` | Explicit confirmation of a verified file |
| `POST /api/cancel` | Discard a transfer or queued installation |
| `POST /api/prompt` | Answer the current structured installer question |
| `POST /api/forget` | Revoke the current browser's saved approval and sessions |
| `POST /api/root` | Queue a confirmed root action with explicit provider and slot |
| `POST /api/reboot` | Queue a confirmed native reboot/power action |
| `POST /api/files/list` | List a mounted-storage directory with paging |
| `POST /api/files/prepare` | Review an existing ZIP or IMG without copying it |
| `POST /api/files/download` | Create a short-lived single-use download ticket |
| `GET /api/files/content/<ticket>` | Stream the authorized regular file |
| `POST /api/files/mkdir`, `rename`, `delete` | Explicit non-recursive file changes |
| `GET /api/certificate` | Export the public device authority |
| `GET /api/log` | Recovery log snapshot |
| `GET /api/log/logcat`, `GET /api/log/kernel` | Bounded diagnostic command snapshot |

Approved endpoints require a bearer session token. A different approved browser
may observe the current job, but cannot install, discard or answer its prompts.
Network handlers never call the recovery partition manager or LVGL. The UI
thread snapshots available targets and starts confirmed jobs only when its
existing critical-operation guards allow it.
Root operations share a serialization lock with native Root Manager actions;
new PC jobs also wait while the native Root Manager page is open. The file tool
does not expose shell execution or block-device writes.

## Dependencies And Tests

- `third_party/mdns/mdns.h`: mjansson/mdns, commit
  `a569c4759bd47e0f2a7bfc4d4c19620445782806`, Unlicense.
- `client/pc/sha256.js`: hash-wasm 4.12.0 SHA-256 build, MIT.
- Client icons: Lucide, ISC. License files are included with the client.

The `pc_connection_host` test target uses the real HTTPS/upload service with a
fake installer and loopback-only binding. It never accesses device partitions.
`pc_connection_test.py` covers physical approval, ownership, interrupted and
invalid uploads, image target restrictions, disconnect during installation,
remembered browser access and revocation. `ui_check --pc-settings` checks native
settings layout in six languages, four UI sizes and both orientations.

The test fixture's `--demo` flag automatically approves browsers for UI testing
only. It is not compiled into recovery and is not a production protocol option.
