# NEXUS

Network inspection for the desktop, the counterpart of the NEXUS screen in the
iPhone app.

- **Discover** asks devices on this network to announce their services with
  multicast DNS (Bonjour) and lists printers, speakers, TVs, file shares,
  computers and smart-home devices with their addresses and ports.
- **Scan** makes an ordinary TCP connection to each port you choose on a host,
  an address range or a CIDR network, and reads the greeting of services that
  send one (SSH, HTTP, FTP, SMTP, POP3, IMAP, VNC). Ports it only guesses from
  their number are marked *inferred*.
- **Inspect** looks up DNS records and checks a web address: its redirect
  chain, status, timing, TLS version, certificate chain and security headers.
- **History** keeps the last 50 scans on this computer, compares each scan
  with the previous one of the same target, and exports a scan as JSON or CSV.

Only this computer and private networks can be scanned until you turn on
**Allow internet targets** in Settings; then up to four single internet hosts
can be scanned after you confirm you own them or have written permission to
test them. Networks you list under **Authorized networks** count as yours.

Install it from the Market to add **NEXUS** to the desktop rail. It is bundled
but off by default. How it works and its limits are in
[docs/marketplace/nexus.md](../../../docs/marketplace/nexus.md).
