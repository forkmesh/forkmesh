#include "MarketingCommon.h"

#include <QAbstractButton>
#include <QCollator>
#include <QComboBox>
#include <QDateTime>
#include <QHash>
#include <QHeaderView>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QLocale>
#include <QRegularExpression>
#include <QScrollBar>
#include <QTableWidget>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <optional>

namespace forkmesh::marketing {

namespace {

const char *const kSortColumn = "marketingSortColumn";
const char *const kSortOrder = "marketingSortOrder";
const char *const kSortPending = "marketingSortPending";
const char *const kSorting = "marketingSorting";

struct SortKey {
    bool empty = true;
    bool numeric = false;
    double number = 0;
    QString text;
};

QVariant widgetSortValue(QWidget *widget)
{
    if (!widget)
        return {};
    const QVariant key = widget->property("sortKey");
    if (key.isValid())
        return key;
    if (auto *edit = qobject_cast<QLineEdit *>(widget))
        return edit->text();
    if (auto *combo = qobject_cast<QComboBox *>(widget))
        return combo->currentText();
    if (auto *label = qobject_cast<QLabel *>(widget)) {
        static const QRegularExpression tags(QStringLiteral("<[^>]*>"));
        return QString(label->text()).remove(tags);
    }
    if (auto *button = qobject_cast<QAbstractButton *>(widget))
        return button->isCheckable() ? QVariant(button->isChecked() ? 1.0 : 0.0) : QVariant(button->text());
    for (QWidget *child : widget->findChildren<QWidget *>(Qt::FindDirectChildrenOnly)) {
        const QVariant value = widgetSortValue(child);
        if (value.isValid() && !value.toString().trimmed().isEmpty())
            return value;
    }
    return {};
}

std::optional<double> localNumber(QString core)
{
    bool ok = false;
    double value = QLocale().toDouble(core, &ok);
    if (!ok)
        value = QLocale::c().toDouble(core.remove(QLatin1Char(',')), &ok);
    return ok ? std::optional<double>(value) : std::nullopt;
}

SortKey sortKey(const QVariant &value)
{
    SortKey key;
    if (!value.isValid())
        return key;
    if (value.typeId() != QMetaType::QString) {
        bool ok = false;
        key.number = value.toDouble(&ok);
        key.numeric = ok;
        key.empty = false;
        key.text = value.toString();
        return key;
    }
    const QString text = value.toString().simplified();
    if (text.isEmpty() || text == kDash || text == QLatin1String("-") || text == QString(QChar(0x2013)) ||
        text == QString(QChar(0x2026)))
        return key;
    key.empty = false;
    key.text = text;
    const auto number = [&key](double result) {
        key.numeric = true;
        key.number = result;
        return key;
    };
    const double now = double(QDateTime::currentMSecsSinceEpoch());
    const QString lower = text.toLower();
    if (lower == QLatin1String("just now") || lower == QLatin1String("now") || lower == QLatin1String("today"))
        return number(now);
    if (lower == QLatin1String("yesterday"))
        return number(now - kDay);
    if (lower == QLatin1String("tomorrow"))
        return number(now + kDay);
    static const QHash<QString, double> units{
        {QStringLiteral("s"), 1e3}, {QStringLiteral("sec"), 1e3}, {QStringLiteral("second"), 1e3},
        {QStringLiteral("m"), 6e4}, {QStringLiteral("min"), 6e4}, {QStringLiteral("minute"), 6e4},
        {QStringLiteral("h"), 36e5}, {QStringLiteral("hr"), 36e5}, {QStringLiteral("hour"), 36e5},
        {QStringLiteral("d"), 864e5}, {QStringLiteral("day"), 864e5}, {QStringLiteral("w"), 6048e5},
        {QStringLiteral("wk"), 6048e5}, {QStringLiteral("week"), 6048e5}, {QStringLiteral("mo"), 2592e6},
        {QStringLiteral("month"), 2592e6}, {QStringLiteral("y"), 31536e6}, {QStringLiteral("yr"), 31536e6},
        {QStringLiteral("year"), 31536e6}};
    static const QRegularExpression relative(QStringLiteral("^(in )?(\\d+(?:\\.\\d+)?)\\s*([a-z]+?)s?( ago)?$"));
    QRegularExpressionMatch match = relative.match(lower);
    if (match.hasMatch() && units.contains(match.captured(3)) &&
        (!match.captured(1).isEmpty() || !match.captured(4).isEmpty()))
        return number(now + (match.captured(1).isEmpty() ? -1 : 1) * match.captured(2).toDouble() *
                                units.value(match.captured(3)));
    static const QRegularExpression duration(QStringLiteral("^(?:(\\d+)h\\s*)?(?:(\\d+)m\\s*)?(?:(\\d+)s)?$"));
    match = duration.match(text);
    if (match.hasMatch())
        return number((match.captured(1).toDouble() * 3600 + match.captured(2).toDouble() * 60 +
                       match.captured(3).toDouble()) * 1000);
    static const QRegularExpression isoDate(QStringLiteral("^\\d{4}-\\d{2}-\\d{2}"));
    if (isoDate.match(text).hasMatch()) {
        QDateTime at = QDateTime::fromString(text.left(16), QStringLiteral("yyyy-MM-dd HH:mm"));
        if (!at.isValid())
            at = QDate::fromString(text.left(10), Qt::ISODate).startOfDay();
        if (at.isValid())
            return number(double(at.toMSecsSinceEpoch()));
    }
    static const QStringList months{QStringLiteral("jan"), QStringLiteral("feb"), QStringLiteral("mar"),
                                    QStringLiteral("apr"), QStringLiteral("may"), QStringLiteral("jun"),
                                    QStringLiteral("jul"), QStringLiteral("aug"), QStringLiteral("sep"),
                                    QStringLiteral("oct"), QStringLiteral("nov"), QStringLiteral("dec")};
    static const QRegularExpression monthDate(QStringLiteral(
        "^(?:(?:mon|tue|wed|thu|fri|sat|sun)[a-z]*,?\\s+)?([a-z]{3})[a-z]*\\.?\\s+(\\d{1,2})\\b(?:,?\\s+(\\d{4}))?"));
    match = monthDate.match(lower);
    if (match.hasMatch() && months.contains(match.captured(1))) {
        const QDate date(match.captured(3).isEmpty() ? QDate::currentDate().year() : match.captured(3).toInt(),
                         int(months.indexOf(match.captured(1))) + 1, match.captured(2).toInt());
        if (date.isValid())
            return number(double(date.startOfDay().toMSecsSinceEpoch()));
    }
    static const QRegularExpression amount(QStringLiteral(
        "^([+-])?\\s*[$\\x{20AC}\\x{00A3}\\x{00A5}]?\\s*(\\d[\\d.,'\\x{00A0}\\x{202F}]*)\\s*([kKMB](?![A-Za-z]))?(.*)$"));
    static const QRegularExpression currencyCode(QStringLiteral("\\b[A-Z]{3}$"));
    static const QRegularExpression letter(QStringLiteral("\\p{L}"));
    match = amount.match(QString(text).replace(QChar(0x2212), QLatin1Char('-')));
    if (match.hasMatch() && !QString(match.captured(4)).remove(currencyCode).contains(letter)) {
        QString core = match.captured(2);
        while (!core.isEmpty() && !core.back().isDigit())
            core.chop(1);
        if (const std::optional<double> parsed = localNumber(core)) {
            const QString scale = match.captured(3);
            const double factor = scale == QLatin1String("B") ? 1e9 : scale == QLatin1String("M") ? 1e6
                                  : scale.isEmpty() ? 1 : 1e3;
            return number((match.captured(1) == QLatin1String("-") ? -1 : 1) * *parsed * factor);
        }
    }
    return key;
}

QVariant cellSortValue(QTableWidget *table, int row, int column)
{
    if (QTableWidgetItem *item = table->item(row, column)) {
        const QVariant key = item->data(kSortKeyRole);
        if (key.isValid())
            return key;
        if (!item->text().trimmed().isEmpty() || !table->cellWidget(row, column))
            return item->text();
    }
    return widgetSortValue(table->cellWidget(row, column));
}

bool pinnedRow(QTableWidget *table, int row)
{
    for (int column = 0; column < table->columnCount(); ++column) {
        if (QTableWidgetItem *item = table->item(row, column); item && item->data(kPinnedRowRole).toBool())
            return true;
    }
    return false;
}

void applySort(QTableWidget *table)
{
    const QVariant stored = table->property(kSortColumn);
    const int column = stored.isValid() ? stored.toInt() : -1;
    const int rows = table->rowCount();
    if (column < 0 || column >= table->columnCount() || rows < 2)
        return;
    const bool descending = table->property(kSortOrder).toInt() == Qt::DescendingOrder;
    struct Entry {
        int row;
        bool pinned;
        SortKey key;
    };
    QList<Entry> entries;
    entries.reserve(rows);
    for (int row = 0; row < rows; ++row)
        entries.append({row, pinnedRow(table, row), sortKey(cellSortValue(table, row, column))});
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::stable_sort(entries.begin(), entries.end(), [&](const Entry &a, const Entry &b) {
        if (a.pinned != b.pinned)
            return b.pinned;
        if (a.key.empty != b.key.empty)
            return b.key.empty;
        if (a.key.empty)
            return false;
        int order = 0;
        if (a.key.numeric && b.key.numeric)
            order = a.key.number < b.key.number ? -1 : a.key.number > b.key.number ? 1 : 0;
        else if (a.key.numeric != b.key.numeric)
            order = a.key.numeric ? -1 : 1;
        else
            order = collator.compare(a.key.text, b.key.text);
        return descending ? order > 0 : order < 0;
    });
    bool moved = false;
    for (int index = 0; index < entries.size() && !moved; ++index)
        moved = entries.at(index).row != index;
    if (!moved)
        return;
    table->setProperty(kSorting, true);
    const int keyColumn = table->columnCount();
    table->insertColumn(keyColumn);
    for (int rank = 0; rank < entries.size(); ++rank) {
        auto *key = new QTableWidgetItem;
        key->setData(Qt::DisplayRole, rank);
        table->setItem(entries.at(rank).row, keyColumn, key);
    }
    table->sortItems(keyColumn, Qt::AscendingOrder);
    table->removeColumn(keyColumn);
    table->setProperty(kSorting, false);
}

void scheduleSort(QTableWidget *table)
{
    if (!table->property(kSortColumn).isValid() || table->property(kSorting).toBool() ||
        table->property(kSortPending).toBool())
        return;
    table->setProperty(kSortPending, true);
    QTimer::singleShot(0, table, [table] {
        table->setProperty(kSortPending, false);
        applySort(table);
    });
}

} // namespace

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
    sortableTable(table);
}

void sortableTable(QTableWidget *table)
{
    QHeaderView *header = table->horizontalHeader();
    header->setSectionsClickable(true);
    QObject::connect(header, &QHeaderView::sectionClicked, table, [table](int column) {
        QHeaderView *header = table->horizontalHeader();
        const QVariant current = table->property(kSortColumn);
        const auto order = Qt::SortOrder(table->property(kSortOrder).toInt());
        const QTableWidgetItem *label = table->horizontalHeaderItem(column);
        if (!label || label->text().trimmed().isEmpty()) {
            if (current.isValid())
                header->setSortIndicator(current.toInt(), order);
            return;
        }
        const Qt::SortOrder next = current.isValid() && current.toInt() == column && order == Qt::AscendingOrder
                                       ? Qt::DescendingOrder : Qt::AscendingOrder;
        table->setProperty(kSortColumn, column);
        table->setProperty(kSortOrder, int(next));
        header->setSortIndicatorShown(true);
        header->setSortIndicator(column, next);
        applySort(table);
    });
    QObject::connect(table->model(), &QAbstractItemModel::rowsInserted, table, [table] { scheduleSort(table); });
    QObject::connect(table->model(), &QAbstractItemModel::dataChanged, table, [table] { scheduleSort(table); });
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
