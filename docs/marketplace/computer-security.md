# Computer Security Checkup package

`marketplace/packages/computer-security` is a bundled desktop package (Market
id `computer-security`) that runs a local security checkup, explains every
result, shows whether the thing it found is actually in use, and offers a
confirmed one-click fix where a safe one exists. It is off by default and
appears in the desktop rail when installed.

## What it checks

The package has more than 300 individually named checks. They cover:

- **Firewall**: a local nftables, UFW or firewalld configuration (UFW counts
  only when `ENABLED=yes`; installed-but-off warns), and UFW's default incoming
  policy.
- **Rootkits**: 21 known rootkit artifact paths (including Ebury, Lion, Ramen
  and SHV4), entries in `/etc/ld.so.preload`, and kernel
  modules hidden from `/proc/modules` (a `/sys/module/<name>/initstate` with no
  matching `/proc/modules` line, which is how Diamorphine-style rootkits hide).
- **Malware**: artifacts of XorDDoS, Kinsing, BPFDoor and XMRig; processes named like known miners and Linux malware; processes whose
  command line poses as a kernel thread (`[kworker/0:1]` with a parent other
  than `kthreadd`); processes running from `/tmp`, `/var/tmp` or `/dev/shm`
  (AppImage `/tmp/.mount_*` and `go run` `/tmp/go-build*` are skipped);
  processes running from a deleted file or from memory only (`memfd`);
  executable files stored in the shared temporary folders; and listeners on
  seven classic backdoor ports (4444, 31337, 1337, 12345, 6667, 5555, 54321).
- **Network exposure**: 35 commonly exposed TCP services and 9 UDP services,
  every *other* port open to the network, and interfaces in promiscuous mode
  (container and VM bridges are skipped).
- **Kernel hardening**: sysctl settings, AppArmor or SELinux, kernel lockdown,
  taint flags and Secure Boot.
- **Kernel modules**: uncommon file systems and network protocols, FireWire.
- **Accounts**: a second UID 0 account, empty passwords, password hashes in
  `/etc/passwd`, shared user IDs, NIS `+` entries, docker group members,
  passwordless `sudo` rules, desktop automatic sign-in (GDM, LightDM, SDDM),
  and `/etc/login.defs` hash method, umask and password lifetime.
- **File permissions**: private system and SSH host-key files, system files
  and folders any user can change (including `/usr/bin`, `/etc/sudoers.d`,
  `/etc/profile.d` and the cron folders), a sweep of `/etc` for world-writable
  files, a Docker socket open to everyone, unsafe `PATH` entries, the home
  folder, and sticky bits.
- **Secrets**: permissions of SSH keys, GnuPG, Git, AWS, Docker, Kubernetes,
  GitHub CLI, PyPI, npm and PostgreSQL credential files in the home folder;
  SSH private keys without a passphrase; and the keys in `authorized_keys`.
- **Privilege escalation**: 30 shells, interpreters and tools that must never be
  set-user-ID.
- **Mounts**: `nodev`, `nosuid` and `noexec` on the shared temporary folders.
- **Remote access**: SSH server options, limits and weak algorithms.
- **Safeguards**: automatic updates, the audit daemon, brute-force protection,
  file integrity monitoring, a USB device policy, an encrypted volume, waiting
  security updates (Ubuntu's `update-notifier` list) and a pending restart.
- **Persistence**: scheduled jobs, custom systemd units, desktop autostart
  entries and shell start-up files that download and run a script, decode
  base64 into a shell, open `/dev/tcp`, attach netcat to a shell, point at a
  mining pool, run a program from a temporary folder, set `LD_PRELOAD` or
  redefine `sudo`; commands in `/etc/rc.local`; library folders in
  `/etc/ld.so.conf*` under `/tmp` or `/home`; and `~/.ssh/rc`.
- **Service exposure**: configuration files for services that commonly expose
  a machine, `rhosts`-style trust files, NFS exports with `no_root_squash`,
  Samba guest shares, Docker's API on TCP, and Redis without a password.

Results are shown as **Pass**, **Review**, **Warning**, **Critical**, or
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

A row of filter buttons shows **All**, **Needs attention** (critical, warning
and review together), **Critical**, **Warnings**, **For review**, **Can fix**,
**Passed** and **Unavailable**, each with its count for the current category
and search. A category menu narrows the list further, and the desktop's shared
search box filters by title, category and result. The summary names the active
filter; an empty result says which filter is empty instead of showing a blank
table.

## Fixes

A finding with a safe automatic fix gets a button in its row and in the detail
panel, plus **Copy command** to run it by hand. Pressing it opens a
confirmation that states what will change, any caution (for example that
stopping SSH ends remote sign-in, that `ip_forward=0` cuts off the Docker
interfaces it found, or that turning SSH passwords off locks out an account
with no key), and the exact command. Cancel is the default button. Fixes
that touch system files run through `pkexec sh -c`, so the operating system asks
for the administrator password; fixes to the user's own files run without it.
After a fix the checkup runs again so the row shows the new state. Passing and
unavailable checks never offer a fix.

The fixes are deliberately conservative:

- Nothing is deleted. Rootkit and malware artifacts, programs in temporary
  folders and malicious cron, systemd and autostart files are **quarantined**:
  moved into `/root/forkmesh-quarantine/<timestamp>/` with every permission
  removed, so they stay available for investigation. `ld.so.preload`,
  `hosts.equiv`, `.rhosts`, `sshrc` and `~/.ssh/rc` are renamed to
  `*.forkmesh-disabled`.
- Kernel settings are applied with `sysctl -w` and saved in
  `/etc/sysctl.d/90-forkmesh-security.conf`; modules are unloaded if unused
  and blocked in `/etc/modprobe.d/forkmesh-security-<name>.conf`.
- SSH settings are written to `sshd_config.d/00-forkmesh-security.conf` when the
  main file includes that folder (00 sorts first and `sshd` keeps the first
  value), or prepended to `sshd_config` otherwise. `sshd_config` is backed up,
  `sshd -t` validates the result, and a rejected change is rolled back before
  SSH is reloaded.
- Services are stopped with `systemctl disable --now` for a fixed list of unit
  names per port or configuration file; if none of them is active or enabled,
  the fix reports that instead of guessing. LLMNR is turned off with a
  `systemd-resolved` drop-in.
- Missing safeguards (UFW or firewalld, unattended-upgrades or dnf-automatic,
  auditd, fail2ban) are installed with `apt-get` or `dnf`, and pending updates
  are installed the same way; the caution says that this downloads packages.
- Remounts last until the next restart; the description says to add the option
  to `/etc/fstab` to keep it.

Some findings have no one-click fix because the right answer depends on the
user: hidden kernel modules, authorised keys, keys without a passphrase, sudo
rules, NFS, Samba, Docker and Redis configuration, weak SSH algorithms, PATH
and Secure Boot. Their detail panel says so and the explanation says what to
look at.

## Severity choices

An artifact matching a rootkit or malware path is critical, not proof of
compromise; it may be a false positive or an analyst's sample. Processes named
like miners, posing as kernel threads or running from a temporary folder are
critical for the same reason: they need a human look. A program running from a
deleted file is only a review, because the common cause is a package update
while the program ran; one running from memory only warns.

A configured service or listening port is a review: whether it is unnecessary
depends on the user's role. Ports that are routinely left open without
authentication are raised: Telnet, Redis, VNC, CouchDB, X11, Jupyter,
Elasticsearch, Kubelet, Memcached, MongoDB and etcd warn, and the unencrypted
Docker API (2375) is critical. A listener bound only to `127.0.0.1` or `::1`
passes, except on a backdoor port, where it is a review.

A loaded kernel module, a tainted kernel, lockdown set to `none`, Secure Boot
off, a folder that is not its own mount, and a missing safeguard are reviews:
each has legitimate causes. A world-readable `/etc/shadow`, `gshadow`,
`sudoers`, SSH host key or Kerberos keytab is critical, as is a world-writable
`/etc/passwd`, `/etc/group`, cron, PAM, systemd, profile, library or binary
folder. A credential file other users can read is critical; one only its group
can read warns.

An absent file to protect passes, and so does every SSH server check when
`sshd_config` does not exist (no SSH server is installed); an `sshd_config` that
exists but cannot be read is still unavailable.

The empty-password and sudo checks read `/etc/shadow` and `/etc/sudoers`, which
only root can read on most systems; run as a normal user they report
unavailable. Duration limits such as `LoginGraceTime` accept the `s`, `m`, `h`,
`d` and `w` suffixes, and a value of 0 (no limit) fails the check.

The persistence patterns are line-based. A temporary-folder path only counts
when it is in command position (after `=`, a shell, `nohup`, a cron time field
or a cron user), so a cleanup job such as `find /tmp/ -mtime +7 -delete` or a
service passing `/tmp/staging` as an argument is not flagged.

## Boundaries and limits

The scan uses Qt file reads, `stat`/`lstat`/`readlink`, and the Linux
`/proc/net/{tcp,tcp6,udp,udp6}` listener tables, `/proc/<pid>/{comm,cmdline,status,exe,fd}`,
`/proc/modules`, `/proc/mounts`, `/proc/sys` and `/sys`. Scanning runs it on a
background thread (`QThread::create`) so the page stays responsive; it takes
under a second on a typical desktop. It does not run a shell command, execute a
rootkit scanner, inspect process memory, open a network connection or upload a
result. Nothing on the system changes unless the user presses a fix and
confirms it. On systems without the relevant Linux files the corresponding
check is unavailable, and fixes run only on Linux (other systems can copy the
command).

Processes and sockets belonging to other users are only partly visible to a
normal user: their names cannot be matched to ports, and their deleted-file
state cannot be read. The detail says so and suggests `sudo ss -lptn`.

SSH configuration is parsed from `/etc/ssh/sshd_config`. When that file has an
`Include` naming `sshd_config.d`, the `*.conf` files there are read first, in
name order, because Debian and Fedora place that `Include` at the top and
`sshd` keeps the first value it sees. Parsing stops at the first `Match`
block, so per-user overrides are not taken as global settings. An `Include`
placed lower in the file, and command-line overrides, can still change the
effective configuration, so administrators should verify it with `sshd -T`
before acting on a warning. The package does not replace endpoint protection,
patch management, backups, incident response, or a professional security
assessment.

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
