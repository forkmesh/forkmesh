#include "LocalControlHosts.h"

#include <QRegularExpression>
#include <QUuid>

#include <algorithm>

namespace forkmesh::localcontrol::hosts {

QStringList mutatingActions()
{
    return {QStringLiteral("update"),  QStringLiteral("installBinary"),
            QStringLiteral("restart"), QStringLiteral("stop"),
            QStringLiteral("start"),   QStringLiteral("install"),
            QStringLiteral("destroy"), QStringLiteral("terminal")};
}

bool isMutatingAction(const QString &action)
{
    return mutatingActions().contains(action);
}

bool isDestructiveAction(const QString &action)
{
    return action == QLatin1String("destroy");
}

QString actionSummary(const QString &action, const QString &host)
{
    if (action == QLatin1String("update"))
        return QStringLiteral("Reinstall the latest ForkMesh on \"%1\" and restart "
                              "its service. Mirrors it serves are briefly "
                              "unavailable.")
            .arg(host);
    if (action == QLatin1String("installBinary"))
        return QStringLiteral("Upload this desktop's ForkMesh binary to \"%1\", "
                              "install it and restart its service, instead of "
                              "downloading the published release.")
            .arg(host);
    if (action == QLatin1String("terminal"))
        return QStringLiteral("Open an SSH shell on \"%1\" through this desktop. "
                              "What you type runs on the host with the saved "
                              "account's rights; the SSH key stays on the "
                              "desktop.")
            .arg(host);
    if (action == QLatin1String("restart"))
        return QStringLiteral("Restart the mirror service on \"%1\". A clone in "
                              "progress is left alone.")
            .arg(host);
    if (action == QLatin1String("stop"))
        return QStringLiteral("Stop the mirror service on \"%1\". It stops serving "
                              "until you start it again.")
            .arg(host);
    if (action == QLatin1String("start"))
        return QStringLiteral("Start the mirror service on \"%1\".").arg(host);
    if (action == QLatin1String("install"))
        return QStringLiteral("Create a new Vultr server%1 and install a ForkMesh "
                              "mirror on it. Vultr bills your account for it "
                              "every month until it is destroyed.")
            .arg(host.isEmpty() ? QString()
                                : QStringLiteral(" named \"%1\"").arg(host));
    if (action == QLatin1String("destroy"))
        return QStringLiteral("Permanently DELETE the Vultr server behind \"%1\". "
                              "Everything on it is gone, billing stops, it leaves "
                              "the Hosts list and the git remotes that pushed to it "
                              "are removed. This cannot be undone.")
            .arg(host);
    return {};
}

bool ownerMayManageHosts(const QString &deviceUser, const QString &desktopAccount)
{
    const QString device = deviceUser.trimmed();
    const QString account = desktopAccount.trimmed();
    return !device.isEmpty() && !account.isEmpty() &&
           device.compare(account, Qt::CaseInsensitive) == 0;
}

bool typedNameMatches(const QString &typed, const QString &host)
{
    const QString name = host.trimmed();
    return !name.isEmpty() && typed.trimmed() == name;
}

bool validHostName(const QString &name)
{
    static const QRegularExpression allowed(
        QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]{0,62}$"));
    return allowed.match(name).hasMatch();
}

QString ConfirmationStore::issue(const QString &deviceKey, const QString &action,
                                 const QString &host, qint64 nowMs)
{
    prune(nowMs);
    while (m_tokens.size() >= kMaxOutstanding) {
        // Drop the one closest to expiry; a phone never needs 32 at once.
        auto oldest = std::min_element(
            m_tokens.begin(), m_tokens.end(), [](const Entry &a, const Entry &b) {
                return a.expiresAtMs < b.expiresAtMs;
            });
        m_tokens.erase(oldest);
    }
    const QString token =
        QUuid::createUuid().toString(QUuid::Id128) +
        QUuid::createUuid().toString(QUuid::Id128).left(16);
    m_tokens.insert(token, Entry{deviceKey, action, host, nowMs + kLifetimeMs});
    return token;
}

ConfirmationStore::Result ConfirmationStore::consume(
    const QString &token, const QString &deviceKey, const QString &action,
    const QString &host, qint64 nowMs)
{
    auto it = m_tokens.find(token);
    if (token.isEmpty() || it == m_tokens.end()) {
        prune(nowMs);
        return Result::Missing;
    }
    const Entry entry = it.value();
    m_tokens.erase(it);
    prune(nowMs);
    if (entry.expiresAtMs <= nowMs)
        return Result::Expired;
    if (entry.deviceKey != deviceKey || entry.action != action ||
        entry.host.compare(host, Qt::CaseInsensitive) != 0)
        return Result::Mismatch;
    return Result::Ok;
}

void ConfirmationStore::prune(qint64 nowMs)
{
    for (auto it = m_tokens.begin(); it != m_tokens.end();) {
        if (it->expiresAtMs <= nowMs)
            it = m_tokens.erase(it);
        else
            ++it;
    }
}

QString confirmationError(ConfirmationStore::Result result)
{
    switch (result) {
    case ConfirmationStore::Result::Ok:
        return {};
    case ConfirmationStore::Result::Missing:
        return QStringLiteral("confirmation_required");
    case ConfirmationStore::Result::Expired:
        return QStringLiteral("confirmation_expired");
    case ConfirmationStore::Result::Mismatch:
        return QStringLiteral("confirmation_mismatch");
    }
    return QStringLiteral("confirmation_required");
}

QString healthFromProbeLabel(const QString &label)
{
    const QString text = label.trimmed();
    if (text.isEmpty())
        return QStringLiteral("unknown");
    if (text.contains(QLatin1String("Provisioning")))
        return QStringLiteral("provisioning");
    if (text.contains(QLatin1String("Offline")))
        return QStringLiteral("offline");
    if (text.contains(QLatin1String("Attention")) ||
        text.contains(QLatin1String("missing")))
        return QStringLiteral("attention");
    if (text.contains(QLatin1String("Online")))
        return QStringLiteral("online");
    if (text.contains(QLatin1String("Checking"), Qt::CaseInsensitive))
        return QStringLiteral("unknown");
    return QStringLiteral("attention");
}

QJsonObject hostRow(const QJsonObject &saved, const QString &probe,
                    const QString &version, bool rosterOnline)
{
    const QString name = saved.value(QStringLiteral("name")).toString().trimmed();
    const QString provider = saved.value(QStringLiteral("provider")).toString();
    const bool vultr = provider.compare(QLatin1String("Vultr"),
                                        Qt::CaseInsensitive) == 0;
    const QString status = saved.value(QStringLiteral("status")).toString();
    QString health = healthFromProbeLabel(probe);
    if (health == QLatin1String("unknown") && rosterOnline)
        health = QStringLiteral("online");
    if (status == QLatin1String("installing"))
        health = QStringLiteral("provisioning");
    QJsonArray actions;
    actions.append(QStringLiteral("update"));
    actions.append(QStringLiteral("installBinary"));
    // The service verbs run as root over the shared host key, which is how
    // the desktop manages its own mirror nodes; a plain SSH host added with a
    // password gets the installer only.
    const bool mirrorNode = vultr || saved.value(QStringLiteral("user"))
                                             .toString() == QLatin1String("root");
    if (mirrorNode) {
        actions.append(QStringLiteral("restart"));
        actions.append(QStringLiteral("stop"));
        actions.append(QStringLiteral("start"));
    }
#if !defined(Q_OS_WIN)
    actions.append(QStringLiteral("terminal"));
#endif
    if (vultr)
        actions.append(QStringLiteral("destroy"));
    return QJsonObject{
        {QStringLiteral("name"), name},
        {QStringLiteral("address"), saved.value(QStringLiteral("ip")).toString()},
        {QStringLiteral("user"), saved.value(QStringLiteral("user")).toString()},
        {QStringLiteral("status"), status},
        {QStringLiteral("statusChangedAt"),
         saved.value(QStringLiteral("statusChangedAt")).toDouble()},
        {QStringLiteral("provider"), provider},
        {QStringLiteral("region"), saved.value(QStringLiteral("region")).toString()},
        {QStringLiteral("plan"), saved.value(QStringLiteral("planType")).toString()},
        {QStringLiteral("monthlyCost"),
         saved.value(QStringLiteral("monthlyCost")).toDouble()},
        {QStringLiteral("ramMb"), saved.value(QStringLiteral("ramMb")).toDouble()},
        {QStringLiteral("diskGb"), saved.value(QStringLiteral("diskGb")).toDouble()},
        {QStringLiteral("dnsHostname"),
         saved.value(QStringLiteral("dnsHostname")).toString()},
        {QStringLiteral("health"), health},
        {QStringLiteral("healthLabel"), probe.trimmed()},
        {QStringLiteral("version"), version},
        {QStringLiteral("online"), rosterOnline},
        {QStringLiteral("actions"), actions},
    };
}

void LogStream::push(qint64 atMs, const QString &source, const QString &text)
{
    Line line;
    line.atMs = atMs;
    line.source = source.left(16);
    line.text = text.size() > kMaxLineChars ? text.left(kMaxLineChars) : text;
    m_buffer.append(line);
    if (m_buffer.size() > kMaxBufferedLines) {
        const qsizetype excess = m_buffer.size() - kMaxBufferedLines;
        m_buffer.remove(0, excess);
        m_dropped += excess;
        m_droppedTotal += excess;
    }
}

QJsonObject LogStream::takeBatch()
{
    if (!hasPending() || !canSend())
        return {};
    QJsonArray lines;
    const qsizetype count = std::min<qsizetype>(m_buffer.size(), kMaxBatchLines);
    for (qsizetype i = 0; i < count; ++i) {
        const Line &line = m_buffer.at(i);
        lines.append(QJsonObject{{QStringLiteral("t"), double(line.atMs)},
                                 {QStringLiteral("src"), line.source},
                                 {QStringLiteral("text"), line.text}});
    }
    m_buffer.remove(0, count);
    const QJsonObject batch{{QStringLiteral("seq"), double(m_nextSeq++)},
                            {QStringLiteral("lines"), lines},
                            {QStringLiteral("dropped"), double(m_dropped)}};
    m_dropped = 0;
    ++m_inFlight;
    return batch;
}

void LogStream::ack(qint64 seq)
{
    if (seq <= m_ackedSeq || seq >= m_nextSeq)
        return;
    m_inFlight = std::max<qint64>(0, m_inFlight - (seq - m_ackedSeq));
    m_ackedSeq = seq;
}

} // namespace forkmesh::localcontrol::hosts
