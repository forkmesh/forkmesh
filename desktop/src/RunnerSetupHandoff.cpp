#include "RunnerSetupHandoff.h"

#include <QRegularExpression>
#include <QUrl>

namespace forkmesh::runners {

namespace {

const QRegularExpression &tokenPattern()
{
    static const QRegularExpression pattern(
        QStringLiteral("^fmrt_[0-9a-f]{64}$"));
    return pattern;
}

const QRegularExpression &namePattern()
{
    static const QRegularExpression pattern(
        QStringLiteral("^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$"));
    return pattern;
}

const QRegularExpression &originPattern()
{
    static const QRegularExpression pattern(
        QStringLiteral("^https://[A-Za-z0-9.-]+(?::[0-9]{1,5})?$"));
    return pattern;
}

QString shellQuote(const QString &value)
{
    QString out = value;
    out.replace(QStringLiteral("'"), QStringLiteral("'\\''"));
    return QStringLiteral("'") + out + QStringLiteral("'");
}

}

bool RunnerSetupHandoff::isValid() const
{
    return tokenPattern().match(token).hasMatch() &&
        namePattern().match(name).hasMatch() &&
        originPattern().match(appUrl).hasMatch();
}

RunnerSetupHandoff parseRunnerSetupHandoff(const QString &role,
                                           const QString &token,
                                           const QString &name,
                                           const QString &appUrl)
{
    RunnerSetupHandoff handoff;
    if (role != QLatin1String("runner"))
        return handoff;
    QString origin = appUrl.trimmed();
    while (origin.endsWith(QLatin1Char('/')))
        origin.chop(1);
    handoff.token = token.trimmed();
    handoff.name = name.trimmed().toLower();
    handoff.appUrl = origin;
    if (!handoff.isValid())
        return RunnerSetupHandoff();
    return handoff;
}

QString runnerInstallScriptUrl(const QString &installScriptUrl)
{
    const QUrl source(installScriptUrl);
    if (!source.isValid() || source.scheme() != QLatin1String("https") ||
        source.host().isEmpty() || !source.userInfo().isEmpty())
        return QStringLiteral("https://forkmesh.com/install-runner.sh");
    QUrl url;
    url.setScheme(QStringLiteral("https"));
    url.setHost(source.host());
    if (source.port() > 0)
        url.setPort(source.port());
    url.setPath(QStringLiteral("/install-runner.sh"));
    return url.toString(QUrl::FullyEncoded);
}

QString runnerInstallRemoteCommand(const RunnerSetupHandoff &handoff,
                                   const QString &scriptUrl,
                                   const QString &sshUser,
                                   bool sudoPassword)
{
    if (!handoff.isValid() || !scriptUrl.startsWith(QLatin1String("https://")))
        return QString();
    const bool root = sshUser == QLatin1String("root");
    const QString line = QStringLiteral(
        "curl -fsSL %1 | %2bash -s -- --token %3 --name %4 --app %5")
        .arg(shellQuote(scriptUrl),
             root ? QString() : QStringLiteral("sudo -n "),
             shellQuote(handoff.token), shellQuote(handoff.name),
             shellQuote(handoff.appUrl));
    if (root)
        return line;
    if (sudoPassword)
        return QStringLiteral("sudo -S -p '' -v && ") + line;
    return QStringLiteral(
        "sudo -n true 2>/dev/null || { echo 'This user needs passwordless "
        "sudo, or use root, or sign in with the password.' >&2; exit 12; } "
        "&& ") + line;
}

QString redactRunnerToken(const QString &text)
{
    static const QRegularExpression token(
        QStringLiteral("fmrt_[0-9a-f]{8,}"));
    QString out = text;
    out.replace(token, QStringLiteral("fmrt_[hidden]"));
    return out;
}

QStringList parseHostKeyFingerprints(const QString &keygenOutput)
{
    static const QRegularExpression line(
        QStringLiteral("^\\s*\\d+\\s+(SHA256:[A-Za-z0-9+/]{43})\\s+.*\\((\\w+)\\)\\s*$"));
    QStringList result;
    const QStringList lines = keygenOutput.split(QLatin1Char('\n'));
    for (const QString &text : lines) {
        const QRegularExpressionMatch match = line.match(text);
        if (!match.hasMatch())
            continue;
        const QString entry = match.captured(2) + QLatin1Char(' ') +
            match.captured(1);
        if (!result.contains(entry))
            result << entry;
    }
    return result;
}

QString hostKeyConfirmationText(const QString &host,
                                const QStringList &fingerprints)
{
    return QStringLiteral(
        "This is the first SSH connection from ForkMesh Desktop to %1.\n\n"
        "It presented these host keys:\n\n%2\n\n"
        "Compare one with the server's own key before you continue: run "
        "ssh-keygen -lf /etc/ssh/ssh_host_ed25519_key.pub on the server, or "
        "check your hosting provider's console. Continue only if they match. "
        "ForkMesh then trusts this key and refuses a different one later.")
        .arg(host, fingerprints.join(QLatin1Char('\n')));
}

}
