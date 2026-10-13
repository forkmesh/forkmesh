#include "SecurityExplanations.h"

#include <QHash>

namespace forkmesh::security {
namespace {

const QHash<QString, QString> &portTexts()
{
    static const QHash<QString, QString> texts = {
        {QStringLiteral("tcp:21"), QStringLiteral("FTP transfers files and sends passwords unencrypted.")},
        {QStringLiteral("tcp:22"), QStringLiteral("SSH lets people sign in to this computer remotely. Keep it only if you or your tools connect to this computer from elsewhere.")},
        {QStringLiteral("tcp:23"), QStringLiteral("Telnet is an old remote sign-in service that sends passwords unencrypted. Nothing modern needs it.")},
        {QStringLiteral("tcp:25"), QStringLiteral("SMTP receives email. A desktop computer rarely needs to accept mail from the network.")},
        {QStringLiteral("tcp:53"), QStringLiteral("DNS answers name lookups for other computers. Only routers and name servers need it on the network.")},
        {QStringLiteral("tcp:80"), QStringLiteral("HTTP serves web pages. A development web server often listens here by accident.")},
        {QStringLiteral("tcp:110"), QStringLiteral("POP3 delivers email to mail clients, usually unencrypted.")},
        {QStringLiteral("tcp:111"), QStringLiteral("RPC (rpcbind) helps NFS find its services. It has been abused for traffic amplification and reveals what the computer runs.")},
        {QStringLiteral("tcp:139"), QStringLiteral("NetBIOS session is old Windows file sharing.")},
        {QStringLiteral("tcp:143"), QStringLiteral("IMAP serves email to mail clients, often unencrypted.")},
        {QStringLiteral("tcp:389"), QStringLiteral("LDAP is a directory server, usually only on company servers.")},
        {QStringLiteral("tcp:445"), QStringLiteral("SMB is Windows file sharing (Samba on Linux). It is a frequent target of worms.")},
        {QStringLiteral("tcp:631"), QStringLiteral("IPP shares printers. Printing to your own printer does not need it open to the network.")},
        {QStringLiteral("tcp:873"), QStringLiteral("Rsync daemon serves files, often without a password.")},
        {QStringLiteral("tcp:1433"), QStringLiteral("Microsoft SQL Server database.")},
        {QStringLiteral("tcp:1883"), QStringLiteral("MQTT is a message broker for smart-home and IoT devices, often without a password.")},
        {QStringLiteral("tcp:2049"), QStringLiteral("NFS shares folders with other computers.")},
        {QStringLiteral("tcp:2375"), QStringLiteral("The Docker API without TLS. Anyone who can reach it can start a container with full control of this computer.")},
        {QStringLiteral("tcp:2376"), QStringLiteral("The Docker API with TLS. It is safe only if client certificates are required.")},
        {QStringLiteral("tcp:2379"), QStringLiteral("etcd stores Kubernetes cluster secrets and often runs without authentication.")},
        {QStringLiteral("tcp:3306"), QStringLiteral("MySQL or MariaDB database. Databases should listen only on this computer unless other machines need them.")},
        {QStringLiteral("tcp:3389"), QStringLiteral("Remote Desktop (xrdp) shows this computer's desktop to other computers.")},
        {QStringLiteral("tcp:5432"), QStringLiteral("PostgreSQL database. Databases should listen only on this computer unless other machines need them.")},
        {QStringLiteral("tcp:5601"), QStringLiteral("Kibana dashboards, often without a password.")},
        {QStringLiteral("tcp:5900"), QStringLiteral("VNC shares the screen, often with a weak or no password.")},
        {QStringLiteral("tcp:5984"), QStringLiteral("CouchDB database, which older versions leave open to everyone.")},
        {QStringLiteral("tcp:6000"), QStringLiteral("X11 lets other computers draw windows and read keystrokes on this display.")},
        {QStringLiteral("tcp:6379"), QStringLiteral("Redis database, which has no password by default; attackers use an open Redis to plant SSH keys and miners.")},
        {QStringLiteral("tcp:6443"), QStringLiteral("Kubernetes API server.")},
        {QStringLiteral("tcp:8888"), QStringLiteral("Jupyter notebooks run any code they are given; an open Jupyter is remote code execution.")},
        {QStringLiteral("tcp:9200"), QStringLiteral("Elasticsearch, which older versions leave open without a password.")},
        {QStringLiteral("tcp:10000"), QStringLiteral("Webmin manages the whole computer from a web page.")},
        {QStringLiteral("tcp:10250"), QStringLiteral("The Kubernetes kubelet API can run commands in containers.")},
        {QStringLiteral("tcp:11211"), QStringLiteral("Memcached has no authentication and leaks cached data.")},
        {QStringLiteral("tcp:27017"), QStringLiteral("MongoDB database; many were wiped and ransomed after being left open.")},
        {QStringLiteral("udp:69"), QStringLiteral("TFTP transfers files with no password at all.")},
        {QStringLiteral("udp:111"), QStringLiteral("RPC over UDP is abused for traffic amplification attacks.")},
        {QStringLiteral("udp:123"), QStringLiteral("An NTP server gives the time to other computers. Keeping your own clock right does not need it.")},
        {QStringLiteral("udp:137"), QStringLiteral("NetBIOS name service announces this computer's name on old Windows networks.")},
        {QStringLiteral("udp:161"), QStringLiteral("SNMP reports system details and, with the default \"public\" community, can leak a lot.")},
        {QStringLiteral("udp:1900"), QStringLiteral("SSDP/UPnP lets devices discover each other and can ask routers to open ports.")},
        {QStringLiteral("udp:5353"), QStringLiteral("mDNS (Avahi/Bonjour) announces this computer and its services on the local network. Handy for printers and casting, but it reveals what you run.")},
        {QStringLiteral("udp:5355"), QStringLiteral("LLMNR answers name lookups from anyone on the network; attackers spoof it to capture passwords.")},
        {QStringLiteral("udp:11211"), QStringLiteral("Memcached over UDP was used for the largest traffic amplification attacks on record.")},
        {QStringLiteral("backdoor:4444"), QStringLiteral("Port 4444 is the default for Metasploit and many reverse shells.")},
        {QStringLiteral("backdoor:31337"), QStringLiteral("Port 31337 (\"elite\") is a classic backdoor port.")},
        {QStringLiteral("backdoor:1337"), QStringLiteral("Port 1337 (\"leet\") is a common backdoor and test-shell port.")},
        {QStringLiteral("backdoor:12345"), QStringLiteral("Port 12345 is used by the NetBus trojan and many copies of it.")},
        {QStringLiteral("backdoor:6667"), QStringLiteral("Port 6667 is IRC, which botnets use to receive commands.")},
        {QStringLiteral("backdoor:5555"), QStringLiteral("Port 5555 is Android Debug Bridge, which gives full control of a device; botnets scan for it.")},
        {QStringLiteral("backdoor:54321"), QStringLiteral("Port 54321 is a common bind-shell port.")},
    };
    return texts;
}

const QHash<QString, QString> &sysctlTexts()
{
    static const QHash<QString, QString> texts = {
        {QStringLiteral("kernel.kptr_restrict"), QStringLiteral("Hides kernel memory addresses from normal programs, which makes kernel exploits harder to aim.")},
        {QStringLiteral("kernel.dmesg_restrict"), QStringLiteral("Stops normal users reading the kernel log, which can leak addresses and hardware details.")},
        {QStringLiteral("kernel.yama.ptrace_scope"), QStringLiteral("Stops a program from reading the memory of other programs you run, so malware cannot lift passwords out of your browser or SSH agent.")},
        {QStringLiteral("kernel.unprivileged_bpf_disabled"), QStringLiteral("Stops normal users loading BPF programs into the kernel, a frequent source of privilege-escalation bugs.")},
        {QStringLiteral("kernel.randomize_va_space"), QStringLiteral("Address space layout randomisation places programs at random addresses so exploits cannot guess where code lives.")},
        {QStringLiteral("fs.protected_hardlinks"), QStringLiteral("Stops users creating hard links to files they do not own, a trick for attacking root programs.")},
        {QStringLiteral("fs.protected_symlinks"), QStringLiteral("Stops root programs following another user's symbolic links in shared folders like /tmp.")},
        {QStringLiteral("fs.protected_fifos"), QStringLiteral("Protects named pipes in shared folders from being hijacked.")},
        {QStringLiteral("fs.protected_regular"), QStringLiteral("Protects regular files in shared folders from being hijacked.")},
        {QStringLiteral("net.ipv4.ip_forward"), QStringLiteral("Routes traffic between networks like a router. Docker, virtual machines and VPN servers need it; other computers do not.")},
        {QStringLiteral("net.ipv4.conf.all.send_redirects"), QStringLiteral("Sends ICMP redirects, which only routers need.")},
        {QStringLiteral("net.ipv4.conf.default.send_redirects"), QStringLiteral("Sends ICMP redirects on new interfaces, which only routers need.")},
        {QStringLiteral("net.ipv4.conf.all.accept_redirects"), QStringLiteral("Accepting ICMP redirects lets someone on the network reroute your traffic.")},
        {QStringLiteral("net.ipv4.conf.default.accept_redirects"), QStringLiteral("Accepting ICMP redirects on new interfaces lets someone on the network reroute your traffic.")},
        {QStringLiteral("net.ipv4.conf.all.secure_redirects"), QStringLiteral("Only accepts redirects from your own gateway.")},
        {QStringLiteral("net.ipv4.conf.default.secure_redirects"), QStringLiteral("Only accepts redirects from your own gateway on new interfaces.")},
        {QStringLiteral("net.ipv4.conf.all.rp_filter"), QStringLiteral("Drops packets with forged source addresses.")},
        {QStringLiteral("net.ipv4.conf.default.rp_filter"), QStringLiteral("Drops packets with forged source addresses on new interfaces.")},
        {QStringLiteral("net.ipv4.conf.all.log_martians"), QStringLiteral("Logs packets with impossible addresses, which helps notice spoofing.")},
        {QStringLiteral("net.ipv4.conf.default.log_martians"), QStringLiteral("Logs packets with impossible addresses on new interfaces.")},
        {QStringLiteral("net.ipv4.tcp_syncookies"), QStringLiteral("Keeps the computer reachable during a SYN flood attack.")},
        {QStringLiteral("net.ipv4.icmp_echo_ignore_broadcasts"), QStringLiteral("Ignores broadcast pings used in smurf amplification attacks.")},
        {QStringLiteral("net.ipv4.icmp_ignore_bogus_error_responses"), QStringLiteral("Ignores malformed ICMP errors instead of logging them.")},
        {QStringLiteral("net.ipv6.conf.all.accept_redirects"), QStringLiteral("Accepting IPv6 redirects lets someone on the network reroute your traffic.")},
        {QStringLiteral("net.ipv6.conf.default.accept_redirects"), QStringLiteral("Accepting IPv6 redirects on new interfaces lets someone reroute your traffic.")},
        {QStringLiteral("net.ipv6.conf.all.forwarding"), QStringLiteral("Routes IPv6 traffic like a router. Only routers, containers and VPN servers need it.")},
        {QStringLiteral("net.ipv6.conf.default.forwarding"), QStringLiteral("Routes IPv6 traffic on new interfaces like a router.")},
        {QStringLiteral("net.ipv6.conf.default.router_solicitations"), QStringLiteral("Asks for router advertisements on new interfaces.")},
        {QStringLiteral("net.ipv4.conf.all.accept_source_route"), QStringLiteral("Source routing lets the sender choose the path, which bypasses firewalls.")},
        {QStringLiteral("net.ipv4.conf.default.accept_source_route"), QStringLiteral("Source routing on new interfaces lets the sender bypass firewalls.")},
        {QStringLiteral("net.ipv6.conf.all.accept_source_route"), QStringLiteral("IPv6 source routing lets the sender bypass firewalls.")},
        {QStringLiteral("net.ipv6.conf.default.accept_source_route"), QStringLiteral("IPv6 source routing on new interfaces lets the sender bypass firewalls.")},
        {QStringLiteral("net.ipv4.tcp_rfc1337"), QStringLiteral("Protects against TIME-WAIT assassination of TCP connections.")},
        {QStringLiteral("net.core.bpf_jit_harden"), QStringLiteral("Hardens the BPF compiler against JIT-spraying exploits.")},
        {QStringLiteral("kernel.kexec_load_disabled"), QStringLiteral("Stops anyone replacing the running kernel without a reboot. Once turned on it stays on until the next restart.")},
        {QStringLiteral("kernel.perf_event_paranoid"), QStringLiteral("Limits performance counters that can leak information across programs. Profilers such as perf need it relaxed.")},
        {QStringLiteral("fs.suid_dumpable"), QStringLiteral("Stops privileged programs writing memory dumps that can contain password hashes.")},
        {QStringLiteral("dev.tty.ldisc_autoload"), QStringLiteral("Stops normal users loading old terminal drivers with known bugs.")},
        {QStringLiteral("dev.tty.legacy_tiocsti"), QStringLiteral("Stops a program typing commands into your terminal, a known way to escape sandboxes and su sessions.")},
        {QStringLiteral("vm.unprivileged_userfaultfd"), QStringLiteral("Stops normal users using userfaultfd, which makes kernel race-condition exploits reliable.")},
        {QStringLiteral("vm.mmap_min_addr"), QStringLiteral("Stops programs mapping memory at address zero, which turns kernel null-pointer bugs into exploits.")},
    };
    return texts;
}

const QHash<QString, QString> &moduleTexts()
{
    static const QHash<QString, QString> texts = {
        {QStringLiteral("cramfs"), QStringLiteral("cramfs is a read-only file system for old embedded devices.")},
        {QStringLiteral("freevxfs"), QStringLiteral("freevxfs reads Veritas file systems from HP-UX.")},
        {QStringLiteral("jffs2"), QStringLiteral("jffs2 is a file system for raw flash chips.")},
        {QStringLiteral("hfs"), QStringLiteral("hfs reads old Mac disks.")},
        {QStringLiteral("hfsplus"), QStringLiteral("hfsplus reads Mac disks formatted before APFS.")},
        {QStringLiteral("udf"), QStringLiteral("udf reads DVDs and Blu-ray discs.")},
        {QStringLiteral("dccp"), QStringLiteral("DCCP is a rarely used network protocol with a history of kernel bugs.")},
        {QStringLiteral("sctp"), QStringLiteral("SCTP is a telecom network protocol with a history of kernel bugs.")},
        {QStringLiteral("rds"), QStringLiteral("RDS is a cluster network protocol with a history of kernel bugs.")},
        {QStringLiteral("tipc"), QStringLiteral("TIPC is a cluster network protocol with a history of kernel bugs.")},
        {QStringLiteral("firewire_core"), QStringLiteral("FireWire lets a plugged-in device read and write memory directly (DMA attacks).")},
        {QStringLiteral("n_hdlc"), QStringLiteral("n_hdlc is an old serial line driver with known privilege-escalation bugs.")},
    };
    return texts;
}

const QHash<QString, QString> &sshTexts()
{
    static const QHash<QString, QString> texts = {
        {QStringLiteral("PermitRootLogin"), QStringLiteral("Allows signing in directly as root over SSH. Signing in as yourself and then using sudo leaves a trail and blocks password guessing against root.")},
        {QStringLiteral("PasswordAuthentication"), QStringLiteral("Allows SSH sign-in with a password, which bots guess around the clock. SSH keys cannot be guessed.")},
        {QStringLiteral("PermitEmptyPasswords"), QStringLiteral("Allows SSH sign-in to accounts without a password.")},
        {QStringLiteral("X11Forwarding"), QStringLiteral("Lets remote programs open windows on your screen through SSH; a compromised server can then read your keystrokes.")},
        {QStringLiteral("AllowTcpForwarding"), QStringLiteral("Lets SSH users tunnel network traffic through this computer, past your firewall.")},
        {QStringLiteral("PermitTunnel"), QStringLiteral("Lets SSH users create VPN-style tunnels into your network.")},
        {QStringLiteral("IgnoreRhosts"), QStringLiteral("Ignores old .rhosts trust files that allow sign-in without a password.")},
        {QStringLiteral("HostbasedAuthentication"), QStringLiteral("Trusts other computers instead of users. If one trusted computer is compromised, so is this one.")},
        {QStringLiteral("UsePAM"), QStringLiteral("Uses the system's sign-in rules (lockouts, two-factor, account expiry) for SSH.")},
        {QStringLiteral("LogLevel"), QStringLiteral("VERBOSE logs which key signed in, so you can trace a stolen key.")},
        {QStringLiteral("PermitUserEnvironment"), QStringLiteral("Lets users set environment variables such as LD_PRELOAD for their SSH session, which can bypass restrictions.")},
        {QStringLiteral("AllowAgentForwarding"), QStringLiteral("Forwards your SSH agent to this computer; anyone with root here can use your keys while you are connected.")},
        {QStringLiteral("GatewayPorts"), QStringLiteral("Lets forwarded ports listen on every network interface instead of only on this computer.")},
        {QStringLiteral("KbdInteractiveAuthentication"), QStringLiteral("Another way of asking for a password over SSH. Turn it off together with PasswordAuthentication unless you use one-time codes.")},
        {QStringLiteral("StrictModes"), QStringLiteral("Refuses keys in folders other users can change.")},
        {QStringLiteral("MaxAuthTries"), QStringLiteral("Limits password or key attempts per connection, slowing down guessing.")},
        {QStringLiteral("LoginGraceTime"), QStringLiteral("Limits how long an unauthenticated connection may stay open, so idle attackers cannot tie up the server.")},
        {QStringLiteral("MaxSessions"), QStringLiteral("Limits how many sessions one connection can open.")},
    };
    return texts;
}

const QHash<QString, QString> &serviceTexts()
{
    static const QHash<QString, QString> texts = {
        {QStringLiteral("/etc/xinetd.conf"), QStringLiteral("xinetd starts old network services on demand, often including telnet or TFTP.")},
        {QStringLiteral("/etc/inetd.conf"), QStringLiteral("inetd starts old network services on demand, often including telnet.")},
        {QStringLiteral("/etc/exports"), QStringLiteral("Lists folders shared with other computers over NFS.")},
        {QStringLiteral("/etc/samba/smb.conf"), QStringLiteral("Samba shares folders and printers with Windows computers.")},
        {QStringLiteral("/etc/vsftpd.conf"), QStringLiteral("vsftpd is an FTP server.")},
        {QStringLiteral("/etc/apache2/apache2.conf"), QStringLiteral("Apache is a web server.")},
        {QStringLiteral("/etc/nginx/nginx.conf"), QStringLiteral("nginx is a web server.")},
        {QStringLiteral("/etc/docker/daemon.json"), QStringLiteral("Docker runs containers. Anyone who controls Docker controls the computer.")},
        {QStringLiteral("/etc/cups/cupsd.conf"), QStringLiteral("CUPS runs printing. It is needed to print, but does not need to share printers with the network.")},
        {QStringLiteral("/etc/postfix/main.cf"), QStringLiteral("Postfix sends and receives email.")},
        {QStringLiteral("/etc/bind/named.conf"), QStringLiteral("BIND is a DNS server.")},
        {QStringLiteral("/etc/rsyncd.conf"), QStringLiteral("The rsync daemon serves files to other computers.")},
        {QStringLiteral("/etc/openvpn/server.conf"), QStringLiteral("An OpenVPN server lets other computers join your network.")},
        {QStringLiteral("/etc/wireguard/wg0.conf"), QStringLiteral("A WireGuard VPN connection or server.")},
        {QStringLiteral("/etc/cron.deny"), QStringLiteral("Lists users who may not schedule jobs; everyone else may. /etc/cron.allow is the stricter way round.")},
        {QStringLiteral("/etc/at.deny"), QStringLiteral("Lists users who may not schedule one-off jobs; everyone else may. /etc/at.allow is the stricter way round.")},
        {QStringLiteral("/etc/nfs.conf"), QStringLiteral("Configures the NFS file-sharing server.")},
        {QStringLiteral("/etc/libvirt/libvirtd.conf"), QStringLiteral("libvirt runs virtual machines.")},
        {QStringLiteral("/etc/snmp/snmpd.conf"), QStringLiteral("snmpd reports system details to monitoring tools.")},
        {QStringLiteral("/etc/tor/torrc"), QStringLiteral("Tor routes traffic anonymously; a relay also carries other people's traffic.")},
        {QStringLiteral("/etc/mosquitto/mosquitto.conf"), QStringLiteral("Mosquitto is an MQTT broker for smart-home devices.")},
        {QStringLiteral("/etc/kubernetes/kubelet.conf"), QStringLiteral("This computer is a Kubernetes node.")},
        {QStringLiteral("/etc/iscsi/iscsid.conf"), QStringLiteral("iSCSI connects to network disks.")},
        {QStringLiteral("/etc/tftp.conf"), QStringLiteral("A TFTP server serves files with no password.")},
        {QStringLiteral("/etc/squid/squid.conf"), QStringLiteral("Squid is a web proxy; an open proxy gets abused by spammers.")},
        {QStringLiteral("/etc/avahi/avahi-daemon.conf"), QStringLiteral("Avahi announces this computer on the local network (mDNS).")},
        {QStringLiteral("/etc/dnsmasq.conf"), QStringLiteral("dnsmasq is a small DNS and DHCP server. libvirt and some VPNs run their own copy.")},
        {QStringLiteral("/etc/xrdp/xrdp.ini"), QStringLiteral("xrdp is a Remote Desktop server.")},
        {QStringLiteral("/etc/redis/redis.conf"), QStringLiteral("Redis is an in-memory database.")},
        {QStringLiteral("/etc/mongod.conf"), QStringLiteral("MongoDB is a database server.")},
        {QStringLiteral("/etc/memcached.conf"), QStringLiteral("Memcached is a cache server without authentication.")},
        {QStringLiteral("/etc/hosts.equiv"), QStringLiteral("Lets users from listed computers sign in without a password (rsh/rlogin trust). Nothing modern needs it.")},
        {QStringLiteral("/root/.rhosts"), QStringLiteral("Lets listed computers sign in as root without a password. Nothing modern needs it.")},
        {QStringLiteral("/etc/ssh/ssh_host_dsa_key"), QStringLiteral("DSA host keys are too weak and were removed from OpenSSH.")},
        {QStringLiteral("/etc/ssh/sshrc"), QStringLiteral("Runs on every SSH sign-in. It is rarely used and is a place attackers hide commands.")},
    };
    return texts;
}

const QHash<QString, QString> &secretTexts()
{
    static const QHash<QString, QString> texts = {
        {QStringLiteral(".ssh"), QStringLiteral("Your SSH folder holds your keys and the list of keys that can sign in as you.")},
        {QStringLiteral(".ssh/id_rsa"), QStringLiteral("Your RSA SSH private key signs you in to servers and Git hosts.")},
        {QStringLiteral(".ssh/id_ecdsa"), QStringLiteral("Your ECDSA SSH private key signs you in to servers and Git hosts.")},
        {QStringLiteral(".ssh/id_ed25519"), QStringLiteral("Your Ed25519 SSH private key signs you in to servers and Git hosts.")},
        {QStringLiteral(".ssh/id_dsa"), QStringLiteral("Your DSA SSH private key; DSA is obsolete and should be replaced.")},
        {QStringLiteral(".gnupg"), QStringLiteral("Your GnuPG folder holds your signing and encryption keys.")},
        {QStringLiteral(".git-credentials"), QStringLiteral("Git's \"store\" helper keeps your Git host tokens here in plain text.")},
        {QStringLiteral(".netrc"), QStringLiteral(".netrc stores FTP and HTTP passwords in plain text for curl, Git and others.")},
        {QStringLiteral(".aws/credentials"), QStringLiteral("AWS access keys; anyone who reads them can use your cloud account.")},
        {QStringLiteral(".pgpass"), QStringLiteral("PostgreSQL passwords in plain text.")},
        {QStringLiteral(".docker/config.json"), QStringLiteral("Docker registry logins, often stored as plain base64.")},
        {QStringLiteral(".kube/config"), QStringLiteral("Kubernetes cluster credentials.")},
        {QStringLiteral(".config/gh/hosts.yml"), QStringLiteral("GitHub CLI tokens.")},
        {QStringLiteral(".pypirc"), QStringLiteral("Python package index upload passwords or tokens.")},
        {QStringLiteral(".npmrc"), QStringLiteral("npm registry tokens that can publish packages in your name.")},
    };
    return texts;
}

QString privateFileText(const QString &target)
{
    if (target.startsWith(QStringLiteral("/etc/ssh/ssh_host_")))
        return QStringLiteral("This is the SSH server's private host key. Anyone who reads it can impersonate this computer to SSH clients.");
    if (target.contains(QStringLiteral("shadow")))
        return QStringLiteral("This file holds password hashes. Anyone who reads it can try to crack every password offline.");
    if (target == QStringLiteral("/etc/sudoers"))
        return QStringLiteral("This file decides who may run commands as root.");
    if (target == QStringLiteral("/root"))
        return QStringLiteral("Root's home folder often holds keys, scripts and shell history.");
    if (target == QStringLiteral("/etc/krb5.keytab"))
        return QStringLiteral("The Kerberos keytab lets this computer prove its identity to the domain.");
    if (target == QStringLiteral("/boot/grub/grub.cfg"))
        return QStringLiteral("The boot menu can hold a boot password hash and kernel options.");
    return QStringLiteral("This file holds security settings that other users should not read.");
}

QString safeguardText(const QString &id)
{
    if (id.endsWith(QStringLiteral("automatic-updates")))
        return QStringLiteral("Installs security fixes automatically, closing known holes within a day instead of whenever you remember.");
    if (id.endsWith(QStringLiteral("audit-daemon")))
        return QStringLiteral("auditd records who changed what, which is how you find out what happened after an incident.");
    if (id.endsWith(QStringLiteral("brute-force-protection")))
        return QStringLiteral("fail2ban or sshguard blocks addresses that keep guessing passwords.");
    if (id.endsWith(QStringLiteral("file-integrity")))
        return QStringLiteral("AIDE records fingerprints of system files so you can spot files an attacker replaced.");
    if (id.endsWith(QStringLiteral("usb-device-policy")))
        return QStringLiteral("USBGuard blocks unknown USB devices, such as a stick that pretends to be a keyboard.");
    if (id.endsWith(QStringLiteral("disk-encryption")))
        return QStringLiteral("Disk encryption keeps your files unreadable if the computer or its disk is stolen.");
    return {};
}

}

QString explainCheck(const CheckDefinition &check)
{
    const QString &probe = check.probe;
    const QString &target = check.target;
    if (probe == QStringLiteral("firewall"))
        return QStringLiteral("A firewall decides which network connections may reach this computer. Without one, every service that listens on the network can be reached by anyone who can reach the computer.");
    if (probe == QStringLiteral("firewall-policy"))
        return QStringLiteral("A firewall that allows incoming connections by default only blocks what you remembered to block. Denying by default means a new service stays private until you open it.");
    if (probe == QStringLiteral("rootkit") && check.category == QStringLiteral("Malware"))
        return QStringLiteral("%1 is a file this malware is known to drop. It usually means the computer runs a cryptocurrency miner or a backdoor; it can also be a researcher's sample.").arg(target);
    if (probe == QStringLiteral("rootkit"))
        return QStringLiteral("A rootkit hides an attacker's files, processes and access. %1 is a path this rootkit is known to create; a normal system never has it.").arg(target);
    if (probe == QStringLiteral("preload"))
        return QStringLiteral("/etc/ld.so.preload loads a library into every program that starts. Rootkits such as Azazel and Jynx use it to hide themselves; ordinary systems leave it empty.");
    if (probe == QStringLiteral("hidden-module"))
        return QStringLiteral("Kernel rootkits such as Diamorphine remove themselves from the module list. A module that the kernel's sysfs still knows about but /proc/modules does not list is being hidden.");
    if (probe == QStringLiteral("malware-process"))
        return QStringLiteral("Looks for programs named like well-known cryptocurrency miners and Linux malware (XMRig, Kinsing, Sysrv and others). Miners slow the computer, run up power bills and usually arrive through another hole.");
    if (probe == QStringLiteral("masquerade"))
        return QStringLiteral("Kernel threads appear as [name] in process lists. Malware renames itself to look like one, such as [kworker/0:1], to hide in plain sight. Real kernel threads have no command line.");
    if (probe == QStringLiteral("temp-process"))
        return QStringLiteral("Programs normally run from /usr or your home folder. Malware runs from /tmp, /var/tmp or /dev/shm because any user can write there. Some developer tools also do this, so check what each program is.");
    if (probe == QStringLiteral("deleted-process"))
        return QStringLiteral("A running program whose file was deleted. Usually the program was updated and simply needs a restart; malware also deletes itself after starting, or runs straight from memory (memfd) to leave no file.");
    if (probe == QStringLiteral("temp-executables"))
        return QStringLiteral("%1 is a shared temporary folder. Downloaded malware is commonly saved and run from there; programs you installed do not live there.").arg(target);
    if (probe == QStringLiteral("backdoor-port"))
        return portTexts().value(QStringLiteral("backdoor:") + target)
               + QStringLiteral(" Legitimate software rarely uses it, so a listener here deserves a close look.");
    if (probe == QStringLiteral("port") || probe == QStringLiteral("udp-port")) {
        const QString key = (probe == QStringLiteral("port") ? QStringLiteral("tcp:") : QStringLiteral("udp:")) + target;
        return portTexts().value(key)
               + QStringLiteral(" A service that listens only on 127.0.0.1 is private to this computer; one listening on a network address can be reached from other computers unless a firewall blocks it.");
    }
    if (probe == QStringLiteral("other-ports"))
        return QStringLiteral("Lists every other %1 port that a program on this computer has opened to the network. Each one is a door other computers can knock on; close the ones you do not recognise.").arg(target.toUpper());
    if (probe == QStringLiteral("promiscuous"))
        return QStringLiteral("A network interface in promiscuous mode receives every packet on the network, not only its own. Packet sniffers such as Wireshark and tcpdump do this, and so does malware that steals passwords from network traffic.");
    if (probe == QStringLiteral("sysctl"))
        return sysctlTexts().value(target) + QStringLiteral(" This is a kernel setting (sysctl).");
    if (probe == QStringLiteral("mac"))
        return QStringLiteral("AppArmor and SELinux confine programs to what they need, so a hacked browser or server cannot read your whole home folder.");
    if (probe == QStringLiteral("lockdown"))
        return QStringLiteral("Kernel lockdown stops even root from modifying the running kernel, so malware with root cannot install a kernel rootkit. It is usually turned on with Secure Boot.");
    if (probe == QStringLiteral("tainted"))
        return QStringLiteral("The kernel marks itself tainted when an unsigned or out-of-tree module loads or after a serious error. Graphics drivers such as NVIDIA's taint it legitimately; an unexpected taint can mean a rootkit module.");
    if (probe == QStringLiteral("secure-boot"))
        return QStringLiteral("Secure Boot only starts a signed bootloader and kernel, which blocks bootkits that load before the operating system.");
    if (probe == QStringLiteral("module"))
        return moduleTexts().value(target)
               + QStringLiteral(" Every loaded module is kernel code an attacker can try to exploit; blocking modules you do not use shrinks that surface.");
    if (probe == QStringLiteral("account-uid-zero"))
        return QStringLiteral("User ID 0 is root, whatever the account is called. A second account with ID 0 is a classic hidden backdoor.");
    if (probe == QStringLiteral("account-empty-password"))
        return QStringLiteral("An account with an empty password can be used by anyone, locally and sometimes over the network.");
    if (probe == QStringLiteral("account-shadowed"))
        return QStringLiteral("/etc/passwd is readable by everyone. Password hashes belong in /etc/shadow, which only root can read.");
    if (probe == QStringLiteral("account-duplicate-uid"))
        return QStringLiteral("Accounts that share a user ID share every file and permission, so one can act as the other.");
    if (probe == QStringLiteral("account-legacy-entries"))
        return QStringLiteral("Lines starting with \"+\" pull accounts from an old NIS directory and, if NIS is gone, can allow passwordless sign-in.");
    if (probe == QStringLiteral("docker-group"))
        return QStringLiteral("Members of the docker group can start a container that mounts the whole disk, which makes them root without a password.");
    if (probe == QStringLiteral("sudoers"))
        return QStringLiteral("sudo rules decide who may run commands as root. NOPASSWD rules let any program running as that user become root silently, without the password prompt that would warn you.");
    if (probe == QStringLiteral("autologin"))
        return QStringLiteral("Automatic sign-in opens your desktop for anyone who turns the computer on, including your browser sessions and keys.");
    if (probe == QStringLiteral("config") || probe == QStringLiteral("config-max")) {
        const QString key = check.expected.section(QLatin1Char('='), 0, 0);
        if (target == QStringLiteral("/etc/login.defs")) {
            if (key == QStringLiteral("ENCRYPT_METHOD"))
                return QStringLiteral("The hash used for new passwords. SHA-512 and yescrypt are slow to crack; MD5 and DES are not.");
            if (key == QStringLiteral("UMASK"))
                return QStringLiteral("The default permissions for new files. 022 makes new files readable by every user; 027 or 077 keeps them private.");
            return QStringLiteral("How long a password may be used before it must be changed.");
        }
        return sshTexts().value(key) + QStringLiteral(" This is an SSH server setting; it only matters if the SSH server runs.");
    }
    if (probe == QStringLiteral("ssh-crypto"))
        return QStringLiteral("Old SSH ciphers and hashes (CBC modes, arcfour, MD5, SHA-1, DSA) have known weaknesses. Current OpenSSH defaults are strong, so the safest setting is usually no override at all.");
    if (probe == QStringLiteral("private-file"))
        return privateFileText(target) + QStringLiteral(" Only root should be able to read it.");
    if (probe == QStringLiteral("world-writable"))
        return QStringLiteral("If any user can change %1, any program running as any user can make root run its code or redirect the system.").arg(target);
    if (probe == QStringLiteral("etc-writable"))
        return QStringLiteral("Files in /etc configure the whole system. Any of them that every user can change is a way to take over the computer.");
    if (probe == QStringLiteral("docker-socket"))
        return QStringLiteral("The Docker socket controls the Docker daemon, which runs as root. Whoever can write to it is effectively root.");
    if (probe == QStringLiteral("path-variable"))
        return QStringLiteral("PATH is the list of folders searched when you type a command. A \".\" entry or a folder anyone can write to lets another user plant a fake \"ls\" or \"sudo\" that you then run.");
    if (probe == QStringLiteral("home-folder"))
        return QStringLiteral("If other accounts on this computer (including service accounts like a web server) can open your home folder, they can read documents, browser profiles and keys that are not locked down individually.");
    if (probe == QStringLiteral("home-private-file"))
        return secretTexts().value(target) + QStringLiteral(" Only you should be able to read it; other accounts on this computer, including compromised services, should not.");
    if (probe == QStringLiteral("ssh-key-passphrase"))
        return QStringLiteral("An SSH private key without a passphrase works for anyone who copies the file. A passphrase (with ssh-agent so you type it once) makes a stolen key useless. Add one with: ssh-keygen -p -f <key file>.");
    if (probe == QStringLiteral("authorized-keys"))
        return QStringLiteral("authorized_keys lists every SSH key that can sign in as you without a password. Attackers add their own key here to keep access, so remove any key you do not recognise.");
    if (probe == QStringLiteral("sticky-dir"))
        return QStringLiteral("The sticky bit on a shared folder stops users deleting or replacing each other's files.");
    if (probe == QStringLiteral("setuid"))
        return QStringLiteral("A set-user-ID program runs as its owner (usually root) for whoever starts it. Shells, interpreters and tools like find or vim can then run any command as root.");
    if (probe == QStringLiteral("mount-option")) {
        if (check.expected == QStringLiteral("nodev"))
            return QStringLiteral("nodev stops device files in %1 from working, so users cannot create a raw disk device there.").arg(target);
        if (check.expected == QStringLiteral("nosuid"))
            return QStringLiteral("nosuid stops set-user-ID programs in %1 from gaining privileges, so a planted root shell does nothing.").arg(target);
        return QStringLiteral("noexec stops programs in %1 from running at all, which breaks many malware droppers.").arg(target);
    }
    if (probe == QStringLiteral("present") || probe == QStringLiteral("entries"))
        return safeguardText(check.id);
    if (probe == QStringLiteral("updates-pending"))
        return QStringLiteral("Security updates fix holes that attackers already know about. Most break-ins use bugs that had a fix available.");
    if (probe == QStringLiteral("reboot-required"))
        return QStringLiteral("Some updates, such as a new kernel or C library, only take effect after a restart. Until then the old, vulnerable code is still running.");
    if (probe == QStringLiteral("persist-cron"))
        return QStringLiteral("Scheduled jobs are the most common way malware restarts itself. A job that downloads a script and pipes it into a shell, opens a reverse shell or runs from /tmp is almost never legitimate.");
    if (probe == QStringLiteral("persist-systemd"))
        return QStringLiteral("Services in /etc/systemd/system and ~/.config/systemd/user start automatically. Malware installs one that runs a downloaded file from a temporary folder.");
    if (probe == QStringLiteral("persist-autostart"))
        return QStringLiteral("Desktop autostart entries run when you sign in. Malware adds one that runs a program from a temporary folder or downloads a script.");
    if (probe == QStringLiteral("persist-shell"))
        return QStringLiteral("Shell startup files run every time a terminal opens. Attackers add LD_PRELOAD exports, a fake \"sudo\" alias to steal your password, or download commands.");
    if (probe == QStringLiteral("persist-rc-local"))
        return QStringLiteral("/etc/rc.local runs as root at every boot. Modern systems rarely use it, which makes it a quiet place to hide a command.");
    if (probe == QStringLiteral("persist-ld-config"))
        return QStringLiteral("These files tell every program where to find shared libraries. A folder in /tmp or a home folder there lets a user replace libraries that root programs load.");
    if (probe == QStringLiteral("home-exists"))
        return QStringLiteral("~/.ssh/rc runs every time someone signs in to your account over SSH. Few people use it, which makes it a quiet place to hide a command.");
    if (probe == QStringLiteral("nfs-exports"))
        return QStringLiteral("no_root_squash lets root on a client computer act as root on your shared folder, for example to plant a set-user-ID root shell.");
    if (probe == QStringLiteral("samba-guest"))
        return QStringLiteral("Guest access lets anyone on the network open the share without a password.");
    if (probe == QStringLiteral("docker-tcp"))
        return QStringLiteral("Docker configured to listen on a TCP address without TLS client certificates gives full control of this computer to anyone who can reach the port.");
    if (probe == QStringLiteral("redis-auth"))
        return QStringLiteral("Redis listening on the network without a password is one of the most common ways Linux servers get a miner installed.");
    if (probe == QStringLiteral("optional"))
        return serviceTexts().value(target, QStringLiteral("A configuration file for a service that can expose this computer."))
               + QStringLiteral(" If you do not use it, turning it off removes something attackers could reach.");
    return {};
}

}
