#include "InvoicesModel.h"

#include <QDateTime>
#include <QHash>
#include <QLocale>
#include <QRegularExpression>
#include <QTimeZone>

#include <algorithm>
#include <cmath>

namespace forkmesh::invoices {

namespace {

const QString kDot = QString::fromUtf8(" \xC2\xB7 ");
const QString kDash = QString::fromUtf8("\xE2\x80\x94");

QString numberText(double value, int decimals)
{
    return QLocale(QLocale::English, QLocale::UnitedStates).toString(value, 'f', decimals);
}

}

QString text(const QJsonObject &object, const char *key)
{
    return object.value(QLatin1String(key)).toString();
}

qint64 integer(const QJsonObject &object, const char *key)
{
    return qint64(std::llround(object.value(QLatin1String(key)).toDouble()));
}

QString currencySymbol(const QString &currency)
{
    static const QHash<QString, QString> symbols{
        {QStringLiteral("USD"), QStringLiteral("$")},
        {QStringLiteral("EUR"), QString::fromUtf8("\xE2\x82\xAC")},
        {QStringLiteral("GBP"), QString::fromUtf8("\xC2\xA3")},
        {QStringLiteral("JPY"), QString::fromUtf8("\xC2\xA5")},
        {QStringLiteral("INR"), QString::fromUtf8("\xE2\x82\xB9")},
        {QStringLiteral("CAD"), QStringLiteral("CA$")},
        {QStringLiteral("AUD"), QStringLiteral("A$")},
        {QStringLiteral("NZD"), QStringLiteral("NZ$")},
    };
    const QString code = currency.trimmed().toUpper();
    return symbols.value(code, code.isEmpty() ? QStringLiteral("$") : code + QLatin1Char(' '));
}

QString moneyText(qint64 cents, const QString &currency)
{
    const QString sign = cents < 0 ? QStringLiteral("-") : QString();
    return sign + currencySymbol(currency) + numberText(std::abs(double(cents)) / 100.0, 2);
}

QString moneyMapText(const QJsonObject &amounts, const QString &fallbackCurrency)
{
    QStringList parts;
    QStringList keys = amounts.keys();
    std::sort(keys.begin(), keys.end());
    for (const QString &currency : std::as_const(keys)) {
        const qint64 value = qint64(std::llround(amounts.value(currency).toDouble()));
        if (value)
            parts << moneyText(value, currency);
    }
    if (parts.isEmpty())
        return moneyText(0, fallbackCurrency.isEmpty() ? QStringLiteral("USD") : fallbackCurrency);
    return parts.join(kDot);
}

qint64 parseCents(const QString &value)
{
    QString cleaned = value;
    cleaned.remove(QRegularExpression(QStringLiteral("[^0-9.\\-]")));
    bool ok = false;
    const double number = cleaned.toDouble(&ok);
    if (!ok || !std::isfinite(number))
        return 0;
    return std::max<qint64>(0, qint64(std::llround(number * 100)));
}

QString centsInput(qint64 cents)
{
    return QString::number(double(cents) / 100.0, 'f', 2);
}

int basisPointsFromPercent(double percent)
{
    return std::clamp(int(std::lround(percent * 100)), 0, 100000);
}

QString percentText(int basisPoints)
{
    QString value = QString::number(double(basisPoints) / 100.0, 'f', 2);
    while (value.contains(QLatin1Char('.')) && (value.endsWith(QLatin1Char('0')) || value.endsWith(QLatin1Char('.'))))
        value.chop(1);
    return value + QLatin1Char('%');
}

qint64 lineAmount(const InvoiceLine &line)
{
    return qint64(std::llround(std::max(0.0, line.quantity) * double(line.unitAmount)));
}

InvoiceTotals totals(const QList<InvoiceLine> &lines, int taxRate)
{
    InvoiceTotals result;
    for (const InvoiceLine &line : lines)
        result.subtotal += lineAmount(line);
    result.tax = qint64(std::llround(double(result.subtotal) * taxRate / 10000.0));
    result.total = result.subtotal + result.tax;
    return result;
}

QList<InvoiceLine> invoiceLines(const QJsonArray &items)
{
    QList<InvoiceLine> lines;
    for (const QJsonValue &value : items) {
        const QJsonObject item = value.toObject();
        lines.append({text(item, "description"), item.value(QStringLiteral("quantity")).toDouble(1),
                      integer(item, "unitAmount")});
    }
    return lines;
}

QJsonArray linesJson(const QList<InvoiceLine> &lines)
{
    QJsonArray items;
    for (const InvoiceLine &line : lines) {
        if (line.description.trimmed().isEmpty() && !line.unitAmount)
            continue;
        items.append(QJsonObject{{QStringLiteral("description"), line.description},
                                 {QStringLiteral("quantity"), line.quantity},
                                 {QStringLiteral("unitAmount"), double(line.unitAmount)}});
    }
    return items;
}

QString statusKey(const QJsonObject &invoice)
{
    const QString display = text(invoice, "displayStatus");
    if (!display.isEmpty())
        return display;
    const QString status = text(invoice, "status");
    return status.isEmpty() ? QStringLiteral("draft") : status;
}

QString statusLabel(const QString &key)
{
    static const QHash<QString, QString> labels{
        {QStringLiteral("draft"), QStringLiteral("Draft")},
        {QStringLiteral("sent"), QStringLiteral("Sent")},
        {QStringLiteral("partial"), QStringLiteral("Part paid")},
        {QStringLiteral("overdue"), QStringLiteral("Overdue")},
        {QStringLiteral("paid"), QStringLiteral("Paid")},
        {QStringLiteral("void"), QStringLiteral("Void")},
    };
    return labels.value(key, key);
}

QStringList filterKeys()
{
    return {QString(), QStringLiteral("open"), QStringLiteral("overdue"), QStringLiteral("draft"),
            QStringLiteral("paid"), QStringLiteral("void")};
}

QString filterLabel(const QString &filter)
{
    if (filter.isEmpty())
        return QStringLiteral("All invoices");
    if (filter == QLatin1String("open"))
        return QStringLiteral("Unpaid");
    if (filter == QLatin1String("draft"))
        return QStringLiteral("Drafts");
    return statusLabel(filter);
}

bool matchesFilter(const QJsonObject &invoice, const QString &filter)
{
    if (filter.isEmpty())
        return true;
    const QString key = statusKey(invoice);
    if (filter == QLatin1String("open"))
        return key == QLatin1String("sent") || key == QLatin1String("partial") ||
               key == QLatin1String("overdue");
    if (filter == QLatin1String("paid"))
        return key == filter || text(invoice, "status") == QLatin1String("paid");
    return key == filter;
}

bool matchesQuery(const QJsonObject &invoice, const QString &query)
{
    const QString needle = query.trimmed();
    if (needle.isEmpty())
        return true;
    const QJsonObject client = invoice.value(QStringLiteral("client")).toObject();
    for (const QString &value : {text(invoice, "number"), text(client, "name"), text(client, "company"),
                                 text(client, "email")}) {
        if (value.contains(needle, Qt::CaseInsensitive))
            return true;
    }
    return false;
}

QString clientLabel(const QJsonObject &invoice)
{
    const QJsonObject client = invoice.value(QStringLiteral("client")).toObject();
    for (const char *key : {"name", "company", "email"}) {
        if (!text(client, key).isEmpty())
            return text(client, key);
    }
    return QStringLiteral("No client");
}

bool canEdit(const QJsonObject &invoice)
{
    const QString status = text(invoice, "status");
    return status == QLatin1String("draft") || status == QLatin1String("sent");
}

bool canSend(const QJsonObject &invoice)
{
    return canEdit(invoice);
}

bool canDelete(const QJsonObject &invoice)
{
    const QString status = text(invoice, "status");
    return status == QLatin1String("draft") || status == QLatin1String("void");
}

bool canVoid(const QJsonObject &invoice)
{
    return !canDelete(invoice);
}

bool canReopen(const QJsonObject &invoice)
{
    const QString status = text(invoice, "status");
    return status == QLatin1String("paid") || status == QLatin1String("void");
}

bool canRecordPayment(const QJsonObject &invoice)
{
    return canEdit(invoice) && integer(invoice, "sentAt") > 0;
}

bool canMarkPaid(const QJsonObject &invoice)
{
    return text(invoice, "status") == QLatin1String("draft");
}

QString sendActionLabel(const QJsonObject &invoice)
{
    return integer(invoice, "sentAt") > 0 ? QStringLiteral("Send reminder") : QStringLiteral("Email invoice");
}

QString eventLabel(const QString &kind)
{
    static const QHash<QString, QString> labels{
        {QStringLiteral("created"), QStringLiteral("Created")},
        {QStringLiteral("sent"), QStringLiteral("Emailed")},
        {QStringLiteral("reminder"), QStringLiteral("Reminder emailed")},
        {QStringLiteral("failed"), QStringLiteral("Email failed")},
        {QStringLiteral("opened"), QStringLiteral("Email opened")},
        {QStringLiteral("viewed"), QStringLiteral("Invoice viewed")},
        {QStringLiteral("clicked"), QStringLiteral("Pay link clicked")},
        {QStringLiteral("paid"), QStringLiteral("Payment recorded")},
        {QStringLiteral("reopened"), QStringLiteral("Reopened")},
        {QStringLiteral("voided"), QStringLiteral("Voided")},
        {QStringLiteral("overdue"), QStringLiteral("Became overdue")},
    };
    return labels.value(kind, kind);
}

QString eventDetail(const QJsonObject &event, const QString &currency)
{
    QStringList parts;
    if (!text(event, "to").isEmpty())
        parts << QStringLiteral("to ") + text(event, "to");
    if (!text(event, "client").isEmpty())
        parts << QStringLiteral("in ") + text(event, "client");
    if (integer(event, "amount") > 0)
        parts << moneyText(integer(event, "amount"), currency);
    if (!text(event, "method").isEmpty())
        parts << text(event, "method");
    if (!text(event, "actor").isEmpty())
        parts << QStringLiteral("by ") + text(event, "actor");
    if (!text(event, "error").isEmpty())
        parts << text(event, "error");
    return parts.join(kDot);
}

QString activityLine(const QJsonObject &event)
{
    QStringList parts;
    const QString number = text(event, "number");
    parts << (number.isEmpty() ? eventLabel(text(event, "kind"))
                               : eventLabel(text(event, "kind")) + kDot + number);
    if (!text(event, "clientName").isEmpty())
        parts << text(event, "clientName");
    if (!text(event, "client").isEmpty())
        parts << text(event, "client");
    if (text(event, "kind") == QLatin1String("paid") && integer(event, "amount") > 0)
        parts << moneyText(integer(event, "amount"), text(event, "currency"));
    return parts.join(kDot);
}

QString dateText(qint64 stamp)
{
    if (stamp <= 0)
        return kDash;
    return QLocale(QLocale::English, QLocale::UnitedStates)
        .toString(utcDate(stamp), QStringLiteral("d MMM yyyy"));
}

QDate utcDate(qint64 stamp)
{
    return QDateTime::fromMSecsSinceEpoch(stamp, QTimeZone::utc()).date();
}

qint64 utcMidnight(const QDate &date)
{
    if (!date.isValid())
        return 0;
    return QDateTime(date, QTime(0, 0), QTimeZone::utc()).toMSecsSinceEpoch();
}

qint64 todayUtc(qint64 nowMs)
{
    return utcMidnight(utcDate(nowMs));
}

QString relativeText(qint64 nowMs, qint64 atMs)
{
    if (atMs <= 0)
        return QString();
    const qint64 seconds = std::max<qint64>(0, (nowMs - atMs) / 1000);
    if (seconds < 60)
        return QStringLiteral("just now");
    if (seconds < 3600)
        return QStringLiteral("%1 min ago").arg(seconds / 60);
    if (seconds < 86400)
        return QStringLiteral("%1 h ago").arg(seconds / 3600);
    if (seconds < 86400 * 7)
        return countPhrase(seconds / 86400, QStringLiteral("day")) + QStringLiteral(" ago");
    return dateText(atMs);
}

int overdueCount(const QJsonObject &overview)
{
    return int(integer(overview.value(QStringLiteral("summary")).toObject()
                           .value(QStringLiteral("counts")).toObject(), "overdue"));
}

QString openRateValue(const QJsonObject &summary)
{
    if (integer(summary, "sent") <= 0)
        return kDash;
    return QStringLiteral("%1%").arg(integer(summary, "openRate"));
}

QString openRateDetail(const QJsonObject &summary)
{
    const qint64 sent = integer(summary, "sent");
    if (sent <= 0)
        return QStringLiteral("No invoices sent yet");
    return QStringLiteral("%1 of %2 opened%3%4 viewed%3%5 clicked pay")
        .arg(integer(summary, "opened"))
        .arg(sent)
        .arg(kDot)
        .arg(integer(summary, "viewed"))
        .arg(integer(summary, "clicked"));
}

QString daysToPayValue(const QJsonObject &summary)
{
    const double days = summary.value(QStringLiteral("averageDaysToPay")).toDouble();
    if (days <= 0)
        return kDash;
    return QStringLiteral("%1 days").arg(QString::number(days, 'g', 4));
}

QString daysToPayDetail(const QJsonObject &summary)
{
    const double hours = summary.value(QStringLiteral("averageHoursToOpen")).toDouble();
    if (hours <= 0)
        return QStringLiteral("From sending to paid");
    return QStringLiteral("Opened %1 h after sending, on average").arg(QString::number(hours, 'g', 4));
}

QString countPhrase(qint64 count, const QString &one, const QString &many)
{
    return QStringLiteral("%1 %2").arg(count).arg(count == 1 ? one : (many.isEmpty() ? one + QLatin1Char('s') : many));
}

QString engagementText(const QJsonObject &invoice)
{
    if (integer(invoice, "sentAt") <= 0)
        return QStringLiteral("Not sent");
    return QStringLiteral("%1 opens%2%3 views%2%4 clicks")
        .arg(integer(invoice, "opens"))
        .arg(kDot)
        .arg(integer(invoice, "views"))
        .arg(integer(invoice, "clicks"));
}

QJsonObject duplicateSeed(const QJsonObject &invoice)
{
    QJsonObject seed;
    for (const char *key : {"client", "contactId", "currency", "items", "taxRate", "notes", "terms", "paymentUrl"}) {
        const QLatin1String name(key);
        if (invoice.contains(name))
            seed.insert(name, invoice.value(name));
    }
    return seed;
}

QString notInstalledError(const QJsonObject &payload)
{
    return text(payload, "error") == QLatin1String("plugin_not_installed") ? text(payload, "organization")
                                                                           : QString();
}

QString readableError(const QString &error)
{
    if (error == QLatin1String("invalid sender"))
        return QStringLiteral("there is no From email; add one in Invoicing settings or in the CRM's email settings");
    if (error == QLatin1String("invalid recipient"))
        return QStringLiteral("the recipient's email address is not valid");
    if (error == QLatin1String("invoice has no amount"))
        return QStringLiteral("the invoice has no line items with an amount");
    if (error == QLatin1String("invoice number taken"))
        return QStringLiteral("another invoice already has that number");
    if (error == QLatin1String("invalid payment url"))
        return QStringLiteral("the pay link must be an https address");
    if (error == QLatin1String("invoice locked"))
        return QStringLiteral("paid and void invoices cannot be changed; reopen it first");
    if (error == QLatin1String("void before delete"))
        return QStringLiteral("void the invoice before deleting it");
    if (error == QLatin1String("settings require admin"))
        return QStringLiteral("only an organization owner or admin can change settings");
    return error;
}

}
