# AERA Host API 2

Host API 2 lets a plugin with a previously unknown ID install and run without
rebuilding recovery. The Host API surface remains rendered by AERA. The plugin
is an ARM64 root worker that sends bounded declarative UI messages over
a `SOCK_SEQPACKET` channel inherited as file descriptor 4. It runs in recovery's
filesystem and network namespaces as UID/GID 0, with recovery's capabilities,
devices, mounted partitions and data available directly. Running the plugin
therefore grants it the same access as recovery itself.

## Manifest contract

An API 2 manifest uses `type: "ui-runtime"`, `entry: "main"`,
`min_host_api: 2`, `protocol_version: 2`, and
`executable: "usr/bin/aera-plugin"`. It must request `display` and
`touch-input`. Supported privileged permissions are:

- `settings-backup` / `settings-restore` for AERA Recovery preferences
- `android-settings-backup` / `android-settings-restore` for Android user
  0's SettingsProvider `system`, `secure`, and `global` databases, plus
  the LineageSettingsProvider database when the installed ROM supplies it
- `screen-mirror` to start or stop AERA's existing USB-only display stream;
  framebuffer capture and input injection remain in the trusted host

Host-mediated operations are denied unless the permission is declared and the
user confirms the individual request in trusted AERA UI. Root plugins may also
operate on recovery resources directly. The Android settings operations use
wire operation IDs 3 and 4; USB mirror start/stop use IDs 5 and 6. The Android
settings convenience implementation includes ROM customization values such as
Infinity-X and Lineage settings but excludes lock credentials, accounts, app
data and SettingsProvider SSAIDs. Restore validates and stages the complete
snapshot before transactionally replacing live files.

API 1 manifests and their built-in scene routing remain compatible.

## Protocol and limits

`protocol.hpp` is the canonical wire definition. Every packet is exactly 1144
bytes, includes the `A2PI` magic and contains only fixed-size strings. The
worker starts with `HELLO`, placing its minimum protocol version in `value` and
maximum in `flags`. AERA replies with `HELLO_ACK` and `LIFECYCLE/RESUME`.

The initial declarative surface supports a page title/body, up to 12 action
buttons, status updates, lifecycle events and mediated operations. The host
advertises optional extensions in the `HELLO_ACK` flags. `kFeatureMetrics`
adds compact section headers, up to 64 keyed metric rows, and in-place metric
updates. `kFeatureBackNavigation` lets a page declare one enabled action as its
parent with `SET_BACK_ACTION`; AERA's edge-back gesture then invokes that
action before leaving the plugin. A worker must not send extension messages
unless the corresponding feature bit was advertised; this preserves
compatibility with older API 2 hosts. AERA rejects bad sequences, duplicate
action or metric IDs, unknown flags, oversized packets and more than 128
worker messages per second.

The host expands a hash-verified runtime into private RAM and launches its musl
loader directly as root. It does not apply a chroot, Minijail, namespaces,
seccomp, capability removal or resource limits to Host API 2 plugins. Leaving
the scene terminates the plugin's process group and closes the channel. `PATH`
contains both the plugin's `usr/bin` and recovery command directories, while
`AERA_PLUGIN_ROOT` names the extracted runtime for bundled resources.
`AERA_LOCALE` contains the selected recovery locale (for example `de_DE` or
`zh_CN`) and `LANG` carries the matching UTF-8 locale. Runtime plugins own
their visible strings and must fall back to English when a locale or string is
missing. Relaunching a plugin after a language change supplies the new locale
without changing the Host API 2 wire format.

`AERA-settings-backup-plugin` is the reference implementation. It proves a new
ID can render and request backup/restore without being added to recovery's
launcher source.

## Host API 3: pixel plugins

A manifest with `protocol_version` 3 and `min_host_api` 3 (permissions
`display`, `touch-input` and `pixel-surface`; optionally `gpu-acceleration`,
`network`, `audio-output`) declares a plugin that draws its own pixels, such
as a Flutter app. The runtime layout, verification and process model are
Host API 2's; `scenes/pixel_plugin_scene.cpp` replaces the declarative page.

- Launch: `--aera-host-api=3`, `AERA_HOST_API=3`, `AERA_SURFACE_FD=3`,
  `AERA_APPEARANCE` (`light` or `dark`, AERA's surface mode) and
  `AERA_PLUGIN_DATA`, a private directory that survives updates
  (`/sdcard/AERA/plugin-data/<id>`, or RAM when storage is not mounted, and
  then `AERA_PLUGIN_DATA_VOLATILE=1`: nothing saved survives a reboot).
- Handshake: the worker's `HELLO` must offer version 3 (`value` 3, `flags`
  3); AERA answers `HELLO_ACK` with `value` 3 and `flags`
  `kFeatureBackNavigation | kFeaturePixelSurface | kFeatureKeyboardInset |
  kFeatureFilePicker`,
  then `SURFACE`, then `LIFECYCLE/RESUME`. Version 3 packets carry 3 in the
  header.
- Surface: fd 3 is a sealed memfd of `slots` BGRA8888 top-down frames.
  `SURFACE` gives width (`value`), height (`flags`), stride in bytes
  (`request_id`), title `BGRA8888` and text `slots=N scale=S refresh=HZ`
  (scale from `ro.sf.lcd_density`). The size is the panel's pixels for the
  area below the status bar.
- Rotation: the plugin keeps running. AERA sends a new `SURFACE` with the
  rotated size on the same fd, which it sized for either orientation at
  launch. Each `SURFACE` starts a generation (0 for the first): `PRESENT`
  carries the generation it was drawn for in `flags`, and a frame of an
  earlier generation is returned with `FRAME_DONE` unshown. After a new
  `SURFACE` every slot is the plugin's again, and nothing is shown until a
  frame of the new generation arrives.
- Lifecycle: like Browser, a running plugin outlives its scene. `LIFECYCLE`
  `value` 1 resume (on screen, taking input), 4 inactive (on screen, but
  the status shade, a sheet or the file picker takes the input; resume
  follows), 2 pause (its scene was left for Home, Recents or another app;
  nothing is shown and no `FRAME_DONE` comes, so drawing stalls) and 3 stop
  (it is ending; the process group is then terminated). After a pause AERA
  sends only `SURFACE` then `RESUME` when the plugin is opened again (every
  slot is the plugin's again, in the current orientation), or `STOP` and
  `CLOSE` when it ends: when it sends `CLOSE` itself, when Recents is
  cleared or drops it, or when an update replaces it. A plugin that ignores
  inactive still behaves correctly. These map to Qt's ApplicationActive,
  ApplicationInactive, ApplicationHidden then ApplicationSuspended and
  aboutToQuit, to Flutter's resumed, inactive, hidden then paused and
  detached, and to SDL's background events.
- Frames: the worker writes a slot it owns and sends `PRESENT` (`request_id`
  sequence, never 0 or repeated; `value` slot; `flags` the surface
  generation). `FRAME_DONE` returns a slot:
  at once when a newer `PRESENT` supersedes a frame never shown, otherwise
  when a newer frame has replaced it on screen. The frame on screen stays
  AERA's. `PRESENT` is outside the 128 messages per second limit.
- Input: `TOUCH_DOWN`/`MOVE`/`UP` (`request_id` pointer, `value` x, `flags`
  y, surface pixels); side edges and the bottom edge stay AERA's gestures.
  `BACK` is AERA's Back; the worker leaves with `CLOSE`.
- Keyboard: `KEYBOARD_SHOW` (`value` 0 text or 2 digits, `flags` 1 for
  multiline) and `KEYBOARD_HIDE` show AERA's keyboard, which sends `KEY`
  (`value` code point; 8 backspace, 13 Enter, and OK on a single-line
  field). `KEYBOARD_INSET` gives the keyboard's height over the surface in
  pixels on every show, hide and resize.
- Files (`kFeatureFilePicker`): `REQUEST_OPERATION` with `value` 8
  (`kPickFiles`) and a non-zero `request_id` opens AERA's file picker over
  the surface. `flags` is the mode (0 one file, 1 several, 2 a folder, 3 a
  name to save as), `title` the folder to start in (empty: the current
  storage), `text` the extensions to show (`zip,img`; empty: all) or, to
  save, the suggested name. It browses like Files and needs no permission:
  the pick is the user's consent, and the plugin is root already. Each
  chosen path comes back as an `OPERATION_RESULT` (`value` 1, `text` the
  absolute path, `flags` 1 while more follow). Closing the picker answers
  `value` 0 with empty `text`; a refusal (one picker at a time) answers
  `value` 0 with the reason. The other host operations stay with Host API
  2's declarative pages; Host API 2 page messages end the session.

`tests/plugin_api_pixel_host_check.cpp` runs a real pixel plugin through
the launcher, session and surface without LVGL.
