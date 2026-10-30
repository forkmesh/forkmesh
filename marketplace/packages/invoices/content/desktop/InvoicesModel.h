#pragma once

#include <QDate>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>

namespace forkmesh::invoices {

inline constexpr qint64 kDayMs = 24LL * 60 * 60 * 1000;

struct InvoiceLine {
    QString description;
    double quantity = 1;
    qint64 unitAmount = 0;
};

struct InvoiceTotals {
    qint64 subtotal = 0;
    qint64 tax = 0;
    qint64 total = 0;
};

QString text(const QJsonObject &object, const char *key);
qint64 integer(const QJsonObject &object, const char *key);

QString currencySymbol(const QString &currency);
QString moneyText(qint64 cents, const QString &currency);
QString moneyMapText(const QJsonObject &amounts, const QString &fallbackCurrency);
qint64 parseCents(const QString &value);
QString centsInput(qint64 cents);
int basisPointsFromPercent(double percent);
QString percentText(int basisPoints);

qint64 lineAmount(const InvoiceLine &line);
InvoiceTotals totals(const QList<InvoiceLine> &lines, int taxRate);
QList<InvoiceLine> invoiceLines(const QJsonArray &items);
QJsonArray linesJson(const QList<InvoiceLine> &lines);

QString statusKey(const QJsonObject &invoice);
QString statusLabel(const QString &key);
QString filterLabel(const QString &filter);
QStringList filterKeys();
bool matchesFilter(const QJsonObject &invoice, const QString &filter);
bool matchesQuery(const QJsonObject &invoice, const QString &query);
QString clientLabel(const QJsonObject &invoice);

bool canEdit(const QJsonObject &invoice);
bool canSend(const QJsonObject &invoice);
bool canDelete(const QJsonObject &invoice);
bool canVoid(const QJsonObject &invoice);
bool canReopen(const QJsonObject &invoice);
bool canRecordPayment(const QJsonObject &invoice);
bool canMarkPaid(const QJsonObject &invoice);
QString sendActionLabel(const QJsonObject &invoice);

QString eventLabel(const QString &kind);
QString eventDetail(const QJsonObject &event, const QString &currency);
QString activityLine(const QJsonObject &event);

QString dateText(qint64 stamp);
QDate utcDate(qint64 stamp);
qint64 utcMidnight(const QDate &date);
qint64 todayUtc(qint64 nowMs);
QString relativeText(qint64 nowMs, qint64 atMs);

int overdueCount(const QJsonObject &overview);
QString openRateValue(const QJsonObject &summary);
QString openRateDetail(const QJsonObject &summary);
QString daysToPayValue(const QJsonObject &summary);
QString daysToPayDetail(const QJsonObject &summary);
QString countPhrase(qint64 count, const QString &one, const QString &many = QString());
QString engagementText(const QJsonObject &invoice);

QJsonObject duplicateSeed(const QJsonObject &invoice);
QString notInstalledError(const QJsonObject &payload);
QString readableError(const QString &error);

}
