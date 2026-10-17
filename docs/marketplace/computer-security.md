# Security package

`marketplace/packages/computer-security` is a bundled desktop package (Market
id `computer-security`) that runs a local security checkup, explains every
result, shows whether the thing it found is actually in use, and offers a
confirmed one-click fix where a safe one exists. It is off by default and
appears in the desktop rail when installed.

Phones have their own checkup, the
[Phone Security package](phone-security.md) (`phone-security`).

## Name and icon

The package is shown as **Security** in the desktop rail, the page heading, the
Market listing and review pages; it was called "Computer Security Checkup"
until 2026-10-16. The Market id `computer-security`, the entrypoint
`builtin:computer-security` and the C++ class names are unchanged, so installed
copies and saved reviews keep working.

The rail icon is `shield`, a plain octicon-style shield outline in
`desktop/resources/icons/forkmesh/shield.svg` (registered in
`desktop/resources/icons.qrc`). The package always asked for `shield`, but no
such icon was bundled, so the rail button rendered blank.
`marketplace/tests/test_package_rail_icons.py` now fails any native package
whose rail icon is missing from `icons.qrc`. When it landed it also caught the
Feed, Usage and Mastodon packages asking for `rss`, `gauge` and `at-sign` (web
dashboard icon names) in their desktop registrations; those now use the bundled
`broadcast`, `pulse` and `mastodon` octicons. Their web navigation icons are
unchanged.

## What it checks

More than 300 individually named checks, in these families:

- **Firewall**: nftables, UFW (only when enabled) or firewalld; UFW's default
  incoming policy.
- **Rootkits**: known artifact paths, `/etc/ld.so.preload` entries, and kernel
  modules hidden from `/proc/modules` (present under `/sys/module` only).
- **Malware**: known malware artifacts; processes named like miners, posing as
  kernel threads, running from temporary folders, from deleted files or from
  memory only (`memfd`); executables in shared temporary folders; listeners
  on classic backdoor ports.
- **Network exposure**: commonly exposed TCP and UDP services, every other
  port open to the network, promiscuous interfaces (bridges skipped).
- **Kernel hardening and modules**: sysctl settings, AppArmor or SELinux,
  lockdown, taint flags, Secure Boot; uncommon file systems and protocols.
- **Accounts**: extra UID 0 accounts, empty passwords, hashes in
  `/etc/passwd`, shared UIDs, docker group members, passwordless `sudo`,
  desktop automatic sign-in, `/etc/login.defs` policy.
- **File permissions**: private system and host-key files, system files and
  folders any user can change, a world-writable sweep of `/etc`, an open
  Docker socket, unsafe `PATH` entries, home folder, sticky bits.
- **Secrets**: permissions of common credential files in the home folder, SSH
  private keys without a passphrase, `authorized_keys`.
- **Privilege escalation**: shells, interpreters and tools that must never be
  set-user-ID.
- **Mounts**: `nodev`, `nosuid` and `noexec` on shared temporary folders.
- **Remote access**: SSH server options, limits and weak algorithms.
- **Safeguards**: automatic updates, audit daemon, brute-force protection,
  file integrity monitoring, USB policy, encrypted volume, waiting security
  updates, pending restart.
- **Persistence**: scheduled jobs, systemd units, autostart entries and shell
  start-up files that download and run scripts, decode into a shell, open
  reverse shells, point at mining pools, run from temporary folders, set
  `LD_PRELOAD` or redefine `sudo`; `/etc/rc.local`; unsafe `ld.so.conf`
  folders; `~/.ssh/rc`.
- **Service exposure**: service configurations that commonly expose a
  machine, `rhosts`-style trust, NFS `no_root_squash`, Samba guest shares,
  Docker's API on TCP, Redis without a password.

Results are **Pass**, **Review**, **Warning**, **Critical** or
**Unavailable**, sorted with Critical first.

## Explanations and "do you need it?"

Every check carries a plain-language explanation of what the thing is and why
it matters (`SecurityExplanations.cpp`); a test fails if any check lacks one.
Selecting a row shows it with the result.

Where the scanner can tell whether something is in use, the finding also says
so, so the user can decide whether they need it:

- an exposed port names the program holding it (matched through
  `/proc/<pid>/fd` socket inodes, so only the user's own processes are named)
  and counts its established TCP connections;
- a configured service says whether its process is running;
- a loaded module gives its use count and dependent modules;
- IP forwarding names the Docker, VM and VPN interfaces that need it;
- turning SSH passwords off says whether the user has an SSH key set up;
- authorised SSH keys say whether SSH is reachable from the network.

## Filters

Filter buttons (**All**, **Needs attention**, **Critical**, **Warnings**,
**For review**, **Can fix**, **Passed**, **Unavailable**) with counts, a
category menu, and the desktop's shared search box (title, category,
result). An empty result names the empty filter.

## Fixes

A finding with a safe automatic fix gets a fix button and **Copy command**.
The confirmation states what changes, any caution (for example that stopping
SSH ends remote sign-in or that turning SSH passwords off locks out an account
without a key) and the exact command; Cancel is the default. System changes
run through `pkexec sh -c`; fixes to the user's own files run without it. The
checkup reruns afterwards. Fixes are conservative:

- Nothing is deleted: suspicious artifacts are quarantined to
  `/root/forkmesh-quarantine/<timestamp>/` with all permissions removed, and
  trust or preload files are renamed `*.forkmesh-disabled`.
- Kernel settings go to `/etc/sysctl.d/90-forkmesh-security.conf`; modules are
  unloaded if unused and blocked in `/etc/modprobe.d/forkmesh-security-<name>.conf`.
- SSH settings go to `sshd_config.d/00-forkmesh-security.conf` (or are
  prepended to `sshd_config`), validated with `sshd -t`, rolled back on
  failure.
- Services are stopped with `systemctl disable --now` from a fixed list of
  unit names; if none is active the fix says so.
- Missing safeguards and pending updates are installed with `apt-get` or
  `dnf`.
- Remounts last until restart; the description says to edit `/etc/fstab`.

Findings whose right answer depends on the user (hidden modules, authorised
keys, keys without a passphrase, sudo rules, NFS, Samba, Docker, Redis, weak
SSH algorithms, PATH, Secure Boot) have no one-click fix.

## Severity choices

- A rootkit or malware artifact, or a suspicious process, is critical because
  it needs a human look, not because it proves compromise. A program running
  from a deleted file is a review (usually a package update); one running from
  memory only warns.
- A configured service or listening port is a review, since need depends on
  the user's role. Ports routinely left open without authentication warn, and
  the unencrypted Docker API (2375) is critical. Loopback-only listeners pass,
  except on a backdoor port.
- Loaded modules, a tainted kernel, lockdown `none`, Secure Boot off and a
  missing safeguard are reviews. World-readable private system files and
  world-writable system files or folders are critical. A credential file
  other users can read is critical; group-readable warns.
- An absent file passes; every SSH server check passes when `sshd_config`
  does not exist. Checks needing root-only files report unavailable for a
  normal user. Duration limits accept `s`, `m`, `h`, `d`, `w`; 0 fails.
- Persistence patterns are line-based; a temporary-folder path counts only in
  command position, so cleanup jobs are not flagged.

## Boundaries and limits

- The scan reads files, `stat`/`lstat`/`readlink`, `/proc` and `/sys` on a
  background thread. It runs no shell command or external scanner, reads no
  process memory, opens no network connection and uploads nothing. Nothing
  changes unless the user confirms a fix. Fixes run only on Linux.
- Other users' processes and sockets are only partly visible to a normal
  user; the detail suggests `sudo ss -lptn`.
- SSH configuration is read from `/etc/ssh/sshd_config` with included
  `sshd_config.d/*.conf` first (sshd keeps the first value) and stops at the
  first `Match` block; verify with `sshd -T` before acting.
- The package does not replace endpoint protection, patch management,
  backups, incident response or a professional security assessment.

## Code and tests

| Path | Responsibility |
| --- | --- |
| `content/desktop/SecurityScanner.{h,cpp}` | Check catalog, read-only probes, usage evidence, fix scripts, severity sort, filters and summaries. |
| `content/desktop/SecurityExplanations.{h,cpp}` | The plain-language explanation for every check. |
| `content/desktop/ComputerSecurityFeature.{h,cpp}` | Rail page: background scan, filter buttons and category menu, findings table with fix buttons, detail panel, confirmation dialog and `pkexec` runner. |
| `content/desktop/tests/test_security_scanner.cpp` | Fixture-based coverage of every probe family, the usage evidence, the fix script for each fix type, that every check is explained, that passing checks offer no fix, that every fix script parses with `sh -n`, the filters and the summaries. |

`forkmesh-computer-security-tests` runs the scanner against temporary fixture
folders rather than host configuration. The fixture passes its root, proc
root, home folder, user name and `PATH` explicitly to `ScanContext`, so tests
never read the user's real system or home files, and the only process they
start is `sh -n`, which parses a script without running it.
