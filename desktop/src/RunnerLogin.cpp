#include "RunnerLogin.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QRegularExpression>
#include <QUrlQuery>

#include <algorithm>

namespace forkmesh::runners {

namespace {

const QRegularExpression &runnerNamePattern()
{
    static const QRegularExpression re(
        QStringLiteral("^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$"));
    return re;
}

const QRegularExpression &loginIdPattern()
{
    static const QRegularExpression re(QStringLiteral("^[0-9a-f]{32}$"));
    return re;
}

qint64 jsonMs(const QJsonObject &object, const QString &key)
{
    const QJsonValue value = object.value(key);
    return value.isDouble() ? static_cast<qint64>(value.toDouble()) : 0;
}

QUrl withPath(const QUrl &base, const QString &path)
{
    QUrl url = base;
    url.setPath(path);
    url.setQuery(QString());
    url.setFragment(QString());
    return url;
}

QString sessionPath(const QString &runner, const QString &id)
{
    return QStringLiteral("/api/runners/") + runner.trimmed().toLower() +
           QStringLiteral("/login/") + id.trimmed();
}

RunnerLoginInfo parseLogin(const QJsonObject &object)
{
    RunnerLoginInfo login;
    login.id = object.value(QStringLiteral("id")).toString().trimmed();
    login.provider =
        object.value(QStringLiteral("provider")).toString().trimmed();
    login.state = object.value(QStringLiteral("state")).toString().trimmed();
    login.createdBy =
        object.value(QStringLiteral("createdBy")).toString().trimmed();
    login.message = object.value(QStringLiteral("message")).toString().trimmed();
    login.createdAtMs = jsonMs(object, QStringLiteral("createdAt"));
    login.expiresAtMs = jsonMs(object, QStringLiteral("expiresAt"));
    login.endedAtMs = jsonMs(object, QStringLiteral("endedAt"));
    return login;
}

QString usageErrorText(const QString &error)
{
    if (error == QLatin1String("token_expired"))
        return QStringLiteral("usage refreshes after its next run");
    if (error == QLatin1String("unauthorized"))
        return QStringLiteral("the provider refused this login; sign it in "
                              "again");
    if (error == QLatin1String("no_oauth_login"))
        return QStringLiteral("usage limits need a subscription sign-in");
    if (error == QLatin1String("missing_login"))
        return QStringLiteral("saved login missing; sign it in again");
    if (error == QLatin1String("unreachable"))
        return QStringLiteral("usage service unreachable");
    return QStringLiteral("usage unavailable (%1)").arg(error);
}

QString durationText(qint64 ms)
{
    const qint64 minutes = qMax<qint64>(1, (ms + 30000) / 60000);
    if (minutes < 60)
        return QStringLiteral("%1 min").arg(minutes);
    const qint64 hours = minutes / 60;
    if (hours < 48)
        return QStringLiteral("%1 h %2 min").arg(hours).arg(minutes % 60);
    return QStringLiteral("%1 days").arg((hours + 12) / 24);
}

RunnerAccountInfo parseAccount(const QJsonObject &object)
{
    RunnerAccountInfo account;
    account.id = object.value(QStringLiteral("id")).toString().trimmed();
    account.label = object.value(QStringLiteral("label")).toString().trimmed();
    account.email = object.value(QStringLiteral("email")).toString().trimmed();
    account.plan = object.value(QStringLiteral("plan")).toString().trimmed();
    account.active = object.value(QStringLiteral("active")).toBool();
    const QJsonValue usageValue = object.value(QStringLiteral("usage"));
    if (usageValue.isObject()) {
        const QJsonObject usage = usageValue.toObject();
        account.usageRead = true;
        account.usageError =
            usage.value(QStringLiteral("error")).toString().trimmed();
        account.usageFetchedAtMs = jsonMs(usage, QStringLiteral("fetchedAt"));
        const QJsonArray windows =
            usage.value(QStringLiteral("windows")).toArray();
        for (const QJsonValue &value : windows) {
            const QJsonObject item = value.toObject();
            RunnerUsageWindow window;
            window.id = item.value(QStringLiteral("id")).toString().trimmed();
            window.label =
                item.value(QStringLiteral("label")).toString().trimmed();
            if (window.label.isEmpty())
                window.label = window.id;
            window.usedPercent = qBound(
                0.0, item.value(QStringLiteral("usedPercent")).toDouble(),
                100.0);
            window.resetsAtMs = jsonMs(item, QStringLiteral("resetsAt"));
            if (!window.id.isEmpty())
                account.usage.append(window);
        }
    }
    return account;
}

} // namespace

QString RunnerAccountInfo::displayName() const
{
    if (!label.isEmpty())
        return label;
    if (!email.isEmpty())
        return email;
    return QStringLiteral("Account");
}

bool RunnerProviderState::signedIn() const
{
    return reported && loginState == QLatin1String("available");
}

bool RunnerProviderState::loginExpired() const
{
    return reported && binaryFound &&
           (loginState == QLatin1String("expired") ||
            authState == QLatin1String("expired"));
}

bool RunnerProviderState::loginExpiring() const
{
    return signedIn() && !loginExpired() &&
           authState == QLatin1String("expiring");
}

const RunnerAccountInfo *RunnerProviderState::activeAccount() const
{
    for (const RunnerAccountInfo &account : accounts)
        if (account.active)
            return &account;
    return nullptr;
}

bool RunnerInfo::accountRequestPending(const QString &provider,
                                       const QString &account) const
{
    for (const RunnerAccountRequestInfo &request : accountRequests)
        if (request.provider == provider && request.account == account &&
            request.state == QLatin1String("pending"))
            return true;
    return false;
}

QString runnerUsageWindowText(const RunnerUsageWindow &window, qint64 nowMs)
{
    QString text = QStringLiteral("%1 %2%")
                       .arg(window.label)
                       .arg(qRound(window.usedPercent));
    if (window.resetsAtMs > nowMs)
        text += QStringLiteral(" (resets in %1)")
                    .arg(durationText(window.resetsAtMs - nowMs));
    return text;
}

QString runnerAccountUsageText(const RunnerAccountInfo &account, qint64 nowMs)
{
    if (!account.usageRead)
        return QStringLiteral("usage not read yet");
    QStringList parts;
    for (const RunnerUsageWindow &window : account.usage)
        parts.append(runnerUsageWindowText(window, nowMs));
    if (!account.usageError.isEmpty())
        parts.append(usageErrorText(account.usageError));
    else if (parts.isEmpty())
        parts.append(QStringLiteral("no usage limits reported"));
    return parts.join(QString::fromUtf8(" \xC2\xB7 "));
}

bool RunnerLoginInfo::open() const
{
    return runnerLoginStateOpen(state);
}

RunnerProviderState RunnerInfo::provider(const QString &provider) const
{
    for (const RunnerProviderState &state : providers)
        if (state.provider == provider)
            return state;
    RunnerProviderState missing;
    missing.provider = provider;
    return missing;
}

QString RunnerInfo::openLoginId(const QString &provider) const
{
    for (const RunnerLoginInfo &login : logins)
        if (login.provider == provider && login.open())
            return login.id;
    return {};
}

QStringList runnerProviders()
{
    return {QStringLiteral("claude-code"), QStringLiteral("codex")};
}

QString runnerProviderLabel(const QString &provider)
{
    if (provider == QLatin1String("claude-code"))
        return QStringLiteral("Claude");
    if (provider == QLatin1String("codex"))
        return QStringLiteral("Codex");
    return provider;
}

QString runnerProviderStateText(const RunnerProviderState &state)
{
    if (!state.reported)
        return QStringLiteral("Not reported");
    if (!state.binaryFound)
        return QStringLiteral("Not installed");
    if (state.loginExpired())
        return QString::fromUtf8("Login expired \xE2\x80\x94 reconnect");
    if (state.signedIn())
        return QStringLiteral("Signed in");
    return QStringLiteral("Needs sign-in");
}

QList<RunnerInfo> parseRunners(const QJsonObject &payload)
{
    QList<RunnerInfo> runners;
    const QJsonArray rows = payload.value(QStringLiteral("runners")).toArray();
    for (const QJsonValue &value : rows) {
        const QJsonObject row = value.toObject();
        RunnerInfo runner;
        runner.name = row.value(QStringLiteral("name")).toString().trimmed();
        if (!runnerNamePattern().match(runner.name).hasMatch())
            continue;
        runner.scope = row.value(QStringLiteral("scope")).toString().trimmed();
        runner.status =
            row.value(QStringLiteral("status")).toString().trimmed();
        runner.registeredBy =
            row.value(QStringLiteral("registeredBy")).toString().trimmed();
        runner.lastSeenAtMs = jsonMs(row, QStringLiteral("lastSeenAt"));
        runner.availabilityAtMs =
            jsonMs(row, QStringLiteral("availabilityAt"));
        const QJsonValue leases = row.value(QStringLiteral("activeLeases"));
        if (leases.isDouble() && leases.toDouble() >= 0)
            runner.activeLeases = leases.toInt();
        const QJsonObject availability =
            row.value(QStringLiteral("availability")).toObject();
        for (const QString &provider : runnerProviders()) {
            RunnerProviderState state;
            state.provider = provider;
            const QJsonValue entryValue = availability.value(provider);
            if (entryValue.isObject()) {
                const QJsonObject entry = entryValue.toObject();
                state.reported = true;
                state.binaryFound =
                    entry.value(QStringLiteral("binaryFound")).toBool();
                state.loginState =
                    entry.value(QStringLiteral("loginState")).toString();
                state.source = entry.value(QStringLiteral("source")).toString();
                const QJsonObject auth =
                    entry.value(QStringLiteral("auth")).toObject();
                state.authState =
                    auth.value(QStringLiteral("state")).toString().trimmed();
                state.authExpiresAtMs =
                    jsonMs(auth, QStringLiteral("expiresAt"));
                const QJsonArray accounts =
                    entry.value(QStringLiteral("accounts")).toArray();
                for (const QJsonValue &account : accounts) {
                    const RunnerAccountInfo info =
                        parseAccount(account.toObject());
                    if (loginIdPattern().match(info.id).hasMatch())
                        state.accounts.append(info);
                }
            }
            runner.providers.append(state);
        }
        const QJsonArray logins = row.value(QStringLiteral("logins")).toArray();
        for (const QJsonValue &login : logins) {
            const RunnerLoginInfo info = parseLogin(login.toObject());
            if (loginIdPattern().match(info.id).hasMatch())
                runner.logins.append(info);
        }
        const QJsonArray requests =
            row.value(QStringLiteral("accountRequests")).toArray();
        for (const QJsonValue &value : requests) {
            const QJsonObject item = value.toObject();
            RunnerAccountRequestInfo request;
            request.id = item.value(QStringLiteral("id")).toString().trimmed();
            request.provider =
                item.value(QStringLiteral("provider")).toString().trimmed();
            request.action =
                item.value(QStringLiteral("action")).toString().trimmed();
            request.account =
                item.value(QStringLiteral("account")).toString().trimmed();
            request.state =
                item.value(QStringLiteral("state")).toString().trimmed();
            request.message =
                item.value(QStringLiteral("message")).toString().trimmed();
            if (loginIdPattern().match(request.id).hasMatch())
                runner.accountRequests.append(request);
        }
        runners.append(runner);
    }
    return runners;
}

QString runnerForTarget(const QList<RunnerInfo> &runners,
                        const QString &targetNode, const QString &provider)
{
    const QString target = targetNode.trimmed().toLower();
    QList<const RunnerInfo *> active;
    for (const RunnerInfo &runner : runners) {
        if (runner.status != QLatin1String("active"))
            continue;
        if (!target.isEmpty() && runner.name == target)
            return runner.name;
        active.append(&runner);
    }
    if (active.size() == 1)
        return active.first()->name;
    QString candidate;
    for (const RunnerInfo *runner : active) {
        const RunnerProviderState state = runner->provider(provider);
        if (state.signedIn() || (state.reported && !state.binaryFound))
            continue;
        if (!candidate.isEmpty())
            return {};
        candidate = runner->name;
    }
    return candidate;
}

bool runnerLoginStateOpen(const QString &state)
{
    return state == QLatin1String("pending") ||
           state == QLatin1String("running");
}

QString runnerLoginStateText(const QString &state)
{
    if (state == QLatin1String("pending"))
        return QStringLiteral("Waiting for the runner to start the sign-in");
    if (state == QLatin1String("running"))
        return QStringLiteral("Sign-in running on the runner");
    if (state == QLatin1String("succeeded"))
        return QStringLiteral("Signed in");
    if (state == QLatin1String("failed"))
        return QStringLiteral("Sign-in failed");
    if (state == QLatin1String("cancelled"))
        return QStringLiteral("Sign-in cancelled");
    if (state == QLatin1String("expired"))
        return QStringLiteral("Sign-in expired");
    return state;
}

QUrl runnersListUrl(const QUrl &base)
{
    return withPath(base, QStringLiteral("/api/runners"));
}

QUrl runnerLoginStartUrl(const QUrl &base, const QString &runner)
{
    return withPath(base, QStringLiteral("/api/runners/") +
                              runner.trimmed().toLower() +
                              QStringLiteral("/login"));
}

QUrl runnerLoginOutputUrl(const QUrl &base, const QString &runner,
                          const QString &id, qint64 afterSeq)
{
    QUrl url = withPath(base, sessionPath(runner, id) +
                                  QStringLiteral("/output"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("after"),
                       QString::number(qMax<qint64>(0, afterSeq)));
    url.setQuery(query);
    return url;
}

QUrl runnerLoginInputUrl(const QUrl &base, const QString &runner,
                         const QString &id)
{
    return withPath(base, sessionPath(runner, id) + QStringLiteral("/input"));
}

QUrl runnerLoginCancelUrl(const QUrl &base, const QString &runner,
                          const QString &id)
{
    return withPath(base, sessionPath(runner, id) + QStringLiteral("/cancel"));
}

QUrl runnerAccountsUrl(const QUrl &base, const QString &runner)
{
    return withPath(base, QStringLiteral("/api/runners/") +
                              runner.trimmed().toLower() +
                              QStringLiteral("/accounts"));
}

QByteArray runnerAccountRequestBody(const QString &provider,
                                    const QString &account,
                                    const QString &action)
{
    return QJsonDocument(
               QJsonObject{{QStringLiteral("provider"), provider.trimmed()},
                           {QStringLiteral("account"), account.trimmed()},
                           {QStringLiteral("action"), action.trimmed()}})
        .toJson(QJsonDocument::Compact);
}

QByteArray runnerLoginStartBody(const QString &provider)
{
    return QJsonDocument(QJsonObject{
                             {QStringLiteral("provider"), provider.trimmed()}})
        .toJson(QJsonDocument::Compact);
}

bool runnerLoginInputValid(const QString &data)
{
    if (data.isEmpty() || data.toUtf8().size() > kRunnerLoginMaxInputBytes)
        return false;
    for (const QChar ch : data) {
        if (ch == QLatin1Char('\r') || ch == QLatin1Char('\n') ||
            ch == QLatin1Char('\t'))
            continue;
        if (ch.isSurrogate())
            continue;
        if (!ch.isPrint())
            return false;
    }
    return true;
}

QByteArray runnerLoginInputBody(const QString &data)
{
    return QJsonDocument(QJsonObject{{QStringLiteral("data"), data}})
        .toJson(QJsonDocument::Compact);
}

QString runnerLoginCodeSubmission(const QString &code)
{
    QString clean = code;
    clean.remove(QLatin1Char('\r'));
    clean.remove(QLatin1Char('\n'));
    clean = clean.trimmed();
    if (clean.isEmpty())
        return {};
    return clean + QLatin1Char('\r');
}

QString applyRunnerLoginKeystrokes(const QString &line, const QByteArray &bytes,
                                   QStringList *submitted)
{
    QString current = line;
    const QString text = QString::fromUtf8(bytes);
    for (const QChar ch : text) {
        if (ch == QLatin1Char('\r') || ch == QLatin1Char('\n')) {
            if (submitted)
                submitted->append(current + QLatin1Char('\r'));
            current.clear();
        } else if (ch == QChar(0x7f) || ch == QChar(0x08)) {
            current.chop(1);
        } else if (ch == QChar(0x15)) {
            current.clear();
        } else if (ch == QLatin1Char('\t') || ch.isPrint() || ch.isSurrogate()) {
            current.append(ch);
        }
    }
    return current;
}

RunnerLoginSession::RunnerLoginSession(const QString &runner,
                                       const QString &provider)
    : m_runner(runner.trimmed().toLower()),
      m_provider(provider.trimmed().toLower())
{
}

void RunnerLoginSession::applyPublic(const QJsonObject &body)
{
    const QString state = body.value(QStringLiteral("state")).toString();
    if (!state.isEmpty())
        m_state = state;
    const QString message = body.value(QStringLiteral("message")).toString();
    if (body.contains(QStringLiteral("state")))
        m_message = message;
}

RunnerLoginSession::Start
RunnerLoginSession::applyStartReply(int httpStatus, const QJsonObject &body)
{
    const QString id = body.value(QStringLiteral("id")).toString().trimmed();
    const QJsonObject session = body.value(QStringLiteral("session")).toObject();
    if ((httpStatus == 200 || httpStatus == 201) &&
        loginIdPattern().match(id).hasMatch()) {
        m_id = id;
        m_state = QStringLiteral("pending");
        applyPublic(session);
        m_error.clear();
        return Start::Started;
    }
    if (httpStatus == 409 &&
        body.value(QStringLiteral("error")).toString() ==
            QLatin1String("login_active") &&
        loginIdPattern().match(id).hasMatch()) {
        m_id = id;
        m_state = QStringLiteral("pending");
        applyPublic(session);
        m_error.clear();
        return Start::Attached;
    }
    QString message = body.value(QStringLiteral("message")).toString().trimmed();
    const QString code = body.value(QStringLiteral("error")).toString().trimmed();
    if (message.isEmpty()) {
        if (httpStatus == 401)
            message = QStringLiteral(
                "Sign in to your ForkMesh account on this desktop first.");
        else if (httpStatus == 403)
            message = QStringLiteral(
                "Only an administrator, or the registrant of a personal "
                "runner, may sign its agent CLIs in.");
        else if (httpStatus == 404)
            message = QStringLiteral("That runner is not registered.");
        else if (!code.isEmpty())
            message = code;
        else
            message = QStringLiteral("The sign-in could not be started.");
    }
    markFailed(message);
    return Start::Failed;
}

QByteArray RunnerLoginSession::applyOutputReply(const QJsonObject &body)
{
    applyPublic(body);
    const QJsonArray chunks = body.value(QStringLiteral("chunks")).toArray();
    QMap<qint64, QByteArray> ordered;
    for (const QJsonValue &value : chunks) {
        const QJsonObject chunk = value.toObject();
        const qint64 seq = static_cast<qint64>(
            chunk.value(QStringLiteral("seq")).toDouble());
        if (seq <= m_afterSeq)
            continue;
        const auto decoded = QByteArray::fromBase64Encoding(
            chunk.value(QStringLiteral("data")).toString().toLatin1(),
            QByteArray::AbortOnBase64DecodingErrors);
        if (!decoded)
            continue;
        ordered.insert(seq, *decoded);
    }
    QByteArray out;
    if (body.value(QStringLiteral("truncated")).toBool() && !ordered.isEmpty())
        out += QByteArrayLiteral(
            "\r\n[older sign-in output was trimmed]\r\n");
    for (auto it = ordered.cbegin(); it != ordered.cend(); ++it) {
        out += it.value();
        m_afterSeq = it.key();
    }
    const qint64 next =
        static_cast<qint64>(body.value(QStringLiteral("nextSeq")).toDouble());
    if (next > m_afterSeq)
        m_afterSeq = next;
    m_more = body.value(QStringLiteral("more")).toBool();
    return out;
}

void RunnerLoginSession::applyCancelReply(const QJsonObject &body)
{
    applyPublic(body);
    if (runnerLoginStateOpen(m_state))
        m_state = QStringLiteral("cancelled");
}

void RunnerLoginSession::attach(const QString &id)
{
    if (!loginIdPattern().match(id).hasMatch())
        return;
    m_id = id;
    m_state = QStringLiteral("pending");
    m_error.clear();
    m_failed = false;
}

void RunnerLoginSession::markFailed(const QString &message)
{
    m_failed = true;
    m_error = message;
}

bool RunnerLoginSession::open() const
{
    return !m_failed && !m_id.isEmpty() && runnerLoginStateOpen(m_state);
}

bool RunnerLoginSession::needsCancelOnClose() const
{
    return !m_id.isEmpty() && runnerLoginStateOpen(m_state);
}

namespace {

QString providerLongLabel(const QString &provider)
{
    return provider == QLatin1String("codex") ? QStringLiteral("Codex")
                                               : QStringLiteral("Claude Code");
}

QString attentionState(const QString &raw)
{
    const QString state = raw.trimmed().toLower();
    if (state.isEmpty() || state == QLatin1String("ok") ||
        state == QLatin1String("available") ||
        state == QLatin1String("signed_in"))
        return {};
    if (state == QLatin1String("expiring"))
        return state;
    return QStringLiteral("expired");
}

QString firstString(const QJsonObject &object, const QStringList &keys)
{
    for (const QString &key : keys) {
        const QString value = object.value(key).toString().trimmed();
        if (!value.isEmpty())
            return value;
    }
    return {};
}

void appendAttention(QList<RunnerAttentionItem> *items, const QString &runner,
                     const QJsonObject &entry, const QJsonArray &logins)
{
    RunnerAttentionItem item;
    item.runner = runner.trimmed().toLower();
    item.provider = entry.value(QStringLiteral("provider"))
                        .toString()
                        .trimmed()
                        .toLower();
    if (!runnerNamePattern().match(item.runner).hasMatch() ||
        !runnerProviders().contains(item.provider))
        return;
    const QJsonObject auth = entry.value(QStringLiteral("auth")).toObject();
    QString raw = firstString(
        entry, {QStringLiteral("state"), QStringLiteral("loginState")});
    if (raw.isEmpty())
        raw = auth.value(QStringLiteral("state")).toString();
    item.state = attentionState(raw);
    if (item.state.isEmpty())
        return;
    item.message = firstString(entry, {QStringLiteral("message")});
    if (item.message.isEmpty())
        item.message =
            auth.value(QStringLiteral("message")).toString().trimmed();
    item.expiresAtMs = jsonMs(entry, QStringLiteral("expiresAt"));
    if (item.expiresAtMs <= 0)
        item.expiresAtMs = jsonMs(auth, QStringLiteral("expiresAt"));
    QString loginId = firstString(entry, {QStringLiteral("loginId")});
    if (loginId.isEmpty())
        loginId = entry.value(QStringLiteral("login"))
                      .toObject()
                      .value(QStringLiteral("id"))
                      .toString()
                      .trimmed();
    if (loginId.isEmpty()) {
        for (const QJsonValue &value : logins) {
            const RunnerLoginInfo login = parseLogin(value.toObject());
            if (login.provider == item.provider && login.open()) {
                loginId = login.id;
                break;
            }
        }
    }
    if (loginIdPattern().match(loginId).hasMatch())
        item.openLoginId = loginId;
    item.canConnect = entry.value(QStringLiteral("canConnect")).toBool(true);
    items->append(item);
}

} // namespace

bool RunnerAttentionItem::expired() const
{
    return state == QLatin1String("expired");
}

QUrl runnerAttentionUrl(const QUrl &base)
{
    return withPath(base, QStringLiteral("/api/runners/attention"));
}

QUrl runnerConnectPageUrl(const QUrl &base, const RunnerAttentionItem &item)
{
    QUrl url = withPath(base, QStringLiteral("/dashboard/runners"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("runner"), item.runner);
    query.addQueryItem(QStringLiteral("provider"), item.provider);
    query.addQueryItem(QStringLiteral("connect"), QStringLiteral("1"));
    url.setQuery(query);
    return url;
}

qint64 runnerAttentionPollMs(const QJsonObject &payload)
{
    const qint64 poll = jsonMs(payload, QStringLiteral("pollMs"));
    if (poll <= 0)
        return 300000;
    return qBound(qint64(60000), poll, qint64(3600000));
}

QList<RunnerAttentionItem> runnerAttentionFromRunners(
    const QList<RunnerInfo> &runners)
{
    QList<RunnerAttentionItem> items;
    for (const RunnerInfo &runner : runners) {
        if (runner.status != QLatin1String("active"))
            continue;
        for (const RunnerProviderState &state : runner.providers) {
            if (!state.loginExpired() && !state.loginExpiring())
                continue;
            RunnerAttentionItem item;
            item.runner = runner.name;
            item.provider = state.provider;
            item.state = state.loginExpired() ? QStringLiteral("expired")
                                              : QStringLiteral("expiring");
            item.expiresAtMs = state.authExpiresAtMs;
            item.openLoginId = runner.openLoginId(state.provider);
            items.append(item);
        }
    }
    return sortedRunnerAttention(items);
}

QList<RunnerAttentionItem> parseRunnerAttention(const QJsonObject &payload)
{
    QList<RunnerAttentionItem> items;
    QJsonArray runnerRows;
    for (const QString &key :
         {QStringLiteral("attention"), QStringLiteral("items"),
          QStringLiteral("runners")}) {
        const QJsonArray rows = payload.value(key).toArray();
        for (const QJsonValue &value : rows) {
            const QJsonObject row = value.toObject();
            if (row.value(QStringLiteral("availability")).isObject()) {
                runnerRows.append(row);
                continue;
            }
            const QString runner = firstString(
                row, {QStringLiteral("runner"), QStringLiteral("name")});
            const QJsonArray logins =
                row.value(QStringLiteral("logins")).toArray();
            const QJsonValue providers = row.value(QStringLiteral("providers"));
            if (providers.isArray()) {
                for (const QJsonValue &entry : providers.toArray())
                    appendAttention(&items, runner, entry.toObject(), logins);
            } else {
                appendAttention(&items, runner, row, logins);
            }
        }
    }
    if (!runnerRows.isEmpty()) {
        QJsonObject rows;
        rows.insert(QStringLiteral("runners"), runnerRows);
        items.append(runnerAttentionFromRunners(parseRunners(rows)));
    }
    return sortedRunnerAttention(items);
}

QList<RunnerAttentionItem> sortedRunnerAttention(
    const QList<RunnerAttentionItem> &items)
{
    QList<RunnerAttentionItem> unique;
    for (const RunnerAttentionItem &item : items) {
        bool merged = false;
        for (RunnerAttentionItem &kept : unique) {
            if (kept.runner != item.runner || kept.provider != item.provider)
                continue;
            const QString openId =
                kept.openLoginId.isEmpty() ? item.openLoginId : kept.openLoginId;
            const bool canConnect = kept.canConnect || item.canConnect;
            if (item.expired() && !kept.expired())
                kept = item;
            kept.openLoginId = openId;
            kept.canConnect = canConnect;
            merged = true;
            break;
        }
        if (!merged)
            unique.append(item);
    }
    std::stable_sort(unique.begin(), unique.end(),
                     [](const RunnerAttentionItem &a,
                        const RunnerAttentionItem &b) {
                         if (a.expired() != b.expired())
                             return a.expired();
                         if (a.runner != b.runner)
                             return a.runner < b.runner;
                         return a.provider < b.provider;
                     });
    return unique;
}

QString runnerAttentionTitle(const RunnerAttentionItem &item)
{
    const QString provider = runnerProviderLabel(item.provider);
    return item.expired()
               ? QStringLiteral("%1 needs %2 sign-in")
                     .arg(item.runner, provider)
               : QStringLiteral("%1: %2 sign-in expires soon")
                     .arg(item.runner, provider);
}

QString runnerAttentionDetail(const RunnerAttentionItem &item)
{
    const QString provider = runnerProviderLabel(item.provider);
    if (item.expired())
        return QString::fromUtf8("%1: %2 login expired \xE2\x80\x94 reconnect. "
                                 "Jobs for %3 are not running on this runner.")
            .arg(item.runner, provider, providerLongLabel(item.provider));
    QString when = QStringLiteral("within a day");
    if (item.expiresAtMs > 0)
        when = QDateTime::fromMSecsSinceEpoch(item.expiresAtMs)
                   .toString(QStringLiteral("d MMM yyyy HH:mm"));
    return QStringLiteral("%1: %2 login expires soon (%3). Sign in again "
                          "before it expires.")
        .arg(item.runner, provider, when);
}

QString runnerAttentionActionText(const RunnerAttentionItem &item)
{
    if (!item.openLoginId.isEmpty())
        return QStringLiteral("Open sign-in");
    return QStringLiteral("Connect %1").arg(runnerProviderLabel(item.provider));
}

RunnerAttentionBadge runnerAttentionBadge(
    const QList<RunnerAttentionItem> &items)
{
    RunnerAttentionBadge badge;
    if (items.isEmpty())
        return badge;
    const QList<RunnerAttentionItem> sorted = sortedRunnerAttention(items);
    badge.visible = true;
    badge.expired = sorted.first().expired();
    badge.text = runnerAttentionTitle(sorted.first());
    if (sorted.size() > 1)
        badge.text += QStringLiteral(" (+%1 more)").arg(sorted.size() - 1);
    QStringList lines;
    for (const RunnerAttentionItem &item : sorted)
        lines.append(runnerAttentionDetail(item));
    lines.append(sorted.size() == 1
                     ? QStringLiteral("Click: %1 on %2.")
                           .arg(runnerAttentionActionText(sorted.first()),
                                sorted.first().runner)
                     : QStringLiteral("Click to pick the runner to sign in."));
    badge.toolTip = lines.join(QLatin1Char('\n'));
    return badge;
}

bool runnerSignInLink(const QUrl &url, QString *runner, QString *provider)
{
    if (url.path() != QLatin1String("/dashboard/runners"))
        return false;
    const QUrlQuery query(url);
    const QString wanted =
        query.queryItemValue(QStringLiteral("provider")).trimmed().toLower();
    if (!runnerProviders().contains(wanted))
        return false;
    const QString name =
        query.queryItemValue(QStringLiteral("runner")).trimmed().toLower();
    if (provider)
        *provider = wanted;
    if (runner)
        *runner = runnerNamePattern().match(name).hasMatch() ? name : QString();
    return true;
}

} // namespace forkmesh::runners
