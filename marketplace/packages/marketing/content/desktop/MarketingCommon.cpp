#include "MarketingCommon.h"

#include <QDateTime>
#include <QHeaderView>
#include <QLayout>
#include <QLineEdit>
#include <QLocale>
#include <QScrollBar>
#include <QTableWidget>
#include <QWidget>

#include <cmath>

namespace forkmesh::marketing {

QString text(const QJsonObject &object, const char *key)
{
    return object.value(QLatin1String(key)).toString().trimmed();
}

QString timestamp(qint64 value)
{
    return value > 0 ? QDateTime::fromMSecsSinceEpoch(value).toLocalTime()
                           .toString(QStringLiteral("yyyy-MM-dd HH:mm"))
                     : QString::fromUtf8("\xE2\x80\x94");
}

QLineEdit *lineEdit(const QString &value)
{
    auto *edit = new QLineEdit(value);
    edit->setClearButtonEnabled(true);
    return edit;
}

QString channelName(const QString &channel)
{
    if (channel == QLatin1String("x"))
        return QStringLiteral("X");
    QString result = channel;
    if (!result.isEmpty())
        result[0] = result.at(0).toUpper();
    return result;
}

QString channelName(const QString &channel, const QJsonObject &settings,
                    const QJsonArray &catalog)
{
    for (const QJsonValue &value :
         settings.value(QStringLiteral("channels")).toArray()) {
        const QJsonObject item = value.toObject();
        if (text(item, "id") == channel && !text(item, "displayName").isEmpty())
            return text(item, "displayName");
    }
    for (const QJsonValue &value : catalog) {
        const QJsonObject item = value.toObject();
        if (text(item, "id") == channel)
            return text(item, "name");
    }
    return channelName(channel);
}

// Ad money arrives as whole hundredths of the ad's currency unit.
QString money(const QJsonValue &cents, const QString &currency)
{
    return QLocale().toString(cents.toDouble() / 100.0, 'f', 2) +
           QLatin1Char(' ') + currency;
}

QString count(const QJsonValue &value)
{
    return QLocale().toString(qint64(value.toDouble()));
}

QString healthLabel(const QString &state)
{
    if (state == QLatin1String("good"))
        return QStringLiteral("On track");
    if (state == QLatin1String("attention"))
        return QStringLiteral("Needs attention");
    if (state == QLatin1String("stale"))
        return QStringLiteral("Stale numbers");
    if (state == QLatin1String("setup"))
        return QStringLiteral("Not set up");
    if (state == QLatin1String("paused"))
        return QStringLiteral("Paused");
    return QString::fromUtf8("\xE2\x80\x94");
}

QString compactCount(qint64 value)
{
    const auto shortened = [](double scaled, const char *suffix) {
        QString number = QString::number(scaled, 'f', scaled < 100 ? 1 : 0);
        if (number.endsWith(QLatin1String(".0")))
            number.chop(2);
        return number + QLatin1String(suffix);
    };
    if (value >= 1000000)
        return shortened(value / 1000000.0, "M");
    if (value >= 1000)
        return shortened(value / 1000.0, "k");
    return QString::number(value);
}

QString choiceLabel(const Choices &choices, const QString &value)
{
    for (const auto &choice : choices) {
        if (choice.first == value)
            return choice.second;
    }
    return value;
}

double number(const QJsonObject &object, const char *key)
{
    return object.value(QLatin1String(key)).toDouble();
}

bool missing(const QJsonObject &object, const char *key)
{
    const QJsonValue value = object.value(QLatin1String(key));
    return value.isNull() || value.isUndefined();
}

QString fullCount(double value)
{
    return QLocale().toString(qint64(std::llround(value)));
}

QString shareText(const QJsonValue &value)
{
    return value.isDouble() ? QString::number(value.toDouble() * 100, 'f', 1) + QLatin1Char('%')
                            : kDash;
}

QString shortDate(qint64 at)
{
    return QDateTime::fromMSecsSinceEpoch(at).toString(QStringLiteral("MMM d"));
}

QString relativeLabel(qint64 at)
{
    const qint64 diff = at - QDateTime::currentMSecsSinceEpoch();
    const qint64 minutes = std::llround(std::abs(double(diff)) / 60000.0);
    const qint64 hours = std::llround(minutes / 60.0);
    const qint64 days = std::llround(hours / 24.0);
    const auto plural = [](qint64 count, const char *unit) {
        return QStringLiteral("%1 %2%3").arg(count).arg(QLatin1String(unit))
            .arg(count == 1 ? QString() : QStringLiteral("s"));
    };
    if (diff > 0) {
        if (minutes < 60)
            return QStringLiteral("in %1 min").arg(qMax<qint64>(1, minutes));
        return QStringLiteral("in ") + (hours < 48 ? plural(hours, "hour") : plural(days, "day"));
    }
    if (minutes < 1)
        return QStringLiteral("just now");
    if (minutes < 60)
        return QStringLiteral("%1 min ago").arg(minutes);
    return (hours < 48 ? plural(hours, "hour") : plural(days, "day")) + QStringLiteral(" ago");
}

QString escaped(const QString &value)
{
    return value.toHtmlEscaped();
}

QTableWidgetItem *numberItem(const QString &value)
{
    auto *item = new QTableWidgetItem(value);
    item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    return item;
}

void clearLayout(QLayout *layout)
{
    while (QLayoutItem *item = layout->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        } else if (QLayout *child = item->layout()) {
            clearLayout(child);
        }
        delete item;
    }
}

void dashboardTable(QTableWidget *table)
{
    table->verticalHeader()->hide();
    table->verticalHeader()->setMinimumSectionSize(20);
    table->verticalHeader()->setDefaultSectionSize(24);
    table->setShowGrid(false);
    table->setFrameShape(QFrame::NoFrame);
    table->setWordWrap(false);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    table->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    table->horizontalHeader()->setHighlightSections(false);
    table->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    table->horizontalHeader()->setMinimumSectionSize(48);
    table->setProperty("dataTable", true);
}

void fitTable(QTableWidget *table, int maxRows)
{
    const int rows = qMin(table->rowCount(), maxRows);
    int height = table->horizontalHeader()->isHidden() ? 0 : table->horizontalHeader()->sizeHint().height();
    for (int row = 0; row < rows; ++row)
        height += table->rowHeight(row);
    if (rows == 0)
        height += table->verticalHeader()->defaultSectionSize();
    if (table->horizontalScrollBarPolicy() != Qt::ScrollBarAlwaysOff &&
        table->horizontalHeader()->length() > table->viewport()->width() && table->viewport()->width() > 0)
        height += table->horizontalScrollBar()->sizeHint().height();
    table->setFixedHeight(height + 2 * table->frameWidth() + 2);
}

} // namespace forkmesh::marketing
