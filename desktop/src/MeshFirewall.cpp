#include "MeshFirewall.h"

#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

namespace {

const char kRuleComment[] = "ForkMesh local mesh";

// The value of a shell-style KEY=value line, unquoted and lower-cased. The
// last assignment wins, as it does when the file is sourced.
QString configValue(const QString &text, const QString &key)
{
    static const QRegularExpression assignment(
        QStringLiteral("^\\s*([A-Za-z_][A-Za-z0-9_]*)\\s*=\\s*"
                       "[\"']?([^\"'#\\r\\n]*?)[\"']?\\s*(?:#.*)?$"),
        QRegularExpression::MultilineOption);
    QString value;
    auto it = assignment.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        if (match.captured(1) == key)
            value = match.captured(2).trimmed().toLower();
    }
    return value;
}

#if defined(Q_OS_LINUX)
QString readSmallFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QString();
    return QString::fromUtf8(file.read(64 * 1024));
}

bool firewalldRunning()
{
    return QFileInfo::exists(QStringLiteral("/run/firewalld.pid")) ||
           QFileInfo::exists(QStringLiteral(
               "/sys/fs/cgroup/system.slice/firewalld.service"));
}
#endif

} // namespace

namespace forkmesh::meshfirewall {

QStringList localSourceRanges()
{
    return {QStringLiteral("10.0.0.0/8"), QStringLiteral("172.16.0.0/12"),
            QStringLiteral("192.168.0.0/16"), QStringLiteral("169.254.0.0/16")};
}

bool ufwBlocksInbound(const QString &ufwConf, const QString &ufwDefaults)
{
    if (configValue(ufwConf, QStringLiteral("ENABLED")) != QLatin1String("yes"))
        return false;
    // ufw ships with DROP, and treats a missing policy the same way.
    return configValue(ufwDefaults, QStringLiteral("DEFAULT_INPUT_POLICY")) !=
           QLatin1String("accept");
}

QString ufwAllowCommand(int port)
{
    // No `proto`, so one rule covers the UDP beacon and the TCP link. The
    // destination is left open because the beacon arrives addressed to the
    // multicast group, not to this machine.
    QStringList rules;
    const QStringList ranges = localSourceRanges();
    for (const QString &range : ranges)
        rules << QStringLiteral("ufw allow from %1 to any port %2 comment '%3'")
                     .arg(range)
                     .arg(port)
                     .arg(QLatin1String(kRuleComment));
    return rules.join(QStringLiteral(" && "));
}

QString firewalldAllowCommand(int port)
{
    // Once for the running firewall, once so it survives a restart.
    const QString ports =
        QStringLiteral("--add-port=%1/tcp --add-port=%1/udp").arg(port);
    return QStringLiteral("firewall-cmd %1 && firewall-cmd --permanent %1")
        .arg(ports);
}

QString windowsAllowCommand(int port)
{
    // Private and domain profiles only: a public network is not a place this
    // machine should accept connections from. The rule name has no spaces, so
    // the command survives every layer of quoting on the way to netsh.
    const QString rule = QStringLiteral(
        "netsh advfirewall firewall add rule name=ForkMeshLocalMesh-%1 dir=in "
        "action=allow protocol=%1 localport=%2 profile=private,domain");
    return rule.arg(QStringLiteral("TCP")).arg(port) + QStringLiteral(" && ") +
           rule.arg(QStringLiteral("UDP")).arg(port);
}

Plan detect(int port)
{
    Plan plan;
#if defined(Q_OS_LINUX)
    if (ufwBlocksInbound(readSmallFile(QStringLiteral("/etc/ufw/ufw.conf")),
                         readSmallFile(QStringLiteral("/etc/default/ufw")))) {
        plan.kind = Kind::Ufw;
        plan.blocksInbound = true;
        plan.firewallName = QStringLiteral("ufw");
        plan.privilegedCommand = ufwAllowCommand(port);
    } else if (firewalldRunning()) {
        plan.kind = Kind::Firewalld;
        plan.blocksInbound = true;
        plan.firewallName = QStringLiteral("firewalld");
        plan.privilegedCommand = firewalldAllowCommand(port);
    }
    if (plan.isValid()) {
        plan.displayCommand = plan.privilegedCommand;
        plan.displayCommand.replace(QStringLiteral(" && "),
                                    QStringLiteral(" && sudo "));
        plan.displayCommand.prepend(QStringLiteral("sudo "));
    }
#elif defined(Q_OS_WIN)
    plan.kind = Kind::Windows;
    plan.firewallName = QStringLiteral("Windows Firewall");
    plan.privilegedCommand = windowsAllowCommand(port);
    plan.displayCommand = plan.privilegedCommand;
#else
    Q_UNUSED(port);
#endif
    if (plan.isValid())
        plan.settingsStamp =
            QStringLiteral("%1:%2").arg(plan.firewallName).arg(port);
    return plan;
}

} // namespace forkmesh::meshfirewall
