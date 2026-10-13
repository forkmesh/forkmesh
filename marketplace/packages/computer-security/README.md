# Computer Security Checkup

A desktop security checkup. It scans local configuration, the process list and
the operating system's TCP and UDP listener tables, then groups more than 300
checks by firewall, rootkits, malware, network exposure, kernel hardening,
kernel modules, accounts, file permissions, secrets, privilege escalation,
mounts, remote access, safeguards, persistence and service exposure.

Every check explains what it is, and findings say whether the service, port or
module is actually in use. Filter buttons show just the checks that need
attention, are critical, are for review or can be fixed. Many findings have a
fix button: it shows exactly what will change, asks for confirmation and then
the administrator password, and never deletes anything (suspicious files are
quarantined).

Install it from the Market to add **Computer Security Checkup** to the desktop
rail. It is bundled but off by default.

The scan itself is read-only and sends results nowhere. Its checks, fixes and
platform limits are documented in
[docs/marketplace/computer-security.md](../../../docs/marketplace/computer-security.md).
