#include "ClientErrorReports.h"

#include <QRegularExpression>

#include <algorithm>

namespace forkmesh {

namespace {

const QRegularExpression &relayStamp()
{
    static const QRegularExpression stamp(
        QStringLiteral("desktop (?:dialog|toast|notice) \\[(?:app|headless)\\]"),
        QRegularExpression::CaseInsensitiveOption);
    return stamp;
}

QString withoutControlCharacters(QString text)
{
    for (QChar &c : text) {
        if (c.unicode() < 0x20 || c.unicode() == 0x7f)
            c = QLatin1Char(' ');
    }
    return text;
}

}

QString ClientErrorReports::redact(const QString &text, int maxChars)
{
    QString out = withoutControlCharacters(text).simplified();
    if (out.isEmpty())
        return out;

    // Order matters: credentials and addresses first, then whole URLs, then the
    // long opaque ids (key material, node ids, hashes), then filesystem paths.
    // Each replacement is deliberately coarse — a report is triage material, not
    // a forensic record, and the shape of the failure survives all of it.
    static const QRegularExpression secret(
        QStringLiteral("\\b(?:authorization|bearer|password|passwd|secret|"
                       "token|api[_-]?key)\\b\\s*[:=]?\\s*[^\\s,;]+"),
        QRegularExpression::CaseInsensitiveOption);
    out.replace(secret, QStringLiteral("<secret>"));
    static const QRegularExpression email(
        QStringLiteral("\\b[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\\.[A-Za-z]{2,}\\b"));
    out.replace(email, QStringLiteral("<email>"));
    static const QRegularExpression url(
        QStringLiteral("\\b[A-Za-z][A-Za-z0-9+.-]*://\\S+"));
    out.replace(url, QStringLiteral("<url>"));
    static const QRegularExpression opaqueId(
        QStringLiteral("\\b[A-Za-z0-9_-]{32,}\\b"));
    out.replace(opaqueId, QStringLiteral("<id>"));
    static const QRegularExpression windowsPath(
        QStringLiteral("[A-Za-z]:\\\\[^\\s\"']+"));
    out.replace(windowsPath, QStringLiteral("<path>"));
    static const QRegularExpression unixPath(
        QStringLiteral("(?:/[A-Za-z0-9._~%+-]+){2,}/?"));
    out.replace(unixPath, QStringLiteral("<path>"));

    out = out.simplified();
    if (maxChars > 1 && out.size() > maxChars)
        out = out.left(maxChars - 1) + QChar(0x2026);
    return out;
}

QString ClientErrorReports::failureGroupKey(const QString &message)
{
    QString key = message.simplified();
    static const QRegularExpression flag(
        QStringLiteral("[\\x{1F1E6}-\\x{1F1FF}]{2}"));
    key.remove(flag);
    static const QRegularExpression ipv4(
        QStringLiteral("\\b\\d{1,3}(?:\\.\\d{1,3}){3}(?::\\d+)?\\b"));
    key.replace(ipv4, QStringLiteral("<ip>"));
    static const QRegularExpression ipv6(
        QStringLiteral("\\b[0-9A-Fa-f]{0,4}(?::[0-9A-Fa-f]{0,4}){2,7}\\b"));
    key.replace(ipv6, QStringLiteral("<ip>"));
    static const QRegularExpression network(
        QStringLiteral("<ip>\\s*\\([^)]*\\)"));
    key.replace(network, QStringLiteral("<ip>"));
    static const QRegularExpression objectId(
        QStringLiteral("\\b(?=[0-9a-f]*[a-f])(?=[0-9a-f]*\\d)[0-9a-f]{7,64}\\b"));
    key.replace(objectId, QStringLiteral("<oid>"));
    static const QRegularExpression count(QStringLiteral("(?<!-> )\\b\\d+\\b"));
    key.replace(count, QStringLiteral("#"));
    return key.simplified();
}

ErrorPingGroups::Group ErrorPingGroups::note(const QString &key, qint64 nowMs)
{
    auto it = m_groups.find(key);
    if (it != m_groups.end() && nowMs - it->lastMs < kQuietMs) {
        ++it->count;
        it->lastMs = nowMs;
        return *it;
    }
    if (it == m_groups.end() && m_groups.size() >= kMaxGroups) {
        auto oldest = m_groups.begin();
        for (auto scan = m_groups.begin(); scan != m_groups.end(); ++scan)
            if (scan->lastMs < oldest->lastMs)
                oldest = scan;
        m_groups.erase(oldest);
    }
    Group fresh;
    fresh.count = 1;
    fresh.firstMs = nowMs;
    fresh.lastMs = nowMs;
    m_groups.insert(key, fresh);
    return fresh;
}

void ErrorPingGroups::bind(const QString &key, qint64 pingId,
                           const QString &title)
{
    auto it = m_groups.find(key);
    if (it == m_groups.end())
        return;
    it->pingId = pingId;
    it->title = title;
}

void ErrorPingGroups::forget(const QString &key)
{
    m_groups.remove(key);
}

bool ClientErrorReports::echoesRelayAlert(const QString &kind,
                                          const QString &text)
{
    const QString alertKind = kind.trimmed().toLower();
    if (alertKind == QLatin1String("error_group")
        || alertKind == QLatin1String("operational_alert"))
        return true;
    // Second line of defence, independent of how the card was labelled: the
    // relay quotes a stored report verbatim, and every one of ours carries the
    // stamp the ingest route builds ("desktop notice [app] …", or "desktop toast
    // [app] …" from builds before notices were shown inline). The retired
	// headless spelling remains in this receive-only regex so an old alert
	// cannot loop when opened by a current desktop. Text wearing
    // that stamp is already a report of ours coming back around.
    return relayStamp().match(withoutControlCharacters(text).simplified())
        .hasMatch();
}

bool ClientErrorReports::describesLocalOperation(const QString &kind)
{
    return kind.trimmed().compare(kLocalOperationNoticeKind, Qt::CaseInsensitive)
           == 0;
}

bool ClientErrorReports::dialogAsksQuestion(const QList<DialogButton> &buttons)
{
    int choices = 0;
    for (const DialogButton button : buttons) {
        if (button == DialogButton::Decision)
            return true;
        if (button == DialogButton::Choice)
            ++choices;
    }
    return choices > 1;
}

bool ClientErrorReports::relayAccepts(const Report &report)
{
    if ((report.kind != QLatin1String("dialog") &&
         report.kind != QLatin1String("notice") &&
         report.kind != QLatin1String("toast")) ||
        (report.surface != QLatin1String("app") &&
         report.surface != QLatin1String("headless")))
        return false;
    const QString message = withoutControlCharacters(report.message).simplified();
    return !message.isEmpty() && !relayStamp().match(message).hasMatch();
}

bool ClientErrorReports::permanentRefusal(int httpStatus)
{
    return httpStatus >= 400 && httpStatus < 500 && httpStatus != 408 &&
           httpStatus != 425 && httpStatus != 429;
}

ClientErrorReports::Report ClientErrorReports::build(const QString &kind,
                                                     const QString &surface,
                                                     const QString &title,
                                                     const QString &message,
                                                     qint64 nowMs)
{
    Report report;
    report.kind = kind.trimmed().toLower();
    report.surface = surface.trimmed().toLower();
    if (report.surface.isEmpty())
        report.surface = QStringLiteral("app");
    report.title = redact(title, kMaxTitleChars);
    report.message = redact(message, kMaxMessageChars);
    report.tsMs = nowMs;
    return report;
}

QJsonObject ClientErrorReports::payload(const Report &report)
{
    return QJsonObject{{QStringLiteral("kind"), report.kind},
                       {QStringLiteral("surface"), report.surface},
                       {QStringLiteral("title"), report.title},
                       {QStringLiteral("message"), report.message}};
}

QString ClientErrorReports::dedupeKey(const Report &report)
{
    return report.kind + QLatin1Char('|') + report.surface + QLatin1Char('|')
           + report.title + QLatin1Char('|') + report.message;
}

void ClientErrorReports::prune(qint64 nowMs)
{
    auto expired = [nowMs](qint64 stamp) { return nowMs - stamp >= kWindowMs; };
    m_acceptedMs.erase(
        std::remove_if(m_acceptedMs.begin(), m_acceptedMs.end(), expired),
        m_acceptedMs.end());
    for (auto it = m_lastSentMs.begin(); it != m_lastSentMs.end();) {
        if (expired(it.value()))
            it = m_lastSentMs.erase(it);
        else
            ++it;
    }
    if (m_lastSentMs.size() > kMaxTrackedKeys) {
        QList<qint64> stamps = m_lastSentMs.values();
        std::sort(stamps.begin(), stamps.end());
        const qint64 cutoff = stamps.at(stamps.size() / 2);
        for (auto it = m_lastSentMs.begin(); it != m_lastSentMs.end();) {
            if (it.value() < cutoff)
                it = m_lastSentMs.erase(it);
            else
                ++it;
        }
    }
}

bool ClientErrorReports::accept(const Report &report)
{
    if (report.kind.isEmpty() || report.surface.isEmpty()
        || report.message.isEmpty())
        return false;
    prune(report.tsMs);
    const QString key = dedupeKey(report);
    const auto sent = m_lastSentMs.constFind(key);
    if (sent != m_lastSentMs.constEnd()
        && report.tsMs - sent.value() < kWindowMs)
        return false;
    if (m_acceptedMs.size() >= kMaxPerWindow)
        return false;
    m_lastSentMs.insert(key, report.tsMs);
    m_acceptedMs.append(report.tsMs);
    return true;
}

bool ClientErrorReports::defer(const Report &report)
{
    if (report.message.isEmpty() || report.attempts >= kMaxAttempts)
        return false;
    Report parked = report;
    ++parked.attempts;
    while (m_deferred.size() >= kMaxDeferred)
        m_deferred.removeFirst();
    m_deferred.append(parked);
    return true;
}

QList<ClientErrorReports::Report> ClientErrorReports::takeDeferred(
    qint64 nowMs, QList<Report> *expiredOut)
{
    QList<Report> ready;
    for (const Report &report : m_deferred) {
        if (nowMs - report.tsMs <= kMaxDeferralMs)
            ready.append(report);
        else if (expiredOut)
            expiredOut->append(report);
    }
    m_deferred.clear();
    return ready;
}

} // namespace forkmesh
