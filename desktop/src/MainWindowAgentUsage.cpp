
#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "AccountCapability.h"
#include "ForkMeshPackageRegistry.h"
#include "RepoStatsStore.h"
#include "AgentCliLocator.h"
#include "AgentJail.h"
#include "AgentCapabilityBroker.h"
#include "AgentCapabilityStore.h"
#include "AgentPermissionController.h"
#include "AgentPromptImages.h"
#include "AgentToolCatalog.h"
#include "AgentToolEfficiencyBoard.h"
#include "AgentGuardrailBoard.h"
#include "AgentToolPolicy.h"
#include "AgentRuntimeRegistry.h"
#include "AgentResumeIdentity.h"
#include "AgentWorktree.h"
#include "FlowLayout.h"
#include "KebabHeaderView.h"
#include "CodexAppServerSession.h"
#include "NodeEventSocket.h"
#include "UsageLimitCalendar.h"

#include <QActionGroup>
#include <QComboBox>
#include <QDebug>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTabBar>
#include <QTextLayout>
#include <QTextOption>
#include <QUrl>
#include <QEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QButtonGroup>
#include <QFrame>
#include <QShowEvent>
#include <QSignalBlocker>

#include <algorithm>
#include <limits>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#else
#include <csignal>
#endif
#include "MainWindowAgentsInternal.h"
#include "SettingsWrite.h"

using namespace forkmesh::ui;

namespace {

QString replyHeader(QNetworkReply *reply, const char *name)
{
    return QString::fromUtf8(reply->rawHeader(name)).trimmed();
}

double jsonNumber(const QJsonValue &value)
{
    if (value.isDouble())
        return value.toDouble();
    if (value.isString()) {
        bool ok = false;
        const double number = value.toString().toDouble(&ok);
        if (ok)
            return number;
    }
    return 0.0;
}

qint64 jsonCount(const QJsonObject &obj, const QString &key)
{
    return static_cast<qint64>(jsonNumber(obj.value(key)));
}

double costAmount(const QJsonObject &amount, QString *currency, bool *found)
{
    if (!amount.contains("value"))
        return 0.0;
    if (found)
        *found = true;
    if (currency && currency->isEmpty())
        *currency = amount.value("currency").toString();
    return jsonNumber(amount.value("value"));
}

double costResultTotal(const QJsonObject &result, QString *currency, bool *found)
{
    bool foundTopLevelAmount = false;
    const double topLevelTotal =
        costAmount(result.value("amount").toObject(), currency, &foundTopLevelAmount);
    if (foundTopLevelAmount) {
        if (found)
            *found = true;
        return topLevelTotal;
    }

    double total = 0.0;
    const QJsonArray lineItems = result.value("line_items").toArray();
    for (const QJsonValue &lineItemValue : lineItems) {
        const QJsonObject lineItem = lineItemValue.toObject();
        total += costAmount(lineItem.value("amount").toObject(), currency, found);
    }
    return total;
}

QString moneyString(double amount, QString currency)
{
    if (currency.isEmpty())
        currency = QStringLiteral("usd");
    const QString formatted = QString::number(amount, 'f', amount < 1.0 ? 4 : 2);
    if (currency.compare(QStringLiteral("usd"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("$%1 USD").arg(formatted);
    return QStringLiteral("%1 %2").arg(formatted, currency.toUpper());
}

QString usageReminderId(const QString &providerKey, const QString &windowKey)
{
    return providerKey + QLatin1Char('/') + windowKey;
}

QString codexWindowKind(const QJsonObject &window, const QString &fallback)
{
    const qint64 minutes = static_cast<qint64>(
        window.value(QStringLiteral("windowDurationMins")).toDouble());
    if (minutes > 0 && minutes <= 6 * 60)
        return QStringLiteral("5h");
    if (minutes >= 6 * 24 * 60)
        return QStringLiteral("weekly");
    return fallback;
}

qint64 codexWindowResetMs(const QJsonObject &window)
{
    qint64 reset = static_cast<qint64>(
        window.value(QStringLiteral("resetsAt")).toDouble());
    if (reset > 0 && reset < 10'000'000'000LL)
        reset *= 1000;
    return reset;
}

QString compactCodexLimitName(const QString &id, const QString &name)
{
    const QString source = name.trimmed().isEmpty() ? agentModelLabel(id)
                                                     : name.trimmed();
    if (source.contains(QStringLiteral("spark"), Qt::CaseInsensitive))
        return QStringLiteral("Spark");
    return source;
}

QJsonArray normalizedCodexUsageWindows(
    const QJsonObject &legacy, const QJsonObject &byLimitId)
{
    struct Bucket {
        QString id;
        QString name;
        QJsonObject snapshot;
        bool general = false;
    };
    QList<Bucket> buckets;
    QSet<QString> seen;
    auto appendBucket = [&](const QString &mapId, const QJsonObject &snapshot,
                            bool legacyBucket) {
        if (snapshot.isEmpty())
            return;
        QString id = snapshot.value(QStringLiteral("limitId")).toString();
        if (id.isEmpty())
            id = mapId;
        const QString identity = id.isEmpty() ? QStringLiteral("codex") : id;
        if (seen.contains(identity))
            return;
        seen.insert(identity);
        const QString name =
            snapshot.value(QStringLiteral("limitName")).toString();
        buckets.append({identity, name, snapshot,
                        legacyBucket || identity == QLatin1String("codex")});
    };

    QString legacyId = legacy.value(QStringLiteral("limitId")).toString();
    if (legacyId.isEmpty())
        legacyId = QStringLiteral("codex");
    const QJsonObject completeLegacy =
        byLimitId.value(legacyId).toObject().isEmpty()
            ? legacy
            : byLimitId.value(legacyId).toObject();
    appendBucket(legacyId, completeLegacy, true);
    for (auto it = byLimitId.constBegin(); it != byLimitId.constEnd(); ++it)
        appendBucket(it.key(), it.value().toObject(), false);

    // The account-wide balance leads, followed by deterministic model buckets.
    std::stable_sort(buckets.begin(), buckets.end(),
                     [](const Bucket &left, const Bucket &right) {
                         if (left.general != right.general)
                             return left.general;
                         return left.id < right.id;
                     });

    QJsonArray rows;
    for (const Bucket &bucket : std::as_const(buckets)) {
        const QString bucketName =
            bucket.general ? QString()
                           : compactCodexLimitName(bucket.id, bucket.name);
        const auto appendWindow = [&](const QString &field,
                                      const QString &fallbackKind) {
            const QJsonValue value = bucket.snapshot.value(field);
            if (!value.isObject())
                return;
            const QJsonObject window = value.toObject();
            if (!window.contains(QStringLiteral("usedPercent")))
                return;
            const QString kind = codexWindowKind(window, fallbackKind);
            const QString windowLabel = kind == QLatin1String("weekly")
                                            ? (bucket.general
                                                   ? QStringLiteral("Weekly")
                                                   : QStringLiteral("weekly"))
                                            : QStringLiteral("5-hour");
            const QString label = bucketName.isEmpty()
                                      ? windowLabel
                                      : QStringLiteral("%1 %2")
                                            .arg(bucketName, windowLabel);
            rows.append(QJsonObject{
                {QStringLiteral("id"),
                 QStringLiteral("%1/%2").arg(bucket.id, field)},
                {QStringLiteral("bucketId"), bucket.id},
                {QStringLiteral("label"), label},
                {QStringLiteral("kind"), kind},
                {QStringLiteral("general"), bucket.general},
                {QStringLiteral("usedPercent"),
                 qBound(0,
                        window.value(QStringLiteral("usedPercent")).toInt(),
                        100)},
                {QStringLiteral("resetAt"), codexWindowResetMs(window)},
                {QStringLiteral("windowDurationMins"),
                 static_cast<qint64>(
                     window.value(QStringLiteral("windowDurationMins"))
                         .toDouble())}});
        };
        appendWindow(QStringLiteral("primary"), QStringLiteral("5h"));
        appendWindow(QStringLiteral("secondary"), QStringLiteral("weekly"));
    }
    return rows;
}

constexpr qint64 kUsageLimitContinueSlackMs = 60 * 1000;

QString usageLimitProviderKey(const QString &provider)
{
    if (agentIsClaudeProvider(provider))
        return QStringLiteral("claude");
    if (agentIsCodexProvider(provider))
        return QStringLiteral("codex");
    return QString();
}

QString usageLimitHoldKey(int sessionId)
{
    return kUsageLimitAgentHoldGroup + QLatin1Char('/') +
           QString::number(sessionId);
}

qint64 exhaustedUsageResetMs(const QString &providerKey, const QString &model,
                             qint64 nowMs, bool *exhausted)
{
    if (exhausted)
        *exhausted = false;
    QStringList windows;
    if (providerKey == QLatin1String("claude")) {
        windows = {QStringLiteral("5h"), QStringLiteral("weekly")};
        if (model.contains(QLatin1String("fable"), Qt::CaseInsensitive))
            windows << QStringLiteral("fable");
    } else if (providerKey == QLatin1String("codex")) {
        windows = {QStringLiteral("5h"), QStringLiteral("weekly")};
    }
    QSettings settings;
    qint64 latest = 0;
    for (const QString &windowKey : std::as_const(windows)) {
        const QString exhaustedKey = usageExhaustedSetting(providerKey, windowKey);
        if (exhaustedKey.isEmpty() || !settings.value(exhaustedKey, false).toBool())
            continue;
        if (exhausted)
            *exhausted = true;
        latest = qMax(latest, settings.value(usageResetSetting(providerKey,
                                                               windowKey))
                                  .toLongLong());
    }
    return latest > nowMs ? latest : 0;
}

} // namespace

void MainWindow::testOpenAiAgentKey()
{
    QSettings settings;
    const QString apiKey = settings.value(kCodexApiKeySetting).toString().trimmed();
    const QString adminKey =
        settings.value(kOpenAiAdminKeySetting).toString().trimmed();
    const QString usageKey = adminKey.isEmpty() ? apiKey : adminKey;
    if (apiKey.isEmpty()) {
        if (m_agentApiKeyStatus)
            m_agentApiKeyStatus->setText("No OpenAI API key saved in Settings.");
        return;
    }
    if (!m_networkAccess) {
        if (m_agentApiKeyStatus)
            m_agentApiKeyStatus->setText("Network client is not ready.");
        return;
    }

    if (m_agentTestApiKeyButton)
        m_agentTestApiKeyButton->setEnabled(false);
    if (m_agentApiKeyStatus)
        m_agentApiKeyStatus->setText("Testing OpenAI key...");

    struct KeyTestState {
        bool modelsOk = false;
        bool usageOk = false;
        bool costsOk = false;
        int modelCount = 0;
        qint64 requests = 0;
        qint64 inputTokens = 0;
        qint64 cachedTokens = 0;
        qint64 outputTokens = 0;
        double costs = 0.0;
        QString currency;
        QString requestId;
        QString organization;
        QString modelsError;
        QString usageError;
        QString costsError;
    };
    auto state = std::make_shared<KeyTestState>();
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const qint64 end = now.toSecsSinceEpoch();
    const qint64 usageStart = end - 24 * 60 * 60;
    const qint64 costsStart =
        QDate(now.date().year(), now.date().month(), 1)
            .startOfDay(QTimeZone::utc())
            .toSecsSinceEpoch();
    const qint64 costsEnd =
        now.date()
            .addDays(1)
            .startOfDay(QTimeZone::utc())
            .toSecsSinceEpoch();

    auto finish = [this, state] {
        if (m_agentTestApiKeyButton)
            m_agentTestApiKeyButton->setEnabled(true);
        if (!m_agentApiKeyStatus)
            return;

        if (!state->modelsOk) {
            if (m_agentOpenAiSpend)
                m_agentOpenAiSpend->setText("OpenAI spend: unavailable");
            m_agentApiKeyStatus->setText(
                QStringLiteral("OpenAI key rejected. %1").arg(state->modelsError));
            return;
        }

        if (m_agentOpenAiSpend) {
            if (state->costsOk) {
                m_openAiSpendUsd = state->costs; // already in dollars
                const QString text =
                    QStringLiteral("OpenAI spend, month to date: %1")
                        .arg(moneyString(state->costs, state->currency));
                m_agentOpenAiSpend->setText(text);
                cacheSpendLabel(kOpenAiSpendTextSetting, kOpenAiSpendTsSetting,
                                text);
            } else {
                m_agentOpenAiSpend->setText("OpenAI spend: unavailable");
            }
        }
        updateAgentTotalSpend();

        const qint64 totalTokens = state->inputTokens + state->outputTokens;
        QStringList lines;
        if (state->usageOk) {
            lines << QStringLiteral(
                         "Last 24h OpenAI usage: %1 requests, %2 total tokens (%3 input, %4 cached input, %5 output).")
                         .arg(formatCount(state->requests))
                         .arg(formatCount(totalTokens))
                         .arg(formatCount(state->inputTokens))
                         .arg(formatCount(state->cachedTokens))
                         .arg(formatCount(state->outputTokens));
        }
        lines << QStringLiteral("OpenAI key works. %1 models visible.")
                     .arg(formatCount(state->modelCount));
        if (!state->organization.isEmpty())
            lines << QStringLiteral("Organization: %1").arg(state->organization);
        if (!state->requestId.isEmpty())
            lines << QStringLiteral("Request ID: %1").arg(state->requestId);
        if (!state->usageOk) {
            lines << QStringLiteral("Usage stats unavailable: %1")
                         .arg(state->usageError);
        }
        if (!state->costsOk) {
            lines << QStringLiteral("Cost stats unavailable: %1")
                         .arg(state->costsError);
        }
        if (!state->usageOk || !state->costsOk)
            lines << QStringLiteral(
                "Organization usage/cost endpoints may require an Admin API key.");
        m_agentApiKeyStatus->setText(lines.join(QStringLiteral("<br>")));
    };

    auto requestCosts = [this, usageKey, costsStart, costsEnd, state, finish] {
        QUrl url(QStringLiteral("https://api.openai.com/v1/organization/costs"));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("start_time"),
                           QString::number(costsStart));
        query.addQueryItem(QStringLiteral("end_time"),
                           QString::number(costsEnd));
        query.addQueryItem(QStringLiteral("bucket_width"), QStringLiteral("1d"));
        query.addQueryItem(QStringLiteral("limit"), QStringLiteral("31"));
        url.setQuery(query);
        QNetworkReply *reply = m_networkAccess->get(openAiRequest(url, usageKey));
        connect(reply, &QNetworkReply::finished, this, [reply, state, finish] {
            const QByteArray body = reply->readAll();
            if (reply->error() == QNetworkReply::NoError) {
                state->costsOk = true;
                const QJsonArray buckets =
                    QJsonDocument::fromJson(body).object().value("data").toArray();
                for (const QJsonValue &bucketValue : buckets) {
                    const QJsonArray results =
                        bucketValue.toObject().value("results").toArray();
                    for (const QJsonValue &resultValue : results) {
                        state->costs += costResultTotal(resultValue.toObject(),
                                                        &state->currency,
                                                        nullptr);
                    }
                }
            } else {
                state->costsError = apiErrorSummary(reply, body);
            }
            reply->deleteLater();
            finish();
        });
    };

    auto requestUsage = [this, usageKey, usageStart, end, state, requestCosts] {
        QUrl url(QStringLiteral(
            "https://api.openai.com/v1/organization/usage/completions"));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("start_time"),
                           QString::number(usageStart));
        query.addQueryItem(QStringLiteral("end_time"), QString::number(end));
        query.addQueryItem(QStringLiteral("bucket_width"), QStringLiteral("1d"));
        url.setQuery(query);
        QNetworkReply *reply = m_networkAccess->get(openAiRequest(url, usageKey));
        connect(reply, &QNetworkReply::finished, this,
                [reply, state, requestCosts] {
                    const QByteArray body = reply->readAll();
                    if (reply->error() == QNetworkReply::NoError) {
                        const QJsonArray buckets =
                            QJsonDocument::fromJson(body)
                                .object()
                                .value("data")
                                .toArray();
                        for (const QJsonValue &bucketValue : buckets) {
                            const QJsonArray results =
                                bucketValue.toObject().value("results").toArray();
                            for (const QJsonValue &resultValue : results) {
                                const QJsonObject result = resultValue.toObject();
                                state->requests +=
                                    jsonCount(result, "num_model_requests");
                                state->inputTokens +=
                                    jsonCount(result, "input_tokens");
                                state->cachedTokens +=
                                    jsonCount(result, "input_cached_tokens");
                                state->outputTokens +=
                                    jsonCount(result, "output_tokens");
                            }
                        }
                        state->usageOk = true;
                    } else {
                        state->usageError = apiErrorSummary(reply, body);
                    }
                    reply->deleteLater();
                    requestCosts();
                });
    };

    QNetworkReply *creditReply = m_networkAccess->get(
        openAiRequest(QUrl(QStringLiteral(
            "https://api.openai.com/v1/dashboard/billing/credit_grants")),
            apiKey));
    connect(creditReply, &QNetworkReply::finished, this,
            [this, creditReply] {
        const QByteArray body = creditReply->readAll();
        creditReply->deleteLater();
        if (!m_agentOpenAiCredit)
            return;
        if (creditReply->error() != QNetworkReply::NoError) {
            m_agentOpenAiCredit->setText(
                QStringLiteral("OpenAI remaining credits: unavailable (%1)")
                    .arg(apiErrorSummary(creditReply, body)));
            return;
        }
        const QJsonObject obj = QJsonDocument::fromJson(body).object();
        const double granted = obj.value(QStringLiteral("total_granted")).toDouble();
        const double used    = obj.value(QStringLiteral("total_used")).toDouble();
        const double available = obj.value(QStringLiteral("total_available")).toDouble(
            granted - used);
        const QString text =
            QStringLiteral("OpenAI remaining credits: $%1 USD (of $%2 granted)")
                .arg(QString::number(available, 'f', 2),
                     QString::number(granted, 'f', 2));
        m_agentOpenAiCredit->setText(text);
        cacheSpendLabel(kOpenAiCreditTextSetting, kOpenAiCreditTsSetting, text);
    });

    QNetworkReply *reply =
        m_networkAccess->get(openAiRequest(
            QUrl(QStringLiteral("https://api.openai.com/v1/models")), apiKey));
    connect(reply, &QNetworkReply::finished, this,
            [reply, state, requestUsage, finish] {
                const QByteArray body = reply->readAll();
                if (reply->error() == QNetworkReply::NoError) {
                    state->modelsOk = true;
                    state->requestId = replyHeader(reply, "x-request-id");
                    state->organization =
                        replyHeader(reply, "openai-organization");
                    state->modelCount = QJsonDocument::fromJson(body)
                                            .object()
                                            .value("data")
                                            .toArray()
                                            .size();
                } else {
                    state->modelsError = apiErrorSummary(reply, body);
                }
                reply->deleteLater();
                if (state->modelsOk)
                    requestUsage();
                else
                    finish();
            });
}

void MainWindow::cacheSpendLabel(const QString &textKey, const QString &tsKey,
                                 const QString &text)
{
    QSettings settings;
    settings.setValue(textKey, text);
    settings.setValue(tsKey, QDateTime::currentMSecsSinceEpoch());
}

void MainWindow::applyCachedSpendLabels()
{
    QSettings settings;
    auto restore = [&settings](QLabel *label, const QString &textKey,
                               const QString &tsKey) {
        if (!label)
            return;
        const QString text = settings.value(textKey).toString();
        if (text.isEmpty())
            return;
        const qint64 ts = settings.value(tsKey).toLongLong();
        QString suffix;
        if (ts > 0)
            suffix = QStringLiteral(" (cached %1)")
                         .arg(QDateTime::fromMSecsSinceEpoch(ts).toString(
                             QStringLiteral("MMM d hh:mm")));
        label->setText(text + suffix);
    };
    restore(m_agentOpenAiSpend, kOpenAiSpendTextSetting, kOpenAiSpendTsSetting);
    restore(m_agentClaudeSpend, kClaudeSpendTextSetting, kClaudeSpendTsSetting);
    restore(m_agentOpenAiCredit, kOpenAiCreditTextSetting, kOpenAiCreditTsSetting);
    restore(m_agentClaudeCredit, kClaudeCreditTextSetting, kClaudeCreditTsSetting);
}

void MainWindow::clearUsageLimitReminders()
{
    for (QTimer *timer : std::as_const(m_usageLimitReminderTimers)) {
        timer->stop();
        timer->deleteLater();
    }
    m_usageLimitReminderTimers.clear();
}

void MainWindow::restoreUsageLimitReminders()
{
    clearUsageLimitReminders();
    QSettings settings;

    struct Reminder {
        const char *providerKey;
        const char *windowKey;
        const char *providerName;
        const char *windowName;
    };
    static const Reminder reminders[] = {
        {"claude", "5h", "Claude Code", "5-hour"},
        {"claude", "weekly", "Claude Code", "weekly"},
        {"claude", "fable", "Claude Code", "Fable weekly"},
        {"codex", "5h", "Codex", "5-hour"},
        {"codex", "weekly", "Codex", "weekly"},
    };
    for (const Reminder &reminder : reminders) {
        const QString providerKey = QString::fromLatin1(reminder.providerKey);
        const QString windowKey = QString::fromLatin1(reminder.windowKey);
        const QString exhaustedKey = usageExhaustedSetting(providerKey, windowKey);
        const QString resetKey = usageResetSetting(providerKey, windowKey);
        if (exhaustedKey.isEmpty() || resetKey.isEmpty() ||
            !settings.value(exhaustedKey, false).toBool()) {
            continue;
        }
        scheduleUsageLimitReminder(providerKey, windowKey,
                                   QString::fromLatin1(reminder.providerName),
                                   QString::fromLatin1(reminder.windowName),
                                   settings.value(resetKey).toLongLong());
    }
}

void MainWindow::scheduleUsageLimitReminder(const QString &providerKey,
                                            const QString &windowKey,
                                            const QString &providerName,
                                            const QString &windowName,
                                            qint64 resetMs)
{
    if (resetMs <= 0)
        return;
    const QString exhaustedKey = usageExhaustedSetting(providerKey, windowKey);
    if (exhaustedKey.isEmpty())
        return;
    const QString id = usageReminderId(providerKey, windowKey);
    QSettings settings;
    // Hitting a limit arms the countdown by itself: nobody should have to opt
    // in to being told the work they were waiting on can start again.
    if (!settings.value(exhaustedKey, false).toBool())
        return;

    // The calendar is what alerts someone while ForkMesh is closed. Once they
    // reopen it after that time, finish the deferred transition here rather
    // than leaving an already-ready window marked as exhausted forever.
    if (resetMs <= QDateTime::currentMSecsSinceEpoch()) {
        settings.setValue(exhaustedKey, false);
        notifyUsageLimitReady(providerKey, windowKey, providerName, windowName,
                              resetMs);
        return;
    }

    // Writing the calendar file hands the machine's calendar app an alarm that
    // survives ForkMesh being closed, at the cost of opening that app unasked,
    // so it stays the opt-in half. The timer below runs either way.
    const QString calendarKey = kUsageLimitCalendarWrittenPrefix + id;
    if (settings.value(kUsageLimitCalendarReminderSetting, false).toBool() &&
        settings.value(calendarKey).toLongLong() != resetMs) {
        settings.setValue(calendarKey, resetMs);
        const QString path = UsageLimitCalendar::writeEvent(
            providerKey, windowKey, providerName, windowName, resetMs);
        if (!path.isEmpty())
            QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    }

    const QString scheduledKey = kUsageLimitReminderScheduledPrefix + id;
    forkmesh::settings::put(settings, scheduledKey, resetMs);

    if (QTimer *old = m_usageLimitReminderTimers.take(id)) {
        old->stop();
        old->deleteLater();
    }
    auto *timer = new QTimer(this);
    timer->setSingleShot(true);
    m_usageLimitReminderTimers.insert(id, timer);
    connect(timer, &QTimer::timeout, this,
            [this, timer, id, providerKey, windowKey, providerName, windowName,
             resetMs] {
                if (m_usageLimitReminderTimers.value(id) != timer)
                    return;
                m_usageLimitReminderTimers.remove(id);
                timer->deleteLater();

                const qint64 now = QDateTime::currentMSecsSinceEpoch();
                if (now < resetMs) {
                    scheduleUsageLimitReminder(providerKey, windowKey,
                                               providerName, windowName, resetMs);
                    return;
                }
                QSettings settings;
                const QString exhaustedKey =
                    usageExhaustedSetting(providerKey, windowKey);
                const QString scheduledKey =
                    kUsageLimitReminderScheduledPrefix + id;
                if (settings.value(scheduledKey).toLongLong() != resetMs ||
                    exhaustedKey.isEmpty() ||
                    !settings.value(exhaustedKey, false).toBool()) {
                    return;
                }
                settings.setValue(exhaustedKey, false);
                notifyUsageLimitReady(providerKey, windowKey, providerName,
                                      windowName, resetMs);
            });
    const qint64 delayMs = qMin(
        resetMs - QDateTime::currentMSecsSinceEpoch(),
        qint64(std::numeric_limits<int>::max()));
    timer->start(static_cast<int>(qMax<qint64>(1, delayMs)));
}

void MainWindow::notifyUsageLimitReady(const QString &providerKey,
                                       const QString &windowKey,
                                       const QString &providerName,
                                       const QString &windowName,
                                       qint64 resetMs)
{
    continueUsageLimitHeldAgentsFor(providerKey);
    QSettings settings;
    const QString id = usageReminderId(providerKey, windowKey);
    const QString resetKey = usageResetSetting(providerKey, windowKey);
    if (resetMs <= 0 && !resetKey.isEmpty())
        resetMs = settings.value(resetKey).toLongLong();
    const QString notifiedKey = kUsageLimitReminderNotifiedPrefix + id;
    if (resetMs > 0 && settings.value(notifiedKey).toLongLong() == resetMs)
        return;
    if (resetMs > 0)
        settings.setValue(notifiedKey, resetMs);

    const QString title =
        QStringLiteral("ForkMesh — %1 usage is ready").arg(providerName);
    const QString body = QStringLiteral(
                             "Your %1 %2 usage window has reset. You can resume "
                             "agent work.")
                             .arg(providerName, windowName);
    AppNotification ping;
    ping.title = title;
    ping.body = body;
    ping.kind = QStringLiteral("usage");
    // This is the one notification that tells someone their stopped work can
    // continue. It must remain visible until they consciously acknowledge it.
    ping.dismissOnly = true;
    recordNotification(ping);
    postNotification(title, body, false, QStringLiteral("appointment-soon"),
                     QString(), /*dismissOnly=*/true);
}

void MainWindow::holdAgentForUsageLimit(int sessionId)
{
    if (!QSettings().value(kUsageLimitAutoContinueSetting, true).toBool() ||
        isCloudSession(sessionId) || isExternalSession(sessionId))
        return;
    const AgentSession *session = findAgentSession(sessionId);
    if (!session || session->associationOnly ||
        session->status != AgentStatus::Failed ||
        !forkmesh::teammates::usageLimitReached(session->lastError))
        return;
    const QString providerKey = usageLimitProviderKey(session->provider);
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 failedAtMs =
        session->finishedAtMs > 0 ? session->finishedAtMs : nowMs;
    qint64 resumeAtMs = forkmesh::teammates::usageLimitResumeAtMs(
        session->lastError, failedAtMs);
    if (resumeAtMs <= 0)
        resumeAtMs =
            exhaustedUsageResetMs(providerKey, session->model, nowMs, nullptr);
    if (resumeAtMs <= 0 && providerKey.isEmpty())
        return;
    QSettings settings;
    const QString key = usageLimitHoldKey(sessionId);
    if (settings.contains(key) &&
        settings.value(key).toLongLong() == resumeAtMs)
        return;

    forkmesh::settings::put(settings, key, resumeAtMs);
    const QString when =
        resumeAtMs > 0
            ? QLocale().toString(QDateTime::fromMSecsSinceEpoch(resumeAtMs),
                                 QLocale::ShortFormat)
            : QStringLiteral("its usage window resets");
    if (m_agentStore)
        m_agentStore->appendLog(
            *session,
            QStringLiteral("\n==> Usage limit reached. ForkMesh continues this "
                           "session automatically at %1.")
                .arg(when));
    logSystem(QStringLiteral("Agent session #%1 hit a usage limit; it continues "
                             "automatically at %2.")
                  .arg(sessionId)
                  .arg(when));
    armUsageLimitAgentHold(sessionId, resumeAtMs);
    if (resumeAtMs <= 0 && providerKey == QLatin1String("claude"))
        refreshClaudeCodeUsage();
}

void MainWindow::armUsageLimitAgentHold(int sessionId, qint64 resumeAtMs)
{
    if (QTimer *old = m_usageLimitAgentHoldTimers.take(sessionId)) {
        old->stop();
        old->deleteLater();
    }
    if (resumeAtMs <= 0)
        return;
    auto *timer = new QTimer(this);
    timer->setSingleShot(true);
    m_usageLimitAgentHoldTimers.insert(sessionId, timer);
    connect(timer, &QTimer::timeout, this, [this, timer, sessionId, resumeAtMs] {
        if (m_usageLimitAgentHoldTimers.value(sessionId) != timer)
            return;
        m_usageLimitAgentHoldTimers.remove(sessionId);
        timer->deleteLater();
        if (QDateTime::currentMSecsSinceEpoch() <
            resumeAtMs + kUsageLimitContinueSlackMs) {
            armUsageLimitAgentHold(sessionId, resumeAtMs);
            return;
        }
        continueUsageLimitHeldAgents({sessionId});
    });
    const qint64 delayMs =
        qMin(resumeAtMs + kUsageLimitContinueSlackMs -
                 QDateTime::currentMSecsSinceEpoch(),
             qint64(std::numeric_limits<int>::max()));
    timer->start(static_cast<int>(qMax<qint64>(1, delayMs)));
}

void MainWindow::restoreUsageLimitAgentHolds()
{
    QSettings settings;
    settings.beginGroup(kUsageLimitAgentHoldGroup);
    const QStringList ids = settings.childKeys();
    settings.endGroup();
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    QList<int> due;
    for (const QString &idText : ids) {
        const int sessionId = idText.toInt();
        const QString key = usageLimitHoldKey(sessionId);
        const AgentSession *session =
            sessionId > 0 ? findAgentSession(sessionId) : nullptr;
        if (!session) {
            settings.remove(kUsageLimitAgentHoldGroup + QLatin1Char('/') +
                            idText);
            continue;
        }
        const qint64 resumeAtMs = settings.value(key).toLongLong();
        bool exhausted = false;
        exhaustedUsageResetMs(usageLimitProviderKey(session->provider),
                              session->model, nowMs, &exhausted);
        if (resumeAtMs > 0 ? resumeAtMs + kUsageLimitContinueSlackMs <= nowMs
                           : !exhausted)
            due << sessionId;
        else
            armUsageLimitAgentHold(sessionId, resumeAtMs);
    }
    continueUsageLimitHeldAgents(due);
}

void MainWindow::continueUsageLimitHeldAgentsFor(const QString &providerKey)
{
    QSettings settings;
    settings.beginGroup(kUsageLimitAgentHoldGroup);
    const QStringList ids = settings.childKeys();
    settings.endGroup();
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    QList<int> due;
    for (const QString &idText : ids) {
        const int sessionId = idText.toInt();
        const AgentSession *session =
            sessionId > 0 ? findAgentSession(sessionId) : nullptr;
        if (!session || usageLimitProviderKey(session->provider) != providerKey)
            continue;
        const qint64 resumeAtMs =
            settings.value(usageLimitHoldKey(sessionId)).toLongLong();
        bool exhausted = false;
        exhaustedUsageResetMs(providerKey, session->model, nowMs, &exhausted);
        if (!exhausted || resumeAtMs <= nowMs + kUsageLimitContinueSlackMs)
            due << sessionId;
    }
    continueUsageLimitHeldAgents(due);
}

void MainWindow::continueUsageLimitHeldAgents(const QList<int> &sessionIds)
{
    QSettings settings;
    const bool enabled =
        settings.value(kUsageLimitAutoContinueSetting, true).toBool();
    bool continued = false;
    for (int sessionId : sessionIds) {
        const QString key = usageLimitHoldKey(sessionId);
        if (!settings.contains(key))
            continue;
        settings.remove(key);
        if (QTimer *timer = m_usageLimitAgentHoldTimers.take(sessionId)) {
            timer->stop();
            timer->deleteLater();
        }
        const AgentSession *session = findAgentSession(sessionId);
        if (!enabled || !session || session->associationOnly ||
            session->status != AgentStatus::Failed ||
            !forkmesh::teammates::usageLimitReached(session->lastError))
            continue;
        logSystem(QStringLiteral("Usage limit reset: continuing agent session "
                                 "#%1.")
                      .arg(sessionId));
        discardWedgedAgentTransport(sessionId);
        if (agentSessionHasLiveTransport(sessionId))
            sendPromptToAgentSession(sessionId, continueWhereLeftOffPrompt());
        else
            continueAgentSession(sessionId, true);
        continued = true;
    }
    if (!continued)
        return;
    reloadAgents();
    processAgentQueue();
}

void MainWindow::markAgentLimitWindow(const QString &provider)
{
    const bool claude = agentIsClaudeProvider(provider);
    const QString k5h =
        claude ? kClaudeLimit5hStartSetting : kCodexLimit5hStartSetting;
    const QString kWeek =
        claude ? kClaudeLimitWeekStartSetting : kCodexLimitWeekStartSetting;
    QSettings settings;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QString accountId = activeAgentAccount(provider).id;
    auto refreshAnchor = [&](const QString &key, qint64 windowMs) {
        const qint64 start = settings.value(key).toLongLong();
        if (start <= 0 || now - start >= windowMs)
            settings.setValue(key, now);
        settings.setValue(
            agentAccountUsageSetting(provider, accountId,
                                     agentAccountUsageField(key)),
            settings.value(key));
    };
    refreshAnchor(k5h, kAgentLimit5hMs);
    refreshAnchor(kWeek, kAgentLimitWeekMs);
    refreshAgentLimitLabel();
    if (agentIsCodexProvider(provider))
        refreshCodexUsageRemaining();
}

void MainWindow::refreshAgentLimitLabel()
{
    if (!m_agentLimitsLabel)
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QSettings settings;
    auto windowText = [&](const QString &key, qint64 windowMs) -> QString {
        const qint64 start = settings.value(key).toLongLong();
        if (start <= 0)
            return QStringLiteral("ready");
        const qint64 remaining = windowMs - (now - start);
        if (remaining <= 0)
            return QStringLiteral("ready");
        return QStringLiteral("resets in %1").arg(humanizeRemaining(remaining));
    };
    auto providerLine = [&](const QString &label, const QString &k5h,
                            const QString &kWeek) {
        return QStringLiteral("%1 — 5h %2 · weekly %3")
            .arg(label, windowText(k5h, kAgentLimit5hMs),
                 windowText(kWeek, kAgentLimitWeekMs));
    };
    m_agentLimitsLabel->setText(
        QStringLiteral("Usage limits · %1 · %2")
            .arg(providerLine(QStringLiteral("Codex"), kCodexLimit5hStartSetting,
                              kCodexLimitWeekStartSetting),
                 providerLine(QStringLiteral("Claude Code"),
                              kClaudeLimit5hStartSetting,
                              kClaudeLimitWeekStartSetting)));
}

void MainWindow::refreshCodexUsageRemaining()
{
    if (!m_navCodexUsage)
        return;
    auto *chart = static_cast<TokenUsageMiniChart *>(m_navCodexUsage);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QSettings settings;
    if (settings.contains(kCodexUsageWindowsSetting)) {
        const QJsonArray rows =
            codexUsageWindows(settings.value(kCodexUsageWindowsSetting));
        QStringList labels;
        QVector<int> remainingPercentages;
        QStringList notes;
        for (const QJsonValue &value : rows) {
            const QJsonObject row = value.toObject();
            labels.append(row.value(QStringLiteral("label")).toString());
            const qint64 resetAt = static_cast<qint64>(
                row.value(QStringLiteral("resetAt")).toDouble());
            if (resetAt > 0 && resetAt <= now) {
                remainingPercentages.append(100);
                notes.append(QStringLiteral("ready"));
                continue;
            }
            remainingPercentages.append(
                100 - qBound(0,
                             row.value(QStringLiteral("usedPercent")).toInt(),
                             100));
            notes.append(resetAt > now
                             ? QStringLiteral("resets in %1")
                                   .arg(humanizeRemaining(resetAt - now))
                             : QString());
        }
        if (labels.isEmpty()) {
            labels = {QStringLiteral("5-hour"), QStringLiteral("Weekly")};
            remainingPercentages = {-1, -1};
            notes = {QString(), QString()};
        }
        chart->setUsageRows(labels, remainingPercentages, notes);
        return;
    }

    // Settings written by releases before multi-bucket snapshots remain
    // readable until the next live account/rateLimits/read response arrives.
    const QString accountId = activeAgentAccount(QStringLiteral("codex")).id;
    auto update = [&](bool weekly, const QString &key, qint64 windowMs,
                      const QString &pctKey, const QString &resetKey) {
        const qint64 providerReset = settings.value(resetKey).toLongLong();
        const qint64 start = settings.value(key).toLongLong();
        const bool windowElapsed =
            providerReset > 0 ? providerReset <= now
                              : (start > 0 && now - start >= windowMs);
        if (windowElapsed) {
            settings.remove(pctKey);
            settings.remove(resetKey);
            settings.remove(agentAccountUsageSetting(
                QStringLiteral("codex"), accountId,
                agentAccountUsageField(pctKey)));
            settings.remove(agentAccountUsageSetting(
                QStringLiteral("codex"), accountId,
                agentAccountUsageField(resetKey)));
        } else if (settings.contains(pctKey)) {
            const int used = qBound(0, settings.value(pctKey).toInt(), 100);
            QString note;
            if (providerReset > now)
                note = QStringLiteral("resets in %1")
                           .arg(humanizeRemaining(providerReset - now));
            else if (start > 0)
                note = QStringLiteral("resets in %1")
                           .arg(humanizeRemaining(windowMs - (now - start)));
            chart->setRemaining(weekly, 100 - used, note);
            return;
        }
        if (start <= 0) {
            chart->setRemaining(weekly, 100, QStringLiteral("ready"));
            return;
        }
        const qint64 remaining = windowMs - (now - start);
        if (remaining <= 0) {
            chart->setRemaining(weekly, 100, QStringLiteral("ready"));
            return;
        }
        const int pct =
            qBound(0, qRound(remaining * 100.0 / double(windowMs)), 100);
        chart->setRemaining(
            weekly, pct,
            QStringLiteral("resets in %1").arg(humanizeRemaining(remaining)));
    };
    update(false, kCodexLimit5hStartSetting, kAgentLimit5hMs,
           kCodexUsage5hPctSetting, kCodexUsage5hResetSetting);
    update(true, kCodexLimitWeekStartSetting, kAgentLimitWeekMs,
           kCodexUsageWeekPctSetting, kCodexUsageWeekResetSetting);
}

void MainWindow::applyClaudeRateLimitEvent(const QJsonObject &info)
{
    QJsonValue utilization = info.value(QStringLiteral("utilization"));
    if (utilization.isUndefined())
        utilization = info.value(QStringLiteral("utilisation"));
    bool valid = utilization.isDouble();
    double used = utilization.toDouble();
    if (!valid && utilization.isString())
        used = utilization.toString().toDouble(&valid);
    if (!valid)
        return;
    const int pct =
        qBound(0, qRound(used >= 0.0 && used <= 1.0 ? used * 100.0 : used), 100);

    QString kind = info.value(QStringLiteral("rateLimitType")).toString().toLower();
    if (kind.isEmpty())
        kind = info.value(QStringLiteral("rate_limit_type")).toString().toLower();
    // Claude Code also reports seven_day_sonnet, seven_day_overage_included and
    // overage. None of those is a gauge we draw, and letting them through would
    // overwrite the all-models weekly or 5-hour figure.
    if (kind.contains(QStringLiteral("sonnet")) ||
        kind.contains(QStringLiteral("overage")))
        return;
    const bool fable = kind.contains(QStringLiteral("fable")) ||
                       kind.contains(QStringLiteral("opus")) ||
                       kind.contains(QStringLiteral("premium"));
    const bool weekly = kind.contains(QStringLiteral("seven")) ||
                        kind.contains(QStringLiteral("week"));

    // The event that says a window is spent is also the one that says when it
    // reopens, so the countdown starts from the run that hit the limit rather
    // than waiting for the next usage poll. A reset already in the past is a
    // window we are no longer in, so it tells us nothing to act on.
    QJsonValue resets = info.value(QStringLiteral("resets_at"));
    if (resets.isUndefined() || resets.isNull())
        resets = info.value(QStringLiteral("resetsAt"));
    qint64 resetMs = 0;
    if (resets.isString()) {
        const QDateTime when =
            QDateTime::fromString(resets.toString(), Qt::ISODate);
        resetMs = when.isValid() ? when.toMSecsSinceEpoch() : 0;
    } else if (resets.isDouble()) {
        resetMs = static_cast<qint64>(resets.toDouble());
        if (resetMs > 0 && resetMs < 10'000'000'000LL)
            resetMs *= 1000; // seconds, as the CLI reports them
    }
    const bool haveReset = resetMs > QDateTime::currentMSecsSinceEpoch();

    // The reset lands first: marking the window spent is what arms the
    // countdown, and it should count down to the time this event just named.
    if (fable) {
        if (haveReset)
            applyClaudeFableReset(resetMs);
        applyClaudeFableUsage(pct);
    } else {
        if (haveReset)
            applyClaudeReset(weekly, resetMs);
        applyClaudeUsage(weekly, pct);
    }
}

void MainWindow::applyCodexRateLimits(
    const QJsonObject &rateLimits,
    const QJsonObject &rateLimitsByLimitId)
{
    if (rateLimits.isEmpty() && rateLimitsByLimitId.isEmpty())
        return;
    const QString accountId = activeAgentAccount(QStringLiteral("codex")).id;
    // The app-server repeats the same rate-limit snapshot on most turns (and
    // several times within one). Everything below — a dozen settings writes,
    // the usage meter, the account menu and the limit label — is a function
    // of the payload and the account, so an identical pair is a no-op.
    if (accountId == m_lastCodexRateLimitsAccount &&
        rateLimits == m_lastCodexRateLimits &&
        rateLimitsByLimitId == m_lastCodexRateLimitsByLimitId)
        return;
    m_lastCodexRateLimitsAccount = accountId;
    m_lastCodexRateLimits = rateLimits;
    m_lastCodexRateLimitsByLimitId = rateLimitsByLimitId;
    QSettings settings;
    auto scoped = [&](const QString &globalKey) {
        return agentAccountUsageSetting(QStringLiteral("codex"), accountId,
                                        agentAccountUsageField(globalKey));
    };
    const QJsonArray rows =
        normalizedCodexUsageWindows(rateLimits, rateLimitsByLimitId);
    const QByteArray encodedRows = codexUsageWindowsJson(rows);
    forkmesh::settings::put(settings, kCodexUsageWindowsSetting, encodedRows);
    forkmesh::settings::put(settings, scoped(kCodexUsageWindowsSetting), encodedRows);

    struct LegacyWindow {
        QString kind;
        QString pctKey;
        QString resetKey;
        QString anchorKey;
        QString exhaustedKey;
        QString displayName;
        bool found = false;
    };
    LegacyWindow fiveHour{QStringLiteral("5h"), kCodexUsage5hPctSetting,
                          kCodexUsage5hResetSetting,
                          kCodexLimit5hStartSetting,
                          kCodexUsage5hExhaustedSetting,
                          QStringLiteral("5-hour")};
    LegacyWindow weekly{QStringLiteral("weekly"), kCodexUsageWeekPctSetting,
                        kCodexUsageWeekResetSetting,
                        kCodexLimitWeekStartSetting,
                        kCodexUsageWeekExhaustedSetting,
                        QStringLiteral("weekly")};
    auto persistGeneral = [&](LegacyWindow &target, const QJsonObject &row) {
        target.found = true;
        const int used = qBound(
            0, row.value(QStringLiteral("usedPercent")).toInt(), 100);
        const qint64 resetAt = static_cast<qint64>(
            row.value(QStringLiteral("resetAt")).toDouble());
        const qint64 durationMs = static_cast<qint64>(
                                       row.value(QStringLiteral(
                                           "windowDurationMins"))
                                           .toDouble()) *
                                   60 * 1000;
        forkmesh::settings::put(settings, target.pctKey, used);
        forkmesh::settings::put(settings, scoped(target.pctKey), used);
        if (resetAt > 0) {
            forkmesh::settings::put(settings, target.resetKey, resetAt);
            forkmesh::settings::put(settings, scoped(target.resetKey), resetAt);
        } else {
            forkmesh::settings::drop(settings, target.resetKey);
            forkmesh::settings::drop(settings, scoped(target.resetKey));
        }
        if (resetAt > 0 && durationMs > 0) {
            forkmesh::settings::put(settings, target.anchorKey, resetAt - durationMs);
            forkmesh::settings::put(settings, scoped(target.anchorKey), resetAt - durationMs);
        } else {
            forkmesh::settings::drop(settings, target.anchorKey);
            forkmesh::settings::drop(settings, scoped(target.anchorKey));
        }
        const bool wasExhausted =
            settings.value(target.exhaustedKey, false).toBool();
        if (used >= 99) {
            forkmesh::settings::put(settings, target.exhaustedKey, true);
            scheduleUsageLimitReminder(QStringLiteral("codex"), target.kind,
                                       QStringLiteral("Codex"),
                                       target.displayName, resetAt);
        } else if (used < 90 && wasExhausted) {
            forkmesh::settings::put(settings, target.exhaustedKey, false);
            notifyUsageLimitReady(QStringLiteral("codex"), target.kind,
                                  QStringLiteral("Codex"),
                                  target.displayName, resetAt);
        }
    };
    for (const QJsonValue &value : rows) {
        const QJsonObject row = value.toObject();
        if (!row.value(QStringLiteral("general")).toBool())
            continue;
        if (row.value(QStringLiteral("kind")).toString() == fiveHour.kind)
            persistGeneral(fiveHour, row);
        else if (row.value(QStringLiteral("kind")).toString() == weekly.kind)
            persistGeneral(weekly, row);
    }
    auto clearMissing = [&](const LegacyWindow &target) {
        if (target.found)
            return;
        forkmesh::settings::drop(settings, target.pctKey);
        forkmesh::settings::drop(settings, target.resetKey);
        forkmesh::settings::drop(settings, target.anchorKey);
        forkmesh::settings::drop(settings, scoped(target.pctKey));
        forkmesh::settings::drop(settings, scoped(target.resetKey));
        forkmesh::settings::drop(settings, scoped(target.anchorKey));
        forkmesh::settings::put(settings, target.exhaustedKey, false);
    };
    clearMissing(fiveHour);
    clearMissing(weekly);

    refreshCodexUsageRemaining();
    refreshAgentAccountUsageMenu(QStringLiteral("codex"));
    refreshAgentLimitLabel();
}

void MainWindow::updateAgentTotalSpend()
{
    if (!m_agentTotalSpend)
        return;
    const bool haveOpenAi = !qIsNaN(m_openAiSpendUsd);
    const bool haveClaude = !qIsNaN(m_claudeSpendUsd);
    if (!haveOpenAi && !haveClaude) {
        m_agentTotalSpend->setText(
            "Total Agent API spend this month: not yet refreshed");
        return;
    }
    const double total = (haveOpenAi ? m_openAiSpendUsd : 0.0) +
                         (haveClaude ? m_claudeSpendUsd : 0.0);
    QString note;
    if (!haveOpenAi)
        note = QString::fromUtf8(" (Claude only \xE2\x80\x94 refresh OpenAI)");
    else if (!haveClaude)
        note = QString::fromUtf8(" (OpenAI only \xE2\x80\x94 refresh Claude)");
    m_agentTotalSpend->setText(
        QStringLiteral("Total Agent API spend, month to date: $%1 USD%2")
            .arg(QString::number(total, 'f', 2), note));
}

void MainWindow::applyClaudeUsage(bool weekly, int percent)
{
    const int pct = qBound(0, percent, 100);
    int &lastPct = weekly ? m_claudeUsageLastWeekPct : m_claudeUsageLast5hPct;
    if (lastPct == pct)
        return;
    lastPct = pct;
    if (m_navTokenUsage)
        static_cast<TokenUsageMiniChart *>(m_navTokenUsage)->setUsage(weekly, pct);
    QSettings settings;
    const QString pctKey = weekly ? kClaudeUsageWeekPctSetting
                                  : kClaudeUsage5hPctSetting;
    forkmesh::settings::put(settings, pctKey, pct);
    forkmesh::settings::put(settings, 
        agentAccountUsageSetting(
            QStringLiteral("claude-code"),
            activeAgentAccount(QStringLiteral("claude-code")).id,
            agentAccountUsageField(pctKey)),
        pct);
    const QString exhaustedKey = weekly ? kClaudeUsageWeekExhaustedSetting
                                        : kClaudeUsage5hExhaustedSetting;
    const QString windowKey = weekly ? QStringLiteral("weekly")
                                     : QStringLiteral("5h");
    const QString windowName = weekly ? QStringLiteral("weekly")
                                      : QStringLiteral("5-hour");
    const QString resetKey = weekly ? kClaudeUsageWeekResetSetting
                                    : kClaudeUsage5hResetSetting;
    if (pct >= 99) {
        forkmesh::settings::put(settings, exhaustedKey, true);
        scheduleUsageLimitReminder(QStringLiteral("claude"), windowKey,
                                   QStringLiteral("Claude Code"), windowName,
                                   settings.value(resetKey).toLongLong());
    } else if (pct < 90 && settings.value(exhaustedKey, false).toBool()) {
        forkmesh::settings::put(settings, exhaustedKey, false);
        notifyUsageLimitReady(QStringLiteral("claude"), windowKey,
                              QStringLiteral("Claude Code"), windowName,
                              settings.value(resetKey).toLongLong());
        maybeEmailCreditsRefilled(weekly);
    }
}

void MainWindow::maybeEmailCreditsRefilled(bool weekly)
{
    if (!QSettings().value(kEmailOnCreditsRefillSetting, false).toBool())
        return;
    // Rides the existing signed heartbeat channel (accounts/heartbeat) rather
    // than a dedicated endpoint; the worker turns the flag into an in-app
    // notification that the email digest cron mails out.
    if (weekly)
        m_pendingCreditsRefilledWeekly = true;
    else
        m_pendingCreditsRefilled5h = true;
    sendNodeHeartbeat();
}

void MainWindow::applyClaudeFableUsage(int percent)
{
    const int pct = qBound(0, percent, 100);
    if (m_navTokenUsage)
        static_cast<TokenUsageMiniChart *>(m_navTokenUsage)
            ->setUsage(TokenUsageMiniChart::Fable, pct);
    QSettings settings;
    forkmesh::settings::put(settings, kClaudeUsageFablePctSetting, pct);
    forkmesh::settings::put(settings, 
        agentAccountUsageSetting(
            QStringLiteral("claude-code"),
            activeAgentAccount(QStringLiteral("claude-code")).id,
            agentAccountUsageField(kClaudeUsageFablePctSetting)),
        pct);
    if (pct >= 99) {
        forkmesh::settings::put(settings, kClaudeUsageFableExhaustedSetting, true);
        scheduleUsageLimitReminder(QStringLiteral("claude"),
                                   QStringLiteral("fable"),
                                   QStringLiteral("Claude Code"),
                                   QStringLiteral("Fable weekly"),
                                   settings.value(kClaudeUsageFableResetSetting)
                                       .toLongLong());
    } else if (pct < 90 &&
               settings.value(kClaudeUsageFableExhaustedSetting, false).toBool()) {
        forkmesh::settings::put(settings, kClaudeUsageFableExhaustedSetting, false);
        notifyUsageLimitReady(QStringLiteral("claude"),
                              QStringLiteral("fable"),
                              QStringLiteral("Claude Code"),
                              QStringLiteral("Fable weekly"),
                              settings.value(kClaudeUsageFableResetSetting)
                                  .toLongLong());
    }
}

void MainWindow::applyClaudeFableReset(qint64 resetMs)
{
    QSettings settings;
    forkmesh::settings::put(settings, kClaudeUsageFableResetSetting, resetMs);
    forkmesh::settings::put(settings, 
        agentAccountUsageSetting(
            QStringLiteral("claude-code"),
            activeAgentAccount(QStringLiteral("claude-code")).id,
            agentAccountUsageField(kClaudeUsageFableResetSetting)),
        resetMs);
    if (settings.value(kClaudeUsageFableExhaustedSetting, false).toBool()) {
        scheduleUsageLimitReminder(QStringLiteral("claude"),
                                   QStringLiteral("fable"),
                                   QStringLiteral("Claude Code"),
                                   QStringLiteral("Fable weekly"), resetMs);
    }
    if (!m_navTokenUsage)
        return;
    const qint64 remaining = resetMs - QDateTime::currentMSecsSinceEpoch();
    static_cast<TokenUsageMiniChart *>(m_navTokenUsage)
        ->setReset(TokenUsageMiniChart::Fable,
                   remaining > 0 ? humanizeRemaining(remaining) : QString());
}

void MainWindow::flashUsageChart(QWidget *chart, bool ok)
{
    if (chart)
        static_cast<TokenUsageMiniChart *>(chart)->flashRefresh(ok);
}

void MainWindow::applyClaudeReset(bool weekly, qint64 resetMs)
{
    const QString accountId =
        activeAgentAccount(QStringLiteral("claude-code")).id;
    QSettings settings;
    const QString resetKey = weekly ? kClaudeUsageWeekResetSetting
                                    : kClaudeUsage5hResetSetting;
    const QString exhaustedKey = weekly ? kClaudeUsageWeekExhaustedSetting
                                        : kClaudeUsage5hExhaustedSetting;
    forkmesh::settings::put(settings, resetKey, resetMs);
    forkmesh::settings::put(settings,
        agentAccountUsageSetting(QStringLiteral("claude-code"), accountId,
                                 agentAccountUsageField(resetKey)),
        resetMs);
    if (settings.value(exhaustedKey, false).toBool()) {
        scheduleUsageLimitReminder(
            QStringLiteral("claude"),
            weekly ? QStringLiteral("weekly") : QStringLiteral("5h"),
            QStringLiteral("Claude Code"),
            weekly ? QStringLiteral("weekly") : QStringLiteral("5-hour"),
            resetMs);
    }
    if (!m_navTokenUsage)
        return;
    const qint64 remaining = resetMs - QDateTime::currentMSecsSinceEpoch();
    static_cast<TokenUsageMiniChart *>(m_navTokenUsage)
        ->setReset(weekly, remaining > 0 ? humanizeRemaining(remaining)
                                         : QString());
}

void MainWindow::applyClaudeUsageCredits(const QJsonValue &extraUsage)
{
    QSettings settings;
    const QString accountId =
        activeAgentAccount(QStringLiteral("claude-code")).id;
    auto store = [&](const QString &globalKey, const QVariant &value) {
        const QString scoped = agentAccountUsageSetting(
            QStringLiteral("claude-code"), accountId,
            agentAccountUsageField(globalKey));
        if (value.isValid()) {
            forkmesh::settings::put(settings, globalKey, value);
            forkmesh::settings::put(settings, scoped, value);
        } else {
            forkmesh::settings::drop(settings, globalKey);
            forkmesh::settings::drop(settings, scoped);
        }
    };
    // A nullable money field: null means "no figure", which is not the same as
    // zero — an unset monthly limit is an unlimited one.
    auto amount = [](const QJsonObject &object,
                     const QString &key) -> QVariant {
        const QJsonValue value = object.value(key);
        if (value.isDouble())
            return value.toDouble();
        bool numeric = false;
        if (value.isString()) {
            const double parsed = value.toString().toDouble(&numeric);
            if (numeric)
                return parsed;
        }
        return {};
    };

    // Null (or an object that never states whether credits are on) means the
    // account has no credit balance to report, and anything remembered from a
    // previous account or a previous plan has to go.
    if (!extraUsage.isObject() ||
        !extraUsage.toObject().value(QStringLiteral("is_enabled")).isBool()) {
        for (const QString &key :
             {kClaudeCreditsEnabledSetting, kClaudeCreditsUsedSetting,
              kClaudeCreditsLimitSetting, kClaudeCreditsPctSetting,
              kClaudeCreditsCurrencySetting})
            store(key, {});
        return;
    }

    const QJsonObject credits = extraUsage.toObject();
    const QVariant used = amount(credits, QStringLiteral("used_credits"));
    const QVariant limit = amount(credits, QStringLiteral("monthly_limit"));
    store(kClaudeCreditsEnabledSetting,
          credits.value(QStringLiteral("is_enabled")).toBool());
    store(kClaudeCreditsUsedSetting, used);
    store(kClaudeCreditsLimitSetting, limit);
    const QString currency =
        credits.value(QStringLiteral("currency")).toString().trimmed();
    store(kClaudeCreditsCurrencySetting,
          currency.isEmpty() ? QVariant() : QVariant(currency));

    QVariant percent;
    if (limit.isValid() && limit.toDouble() > 0.0 && used.isValid()) {
        percent = qBound(
            0, qRound(used.toDouble() * 100.0 / limit.toDouble()), 100);
    } else if (const QVariant reported =
                   amount(credits, QStringLiteral("utilization"));
               reported.isValid()) {
        // Unlike the rate-limit windows, credit utilization is reported as a
        // share of the monthly limit already scaled to 0-100.
        percent = qBound(0, qRound(reported.toDouble()), 100);
    }
    store(kClaudeCreditsPctSetting, percent);
}

void MainWindow::applyClaudeUsageResponse(const QJsonObject &root,
                                          const QString &accountId)
{
    const bool activeAccount =
        accountId.isEmpty() ||
        accountId == activeAgentAccount(QStringLiteral("claude-code")).id;
    auto percentage = [](const QJsonObject &window, bool *ok) {
        QJsonValue value = window.value(QStringLiteral("utilization"));
        if (value.isUndefined())
            value = window.value(QStringLiteral("utilisation"));
        if (value.isUndefined())
            value = window.value(QStringLiteral("used_percent"));
        bool numeric = value.isDouble();
        double amount = value.toDouble();
        if (!numeric && value.isString())
            amount = value.toString().toDouble(&numeric);
        *ok = numeric;
        return qBound(0, qRound(amount >= 0.0 && amount <= 1.0
                                    ? amount * 100.0
                                    : amount),
                      100);
    };
    auto resetMs = [](const QJsonObject &window) -> qint64 {
        QJsonValue value = window.value(QStringLiteral("resets_at"));
        if (value.isUndefined() || value.isNull())
            value = window.value(QStringLiteral("resetsAt"));
        if (value.isString()) {
            const QDateTime when =
                QDateTime::fromString(value.toString(), Qt::ISODate);
            return when.isValid() ? when.toMSecsSinceEpoch() : 0;
        }
        if (!value.isDouble())
            return 0;
        qint64 stamp = static_cast<qint64>(value.toDouble());
        if (stamp > 0 && stamp < 10'000'000'000LL)
            stamp *= 1000;
        return stamp;
    };
    // A window with no reset time has not started yet (nothing used in it), so
    // an old countdown must be cleared rather than left ticking in the tooltip.
    auto applyWindow = [&](TokenUsageMiniChart::Window window, int pct,
                           qint64 reset) {
        if (!activeAccount) {
            const bool fable = window == TokenUsageMiniChart::Fable;
            const bool weekly = window == TokenUsageMiniChart::Weekly;
            const QString pctKey = fable ? kClaudeUsageFablePctSetting
                                   : weekly ? kClaudeUsageWeekPctSetting
                                            : kClaudeUsage5hPctSetting;
            const QString resetKey = fable ? kClaudeUsageFableResetSetting
                                     : weekly ? kClaudeUsageWeekResetSetting
                                              : kClaudeUsage5hResetSetting;
            QSettings settings;
            forkmesh::settings::put(
                settings,
                agentAccountUsageSetting(QStringLiteral("claude-code"), accountId,
                                         agentAccountUsageField(pctKey)),
                pct);
            forkmesh::settings::put(
                settings,
                agentAccountUsageSetting(QStringLiteral("claude-code"), accountId,
                                         agentAccountUsageField(resetKey)),
                reset);
            return;
        }
        if (window == TokenUsageMiniChart::Fable) {
            applyClaudeFableUsage(pct);
            applyClaudeFableReset(reset);
        } else {
            const bool weekly = window == TokenUsageMiniChart::Weekly;
            applyClaudeUsage(weekly, pct);
            applyClaudeReset(weekly, reset);
        }
    };
    auto apply = [&](const QString &key, TokenUsageMiniChart::Window window) {
        const QJsonObject details = root.value(key).toObject();
        bool valid = false;
        const int pct = percentage(details, &valid);
        if (!valid)
            return false;
        applyWindow(window, pct, resetMs(details));
        return true;
    };
    // The newer payload also lists every limit in "limits": [{kind, percent,
    // resets_at, scope}], and model-scoped weekly limits (Fable) appear ONLY
    // there — the named seven_day_<model> keys for them stay null. "percent"
    // is always a 0-100 integer, so it skips the fraction heuristic above.
    const QJsonArray limits = root.value(QStringLiteral("limits")).toArray();
    auto applyLimit = [&](TokenUsageMiniChart::Window window,
                          const std::function<bool(const QJsonObject &)> &matches) {
        for (const QJsonValue &value : limits) {
            const QJsonObject limit = value.toObject();
            if (!matches(limit))
                continue;
            const QJsonValue percent = limit.value(QStringLiteral("percent"));
            bool numeric = percent.isDouble();
            double amount = percent.toDouble();
            if (!numeric && percent.isString())
                amount = percent.toString().toDouble(&numeric);
            if (!numeric)
                continue;
            applyWindow(window, qBound(0, qRound(amount), 100), resetMs(limit));
            return true;
        }
        return false;
    };
    auto limitKind = [](const QString &kind) {
        return [kind](const QJsonObject &limit) {
            return limit.value(QStringLiteral("kind")).toString() == kind &&
                   !limit.value(QStringLiteral("scope")).isObject();
        };
    };

    if (!apply(QStringLiteral("five_hour"), TokenUsageMiniChart::FiveHour))
        applyLimit(TokenUsageMiniChart::FiveHour,
                   limitKind(QStringLiteral("session")));
    if (!apply(QStringLiteral("seven_day"), TokenUsageMiniChart::Weekly))
        applyLimit(TokenUsageMiniChart::Weekly,
                   limitKind(QStringLiteral("weekly_all")));
    // Usage credits are not a rate-limit window — they are a money balance in
    // the same payload — so they bypass the percentage plumbing above. A
    // missing key means this response says nothing about them; an explicit
    // null means the account has none, and clears what we knew.
    if (activeAccount && root.contains(QStringLiteral("extra_usage")))
        applyClaudeUsageCredits(root.value(QStringLiteral("extra_usage")));

    const QStringList explicitFableKeys = {
        QStringLiteral("seven_day_fable"),
        QStringLiteral("seven_day_fable_5"),
        QStringLiteral("seven_day_fable5")};
    bool fableApplied = applyLimit(
        TokenUsageMiniChart::Fable, [](const QJsonObject &limit) {
            const QJsonObject model = limit.value(QStringLiteral("scope"))
                                          .toObject()
                                          .value(QStringLiteral("model"))
                                          .toObject();
            return (model.value(QStringLiteral("display_name")).toString() +
                    QLatin1Char(' ') +
                    model.value(QStringLiteral("id")).toString())
                .contains(QStringLiteral("fable"), Qt::CaseInsensitive);
        });
    for (const QString &key : explicitFableKeys) {
        if (fableApplied)
            break;
        if (apply(key, TokenUsageMiniChart::Fable)) {
            fableApplied = true;
            break;
        }
    }
    for (auto it = root.constBegin(); it != root.constEnd(); ++it) {
        if (fableApplied)
            break;
        const QJsonObject details = it.value().toObject();
        const QString metadata =
            (it.key() + QLatin1Char(' ') +
             details.value(QStringLiteral("model")).toString() + QLatin1Char(' ') +
             details.value(QStringLiteral("model_name")).toString() + QLatin1Char(' ') +
             details.value(QStringLiteral("label")).toString()).toLower();
        if (metadata.contains(QStringLiteral("fable")) &&
            apply(it.key(), TokenUsageMiniChart::Fable)) {
            fableApplied = true;
            break;
        }
    }
    const QStringList legacyPremiumKeys = {
        QStringLiteral("seven_day_premium"),
        QStringLiteral("seven_day_opus")};
    if (!fableApplied) {
        for (const QString &key : legacyPremiumKeys) {
            if (apply(key, TokenUsageMiniChart::Fable)) {
                fableApplied = true;
                break;
            }
        }
    }
    refreshAgentAccountUsageMenu(QStringLiteral("claude-code"));
}

void MainWindow::refreshClaudeCodeUsage(bool fromHover)
{
    auto giveUp = [this, fromHover] {
        if (fromHover)
            flashUsageChart(m_navTokenUsage, false);
    };
    if (!m_networkAccess) {
        giveUp();
        return;
    }
    // Claude Code authenticates with a claude.ai OAuth token, kept in
    // ~/.claude/.credentials.json. Read the access token fresh every poll so a
    // token the CLI has since rotated is picked up automatically; if it is
    // absent (API-key login, or not signed in) there is nothing to query and the
    // rate-limit-event path remains the only feed.
    const QString token = claudeCodeOAuthToken();
    if (token.isEmpty()) {
        giveUp();
        return;
    }
    const QString accountId =
        activeAgentAccount(QStringLiteral("claude-code")).id;
    const QString pollKey = QStringLiteral("claude-usage-%1").arg(accountId);
    if (!m_pollBackoff.ready(pollKey,
                             QDateTime::currentMSecsSinceEpoch())) {
        giveUp();
        return;
    }

    QNetworkRequest req(
        QUrl(QStringLiteral("https://api.anthropic.com/api/oauth/usage")));
    req.setRawHeader("Authorization", "Bearer " + token.toUtf8());
    req.setRawHeader("anthropic-beta", "oauth-2025-04-20");
    req.setRawHeader("Accept", "application/json");

    QNetworkReply *reply = m_networkAccess->get(req);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, fromHover, pollKey, accountId] {
        const QByteArray body = reply->readAll();
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            const QByteArray retryAfter = reply->rawHeader("Retry-After");
            bool retryAfterOk = false;
            const qint64 retryAfterMs =
                QString::fromLatin1(retryAfter).trimmed().toLongLong(&retryAfterOk) * 1000;
            m_pollBackoff.noteFailure(
                pollKey, QDateTime::currentMSecsSinceEpoch(),
                NetworkBackoff::kDefaultBaseMs, NetworkBackoff::kDefaultCapMs,
                retryAfterOk && retryAfterMs > 0 ? retryAfterMs : 0);
            if (fromHover)
                flashUsageChart(m_navTokenUsage, false);
            return;
        }
        m_pollBackoff.noteSuccess(pollKey);
        if (activeAgentAccount(QStringLiteral("claude-code")).id != accountId)
            return;
        if (fromHover)
            flashUsageChart(m_navTokenUsage, true);
        applyClaudeUsageResponse(QJsonDocument::fromJson(body).object());
    });
}

void MainWindow::refreshAgentUsageAccounts(std::function<void()> done)
{
    refreshCodexUsageRemaining();
    auto queue = std::make_shared<QList<AgentAccountProfile>>();
    if (m_networkAccess) {
        for (const AgentAccountProfile &profile :
             agentAccountProfiles(QStringLiteral("claude-code")))
            queue->append(profile);
    }
    auto next = std::make_shared<std::function<void()>>();
    const std::weak_ptr<std::function<void()>> weakNext = next;
    *next = [this, queue, weakNext, done = std::move(done)]() mutable {
        const std::shared_ptr<std::function<void()>> next = weakNext.lock();
        while (next && !queue->isEmpty()) {
            const AgentAccountProfile profile = queue->takeFirst();
            const QString configDir =
                profile.configDir.isEmpty()
                    ? agentAccountDefaultConfigDir(QStringLiteral("claude-code"))
                    : profile.configDir;
            const QString token = claudeCodeOAuthToken(configDir);
            const QString pollKey =
                QStringLiteral("claude-usage-%1").arg(profile.id);
            if (token.isEmpty() ||
                !m_pollBackoff.ready(pollKey, QDateTime::currentMSecsSinceEpoch()))
                continue;
            QNetworkRequest req(
                QUrl(QStringLiteral("https://api.anthropic.com/api/oauth/usage")));
            req.setRawHeader("Authorization", "Bearer " + token.toUtf8());
            req.setRawHeader("anthropic-beta", "oauth-2025-04-20");
            req.setRawHeader("Accept", "application/json");
            req.setTransferTimeout(15000);
            QNetworkReply *reply = m_networkAccess->get(req);
            connect(reply, &QNetworkReply::finished, this,
                    [this, reply, pollKey, next, accountId = profile.id] {
                const QByteArray body = reply->readAll();
                reply->deleteLater();
                if (reply->error() != QNetworkReply::NoError) {
                    m_pollBackoff.noteFailure(pollKey,
                                              QDateTime::currentMSecsSinceEpoch());
                } else {
                    m_pollBackoff.noteSuccess(pollKey);
                    applyClaudeUsageResponse(
                        QJsonDocument::fromJson(body).object(), accountId);
                }
                (*next)();
            });
            return;
        }
        refreshAgentAccountUsageMenu(QStringLiteral("claude-code"));
        scheduleAgentUsagePagesRefresh();
        if (done)
            std::exchange(done, nullptr)();
    };
    (*next)();
}

void MainWindow::applyLiveClaudeModelsToCombos()
{
    if (m_liveClaudeModels.isEmpty())
        return;
    const QJsonArray &models = m_liveClaudeModels;
    mergeLiveClaudeModels(m_quickAddClaudeModel, models);
    if (m_quickAddClaudeModel) {
        const QString saved =
            QSettings().value(kClaudeCodeModelSetting).toString().trimmed();
        const int idx = m_quickAddClaudeModel->findData(saved);
        if (idx >= 0) {
            QSignalBlocker b(m_quickAddClaudeModel);
            m_quickAddClaudeModel->setCurrentIndex(idx);
        }
    }
    const QString claudeCode = QStringLiteral("claude-code");
    if (m_branchFixModelCombo && m_branchFixAgentCombo &&
        m_branchFixAgentCombo->currentData().toString() == claudeCode)
        mergeLiveClaudeModels(m_branchFixModelCombo, models);
    if (m_actionFixModelCombo && m_actionFixAgentCombo &&
        m_actionFixAgentCombo->currentData().toString() == claudeCode)
        mergeLiveClaudeModels(m_actionFixModelCombo, models);
    if (m_issueAgentModel && m_issueAgentProvider &&
        m_issueAgentProvider->currentData().toString() == claudeCode)
        mergeLiveClaudeModels(m_issueAgentModel, models);
    refreshAgentModelSelectors();
    refreshComposerModelVisibilityList();
}

void MainWindow::refreshClaudeModelCombo()
{
    applyLiveClaudeModelsToCombos();

    if (!m_networkAccess)
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_claudeModelsFetchedMs > 0 &&
        now - m_claudeModelsFetchedMs < 60LL * 1000)
        return;
    // Claude Code authenticates with the claude.ai OAuth token in
    // ~/.claude/.credentials.json. Without it we can't query the model list.
    const QString token = claudeCodeOAuthToken();
    if (token.isEmpty())
        return;
    m_claudeModelsFetchedMs = now;

    QNetworkRequest req(QUrl(
        QStringLiteral("https://api.anthropic.com/v1/models?limit=1000")));
    req.setRawHeader("Authorization", "Bearer " + token.toUtf8());
    req.setRawHeader("anthropic-beta", "oauth-2025-04-20");
    req.setRawHeader("anthropic-version", "2023-06-01");
    req.setRawHeader("Accept", "application/json");

    QNetworkReply *reply = m_networkAccess->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const QByteArray body = reply->readAll();
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
            return;
        const QJsonArray models =
            QJsonDocument::fromJson(body).object().value("data").toArray();
        if (models.isEmpty())
            return;
        m_liveClaudeModels = models;
        QSettings().setValue(kClaudeModelsCacheSetting,
                             QJsonDocument(models).toJson(QJsonDocument::Compact));
        applyLiveClaudeModelsToCombos();
    });
}

void MainWindow::refreshClaudeSpend()
{
    const QString adminKey =
        QSettings().value(kClaudeAdminKeySetting).toString().trimmed();
    if (adminKey.isEmpty()) {
        if (m_agentClaudeStatus)
            m_agentClaudeStatus->setText(
                "Add a Claude Admin API key (sk-ant-admin01-…) in Settings to "
                "see spend. A regular API key can't read organization costs.");
        if (m_agentClaudeSpend)
            m_agentClaudeSpend->setText("Claude spend this month: needs Admin key");
        if (m_agentClaudeCredit)
            m_agentClaudeCredit->setText(
                "Claude remaining credits: add an Admin key to see spend; "
                "remaining balance is visible in the Anthropic Console.");
        return;
    }
    if (!m_networkAccess) {
        if (m_agentClaudeStatus)
            m_agentClaudeStatus->setText("Network client is not ready.");
        return;
    }

    if (m_agentClaudeStatus)
        m_agentClaudeStatus->setText("Refreshing Claude spend...");

    // The Cost API takes RFC 3339 timestamps and daily buckets; start at the
    // first of the month (UTC) through the next midnight so today is included.
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QDateTime monthStart(QDate(now.date().year(), now.date().month(), 1),
                               QTime(0, 0), QTimeZone::utc());
    const QDateTime end(now.date().addDays(1), QTime(0, 0),
                        QTimeZone::utc());
    const QString iso = QStringLiteral("yyyy-MM-ddTHH:mm:ssZ");
    const QString startStr = monthStart.toString(iso);
    const QString endStr = end.toString(iso);

    auto cents = std::make_shared<double>(0.0);
    auto fetchPage = std::make_shared<std::function<void(const QString &)>>();
    *fetchPage = [this, adminKey, startStr, endStr, iso, cents,
                  fetchPage](const QString &page) {
        QUrl url(QStringLiteral(
            "https://api.anthropic.com/v1/organizations/cost_report"));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("starting_at"), startStr);
        query.addQueryItem(QStringLiteral("ending_at"), endStr);
        query.addQueryItem(QStringLiteral("bucket_width"), QStringLiteral("1d"));
        if (!page.isEmpty())
            query.addQueryItem(QStringLiteral("page"), page);
        url.setQuery(query);

        QNetworkRequest request(url);
        request.setRawHeader("x-api-key", adminKey.toUtf8());
        request.setRawHeader("anthropic-version", "2023-06-01");
        request.setRawHeader("Accept", "application/json");

        QNetworkReply *reply = m_networkAccess->get(request);
        connect(reply, &QNetworkReply::finished, this,
                [this, reply, cents, fetchPage] {
            const QByteArray body = reply->readAll();
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                if (m_agentClaudeStatus)
                    m_agentClaudeStatus->setText(
                        QStringLiteral("Claude spend unavailable: %1")
                            .arg(apiErrorSummary(reply, body)));
                if (m_agentClaudeSpend)
                    m_agentClaudeSpend->setText(
                        "Claude spend this month: unavailable");
                return;
            }
            const QJsonObject root = QJsonDocument::fromJson(body).object();
            for (const QJsonValue &bucket : root.value("data").toArray()) {
                for (const QJsonValue &result :
                     bucket.toObject().value("results").toArray()) {
                    *cents += result.toObject()
                                  .value("amount")
                                  .toString()
                                  .toDouble();
                }
            }
            const QString next = root.value("next_page").toString();
            if (root.value("has_more").toBool() && !next.isEmpty()) {
                (*fetchPage)(next);
                return;
            }

            const double usd = *cents / 100.0;
            m_claudeSpendUsd = usd;
            if (m_agentClaudeSpend) {
                const QString text =
                    QStringLiteral("Claude spend, month to date: $%1 USD")
                        .arg(QString::number(usd, 'f', 2));
                m_agentClaudeSpend->setText(text);
                cacheSpendLabel(kClaudeSpendTextSetting, kClaudeSpendTsSetting,
                                text);
            }
            if (m_agentClaudeStatus)
                m_agentClaudeStatus->setText("Claude spend refreshed.");
            if (m_agentClaudeCredit) {
                double totalCreditUsed = 0.0;
                for (const AgentSession &s : std::as_const(m_agentSessions)) {
                    if (s.provider.startsWith(QLatin1String("claude")))
                        totalCreditUsed += s.costUsd;
                }
                const QString creditText =
                    QStringLiteral(
                        "Claude remaining credits: check the Anthropic Console "
                        "(API does not expose credit balance). "
                        "Estimated local session spend: $%1 USD.")
                        .arg(QString::number(totalCreditUsed, 'f', 4));
                m_agentClaudeCredit->setText(creditText);
                cacheSpendLabel(kClaudeCreditTextSetting, kClaudeCreditTsSetting, creditText);
            }
            updateAgentTotalSpend();
        });
    };
    (*fetchPage)(QString());
}
