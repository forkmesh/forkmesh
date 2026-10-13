#include "SecurityScanner.h"

#include "SecurityExplanations.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMap>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <utility>

#if defined(Q_OS_UNIX)
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace forkmesh::security {
namespace {

constexpr unsigned kOtherExec = 01;
constexpr unsigned kOtherWrite = 02;
constexpr unsigned kOtherRead = 04;
constexpr unsigned kGroupWrite = 020;
constexpr unsigned kGroupRead = 040;
constexpr unsigned kAnyExec = 0111;
constexpr unsigned kSticky = 01000;
constexpr unsigned kSetuid = 04000;

CheckDefinition check(QString id, QString title, QString category, QString probe, QString target = {}, QString expected = {})
{
    return {std::move(id), std::move(title), std::move(category), std::move(target), std::move(expected), std::move(probe), {}};
}

QString inRoot(const QString &root, const QString &path)
{
    if (root == QStringLiteral("/"))
        return path;
    return QDir(root).filePath(path.startsWith(QLatin1Char('/')) ? path.mid(1) : path);
}

QString systemPath(const ScanContext &context, const QString &path)
{
    if (context.root == QStringLiteral("/"))
        return path;
    return QLatin1Char('/') + QDir(context.root).relativeFilePath(path);
}

QString readText(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(file.readAll());
}

QByteArray readBytes(const QString &path, qint64 limit = 1 << 20)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.read(limit);
}

QString configValue(const QString &text, const QString &key)
{
    const QString folded = key.toCaseFolded();
    for (QString line : text.split(QLatin1Char('\n'))) {
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        const QStringList fields = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        if (!fields.isEmpty() && fields.first().toCaseFolded() == QStringLiteral("match"))
            break;
        if (fields.size() >= 2 && fields.first().toCaseFolded() == folded)
            return fields.mid(1).join(QLatin1Char(' '));
    }
    return {};
}

QStringList entries(const QString &text)
{
    QStringList lines;
    for (QString line : text.split(QLatin1Char('\n'))) {
        line = line.trimmed();
        if (!line.isEmpty() && !line.startsWith(QLatin1Char('#')))
            lines << line;
    }
    return lines;
}

QList<QStringList> records(const QString &text)
{
    QList<QStringList> rows;
    for (const QString &line : entries(text))
        rows << line.split(QLatin1Char(':'));
    return rows;
}

QString listed(const QStringList &items, int limit = 6)
{
    if (items.size() <= limit)
        return items.join(QStringLiteral(", "));
    return QStringLiteral("%1 and %2 more").arg(items.mid(0, limit).join(QStringLiteral(", ")), QString::number(items.size() - limit));
}

QString counted(qsizetype count, const QString &one, const QString &many)
{
    return QStringLiteral("%1 %2").arg(QString::number(count), count == 1 ? one : many);
}

FindingState severity(const QString &expected, FindingState fallback)
{
    if (expected == QStringLiteral("critical"))
        return FindingState::Critical;
    if (expected == QStringLiteral("warning"))
        return FindingState::Warning;
    if (expected == QStringLiteral("review"))
        return FindingState::Review;
    return fallback;
}

void set(Finding &finding, FindingState state, const QString &detail)
{
    finding.state = state;
    finding.detail = detail;
}

FixAction makeFix(const QString &label, const QString &description, const QString &script, const QString &caution = {}, bool elevated = true)
{
    FixAction fix;
    fix.label = label;
    fix.description = description;
    fix.script = script;
    fix.caution = caution;
    fix.elevated = elevated;
    return fix;
}

QString quotedList(const QStringList &values)
{
    QStringList quoted;
    for (const QString &value : values)
        quoted << shellQuote(value);
    return quoted.join(QLatin1Char(' '));
}

QString linkTarget(const QString &path)
{
#if defined(Q_OS_UNIX)
    char buffer[4096];
    const ssize_t length = ::readlink(QFile::encodeName(path).constData(), buffer, sizeof buffer - 1);
    if (length <= 0)
        return {};
    return QFile::decodeName(QByteArray(buffer, int(length)));
#else
    Q_UNUSED(path)
    return {};
#endif
}

struct Mode {
    bool known = false;
    unsigned bits = 0;
    bool directory = false;
    bool regular = false;
};

Mode fileMode(const QString &path, bool follow = true)
{
    Mode mode;
#if defined(Q_OS_UNIX)
    struct stat info {};
    const QByteArray name = QFile::encodeName(path);
    if ((follow ? ::stat(name.constData(), &info) : ::lstat(name.constData(), &info)) != 0)
        return mode;
    mode.known = true;
    mode.bits = info.st_mode & 07777;
    mode.directory = S_ISDIR(info.st_mode);
    mode.regular = S_ISREG(info.st_mode);
#else
    Q_UNUSED(path)
    Q_UNUSED(follow)
#endif
    return mode;
}

QStringList filesIn(const QString &directory, const QStringList &patterns = {}, bool recursive = false)
{
    QStringList files;
    QDirIterator it(directory, patterns, QDir::Files | QDir::Hidden | QDir::NoSymLinks,
                    recursive ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags);
    while (it.hasNext() && files.size() < 5000)
        files << it.next();
    files.sort();
    return files;
}

bool loopbackAddress(const QString &address)
{
    if (address.size() == 8)
        return address.endsWith(QStringLiteral("7F"));
    if (address == QStringLiteral("00000000000000000000000001000000"))
        return true;
    return address.startsWith(QStringLiteral("0000000000000000FFFF0000")) && address.endsWith(QStringLiteral("7F"));
}

struct Listeners {
    QHash<int, bool> exposed;
    QHash<int, int> established;
    QHash<int, QStringList> inodes;
    bool available = false;
};

struct Process {
    QString pid;
    QString comm;
    QString cmdline;
    QString ppid;
    QString exe;
};

struct ModuleInfo {
    int users = 0;
    QStringList dependents;
};

struct Snapshot {
    Listeners tcp;
    Listeners udp;
    QHash<QString, ModuleInfo> modules;
    bool modulesAvailable = false;
    QHash<QString, QStringList> mountOptions;
    bool mountsAvailable = false;
    QList<Process> processes;
    QHash<QString, QString> socketOwners;
    QStringList interfaces;
    QSet<int> knownTcp;
    QSet<int> knownUdp;
};

Listeners listeners(const ScanContext &context, const QStringList &tables, const QString &state, bool countConnections)
{
    Listeners result;
    for (const QString &name : tables) {
        const QString text = readText(QDir(context.procRoot).filePath(name));
        if (text.isEmpty())
            continue;
        result.available = true;
        const QStringList lines = text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        for (int i = 1; i < lines.size(); ++i) {
            const QStringList fields = lines.at(i).split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
            if (fields.size() < 4)
                continue;
            bool ok = false;
            const int port = fields.at(1).section(QLatin1Char(':'), 1).toInt(&ok, 16);
            if (!ok)
                continue;
            if (fields.at(3) == state) {
                result.exposed[port] = result.exposed.value(port) || !loopbackAddress(fields.at(1).section(QLatin1Char(':'), 0, 0).toUpper());
                if (fields.size() > 9)
                    result.inodes[port] << fields.at(9);
            } else if (countConnections && fields.at(3) == QStringLiteral("01")) {
                ++result.established[port];
            }
        }
    }
    return result;
}

QList<Process> processes(const ScanContext &context)
{
    static const QRegularExpression digits(QStringLiteral("^\\d+$"));
    static const QRegularExpression parent(QStringLiteral("^PPid:\\s*(\\d+)"), QRegularExpression::MultilineOption);
    QList<Process> result;
    const QDir proc(context.procRoot);
    for (const QString &pid : proc.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (!digits.match(pid).hasMatch())
            continue;
        const QString base = proc.filePath(pid);
        Process process;
        process.pid = pid;
        process.comm = readText(base + QStringLiteral("/comm")).trimmed();
        QByteArray command = readBytes(base + QStringLiteral("/cmdline"), 4096);
        command.replace('\0', ' ');
        process.cmdline = QString::fromUtf8(command).trimmed();
        process.ppid = parent.match(readText(base + QStringLiteral("/status"))).captured(1);
        process.exe = linkTarget(base + QStringLiteral("/exe"));
        result << process;
    }
    return result;
}

QHash<QString, QString> socketOwners(const ScanContext &context, const QList<Process> &running, const QSet<QString> &inodes)
{
    QHash<QString, QString> owners;
    if (inodes.isEmpty())
        return owners;
    for (const Process &process : running) {
        const QDir fds(QDir(context.procRoot).filePath(process.pid + QStringLiteral("/fd")));
        for (const QString &fd : fds.entryList(QDir::AllEntries | QDir::System | QDir::Hidden | QDir::NoDotAndDotDot)) {
            const QString target = linkTarget(fds.filePath(fd));
            if (!target.startsWith(QStringLiteral("socket:[")))
                continue;
            const QString inode = target.mid(8).chopped(1);
            if (inodes.contains(inode) && !owners.contains(inode))
                owners.insert(inode, process.pid);
        }
        if (owners.size() == inodes.size())
            break;
    }
    return owners;
}

Snapshot snapshot(const ScanContext &context)
{
    Snapshot result;
    result.tcp = listeners(context, {QStringLiteral("net/tcp"), QStringLiteral("net/tcp6")}, QStringLiteral("0A"), true);
    result.udp = listeners(context, {QStringLiteral("net/udp"), QStringLiteral("net/udp6")}, QStringLiteral("07"), false);
    const QString modules = readText(QDir(context.procRoot).filePath(QStringLiteral("modules")));
    result.modulesAvailable = !modules.isEmpty();
    for (const QString &line : modules.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const QStringList fields = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (fields.isEmpty())
            continue;
        ModuleInfo info;
        info.users = fields.value(2).toInt();
        for (const QString &name : fields.value(3).split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            if (name != QStringLiteral("-"))
                info.dependents << name;
        }
        result.modules.insert(fields.first(), info);
    }
    const QString mounts = readText(QDir(context.procRoot).filePath(QStringLiteral("mounts")));
    result.mountsAvailable = !mounts.isEmpty();
    for (const QString &line : mounts.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const QStringList fields = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (fields.size() >= 4)
            result.mountOptions.insert(fields.at(1), fields.at(3).split(QLatin1Char(',')));
    }
    result.processes = processes(context);
    QSet<QString> inodes;
    for (const Listeners *table : {&result.tcp, &result.udp}) {
        for (auto it = table->inodes.cbegin(); it != table->inodes.cend(); ++it) {
            if (table->exposed.value(it.key()))
                inodes.unite(QSet<QString>(it.value().cbegin(), it.value().cend()));
        }
    }
    result.socketOwners = socketOwners(context, result.processes, inodes);
    result.interfaces = QDir(inRoot(context.root, QStringLiteral("/sys/class/net"))).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    return result;
}

QString commOf(const Snapshot &state, const QString &pid)
{
    for (const Process &process : state.processes) {
        if (process.pid == pid)
            return process.comm;
    }
    return {};
}

QStringList running(const Snapshot &state, const QStringList &names)
{
    QMap<QString, QStringList> pids;
    for (const Process &process : state.processes) {
        if (names.contains(process.comm))
            pids[process.comm] << process.pid;
    }
    QStringList found;
    for (auto it = pids.cbegin(); it != pids.cend(); ++it)
        found << (it.value().size() == 1 ? QStringLiteral("%1 (process %2)").arg(it.key(), it.value().first())
                                         : QStringLiteral("%1 (%2 processes)").arg(it.key(), QString::number(it.value().size())));
    return found;
}

QStringList portOwners(const Snapshot &state, const Listeners &table, int port, QStringList *pids = nullptr)
{
    QStringList owners;
    for (const QString &inode : table.inodes.value(port)) {
        const QString pid = state.socketOwners.value(inode);
        if (pid.isEmpty())
            continue;
        owners << QStringLiteral("%1 (process %2)").arg(commOf(state, pid), pid);
        if (pids)
            *pids << pid;
    }
    owners.removeDuplicates();
    if (pids)
        pids->removeDuplicates();
    return owners;
}

QString portUsage(const Snapshot &state, const Listeners &table, int port, bool tcp)
{
    const QStringList owners = portOwners(state, table, port);
    QString text = owners.isEmpty()
                       ? QStringLiteral("The program holding the port runs as another user, so its name is only visible to an administrator (try: sudo ss -lptn).")
                       : QStringLiteral("Held by %1.").arg(listed(owners));
    if (!tcp)
        return text;
    const int active = table.established.value(port);
    if (active > 0)
        return text + QStringLiteral(" %1 right now, so something is using it.").arg(counted(active, QStringLiteral("active connection"), QStringLiteral("active connections")));
    return text + QStringLiteral(" No active connections right now; if you do not recognise the program, you probably do not need it.");
}

QStringList forwardingInterfaces(const Snapshot &state)
{
    static const QStringList prefixes = {QStringLiteral("docker"), QStringLiteral("br-"), QStringLiteral("virbr"), QStringLiteral("wg"),
        QStringLiteral("tun"), QStringLiteral("tap"), QStringLiteral("lxcbr"), QStringLiteral("lxdbr"), QStringLiteral("cni"),
        QStringLiteral("flannel"), QStringLiteral("podman"), QStringLiteral("zt"), QStringLiteral("tailscale")};
    QStringList found;
    for (const QString &name : state.interfaces) {
        for (const QString &prefix : prefixes) {
            if (name.startsWith(prefix)) {
                found << name;
                break;
            }
        }
    }
    return found;
}

QString packageManager(const ScanContext &context)
{
    if (QFileInfo::exists(inRoot(context.root, QStringLiteral("/usr/bin/apt-get"))))
        return QStringLiteral("apt");
    if (QFileInfo::exists(inRoot(context.root, QStringLiteral("/usr/bin/dnf"))))
        return QStringLiteral("dnf");
    return {};
}

QString installCommand(const QString &manager, const QString &aptPackage, const QString &dnfPackage)
{
    if (manager == QStringLiteral("apt"))
        return QStringLiteral("DEBIAN_FRONTEND=noninteractive apt-get install -y %1").arg(aptPackage);
    return QStringLiteral("dnf install -y %1").arg(dnfPackage);
}

QString stopServicesScript(const QStringList &units, const QString &what)
{
    return QStringLiteral("found=0; for u in %1; do if systemctl is-active --quiet \"$u\" 2>/dev/null || systemctl is-enabled --quiet \"$u\" 2>/dev/null; then "
                          "systemctl disable --now \"$u\" && found=1; fi; done; [ \"$found\" = 1 ] || { echo %2 >&2; exit 1; }")
        .arg(units.join(QLatin1Char(' ')),
             shellQuote(QStringLiteral("No running service was found for %1. Stop it from the program that started it.").arg(what)));
}

QString quarantineScript(const QStringList &paths)
{
    return QStringLiteral("q=/root/forkmesh-quarantine/$(date +%Y%m%d-%H%M%S); mkdir -p \"$q\" && chmod 700 /root/forkmesh-quarantine \"$q\" || exit 1; "
                          "for f in %1; do [ -e \"$f\" ] || [ -L \"$f\" ] || continue; n=$(printf '%s' \"$f\" | tr / _); "
                          "mv -- \"$f\" \"$q/$n\" && chmod 000 \"$q/$n\"; done")
        .arg(quotedList(paths));
}

FixAction quarantineFix(const QStringList &paths, const QString &caution = {})
{
    return makeFix(QStringLiteral("Quarantine"),
                   QStringLiteral("Moves %1 into /root/forkmesh-quarantine and removes every permission on it, so it cannot run but stays available for investigation. Nothing is deleted.").arg(listed(paths)),
                   quarantineScript(paths), caution);
}

QString sysctlScript(const QString &key, const QString &value)
{
    QString pattern = key;
    pattern.replace(QLatin1Char('.'), QStringLiteral("\\."));
    return QStringLiteral("F=/etc/sysctl.d/90-forkmesh-security.conf; sysctl -w %1=%2 && mkdir -p /etc/sysctl.d && touch \"$F\" && "
                          "sed -i '/^[[:space:]]*%3[[:space:]]*=/d' \"$F\" && printf '%s\\n' '%1 = %2' >> \"$F\"")
        .arg(key, value, pattern);
}

QString sshdScript(const QString &key, const QString &value)
{
    const QString line = key + QLatin1Char(' ') + value;
    return QStringLiteral("F=/etc/ssh/sshd_config; D=/etc/ssh/sshd_config.d/00-forkmesh-security.conf; cp -p \"$F\" \"$F.forkmesh-backup\" || exit 1; "
                          "if grep -qiE '^[[:space:]]*Include[[:space:]].*sshd_config\\.d' \"$F\"; then "
                          "mkdir -p /etc/ssh/sshd_config.d && touch \"$D\" && sed -i '/^[[:space:]]*%1[[:space:]]/Id' \"$D\" && printf '%s\\n' %2 >> \"$D\"; "
                          "else sed -i '1i %3' \"$F\"; fi; "
                          "if sshd -t; then systemctl reload ssh 2>/dev/null || systemctl reload sshd 2>/dev/null || true; "
                          "else cp -p \"$F.forkmesh-backup\" \"$F\"; [ -f \"$D\" ] && sed -i '/^[[:space:]]*%1[[:space:]]/Id' \"$D\"; "
                          "echo 'sshd rejected the new setting, so the previous configuration was restored.' >&2; exit 1; fi")
        .arg(key, shellQuote(line), line);
}

QString loginDefsScript(const QString &key, const QString &value)
{
    return QStringLiteral("F=/etc/login.defs; if grep -qE '^[[:space:]]*%1([[:space:]]|$)' \"$F\"; then "
                          "sed -i -E 's/^[[:space:]]*%1([[:space:]].*)?$/%1 %2/' \"$F\"; else printf '%s\\n' '%1 %2' >> \"$F\"; fi")
        .arg(key, value);
}

QStringList portUnits(bool tcp, int port)
{
    static const QHash<int, QStringList> tcpUnits = {
        {21, {QStringLiteral("vsftpd"), QStringLiteral("proftpd"), QStringLiteral("pure-ftpd")}},
        {22, {QStringLiteral("ssh"), QStringLiteral("ssh.socket"), QStringLiteral("sshd")}},
        {23, {QStringLiteral("inetd"), QStringLiteral("openbsd-inetd"), QStringLiteral("xinetd"), QStringLiteral("telnet.socket")}},
        {25, {QStringLiteral("postfix"), QStringLiteral("exim4"), QStringLiteral("sendmail")}},
        {53, {QStringLiteral("named"), QStringLiteral("bind9"), QStringLiteral("dnsmasq")}},
        {80, {QStringLiteral("apache2"), QStringLiteral("httpd"), QStringLiteral("nginx"), QStringLiteral("lighttpd")}},
        {110, {QStringLiteral("dovecot")}}, {143, {QStringLiteral("dovecot")}},
        {111, {QStringLiteral("rpcbind"), QStringLiteral("rpcbind.socket")}},
        {139, {QStringLiteral("smbd"), QStringLiteral("nmbd")}}, {445, {QStringLiteral("smbd")}},
        {389, {QStringLiteral("slapd")}}, {631, {QStringLiteral("cups"), QStringLiteral("cups.socket")}},
        {873, {QStringLiteral("rsync")}}, {1433, {QStringLiteral("mssql-server")}}, {1883, {QStringLiteral("mosquitto")}},
        {2049, {QStringLiteral("nfs-server")}}, {2375, {QStringLiteral("docker"), QStringLiteral("docker.socket")}},
        {2376, {QStringLiteral("docker"), QStringLiteral("docker.socket")}}, {2379, {QStringLiteral("etcd")}},
        {3306, {QStringLiteral("mysql"), QStringLiteral("mariadb"), QStringLiteral("mysqld")}},
        {3389, {QStringLiteral("xrdp"), QStringLiteral("xrdp-sesman")}}, {5432, {QStringLiteral("postgresql")}},
        {5601, {QStringLiteral("kibana")}}, {5900, {QStringLiteral("x11vnc")}}, {5984, {QStringLiteral("couchdb")}},
        {6379, {QStringLiteral("redis-server"), QStringLiteral("redis")}}, {9200, {QStringLiteral("elasticsearch")}},
        {10000, {QStringLiteral("webmin")}}, {10250, {QStringLiteral("kubelet")}}, {11211, {QStringLiteral("memcached")}},
        {27017, {QStringLiteral("mongod")}}};
    static const QHash<int, QStringList> udpUnits = {
        {69, {QStringLiteral("tftpd-hpa"), QStringLiteral("atftpd")}}, {111, {QStringLiteral("rpcbind"), QStringLiteral("rpcbind.socket")}},
        {123, {QStringLiteral("ntp"), QStringLiteral("ntpsec")}}, {137, {QStringLiteral("nmbd")}}, {161, {QStringLiteral("snmpd")}},
        {1900, {QStringLiteral("minidlna")}}, {5353, {QStringLiteral("avahi-daemon"), QStringLiteral("avahi-daemon.socket")}},
        {11211, {QStringLiteral("memcached")}}};
    return (tcp ? tcpUnits : udpUnits).value(port);
}

struct Service {
    QStringList processes;
    QStringList units;
};

const QHash<QString, Service> &services()
{
    static const QHash<QString, Service> table = {
        {QStringLiteral("/etc/xinetd.conf"), {{QStringLiteral("xinetd")}, {QStringLiteral("xinetd")}}},
        {QStringLiteral("/etc/inetd.conf"), {{QStringLiteral("inetd")}, {QStringLiteral("inetd"), QStringLiteral("openbsd-inetd")}}},
        {QStringLiteral("/etc/exports"), {{QStringLiteral("nfsd")}, {QStringLiteral("nfs-server")}}},
        {QStringLiteral("/etc/samba/smb.conf"), {{QStringLiteral("smbd"), QStringLiteral("nmbd")}, {QStringLiteral("smbd"), QStringLiteral("nmbd")}}},
        {QStringLiteral("/etc/vsftpd.conf"), {{QStringLiteral("vsftpd")}, {QStringLiteral("vsftpd")}}},
        {QStringLiteral("/etc/apache2/apache2.conf"), {{QStringLiteral("apache2")}, {QStringLiteral("apache2")}}},
        {QStringLiteral("/etc/nginx/nginx.conf"), {{QStringLiteral("nginx")}, {QStringLiteral("nginx")}}},
        {QStringLiteral("/etc/docker/daemon.json"), {{QStringLiteral("dockerd")}, {QStringLiteral("docker"), QStringLiteral("docker.socket")}}},
        {QStringLiteral("/etc/cups/cupsd.conf"), {{QStringLiteral("cupsd")}, {QStringLiteral("cups"), QStringLiteral("cups.socket"), QStringLiteral("cups.path")}}},
        {QStringLiteral("/etc/postfix/main.cf"), {{QStringLiteral("master")}, {QStringLiteral("postfix")}}},
        {QStringLiteral("/etc/bind/named.conf"), {{QStringLiteral("named")}, {QStringLiteral("named"), QStringLiteral("bind9")}}},
        {QStringLiteral("/etc/rsyncd.conf"), {{QStringLiteral("rsync")}, {QStringLiteral("rsync")}}},
        {QStringLiteral("/etc/openvpn/server.conf"), {{QStringLiteral("openvpn")}, {QStringLiteral("openvpn-server@server"), QStringLiteral("openvpn@server")}}},
        {QStringLiteral("/etc/wireguard/wg0.conf"), {{}, {QStringLiteral("wg-quick@wg0")}}},
        {QStringLiteral("/etc/nfs.conf"), {{QStringLiteral("nfsd")}, {QStringLiteral("nfs-server")}}},
        {QStringLiteral("/etc/libvirt/libvirtd.conf"), {{QStringLiteral("libvirtd")}, {QStringLiteral("libvirtd"), QStringLiteral("libvirtd.socket")}}},
        {QStringLiteral("/etc/snmp/snmpd.conf"), {{QStringLiteral("snmpd")}, {QStringLiteral("snmpd")}}},
        {QStringLiteral("/etc/tor/torrc"), {{QStringLiteral("tor")}, {QStringLiteral("tor")}}},
        {QStringLiteral("/etc/mosquitto/mosquitto.conf"), {{QStringLiteral("mosquitto")}, {QStringLiteral("mosquitto")}}},
        {QStringLiteral("/etc/kubernetes/kubelet.conf"), {{QStringLiteral("kubelet")}, {QStringLiteral("kubelet")}}},
        {QStringLiteral("/etc/iscsi/iscsid.conf"), {{QStringLiteral("iscsid")}, {QStringLiteral("iscsid"), QStringLiteral("iscsid.socket")}}},
        {QStringLiteral("/etc/tftp.conf"), {{QStringLiteral("in.tftpd")}, {QStringLiteral("tftpd-hpa")}}},
        {QStringLiteral("/etc/squid/squid.conf"), {{QStringLiteral("squid")}, {QStringLiteral("squid")}}},
        {QStringLiteral("/etc/avahi/avahi-daemon.conf"), {{QStringLiteral("avahi-daemon")}, {QStringLiteral("avahi-daemon"), QStringLiteral("avahi-daemon.socket")}}},
        {QStringLiteral("/etc/dnsmasq.conf"), {{QStringLiteral("dnsmasq")}, {QStringLiteral("dnsmasq")}}},
        {QStringLiteral("/etc/xrdp/xrdp.ini"), {{QStringLiteral("xrdp")}, {QStringLiteral("xrdp"), QStringLiteral("xrdp-sesman")}}},
        {QStringLiteral("/etc/redis/redis.conf"), {{QStringLiteral("redis-server")}, {QStringLiteral("redis-server")}}},
        {QStringLiteral("/etc/mongod.conf"), {{QStringLiteral("mongod")}, {QStringLiteral("mongod")}}},
        {QStringLiteral("/etc/memcached.conf"), {{QStringLiteral("memcached")}, {QStringLiteral("memcached")}}},
    };
    return table;
}

bool withinLimit(const QString &value, const QString &limit)
{
    const QRegularExpressionMatch match = QRegularExpression(QStringLiteral("^(\\d+)([smhdw]?)$"), QRegularExpression::CaseInsensitiveOption).match(value.trimmed());
    if (!match.hasMatch())
        return false;
    static const QHash<QString, qint64> units = {{QString(), 1}, {QStringLiteral("s"), 1}, {QStringLiteral("m"), 60},
        {QStringLiteral("h"), 3600}, {QStringLiteral("d"), 86400}, {QStringLiteral("w"), 604800}};
    const qint64 amount = match.captured(1).toLongLong() * units.value(match.captured(2).toLower(), 1);
    return amount > 0 && amount <= limit.toLongLong();
}

QString effectiveConfig(const QString &path, const QString &target)
{
    QString text = readText(path);
    if (target != QStringLiteral("/etc/ssh/sshd_config") || !configValue(text, QStringLiteral("Include")).contains(QStringLiteral("sshd_config.d")))
        return text;
    const QDir drops(QFileInfo(path).dir().filePath(QStringLiteral("sshd_config.d")));
    QString merged;
    for (const QString &name : drops.entryList({QStringLiteral("*.conf")}, QDir::Files, QDir::Name))
        merged += readText(drops.filePath(name)) + QLatin1Char('\n');
    return merged + text;
}

QStringList keyLabels(const QString &text)
{
    static const QRegularExpression keyType(QStringLiteral("^(ssh-|ecdsa-|sk-)"));
    QStringList labels;
    for (const QString &line : entries(text)) {
        const QStringList fields = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        for (int i = 0; i < fields.size(); ++i) {
            if (!keyType.match(fields.at(i)).hasMatch() || i + 1 >= fields.size())
                continue;
            const QString comment = fields.mid(i + 2).join(QLatin1Char(' '));
            labels << (comment.isEmpty() ? QStringLiteral("an unnamed %1 key").arg(fields.at(i)) : comment);
            break;
        }
    }
    return labels;
}

int userKeyCount(const ScanContext &context)
{
    return int(keyLabels(readText(QDir(context.home).filePath(QStringLiteral(".ssh/authorized_keys")))).size());
}

void evaluateAccount(Finding &finding, const ScanContext &context)
{
    const QString probe = finding.check.probe;
    const QString passwdText = readText(inRoot(context.root, QStringLiteral("/etc/passwd")));
    if (passwdText.isEmpty()) {
        set(finding, FindingState::Unavailable, QStringLiteral("/etc/passwd is not present or cannot be read."));
        return;
    }
    const QList<QStringList> passwd = records(passwdText);
    QStringList names;
    if (probe == QStringLiteral("account-uid-zero")) {
        for (const QStringList &row : passwd) {
            if (row.size() > 2 && row.at(2) == QStringLiteral("0") && row.first() != QStringLiteral("root"))
                names << row.first();
        }
        finding.state = names.isEmpty() ? FindingState::Pass : FindingState::Critical;
        finding.detail = names.isEmpty() ? QStringLiteral("Only root has user ID 0.")
                                         : QStringLiteral("Accounts other than root have user ID 0: %1.").arg(names.join(QStringLiteral(", ")));
        if (!names.isEmpty())
            finding.fix = makeFix(QStringLiteral("Lock accounts"),
                                  QStringLiteral("Locks and expires %1 so nobody can sign in with them. The accounts stay in place for investigation.").arg(names.join(QStringLiteral(", "))),
                                  QStringLiteral("for u in %1; do usermod -L -e 1 \"$u\"; done").arg(quotedList(names)),
                                  QStringLiteral("If you created one of these accounts on purpose, it stops working."));
    } else if (probe == QStringLiteral("account-duplicate-uid")) {
        QMap<QString, QStringList> owners;
        for (const QStringList &row : passwd) {
            if (row.size() > 2 && !row.first().startsWith(QLatin1Char('+')))
                owners[row.at(2)] << row.first();
        }
        for (auto it = owners.cbegin(); it != owners.cend(); ++it) {
            if (it.value().size() > 1)
                names << QStringLiteral("%1 (%2)").arg(it.key(), it.value().join(QStringLiteral(", ")));
        }
        finding.state = names.isEmpty() ? FindingState::Pass : FindingState::Warning;
        finding.detail = names.isEmpty() ? QStringLiteral("Every account has its own user ID.")
                                         : QStringLiteral("Several accounts share a user ID: %1.").arg(names.join(QStringLiteral("; ")));
    } else if (probe == QStringLiteral("account-shadowed")) {
        for (const QStringList &row : passwd) {
            if (row.size() > 1 && !row.first().startsWith(QLatin1Char('+')) && row.at(1) != QStringLiteral("x")
                && !row.at(1).startsWith(QLatin1Char('*')) && !row.at(1).startsWith(QLatin1Char('!')) && !row.at(1).isEmpty())
                names << row.first();
        }
        finding.state = names.isEmpty() ? FindingState::Pass : FindingState::Warning;
        finding.detail = names.isEmpty() ? QStringLiteral("No password data is stored in the world-readable /etc/passwd.")
                                         : QStringLiteral("/etc/passwd holds password data instead of /etc/shadow for: %1.").arg(names.join(QStringLiteral(", ")));
        if (!names.isEmpty())
            finding.fix = makeFix(QStringLiteral("Move to shadow"), QStringLiteral("Runs pwconv, which moves the password hashes into /etc/shadow."), QStringLiteral("pwconv"));
    } else if (probe == QStringLiteral("account-legacy-entries")) {
        for (const QString &file : {QStringLiteral("/etc/passwd"), QStringLiteral("/etc/shadow"), QStringLiteral("/etc/group")}) {
            for (const QString &line : entries(readText(inRoot(context.root, file)))) {
                if (line.startsWith(QLatin1Char('+'))) {
                    names << file;
                    break;
                }
            }
        }
        finding.state = names.isEmpty() ? FindingState::Pass : FindingState::Warning;
        finding.detail = names.isEmpty() ? QStringLiteral("No legacy NIS \"+\" entries were found.")
                                         : QStringLiteral("Legacy NIS \"+\" entries are present in %1.").arg(names.join(QStringLiteral(", ")));
    } else {
        for (const QStringList &row : passwd) {
            if (row.size() > 1 && row.at(1).isEmpty() && !row.first().startsWith(QLatin1Char('+')))
                names << row.first();
        }
        const QString shadowText = readText(inRoot(context.root, QStringLiteral("/etc/shadow")));
        for (const QStringList &row : records(shadowText)) {
            if (row.size() > 1 && row.at(1).isEmpty() && !row.first().startsWith(QLatin1Char('+')) && !names.contains(row.first()))
                names << row.first();
        }
        if (!names.isEmpty()) {
            set(finding, FindingState::Critical, QStringLiteral("These accounts can sign in without a password: %1.").arg(names.join(QStringLiteral(", "))));
            finding.fix = makeFix(QStringLiteral("Lock accounts"),
                                  QStringLiteral("Locks the password of %1. The owner can set a new password with passwd afterwards.").arg(names.join(QStringLiteral(", "))),
                                  QStringLiteral("for u in %1; do passwd -l \"$u\"; done").arg(quotedList(names)),
                                  QStringLiteral("Anyone who signs in to these accounts without a password today will be locked out."));
        } else if (shadowText.isEmpty()) {
            set(finding, FindingState::Unavailable, QStringLiteral("/etc/shadow cannot be read by this user; /etc/passwd shows no empty password."));
        } else {
            set(finding, FindingState::Pass, QStringLiteral("Every account has a password or is locked."));
        }
    }
}

bool hasAny(const QStringList &paths, const ScanContext &context)
{
    for (const QString &path : paths) {
        if (QFileInfo::exists(inRoot(context.root, path)))
            return true;
    }
    return false;
}

bool ufwEnabled(const ScanContext &context)
{
    static const QRegularExpression enabled(QStringLiteral("^\\s*ENABLED\\s*=\\s*yes\\b"), QRegularExpression::MultilineOption | QRegularExpression::CaseInsensitiveOption);
    return enabled.match(readText(inRoot(context.root, QStringLiteral("/etc/ufw/ufw.conf")))).hasMatch();
}

void evaluateFirewall(Finding &finding, const ScanContext &context, const Snapshot &state)
{
    const bool ufwOn = ufwEnabled(context);
    const bool ufwInstalled = QFileInfo::exists(inRoot(context.root, QStringLiteral("/etc/ufw/ufw.conf")));
    QStringList found;
    if (ufwOn)
        found << QStringLiteral("UFW is turned on");
    if (QFileInfo::exists(inRoot(context.root, QStringLiteral("/etc/nftables.conf"))))
        found << QStringLiteral("an nftables rules file");
    if (QFileInfo::exists(inRoot(context.root, QStringLiteral("/etc/firewalld/firewalld.conf"))))
        found << QStringLiteral("a firewalld configuration");
    if (!found.isEmpty()) {
        set(finding, FindingState::Pass, QStringLiteral("A local firewall configuration was found: %1.").arg(found.join(QStringLiteral(", "))));
        return;
    }
    set(finding, FindingState::Warning, ufwInstalled ? QStringLiteral("UFW is installed but turned off, and no other firewall is configured.")
                                                     : QStringLiteral("No nftables, UFW, or firewalld configuration was found."));
    const bool ssh = state.tcp.exposed.value(22);
    const QString allowSsh = ssh ? QStringLiteral("ufw allow OpenSSH >/dev/null 2>&1 || ufw allow 22/tcp; ") : QString();
    const QString caution = QStringLiteral("Incoming connections that are not allowed explicitly will be blocked. Local sharing (printers, game streaming, ForkMesh's local mesh) may need a rule added afterwards.");
    if (ufwInstalled || hasAny({QStringLiteral("/usr/sbin/ufw"), QStringLiteral("/sbin/ufw")}, context)) {
        finding.fix = makeFix(QStringLiteral("Turn on firewall"),
                              ssh ? QStringLiteral("Allows SSH, then turns on UFW, which blocks other incoming connections.")
                                  : QStringLiteral("Turns on UFW, which blocks incoming connections."),
                              allowSsh + QStringLiteral("ufw --force enable"), caution);
        return;
    }
    const QString manager = packageManager(context);
    if (manager == QStringLiteral("apt"))
        finding.fix = makeFix(QStringLiteral("Install firewall"),
                              QStringLiteral("Installs UFW from your distribution's package archive and turns it on."),
                              installCommand(manager, QStringLiteral("ufw"), QString()) + QStringLiteral(" && ") + allowSsh + QStringLiteral("ufw --force enable"), caution);
    else if (manager == QStringLiteral("dnf"))
        finding.fix = makeFix(QStringLiteral("Install firewall"),
                              QStringLiteral("Installs firewalld from your distribution's package archive and turns it on."),
                              installCommand(manager, QString(), QStringLiteral("firewalld")) + QStringLiteral(" && systemctl enable --now firewalld"), caution);
}

void evaluateFirewallPolicy(Finding &finding, const ScanContext &context)
{
    if (!ufwEnabled(context)) {
        set(finding, FindingState::Unavailable, QStringLiteral("This reads UFW's default policy, and UFW is not turned on."));
        return;
    }
    QString policy = configValue(readText(inRoot(context.root, QStringLiteral("/etc/default/ufw"))).replace(QLatin1Char('='), QLatin1Char(' ')),
                                 QStringLiteral("DEFAULT_INPUT_POLICY"));
    policy.remove(QLatin1Char('"'));
    if (policy == QStringLiteral("DROP") || policy == QStringLiteral("REJECT")) {
        set(finding, FindingState::Pass, QStringLiteral("UFW blocks incoming connections by default (%1).").arg(policy));
        return;
    }
    set(finding, FindingState::Warning, policy.isEmpty() ? QStringLiteral("UFW's default incoming policy is not set.")
                                                         : QStringLiteral("UFW allows incoming connections by default (%1).").arg(policy));
    finding.fix = makeFix(QStringLiteral("Deny by default"), QStringLiteral("Sets UFW to block incoming connections unless a rule allows them."),
                          QStringLiteral("ufw default deny incoming && ufw reload"),
                          QStringLiteral("Services without an allow rule stop being reachable from other computers."));
}

void evaluatePort(Finding &finding, const Snapshot &state, bool tcp, bool backdoor)
{
    const CheckDefinition &definition = finding.check;
    const Listeners &table = tcp ? state.tcp : state.udp;
    const QString protocol = tcp ? QStringLiteral("TCP") : QStringLiteral("UDP");
    const int port = definition.target.toInt();
    const QString name = definition.title.section(QStringLiteral(" is not exposed"), 0, 0);
    if (!table.available) {
        set(finding, FindingState::Unavailable, QStringLiteral("The operating system did not expose %1 listener data.").arg(protocol));
        return;
    }
    if (!table.exposed.contains(port)) {
        set(finding, FindingState::Pass, QStringLiteral("%1 port %2 is not listening.").arg(protocol, definition.target));
        return;
    }
    finding.usage = portUsage(state, table, port, tcp);
    if (!table.exposed.value(port)) {
        if (backdoor)
            set(finding, FindingState::Review, QStringLiteral("%1 port %2 listens on this computer only. Check which program opened it.").arg(protocol, definition.target));
        else
            set(finding, FindingState::Pass, QStringLiteral("%1 port %2 listens only on the loopback interface.").arg(protocol, definition.target));
        return;
    }
    if (backdoor) {
        set(finding, FindingState::Critical, QStringLiteral("%1 port %2 is listening on a network interface. Backdoors and remote shells commonly use this port.").arg(protocol, definition.target));
        QStringList pids;
        portOwners(state, table, port, &pids);
        if (!pids.isEmpty())
            finding.fix = makeFix(QStringLiteral("Stop program"),
                                  QStringLiteral("Stops process %1, which holds the port. If it was started by a scheduled job or service it can come back; check the Persistence results.").arg(pids.join(QStringLiteral(", "))),
                                  QStringLiteral("kill %1").arg(pids.join(QLatin1Char(' '))),
                                  QStringLiteral("If this is a program you use, it closes without saving."));
        return;
    }
    set(finding, severity(definition.expected, FindingState::Review),
        QStringLiteral("%1 port %2 is listening on a network interface. Confirm that this service is required.").arg(protocol, definition.target));
    if (!tcp && port == 5355) {
        finding.fix = makeFix(QStringLiteral("Turn off LLMNR"), QStringLiteral("Turns off LLMNR in systemd-resolved. Normal DNS and mDNS keep working."),
                              QStringLiteral("mkdir -p /etc/systemd/resolved.conf.d && printf '[Resolve]\\nLLMNR=no\\n' > /etc/systemd/resolved.conf.d/forkmesh-security.conf && systemctl restart systemd-resolved"));
        return;
    }
    const QStringList units = portUnits(tcp, port);
    if (units.isEmpty())
        return;
    QString caution;
    if (tcp && port == 22)
        caution = QStringLiteral("Stopping SSH ends remote sign-in to this computer, including any SSH session open right now.");
    else if (tcp && table.established.value(port) > 0)
        caution = QStringLiteral("%1 has active connections right now; they will be cut off.").arg(name);
    finding.fix = makeFix(QStringLiteral("Stop service"),
                          QStringLiteral("Stops and disables %1 (%2) so the port closes and stays closed after a restart. Turn it back on with: sudo systemctl enable --now %3")
                              .arg(name, units.join(QStringLiteral(" or ")), units.first()),
                          stopServicesScript(units, name), caution);
}

void evaluateOtherPorts(Finding &finding, const Snapshot &state)
{
    const bool tcp = finding.check.target == QStringLiteral("tcp");
    const Listeners &table = tcp ? state.tcp : state.udp;
    const QSet<int> &known = tcp ? state.knownTcp : state.knownUdp;
    if (!table.available) {
        set(finding, FindingState::Unavailable, QStringLiteral("The operating system did not expose listener data."));
        return;
    }
    QList<int> ports;
    for (auto it = table.exposed.cbegin(); it != table.exposed.cend(); ++it) {
        if (!it.value() || known.contains(it.key()))
            continue;
        if (!tcp && (it.key() == 68 || it.key() == 546 || it.key() >= 32768))
            continue;
        ports << it.key();
    }
    std::sort(ports.begin(), ports.end());
    if (ports.isEmpty()) {
        set(finding, FindingState::Pass, QStringLiteral("No other %1 ports are open to the network.").arg(tcp ? QStringLiteral("TCP") : QStringLiteral("UDP")));
        return;
    }
    QStringList labels;
    for (int port : ports) {
        const QStringList owners = portOwners(state, table, port);
        labels << (owners.isEmpty() ? QString::number(port) : QStringLiteral("%1 by %2").arg(QString::number(port), owners.join(QStringLiteral(", "))));
    }
    set(finding, FindingState::Review, QStringLiteral("Other %1 ports open to the network: %2.").arg(tcp ? QStringLiteral("TCP") : QStringLiteral("UDP"), listed(labels, 10)));
    finding.usage = QStringLiteral("Ports without a program name belong to another user; an administrator can see them with: sudo ss -lpn");
}

void evaluateProcesses(Finding &finding, const Snapshot &state)
{
    const QString probe = finding.check.probe;
    if (state.processes.isEmpty()) {
        set(finding, FindingState::Unavailable, QStringLiteral("The process list is not available."));
        return;
    }
    static const QStringList temp = {QStringLiteral("/tmp/"), QStringLiteral("/var/tmp/"), QStringLiteral("/dev/shm/")};
    static const QStringList malware = {QStringLiteral("xmrig"), QStringLiteral("xmr-stak"), QStringLiteral("minerd"), QStringLiteral("cpuminer"),
        QStringLiteral("ccminer"), QStringLiteral("nbminer"), QStringLiteral("lolminer"), QStringLiteral("phoenixminer"), QStringLiteral("t-rex"),
        QStringLiteral("kdevtmpfsi"), QStringLiteral("kinsing"), QStringLiteral("kthreaddi"), QStringLiteral("kworkerds"), QStringLiteral("dbused"),
        QStringLiteral("sysrv"), QStringLiteral("watchdogs"), QStringLiteral("kerberods"), QStringLiteral("khugepageds")};
    QStringList hits;
    QStringList pids;
    bool memoryOnly = false;
    for (const Process &process : state.processes) {
        const QString label = QStringLiteral("%1 (process %2)").arg(process.comm, process.pid);
        if (probe == QStringLiteral("malware-process")) {
            const QString exeName = QFileInfo(process.exe.section(QStringLiteral(" (deleted)"), 0, 0)).fileName().toLower();
            if (malware.contains(process.comm.toLower()) || (!exeName.isEmpty() && malware.contains(exeName))) {
                hits << label;
                pids << process.pid;
            }
        } else if (probe == QStringLiteral("masquerade")) {
            if (process.cmdline.startsWith(QLatin1Char('[')) && process.ppid != QStringLiteral("2") && process.pid != QStringLiteral("2")) {
                hits << QStringLiteral("\"%1\" (process %2)").arg(process.cmdline.left(40), process.pid);
                pids << process.pid;
            }
        } else if (probe == QStringLiteral("temp-process")) {
            bool fromTemp = false;
            for (const QString &prefix : temp)
                fromTemp = fromTemp || process.exe.startsWith(prefix);
            if (fromTemp && !process.exe.startsWith(QStringLiteral("/tmp/.mount_")) && !process.exe.startsWith(QStringLiteral("/tmp/go-build"))) {
                hits << QStringLiteral("%1 runs %2").arg(label, process.exe);
                pids << process.pid;
            }
        } else if (process.exe.endsWith(QStringLiteral(" (deleted)"))) {
            const bool memfd = process.exe.startsWith(QStringLiteral("/memfd:")) && !process.exe.contains(QStringLiteral("runc"));
            memoryOnly = memoryOnly || memfd;
            hits << (memfd ? QStringLiteral("%1 runs from memory only").arg(label) : label);
        }
    }
    if (hits.isEmpty()) {
        const QHash<QString, QString> passes = {
            {QStringLiteral("malware-process"), QStringLiteral("No process is named like a known miner or Linux malware.")},
            {QStringLiteral("masquerade"), QStringLiteral("No program is posing as a kernel thread.")},
            {QStringLiteral("temp-process"), QStringLiteral("No visible program runs from a temporary folder.")},
            {QStringLiteral("deleted-process"), QStringLiteral("Every visible program still has its file on disk.")}};
        set(finding, FindingState::Pass, passes.value(probe));
        finding.usage = QStringLiteral("Programs run by other users are only fully visible when the checkup runs as an administrator.");
        return;
    }
    const QString stop = QStringLiteral("kill -9 %1").arg(pids.join(QLatin1Char(' ')));
    const QString comeBack = QStringLiteral("It may start again if a scheduled job or service launches it; check the Persistence results and investigate before trusting this computer again.");
    if (probe == QStringLiteral("malware-process")) {
        set(finding, FindingState::Critical, QStringLiteral("Processes named like known malware are running: %1.").arg(listed(hits)));
        finding.fix = makeFix(QStringLiteral("Stop processes"), QStringLiteral("Force-stops %1.").arg(listed(hits)), stop, comeBack);
    } else if (probe == QStringLiteral("masquerade")) {
        set(finding, FindingState::Critical, QStringLiteral("Programs are disguised as kernel threads: %1.").arg(listed(hits)));
        finding.fix = makeFix(QStringLiteral("Stop processes"), QStringLiteral("Force-stops %1.").arg(listed(hits)), stop, comeBack);
    } else if (probe == QStringLiteral("temp-process")) {
        set(finding, FindingState::Critical, QStringLiteral("Programs are running from a temporary folder: %1.").arg(listed(hits, 4)));
        finding.fix = makeFix(QStringLiteral("Stop processes"), QStringLiteral("Force-stops processes %1.").arg(pids.join(QStringLiteral(", "))), stop,
                              QStringLiteral("If one of these is a development tool you started, it closes without saving. ") + comeBack);
    } else if (memoryOnly) {
        set(finding, FindingState::Warning, QStringLiteral("Programs are running without a file on disk: %1.").arg(listed(hits, 4)));
    } else {
        set(finding, FindingState::Review, QStringLiteral("Programs are running from deleted files: %1.").arg(listed(hits, 4)));
        finding.usage = QStringLiteral("This is usually a program that was updated while running; restarting it, or the computer, loads the new version.");
    }
}

void evaluateHiddenModules(Finding &finding, const ScanContext &context, const Snapshot &state)
{
    const QDir sys(inRoot(context.root, QStringLiteral("/sys/module")));
    if (!state.modulesAvailable || !sys.exists()) {
        set(finding, FindingState::Unavailable, QStringLiteral("The kernel module lists are not available."));
        return;
    }
    QStringList hidden;
    for (const QString &name : sys.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (QFileInfo::exists(sys.filePath(name + QStringLiteral("/initstate"))) && !state.modules.contains(name))
            hidden << name;
    }
    if (hidden.isEmpty())
        set(finding, FindingState::Pass, QStringLiteral("Every loaded module appears in /proc/modules."));
    else
        set(finding, FindingState::Critical, QStringLiteral("Loaded modules are missing from /proc/modules, which is how kernel rootkits hide: %1.").arg(listed(hidden)));
}

void evaluateTempExecutables(Finding &finding, const ScanContext &context)
{
    const QString directory = inRoot(context.root, finding.check.target);
    if (!QFileInfo(directory).isDir()) {
        set(finding, FindingState::Unavailable, QStringLiteral("%1 is not present.").arg(finding.check.target));
        return;
    }
    QStringList found;
    QDirIterator it(directory, QDir::Files | QDir::Hidden | QDir::NoSymLinks | QDir::System, QDirIterator::Subdirectories);
    int seen = 0;
    while (it.hasNext() && ++seen < 5000) {
        const QString path = it.next();
        const QString shown = systemPath(context, path);
        if (shown.startsWith(QStringLiteral("/tmp/.mount_")) || QDir(directory).relativeFilePath(path).count(QLatin1Char('/')) > 3)
            continue;
        const Mode mode = fileMode(path, false);
        if (mode.regular && (mode.bits & kAnyExec))
            found << shown;
    }
    if (found.isEmpty()) {
        set(finding, FindingState::Pass, QStringLiteral("No executable files were found in %1.").arg(finding.check.target));
        return;
    }
    set(finding, severity(finding.check.expected, FindingState::Review),
        QStringLiteral("%1 in %2: %3.").arg(counted(found.size(), QStringLiteral("executable file"), QStringLiteral("executable files")), finding.check.target, listed(found, 5)));
    finding.usage = QStringLiteral("Build tools, installers and some apps unpack helpers here. If you did not just install or build something, treat these as suspicious.");
    finding.fix = quarantineFix(found, QStringLiteral("A program you are installing or building right now may fail if its helper is moved."));
}

void evaluatePromiscuous(Finding &finding, const ScanContext &context, const Snapshot &state)
{
    if (state.interfaces.isEmpty()) {
        set(finding, FindingState::Unavailable, QStringLiteral("The network interface list is not available."));
        return;
    }
    static const QStringList bridged = {QStringLiteral("veth"), QStringLiteral("docker"), QStringLiteral("br-"), QStringLiteral("virbr"),
        QStringLiteral("vnet"), QStringLiteral("lxc"), QStringLiteral("cni"), QStringLiteral("flannel"), QStringLiteral("cali"), QStringLiteral("tap")};
    QStringList sniffing;
    for (const QString &name : state.interfaces) {
        bool skip = false;
        for (const QString &prefix : bridged)
            skip = skip || name.startsWith(prefix);
        bool ok = false;
        const uint flags = readText(inRoot(context.root, QStringLiteral("/sys/class/net/%1/flags").arg(name))).trimmed().toUInt(&ok, 16);
        if (!skip && ok && (flags & 0x100))
            sniffing << name;
    }
    if (sniffing.isEmpty()) {
        set(finding, FindingState::Pass, QStringLiteral("No network interface is capturing other computers' traffic."));
        return;
    }
    set(finding, FindingState::Review, QStringLiteral("Interfaces in promiscuous mode: %1.").arg(sniffing.join(QStringLiteral(", "))));
    finding.usage = QStringLiteral("If you are running Wireshark, tcpdump or a virtual machine bridge, this is expected.");
    finding.fix = makeFix(QStringLiteral("Turn off capture"), QStringLiteral("Turns promiscuous mode off on %1.").arg(sniffing.join(QStringLiteral(", "))),
                          QStringLiteral("for i in %1; do ip link set dev \"$i\" promisc off; done").arg(quotedList(sniffing)),
                          QStringLiteral("A packet capture or VM bridge using the interface stops seeing other traffic."));
}

void evaluateSecureBoot(Finding &finding, const ScanContext &context)
{
    if (!QFileInfo(inRoot(context.root, QStringLiteral("/sys/firmware/efi"))).isDir()) {
        set(finding, FindingState::Review, QStringLiteral("The computer started in legacy BIOS mode, which has no Secure Boot."));
        return;
    }
    const QByteArray value = readBytes(inRoot(context.root, QStringLiteral("/sys/firmware/efi/efivars/SecureBoot-8be4df61-93ca-11d2-aa0d-00e098032b8c")), 16);
    if (value.size() < 5) {
        set(finding, FindingState::Unavailable, QStringLiteral("The firmware does not report the Secure Boot state."));
        return;
    }
    if (value.at(4) == 1)
        set(finding, FindingState::Pass, QStringLiteral("Secure Boot is on."));
    else
        set(finding, FindingState::Review, QStringLiteral("Secure Boot is off. It is turned on in the computer's firmware settings."));
}

void evaluateUpdates(Finding &finding, const ScanContext &context)
{
    const QString text = readText(inRoot(context.root, QStringLiteral("/var/lib/update-notifier/updates-available")));
    if (text.trimmed().isEmpty()) {
        set(finding, FindingState::Unavailable, QStringLiteral("This system does not keep a list of pending updates (Ubuntu's update-notifier writes one)."));
        return;
    }
    static const QRegularExpression security(QStringLiteral("(\\d+)[^\\n]*security update"));
    static const QRegularExpression total(QStringLiteral("(\\d+) updates? can be (?:applied|installed) immediately"));
    const int securityCount = security.match(text).captured(1).toInt();
    const int totalCount = total.match(text).captured(1).toInt();
    if (securityCount == 0 && totalCount == 0) {
        set(finding, FindingState::Pass, QStringLiteral("No updates are waiting."));
        return;
    }
    if (securityCount > 0)
        set(finding, FindingState::Warning, QStringLiteral("%1 waiting to be installed.").arg(counted(securityCount, QStringLiteral("security update is"), QStringLiteral("security updates are"))));
    else
        set(finding, FindingState::Review, QStringLiteral("%1 waiting to be installed.").arg(counted(totalCount, QStringLiteral("update is"), QStringLiteral("updates are"))));
    const QString manager = packageManager(context);
    if (manager == QStringLiteral("apt"))
        finding.fix = makeFix(QStringLiteral("Install updates"), QStringLiteral("Refreshes the package lists and installs every pending update."),
                              QStringLiteral("apt-get update && DEBIAN_FRONTEND=noninteractive apt-get -y upgrade"),
                              QStringLiteral("This downloads and installs packages and can take several minutes. Some programs may ask to be restarted."));
    else if (manager == QStringLiteral("dnf"))
        finding.fix = makeFix(QStringLiteral("Install updates"), QStringLiteral("Installs every pending security update."),
                              QStringLiteral("dnf -y upgrade --security"), QStringLiteral("This downloads and installs packages and can take several minutes."));
}

void evaluateReboot(Finding &finding, const ScanContext &context)
{
    QString marker;
    for (const QString &path : {QStringLiteral("/run/reboot-required"), QStringLiteral("/var/run/reboot-required")}) {
        if (QFileInfo::exists(inRoot(context.root, path)))
            marker = inRoot(context.root, path);
    }
    if (marker.isEmpty()) {
        set(finding, FindingState::Pass, QStringLiteral("No pending restart was recorded."));
        return;
    }
    const QStringList packages = entries(readText(marker + QStringLiteral(".pkgs")));
    set(finding, FindingState::Review, packages.isEmpty() ? QStringLiteral("Installed updates need a restart to take effect.")
                                                          : QStringLiteral("Installed updates need a restart to take effect: %1.").arg(listed(packages)));
    finding.fix = makeFix(QStringLiteral("Restart now"), QStringLiteral("Restarts the computer so the updated code runs."), QStringLiteral("systemctl reboot"),
                          QStringLiteral("Every program closes immediately. Save your work first."));
}

void evaluateDockerGroup(Finding &finding, const ScanContext &context, const Snapshot &state)
{
    const QString groups = readText(inRoot(context.root, QStringLiteral("/etc/group")));
    if (groups.isEmpty()) {
        set(finding, FindingState::Unavailable, QStringLiteral("/etc/group cannot be read."));
        return;
    }
    QStringList members;
    for (const QStringList &row : records(groups)) {
        if (row.size() > 3 && row.first() == QStringLiteral("docker"))
            members = row.at(3).split(QLatin1Char(','), Qt::SkipEmptyParts);
    }
    if (members.isEmpty()) {
        set(finding, FindingState::Pass, QStringLiteral("Nobody is in the docker group."));
        return;
    }
    set(finding, FindingState::Review, QStringLiteral("These accounts can control Docker and so act as root: %1.").arg(members.join(QStringLiteral(", "))));
    const QStringList daemon = running(state, {QStringLiteral("dockerd")});
    finding.usage = daemon.isEmpty() ? QStringLiteral("Docker is not running right now, so this membership is not being used.")
                                     : QStringLiteral("Docker is running. Members use it without sudo; removing them means typing sudo before docker.");
    finding.fix = makeFix(QStringLiteral("Remove from group"), QStringLiteral("Removes %1 from the docker group.").arg(members.join(QStringLiteral(", "))),
                          QStringLiteral("for u in %1; do gpasswd -d \"$u\" docker; done").arg(quotedList(members)),
                          QStringLiteral("These accounts will need sudo to run docker commands, starting from their next sign-in."));
}

void evaluateSudoers(Finding &finding, const ScanContext &context)
{
    const QString main = readText(inRoot(context.root, QStringLiteral("/etc/sudoers")));
    if (main.isEmpty()) {
        set(finding, FindingState::Unavailable, QStringLiteral("Only an administrator can read the sudo rules."));
        return;
    }
    QStringList everyone;
    QStringList passwordless;
    QStringList files = {inRoot(context.root, QStringLiteral("/etc/sudoers"))};
    files << filesIn(inRoot(context.root, QStringLiteral("/etc/sudoers.d")));
    static const QRegularExpression nopasswd(QStringLiteral("NOPASSWD\\s*:\\s*ALL\\b"));
    static const QRegularExpression noAuth(QStringLiteral("^Defaults\\b.*!authenticate"));
    for (const QString &file : files) {
        for (const QString &line : entries(readText(file))) {
            const QString who = line.section(QRegularExpression(QStringLiteral("\\s+")), 0, 0);
            if (noAuth.match(line).hasMatch())
                everyone << QStringLiteral("%1 turns off password checks").arg(systemPath(context, file));
            else if (nopasswd.match(line).hasMatch())
                (who == QStringLiteral("ALL") ? everyone : passwordless) << who;
        }
    }
    if (!everyone.isEmpty())
        set(finding, FindingState::Critical, QStringLiteral("Every user can run any command as root without a password (%1).").arg(listed(everyone)));
    else if (!passwordless.isEmpty())
        set(finding, FindingState::Warning, QStringLiteral("These can run any command as root without a password: %1.").arg(listed(passwordless)));
    else
        set(finding, FindingState::Pass, QStringLiteral("sudo asks for a password before running commands as root."));
}

void evaluateAutologin(Finding &finding, const ScanContext &context)
{
    QStringList files;
    QStringList scripts;
    static const QRegularExpression gdm(QStringLiteral("^\\s*(AutomaticLoginEnable|TimedLoginEnable)\\s*=\\s*(true|1)\\s*$"), QRegularExpression::MultilineOption | QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression lightdm(QStringLiteral("^\\s*autologin-user\\s*=\\s*\\S+"), QRegularExpression::MultilineOption);
    static const QRegularExpression sddm(QStringLiteral("^\\[Autologin\\][^\\[]*^\\s*User\\s*=\\s*\\S+"), QRegularExpression::MultilineOption);
    for (const QString &path : {QStringLiteral("/etc/gdm3/custom.conf"), QStringLiteral("/etc/gdm/custom.conf")}) {
        if (gdm.match(readText(inRoot(context.root, path))).hasMatch()) {
            files << path;
            scripts << QStringLiteral("sed -i -E 's/^[[:space:]]*(AutomaticLoginEnable|TimedLoginEnable)[[:space:]]*=.*/\\1=false/I' %1").arg(shellQuote(path));
        }
    }
    QStringList lightdmFiles = {QStringLiteral("/etc/lightdm/lightdm.conf")};
    for (const QString &path : filesIn(inRoot(context.root, QStringLiteral("/etc/lightdm/lightdm.conf.d")), {QStringLiteral("*.conf")}))
        lightdmFiles << systemPath(context, path);
    for (const QString &path : lightdmFiles) {
        if (lightdm.match(readText(inRoot(context.root, path))).hasMatch()) {
            files << path;
            scripts << QStringLiteral("sed -i -E 's/^[[:space:]]*(autologin-user[[:space:]]*=.*)/#\\1/' %1").arg(shellQuote(path));
        }
    }
    QStringList sddmFiles = {QStringLiteral("/etc/sddm.conf")};
    for (const QString &path : filesIn(inRoot(context.root, QStringLiteral("/etc/sddm.conf.d")), {QStringLiteral("*.conf")}))
        sddmFiles << systemPath(context, path);
    for (const QString &path : sddmFiles) {
        if (sddm.match(readText(inRoot(context.root, path))).hasMatch()) {
            files << path;
            scripts << QStringLiteral("sed -i -E '/^\\[Autologin\\]/,/^\\[/ s/^[[:space:]]*(User[[:space:]]*=.*)/#\\1/' %1").arg(shellQuote(path));
        }
    }
    if (files.isEmpty()) {
        set(finding, FindingState::Pass, QStringLiteral("The desktop asks for a password at start-up."));
        return;
    }
    set(finding, FindingState::Warning, QStringLiteral("Automatic sign-in is turned on in %1.").arg(files.join(QStringLiteral(", "))));
    finding.fix = makeFix(QStringLiteral("Turn off auto sign-in"), QStringLiteral("Turns off automatic sign-in so the desktop asks for your password at start-up."),
                          scripts.join(QStringLiteral(" && ")), QStringLiteral("Make sure you know your password before restarting."));
}

void evaluateEtcWritable(Finding &finding, const ScanContext &context)
{
    const QString etc = inRoot(context.root, QStringLiteral("/etc"));
    if (!QFileInfo(etc).isDir()) {
        set(finding, FindingState::Unavailable, QStringLiteral("/etc is not present."));
        return;
    }
    QStringList found;
    QDirIterator it(etc, QDir::Files | QDir::Hidden | QDir::NoSymLinks, QDirIterator::Subdirectories);
    int seen = 0;
    while (it.hasNext() && ++seen < 20000) {
        const QString path = it.next();
        const Mode mode = fileMode(path, false);
        if (mode.regular && (mode.bits & kOtherWrite))
            found << systemPath(context, path);
    }
    if (found.isEmpty()) {
        set(finding, FindingState::Pass, QStringLiteral("No file in /etc can be changed by every user."));
        return;
    }
    set(finding, FindingState::Warning, QStringLiteral("Files in /etc that every user can change: %1.").arg(listed(found, 5)));
    finding.fix = makeFix(QStringLiteral("Make read-only"), QStringLiteral("Removes write access for other users from %1.").arg(listed(found, 5)),
                          QStringLiteral("chmod o-w -- %1").arg(quotedList(found)));
}

void evaluateDockerSocket(Finding &finding, const ScanContext &context)
{
    for (const QString &socket : {QStringLiteral("/run/docker.sock"), QStringLiteral("/var/run/docker.sock")}) {
        const Mode mode = fileMode(inRoot(context.root, socket));
        if (!mode.known)
            continue;
        if (mode.bits & kOtherWrite) {
            set(finding, FindingState::Critical, QStringLiteral("%1 can be used by every user, so every user can become root.").arg(socket));
            finding.fix = makeFix(QStringLiteral("Restrict socket"), QStringLiteral("Limits %1 to root and the docker group.").arg(socket),
                                  QStringLiteral("chmod o-rw %1").arg(shellQuote(socket)));
        } else {
            set(finding, FindingState::Pass, QStringLiteral("%1 is limited to root and the docker group.").arg(socket));
        }
        return;
    }
    set(finding, FindingState::Pass, QStringLiteral("Docker's socket is not present."));
}

void evaluatePathVariable(Finding &finding, const ScanContext &context)
{
    if (context.pathVariable.isEmpty()) {
        set(finding, FindingState::Unavailable, QStringLiteral("PATH is not set for this program."));
        return;
    }
    QStringList relative;
    QStringList writable;
    for (const QString &entry : context.pathVariable.split(QLatin1Char(':'))) {
        if (entry.isEmpty() || !entry.startsWith(QLatin1Char('/'))) {
            relative << (entry.isEmpty() ? QStringLiteral("an empty entry") : QStringLiteral("\"%1\"").arg(entry));
            continue;
        }
        const Mode mode = fileMode(inRoot(context.root, entry));
        if (mode.known && mode.directory && (mode.bits & kOtherWrite))
            writable << entry;
    }
    if (!writable.isEmpty())
        set(finding, FindingState::Critical, QStringLiteral("PATH includes folders every user can write to: %1.").arg(writable.join(QStringLiteral(", "))));
    else if (!relative.isEmpty())
        set(finding, FindingState::Warning, QStringLiteral("PATH searches the current folder through %1.").arg(relative.join(QStringLiteral(", "))));
    else
        set(finding, FindingState::Pass, QStringLiteral("Every PATH folder is absolute and protected."));
    if (finding.state != FindingState::Pass)
        finding.usage = QStringLiteral("PATH is set in your shell start-up files (~/.profile, ~/.bashrc) or /etc/environment; remove the entry there.");
}

void evaluateHomeFolder(Finding &finding, const ScanContext &context)
{
    const Mode mode = fileMode(context.home);
    if (!mode.known) {
        set(finding, FindingState::Unavailable, QStringLiteral("The home folder cannot be read."));
        return;
    }
    if (!(mode.bits & (kOtherRead | kOtherExec))) {
        set(finding, FindingState::Pass, QStringLiteral("Other accounts cannot open your home folder."));
        return;
    }
    set(finding, FindingState::Review, QStringLiteral("Other accounts on this computer can open %1.").arg(context.home));
    finding.usage = QStringLiteral("A web server serving ~/public_html needs this; most desktops do not.");
    finding.fix = makeFix(QStringLiteral("Make private"), QStringLiteral("Removes access to %1 for other accounts.").arg(context.home),
                          QStringLiteral("chmod o-rwx %1").arg(shellQuote(context.home)), {}, false);
}

void evaluateHomePrivate(Finding &finding, const ScanContext &context)
{
    const QString path = QDir(context.home).filePath(finding.check.target);
    const Mode mode = fileMode(path);
    if (!mode.known) {
        set(finding, FindingState::Pass, QStringLiteral("~/%1 is not present.").arg(finding.check.target));
        return;
    }
    if (mode.bits & (kOtherRead | kOtherWrite)) {
        set(finding, FindingState::Critical, QStringLiteral("Every account on this computer can read ~/%1.").arg(finding.check.target));
    } else if (mode.bits & (kGroupRead | kGroupWrite)) {
        set(finding, FindingState::Warning, QStringLiteral("Members of the file's group can read ~/%1.").arg(finding.check.target));
    } else {
        set(finding, FindingState::Pass, QStringLiteral("~/%1 is private to you.").arg(finding.check.target));
        return;
    }
    finding.fix = makeFix(QStringLiteral("Make private"), QStringLiteral("Limits ~/%1 to your account only.").arg(finding.check.target),
                          QStringLiteral("chmod go-rwx %1").arg(shellQuote(path)), {}, false);
}

int keyEncryption(const QByteArray &text)
{
    if (!text.contains("PRIVATE KEY-----"))
        return -1;
    if (text.contains("ENCRYPTED"))
        return 0;
    if (!text.contains("BEGIN OPENSSH PRIVATE KEY"))
        return 1;
    QByteArray body;
    for (const QByteArray &line : text.split('\n')) {
        if (!line.startsWith("-----"))
            body += line.trimmed();
    }
    const QByteArray raw = QByteArray::fromBase64(body);
    const QByteArray magic("openssh-key-v1\0", 15);
    if (!raw.startsWith(magic) || raw.size() < 19)
        return -1;
    const quint32 length = (quint32(uchar(raw.at(15))) << 24) | (quint32(uchar(raw.at(16))) << 16) | (quint32(uchar(raw.at(17))) << 8) | quint32(uchar(raw.at(18)));
    return raw.mid(19, int(length)) == "none" ? 1 : 0;
}

void evaluateKeyPassphrase(Finding &finding, const ScanContext &context)
{
    const QString folder = QDir(context.home).filePath(QStringLiteral(".ssh"));
    QStringList open;
    int keys = 0;
    for (const QString &path : filesIn(folder)) {
        const QString name = QFileInfo(path).fileName();
        if (name.endsWith(QStringLiteral(".pub")) || name.startsWith(QStringLiteral("known_hosts")) || name.startsWith(QStringLiteral("authorized_keys")) || name == QStringLiteral("config"))
            continue;
        const int state = keyEncryption(readBytes(path, 65536));
        if (state < 0)
            continue;
        ++keys;
        if (state == 1)
            open << QStringLiteral("~/.ssh/") + name;
    }
    if (keys == 0)
        set(finding, FindingState::Pass, QStringLiteral("No SSH private keys were found in ~/.ssh."));
    else if (open.isEmpty())
        set(finding, FindingState::Pass, QStringLiteral("Every SSH private key in ~/.ssh has a passphrase."));
    else
        set(finding, FindingState::Review, QStringLiteral("SSH private keys without a passphrase: %1. Add one with ssh-keygen -p -f <file>.").arg(listed(open)));
}

void evaluateAuthorizedKeys(Finding &finding, const ScanContext &context, const Snapshot &state)
{
    struct KeyFile {
        QString path;
        QString shown;
        bool elevated;
    };
    const QList<KeyFile> files = {
        {QDir(context.home).filePath(QStringLiteral(".ssh/authorized_keys")), QStringLiteral("~/.ssh/authorized_keys"), false},
        {QDir(context.home).filePath(QStringLiteral(".ssh/authorized_keys2")), QStringLiteral("~/.ssh/authorized_keys2"), false},
        {inRoot(context.root, QStringLiteral("/root/.ssh/authorized_keys")), QStringLiteral("/root/.ssh/authorized_keys"), true}};
    QStringList labels;
    QStringList writable;
    QStringList writablePaths;
    bool elevated = false;
    for (const KeyFile &file : files) {
        const Mode mode = fileMode(file.path);
        if (!mode.known)
            continue;
        if (mode.bits & (kOtherWrite | kGroupWrite)) {
            writable << file.shown;
            writablePaths << file.path;
            elevated = elevated || file.elevated;
        }
        for (const QString &label : keyLabels(readText(file.path)))
            labels << QStringLiteral("%1 (%2)").arg(label, file.shown);
    }
    const bool sshListening = state.tcp.exposed.value(22);
    finding.usage = sshListening ? QStringLiteral("SSH is listening on the network, so these keys can sign in from other computers.")
                                 : QStringLiteral("SSH is not listening on the network right now, so these keys are not usable from other computers.");
    if (!writable.isEmpty()) {
        set(finding, FindingState::Critical, QStringLiteral("Other users can add their own key to %1.").arg(writable.join(QStringLiteral(", "))));
        finding.fix = makeFix(QStringLiteral("Protect keys"), QStringLiteral("Removes write access for other users from %1.").arg(writable.join(QStringLiteral(", "))),
                              QStringLiteral("chmod go-w -- %1").arg(quotedList(writablePaths)), {}, elevated);
    } else if (!labels.isEmpty()) {
        set(finding, FindingState::Review, QStringLiteral("%1 can sign in without a password: %2.").arg(counted(labels.size(), QStringLiteral("SSH key"), QStringLiteral("SSH keys")), listed(labels)));
    } else {
        set(finding, FindingState::Pass, QStringLiteral("No SSH keys are authorised to sign in as you."));
        finding.usage.clear();
    }
}

QString suspiciousReason(const QString &line, bool tempPaths, bool shellStartup)
{
    static const QRegularExpression download(QStringLiteral(R"((curl|wget)\b[^|;\n]*\|\s*(sudo\s+)?(ba|da|z|k)?sh\b)"));
    static const QRegularExpression decode(QStringLiteral(R"(base64\s+(-d|--decode)[^\n]*\|\s*(sudo\s+)?(ba|da|z)?sh\b)"));
    static const QRegularExpression devTcp(QStringLiteral(R"(/dev/(tcp|udp)/)"));
    static const QRegularExpression netcat(QStringLiteral(R"(\b(nc|ncat|netcat)\b[^\n]*\s-[ec]\s)"));
    static const QRegularExpression miner(QStringLiteral(R"(stratum\+(tcp|ssl)://|\bxmrig\b)"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression preload(QStringLiteral(R"(\bLD_PRELOAD\s*=)"));
    static const QRegularExpression sudoAlias(QStringLiteral(R"(\balias\s+sudo\s*=)"));
    static const QRegularExpression temp(QStringLiteral(R"((^|[;&|=@-]|\b(ba|da|z)?sh\s|\bnohup\s|\b(root|\*|\d+)\s)\s*(/tmp|/var/tmp|/dev/shm)/[^\s;&|'"]+)"));
    if (download.match(line).hasMatch())
        return QStringLiteral("downloads a script and runs it");
    if (decode.match(line).hasMatch())
        return QStringLiteral("decodes hidden base64 and runs it");
    if (devTcp.match(line).hasMatch())
        return QStringLiteral("opens a raw network connection (/dev/tcp), a reverse-shell trick");
    if (netcat.match(line).hasMatch())
        return QStringLiteral("starts netcat with a shell attached");
    if (miner.match(line).hasMatch())
        return QStringLiteral("points at a cryptocurrency miner");
    if (shellStartup && preload.match(line).hasMatch())
        return QStringLiteral("sets LD_PRELOAD for every program");
    if (shellStartup && sudoAlias.match(line).hasMatch())
        return QStringLiteral("redefines sudo");
    if (tempPaths && temp.match(line).hasMatch())
        return QStringLiteral("runs a program from a temporary folder");
    return {};
}

QStringList suspiciousFiles(const ScanContext &context, const QStringList &files, bool tempPaths, bool shellStartup, const QString &linePrefix,
                            QStringList *flagged = nullptr)
{
    QStringList found;
    for (const QString &file : files) {
        for (const QString &line : entries(readText(file))) {
            if (!linePrefix.isEmpty() && !line.startsWith(linePrefix))
                continue;
            const QString reason = suspiciousReason(line, tempPaths, shellStartup);
            if (reason.isEmpty())
                continue;
            found << QStringLiteral("%1 %2").arg(systemPath(context, file), reason);
            if (flagged)
                *flagged << systemPath(context, file);
            break;
        }
    }
    return found;
}

void evaluatePersistence(Finding &finding, const ScanContext &context)
{
    const QString probe = finding.check.probe;
    const QDir home(context.home);
    QStringList files;
    QStringList movable;
    bool tempPaths = true;
    bool shellStartup = false;
    QString prefix;
    if (probe == QStringLiteral("persist-cron")) {
        files << inRoot(context.root, QStringLiteral("/etc/crontab"));
        for (const QString &folder : {QStringLiteral("/etc/cron.d"), QStringLiteral("/etc/cron.hourly"), QStringLiteral("/etc/cron.daily"),
                                      QStringLiteral("/etc/cron.weekly"), QStringLiteral("/etc/cron.monthly")})
            movable << filesIn(inRoot(context.root, folder));
        files << movable;
        files << filesIn(inRoot(context.root, QStringLiteral("/var/spool/cron/crontabs"))) << filesIn(inRoot(context.root, QStringLiteral("/var/spool/cron")));
    } else if (probe == QStringLiteral("persist-systemd")) {
        movable << filesIn(inRoot(context.root, QStringLiteral("/etc/systemd/system")), {QStringLiteral("*.service")}, true)
                << filesIn(home.filePath(QStringLiteral(".config/systemd/user")), {QStringLiteral("*.service")}, true);
        files = movable;
        prefix = QStringLiteral("Exec");
    } else if (probe == QStringLiteral("persist-autostart")) {
        movable << filesIn(inRoot(context.root, QStringLiteral("/etc/xdg/autostart")), {QStringLiteral("*.desktop")})
                << filesIn(home.filePath(QStringLiteral(".config/autostart")), {QStringLiteral("*.desktop")});
        files = movable;
        prefix = QStringLiteral("Exec=");
    } else if (probe == QStringLiteral("persist-shell")) {
        for (const QString &path : {QStringLiteral("/etc/profile"), QStringLiteral("/etc/bash.bashrc"), QStringLiteral("/etc/environment"), QStringLiteral("/etc/zsh/zshrc")})
            files << inRoot(context.root, path);
        files << filesIn(inRoot(context.root, QStringLiteral("/etc/profile.d")), {QStringLiteral("*.sh")});
        for (const QString &name : {QStringLiteral(".bashrc"), QStringLiteral(".profile"), QStringLiteral(".bash_profile"), QStringLiteral(".bash_login"),
                                    QStringLiteral(".zshrc"), QStringLiteral(".zprofile")})
            files << home.filePath(name);
        tempPaths = false;
        shellStartup = true;
    } else if (probe == QStringLiteral("persist-rc-local")) {
        files << inRoot(context.root, QStringLiteral("/etc/rc.local"));
    }
    QStringList flagged;
    const QStringList found = suspiciousFiles(context, files, tempPaths, shellStartup, prefix, &flagged);
    if (probe == QStringLiteral("persist-rc-local") && found.isEmpty()) {
        QStringList commands = entries(readText(files.first()));
        commands.removeAll(QStringLiteral("exit 0"));
        if (commands.isEmpty())
            set(finding, FindingState::Pass, QStringLiteral("/etc/rc.local is absent or runs nothing."));
        else
            set(finding, FindingState::Review, QStringLiteral("/etc/rc.local runs %1 at every boot: %2.").arg(counted(commands.size(), QStringLiteral("command"), QStringLiteral("commands")), listed(commands, 3)));
        return;
    }
    if (found.isEmpty()) {
        set(finding, FindingState::Pass, QStringLiteral("Checked %1; nothing suspicious was found.").arg(counted(files.size(), QStringLiteral("file"), QStringLiteral("files"))));
        return;
    }
    set(finding, FindingState::Critical, QStringLiteral("Suspicious start-up entries: %1.").arg(listed(found, 4)));
    QStringList quarantine;
    for (const QString &path : flagged) {
        if (movable.contains(inRoot(context.root, path)) || movable.contains(path))
            quarantine << path;
    }
    if (quarantine.isEmpty())
        return;
    finding.fix = quarantineFix(quarantine, QStringLiteral("If one of these is a job you set up yourself, it stops running."));
    if (probe == QStringLiteral("persist-systemd"))
        finding.fix.script += QStringLiteral("; systemctl daemon-reload");
}

void evaluateLibraryPath(Finding &finding, const ScanContext &context)
{
    QStringList files = {inRoot(context.root, QStringLiteral("/etc/ld.so.conf"))};
    files << filesIn(inRoot(context.root, QStringLiteral("/etc/ld.so.conf.d")), {QStringLiteral("*.conf")});
    QStringList unsafe;
    for (const QString &file : files) {
        for (const QString &line : entries(readText(file))) {
            for (const QString &prefix : {QStringLiteral("/tmp"), QStringLiteral("/var/tmp"), QStringLiteral("/dev/shm"), QStringLiteral("/home")}) {
                if (line == prefix || line.startsWith(prefix + QLatin1Char('/')))
                    unsafe << QStringLiteral("%1 in %2").arg(line, systemPath(context, file));
            }
        }
    }
    if (unsafe.isEmpty())
        set(finding, FindingState::Pass, QStringLiteral("Shared libraries are only loaded from system folders."));
    else
        set(finding, FindingState::Critical, QStringLiteral("Libraries are loaded from user-writable folders: %1.").arg(listed(unsafe)));
}

void evaluateNfs(Finding &finding, const ScanContext &context)
{
    const QStringList lines = entries(readText(inRoot(context.root, QStringLiteral("/etc/exports"))));
    if (lines.isEmpty()) {
        set(finding, FindingState::Pass, QStringLiteral("No folders are shared over NFS."));
        return;
    }
    QStringList open;
    QStringList squashed;
    static const QRegularExpression world(QStringLiteral(R"((^|\s)(\*)?\([^)]*\brw\b[^)]*\))"));
    for (const QString &line : lines) {
        if (!line.contains(QStringLiteral("no_root_squash")))
            continue;
        (world.match(line).hasMatch() ? open : squashed) << line.section(QRegularExpression(QStringLiteral("\\s+")), 0, 0);
    }
    if (!open.isEmpty())
        set(finding, FindingState::Critical, QStringLiteral("Folders shared writable with every computer and no_root_squash: %1.").arg(listed(open)));
    else if (!squashed.isEmpty())
        set(finding, FindingState::Warning, QStringLiteral("Folders shared with no_root_squash: %1.").arg(listed(squashed)));
    else
        set(finding, FindingState::Pass, QStringLiteral("NFS shares map remote root to an unprivileged user."));
}

void evaluateSamba(Finding &finding, const ScanContext &context)
{
    const QString path = inRoot(context.root, QStringLiteral("/etc/samba/smb.conf"));
    if (!QFileInfo::exists(path)) {
        set(finding, FindingState::Pass, QStringLiteral("Samba is not configured."));
        return;
    }
    static const QRegularExpression guest(QStringLiteral(R"(^\s*(guest ok|public)\s*=\s*yes\b)"), QRegularExpression::CaseInsensitiveOption);
    QString section;
    QStringList shares;
    for (const QString &line : entries(readText(path))) {
        if (line.startsWith(QLatin1Char('[')))
            section = line;
        else if (guest.match(line).hasMatch() && !shares.contains(section))
            shares << section;
    }
    if (shares.isEmpty())
        set(finding, FindingState::Pass, QStringLiteral("No Samba share allows guests."));
    else
        set(finding, FindingState::Warning, QStringLiteral("Samba shares open to guests without a password: %1.").arg(shares.join(QStringLiteral(", "))));
}

void evaluateDockerTcp(Finding &finding, const ScanContext &context)
{
    const QString text = readText(inRoot(context.root, QStringLiteral("/etc/docker/daemon.json")));
    static const QRegularExpression tcp(QStringLiteral(R"(tcp://([^"]+))"));
    static const QRegularExpression verify(QStringLiteral(R"("tlsverify"\s*:\s*true)"));
    const QRegularExpressionMatch match = tcp.match(text);
    if (!match.hasMatch()) {
        set(finding, FindingState::Pass, QStringLiteral("Docker is not configured to listen on a TCP address."));
        return;
    }
    const QString address = match.captured(1);
    if (address.startsWith(QStringLiteral("127.0.0.1")) || address.startsWith(QStringLiteral("localhost")))
        set(finding, FindingState::Review, QStringLiteral("Docker listens on %1, which other users on this computer can reach.").arg(address));
    else if (verify.match(text).hasMatch())
        set(finding, FindingState::Review, QStringLiteral("Docker listens on %1 and requires TLS client certificates.").arg(address));
    else
        set(finding, FindingState::Critical, QStringLiteral("Docker listens on %1 without TLS client certificates.").arg(address));
}

void evaluateRedis(Finding &finding, const ScanContext &context)
{
    const QString path = inRoot(context.root, QStringLiteral("/etc/redis/redis.conf"));
    if (!QFileInfo::exists(path)) {
        set(finding, FindingState::Pass, QStringLiteral("Redis is not installed."));
        return;
    }
    const QString text = readText(path);
    if (text.isEmpty()) {
        set(finding, FindingState::Unavailable, QStringLiteral("Only an administrator can read the Redis configuration."));
        return;
    }
    const QStringList bind = configValue(text, QStringLiteral("bind")).split(QLatin1Char(' '), Qt::SkipEmptyParts);
    bool local = !bind.isEmpty();
    for (const QString &address : bind)
        local = local && QStringList{QStringLiteral("127.0.0.1"), QStringLiteral("::1"), QStringLiteral("-::1"), QStringLiteral("localhost")}.contains(address);
    const bool password = !configValue(text, QStringLiteral("requirepass")).isEmpty();
    const bool protectedMode = configValue(text, QStringLiteral("protected-mode")).toLower() != QStringLiteral("no");
    if (local || password)
        set(finding, FindingState::Pass, local ? QStringLiteral("Redis listens only on this computer.") : QStringLiteral("Redis requires a password."));
    else if (protectedMode)
        set(finding, FindingState::Warning, QStringLiteral("Redis listens on %1 without a password; only protected mode blocks other computers.").arg(bind.isEmpty() ? QStringLiteral("every address") : bind.join(QStringLiteral(", "))));
    else
        set(finding, FindingState::Critical, QStringLiteral("Redis listens on %1 without a password and with protected mode off.").arg(bind.isEmpty() ? QStringLiteral("every address") : bind.join(QStringLiteral(", "))));
}

void evaluateSshCrypto(Finding &finding, const ScanContext &context)
{
    const QString path = inRoot(context.root, QStringLiteral("/etc/ssh/sshd_config"));
    const QString text = effectiveConfig(path, QStringLiteral("/etc/ssh/sshd_config"));
    if (text.isEmpty() && !QFileInfo::exists(path)) {
        set(finding, FindingState::Pass, QStringLiteral("The SSH server is not installed, so this setting does not apply."));
        return;
    }
    if (text.isEmpty()) {
        set(finding, FindingState::Unavailable, QStringLiteral("/etc/ssh/sshd_config is not present or cannot be read."));
        return;
    }
    static const QRegularExpression weak(QStringLiteral(R"(cbc|arcfour|3des|blowfish|cast128|hmac-md5|hmac-sha1|umac-64@|diffie-hellman-group1-sha1|diffie-hellman-group14-sha1|diffie-hellman-group-exchange-sha1|ssh-dss)"),
                                         QRegularExpression::CaseInsensitiveOption);
    QStringList found;
    for (const QString &key : {QStringLiteral("Ciphers"), QStringLiteral("MACs"), QStringLiteral("KexAlgorithms"), QStringLiteral("HostKeyAlgorithms")}) {
        for (const QString &algorithm : configValue(text, key).split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            if (weak.match(algorithm).hasMatch())
                found << algorithm.trimmed();
        }
    }
    if (found.isEmpty())
        set(finding, FindingState::Pass, QStringLiteral("No weak SSH algorithms are enabled."));
    else
        set(finding, FindingState::Warning, QStringLiteral("Weak SSH algorithms are enabled: %1. Remove them from sshd_config.").arg(listed(found)));
}

FixAction safeguardFix(const QString &id, const ScanContext &context)
{
    const QString manager = packageManager(context);
    if (manager.isEmpty())
        return {};
    const QString caution = QStringLiteral("This downloads and installs packages from your distribution.");
    if (id == QStringLiteral("safeguard-automatic-updates")) {
        if (manager == QStringLiteral("apt"))
            return makeFix(QStringLiteral("Turn on"), QStringLiteral("Installs unattended-upgrades and turns on daily security updates."),
                           installCommand(manager, QStringLiteral("unattended-upgrades"), QString())
                               + QStringLiteral(" && printf '%s\\n' 'APT::Periodic::Update-Package-Lists \"1\";' 'APT::Periodic::Unattended-Upgrade \"1\";' > /etc/apt/apt.conf.d/20auto-upgrades"),
                           caution);
        return makeFix(QStringLiteral("Turn on"), QStringLiteral("Installs dnf-automatic and turns on automatic updates."),
                       installCommand(manager, QString(), QStringLiteral("dnf-automatic")) + QStringLiteral(" && systemctl enable --now dnf-automatic-install.timer"), caution);
    }
    if (id == QStringLiteral("safeguard-audit-daemon"))
        return makeFix(QStringLiteral("Install"), QStringLiteral("Installs and starts the audit daemon."),
                       installCommand(manager, QStringLiteral("auditd"), QStringLiteral("audit")) + QStringLiteral(" && systemctl enable --now auditd"), caution);
    if (id == QStringLiteral("safeguard-brute-force-protection"))
        return makeFix(QStringLiteral("Install"), QStringLiteral("Installs and starts fail2ban, which blocks addresses that keep failing to sign in."),
                       installCommand(manager, QStringLiteral("fail2ban"), QStringLiteral("fail2ban")) + QStringLiteral(" && systemctl enable --now fail2ban"), caution);
    return {};
}

void evaluateOptional(Finding &finding, const ScanContext &context, const Snapshot &state)
{
    const CheckDefinition &definition = finding.check;
    const bool exists = QFileInfo::exists(inRoot(context.root, definition.target));
    if (!exists) {
        set(finding, FindingState::Pass, QStringLiteral("%1 is not installed or configured.").arg(definition.target));
        return;
    }
    set(finding, FindingState::Review, QStringLiteral("%1 is installed or configured; review whether it is needed.").arg(definition.target));
    if (definition.target == QStringLiteral("/etc/hosts.equiv") || definition.target == QStringLiteral("/root/.rhosts") || definition.target == QStringLiteral("/etc/ssh/sshrc")) {
        finding.fix = makeFix(QStringLiteral("Disable"), QStringLiteral("Renames %1 so it is no longer read. Rename it back to undo.").arg(definition.target),
                              QStringLiteral("mv -- %1 %2").arg(shellQuote(definition.target), shellQuote(definition.target + QStringLiteral(".forkmesh-disabled"))));
        return;
    }
    if (!services().contains(definition.target))
        return;
    const Service service = services().value(definition.target);
    QStringList active = running(state, service.processes);
    if (definition.target == QStringLiteral("/etc/wireguard/wg0.conf") && state.interfaces.contains(QStringLiteral("wg0")))
        active << QStringLiteral("the wg0 interface");
    finding.usage = active.isEmpty()
                        ? QStringLiteral("Not running right now: the configuration is installed but nothing is using it.")
                        : QStringLiteral("Running right now: %1. Something on this computer may rely on it.").arg(listed(active));
    finding.fix = makeFix(QStringLiteral("Turn off"),
                          QStringLiteral("Stops and disables %1 so it does not start again. Turn it back on with: sudo systemctl enable --now %2").arg(service.units.join(QStringLiteral(", ")), service.units.first()),
                          stopServicesScript(service.units, definition.target),
                          active.isEmpty() ? QString() : QStringLiteral("It is running now; anything that depends on it stops working."));
}

Finding evaluate(const CheckDefinition &definition, const ScanContext &context, const Snapshot &state)
{
    Finding finding;
    finding.check = definition;
    const QString &probe = definition.probe;
    const QString path = inRoot(context.root, definition.target);
    if (probe == QStringLiteral("firewall")) {
        evaluateFirewall(finding, context, state);
    } else if (probe == QStringLiteral("firewall-policy")) {
        evaluateFirewallPolicy(finding, context);
    } else if (probe == QStringLiteral("rootkit")) {
        const bool found = QFileInfo::exists(path) || QFileInfo(path).isSymLink();
        set(finding, found ? FindingState::Critical : FindingState::Pass,
            found ? QStringLiteral("A known %1 artifact is present at %2.").arg(definition.category == QStringLiteral("Malware") ? QStringLiteral("malware") : QStringLiteral("rootkit"), definition.target)
                  : QStringLiteral("No known artifact found at %1.").arg(definition.target));
        if (found)
            finding.fix = quarantineFix({definition.target}, QStringLiteral("Moving the file does not remove an infection that is already running. Treat the computer as compromised: change passwords from another device and consider reinstalling."));
    } else if (probe == QStringLiteral("port") || probe == QStringLiteral("udp-port") || probe == QStringLiteral("backdoor-port")) {
        evaluatePort(finding, state, probe != QStringLiteral("udp-port"), probe == QStringLiteral("backdoor-port"));
    } else if (probe == QStringLiteral("other-ports")) {
        evaluateOtherPorts(finding, state);
    } else if (probe == QStringLiteral("preload")) {
        const QStringList libraries = entries(readText(path));
        set(finding, libraries.isEmpty() ? FindingState::Pass : FindingState::Review,
            libraries.isEmpty() ? QStringLiteral("%1 preloads no libraries.").arg(definition.target)
                                : QStringLiteral("%1 preloads %2 into every program. Rootkits use this file; confirm each entry.").arg(definition.target, libraries.join(QStringLiteral(", "))));
        if (!libraries.isEmpty())
            finding.fix = makeFix(QStringLiteral("Disable"), QStringLiteral("Renames %1 so no library is forced into every program. Rename it back to undo.").arg(definition.target),
                                  QStringLiteral("mv -- /etc/ld.so.preload /etc/ld.so.preload.forkmesh-disabled"),
                                  QStringLiteral("If a security or monitoring product put this entry there, it stops working until you undo this."));
    } else if (probe == QStringLiteral("hidden-module")) {
        evaluateHiddenModules(finding, context, state);
    } else if (probe == QStringLiteral("malware-process") || probe == QStringLiteral("masquerade") || probe == QStringLiteral("temp-process") || probe == QStringLiteral("deleted-process")) {
        evaluateProcesses(finding, state);
    } else if (probe == QStringLiteral("temp-executables")) {
        evaluateTempExecutables(finding, context);
    } else if (probe == QStringLiteral("promiscuous")) {
        evaluatePromiscuous(finding, context, state);
    } else if (probe == QStringLiteral("module")) {
        if (!state.modulesAvailable) {
            set(finding, FindingState::Unavailable, QStringLiteral("The kernel module list is not available."));
        } else if (state.modules.contains(definition.target)) {
            const ModuleInfo info = state.modules.value(definition.target);
            set(finding, FindingState::Review, QStringLiteral("The %1 kernel module is loaded. Block it if this computer does not need it.").arg(definition.target));
            finding.usage = info.users > 0
                                ? QStringLiteral("In use: %1%2. It will stay loaded until the next restart.")
                                      .arg(counted(info.users, QStringLiteral("user"), QStringLiteral("users")),
                                           info.dependents.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(info.dependents.join(QStringLiteral(", "))))
                                : QStringLiteral("Nothing is using it right now, so it is safe to unload.");
            finding.fix = makeFix(QStringLiteral("Block module"),
                                  QStringLiteral("Unloads %1 if nothing uses it and stops it loading again. Undo by deleting /etc/modprobe.d/forkmesh-security-%1.conf.").arg(definition.target),
                                  QStringLiteral("modprobe -r %1 2>/dev/null; printf 'install %1 /bin/false\\nblacklist %1\\n' > /etc/modprobe.d/forkmesh-security-%1.conf").arg(definition.target),
                                  info.users > 0 ? QStringLiteral("Something uses this module now; whatever needs it stops working after the next restart.") : QString());
        } else {
            set(finding, FindingState::Pass, QStringLiteral("The %1 kernel module is not loaded.").arg(definition.target));
        }
    } else if (probe == QStringLiteral("mount-option")) {
        if (!state.mountsAvailable) {
            set(finding, FindingState::Unavailable, QStringLiteral("The mount table is not available."));
        } else if (!state.mountOptions.contains(definition.target)) {
            set(finding, FindingState::Review, QStringLiteral("%1 is not a separate mount, so mount options cannot restrict it.").arg(definition.target));
        } else if (state.mountOptions.value(definition.target).contains(definition.expected)) {
            set(finding, FindingState::Pass, QStringLiteral("%1 is mounted with %2.").arg(definition.target, definition.expected));
        } else {
            set(finding, FindingState::Warning, QStringLiteral("%1 is mounted without %2.").arg(definition.target, definition.expected));
            finding.fix = makeFix(QStringLiteral("Remount"), QStringLiteral("Remounts %1 with %2 until the next restart. Add %2 to its line in /etc/fstab to keep it.").arg(definition.target, definition.expected),
                                  QStringLiteral("mount -o remount,%1 %2").arg(definition.expected, shellQuote(definition.target)),
                                  definition.expected == QStringLiteral("noexec") ? QStringLiteral("Programs that run helpers from this folder will fail until it is remounted without noexec.") : QString());
        }
    } else if (probe == QStringLiteral("world-writable")) {
        const QFileInfo info(path);
        if (!info.exists()) {
            set(finding, FindingState::Unavailable, QStringLiteral("%1 is not present.").arg(definition.target));
        } else if (info.permissions() & QFile::WriteOther) {
            set(finding, severity(definition.expected, FindingState::Warning), QStringLiteral("%1 can be changed by any user.").arg(definition.target));
            finding.fix = makeFix(QStringLiteral("Make read-only"), QStringLiteral("Removes write access for other users from %1.").arg(definition.target),
                                  QStringLiteral("chmod o-w %1").arg(shellQuote(definition.target)));
        } else {
            set(finding, FindingState::Pass, QStringLiteral("%1 cannot be changed by other users.").arg(definition.target));
        }
    } else if (probe == QStringLiteral("etc-writable")) {
        evaluateEtcWritable(finding, context);
    } else if (probe == QStringLiteral("docker-socket")) {
        evaluateDockerSocket(finding, context);
    } else if (probe == QStringLiteral("path-variable")) {
        evaluatePathVariable(finding, context);
    } else if (probe == QStringLiteral("home-folder")) {
        evaluateHomeFolder(finding, context);
    } else if (probe == QStringLiteral("home-private-file")) {
        evaluateHomePrivate(finding, context);
    } else if (probe == QStringLiteral("ssh-key-passphrase")) {
        evaluateKeyPassphrase(finding, context);
    } else if (probe == QStringLiteral("authorized-keys")) {
        evaluateAuthorizedKeys(finding, context, state);
    } else if (probe == QStringLiteral("sticky-dir")) {
        const Mode mode = fileMode(path);
        if (!mode.known) {
            set(finding, FindingState::Unavailable, QStringLiteral("%1 is not present or its mode cannot be read.").arg(definition.target));
        } else if ((mode.bits & kOtherWrite) && !(mode.bits & kSticky)) {
            set(finding, FindingState::Warning, QStringLiteral("%1 is writable by every user without the sticky bit, so users can delete each other's files.").arg(definition.target));
            finding.fix = makeFix(QStringLiteral("Set sticky bit"), QStringLiteral("Sets the sticky bit on %1.").arg(definition.target), QStringLiteral("chmod +t %1").arg(shellQuote(definition.target)));
        } else {
            set(finding, FindingState::Pass, (mode.bits & kOtherWrite) ? QStringLiteral("%1 is shared and has the sticky bit set.").arg(definition.target)
                                                                       : QStringLiteral("%1 is not writable by every user.").arg(definition.target));
        }
    } else if (probe == QStringLiteral("setuid")) {
        const Mode mode = fileMode(path);
        if (!QFileInfo::exists(path)) {
            set(finding, FindingState::Pass, QStringLiteral("%1 is not installed.").arg(definition.target));
        } else if (!mode.known) {
            set(finding, FindingState::Unavailable, QStringLiteral("The mode of %1 cannot be read.").arg(definition.target));
        } else if (mode.bits & kSetuid) {
            set(finding, FindingState::Critical, QStringLiteral("%1 is set-user-ID, so any user can run it with its owner's privileges.").arg(definition.target));
            finding.fix = makeFix(QStringLiteral("Remove set-user-ID"), QStringLiteral("Removes the set-user-ID bit from %1. It keeps working as a normal program.").arg(definition.target),
                                  QStringLiteral("chmod u-s %1").arg(shellQuote(definition.target)));
        } else {
            set(finding, FindingState::Pass, QStringLiteral("%1 is not set-user-ID.").arg(definition.target));
        }
    } else if (probe == QStringLiteral("present")) {
        QStringList found;
        const QStringList candidates = definition.target.split(QLatin1Char('|'));
        for (const QString &candidate : candidates) {
            if (QFileInfo::exists(inRoot(context.root, candidate)))
                found << candidate;
        }
        set(finding, found.isEmpty() ? FindingState::Review : FindingState::Pass,
            found.isEmpty() ? QStringLiteral("Not found: %1.").arg(candidates.join(QStringLiteral(", "))) : QStringLiteral("Found %1.").arg(found.join(QStringLiteral(", "))));
        if (found.isEmpty())
            finding.fix = safeguardFix(definition.id, context);
    } else if (probe == QStringLiteral("entries")) {
        const qsizetype count = entries(readText(path)).size();
        set(finding, count > 0 ? FindingState::Pass : FindingState::Review,
            count > 0 ? QStringLiteral("%1 lists %2 entries.").arg(definition.target, QString::number(count)) : QStringLiteral("%1 is missing or lists no entries.").arg(definition.target));
        if (count == 0)
            finding.usage = QStringLiteral("Encrypting an existing disk means reinstalling or using your distribution's encryption tool; it cannot be switched on safely from here.");
    } else if (probe == QStringLiteral("mac")) {
        const QString apparmor = readText(inRoot(context.root, QStringLiteral("/sys/module/apparmor/parameters/enabled"))).trimmed();
        const QString selinux = readText(inRoot(context.root, QStringLiteral("/sys/fs/selinux/enforce"))).trimmed();
        if (apparmor == QStringLiteral("Y"))
            set(finding, FindingState::Pass, QStringLiteral("AppArmor is enabled."));
        else if (selinux == QStringLiteral("1"))
            set(finding, FindingState::Pass, QStringLiteral("SELinux is enforcing."));
        else if (selinux == QStringLiteral("0")) {
            set(finding, FindingState::Warning, QStringLiteral("SELinux is in permissive mode."));
            finding.fix = makeFix(QStringLiteral("Enforce SELinux"), QStringLiteral("Switches SELinux to enforcing until the next restart. Set SELINUX=enforcing in /etc/selinux/config to keep it."),
                                  QStringLiteral("setenforce 1"), QStringLiteral("Programs without a matching SELinux policy may be blocked."));
        } else
            set(finding, FindingState::Warning, QStringLiteral("Neither AppArmor nor SELinux is enabled."));
    } else if (probe == QStringLiteral("lockdown")) {
        const QString mode = QRegularExpression(QStringLiteral("\\[(\\w+)\\]")).match(readText(path).trimmed()).captured(1);
        if (mode.isEmpty())
            set(finding, FindingState::Unavailable, QStringLiteral("This kernel does not report a lockdown mode."));
        else
            set(finding, mode == QStringLiteral("none") ? FindingState::Review : FindingState::Pass, QStringLiteral("Kernel lockdown is %1.").arg(mode));
    } else if (probe == QStringLiteral("tainted")) {
        const QString value = readText(QDir(context.procRoot).filePath(QStringLiteral("sys/kernel/tainted"))).trimmed();
        if (value.isEmpty())
            set(finding, FindingState::Unavailable, QStringLiteral("The kernel taint flags are not available."));
        else
            set(finding, value == QStringLiteral("0") ? FindingState::Pass : FindingState::Review,
                value == QStringLiteral("0") ? QStringLiteral("The kernel is not tainted.")
                                             : QStringLiteral("The kernel taint flags are %1: an out-of-tree, unsigned or forced module was loaded, or the kernel hit an error.").arg(value));
    } else if (probe == QStringLiteral("secure-boot")) {
        evaluateSecureBoot(finding, context);
    } else if (probe.startsWith(QStringLiteral("account-"))) {
        evaluateAccount(finding, context);
    } else if (probe == QStringLiteral("docker-group")) {
        evaluateDockerGroup(finding, context, state);
    } else if (probe == QStringLiteral("sudoers")) {
        evaluateSudoers(finding, context);
    } else if (probe == QStringLiteral("autologin")) {
        evaluateAutologin(finding, context);
    } else if (probe == QStringLiteral("sysctl")) {
        QString sysctlPath = definition.target;
        const QString value = readText(QDir(context.procRoot).filePath(QStringLiteral("sys/") + sysctlPath.replace(QLatin1Char('.'), QLatin1Char('/')))).trimmed();
        const QStringList accepted = definition.expected.split(QLatin1Char('|'));
        if (value.isEmpty()) {
            set(finding, FindingState::Unavailable, QStringLiteral("This kernel setting is not available on this system."));
        } else if (accepted.contains(value)) {
            set(finding, FindingState::Pass, QStringLiteral("%1 is %2.").arg(definition.target, value));
        } else {
            set(finding, FindingState::Warning, QStringLiteral("%1 is %2; expected %3.").arg(definition.target, value, definition.expected));
            QString caution;
            if (definition.target.contains(QStringLiteral("forward"))) {
                const QStringList users = forwardingInterfaces(state);
                finding.usage = users.isEmpty() ? QStringLiteral("No container, virtual machine or VPN interface was found, so nothing appears to need forwarding.")
                                                : QStringLiteral("In use: %1 rely on forwarding. Leave it on if you use them.").arg(users.join(QStringLiteral(", ")));
                caution = users.isEmpty() ? QStringLiteral("Containers, virtual machines and VPN servers lose network access.")
                                          : QStringLiteral("%1 will lose network access.").arg(users.join(QStringLiteral(", ")));
            } else if (definition.target == QStringLiteral("kernel.kexec_load_disabled") || definition.target == QStringLiteral("kernel.unprivileged_bpf_disabled")) {
                caution = QStringLiteral("This cannot be turned back off until the next restart.");
            } else if (definition.target == QStringLiteral("kernel.perf_event_paranoid") || definition.target == QStringLiteral("kernel.yama.ptrace_scope")) {
                caution = QStringLiteral("Debuggers and profilers such as gdb and perf may need sudo afterwards.");
            }
            finding.fix = makeFix(QStringLiteral("Apply setting"),
                                  QStringLiteral("Sets %1 to %2 now and saves it in /etc/sysctl.d/90-forkmesh-security.conf so it survives a restart.").arg(definition.target, accepted.first()),
                                  sysctlScript(definition.target, accepted.first()), caution);
        }
    } else if (probe == QStringLiteral("private-file")) {
        const QFileInfo info(path);
        if (!info.exists()) {
            set(finding, FindingState::Pass, QStringLiteral("%1 is not present, so there is nothing to protect.").arg(definition.target));
        } else if (info.permissions() & (QFile::ReadOther | QFile::WriteOther | QFile::ExeOther)) {
            set(finding, severity(definition.expected, FindingState::Warning), QStringLiteral("%1 is readable, writable, or executable by other users.").arg(definition.target));
            finding.fix = makeFix(QStringLiteral("Make private"), QStringLiteral("Removes every permission for other users from %1.").arg(definition.target),
                                  QStringLiteral("chmod o-rwx %1").arg(shellQuote(definition.target)));
        } else {
            set(finding, FindingState::Pass, QStringLiteral("%1 is not accessible to other users.").arg(definition.target));
        }
    } else if (probe == QStringLiteral("config") || probe == QStringLiteral("config-max")) {
        const QString text = effectiveConfig(path, definition.target);
        const QString key = definition.expected.section(QLatin1Char('='), 0, 0);
        const QString desired = definition.expected.section(QLatin1Char('='), 1);
        if (text.isEmpty() && !QFileInfo::exists(path) && definition.target == QStringLiteral("/etc/ssh/sshd_config")) {
            set(finding, FindingState::Pass, QStringLiteral("The SSH server is not installed, so this setting does not apply."));
        } else if (text.isEmpty()) {
            set(finding, FindingState::Unavailable, QStringLiteral("%1 is not present or cannot be read.").arg(definition.target));
        } else {
            const QString value = configValue(text, key);
            const bool limited = probe == QStringLiteral("config-max");
            const bool matches = limited ? withinLimit(value, desired) : desired.toCaseFolded().split(QLatin1Char('|')).contains(value.toCaseFolded());
            const QString wanted = limited ? QStringLiteral("at most %1").arg(desired) : desired.split(QLatin1Char('|')).join(QStringLiteral(" or "));
            finding.state = matches ? FindingState::Pass : FindingState::Warning;
            if (matches)
                finding.detail = QStringLiteral("%1 is set to %2.").arg(key, value);
            else if (value.isEmpty())
                finding.detail = QStringLiteral("%1 is not explicitly configured; expected %2.").arg(key, wanted);
            else
                finding.detail = QStringLiteral("%1 is set to %2; expected %3.").arg(key, value, wanted);
            if (!matches) {
                const QString target = desired.section(QLatin1Char('|'), 0, 0);
                if (definition.target == QStringLiteral("/etc/login.defs")) {
                    finding.fix = makeFix(QStringLiteral("Apply setting"), QStringLiteral("Sets %1 to %2 in /etc/login.defs. Existing passwords and files are not changed.").arg(key, target),
                                          loginDefsScript(key, target));
                } else {
                    QString caution;
                    if (key == QStringLiteral("PasswordAuthentication") || key == QStringLiteral("KbdInteractiveAuthentication")) {
                        const int keys = userKeyCount(context);
                        finding.usage = keys > 0 ? QStringLiteral("You have %1 set up, so you can still sign in without a password.").arg(counted(keys, QStringLiteral("SSH key"), QStringLiteral("SSH keys")))
                                                 : QStringLiteral("No SSH key is set up for your account (~/.ssh/authorized_keys is empty or missing).");
                        caution = keys > 0 ? QStringLiteral("Accounts without an SSH key will no longer be able to sign in over SSH.")
                                           : QStringLiteral("You have no SSH key set up: you will not be able to sign in to this computer over SSH until you add one.");
                    } else if (key == QStringLiteral("AllowTcpForwarding") || key == QStringLiteral("AllowAgentForwarding") || key == QStringLiteral("X11Forwarding")) {
                        caution = QStringLiteral("SSH tunnels, agent forwarding or remote windows that you use through this computer stop working.");
                    } else if (key == QStringLiteral("PermitRootLogin")) {
                        caution = QStringLiteral("Sign in as your own user and use sudo instead of signing in as root.");
                    }
                    finding.fix = makeFix(QStringLiteral("Apply setting"),
                                          QStringLiteral("Sets %1 to %2 for the SSH server, checks the configuration with sshd -t (restoring the old file if it fails) and reloads SSH.").arg(key, target),
                                          sshdScript(key, target), caution);
                }
            }
        }
    } else if (probe == QStringLiteral("ssh-crypto")) {
        evaluateSshCrypto(finding, context);
    } else if (probe == QStringLiteral("updates-pending")) {
        evaluateUpdates(finding, context);
    } else if (probe == QStringLiteral("reboot-required")) {
        evaluateReboot(finding, context);
    } else if (probe.startsWith(QStringLiteral("persist-"))) {
        if (probe == QStringLiteral("persist-ld-config"))
            evaluateLibraryPath(finding, context);
        else
            evaluatePersistence(finding, context);
    } else if (probe == QStringLiteral("home-exists")) {
        const QString file = QDir(context.home).filePath(definition.target);
        const bool exists = QFileInfo::exists(file);
        set(finding, exists ? FindingState::Review : FindingState::Pass,
            exists ? QStringLiteral("~/%1 exists and runs at every SSH sign-in: %2").arg(definition.target, listed(entries(readText(file)), 2))
                   : QStringLiteral("~/%1 is not present.").arg(definition.target));
        if (exists)
            finding.fix = makeFix(QStringLiteral("Disable"), QStringLiteral("Renames ~/%1 so it no longer runs. Rename it back to undo.").arg(definition.target),
                                  QStringLiteral("mv -- %1 %2").arg(shellQuote(file), shellQuote(file + QStringLiteral(".forkmesh-disabled"))), {}, false);
    } else if (probe == QStringLiteral("nfs-exports")) {
        evaluateNfs(finding, context);
    } else if (probe == QStringLiteral("samba-guest")) {
        evaluateSamba(finding, context);
    } else if (probe == QStringLiteral("docker-tcp")) {
        evaluateDockerTcp(finding, context);
    } else if (probe == QStringLiteral("redis-auth")) {
        evaluateRedis(finding, context);
    } else {
        evaluateOptional(finding, context, state);
    }
    if (finding.state == FindingState::Pass || finding.state == FindingState::Unavailable)
        finding.fix = {};
    return finding;
}

QString pathId(QString path)
{
    if (path.startsWith(QLatin1Char('/')) || path.startsWith(QLatin1Char('.')))
        path = path.mid(1);
    return path.replace(QLatin1Char('/'), QLatin1Char('-')).replace(QLatin1Char('.'), QLatin1Char('-'));
}

}

QList<CheckDefinition> checkCatalog()
{
    QList<CheckDefinition> checks;
    checks << check(QStringLiteral("firewall-configured"), QStringLiteral("A firewall is configured"), QStringLiteral("Firewall"), QStringLiteral("firewall"));
    checks << check(QStringLiteral("firewall-default-deny"), QStringLiteral("The firewall blocks incoming connections by default"), QStringLiteral("Firewall"), QStringLiteral("firewall-policy"));
    const QList<QPair<QString, QString>> rootkits = {
        {QStringLiteral("adore"), QStringLiteral("/usr/lib/libadore.so")}, {QStringLiteral("azazel"), QStringLiteral("/usr/lib/libazazel.so")},
        {QStringLiteral("beastkit"), QStringLiteral("/usr/sbin/xzibit")}, {QStringLiteral("diamorphine"), QStringLiteral("/usr/lib/libdiamorphine.so")},
        {QStringLiteral("enyelkm"), QStringLiteral("/dev/enyelkm")}, {QStringLiteral("jynx"), QStringLiteral("/usr/lib/libjynx2.so")},
        {QStringLiteral("knark"), QStringLiteral("/dev/knark")}, {QStringLiteral("phalanx"), QStringLiteral("/etc/phalanx2")},
        {QStringLiteral("reptile"), QStringLiteral("/reptile/reptile.ko")}, {QStringLiteral("rkloader"), QStringLiteral("/usr/bin/rkloader")},
        {QStringLiteral("rootkit-hunter"), QStringLiteral("/usr/local/bin/asp")}, {QStringLiteral("suckit"), QStringLiteral("/dev/suckit")},
        {QStringLiteral("synapsys"), QStringLiteral("/usr/lib/libsynapsys.so")}, {QStringLiteral("t0rn"), QStringLiteral("/usr/sbin/ttyload")},
        {QStringLiteral("tuxtendo"), QStringLiteral("/dev/tux")}, {QStringLiteral("xlogin"), QStringLiteral("/usr/lib/libxlogin.so")},
        {QStringLiteral("ebury"), QStringLiteral("/lib64/libns2.so")}, {QStringLiteral("ebury-keyutils"), QStringLiteral("/lib64/libkeyutils.so.1.9")},
        {QStringLiteral("lion"), QStringLiteral("/dev/.lib")}, {QStringLiteral("ramen"), QStringLiteral("/usr/src/.poop")},
        {QStringLiteral("shv4"), QStringLiteral("/lib/libext-2.so.7")}
    };
    for (const auto &[name, path] : rootkits)
        checks << check(QStringLiteral("rootkit-") + name, QStringLiteral("No %1 rootkit artifact").arg(name), QStringLiteral("Rootkits"), QStringLiteral("rootkit"), path);
    checks << check(QStringLiteral("rootkit-ld-so-preload"), QStringLiteral("No library is preloaded into every program"), QStringLiteral("Rootkits"), QStringLiteral("preload"), QStringLiteral("/etc/ld.so.preload"));
    checks << check(QStringLiteral("rootkit-hidden-module"), QStringLiteral("No kernel module is hidden"), QStringLiteral("Rootkits"), QStringLiteral("hidden-module"));
    const QList<QPair<QString, QString>> malware = {
        {QStringLiteral("xorddos"), QStringLiteral("/lib/libgcc4.so")}, {QStringLiteral("kinsing"), QStringLiteral("/tmp/kdevtmpfsi")},
        {QStringLiteral("kinsing-loader"), QStringLiteral("/tmp/kinsing")}, {QStringLiteral("bpfdoor"), QStringLiteral("/dev/shm/kdmtmpflush")},
        {QStringLiteral("kinsing-var-tmp"), QStringLiteral("/var/tmp/kinsing")}, {QStringLiteral("xmrig-tmp"), QStringLiteral("/tmp/xmrig")},
        {QStringLiteral("xmrig-dev-shm"), QStringLiteral("/dev/shm/xmrig")}
    };
    for (const auto &[name, path] : malware)
        checks << check(QStringLiteral("malware-") + name, QStringLiteral("No %1 malware artifact").arg(name), QStringLiteral("Malware"), QStringLiteral("rootkit"), path);
    checks << check(QStringLiteral("malware-known-process"), QStringLiteral("No known miner or malware process is running"), QStringLiteral("Malware"), QStringLiteral("malware-process"));
    checks << check(QStringLiteral("malware-masquerading-process"), QStringLiteral("No program poses as a kernel thread"), QStringLiteral("Malware"), QStringLiteral("masquerade"));
    checks << check(QStringLiteral("malware-temp-process"), QStringLiteral("No program runs from a temporary folder"), QStringLiteral("Malware"), QStringLiteral("temp-process"));
    checks << check(QStringLiteral("malware-deleted-process"), QStringLiteral("Every running program has its file on disk"), QStringLiteral("Malware"), QStringLiteral("deleted-process"));
    const QList<QPair<QString, QString>> tempFolders = {{QStringLiteral("/dev/shm"), QStringLiteral("warning")}, {QStringLiteral("/tmp"), QStringLiteral("review")},
                                                        {QStringLiteral("/var/tmp"), QStringLiteral("review")}};
    for (const auto &[folder, level] : tempFolders)
        checks << check(QStringLiteral("malware-executables-") + pathId(folder), QStringLiteral("No programs are stored in %1").arg(folder), QStringLiteral("Malware"), QStringLiteral("temp-executables"), folder, level);
    const QList<QPair<QString, int>> backdoors = {{QStringLiteral("Metasploit"), 4444}, {QStringLiteral("Elite backdoor"), 31337}, {QStringLiteral("Leet backdoor"), 1337},
        {QStringLiteral("NetBus"), 12345}, {QStringLiteral("IRC botnet"), 6667}, {QStringLiteral("Android Debug Bridge"), 5555}, {QStringLiteral("Bind shell"), 54321}};
    for (const auto &[name, port] : backdoors)
        checks << check(QStringLiteral("backdoor-") + QString::number(port), QStringLiteral("%1 port %2 is not exposed").arg(name, QString::number(port)), QStringLiteral("Malware"), QStringLiteral("backdoor-port"), QString::number(port));
    struct Port {
        QString name;
        int port;
        QString level;
    };
    const QList<Port> ports = {{QStringLiteral("FTP"), 21, {}}, {QStringLiteral("SSH"), 22, {}}, {QStringLiteral("Telnet"), 23, QStringLiteral("warning")},
        {QStringLiteral("SMTP"), 25, {}}, {QStringLiteral("DNS"), 53, {}}, {QStringLiteral("HTTP"), 80, {}}, {QStringLiteral("RPC"), 111, {}},
        {QStringLiteral("NetBIOS"), 139, {}}, {QStringLiteral("SMB"), 445, {}}, {QStringLiteral("Rsync"), 873, {}}, {QStringLiteral("MySQL"), 3306, {}},
        {QStringLiteral("PostgreSQL"), 5432, {}}, {QStringLiteral("Redis"), 6379, QStringLiteral("warning")}, {QStringLiteral("Docker"), 2375, QStringLiteral("critical")},
        {QStringLiteral("POP3"), 110, {}}, {QStringLiteral("IMAP"), 143, {}}, {QStringLiteral("LDAP"), 389, {}}, {QStringLiteral("Printing (IPP)"), 631, {}},
        {QStringLiteral("SQL Server"), 1433, {}}, {QStringLiteral("Docker TLS"), 2376, {}}, {QStringLiteral("Remote Desktop"), 3389, {}},
        {QStringLiteral("VNC"), 5900, QStringLiteral("warning")}, {QStringLiteral("CouchDB"), 5984, QStringLiteral("warning")}, {QStringLiteral("X11"), 6000, QStringLiteral("warning")},
        {QStringLiteral("Kubernetes API"), 6443, {}}, {QStringLiteral("Jupyter"), 8888, QStringLiteral("warning")}, {QStringLiteral("Elasticsearch"), 9200, QStringLiteral("warning")},
        {QStringLiteral("Kubelet"), 10250, QStringLiteral("warning")}, {QStringLiteral("Memcached"), 11211, QStringLiteral("warning")}, {QStringLiteral("MongoDB"), 27017, QStringLiteral("warning")},
        {QStringLiteral("MQTT"), 1883, {}}, {QStringLiteral("NFS"), 2049, {}}, {QStringLiteral("etcd"), 2379, QStringLiteral("warning")},
        {QStringLiteral("Kibana"), 5601, {}}, {QStringLiteral("Webmin"), 10000, {}}};
    for (const Port &port : ports)
        checks << check(QStringLiteral("port-") + QString::number(port.port), port.name + QStringLiteral(" is not exposed"), QStringLiteral("Network exposure"), QStringLiteral("port"), QString::number(port.port), port.level);
    const QList<QPair<QString, int>> udpPorts = {{QStringLiteral("TFTP"), 69}, {QStringLiteral("RPC"), 111}, {QStringLiteral("NTP server"), 123},
        {QStringLiteral("NetBIOS name service"), 137}, {QStringLiteral("SNMP"), 161}, {QStringLiteral("SSDP/UPnP"), 1900},
        {QStringLiteral("mDNS"), 5353}, {QStringLiteral("LLMNR"), 5355}, {QStringLiteral("Memcached UDP"), 11211}};
    for (const auto &[name, port] : udpPorts)
        checks << check(QStringLiteral("udp-") + QString::number(port), name + QStringLiteral(" is not exposed over UDP"), QStringLiteral("Network exposure"), QStringLiteral("udp-port"), QString::number(port));
    checks << check(QStringLiteral("port-other-tcp"), QStringLiteral("No other TCP ports are open to the network"), QStringLiteral("Network exposure"), QStringLiteral("other-ports"), QStringLiteral("tcp"));
    checks << check(QStringLiteral("port-other-udp"), QStringLiteral("No other UDP ports are open to the network"), QStringLiteral("Network exposure"), QStringLiteral("other-ports"), QStringLiteral("udp"));
    checks << check(QStringLiteral("network-promiscuous"), QStringLiteral("No network interface captures all traffic"), QStringLiteral("Network exposure"), QStringLiteral("promiscuous"));
    const QList<QPair<QString, QString>> sysctls = {
        {QStringLiteral("kernel.kptr_restrict"), QStringLiteral("2|1")}, {QStringLiteral("kernel.dmesg_restrict"), QStringLiteral("1")},
        {QStringLiteral("kernel.yama.ptrace_scope"), QStringLiteral("1|2|3")}, {QStringLiteral("kernel.unprivileged_bpf_disabled"), QStringLiteral("1|2")},
        {QStringLiteral("kernel.randomize_va_space"), QStringLiteral("2")}, {QStringLiteral("fs.protected_hardlinks"), QStringLiteral("1")},
        {QStringLiteral("fs.protected_symlinks"), QStringLiteral("1")}, {QStringLiteral("fs.protected_fifos"), QStringLiteral("1|2")},
        {QStringLiteral("fs.protected_regular"), QStringLiteral("1|2")}, {QStringLiteral("net.ipv4.ip_forward"), QStringLiteral("0")},
        {QStringLiteral("net.ipv4.conf.all.send_redirects"), QStringLiteral("0")}, {QStringLiteral("net.ipv4.conf.default.send_redirects"), QStringLiteral("0")},
        {QStringLiteral("net.ipv4.conf.all.accept_redirects"), QStringLiteral("0")}, {QStringLiteral("net.ipv4.conf.default.accept_redirects"), QStringLiteral("0")},
        {QStringLiteral("net.ipv4.conf.all.secure_redirects"), QStringLiteral("1")}, {QStringLiteral("net.ipv4.conf.default.secure_redirects"), QStringLiteral("1")},
        {QStringLiteral("net.ipv4.conf.all.rp_filter"), QStringLiteral("1|2")}, {QStringLiteral("net.ipv4.conf.default.rp_filter"), QStringLiteral("1|2")},
        {QStringLiteral("net.ipv4.conf.all.log_martians"), QStringLiteral("1")}, {QStringLiteral("net.ipv4.conf.default.log_martians"), QStringLiteral("1")},
        {QStringLiteral("net.ipv4.tcp_syncookies"), QStringLiteral("1")}, {QStringLiteral("net.ipv4.icmp_echo_ignore_broadcasts"), QStringLiteral("1")},
        {QStringLiteral("net.ipv4.icmp_ignore_bogus_error_responses"), QStringLiteral("1")}, {QStringLiteral("net.ipv6.conf.all.accept_redirects"), QStringLiteral("0")},
        {QStringLiteral("net.ipv6.conf.default.accept_redirects"), QStringLiteral("0")}, {QStringLiteral("net.ipv6.conf.all.forwarding"), QStringLiteral("0")},
        {QStringLiteral("net.ipv6.conf.default.router_solicitations"), QStringLiteral("0")}, {QStringLiteral("net.ipv4.conf.all.accept_source_route"), QStringLiteral("0")},
        {QStringLiteral("net.ipv4.conf.default.accept_source_route"), QStringLiteral("0")}, {QStringLiteral("net.ipv6.conf.all.accept_source_route"), QStringLiteral("0")},
        {QStringLiteral("net.ipv6.conf.default.accept_source_route"), QStringLiteral("0")}, {QStringLiteral("net.ipv6.conf.default.forwarding"), QStringLiteral("0")},
        {QStringLiteral("net.ipv4.tcp_rfc1337"), QStringLiteral("1")}, {QStringLiteral("net.core.bpf_jit_harden"), QStringLiteral("2")},
        {QStringLiteral("kernel.kexec_load_disabled"), QStringLiteral("1")}, {QStringLiteral("kernel.perf_event_paranoid"), QStringLiteral("2|3|4")},
        {QStringLiteral("fs.suid_dumpable"), QStringLiteral("0")}, {QStringLiteral("dev.tty.ldisc_autoload"), QStringLiteral("0")},
        {QStringLiteral("dev.tty.legacy_tiocsti"), QStringLiteral("0")}, {QStringLiteral("vm.unprivileged_userfaultfd"), QStringLiteral("0")},
        {QStringLiteral("vm.mmap_min_addr"), QStringLiteral("65536")}
    };
    for (const auto &[name, expected] : sysctls) {
        QString id = name;
        checks << check(QStringLiteral("sysctl-") + id.replace(QLatin1Char('.'), QLatin1Char('-')), name, QStringLiteral("Kernel hardening"), QStringLiteral("sysctl"), name, expected);
    }
    checks << check(QStringLiteral("kernel-mac"), QStringLiteral("AppArmor or SELinux is enforcing"), QStringLiteral("Kernel hardening"), QStringLiteral("mac"));
    checks << check(QStringLiteral("kernel-lockdown"), QStringLiteral("Kernel lockdown is active"), QStringLiteral("Kernel hardening"), QStringLiteral("lockdown"), QStringLiteral("/sys/kernel/security/lockdown"));
    checks << check(QStringLiteral("kernel-tainted"), QStringLiteral("The kernel is not tainted"), QStringLiteral("Kernel hardening"), QStringLiteral("tainted"));
    checks << check(QStringLiteral("kernel-secure-boot"), QStringLiteral("Secure Boot is on"), QStringLiteral("Kernel hardening"), QStringLiteral("secure-boot"));
    const QStringList modules = {QStringLiteral("cramfs"), QStringLiteral("freevxfs"), QStringLiteral("jffs2"), QStringLiteral("hfs"),
        QStringLiteral("hfsplus"), QStringLiteral("udf"), QStringLiteral("dccp"), QStringLiteral("sctp"), QStringLiteral("rds"),
        QStringLiteral("tipc"), QStringLiteral("firewire_core"), QStringLiteral("n_hdlc")};
    for (const QString &name : modules)
        checks << check(QStringLiteral("module-") + name, QStringLiteral("The %1 module is not loaded").arg(name), QStringLiteral("Kernel modules"), QStringLiteral("module"), name);
    const QList<QPair<QString, QString>> accounts = {{QStringLiteral("account-uid-zero"), QStringLiteral("Only root has user ID 0")},
        {QStringLiteral("account-empty-password"), QStringLiteral("No account has an empty password")},
        {QStringLiteral("account-shadowed"), QStringLiteral("Passwords are kept out of /etc/passwd")},
        {QStringLiteral("account-duplicate-uid"), QStringLiteral("Every account has its own user ID")},
        {QStringLiteral("account-legacy-entries"), QStringLiteral("No legacy NIS \"+\" entries")}};
    for (const auto &[id, title] : accounts)
        checks << check(id, title, QStringLiteral("Accounts"), id);
    checks << check(QStringLiteral("account-docker-group"), QStringLiteral("Nobody gets root through the docker group"), QStringLiteral("Accounts"), QStringLiteral("docker-group"));
    checks << check(QStringLiteral("account-sudo-nopasswd"), QStringLiteral("sudo asks for a password"), QStringLiteral("Accounts"), QStringLiteral("sudoers"));
    checks << check(QStringLiteral("account-autologin"), QStringLiteral("The desktop does not sign in automatically"), QStringLiteral("Accounts"), QStringLiteral("autologin"));
    checks << check(QStringLiteral("login-encrypt-method"), QStringLiteral("New passwords use SHA-512 or yescrypt"), QStringLiteral("Accounts"), QStringLiteral("config"), QStringLiteral("/etc/login.defs"), QStringLiteral("ENCRYPT_METHOD=SHA512|YESCRYPT"));
    checks << check(QStringLiteral("login-umask"), QStringLiteral("New files are private by default"), QStringLiteral("Accounts"), QStringLiteral("config"), QStringLiteral("/etc/login.defs"), QStringLiteral("UMASK=027|077"));
    checks << check(QStringLiteral("login-pass-max-days"), QStringLiteral("Passwords expire within a year"), QStringLiteral("Accounts"), QStringLiteral("config-max"), QStringLiteral("/etc/login.defs"), QStringLiteral("PASS_MAX_DAYS=365"));
    const QList<QPair<QString, QString>> privateFiles = {{QStringLiteral("/etc/shadow"), QStringLiteral("critical")}, {QStringLiteral("/etc/gshadow"), QStringLiteral("critical")},
        {QStringLiteral("/etc/sudoers"), QStringLiteral("critical")}, {QStringLiteral("/etc/ssh/sshd_config"), {}}, {QStringLiteral("/etc/ssh/ssh_host_rsa_key"), QStringLiteral("critical")},
        {QStringLiteral("/etc/ssh/ssh_host_ecdsa_key"), QStringLiteral("critical")}, {QStringLiteral("/etc/ssh/ssh_host_ed25519_key"), QStringLiteral("critical")},
        {QStringLiteral("/etc/cron.allow"), {}}, {QStringLiteral("/etc/at.allow"), {}}, {QStringLiteral("/etc/security/opasswd"), {}}, {QStringLiteral("/boot/grub/grub.cfg"), {}},
        {QStringLiteral("/etc/krb5.keytab"), QStringLiteral("critical")}, {QStringLiteral("/root"), {}}, {QStringLiteral("/etc/shadow-"), QStringLiteral("critical")},
        {QStringLiteral("/etc/gshadow-"), QStringLiteral("critical")}};
    for (const auto &[path, level] : privateFiles)
        checks << check(QStringLiteral("permissions-") + pathId(path), QStringLiteral("%1 is private").arg(path), QStringLiteral("File permissions"), QStringLiteral("private-file"), path, level);
    const QList<QPair<QString, QString>> protectedFiles = {{QStringLiteral("/etc/passwd"), QStringLiteral("critical")}, {QStringLiteral("/etc/group"), QStringLiteral("critical")},
        {QStringLiteral("/etc/hosts"), {}}, {QStringLiteral("/etc/crontab"), QStringLiteral("critical")}, {QStringLiteral("/etc/cron.d"), QStringLiteral("critical")},
        {QStringLiteral("/etc/profile"), QStringLiteral("critical")}, {QStringLiteral("/etc/environment"), {}}, {QStringLiteral("/etc/ld.so.conf"), QStringLiteral("critical")},
        {QStringLiteral("/etc/pam.d"), QStringLiteral("critical")}, {QStringLiteral("/etc/systemd/system"), QStringLiteral("critical")}, {QStringLiteral("/etc/resolv.conf"), {}},
        {QStringLiteral("/etc/fstab"), {}}, {QStringLiteral("/usr/local/bin"), QStringLiteral("critical")}, {QStringLiteral("/usr/local/sbin"), QStringLiteral("critical")},
        {QStringLiteral("/etc/sudoers.d"), QStringLiteral("critical")}, {QStringLiteral("/etc/profile.d"), QStringLiteral("critical")},
        {QStringLiteral("/etc/cron.hourly"), QStringLiteral("critical")}, {QStringLiteral("/etc/cron.daily"), QStringLiteral("critical")},
        {QStringLiteral("/etc/init.d"), QStringLiteral("critical")}, {QStringLiteral("/etc/ld.so.conf.d"), QStringLiteral("critical")},
        {QStringLiteral("/usr/bin"), QStringLiteral("critical")}, {QStringLiteral("/usr/sbin"), QStringLiteral("critical")},
        {QStringLiteral("/usr/lib"), QStringLiteral("critical")}, {QStringLiteral("/etc/ssh"), QStringLiteral("critical")}};
    for (const auto &[path, level] : protectedFiles)
        checks << check(QStringLiteral("writable-") + pathId(path), QStringLiteral("%1 is not writable by everyone").arg(path), QStringLiteral("File permissions"), QStringLiteral("world-writable"), path, level);
    checks << check(QStringLiteral("permissions-etc-world-writable"), QStringLiteral("No file in /etc is writable by everyone"), QStringLiteral("File permissions"), QStringLiteral("etc-writable"));
    checks << check(QStringLiteral("permissions-docker-socket"), QStringLiteral("The Docker socket is not open to everyone"), QStringLiteral("File permissions"), QStringLiteral("docker-socket"));
    checks << check(QStringLiteral("permissions-path-variable"), QStringLiteral("PATH has no unsafe folders"), QStringLiteral("File permissions"), QStringLiteral("path-variable"));
    checks << check(QStringLiteral("permissions-home-folder"), QStringLiteral("Your home folder is private"), QStringLiteral("File permissions"), QStringLiteral("home-folder"));
    for (const QString &path : {QStringLiteral("/tmp"), QStringLiteral("/var/tmp"), QStringLiteral("/dev/shm")})
        checks << check(QStringLiteral("sticky-") + pathId(path), QStringLiteral("%1 has the sticky bit").arg(path), QStringLiteral("File permissions"), QStringLiteral("sticky-dir"), path);
    const QStringList secrets = {QStringLiteral(".ssh"), QStringLiteral(".ssh/id_rsa"), QStringLiteral(".ssh/id_ecdsa"), QStringLiteral(".ssh/id_ed25519"),
        QStringLiteral(".ssh/id_dsa"), QStringLiteral(".gnupg"), QStringLiteral(".git-credentials"), QStringLiteral(".netrc"), QStringLiteral(".aws/credentials"),
        QStringLiteral(".pgpass"), QStringLiteral(".docker/config.json"), QStringLiteral(".kube/config"), QStringLiteral(".config/gh/hosts.yml"),
        QStringLiteral(".pypirc"), QStringLiteral(".npmrc")};
    for (const QString &path : secrets)
        checks << check(QStringLiteral("secrets-") + pathId(path), QStringLiteral("~/%1 is private").arg(path), QStringLiteral("Secrets"), QStringLiteral("home-private-file"), path);
    checks << check(QStringLiteral("secrets-ssh-key-passphrase"), QStringLiteral("SSH private keys have a passphrase"), QStringLiteral("Secrets"), QStringLiteral("ssh-key-passphrase"));
    checks << check(QStringLiteral("secrets-authorized-keys"), QStringLiteral("Only keys you recognise can sign in over SSH"), QStringLiteral("Secrets"), QStringLiteral("authorized-keys"));
    const QStringList escalation = {QStringLiteral("/usr/bin/bash"), QStringLiteral("/usr/bin/dash"), QStringLiteral("/usr/bin/zsh"),
        QStringLiteral("/usr/bin/python3"), QStringLiteral("/usr/bin/perl"), QStringLiteral("/usr/bin/find"), QStringLiteral("/usr/bin/vim.basic"),
        QStringLiteral("/usr/bin/less"), QStringLiteral("/usr/bin/env"), QStringLiteral("/usr/bin/tar"), QStringLiteral("/usr/bin/cp"),
        QStringLiteral("/usr/bin/nmap"), QStringLiteral("/usr/bin/awk"), QStringLiteral("/usr/bin/gawk"), QStringLiteral("/usr/bin/sed"),
        QStringLiteral("/usr/bin/node"), QStringLiteral("/usr/bin/ruby"), QStringLiteral("/usr/bin/php"), QStringLiteral("/usr/bin/lua"),
        QStringLiteral("/usr/bin/docker"), QStringLiteral("/usr/bin/tee"), QStringLiteral("/usr/bin/dd"), QStringLiteral("/usr/bin/rsync"),
        QStringLiteral("/usr/bin/git"), QStringLiteral("/usr/bin/socat"), QStringLiteral("/usr/bin/nc.openbsd"), QStringLiteral("/usr/bin/wget"),
        QStringLiteral("/usr/bin/curl"), QStringLiteral("/usr/bin/chmod"), QStringLiteral("/usr/bin/chown")};
    for (const QString &path : escalation)
        checks << check(QStringLiteral("setuid-") + QFileInfo(path).fileName().replace(QLatin1Char('.'), QLatin1Char('-')), QStringLiteral("%1 is not set-user-ID").arg(path), QStringLiteral("Privilege escalation"), QStringLiteral("setuid"), path);
    const QList<QPair<QString, QString>> mountOptions = {{QStringLiteral("/tmp"), QStringLiteral("nodev")}, {QStringLiteral("/tmp"), QStringLiteral("nosuid")},
        {QStringLiteral("/dev/shm"), QStringLiteral("nodev")}, {QStringLiteral("/dev/shm"), QStringLiteral("nosuid")}, {QStringLiteral("/dev/shm"), QStringLiteral("noexec")},
        {QStringLiteral("/var/tmp"), QStringLiteral("nosuid")}};
    for (const auto &[path, option] : mountOptions)
        checks << check(QStringLiteral("mount-") + pathId(path) + QLatin1Char('-') + option, QStringLiteral("%1 is mounted %2").arg(path, option), QStringLiteral("Mounts"), QStringLiteral("mount-option"), path, option);
    const QList<QPair<QString, QString>> ssh = {{QStringLiteral("PermitRootLogin"), QStringLiteral("no")}, {QStringLiteral("PasswordAuthentication"), QStringLiteral("no")},
        {QStringLiteral("PermitEmptyPasswords"), QStringLiteral("no")}, {QStringLiteral("X11Forwarding"), QStringLiteral("no")},
        {QStringLiteral("AllowTcpForwarding"), QStringLiteral("no")}, {QStringLiteral("PermitTunnel"), QStringLiteral("no")},
        {QStringLiteral("IgnoreRhosts"), QStringLiteral("yes")}, {QStringLiteral("HostbasedAuthentication"), QStringLiteral("no")},
        {QStringLiteral("UsePAM"), QStringLiteral("yes")}, {QStringLiteral("LogLevel"), QStringLiteral("verbose")},
        {QStringLiteral("PermitUserEnvironment"), QStringLiteral("no")}, {QStringLiteral("AllowAgentForwarding"), QStringLiteral("no")},
        {QStringLiteral("GatewayPorts"), QStringLiteral("no")}, {QStringLiteral("KbdInteractiveAuthentication"), QStringLiteral("no")},
        {QStringLiteral("StrictModes"), QStringLiteral("yes")}};
    for (const auto &[name, value] : ssh)
        checks << check(QStringLiteral("ssh-") + name.toLower(), QStringLiteral("SSH %1 is %2").arg(name, value), QStringLiteral("Remote access"), QStringLiteral("config"), QStringLiteral("/etc/ssh/sshd_config"), name + QLatin1Char('=') + value);
    const QList<QPair<QString, QString>> sshLimits = {{QStringLiteral("MaxAuthTries"), QStringLiteral("4")}, {QStringLiteral("LoginGraceTime"), QStringLiteral("60")},
                                                      {QStringLiteral("MaxSessions"), QStringLiteral("10")}};
    for (const auto &[name, limit] : sshLimits)
        checks << check(QStringLiteral("ssh-") + name.toLower(), QStringLiteral("SSH %1 is at most %2").arg(name, limit), QStringLiteral("Remote access"), QStringLiteral("config-max"), QStringLiteral("/etc/ssh/sshd_config"), name + QLatin1Char('=') + limit);
    checks << check(QStringLiteral("ssh-weak-crypto"), QStringLiteral("SSH uses no weak algorithms"), QStringLiteral("Remote access"), QStringLiteral("ssh-crypto"));
    struct Safeguard {
        QString id;
        QString title;
        QString paths;
    };
    const QList<Safeguard> safeguards = {
        {QStringLiteral("automatic-updates"), QStringLiteral("Automatic security updates are configured"),
         QStringLiteral("/etc/apt/apt.conf.d/20auto-upgrades|/etc/dnf/automatic.conf|/etc/yum/yum-cron.conf")},
        {QStringLiteral("audit-daemon"), QStringLiteral("The audit daemon is configured"), QStringLiteral("/etc/audit/auditd.conf")},
        {QStringLiteral("brute-force-protection"), QStringLiteral("Sign-in brute-force protection is configured"),
         QStringLiteral("/etc/fail2ban/jail.conf|/etc/sshguard/sshguard.conf")},
        {QStringLiteral("file-integrity"), QStringLiteral("File integrity monitoring is configured"), QStringLiteral("/etc/aide/aide.conf|/etc/aide.conf")},
        {QStringLiteral("usb-device-policy"), QStringLiteral("A USB device policy is configured"), QStringLiteral("/etc/usbguard/usbguard-daemon.conf")}};
    for (const Safeguard &safeguard : safeguards)
        checks << check(QStringLiteral("safeguard-") + safeguard.id, safeguard.title, QStringLiteral("Safeguards"), QStringLiteral("present"), safeguard.paths);
    checks << check(QStringLiteral("safeguard-disk-encryption"), QStringLiteral("An encrypted volume is configured"), QStringLiteral("Safeguards"), QStringLiteral("entries"), QStringLiteral("/etc/crypttab"));
    checks << check(QStringLiteral("safeguard-security-updates"), QStringLiteral("No security updates are waiting"), QStringLiteral("Safeguards"), QStringLiteral("updates-pending"));
    checks << check(QStringLiteral("safeguard-reboot-pending"), QStringLiteral("No restart is pending for updates"), QStringLiteral("Safeguards"), QStringLiteral("reboot-required"));
    checks << check(QStringLiteral("persistence-cron"), QStringLiteral("Scheduled jobs look normal"), QStringLiteral("Persistence"), QStringLiteral("persist-cron"));
    checks << check(QStringLiteral("persistence-systemd"), QStringLiteral("Custom services look normal"), QStringLiteral("Persistence"), QStringLiteral("persist-systemd"));
    checks << check(QStringLiteral("persistence-autostart"), QStringLiteral("Desktop autostart entries look normal"), QStringLiteral("Persistence"), QStringLiteral("persist-autostart"));
    checks << check(QStringLiteral("persistence-shell-startup"), QStringLiteral("Shell start-up files look normal"), QStringLiteral("Persistence"), QStringLiteral("persist-shell"));
    checks << check(QStringLiteral("persistence-rc-local"), QStringLiteral("/etc/rc.local runs nothing unexpected"), QStringLiteral("Persistence"), QStringLiteral("persist-rc-local"));
    checks << check(QStringLiteral("persistence-library-path"), QStringLiteral("Libraries load only from system folders"), QStringLiteral("Persistence"), QStringLiteral("persist-ld-config"));
    checks << check(QStringLiteral("persistence-user-ssh-rc"), QStringLiteral("No command runs at each SSH sign-in"), QStringLiteral("Persistence"), QStringLiteral("home-exists"), QStringLiteral(".ssh/rc"));
    checks << check(QStringLiteral("service-nfs-root-squash"), QStringLiteral("NFS shares do not trust remote root"), QStringLiteral("Service exposure"), QStringLiteral("nfs-exports"));
    checks << check(QStringLiteral("service-samba-guest"), QStringLiteral("Samba shares need a password"), QStringLiteral("Service exposure"), QStringLiteral("samba-guest"));
    checks << check(QStringLiteral("service-docker-tcp"), QStringLiteral("Docker's API is not open on the network"), QStringLiteral("Service exposure"), QStringLiteral("docker-tcp"));
    checks << check(QStringLiteral("service-redis-auth"), QStringLiteral("Redis is not open without a password"), QStringLiteral("Service exposure"), QStringLiteral("redis-auth"));
    const QStringList optionalServices = {QStringLiteral("/etc/xinetd.conf"), QStringLiteral("/etc/inetd.conf"), QStringLiteral("/etc/exports"),
        QStringLiteral("/etc/samba/smb.conf"), QStringLiteral("/etc/vsftpd.conf"), QStringLiteral("/etc/apache2/apache2.conf"),
        QStringLiteral("/etc/nginx/nginx.conf"), QStringLiteral("/etc/docker/daemon.json"), QStringLiteral("/etc/cups/cupsd.conf"),
        QStringLiteral("/etc/postfix/main.cf"), QStringLiteral("/etc/bind/named.conf"), QStringLiteral("/etc/rsyncd.conf"),
        QStringLiteral("/etc/openvpn/server.conf"), QStringLiteral("/etc/wireguard/wg0.conf"), QStringLiteral("/etc/cron.deny"),
        QStringLiteral("/etc/at.deny"), QStringLiteral("/etc/nfs.conf"), QStringLiteral("/etc/libvirt/libvirtd.conf"),
        QStringLiteral("/etc/snmp/snmpd.conf"), QStringLiteral("/etc/tor/torrc"), QStringLiteral("/etc/mosquitto/mosquitto.conf"),
        QStringLiteral("/etc/kubernetes/kubelet.conf"), QStringLiteral("/etc/iscsi/iscsid.conf"), QStringLiteral("/etc/tftp.conf"),
        QStringLiteral("/etc/squid/squid.conf"), QStringLiteral("/etc/avahi/avahi-daemon.conf"), QStringLiteral("/etc/dnsmasq.conf"),
        QStringLiteral("/etc/xrdp/xrdp.ini"), QStringLiteral("/etc/redis/redis.conf"), QStringLiteral("/etc/mongod.conf"),
        QStringLiteral("/etc/memcached.conf"), QStringLiteral("/etc/hosts.equiv"), QStringLiteral("/root/.rhosts"),
        QStringLiteral("/etc/ssh/ssh_host_dsa_key"), QStringLiteral("/etc/ssh/sshrc")};
    for (const QString &path : optionalServices)
        checks << check(QStringLiteral("review-") + pathId(path), QStringLiteral("Review %1").arg(path), QStringLiteral("Service exposure"), QStringLiteral("optional"), path);
    for (CheckDefinition &definition : checks)
        definition.explanation = explainCheck(definition);
    return checks;
}

QList<Finding> scan(const ScanContext &context)
{
    const QList<CheckDefinition> catalog = checkCatalog();
    Snapshot state = snapshot(context);
    for (const CheckDefinition &definition : catalog) {
        if (definition.probe == QStringLiteral("port") || definition.probe == QStringLiteral("backdoor-port"))
            state.knownTcp.insert(definition.target.toInt());
        else if (definition.probe == QStringLiteral("udp-port"))
            state.knownUdp.insert(definition.target.toInt());
    }
    QList<Finding> findings;
    for (const CheckDefinition &definition : catalog)
        findings << evaluate(definition, context, state);
    return findings;
}

QString stateName(FindingState state)
{
    switch (state) {
    case FindingState::Pass: return QStringLiteral("Pass");
    case FindingState::Review: return QStringLiteral("Review");
    case FindingState::Warning: return QStringLiteral("Warning");
    case FindingState::Critical: return QStringLiteral("Critical");
    case FindingState::Unavailable: return QStringLiteral("Unavailable");
    }
    return {};
}

int severityRank(FindingState state)
{
    switch (state) {
    case FindingState::Critical: return 0;
    case FindingState::Warning: return 1;
    case FindingState::Review: return 2;
    case FindingState::Unavailable: return 3;
    case FindingState::Pass: return 4;
    }
    return 5;
}

QString stateSummary(const QList<Finding> &findings)
{
    QMap<FindingState, int> counts;
    for (const Finding &finding : findings)
        ++counts[finding.state];
    return QStringLiteral("%1 checks: %2 pass, %3 need review, %4 warnings, %5 critical, %6 unavailable")
        .arg(findings.size()).arg(counts[FindingState::Pass]).arg(counts[FindingState::Review])
        .arg(counts[FindingState::Warning]).arg(counts[FindingState::Critical]).arg(counts[FindingState::Unavailable]);
}

QList<FindingFilter> filters()
{
    return {FindingFilter::All, FindingFilter::Attention, FindingFilter::Critical, FindingFilter::Warning, FindingFilter::Review,
            FindingFilter::Fixable, FindingFilter::Pass, FindingFilter::Unavailable};
}

QString filterName(FindingFilter filter)
{
    switch (filter) {
    case FindingFilter::All: return QStringLiteral("All");
    case FindingFilter::Attention: return QStringLiteral("Needs attention");
    case FindingFilter::Critical: return QStringLiteral("Critical");
    case FindingFilter::Warning: return QStringLiteral("Warnings");
    case FindingFilter::Review: return QStringLiteral("For review");
    case FindingFilter::Fixable: return QStringLiteral("Can fix");
    case FindingFilter::Pass: return QStringLiteral("Passed");
    case FindingFilter::Unavailable: return QStringLiteral("Unavailable");
    }
    return {};
}

bool matchesFilter(const Finding &finding, FindingFilter filter)
{
    switch (filter) {
    case FindingFilter::All: return true;
    case FindingFilter::Attention:
        return finding.state == FindingState::Critical || finding.state == FindingState::Warning || finding.state == FindingState::Review;
    case FindingFilter::Critical: return finding.state == FindingState::Critical;
    case FindingFilter::Warning: return finding.state == FindingState::Warning;
    case FindingFilter::Review: return finding.state == FindingState::Review;
    case FindingFilter::Fixable: return !finding.fix.script.isEmpty();
    case FindingFilter::Pass: return finding.state == FindingState::Pass;
    case FindingFilter::Unavailable: return finding.state == FindingState::Unavailable;
    }
    return false;
}

bool matchesQuery(const Finding &finding, const QString &query)
{
    const QString needle = query.trimmed();
    if (needle.isEmpty())
        return true;
    const QString haystack = finding.check.title + QLatin1Char(' ') + finding.check.category + QLatin1Char(' ') + finding.detail;
    return haystack.contains(needle, Qt::CaseInsensitive);
}

QString filteredSummary(const QList<Finding> &findings, int matched, const QString &query, FindingFilter filter, const QString &category)
{
    const QString needle = query.trimmed();
    QStringList scope;
    if (filter != FindingFilter::All)
        scope << filterName(filter);
    if (!category.isEmpty())
        scope << category;
    const QString scopeText = scope.join(QStringLiteral(" · "));
    if (needle.isEmpty() && scope.isEmpty())
        return stateSummary(findings);
    if (matched == 0) {
        if (scope.isEmpty())
            return QStringLiteral("No checks match \"%1\". Try a different search.").arg(needle);
        if (needle.isEmpty())
            return QStringLiteral("No checks in %1. Try a different filter.").arg(scopeText);
        return QStringLiteral("No checks in %1 match \"%2\". Try a different filter or search.").arg(scopeText, needle);
    }
    QString description;
    if (scope.isEmpty())
        description = QStringLiteral("matching \"%1\"").arg(needle);
    else if (needle.isEmpty())
        description = QStringLiteral("in %1").arg(scopeText);
    else
        description = QStringLiteral("in %1 matching \"%2\"").arg(scopeText, needle);
    return QStringLiteral("Showing %1 of %2 checks %3. %4").arg(QString::number(matched), QString::number(findings.size()), description, stateSummary(findings));
}

QString shellQuote(const QString &value)
{
    QString quoted = value;
    quoted.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + quoted + QLatin1Char('\'');
}

QString manualCommand(const FixAction &fix)
{
    if (fix.script.isEmpty())
        return {};
    return (fix.elevated ? QStringLiteral("sudo sh -c ") : QStringLiteral("sh -c ")) + shellQuote(fix.script);
}

}
