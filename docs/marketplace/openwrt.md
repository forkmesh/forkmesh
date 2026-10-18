# OpenWrt Router package

`marketplace/packages/openwrt` (Market id `openwrt`, entrypoint
`builtin:openwrt`) manages an OpenWrt router on the user's network from the
desktop (rail label **Router**, icon `broadcast`), the iPhone and Android
(Home tile **Router**). It is off by default and account-synced
(`install.sync: "account"`, `places: ["mobile", "desktop"]`), so installing it
on one device shows it on the others. On Linux the desktop page is a lazily
loaded package module ([../desktop/package-modules.md](../desktop/package-modules.md)).
The user-facing description is the package
[README](../../marketplace/packages/openwrt/README.md).

## Where it runs

The router lives on the user's LAN. The web app runs on the core nodes and
cannot reach it, and a browser page on the LAN would hit mixed-content and
CORS walls against the router's own HTTP server, so there is no web page. The
desktop and the phones sit on the same network and talk to the router
directly. All three implement the same model and the same calls, with the same
strings and rules, so the surfaces behave alike.

| Surface | Logic | Screens | Tests |
| --- | --- | --- | --- |
| Desktop | `content/desktop/OpenWrtModel.*`, `OpenWrtClient.*` | `OpenWrtFeature.*` (ten tabs) | `forkmesh-openwrt-tests` |
| iPhone | `native/ios/ForkMeshCore/Sources/FMPackages/OpenWrt{Model,Client,Router}.swift` | `native/ios/ForkMesh/Features/Router/`, `Platform/OpenWrtHTTPClient.swift` | `FMPackagesTests/OpenWrtTests.swift` |
| Android | `native/android/core/.../openwrt/OpenWrt{Model,Client}.kt` | `app/.../feature/openwrt/` | `core/.../openwrt/OpenWrtTest.kt` |

The phones carry everything except backup, restore, firmware upgrade and
factory reset, which need files and are safer at a computer; their System
screen says so.

## Code map (desktop)

| File | Responsibility |
| --- | --- |
| `OpenWrtModel.{h,cpp}` | Pure logic: JSON-RPC bodies and reply parsing, address rules, default-gateway parsing, UCI section helpers, change descriptions, lease, package, service and storage parsing, validators, choices. Qt Core and `QHostAddress` only. |
| `OpenWrtClient.{h,cpp}` | HTTP to the router through `NativePackageHost::network`: sign-in, calls, session renewal, `file exec`, `cgi-upload` and `cgi-backup`, certificate pinning. |
| `OpenWrtFeature.{h,cpp}` | The page: sign-in form, pending-changes bar and the ten tabs. |
| `tests/test_openwrt.cpp` | `forkmesh-openwrt-tests`: model, client against a fake ubus server, and the page signing in and staging a change. |

On the phones the same logic is split into the model, the client and a
snapshot loader (iOS `OpenWrtRouter.swift`, Android `OpenWrtSnapshot` in
`OpenWrtClient.kt`) with `stage`, `addSection`, `deleteSection`, `discard`,
`apply` and `packageAction`. Edits are built by `interfaceEdit`, `radioEdit`,
`wifiEdit`, `forwardEdit`, `reservationValues` and friends in the model, so
validation sits next to its tests rather than in the views.

## Phones

- **Reaching the router.** iOS resolves the address first and sends the
  request to the IP literal, with `NSAllowsLocalNetworking` in
  `ForkMesh-Info.plist`, so App Transport Security allows plain HTTP to the
  router. `OpenWrtHTTPClient` (URLSession) refuses redirects and replaces
  certificate evaluation with the pinned fingerprint. The local-network
  permission text names the Router plugin. On Android the app's network
  security config forbids cleartext except to the emulator host, and a domain
  rule cannot list every private address, so `RouterHttp` speaks HTTP/1.0 over
  a plain socket (or an `SSLSocket` whose trust manager accepts only the
  pinned fingerprint) instead of `HttpURLConnection`. It reads the answer to
  the end and undoes chunked encoding. Both paths run only after
  `isLocalHost` accepted every resolved address, the rule the desktop applies.
- **State.** iOS keeps an `@Observable` `OpenWrtRouterModel` per Router
  screen; Android a process-wide `OpenWrtState.shared` its sub-screens share.
  The address, user and pinned fingerprints are stored (`UserDefaults`, and the
  `market.packages.openwrt` shared preferences); the password is not.
- **Apply.** The same rollback loop as the desktop, with real sleeps injected
  by the app (`apply(rollback:wait:)`); the tests pass a recorder instead.

## USB storage

The Storage tab (and phone screen) lists USB disks and memory cards and what
can be done with them:

- **Disks.** `luci getBlockDevices` (it runs `block info`) and `luci
  getMountPoints` (`/proc/mounts` plus `statvfs`), merged by `storageDisks`
  with the fstab `mount` sections (matched by UUID, then device, then label).
  Only `sd*`, `mmcblk*`, `nvme*`, `hd*` and `vd*` names count
  (`isStorageDevice`); the router's own flash (`mtdblock*`, `ubiblock*`),
  `loop` and `zram` never show up.
- **Mount at boot** adds or edits an fstab `mount` section (`target`,
  `enabled`, and `uuid`, or `device` when the filesystem has no UUID).
  Folders must be under `/mnt`, `/media`, `/srv` or `/data`
  (`mountPathProblem`), so a typo cannot mount a disk over `/etc`.
- **Mount any disk when it is plugged in** sets the fstab `global` section's
  `anon_mount` (and `auto_mount`), which mounts unknown disks at
  `/mnt/<device>`.
- **Mount disks now** runs `/sbin/block mount`; it mounts what is saved, so
  the page warns when fstab changes are still waiting. **Unmount** runs
  `/bin/umount <mount point>` after a confirmation. LuCI's Mount Points page
  uses the same two commands, so its `luci-mod-system` ACL allows them.
- **Network shares.** When the router has a `ksmbd` (preferred) or `samba4`
  configuration, its `share` / `sambashare` sections are listed and a mounted
  disk can be shared with or without guest access (`read_only no`,
  `create_mask 0666`, `dir_mask 0777`). Without one, the page names the
  packages to install.
- **Install USB storage support** installs `kmod-usb-storage`,
  `kmod-usb-storage-uas`, `block-mount`, `kmod-fs-ext4`, `kmod-fs-vfat` and
  `kmod-fs-exfat` through the package tool in one command.
- Formatting is not offered: `mkfs` is in no LuCI ACL, and erasing a disk from
  a phone is too easy to get wrong.

## Protocol

Everything goes through LuCI's own backend: `POST /ubus` JSON-RPC
(`uhttpd-mod-ubus` and `rpcd`), so ForkMesh needs nothing installed on the
router beyond what a LuCI image ships and acts with exactly the rights the
signed-in account has.

- **Sign-in**: `session login` with a 3600 s timeout. When a call is answered
  with access denied (`-32002` or ubus status 6) and the password is still in
  memory, the client signs in once more and retries; calls that fail meanwhile
  queue behind the one renewal. rpcd keeps staged UCI changes per session, so
  a renewal loses them, and the page says so if any were pending.
- **Settings**: every edit is `uci set`/`add`/`delete` into the session's
  staging area, never a direct commit. `uci changes` feeds the bar. Apply is
  `uci apply` with `rollback: true, timeout: 90`, then `uci confirm` retried
  every 2 s until 80 s have passed; if the router never answers, rpcd rolls
  back by itself. This is LuCI's own safety net, and it is on by default.
  Changing the LAN address you are connected through cannot be confirmed at
  the old address, so the page warns to turn the safety net off for that one
  change. Values left empty delete the option rather than setting `''`.
- **Status**: `system board`, `system info`, `network.interface dump`,
  `network.wireless status`, `rc list`, and `luci-rpc getDHCPLeases` with a
  fallback to reading `/tmp/dhcp.leases`.
- **Actions**: `rc init` for services, `network.interface down`/`up` to
  restart an interface, `system reboot`, `luci setPassword`.
- **Packages**: `file exec` of the first tool the router allows, in order:
  `/usr/libexec/package-manager-call` (LuCI 24.10+), `/usr/libexec/opkg-call`
  (older LuCI), `/usr/bin/apk`, `/bin/opkg`. rpcd only executes commands an
  ACL names, and the LuCI wrappers are what LuCI's ACLs name; the raw managers
  are the fallback for routers with custom ACLs. The list parser accepts
  `name - version` lines, opkg status blocks and `apk list` lines. Package
  names are checked against `[a-z0-9][a-z0-9+._-]*` so a name cannot become an
  option or a second command.
- **Backup, restore, firmware, reset**: the same calls LuCI's System pages
  make, so LuCI's `luci-mod-system` ACL allows them: `cgi-backup`;
  `cgi-upload` to `/tmp/backup.tar.gz` then `sysupgrade --restore-backup` and
  a reboot; `cgi-upload` to `/tmp/firmware.bin`, `system
  validate_firmware_image`, then `sysupgrade [-n] /tmp/firmware.bin`; and
  `firstboot -r -y`. Uploads are checked against the SHA-256 the router
  reports. An image that fails validation is refused with the reason (a
  device-mismatch is named) and removed; a forced flash is not offered.

## Safety

- **Local routers only.** `parseAddress` refuses a public IP literal and any
  address with user info; a hostname that does not look local (`.lan`,
  `.local`, `.home.arpa`, a single label …) is resolved and accepted only if
  every address is private, link-local, unique-local or loopback. The root
  password must never be typed into a page that sends it across the internet.
- **Password.** Kept in memory by `Client` for session renewal, cleared on
  sign-out, never written to settings. Only the address, user and pinned
  certificate fingerprints are stored, under
  `market/packages/openwrt/data/`.
- **HTTPS.** OpenWrt's `luci-ssl` certificate is self-signed. On a TLS error
  the client reports the leaf certificate's SHA-256; the page shows it and
  **Trust this certificate** pins it per host and port. A pinned certificate
  is accepted only while it matches. Plain HTTP works (it is OpenWrt's
  default) with a note that the password crosses the LAN unencrypted.
- **Network log.** Every request is marked with
  `forkmesh::markPrivatePayload` (`desktop/src/NetworkReplyError.h`).
  `BackoffNetworkAccessManager` then records only the body sizes, because
  these bodies carry the password, the session id and Wi-Fi keys, and the
  network log is archived to disk and shared in diagnostics. The firewall
  still asks about the router's address like any other destination.
- **Confirmations.** Restarting, stopping or disabling a service the router
  needs to stay reachable (`network`, `firewall`, `dropbear`, `uhttpd`,
  `rpcd`, `dnsmasq`, `odhcpd`, `ubus`, `log`), removing a package, deleting a
  Wi-Fi network, port forward or section, restoring a backup and flashing
  firmware all ask first; a factory reset requires typing the hostname.
- **Masking.** The change list and the All settings tree show `key`,
  `password`, `sae_password`, `private_key` and similar options as dots; the
  editor shows the real value only when the option is selected.

## Limits

- No wireless scan, mesh or client (station) mode editing beyond the generic
  All settings tab; station networks show their mode and can be edited there.
- No IPv6 prefix delegation or VLAN/bridge device editor; both are reachable
  through All settings.
- The package list shows installed packages only; installing needs the exact
  package name.
- macOS reads the gateway with `/sbin/route -n get default`, Linux from
  `/proc/net/route`; Windows starts with an empty address.
