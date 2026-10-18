# OpenWrt Router package

`marketplace/packages/openwrt` is a bundled desktop package (Market id
`openwrt`, entrypoint `builtin:openwrt`, rail label **Router**, icon
`broadcast`) that manages an OpenWrt router on the user's network. It is off by
default and built as a lazily loaded package module on Linux
([../desktop/package-modules.md](../desktop/package-modules.md)). The
user-facing description is the package
[README](../../marketplace/packages/openwrt/README.md).

## Why the desktop only

The router lives on the user's LAN. The web app runs on the core nodes and
cannot reach it, and a browser page on the LAN would hit mixed-content and
CORS walls against the router's own HTTP server. The desktop sits on the same
network and already has the egress firewall, so it is the one surface that can
talk to the router directly. The phone apps could do the same later; nothing
in the model or client depends on Qt Widgets.

## Code map

| File | Responsibility |
| --- | --- |
| `OpenWrtModel.{h,cpp}` | Pure logic: JSON-RPC bodies and reply parsing, address rules, default-gateway parsing, UCI section helpers, change descriptions, lease, package and service parsing, validators, choices. Qt Core and `QHostAddress` only. |
| `OpenWrtClient.{h,cpp}` | HTTP to the router through `NativePackageHost::network`: sign-in, calls, session renewal, `file exec`, `cgi-upload` and `cgi-backup`, certificate pinning. |
| `OpenWrtFeature.{h,cpp}` | The page: sign-in form, pending-changes bar and the nine tabs. |
| `tests/test_openwrt.cpp` | `forkmesh-openwrt-tests`: model, client against a fake ubus server, and the page signing in and staging a change. |

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
