# OpenWrt Router

Manage an OpenWrt router on your network from the ForkMesh desktop, iPhone or
Android phone. Install it from the Market to add **Router** to the desktop rail
and the phones' Home screen; it is bundled but off by default, and installing
it on one device installs it on your others.

Sign in with the router's address (ForkMesh fills in your default gateway),
user (usually `root`) and password. The page then offers:

- **Overview**: model, firmware, kernel, uptime, load, memory, storage and the
  state and addresses of every interface. Restart the router.
- **Network**: each interface's protocol (static, DHCP, DHCPv6, PPPoE or
  unmanaged), address, netmask, gateway and DNS servers, the DHCP server for
  the network (on or off, address pool, lease time), and restarting an
  interface.
- **Wi-Fi**: each radio's on/off, channel, country and transmit power; each
  network's name, security (WPA2/WPA3, WPA3, WPA2, WPA/WPA2, OWE, open),
  password, attached network, hidden name and guest isolation. Add and delete
  networks.
- **Devices**: the devices that have an address from the router, and address
  reservations (add, reserve a connected device's address, remove).
- **Port forwards**: add, edit, switch off and delete.
- **Storage**: USB disks and memory cards plugged into the router, with size,
  free space and filesystem. Mount them at boot in a folder you choose, mount
  any disk as soon as it is plugged in, unmount before unplugging, share a
  disk on your network (SMB, with or without a password) and install USB
  storage support in one step.
- **Services**: start, stop, restart, and choose whether each starts at boot.
- **Packages**: list, filter, install and remove packages and update the
  package lists (opkg or apk).
- **System**: hostname, the router password, saving and restoring a backup,
  firmware upgrades (the router checks the image first; keep settings or not)
  and a factory reset.
- **All settings**: every UCI configuration file, section and option on the
  router, with set, delete and add.

The phones have everything except backups, firmware upgrades and factory
resets, which stay on the desktop where the files are.

Changes are not live until you apply them: saved edits wait on the router and
the bar above the tabs lists them, with **Review**, **Discard** and **Apply**.
With "Undo automatically if the router stops answering" on, ForkMesh confirms
the router still answers after applying; if it cannot within 90 seconds, the
router puts the old settings back by itself.

ForkMesh only connects to routers on your own network (private, link-local or
`.lan`-style addresses), keeps the password in memory for the session only, and
the desktop's network log records the size of router traffic but never its
contents. The router needs `uhttpd-mod-ubus` and `rpcd`, which every OpenWrt
image with LuCI includes; a few features use LuCI's access lists (see
[docs/marketplace/openwrt.md](../../../docs/marketplace/openwrt.md)).
