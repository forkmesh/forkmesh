#include "InvoicesModel.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstdio>

using namespace forkmesh::invoices;

namespace {

int failures = 0;

void check(bool condition, const char *message, const QString &detail = QString())
{
    if (condition) {
        std::printf("PASS: %s\n", message);
    } else {
        std::printf("FAIL: %s%s%s\n", message, detail.isEmpty() ? "" : " -- ", detail.toUtf8().constData());
        ++failures;
    }
}

QJsonObject object(const char *json)
{
    return QJsonDocument::fromJson(QByteArray(json)).object();
}

}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QString dot = QString::fromUtf8(" \xC2\xB7 ");
    const QString dash = QString::fromUtf8("\xE2\x80\x94");

    check(moneyText(123456, QStringLiteral("USD")) == QStringLiteral("$1,234.56"), "dollars use a symbol and grouping",
          moneyText(123456, QStringLiteral("USD")));
    check(moneyText(5, QStringLiteral("EUR")) == QString::fromUtf8("\xE2\x82\xAC") + QStringLiteral("0.05"),
          "cents keep two decimals");
    check(moneyText(100000, QStringLiteral("SEK")) == QStringLiteral("SEK 1,000.00"),
          "an unknown currency is shown by its code", moneyText(100000, QStringLiteral("SEK")));
    check(moneyText(250, QStringLiteral("JPY")) == QString::fromUtf8("\xC2\xA5") + QStringLiteral("2.50"),
          "every currency shows two decimals, as the worker does");
    check(moneyMapText(object(R"({"USD": 1000, "EUR": 250, "GBP": 0})"), QStringLiteral("USD")) ==
              QString::fromUtf8("\xE2\x82\xAC") + QStringLiteral("2.50") + dot + QStringLiteral("$10.00"),
          "per-currency totals are listed in code order without zero entries");
    check(moneyMapText(QJsonObject(), QStringLiteral("GBP")) == QString::fromUtf8("\xC2\xA3") + QStringLiteral("0.00"),
          "no totals show zero in the default currency");

    check(parseCents(QStringLiteral("12.34")) == 1234, "a decimal price becomes cents");
    check(parseCents(QStringLiteral("$1,200")) == 120000, "symbols and separators are ignored");
    check(parseCents(QStringLiteral("abc")) == 0 && parseCents(QStringLiteral("-5")) == 0,
          "nonsense and negative amounts are zero");
    check(parseCents(QStringLiteral("0.105")) == 11 || parseCents(QStringLiteral("0.105")) == 10,
          "fractions of a cent round");
    check(centsInput(120050) == QStringLiteral("1200.50"), "cents fill an input with two decimals");
    check(basisPointsFromPercent(10) == 1000 && basisPointsFromPercent(7.25) == 725,
          "a tax percent becomes basis points");
    check(percentText(1000) == QStringLiteral("10%") && percentText(725) == QStringLiteral("7.25%"),
          "basis points read as a percent", percentText(725));

    const QList<InvoiceLine> lines{{QStringLiteral("Design"), 2, 15000}, {QStringLiteral("Hosting"), 1.5, 999},
                                   {QString(), 1, 0}};
    check(lineAmount(lines.at(1)) == 1499, "a line rounds quantity times price to the cent",
          QString::number(lineAmount(lines.at(1))));
    const InvoiceTotals sum = totals(lines, 1000);
    check(sum.subtotal == 31499 && sum.tax == 3150 && sum.total == 34649, "totals add tax in basis points",
          QStringLiteral("%1 %2 %3").arg(sum.subtotal).arg(sum.tax).arg(sum.total));
    const QJsonArray json = linesJson(lines);
    check(json.size() == 2, "empty lines are not sent");
    check(json.at(0).toObject().value(QStringLiteral("unitAmount")).toDouble() == 15000 &&
              json.at(1).toObject().value(QStringLiteral("quantity")).toDouble() == 1.5,
          "lines carry quantity and unit price in cents");
    const QList<InvoiceLine> parsed = invoiceLines(json);
    check(parsed.size() == 2 && parsed.at(0).description == QStringLiteral("Design") && parsed.at(1).unitAmount == 999,
          "worker items read back as lines");

    const QJsonObject draft = object(R"({"id": "a", "number": "INV-0001", "status": "draft", "displayStatus": "draft",
        "client": {"name": "Ada Lovelace", "company": "Engines Ltd", "email": "ada@example.com"}, "total": 1000})");
    const QJsonObject overdue = object(R"({"id": "b", "number": "INV-0002", "status": "sent", "displayStatus": "overdue",
        "sentAt": 1000, "client": {"company": "Babbage & Co"}, "total": 5000, "balance": 5000,
        "opens": 3, "views": 2, "clicks": 1})");
    const QJsonObject partial = object(R"({"id": "c", "number": "INV-0003", "status": "sent", "displayStatus": "partial",
        "sentAt": 1000, "client": {}, "total": 5000, "balance": 2000})");
    const QJsonObject paid = object(R"({"id": "d", "number": "INV-0004", "status": "paid", "displayStatus": "paid",
        "sentAt": 1000, "client": {"email": "pay@example.com"}})");
    const QJsonObject voided = object(R"({"id": "e", "number": "INV-0005", "status": "void", "client": {}})");

    check(statusKey(overdue) == QStringLiteral("overdue") && statusKey(voided) == QStringLiteral("void"),
          "the shown status prefers displayStatus");
    check(statusLabel(QStringLiteral("partial")) == QStringLiteral("Part paid"), "part paid has its own label");
    check(matchesFilter(overdue, QStringLiteral("open")) && matchesFilter(partial, QStringLiteral("open")) &&
              !matchesFilter(draft, QStringLiteral("open")) && !matchesFilter(paid, QStringLiteral("open")),
          "unpaid lists sent, part paid and overdue invoices");
    check(matchesFilter(overdue, QStringLiteral("overdue")) && !matchesFilter(partial, QStringLiteral("overdue")),
          "overdue lists only overdue invoices");
    check(matchesFilter(draft, QStringLiteral("draft")) && matchesFilter(paid, QStringLiteral("paid")) &&
              matchesFilter(voided, QStringLiteral("void")) && matchesFilter(voided, QString()),
          "drafts, paid, void and all filter by status");
    check(filterKeys().size() == 6 && filterLabel(QStringLiteral("open")) == QStringLiteral("Unpaid") &&
              filterLabel(QStringLiteral("draft")) == QStringLiteral("Drafts"),
          "the status filter matches the web page");
    check(matchesQuery(draft, QStringLiteral("inv-0001")) && matchesQuery(draft, QStringLiteral("lovelace")) &&
              matchesQuery(overdue, QStringLiteral("babbage")) && matchesQuery(paid, QStringLiteral("pay@")) &&
              !matchesQuery(draft, QStringLiteral("babbage")),
          "search matches number, client name, company and email");
    check(clientLabel(draft) == QStringLiteral("Ada Lovelace") && clientLabel(overdue) == QStringLiteral("Babbage & Co") &&
              clientLabel(paid) == QStringLiteral("pay@example.com") && clientLabel(partial) == QStringLiteral("No client"),
          "the client label falls back from name to company to email");

    check(canEdit(draft) && canEdit(overdue) && !canEdit(paid) && !canEdit(voided), "draft and sent invoices are editable");
    check(canDelete(draft) && canDelete(voided) && !canDelete(overdue) && !canDelete(paid),
          "only drafts and void invoices can be deleted");
    check(canVoid(overdue) && canVoid(paid) && !canVoid(draft), "sent and paid invoices can be voided");
    check(canReopen(paid) && canReopen(voided) && !canReopen(draft), "paid and void invoices can be reopened");
    check(canRecordPayment(overdue) && !canRecordPayment(draft) && !canRecordPayment(paid),
          "payments are recorded on sent invoices");
    check(canMarkPaid(draft) && !canMarkPaid(overdue), "a draft can be marked paid");
    check(sendActionLabel(draft) == QStringLiteral("Email invoice") &&
              sendActionLabel(overdue) == QStringLiteral("Send reminder"),
          "a sent invoice is reminded, a draft is emailed");
    check(engagementText(draft) == QStringLiteral("Not sent") &&
              engagementText(overdue) == QStringLiteral("3 opens") + dot + QStringLiteral("2 views") + dot +
                                             QStringLiteral("1 clicks"),
          "engagement counts opens, views and pay clicks", engagementText(overdue));

    check(eventLabel(QStringLiteral("clicked")) == QStringLiteral("Pay link clicked") &&
              eventLabel(QStringLiteral("unknown")) == QStringLiteral("unknown"),
          "event kinds have readable labels");
    const QJsonObject paidEvent = object(R"({"kind": "paid", "amount": 2500, "method": "Card", "actor": "jett",
        "number": "INV-0002", "clientName": "Babbage & Co", "currency": "USD"})");
    check(eventDetail(paidEvent, QStringLiteral("USD")) ==
              QStringLiteral("$25.00") + dot + QStringLiteral("Card") + dot + QStringLiteral("by jett"),
          "a payment event lists amount, method and who", eventDetail(paidEvent, QStringLiteral("USD")));
    check(activityLine(paidEvent) == QStringLiteral("Payment recorded") + dot + QStringLiteral("INV-0002") + dot +
                                         QStringLiteral("Babbage & Co") + dot + QStringLiteral("$25.00"),
          "an activity line names the invoice, client and amount", activityLine(paidEvent));

    const qint64 day = utcMidnight(QDate(2026, 10, 28));
    check(day == 1793145600000LL, "dates are stored as UTC midnight", QString::number(day));
    check(utcDate(day + 23 * 3600 * 1000LL) == QDate(2026, 10, 28), "a stamp reads back as its UTC day");
    check(dateText(day) == QStringLiteral("28 Oct 2026") && dateText(0) == dash, "dates read as day month year");
    check(todayUtc(day + 5000) == day, "today is the UTC midnight before now");
    check(relativeText(day + 30000, day) == QStringLiteral("just now") &&
              relativeText(day + 5 * 60000, day) == QStringLiteral("5 min ago") &&
              relativeText(day + 3 * 3600000LL, day) == QStringLiteral("3 h ago") &&
              relativeText(day + 2 * kDayMs, day) == QStringLiteral("2 days ago") &&
              relativeText(day + 30 * kDayMs, day) == QStringLiteral("28 Oct 2026"),
          "relative times step from minutes to dates");

    const QJsonObject overview = object(R"({"summary": {"counts": {"overdue": 2}, "sent": 4, "opened": 3, "viewed": 2,
        "clicked": 1, "openRate": 75, "averageDaysToPay": 6.5, "averageHoursToOpen": 2}})");
    const QJsonObject summary = overview.value(QStringLiteral("summary")).toObject();
    check(overdueCount(overview) == 2 && overdueCount(QJsonObject()) == 0, "the badge counts overdue invoices");
    check(openRateValue(summary) == QStringLiteral("75%") &&
              openRateDetail(summary) == QStringLiteral("3 of 4 opened") + dot + QStringLiteral("2 viewed") + dot +
                                             QStringLiteral("1 clicked pay"),
          "the open rate tile shows opened, viewed and clicked", openRateDetail(summary));
    check(openRateValue(QJsonObject()) == dash && openRateDetail(QJsonObject()) == QStringLiteral("No invoices sent yet"),
          "nothing sent shows a dash");
    check(daysToPayValue(summary) == QStringLiteral("6.5 days") &&
              daysToPayDetail(summary) == QStringLiteral("Opened 2 h after sending, on average"),
          "average days to pay and hours to open", daysToPayValue(summary));
    check(countPhrase(1, QStringLiteral("invoice")) == QStringLiteral("1 invoice") &&
              countPhrase(3, QStringLiteral("invoice")) == QStringLiteral("3 invoices"),
          "counts pluralize");

    const QJsonObject seed = duplicateSeed(object(R"({"id": "x", "number": "INV-9", "status": "paid", "sentAt": 5,
        "client": {"name": "Ada"}, "items": [{"description": "A", "quantity": 1, "unitAmount": 100}], "taxRate": 500,
        "currency": "EUR", "contactId": "c1", "notes": "n"})"));
    check(!seed.contains(QStringLiteral("id")) && !seed.contains(QStringLiteral("number")) &&
              !seed.contains(QStringLiteral("status")) && !seed.contains(QStringLiteral("sentAt")) &&
              !seed.contains(QStringLiteral("issuedAt")),
          "a duplicate drops identity, status and dates");
    check(seed.value(QStringLiteral("items")).toArray().size() == 1 &&
              seed.value(QStringLiteral("currency")).toString() == QStringLiteral("EUR") &&
              seed.value(QStringLiteral("contactId")).toString() == QStringLiteral("c1"),
          "a duplicate keeps the client, lines, currency and contact");

    check(notInstalledError(object(R"({"error": "plugin_not_installed", "organization": "acme", "canInstall": true})")) ==
              QStringLiteral("acme") &&
              notInstalledError(object(R"({"error": "not_found"})")).isEmpty(),
          "a 409 plugin_not_installed is recognised from its payload");
    check(readableError(QStringLiteral("invalid sender")).contains(QStringLiteral("From email")) &&
              readableError(QStringLiteral("something else")) == QStringLiteral("something else"),
          "worker errors read as sentences");

    std::printf("%s: %d failure(s)\n", failures ? "FAILED" : "OK", failures);
    return failures ? 1 : 0;
}
