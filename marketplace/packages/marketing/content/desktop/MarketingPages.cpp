#include "MarketingFeature.h"
#include "MarketingCommon.h"
#include "MarketingDashboard.h"

#include <QAbstractButton>
#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCompleter>
#include <QDateEdit>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QDoubleValidator>
#include <QFile>
#include <QFileDialog>
#include <QFont>
#include <QFontDatabase>
#include <QFormLayout>
#include <QFrame>
#include <QInputDialog>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QJsonDocument>
#include <QImage>
#include <QIntValidator>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QLocale>
#include <QMap>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>
#include <QTimeZone>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <memory>
#include <tuple>

namespace forkmesh::marketing {
namespace {

const Choices &adStatuses()
{
    static const Choices choices{
        {QStringLiteral("draft"), QStringLiteral("Draft")},
        {QStringLiteral("review"), QStringLiteral("Review")},
        {QStringLiteral("scheduled"), QStringLiteral("Scheduled")},
        {QStringLiteral("active"), QStringLiteral("Active")},
        {QStringLiteral("paused"), QStringLiteral("Paused")},
        {QStringLiteral("completed"), QStringLiteral("Completed")},
        {QStringLiteral("archived"), QStringLiteral("Archived")}};
    return choices;
}

const Choices &experimentStatuses()
{
    static const Choices choices{
        {QStringLiteral("idea"), QStringLiteral("Idea")},
        {QStringLiteral("planned"), QStringLiteral("Planned")},
        {QStringLiteral("running"), QStringLiteral("Running")},
        {QStringLiteral("review"), QStringLiteral("In review")},
        {QStringLiteral("done"), QStringLiteral("Done")},
        {QStringLiteral("dropped"), QStringLiteral("Dropped")}};
    return choices;
}

const Choices &experimentStages()
{
    static const Choices choices{
        {QStringLiteral("acquisition"), QStringLiteral("Acquisition")},
        {QStringLiteral("activation"), QStringLiteral("Activation")},
        {QStringLiteral("retention"), QStringLiteral("Retention")},
        {QStringLiteral("referral"), QStringLiteral("Referral")},
        {QStringLiteral("revenue"), QStringLiteral("Revenue")},
        {QStringLiteral("measurement"), QStringLiteral("Measurement")}};
    return choices;
}

const Choices &experimentDecisions()
{
    static const Choices choices{
        {QString(), QStringLiteral("Not decided")},
        {QStringLiteral("keep"), QStringLiteral("Keep")},
        {QStringLiteral("iterate"), QStringLiteral("Iterate")},
        {QStringLiteral("revert"), QStringLiteral("Revert")},
        {QStringLiteral("extend"), QStringLiteral("Extend the window")}};
    return choices;
}

const Choices &redditStatuses()
{
    static const Choices choices{
        {QStringLiteral("to_review"), QStringLiteral("To review")},
        {QStringLiteral("drafting"), QStringLiteral("Drafting")},
        {QStringLiteral("replied"), QStringLiteral("Replied")},
        {QStringLiteral("skipped"), QStringLiteral("Skipped")}};
    return choices;
}

const Choices &healthFilters()
{
    static const Choices choices{
        {QString(), QStringLiteral("All")},
        {QStringLiteral("attention"), QStringLiteral("Needs attention")},
        {QStringLiteral("stale"), QStringLiteral("Stale")},
        {QStringLiteral("setup"), QStringLiteral("Not set up")},
        {QStringLiteral("good"), QStringLiteral("On track")},
        {QStringLiteral("paused"), QStringLiteral("Paused")}};
    return choices;
}

QStringList choiceLabels(const Choices &choices)
{
    QStringList labels;
    for (const auto &choice : choices)
        labels << choice.second;
    return labels;
}

int choiceIndex(const Choices &choices, const QString &value)
{
    for (int index = 0; index < choices.size(); ++index) {
        if (choices.at(index).first == value)
            return index;
    }
    return -1;
}

QComboBox *choiceBox(const Choices &choices, const QString &current)
{
    auto *combo = new QComboBox;
    for (const auto &choice : choices)
        combo->addItem(choice.second, choice.first);
    combo->setCurrentIndex(qMax(0, combo->findData(current)));
    return combo;
}

QLabel *hintLabel(const QString &value = QString(), const char *name = "marketingHint")
{
    auto *label = new QLabel(value);
    label->setObjectName(QLatin1String(name));
    label->setWordWrap(true);
    return label;
}

QLabel *richLabel(const char *name = "marketingNote")
{
    auto *label = hintLabel(QString(), name);
    label->setTextFormat(Qt::RichText);
    return label;
}

QTableWidget *pageTable(const char *name, const QStringList &headers, int stretch)
{
    auto *table = new QTableWidget(0, int(headers.size()));
    table->setObjectName(QLatin1String(name));
    table->setHorizontalHeaderLabels(headers);
    dashboardTable(table);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    if (stretch >= 0)
        table->horizontalHeader()->setSectionResizeMode(stretch, QHeaderView::Stretch);
    return table;
}

QWidget *page(QScrollArea *&scroll, QVBoxLayout *&layout, const char *name)
{
    scroll = new QScrollArea;
    scroll->setObjectName(QLatin1String(name));
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *content = new QWidget;
    content->setObjectName(QStringLiteral("marketingDashboardPage"));
    layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 4, 6, 18);
    layout->setSpacing(14);
    scroll->setWidget(content);
    return content;
}

void setSegmentLabels(QButtonGroup *group, const QStringList &labels)
{
    for (int index = 0; index < labels.size(); ++index) {
        if (QAbstractButton *button = group->button(index))
            button->setText(labels.at(index));
    }
}

void addRow(QTableWidget *table, const QStringList &values, const QString &id,
            int firstNumber, const QStringList &tips = {})
{
    const int row = table->rowCount();
    table->insertRow(row);
    for (int column = 0; column < values.size(); ++column) {
        auto *cell = column >= firstNumber && firstNumber >= 0 ? numberItem(values.at(column))
                                                                : new QTableWidgetItem(values.at(column));
        if (column < tips.size() && !tips.at(column).isEmpty())
            cell->setToolTip(tips.at(column));
        if (column == 0)
            cell->setData(Qt::UserRole, id);
        table->setItem(row, column, cell);
    }
}

QString rowId(QTableWidget *table)
{
    const int row = table->currentRow();
    QTableWidgetItem *cell = row >= 0 ? table->item(row, 0) : nullptr;
    return cell ? cell->data(Qt::UserRole).toString() : QString();
}

QJsonObject byId(const QJsonArray &items, const QString &id)
{
    for (const QJsonValue &value : items) {
        if (!id.isEmpty() && text(value.toObject(), "id") == id)
            return value.toObject();
    }
    return {};
}

bool present(const QJsonValue &value)
{
    return value.isDouble();
}

QString countText(const QJsonValue &value)
{
    return present(value) ? fullCount(value.toDouble()) : kDash;
}

QString rateText(const QJsonValue &value)
{
    if (!present(value))
        return kDash;
    QString number = QLocale().toString(value.toDouble(), 'f', 1);
    if (number.endsWith(QLocale().decimalPoint() + QStringLiteral("0")))
        number.chop(2);
    return number + QLatin1Char('%');
}

QString plural(qint64 amount, const QString &one, const QString &many)
{
    return QStringLiteral("%1 %2").arg(fullCount(double(amount)), amount == 1 ? one : many);
}

QString redditViewsChange(const QJsonObject &item)
{
    QList<QJsonObject> points;
    for (const QJsonValue &value : item.value(QStringLiteral("history")).toArray()) {
        if (present(value.toObject().value(QStringLiteral("views"))))
            points << value.toObject();
    }
    if (points.size() < 2)
        return {};
    const QJsonObject latest = points.last();
    const double dayBefore = number(latest, "at") - 24.0 * 3600 * 1000;
    QJsonObject base = points.first();
    bool day = false;
    for (auto it = points.crbegin(); it != points.crend(); ++it) {
        if (number(*it, "at") <= dayBefore) {
            base = *it;
            day = true;
            break;
        }
    }
    const qint64 change = qint64(number(latest, "views") - number(base, "views"));
    return QStringLiteral("%1%2 %3").arg(change >= 0 ? QStringLiteral("+") : QStringLiteral("−"),
                                         QLocale().toString(qAbs(change)),
                                         day ? QStringLiteral("in the last day") : QStringLiteral("since the first read"));
}

QStringList redditCommentActivity(const QJsonObject &item)
{
    QStringList parts;
    const bool comment = !text(item, "monitorCommentUrl").isEmpty();
    if (comment && (present(item.value(QStringLiteral("commentViews"))) || present(item.value(QStringLiteral("commentScore"))))) {
        QStringList numbers;
        if (present(item.value(QStringLiteral("commentViews"))))
            numbers << plural(qint64(number(item, "commentViews")), QStringLiteral("view"), QStringLiteral("views"));
        if (present(item.value(QStringLiteral("commentScore"))))
            numbers << plural(qint64(number(item, "commentScore")), QStringLiteral("point"), QStringLiteral("points"));
        parts << (text(item, "commentAuthor").isEmpty() ? QStringLiteral("your comment: ")
                                                        : QStringLiteral("u/%1's comment: ").arg(text(item, "commentAuthor")))
                     + numbers.join(QStringLiteral(", "));
    }
    const QString owner = comment ? text(item, "commentAuthor") : text(item, "author");
    const bool shown = present(item.value(comment ? QStringLiteral("commentViews") : QStringLiteral("views")));
    if (number(item, "checkedAt") > 0 && !shown && !owner.isEmpty())
        parts << QStringLiteral("views show only to u/%1").arg(owner);
    return parts;
}

QString redditMonitorSummary(const QJsonObject &item)
{
    if (number(item, "checkedAt") <= 0)
        return QStringLiteral("Not read yet: the ForkMesh browser extension reads it when the browser starts and every hour, or press Update.");
    QStringList parts;
    if (present(item.value(QStringLiteral("views")))) {
        const QString change = redditViewsChange(item);
        parts << plural(qint64(number(item, "views")), QStringLiteral("view"), QStringLiteral("views"))
                     + (change.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(change));
    } else {
        parts << QStringLiteral("views not shown to the account that read it");
    }
    if (present(item.value(QStringLiteral("score"))))
        parts << plural(qint64(number(item, "score")), QStringLiteral("point"), QStringLiteral("points"));
    if (present(item.value(QStringLiteral("comments"))))
        parts << plural(qint64(number(item, "comments")), QStringLiteral("comment"), QStringLiteral("comments"));
    if (!text(item, "monitorCommentUrl").isEmpty()) {
        QStringList comment;
        if (present(item.value(QStringLiteral("commentScore"))))
            comment << plural(qint64(number(item, "commentScore")), QStringLiteral("point"), QStringLiteral("points"));
        if (present(item.value(QStringLiteral("commentViews"))))
            comment << plural(qint64(number(item, "commentViews")), QStringLiteral("view"), QStringLiteral("views"));
        parts << QStringLiteral("comment: ") + (comment.isEmpty() ? QStringLiteral("not found on the page") : comment.join(QStringLiteral(", ")));
    }
    parts << QStringLiteral("read ") + relativeLabel(qint64(number(item, "checkedAt")));
    return parts.join(QStringLiteral(" · "));
}

QPair<QString, int> changeDelta(const QJsonValue &change, const QString &unit)
{
    if (!present(change))
        return {kDash, 0};
    const double value = change.toDouble();
    QString amount = QLocale().toString(std::abs(value), 'f', 2);
    while (amount.contains(QLocale().decimalPoint()) && (amount.endsWith(QLatin1Char('0'))))
        amount.chop(1);
    if (amount.endsWith(QLocale().decimalPoint()))
        amount.chop(1);
    const QString arrow = value > 0 ? QStringLiteral("▲ ") : value < 0 ? QStringLiteral("▼ ") : QString();
    return {arrow + amount + unit, value > 0 ? 1 : value < 0 ? -1 : 0};
}

QPair<QString, int> windowDelta(const QJsonValue &current, const QJsonValue &previous, bool lowerIsBetter = false)
{
    if (!present(current) || !present(previous))
        return {kDash, 0};
    const double now = current.toDouble();
    const double before = previous.toDouble();
    if (before == 0)
        return {now != 0 ? QStringLiteral("New") : kDash, 0};
    const double change = (now - before) / before * 100;
    const bool good = lowerIsBetter ? change < -0.05 : change > 0.05;
    const bool bad = lowerIsBetter ? change > 0.05 : change < -0.05;
    const QString arrow = change > 0.05 ? QStringLiteral("▲") : change < -0.05 ? QStringLiteral("▼") : QStringLiteral("•");
    return {QStringLiteral("%1 %2%").arg(arrow, QLocale().toString(std::abs(change), 'f', 1)),
            good ? 1 : bad ? -1 : 0};
}

QString compactMoney(double cents, const QString &currency)
{
    return compactCount(qint64(std::llround(cents / 100.0))) + QLatin1Char(' ') + currency;
}

qint64 weekStartOf(qint64 at)
{
    const QDate day = QDateTime::fromMSecsSinceEpoch(at, QTimeZone::UTC).date();
    return QDateTime(day.addDays(1 - day.dayOfWeek()), QTime(0, 0), QTimeZone::UTC).toMSecsSinceEpoch();
}

QString weekLabel(qint64 at)
{
    return QDateTime::fromMSecsSinceEpoch(at, QTimeZone::UTC).toString(QStringLiteral("MMM d, yyyy"));
}

QString utcDate(qint64 at)
{
    return QDateTime::fromMSecsSinceEpoch(at, QTimeZone::UTC).toString(QStringLiteral("yyyy-MM-dd"));
}

qint64 utcMidnight(const QDate &date)
{
    return QDateTime(date, QTime(0, 0), QTimeZone::UTC).toMSecsSinceEpoch();
}

QString icon(const QString &level, const DashboardColors &colors)
{
    const QColor color = level == QLatin1String("win") || level == QLatin1String("ok") ? colors.good
        : level == QLatin1String("warning") || level == QLatin1String("warn") ? colors.warn
        : level == QLatin1String("error") || level == QLatin1String("bad") ? colors.bad
        : level == QLatin1String("action") || level == QLatin1String("setup") ? colors.primary
        : colors.muted;
    const QString glyph = level == QLatin1String("win") || level == QLatin1String("ok") ? QStringLiteral("✓")
        : level == QLatin1String("setup") ? QStringLiteral("+")
        : level == QLatin1String("action") ? QStringLiteral("›")
        : level == QLatin1String("info") ? QStringLiteral("i")
        : QStringLiteral("⚠");
    return QStringLiteral("<span style=\"color:%1; font-weight:600\">%2</span>").arg(color.name(), glyph);
}

QString checkList(const QJsonArray &checks, const DashboardColors &colors, const QString &allClear)
{
    QStringList lines;
    for (const QJsonValue &value : checks) {
        const QJsonObject check = value.toObject();
        lines << icon(text(check, "level").isEmpty() ? QStringLiteral("warning") : text(check, "level"), colors) +
                     QStringLiteral("&nbsp; ") + escaped(text(check, "message"));
    }
    if (lines.isEmpty() && !allClear.isEmpty())
        lines << icon(QStringLiteral("ok"), colors) + QStringLiteral("&nbsp; ") + escaped(allClear);
    return lines.join(QStringLiteral("<br>"));
}

QLineEdit *optionalNumber(const QJsonValue &value, bool decimals = false)
{
    auto *edit = lineEdit(present(value) ? (decimals ? QString::number(value.toDouble())
                                                     : QString::number(qint64(value.toDouble())))
                                         : QString());
    edit->setPlaceholderText(kDash);
    if (decimals)
        edit->setValidator(new QDoubleValidator(0, 1e12, 2, edit));
    else
        edit->setValidator(new QIntValidator(0, 2000000000, edit));
    return edit;
}

QJsonValue optionalValue(const QLineEdit *edit)
{
    const QString value = edit->text().trimmed();
    if (value.isEmpty())
        return QJsonValue::Null;
    bool ok = false;
    const double number = QLocale().toDouble(value, &ok);
    return ok ? QJsonValue(number) : QJsonValue(value.toDouble());
}

struct OptionalDate {
    QHBoxLayout *row = nullptr;
    QCheckBox *set = nullptr;
    QDateEdit *edit = nullptr;
    double value() const
    {
        return set->isChecked() ? double(QDateTime(edit->date(), QTime(0, 0)).toMSecsSinceEpoch()) : 0.0;
    }
};

OptionalDate optionalDate(const QJsonObject &item, const char *key)
{
    const qint64 at = qint64(number(item, key));
    OptionalDate date;
    date.row = new QHBoxLayout;
    date.set = new QCheckBox(QStringLiteral("Set"));
    date.set->setChecked(at > 0);
    date.edit = new QDateEdit(at > 0 ? QDateTime::fromMSecsSinceEpoch(at).date() : QDate::currentDate());
    date.edit->setCalendarPopup(true);
    date.edit->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    date.edit->setEnabled(at > 0);
    QObject::connect(date.set, &QCheckBox::toggled, date.edit, &QWidget::setEnabled);
    date.row->addWidget(date.set);
    date.row->addWidget(date.edit, 1);
    return date;
}

QDialogButtonBox *dialogButtons(QDialog *dialog, const QString &save, bool withDelete)
{
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    auto *accept = buttons->addButton(save, QDialogButtonBox::AcceptRole);
    accept->setObjectName(QStringLiteral("primaryButton"));
    accept->setDefault(true);
    if (withDelete) {
        auto *remove = buttons->addButton(QStringLiteral("Delete"), QDialogButtonBox::DestructiveRole);
        remove->setObjectName(QStringLiteral("dangerButton"));
        QObject::connect(remove, &QPushButton::clicked, dialog, [dialog] { dialog->done(2); });
    }
    QObject::connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    return buttons;
}

QLabel *dialogIntro(const QString &value)
{
    auto *label = new QLabel(value);
    label->setObjectName(QStringLiteral("mutedLabel"));
    label->setWordWrap(true);
    return label;
}

QStringList csvCells(const QString &line, QChar separator)
{
    QStringList cells;
    QString current;
    bool quoted = false;
    for (int index = 0; index < line.size(); ++index) {
        const QChar character = line.at(index);
        if (character == QLatin1Char('"')) {
            if (quoted && index + 1 < line.size() && line.at(index + 1) == QLatin1Char('"')) {
                current += QLatin1Char('"');
                ++index;
            } else {
                quoted = !quoted;
            }
        } else if (character == separator && !quoted) {
            cells << current.trimmed();
            current.clear();
        } else {
            current += character;
        }
    }
    cells << current.trimmed();
    return cells;
}

QJsonValue importNumber(const QString &value)
{
    static const QRegularExpression spaces(QStringLiteral("[%\\s]"));
    static const QRegularExpression thousands(QStringLiteral(",(?=\\d{3}\\b)"));
    QString clean = value;
    clean.remove(spaces);
    clean.remove(thousands);
    if (clean.isEmpty())
        return QJsonValue::Null;
    bool ok = false;
    const double number = QString(clean).replace(QLatin1Char(','), QLatin1Char('.')).toDouble(&ok);
    return ok ? QJsonValue(number) : QJsonValue::Null;
}

QList<QJsonObject> parseKeywordImport(const QString &source)
{
    QStringList lines;
    for (const QString &line : source.split(QRegularExpression(QStringLiteral("\\r?\\n")))) {
        if (!line.trimmed().isEmpty())
            lines << line.trimmed();
    }
    if (lines.isEmpty())
        return {};
    const QString &first = lines.first();
    const QChar separator = first.contains(QLatin1Char('\t')) ? QLatin1Char('\t')
        : first.contains(QLatin1Char(';')) && !first.contains(QLatin1Char(',')) ? QLatin1Char(';')
        : QLatin1Char(',');
    QStringList head = csvCells(first, separator);
    for (QString &cell : head)
        cell = cell.toLower();
    const auto find = [&head](const QStringList &names) {
        for (int index = 0; index < head.size(); ++index) {
            for (const QString &name : names) {
                if (head.at(index).contains(name))
                    return index;
            }
        }
        return -1;
    };
    const int term = find({QStringLiteral("quer"), QStringLiteral("keyword"), QStringLiteral("search term"),
                           QStringLiteral("term")});
    const int clicks = find({QStringLiteral("click")});
    const int impressions = find({QStringLiteral("impression")});
    const int position = find({QStringLiteral("position"), QStringLiteral("rank")});
    const bool header = term >= 0 && (clicks >= 0 || impressions >= 0 || position >= 0 || head.size() == 1);
    QList<QJsonObject> rows;
    for (int index = header ? 1 : 0; index < lines.size(); ++index) {
        QJsonObject row;
        if (!header) {
            QString value = lines.at(index).section(separator, 0, 0);
            if (value.startsWith(QLatin1Char('"')))
                value.remove(0, 1);
            if (value.endsWith(QLatin1Char('"')))
                value.chop(1);
            row.insert(QStringLiteral("term"), value);
        } else {
            const QStringList cells = csvCells(lines.at(index), separator);
            const auto cell = [&cells](int column) { return column >= 0 && column < cells.size() ? cells.at(column) : QString(); };
            row.insert(QStringLiteral("term"), cell(term));
            row.insert(QStringLiteral("clicks"), clicks >= 0 ? importNumber(cell(clicks)) : QJsonValue::Null);
            row.insert(QStringLiteral("impressions"), impressions >= 0 ? importNumber(cell(impressions)) : QJsonValue::Null);
            row.insert(QStringLiteral("position"), position >= 0 ? importNumber(cell(position)) : QJsonValue::Null);
        }
        if (!text(row, "term").isEmpty())
            rows << row;
    }
    return rows;
}

const QList<std::pair<QString, QString>> &advisorAreas()
{
    static const QList<std::pair<QString, QString>> areas{
        {QString(), QStringLiteral("All")},
        {QStringLiteral("website"), QStringLiteral("Website")},
        {QStringLiteral("content"), QStringLiteral("Content")},
        {QStringLiteral("ads"), QStringLiteral("Ads")},
        {QStringLiteral("growth"), QStringLiteral("Growth")},
        {QStringLiteral("campaigns"), QStringLiteral("Campaigns")},
        {QStringLiteral("channels"), QStringLiteral("Channels")},
        {QStringLiteral("search"), QStringLiteral("Search")},
        {QStringLiteral("community"), QStringLiteral("Community")}};
    return areas;
}

QStringList advisorAreaLabels()
{
    QStringList labels;
    for (const auto &area : advisorAreas())
        labels << area.second;
    return labels;
}

QString advisorDoneKey(const QString &organization)
{
    return QStringLiteral("marketing/advisorDone/") + organization;
}

QStringList words(const QJsonValue &value)
{
    QStringList list;
    for (const QJsonValue &entry : value.toArray())
        list << entry.toString();
    return list;
}

} // namespace

QString MarketingFeature::channelLabel(const QString &id) const
{
    return channelName(id, m_settings, m_channelCatalog);
}

void MarketingFeature::buildPages(QVBoxLayout *outer, QVBoxLayout *content)
{
    m_pages << m_scroll;
    m_pageNav = clearWidget();
    auto *nav = new QHBoxLayout(m_pageNav);
    nav->setContentsMargins(0, 0, 0, 0);
    nav->setSpacing(6);
    m_pageGroup = new QButtonGroup(this);
    nav->addWidget(segmented({QStringLiteral("Dashboard"), QStringLiteral("Channels"),
                              QStringLiteral("Ads manager"), QStringLiteral("Growth"),
                              QStringLiteral("Advisor")},
                             m_pageGroup, 0, QStringLiteral("Marketing section")));
    nav->addStretch(1);
    const auto tools = [this, nav] {
        auto *widget = clearWidget();
        auto *layout = new QHBoxLayout(widget);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(6);
        nav->addWidget(widget);
        m_pageTools << widget;
        return layout;
    };
    const auto button = [](const QString &label, bool primary, const QString &tip = QString()) {
        auto *result = new QPushButton(label);
        result->setObjectName(primary ? QStringLiteral("primaryButton") : QStringLiteral("ghostButton"));
        if (!tip.isEmpty())
            result->setToolTip(tip);
        return result;
    };
    const auto addPage = [this, content](QVBoxLayout *&layout, const char *name) {
        QScrollArea *scroll = nullptr;
        page(scroll, layout, name);
        scroll->hide();
        content->addWidget(scroll, 1);
        m_pages << scroll;
    };

    QHBoxLayout *dashboardTools = tools();
    dashboardTools->addWidget(m_rangeSwitch);
    dashboardTools->addWidget(m_metricButton);
    dashboardTools->addWidget(m_newButton);

    QHBoxLayout *channelsTools = tools();
    auto *staleButton = button(QStringLiteral("Update stale"), false,
                               QStringLiteral("Refresh every channel whose numbers are older than 7 days"));
    auto *addChannelsButton = button(QStringLiteral("Add channels"), true);
    auto *addLinkButton = button(QStringLiteral("Add link"), false,
                                 QStringLiteral("Track the stats of something you posted on any channel"));
    channelsTools->addWidget(addLinkButton);
    channelsTools->addWidget(staleButton);
    channelsTools->addWidget(addChannelsButton);

    QHBoxLayout *adsTools = tools();
    auto *newAdButton = button(QStringLiteral("New ad"), true);
    adsTools->addWidget(newAdButton);

    QHBoxLayout *growthTools = tools();
    auto *reportButton = button(QStringLiteral("Weekly report"), false,
                                QStringLiteral("Scorecard, experiments, ads, content and Reddit as Markdown"));
    auto *newExperimentButton = button(QStringLiteral("New experiment"), true);
    auto *goalsButton = button(QStringLiteral("Goals"), false, QStringLiteral("Which Analytics events count as a conversion"));
    growthTools->addWidget(goalsButton);
    growthTools->addWidget(reportButton);
    growthTools->addWidget(newExperimentButton);

    QHBoxLayout *advisorTools = tools();
    auto *advisorRefresh = button(QStringLiteral("Refresh"), false,
                                  QStringLiteral("Read every channel, campaign, ad, experiment and Analytics again"));
    advisorTools->addWidget(advisorRefresh);

    m_heading->insertWidget(1, m_pageNav, 1);
    m_channelBar = new TileGrid(196, 6);
    m_channelBar->setAccessibleName(QStringLiteral("Marketing channels"));
    m_channelBar->hide();
    outer->insertWidget(1, m_channelBar);

    QVBoxLayout *channels = nullptr;
    addPage(channels, "marketingChannelsPage");
    m_healthKpis = new KpiStrip;
    channels->addWidget(m_healthKpis);
    auto *healthRow = new QHBoxLayout;
    healthRow->setSpacing(6);
    m_healthFilter = new QButtonGroup(this);
    healthRow->addWidget(segmented(choiceLabels(healthFilters()), m_healthFilter, 0, QStringLiteral("Channel status")));
    healthRow->addStretch(1);
    m_channelsSearch = lineEdit();
    m_channelsSearch->setPlaceholderText(QStringLiteral("Search channels"));
    m_channelsSearch->setAccessibleName(QStringLiteral("Search channels"));
    m_channelsSearch->setMaximumWidth(260);
    healthRow->addWidget(m_channelsSearch);
    m_channelsCategory = new QComboBox;
    m_channelsCategory->setAccessibleName(QStringLiteral("Channel category"));
    healthRow->addWidget(m_channelsCategory);
    channels->addLayout(healthRow);
    m_channelGroups = new TileGrid(480, 14);
    channels->addWidget(m_channelGroups);
    m_channelsEmpty = hintLabel(QStringLiteral("No channels in this view."));
    m_channelsEmpty->setAlignment(Qt::AlignCenter);
    m_channelsEmpty->setContentsMargins(12, 24, 12, 24);
    channels->addWidget(m_channelsEmpty);
    channels->addWidget(hintLabel(QStringLiteral(
        "Health checks: a channel link, numbers updated in the last 7 days, a post within twice the channel's usual cadence, and an audience that is not shrinking. The extension reads only the channel page you asked it to open, in your own browser; ForkMesh's servers never sign in to your accounts.")));
    m_postsPanel = new DashboardPanel(QStringLiteral("Posted links"));
    m_postsPanel->body()->setContentsMargins(6, 4, 6, 8);
    auto *addPostLink = linkButton(QStringLiteral("Add link"), QStringLiteral("Track the stats of something you posted on any channel"));
    m_postsPanel->tools()->addWidget(addPostLink);
    m_postsEmpty = hintLabel(QStringLiteral(
        "Add a link to anything you posted, on any channel, to keep its views, likes, replies and reposts here. For X, scroll your profile in the browser with the ForkMesh extension and press Sync now in its popup: every post it read is added with its stats."));
    m_postsEmpty->setContentsMargins(8, 8, 8, 8);
    m_postsPanel->body()->addWidget(m_postsEmpty);
    m_postsTable = pageTable("marketingPostsTable",
        {QStringLiteral("Post"), QStringLiteral("Channel"), QStringLiteral("Posted"), QStringLiteral("Views"),
         QStringLiteral("Likes"), QStringLiteral("Replies"), QStringLiteral("Reposts"), QStringLiteral("Checked"),
         QStringLiteral("Added by")}, 0);
    m_postsPanel->body()->addWidget(m_postsTable);
    m_channelsGrid = new DashboardGrid;
    m_channelsGrid->addPanel(m_postsPanel, 3);
    m_channelsGrid->addPanel(m_channelsPanel, 3);
    m_channelsGrid->addPanel(m_catalogPanel, 3);
    channels->addWidget(m_channelsGrid);
    channels->addStretch(1);

    QVBoxLayout *ads = nullptr;
    addPage(ads, "marketingAdsPage");
    m_adsKpis = new KpiStrip;
    ads->addWidget(m_adsKpis);
    m_adsGrid = new DashboardGrid;
    m_adsSpendPanel = new DashboardPanel(QStringLiteral("Spend by channel"));
    m_adsSpendPanel->hint()->setText(QStringLiteral("Select a channel to filter"));
    m_adsCurrency = new QComboBox;
    m_adsCurrency->setAccessibleName(QStringLiteral("Currency"));
    m_adsCurrency->hide();
    m_adsSpendPanel->tools()->addWidget(m_adsCurrency);
    m_adsSpend = new BarRows;
    m_adsSpend->setAccessibleName(QStringLiteral("Ad spend by channel"));
    m_adsSpendPanel->body()->addWidget(m_adsSpend);
    m_adsGrid->addPanel(m_adsSpendPanel, 2);
    m_adsAttentionPanel = new DashboardPanel(QStringLiteral("Needs attention"));
    m_adsAttention = new QVBoxLayout;
    m_adsAttention->setSpacing(8);
    m_adsAttentionPanel->body()->addLayout(m_adsAttention);
    m_adsGrid->addPanel(m_adsAttentionPanel, 1);
    m_adPlanPanel = new DashboardPanel(QStringLiteral("Where to advertise"));
    m_adPlanPanel->hint()->setText(QStringLiteral("Ranked from your visits, conversions and logged ads"));
    m_adPlanPanel->body()->setContentsMargins(6, 4, 6, 8);
    m_adPlanTable = pageTable("marketingAdPlanTable",
        {QStringLiteral("Score"), QStringLiteral("Where"), QStringLiteral("Start at / day"), QStringLiteral("Measure"),
         QStringLiteral("Target")}, 1);
    m_adPlanPanel->body()->addWidget(m_adPlanTable);
    m_adPlanWhy = hintLabel();
    m_adPlanWhy->setTextFormat(Qt::RichText);
    m_adPlanWhy->setContentsMargins(6, 4, 6, 0);
    m_adPlanPanel->body()->addWidget(m_adPlanWhy);
    auto *planActions = new QHBoxLayout;
    planActions->addStretch(1);
    m_adPlanConsole = new QPushButton(QStringLiteral("Ad console"));
    m_adPlanLink = new QPushButton(QStringLiteral("Copy link"));
    m_adPlanAd = new QPushButton(QStringLiteral("Plan ad"));
    for (QPushButton *item : {m_adPlanConsole, m_adPlanLink, m_adPlanAd})
        planActions->addWidget(item);
    m_adPlanPanel->body()->addLayout(planActions);
    m_adsGrid->addPanel(m_adPlanPanel, 2);
    m_readinessPanel = new DashboardPanel(QStringLiteral("Ad readiness"));
    m_readinessPanel->body()->setContentsMargins(6, 4, 6, 8);
    m_readinessTable = pageTable("marketingAdReadinessTable", {QStringLiteral("Check"), QStringLiteral("Status")}, 0);
    m_readinessPanel->body()->addWidget(m_readinessTable);
    m_readinessPanel->body()->addWidget(hintLabel(QStringLiteral("Double-click a check that is not ready to fix it.")));
    m_adsGrid->addPanel(m_readinessPanel, 1);
    auto *adFilters = new QVBoxLayout;
    adFilters->setSpacing(6);
    auto *adTabsRow = new QHBoxLayout;
    m_adsStatus = new QButtonGroup(this);
    QStringList adTabs{QStringLiteral("All")};
    adTabs << choiceLabels(adStatuses());
    adTabsRow->addWidget(segmented(adTabs, m_adsStatus, 0, QStringLiteral("Ad status")));
    adTabsRow->addStretch(1);
    adFilters->addLayout(adTabsRow);
    auto *adSearchRow = new QHBoxLayout;
    adSearchRow->setSpacing(6);
    adFilters->addLayout(adSearchRow);
    m_adsSearch = lineEdit();
    m_adsSearch->setPlaceholderText(QStringLiteral("Search ad, campaign, owner, audience or tag"));
    m_adsSearch->setAccessibleName(QStringLiteral("Search ads"));
    adSearchRow->addWidget(m_adsSearch, 1);
    m_adsChannel = new QComboBox;
    m_adsChannel->setAccessibleName(QStringLiteral("Ad channel"));
    adSearchRow->addWidget(m_adsChannel);
    m_adsPanel->body()->insertLayout(1, adFilters);
    m_adsSummary->hide();
    m_adsGrid->addPanel(m_adsPanel, 3);
    auto *measuredAds = new DashboardPanel(QStringLiteral("Ads measured by Analytics"));
    measuredAds->setCompact();
    measuredAds->hint()->setText(QStringLiteral("Matched by each ad's link tags · last 28 days"));
    measuredAds->body()->setContentsMargins(6, 4, 6, 8);
    m_performanceAds = pageTable("marketingPerformanceAdsTable",
        {QStringLiteral("Ad"), QStringLiteral("Channel"), QStringLiteral("Clicks logged"), QStringLiteral("Visits"),
         QStringLiteral("Click → visit"), QStringLiteral("Conv."), QStringLiteral("Cost / visit"),
         QStringLiteral("Cost / conv.")}, 0);
    measuredAds->body()->addWidget(m_performanceAds);
    m_adsGrid->addPanel(measuredAds, 3);
    ads->addWidget(m_adsGrid);
    ads->addWidget(hintLabel(QStringLiteral(
        "Ads are planned and measured here and bought in each platform's own console. Results are logged by hand; ForkMesh never spends money or connects to an ad account. Follow each platform's advertising policies and the disclosure rules of every market you advertise in.")));
    ads->addStretch(1);

    QVBoxLayout *growth = nullptr;
    addPage(growth, "marketingGrowthPage");
    m_growthKpis = new KpiStrip;
    growth->addWidget(m_growthKpis);
    auto *growthGrid = new DashboardGrid;
    m_funnelPanel = new DashboardPanel(QStringLiteral("Funnel"));
    auto *trackingButton = linkButton(QStringLiteral("Targets"), QStringLiteral("Funnel targets and the link-tracking allowlists"));
    auto *logWeekButton = linkButton(QStringLiteral("Log week"), QStringLiteral("Copy a week's numbers from your analytics"));
    m_funnelPanel->tools()->addWidget(trackingButton);
    m_funnelPanel->tools()->addWidget(logWeekButton);
    m_funnelBars = new BarRows;
    m_funnelBars->setAccessibleName(QStringLiteral("Funnel for the latest week"));
    m_funnelPanel->body()->addWidget(m_funnelBars);
    m_bottleneck = richLabel();
    m_funnelPanel->body()->addWidget(m_bottleneck);
    growthGrid->addPanel(m_funnelPanel, 2);
    auto *linkPanel = new DashboardPanel(QStringLiteral("Link builder"));
    auto *allowlists = linkButton(QStringLiteral("Allowlists"), QStringLiteral("Sources, mediums and campaigns the checks accept"));
    linkPanel->tools()->addWidget(allowlists);
    auto *linkForm = new QGridLayout;
    linkForm->setSpacing(6);
    m_linkUrl = lineEdit();
    m_linkUrl->setPlaceholderText(QStringLiteral("https://…"));
    m_linkUrl->setAccessibleName(QStringLiteral("Landing page"));
    m_linkSource = lineEdit();
    m_linkSource->setPlaceholderText(QStringLiteral("Source, e.g. mastodon"));
    m_linkMedium = lineEdit();
    m_linkMedium->setPlaceholderText(QStringLiteral("Medium, e.g. social"));
    m_linkCampaign = lineEdit();
    m_linkCampaign->setPlaceholderText(QStringLiteral("Campaign, e.g. launch"));
    m_linkContent = lineEdit();
    m_linkContent->setPlaceholderText(QStringLiteral("Content, e.g. post-slug"));
    for (QLineEdit *edit : {m_linkSource, m_linkMedium, m_linkCampaign, m_linkContent})
        edit->setAccessibleName(edit->placeholderText().section(QLatin1Char(','), 0, 0));
    linkForm->addWidget(m_linkUrl, 0, 0, 1, 2);
    linkForm->addWidget(m_linkSource, 1, 0);
    linkForm->addWidget(m_linkMedium, 1, 1);
    linkForm->addWidget(m_linkCampaign, 2, 0);
    linkForm->addWidget(m_linkContent, 2, 1);
    linkPanel->body()->addLayout(linkForm);
    auto *linkActions = new QHBoxLayout;
    auto *buildButton = new QPushButton(QStringLiteral("Build and check"));
    buildButton->setObjectName(QStringLiteral("primaryButton"));
    m_linkCopy = new QPushButton(QStringLiteral("Copy link"));
    m_linkCopy->hide();
    linkActions->addWidget(buildButton);
    linkActions->addWidget(m_linkCopy);
    linkActions->addStretch();
    linkPanel->body()->addLayout(linkActions);
    m_linkOutput = hintLabel(QString(), "marketingNote");
    m_linkOutput->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_linkOutput->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_linkOutput->hide();
    linkPanel->body()->addWidget(m_linkOutput);
    m_linkChecks = richLabel();
    m_linkChecks->hide();
    linkPanel->body()->addWidget(m_linkChecks);
    growthGrid->addPanel(linkPanel, 1);
    m_experimentsPanel = new DashboardPanel(QStringLiteral("Experiment backlog"));
    m_experimentsPanel->body()->setContentsMargins(6, 4, 6, 8);
    m_experimentNotes = richLabel();
    m_experimentNotes->setContentsMargins(6, 4, 6, 4);
    m_experimentsPanel->body()->addWidget(m_experimentNotes);
    m_experimentStatus = new QButtonGroup(this);
    QStringList experimentTabs{QStringLiteral("Open")};
    experimentTabs << choiceLabels(experimentStatuses());
    auto *experimentFilter = new QHBoxLayout;
    experimentFilter->addWidget(segmented(experimentTabs, m_experimentStatus, 0, QStringLiteral("Experiment status")));
    experimentFilter->addStretch(1);
    m_experimentsPanel->body()->addLayout(experimentFilter);
    m_experimentsTable = pageTable("marketingExperimentsTable",
        {QStringLiteral("Experiment"), QStringLiteral("Stage"), QStringLiteral("Status"), QStringLiteral("Impact"),
         QStringLiteral("Confidence"), QStringLiteral("Ease"), QStringLiteral("Score"), QStringLiteral("Dates")}, 0);
    m_experimentsPanel->body()->addWidget(m_experimentsTable);
    auto *experimentActions = new QHBoxLayout;
    experimentActions->addWidget(hintLabel(QStringLiteral(
        "Score each idea by impact, confidence and ease from 1 to 10, then run the top three.")), 1);
    auto *editExperimentButton = new QPushButton(QStringLiteral("Edit"));
    auto *addExperimentButton = new QPushButton(QStringLiteral("New experiment"));
    experimentActions->addWidget(editExperimentButton);
    experimentActions->addWidget(addExperimentButton);
    m_experimentsPanel->body()->addLayout(experimentActions);
    growthGrid->addPanel(m_experimentsPanel, 2);
    m_performanceInsightsPanel = new DashboardPanel(QStringLiteral("Where to improve"));
    m_performanceInsightsPanel->setCompact();
    m_performanceInsights = new QVBoxLayout;
    m_performanceInsights->setSpacing(6);
    m_performanceInsightsPanel->body()->addLayout(m_performanceInsights);
    growthGrid->addPanel(m_performanceInsightsPanel, 1);
    auto *weeksPanel = new DashboardPanel(QStringLiteral("Weekly funnel"));
    weeksPanel->hint()->setText(QStringLiteral("A dash means nobody logged it, not zero"));
    weeksPanel->body()->setContentsMargins(6, 4, 6, 8);
    m_funnelTable = pageTable("marketingFunnelTable",
        {QStringLiteral("Week of"), QStringLiteral("North star"), QStringLiteral("Visits"), QStringLiteral("Signups"),
         QStringLiteral("Activated"), QStringLiteral("Retained"), QStringLiteral("Referrals"), QStringLiteral("Revenue"),
         QStringLiteral("Source")}, 8);
    weeksPanel->body()->addWidget(m_funnelTable);
    auto *weekActions = new QHBoxLayout;
    weekActions->addWidget(hintLabel(QStringLiteral(
        "Funnel numbers are copied by hand from your own analytics; ForkMesh does not estimate or fill them. A rate whose base is under 20 is marked as too small to read.")), 1);
    auto *editWeekButton = new QPushButton(QStringLiteral("Edit"));
    auto *deleteWeekButton = new QPushButton(QStringLiteral("Delete"));
    deleteWeekButton->setObjectName(QStringLiteral("dangerButton"));
    auto *addWeekButton = new QPushButton(QStringLiteral("Log week"));
    for (QPushButton *item : {editWeekButton, deleteWeekButton, addWeekButton})
        weekActions->addWidget(item);
    weeksPanel->body()->addLayout(weekActions);
    growthGrid->addPanel(weeksPanel, 3);
    growth->addWidget(growthGrid);
    growth->addStretch(1);

    QVBoxLayout *advisor = nullptr;
    addPage(advisor, "marketingAdvisorPage");
    auto *advisorPanel = new DashboardPanel(QStringLiteral("Next to-dos"));
    advisorPanel->body()->setContentsMargins(6, 4, 6, 8);
    m_advisorSummary = advisorPanel->hint();
    m_advisorPriority = new QComboBox;
    m_advisorPriority->setAccessibleName(QStringLiteral("Priority"));
    m_advisorPriority->addItem(QStringLiteral("All priorities"), QString());
    m_advisorPriority->addItem(QStringLiteral("High"), QStringLiteral("high"));
    m_advisorPriority->addItem(QStringLiteral("Medium"), QStringLiteral("medium"));
    m_advisorPriority->addItem(QStringLiteral("Low"), QStringLiteral("low"));
    advisorPanel->tools()->addWidget(m_advisorPriority);
    m_advisorShowDone = new QCheckBox(QStringLiteral("Show done"));
    advisorPanel->tools()->addWidget(m_advisorShowDone);
    auto *areaRow = new QHBoxLayout;
    m_advisorArea = new QButtonGroup(this);
    areaRow->addWidget(segmented(advisorAreaLabels(), m_advisorArea, 0, QStringLiteral("To-do area")));
    areaRow->addStretch(1);
    advisorPanel->body()->addLayout(areaRow);
    m_advisorTable = pageTable("marketingAdvisorTable",
        {QStringLiteral("Priority"), QStringLiteral("To-do"), QStringLiteral("Why"), QStringLiteral("Area"),
         QStringLiteral("Channel")}, 2);
    m_advisorTable->setWordWrap(true);
    m_advisorTable->setMinimumHeight(360);
    advisorPanel->body()->addWidget(m_advisorTable);
    auto *advisorActions = new QHBoxLayout;
    advisorActions->addWidget(hintLabel(QStringLiteral(
        "Read from every channel, campaign, piece of content, ad, experiment, the funnel scorecard and the last 30 days of Analytics, most important first. Done hides an item on this computer for 7 days.")), 1);
    m_advisorDone = new QPushButton(QStringLiteral("Mark done"));
    m_advisorRun = new QPushButton(QStringLiteral("Open"));
    advisorActions->addWidget(m_advisorDone);
    advisorActions->addWidget(m_advisorRun);
    advisorPanel->body()->addLayout(advisorActions);
    advisor->addWidget(advisorPanel);
    advisor->addStretch(1);

    auto *reddit = static_cast<QVBoxLayout *>(m_detailRedditBox->layout());
    auto *redditGrid = new DashboardGrid;
    static_cast<QGridLayout *>(redditGrid->layout())->setSpacing(6);
    const auto redditPanel = [](const QString &title) {
        auto *panel = new DashboardPanel(title);
        panel->setCompact();
        return panel;
    };
    const auto linkList = [this] {
        auto *list = richLabel();
        list->setOpenExternalLinks(false);
        list->setTextInteractionFlags(Qt::TextBrowserInteraction);
        connect(list, &QLabel::linkActivated, this, [this](const QString &link) { openRedditLink(link); });
        return list;
    };

    m_redditIdeasPanel = redditPanel(QStringLiteral("Where to post next"));
    m_redditIdeasPanel->setToolTip(QStringLiteral(
        "Ranked by what the log already knows: open threads waiting for an answer, replies and posts that landed, and whether the subreddit's rules are on file. "
        "Discovered subreddits come from threads and posts logged outside the watch list and from communities related to your topics and keywords. "
        "Rules are read by the ForkMesh browser extension in your own browser and saved for the whole team; they do not tick “Rules read” on a thread. "
        "Draft with ChatGPT opens a prompt with ForkMesh, links tagged for the subreddit, its rules, the posts logged there and how our earlier Reddit posts did."));
    auto *addPlace = linkButton(QStringLiteral("Add"), QStringLiteral("Add a subreddit to post in or a topic to watch"));
    auto *readRules = linkButton(QStringLiteral("Read missing rules"),
                                 QStringLiteral("The ForkMesh browser extension opens each watched subreddit's rules page in one tab and saves what it reads"));
    auto *copyRules = linkButton(QStringLiteral("Copy rules"),
                                 QStringLiteral("Every rule set on file as Markdown, for a Reddit scout run"));
    for (QPushButton *item : {addPlace, readRules, copyRules})
        m_redditIdeasPanel->tools()->addWidget(item);
    auto *ideaColumns = new QHBoxLayout;
    ideaColumns->setSpacing(14);
    const auto ideaColumn = [&ideaColumns, &linkList](const QString &title, int stretch) {
        auto *column = new QVBoxLayout;
        column->setSpacing(3);
        auto *head = hintLabel(title.toUpper(), "marketingFactLabel");
        column->addWidget(head);
        QLabel *list = linkList();
        column->addWidget(list);
        column->addStretch(1);
        ideaColumns->addLayout(column, stretch);
        return list;
    };
    m_redditPlaces = ideaColumn(QStringLiteral("Subreddits and their rules"), 3);
    m_redditDiscover = ideaColumn(QStringLiteral("Discovered"), 2);
    m_redditTopics = ideaColumn(QStringLiteral("What to talk about"), 2);
    m_redditIdeasPanel->body()->addLayout(ideaColumns);
    redditGrid->addPanel(m_redditIdeasPanel, 3);

    m_redditPanel = redditPanel(QStringLiteral("Thread log"));
    m_redditPanel->body()->setContentsMargins(6, 4, 6, 6);
    m_redditStatus = new QButtonGroup(this);
    QStringList redditTabs{QStringLiteral("Open")};
    redditTabs << choiceLabels(redditStatuses()) << QStringLiteral("Monitored");
    auto *redditFilter = new QHBoxLayout;
    redditFilter->addWidget(segmented(redditTabs, m_redditStatus, 0, QStringLiteral("Thread status")));
    redditFilter->addStretch(1);
    m_redditPanel->body()->addLayout(redditFilter);
    m_redditTable = pageTable("marketingRedditTable",
        {QStringLiteral("Thread"), QStringLiteral("Subreddit"), QStringLiteral("Status"), QStringLiteral("Activity"),
         QStringLiteral("Checks"), QStringLiteral("Tagged link"), QString()}, 5);
    m_redditTable->setTextElideMode(Qt::ElideMiddle);
    m_redditPanel->body()->addWidget(m_redditTable);
    auto *threadActions = new QHBoxLayout;
    threadActions->setSpacing(6);
    threadActions->addStretch(1);
    auto *openThread = new QPushButton(QStringLiteral("Open on Reddit ↗"));
    auto *copyThreadLink = new QPushButton(QStringLiteral("Copy tagged link"));
    auto *editThread = new QPushButton(QStringLiteral("Edit"));
    auto *addThread = new QPushButton(QStringLiteral("Log thread"));
    addThread->setObjectName(QStringLiteral("primaryButton"));
    auto *monitorThread = new QPushButton(QStringLiteral("Monitor a thread…"));
    monitorThread->setToolTip(QStringLiteral("Paste a Reddit thread or comment link to follow its views, points and comments. The ForkMesh browser extension reads them in your signed-in browser."));
    auto *updateThreads = new QPushButton(QStringLiteral("Update all"));
    updateThreads->setToolTip(QStringLiteral("Opens the Reddit desk in your browser, where the ForkMesh extension reads every logged thread's comments, points and views in a background tab. This log updates as they are read."));
    for (QPushButton *item : {updateThreads, openThread, copyThreadLink, editThread, monitorThread})
        item->setObjectName(QStringLiteral("ghostButton"));
    for (QPushButton *item : {updateThreads, openThread, copyThreadLink, editThread, monitorThread, addThread})
        threadActions->addWidget(item);
    m_redditPanel->body()->addLayout(threadActions);
    redditGrid->addPanel(m_redditPanel, 2);

    auto *watchPanel = redditPanel(QStringLiteral("Watch"));
    watchPanel->setToolTip(QStringLiteral(
        "Each link opens Reddit's newest posts in your own browser. ForkMesh's servers never fetch or post to Reddit: log a thread by hand, or let the ForkMesh browser extension fill the log from the pages you open and send it from its popup."));
    auto *editWatch = linkButton(QStringLiteral("Edit"), QStringLiteral("Change the subreddits and searches"));
    watchPanel->tools()->addWidget(editWatch);
    m_redditSubreddits = linkList();
    m_redditQueries = linkList();
    m_redditAccounts = linkList();
    watchPanel->body()->addWidget(m_redditSubreddits);
    watchPanel->body()->addWidget(m_redditQueries);
    watchPanel->body()->addWidget(m_redditAccounts);
    auto *checklist = hintLabel(QStringLiteral(
        "Before you reply: read the rules on self-promotion · say you work on ForkMesh · write and post it yourself · never vote or ask for votes"));
    checklist->setToolTip(QStringLiteral(
        "A thread can be marked replied only after Rules read and Disclosed are ticked and the address of your reply is saved."));
    watchPanel->body()->addWidget(checklist);
    watchPanel->body()->addStretch(1);
    redditGrid->addPanel(watchPanel, 1);

    m_performanceRedditPanel = redditPanel(QStringLiteral("Results by subreddit"));
    m_performanceRedditPanel->hint()->setText(QStringLiteral("Visits from each subreddit's tagged links"));
    m_performanceRedditPanel->body()->setContentsMargins(6, 4, 6, 6);
    m_performanceReddit = pageTable("marketingPerformanceRedditTable",
        {QStringLiteral("Subreddit"), QStringLiteral("Threads"), QStringLiteral("Replied"), QStringLiteral("Visits"),
         QStringLiteral("Bounce"), QStringLiteral("Conv.")}, 0);
    m_performanceRedditPanel->body()->addWidget(m_performanceReddit);
    m_performanceRedditPanel->hide();
    redditGrid->addPanel(m_performanceRedditPanel, 3);
    reddit->addWidget(redditGrid);

    auto *appStore = static_cast<QVBoxLayout *>(m_detailAppStoreBox->layout());
    auto *appStoreHead = new QHBoxLayout;
    appStoreHead->setSpacing(6);
    m_appStoreFacts = richLabel("marketingHint");
    m_appStoreFacts->setAccessibleName(QStringLiteral("App Store facts"));
    appStoreHead->addWidget(m_appStoreFacts, 1);
    m_appStoreOpen = linkButton(QStringLiteral("Open in App Store ↗"),
                                QStringLiteral("Open the app's page in the App Store"));
    appStoreHead->addWidget(m_appStoreOpen, 0, Qt::AlignTop);
    appStore->addLayout(appStoreHead);
    m_appStoreSetup = hintLabel();
    m_appStoreSetup->setTextFormat(Qt::PlainText);
    m_appStoreSetup->setAccessibleName(QStringLiteral("App Store setup"));
    appStore->addWidget(m_appStoreSetup);
    m_appStoreKpis = new KpiStrip;
    m_appStoreKpis->setCompact(true);
    m_appStoreKpis->setAccessibleName(QStringLiteral("App Store numbers"));
    appStore->addWidget(m_appStoreKpis);
    auto *appStoreGrid = new DashboardGrid;
    static_cast<QGridLayout *>(appStoreGrid->layout())->setSpacing(6);
    m_appStoreChartPanel = redditPanel(QStringLiteral("Downloads over time"));
    m_appStoreRange = new QButtonGroup(this);
    m_appStoreChartPanel->tools()->addWidget(segmented(
        {QStringLiteral("30D"), QStringLiteral("90D"), QStringLiteral("365D")},
        m_appStoreRange, 0, QStringLiteral("App Store range")));
    m_appStoreChart = new LineChart;
    m_appStoreChart->setAccessibleName(QStringLiteral("App Store downloads chart"));
    m_appStoreChart->setMaximumHeight(170);
    m_appStoreChart->setMinimumWidth(240);
    m_appStoreChartPanel->body()->addWidget(m_appStoreChart);
    appStoreGrid->addPanel(m_appStoreChartPanel, 2);
    m_appStoreCountriesPanel = redditPanel(QStringLiteral("Top countries"));
    m_appStoreCountries = new BarRows;
    m_appStoreCountries->setAccessibleName(QStringLiteral("App Store downloads by country"));
    m_appStoreCountries->setEmptyText(QStringLiteral("No downloads in this range"));
    m_appStoreCountriesPanel->body()->addWidget(m_appStoreCountries);
    m_appStoreCountriesPanel->body()->addStretch(1);
    appStoreGrid->addPanel(m_appStoreCountriesPanel, 1);
    m_appStoreDevicesPanel = redditPanel(QStringLiteral("Devices"));
    m_appStoreDevices = new BarRows;
    m_appStoreDevices->setAccessibleName(QStringLiteral("App Store downloads by device"));
    m_appStoreDevices->setEmptyText(QStringLiteral("No downloads in this range"));
    m_appStoreDevicesPanel->body()->addWidget(m_appStoreDevices);
    m_appStoreDevicesPanel->body()->addStretch(1);
    appStoreGrid->addPanel(m_appStoreDevicesPanel, 1);
    m_appStoreVersionsPanel = redditPanel(QStringLiteral("Versions"));
    m_appStoreVersions = richLabel();
    m_appStoreVersionsPanel->body()->addWidget(m_appStoreVersions);
    m_appStoreVersionsPanel->body()->addStretch(1);
    appStoreGrid->addPanel(m_appStoreVersionsPanel, 1);
    m_appStoreReviewsPanel = redditPanel(QStringLiteral("Reviews"));
    m_appStoreReviews = richLabel();
    m_appStoreReviewsPanel->body()->addWidget(m_appStoreReviews);
    m_appStoreReviewsPanel->body()->addStretch(1);
    appStoreGrid->addPanel(m_appStoreReviewsPanel, 1);
    appStore->addWidget(appStoreGrid);
    connect(m_appStoreRange, &QButtonGroup::idClicked, this, [this](int id) {
        static constexpr int windows[] = {30, 90, 365};
        m_appStoreDays = windows[qBound(0, id, 2)];
        renderAppStore();
    });
    connect(m_appStoreOpen, &QPushButton::clicked, this, [this] {
        const QUrl url(m_appStoreOpen->property("url").toString());
        if (url.isValid() && url.scheme() == QLatin1String("https"))
            QDesktopServices::openUrl(url);
    });

    connect(m_pageGroup, &QButtonGroup::idClicked, this, [this](int id) { showPage(id); });
    connect(advisorRefresh, &QPushButton::clicked, this, [this] { loadAdvisor(); });
    connect(m_advisorArea, &QButtonGroup::idClicked, this, [this](int) { renderAdvisor(); });
    connect(m_advisorPriority, &QComboBox::activated, this, [this](int) { renderAdvisor(); });
    connect(m_advisorShowDone, &QCheckBox::toggled, this, [this](bool) { renderAdvisor(); });
    const auto advisorItem = [this] {
        const QString id = rowId(m_advisorTable);
        for (const QJsonValue &value : m_advisor.value(QStringLiteral("items")).toArray())
            if (!id.isEmpty() && text(value.toObject(), "id") == id)
                return value.toObject();
        return QJsonObject();
    };
    connect(m_advisorRun, &QPushButton::clicked, this, [this, advisorItem] {
        runAdvisorAction(advisorItem().value(QStringLiteral("action")).toObject());
    });
    connect(m_advisorTable, &QTableWidget::itemDoubleClicked, this, [this, advisorItem](QTableWidgetItem *) {
        runAdvisorAction(advisorItem().value(QStringLiteral("action")).toObject());
    });
    connect(m_advisorDone, &QPushButton::clicked, this, [this] {
        if (!rowId(m_advisorTable).isEmpty())
            toggleAdvisorDone(rowId(m_advisorTable));
    });
    connect(m_advisorTable, &QTableWidget::currentCellChanged, this, [this, advisorItem](int, int, int, int) {
        const QJsonObject item = advisorItem();
        m_advisorRun->setEnabled(!item.value(QStringLiteral("action")).toObject().isEmpty());
        m_advisorDone->setEnabled(!item.isEmpty());
        m_advisorDone->setText(advisorDone().contains(text(item, "id")) ? QStringLiteral("Undo done")
                                                                         : QStringLiteral("Mark done"));
    });
    const auto planItem = [this] {
        return byId(m_adPlan.value(QStringLiteral("recommendations")).toArray(), rowId(m_adPlanTable));
    };
    connect(m_adPlanTable, &QTableWidget::currentCellChanged, this, [this, planItem](int, int, int, int) {
        const QJsonObject item = planItem();
        QStringList lines;
        for (const QJsonValue &line : item.value(QStringLiteral("why")).toArray())
            lines << QStringLiteral("• ") + line.toString().toHtmlEscaped();
        m_adPlanWhy->setText(item.isEmpty() ? QString()
                                            : QStringLiteral("<b>%1</b><br>%2<br>%3")
                                                  .arg(text(item, "name").toHtmlEscaped(),
                                                       text(item, "how").toHtmlEscaped(),
                                                       lines.join(QStringLiteral("<br>"))));
        m_adPlanAd->setEnabled(!item.isEmpty());
        m_adPlanLink->setEnabled(!text(item, "link").isEmpty());
        m_adPlanConsole->setEnabled(text(item, "manager").startsWith(QLatin1String("https://")));
    });
    connect(m_adPlanAd, &QPushButton::clicked, this, [this, planItem] { planAd(planItem()); });
    connect(m_adPlanTable, &QTableWidget::itemDoubleClicked, this, [this, planItem](QTableWidgetItem *) {
        planAd(planItem());
    });
    connect(m_adPlanLink, &QPushButton::clicked, this, [this, planItem] {
        if (text(planItem(), "link").isEmpty())
            return;
        QGuiApplication::clipboard()->setText(text(planItem(), "link"));
        setStatus(QStringLiteral("Tagged ad link copied."));
    });
    connect(m_adPlanConsole, &QPushButton::clicked, this, [planItem] {
        const QUrl url(text(planItem(), "manager"));
        if (url.isValid() && url.scheme() == QLatin1String("https"))
            QDesktopServices::openUrl(url);
    });
    connect(m_readinessTable, &QTableWidget::itemDoubleClicked, this, [this](QTableWidgetItem *cell) {
        QTableWidgetItem *first = cell ? m_readinessTable->item(cell->row(), 0) : nullptr;
        const QJsonObject check = byId(m_adPlan.value(QStringLiteral("readiness")).toArray(),
                                       first ? first->data(Qt::UserRole).toString() : QString());
        if (!check.isEmpty() && !check.value(QStringLiteral("done")).toBool())
            runAdvisorAction(check.value(QStringLiteral("action")).toObject());
    });
    connect(goalsButton, &QPushButton::clicked, this, [this] { editGoals(); });
    connect(m_healthFilter, &QButtonGroup::idClicked, this, [this](int) { renderChannelHealth(); });
    connect(m_channelsSearch, &QLineEdit::textChanged, this, [this](const QString &) { renderChannelHealth(); });
    connect(m_channelsCategory, &QComboBox::activated, this, [this](int) { renderChannelHealth(); });
    connect(staleButton, &QPushButton::clicked, this, [this] { updateStaleChannels(); });
    connect(addChannelsButton, &QPushButton::clicked, this, [this] { reveal(m_catalogPanel); });
    connect(addLinkButton, &QPushButton::clicked, this, [this] { editPostedLink(); });
    connect(addPostLink, &QPushButton::clicked, this, [this] { editPostedLink(); });
    connect(m_postsTable, &QTableWidget::itemDoubleClicked, this, [this](QTableWidgetItem *) {
        const QJsonObject post = byId(m_posts, rowId(m_postsTable));
        if (!post.isEmpty())
            editPostedLink(post);
    });
    connect(newAdButton, &QPushButton::clicked, this, [this] { editAd(); });
    connect(m_adsCurrency, &QComboBox::activated, this, [this](int index) {
        m_adCurrency = m_adsCurrency->itemData(index).toString();
        renderAdsPage();
    });
    m_adsSpend->onClicked = [this](int index) {
        const QStringList channels = m_adsSpend->property("channels").toStringList();
        if (index < 0 || index >= channels.size())
            return;
        m_adChannel = m_adChannel == channels.at(index) ? QString() : channels.at(index);
        renderAdsPage();
    };
    connect(m_adsStatus, &QButtonGroup::idClicked, this, [this](int) { renderAds(); });
    connect(m_adsSearch, &QLineEdit::textChanged, this, [this](const QString &) { renderAdsPage(); });
    connect(m_adsChannel, &QComboBox::activated, this, [this](int index) {
        m_adChannel = m_adsChannel->itemData(index).toString();
        renderAdsPage();
    });
    connect(reportButton, &QPushButton::clicked, this, [this] { showReport(); });
    connect(trackingButton, &QPushButton::clicked, this, [this] { editTracking(); });
    connect(allowlists, &QPushButton::clicked, this, [this] { editTracking(); });
    connect(logWeekButton, &QPushButton::clicked, this, [this] { editFunnelWeek(); });
    connect(addWeekButton, &QPushButton::clicked, this, [this] { editFunnelWeek(); });
    connect(newExperimentButton, &QPushButton::clicked, this, [this] { editExperiment(); });
    connect(addExperimentButton, &QPushButton::clicked, this, [this] { editExperiment(); });
    connect(buildButton, &QPushButton::clicked, this, [this] { buildLink(); });
    connect(m_linkUrl, &QLineEdit::returnPressed, this, [this] { buildLink(); });
    connect(m_linkCopy, &QPushButton::clicked, this, [this] {
        QGuiApplication::clipboard()->setText(m_linkOutput->text());
        setStatus(QStringLiteral("Link copied."));
    });
    connect(m_experimentStatus, &QButtonGroup::idClicked, this, [this](int) { renderGrowth(); });
    const auto editSelectedExperiment = [this] {
        const QJsonObject item = byId(m_experiments, rowId(m_experimentsTable));
        if (item.isEmpty())
            return setStatus(QStringLiteral("Select an experiment to edit."), true);
        editExperiment(item);
    };
    connect(editExperimentButton, &QPushButton::clicked, this, editSelectedExperiment);
    connect(m_experimentsTable, &QTableWidget::itemDoubleClicked, this, editSelectedExperiment);
    const auto selectedWeek = [this] { return byId(m_funnel, rowId(m_funnelTable)); };
    connect(editWeekButton, &QPushButton::clicked, this, [this, selectedWeek] {
        if (selectedWeek().isEmpty())
            return setStatus(QStringLiteral("Select a week to edit."), true);
        editFunnelWeek(selectedWeek());
    });
    connect(m_funnelTable, &QTableWidget::itemDoubleClicked, this, [this, selectedWeek] { editFunnelWeek(selectedWeek()); });
    connect(deleteWeekButton, &QPushButton::clicked, this, [this, selectedWeek] {
        if (selectedWeek().isEmpty())
            return setStatus(QStringLiteral("Select a week to delete."), true);
        deleteFunnelWeek(selectedWeek());
    });
    connect(m_redditStatus, &QButtonGroup::idClicked, this, [this](int) { renderReddit(); });
    connect(editWatch, &QPushButton::clicked, this, [this] { editRedditWatch(); });
    connect(copyRules, &QPushButton::clicked, this, [this] { copyRedditRules(); });
    connect(addPlace, &QPushButton::clicked, this, [this] { addRedditPlace(); });
    connect(readRules, &QPushButton::clicked, this, [this] { readRedditRules(); });
    connect(addThread, &QPushButton::clicked, this, [this] { editRedditThread(); });
    connect(monitorThread, &QPushButton::clicked, this, [this] { monitorRedditThread(); });
    connect(updateThreads, &QPushButton::clicked, this, [this] { updateRedditThreads(); });
    const auto selectedThread = [this] { return byId(m_redditThreads, rowId(m_redditTable)); };
    connect(editThread, &QPushButton::clicked, this, [this, selectedThread] {
        if (selectedThread().isEmpty())
            return setStatus(QStringLiteral("Select a thread to edit."), true);
        editRedditThread(selectedThread());
    });
    connect(m_redditTable, &QTableWidget::itemDoubleClicked, this, [this, selectedThread] { editRedditThread(selectedThread()); });
    connect(openThread, &QPushButton::clicked, this, [this, selectedThread] {
        const QString url = text(selectedThread(), "url");
        if (url.startsWith(QLatin1String("https://")))
            QDesktopServices::openUrl(QUrl(url));
        else
            setStatus(QStringLiteral("Select a thread to open."), true);
    });
    connect(copyThreadLink, &QPushButton::clicked, this, [this, selectedThread] {
        const QString link = text(selectedThread(), "taggedLink");
        if (link.isEmpty())
            return setStatus(QStringLiteral("Select a thread with a tagged link."), true);
        QGuiApplication::clipboard()->setText(link);
        setStatus(QStringLiteral("Tagged link copied."));
    });
}

void MarketingFeature::showPage(int page)
{
    const int previous = m_page;
    m_page = qBound(0, page, int(m_pages.size()) - 1);
    if (QAbstractButton *button = m_pageGroup ? m_pageGroup->button(m_page) : nullptr)
        button->setChecked(true);
    m_detailChannel.clear();
    m_detailRefresh.clear();
    applyView();
    if ((m_page == 2 || m_page == 3) && (previous < 2 || previous > 3 || m_performance.isEmpty()))
        loadPerformance();
    if ((m_page == 2 || m_page == 4) && m_advisor.isEmpty())
        loadAdvisor();
}

void MarketingFeature::renderPages()
{
    if (!m_performanceAds)
        return;
    renderPerformance();
    renderChannelHealth();
    renderPostedLinks();
    renderAdsPage();
    renderGrowth();
    renderReddit();
}

void MarketingFeature::loadPerformance()
{
    if (!m_installed || m_organization->currentData().toString().isEmpty())
        return;
    const int serial = ++m_performanceSerial;
    const qint64 to = QDateTime::currentMSecsSinceEpoch();
    const qint64 from = to - m_performanceDays * kDay;
    const QString path = organizationPath(QStringLiteral("/performance")) + QStringLiteral("?from=%1&to=%2").arg(from).arg(to);
    request(QByteArrayLiteral("GET"), path, {},
            [this, serial](bool ok, const QJsonObject &payload, const QString &error) {
        if (serial != m_performanceSerial)
            return;
        if (!ok)
            return setStatus(QStringLiteral("Could not read campaign results: %1").arg(error), true);
        m_performance = payload.value(QStringLiteral("performance")).toObject();
        if (m_performance.isEmpty())
            m_performance.insert(QStringLiteral("status"), QStringLiteral("unavailable"));
        renderPerformance();
    });
}

void MarketingFeature::renderPerformance()
{
    if (!m_performanceAds)
        return;
    const DashboardColors colors = DashboardColors::from(this);
    clearLayout(m_performanceInsights);
    if (m_performance.isEmpty()) {
        auto *loading = richLabel();
        loading->setText(QStringLiteral("Reading campaign results from Analytics…"));
        m_performanceInsights->addWidget(loading);
        return;
    }
    const bool ready = text(m_performance, "status") == QLatin1String("ok");
    const bool goals = !m_performance.value(QStringLiteral("goals")).toArray().isEmpty();
    const auto value = [](const QJsonObject &object, const char *key) { return object.value(QLatin1String(key)); };
    const QJsonArray insights = m_performance.value(QStringLiteral("insights")).toArray();
    m_performanceInsightsPanel->hint()->setText(insights.isEmpty() ? QString() : QString::number(insights.size()));
    if (!ready) {
        auto *setup = richLabel();
        setup->setText(QStringLiteral("<b>Connect Analytics</b> to measure campaigns. Visits, campaigns, landing pages and search terms are on the Analytics page."));
        m_performanceInsights->addWidget(setup);
    } else if (insights.isEmpty()) {
        auto *clear = richLabel();
        clear->setText(icon(QStringLiteral("win"), colors) + QStringLiteral("&nbsp; Nothing stands out in the last %1 days.").arg(m_performanceDays));
        m_performanceInsights->addWidget(clear);
    }
    for (const QJsonValue &entry : insights) {
        const QJsonObject insight = entry.toObject();
        auto *line = richLabel();
        line->setText(icon(text(insight, "level"), colors) + QStringLiteral("&nbsp; ") + escaped(text(insight, "title")) +
                      (text(insight, "detail").isEmpty() ? QString()
                           : QStringLiteral("<br><span style=\"color:%1; font-size:11px\">%2</span>").arg(colors.muted.name(), escaped(text(insight, "detail")))));
        m_performanceInsights->addWidget(line);
    }
    m_performanceInsights->addStretch(1);

    m_performanceAds->setRowCount(0);
    for (const QJsonValue &entry : m_performance.value(QStringLiteral("ads")).toArray()) {
        const QJsonObject ad = entry.toObject();
        const QString currency = text(ad, "currency");
        const auto cost = [&ad, &currency](const char *key) {
            const QJsonValue cents = ad.value(QLatin1String(key));
            return present(cents) ? money(cents, currency) : kDash;
        };
        const bool taggedLink = ad.value(QStringLiteral("tagged")).toBool(true);
        addRow(m_performanceAds,
               {text(ad, "name") + (taggedLink ? QString() : QStringLiteral("  ⚠")), channelLabel(text(ad, "channel")),
                fullCount(number(ad, "clicks")), countText(value(ad, "sessions")), shareText(value(ad, "clickToVisit")),
                countText(value(ad, "conversions")), cost("costPerSessionCents"), cost("costPerConversionCents")},
               text(ad, "id"), 2, {taggedLink ? QString() : QStringLiteral("The landing link has no campaign tags")});
    }
    if (m_performanceAds->rowCount() == 0)
        addRow(m_performanceAds, {ready ? QStringLiteral("No ads yet. Plan one with a tagged landing link.")
                                        : QStringLiteral("Connect Analytics to match ads to visits.")}, QString(), -1);
    fitTable(m_performanceAds, 10);

    const QJsonArray redditRows = m_performance.value(QStringLiteral("reddit")).toArray();
    m_performanceRedditPanel->setVisible(!redditRows.isEmpty());
    m_performanceReddit->setRowCount(0);
    for (const QJsonValue &entry : redditRows) {
        const QJsonObject row = entry.toObject();
        addRow(m_performanceReddit, {QStringLiteral("r/") + text(row, "subreddit"), fullCount(number(row, "threads")),
                                     fullCount(number(row, "replied")), fullCount(number(row, "sessions")),
                                     number(row, "sessions") > 0 ? shareText(value(row, "bounceRate")) : kDash,
                                     goals ? countText(value(row, "conversions")) : kDash},
               QString(), 1);
    }
    fitTable(m_performanceReddit, 10);
    renderReddit();
}

void MarketingFeature::renderChannelHealth()
{
    if (!m_healthKpis)
        return;
    const QJsonObject counts = m_summaryData.value(QStringLiteral("health")).toObject();
    const QJsonArray rows = m_summaryData.value(QStringLiteral("channels")).toArray();
    double audience = 0;
    double change = 0;
    bool tracked = false;
    for (const QJsonValue &value : rows) {
        const QJsonObject item = value.toObject();
        const QJsonObject health = item.value(QStringLiteral("health")).toObject();
        audience += number(item, "followers");
        if (present(health.value(QStringLiteral("followersChange")))) {
            tracked = true;
            change += number(health, "followersChange");
        }
    }
    const auto healthKpi = [&counts](const QString &label, const char *key, const QString &note) {
        KpiStrip::Kpi kpi;
        kpi.label = label;
        kpi.value = fullCount(number(counts, key));
        kpi.note = note;
        return kpi;
    };
    KpiStrip::Kpi audienceKpi;
    audienceKpi.label = QStringLiteral("Audience");
    audienceKpi.value = compactCount(qint64(audience));
    audienceKpi.tooltip = fullCount(audience);
    audienceKpi.delta = tracked ? (change > 0 ? QStringLiteral("▲ ") : change < 0 ? QStringLiteral("▼ ") : QStringLiteral("• ")) +
                                      compactCount(qint64(std::abs(change)))
                                : kDash;
    audienceKpi.tone = tracked && change > 0 ? 1 : tracked && change < 0 ? -1 : 0;
    audienceKpi.note = QStringLiteral("over 30 days");
    m_healthKpis->setKpis({healthKpi(QStringLiteral("On track"), "good", QStringLiteral("every check passes")),
                           healthKpi(QStringLiteral("Needs attention"), "attention", QStringLiteral("a check is failing")),
                           healthKpi(QStringLiteral("Stale numbers"), "stale", QStringLiteral("not updated in 14 days")),
                           healthKpi(QStringLiteral("Not set up"), "setup", QStringLiteral("no link, no numbers")),
                           audienceKpi});

    const Choices &filters = healthFilters();
    QStringList labels;
    for (const auto &filter : filters) {
        const int total = filter.first.isEmpty() ? int(rows.size())
                                                 : int(number(counts, filter.first.toLatin1().constData()));
        labels << QStringLiteral("%1 %2").arg(filter.second).arg(total);
    }
    setSegmentLabels(m_healthFilter, labels);
    const int filterIndex = qMax(0, m_healthFilter->checkedId());
    const QString state = filters.at(filterIndex).first;

    QStringList categories;
    for (const QJsonValue &value : rows) {
        const QString category = [&] {
            for (const QJsonValue &entry : std::as_const(m_channelCatalog)) {
                if (text(entry.toObject(), "id") == text(value.toObject(), "channel"))
                    return text(entry.toObject(), "category");
            }
            return QString();
        }();
        if (!category.isEmpty() && !categories.contains(category))
            categories << category;
    }
    std::sort(categories.begin(), categories.end(), [](const QString &a, const QString &b) {
        return QString::localeAwareCompare(a, b) < 0;
    });
    QString chosen = m_channelsCategory->currentData().toString();
    {
        const QSignalBlocker blocker(m_channelsCategory);
        m_channelsCategory->clear();
        m_channelsCategory->addItem(QStringLiteral("All categories"), QString());
        for (const QString &category : std::as_const(categories))
            m_channelsCategory->addItem(category, category);
        if (!categories.contains(chosen))
            chosen.clear();
        m_channelsCategory->setCurrentIndex(qMax(0, m_channelsCategory->findData(chosen)));
    }
    const QString query = m_channelsSearch->text().trimmed().toLower();

    QMap<QString, QList<QJsonObject>> groups;
    for (const QJsonValue &value : rows) {
        const QJsonObject item = value.toObject();
        const QString id = text(item, "channel");
        QJsonObject catalog;
        for (const QJsonValue &entry : std::as_const(m_channelCatalog)) {
            if (text(entry.toObject(), "id") == id)
                catalog = entry.toObject();
        }
        const QString category = text(catalog, "category");
        if (!state.isEmpty() && text(item.value(QStringLiteral("health")).toObject(), "state") != state)
            continue;
        if (!chosen.isEmpty() && category != chosen)
            continue;
        if (!query.isEmpty() && !(channelLabel(id) + QLatin1Char(' ') + text(catalog, "name") + QLatin1Char(' ') + category)
                                     .toLower().contains(query))
            continue;
        groups[category.isEmpty() ? QStringLiteral("Other") : category].append(item);
    }
    QList<QWidget *> sections;
    m_tileRefresh.removeIf([](const auto &it) { return it.value().isNull(); });
    const DashboardColors colors = DashboardColors::from(this);
    for (auto it = groups.cbegin(); it != groups.cend(); ++it) {
        const QJsonObject playbook = m_playbooks.value(it.key()).toObject();
        auto *head = new QHBoxLayout;
        head->setSpacing(10);
        auto *title = new QLabel(QStringLiteral("<span style=\"color:%1; font-weight:600\">%2</span> · %3")
                                     .arg(colors.fg.name(), escaped(it.key())).arg(it.value().size()));
        title->setTextFormat(Qt::RichText);
        head->addWidget(title);
        if (!playbook.isEmpty()) {
            auto *goal = hintLabel(text(playbook, "goal"));
            head->addWidget(goal, 1);
            QStringList tips;
            for (const QJsonValue &tip : playbook.value(QStringLiteral("tips")).toArray())
                tips << QStringLiteral("• ") + tip.toString();
            const int cadence = int(number(playbook, "cadenceDays"));
            auto *guide = linkButton(QStringLiteral("Playbook"),
                QStringLiteral("Track: %1.\nCadence: at least every %2.\n\n%3")
                    .arg(words(playbook.value(QStringLiteral("kpis"))).join(QStringLiteral(" · ")),
                         cadence == 1 ? QStringLiteral("day") : QStringLiteral("%1 days").arg(cadence),
                         tips.join(QLatin1Char('\n'))));
            connect(guide, &QPushButton::clicked, guide, [guide] {
                QMessageBox::information(guide->window(), guide->text(), guide->toolTip());
            });
            head->addWidget(guide);
        } else {
            head->addStretch(1);
        }
        auto *section = clearWidget();
        auto *sectionLayout = new QVBoxLayout(section);
        sectionLayout->setContentsMargins(0, 0, 0, 0);
        sectionLayout->setSpacing(8);
        sectionLayout->addLayout(head);
        auto *grid = new TileGrid(228, 8);
        QList<QWidget *> tiles;
        for (const QJsonObject &item : it.value())
            tiles << channelTile(item);
        grid->setTiles(tiles);
        sectionLayout->addWidget(grid);
        sectionLayout->addStretch(1);
        sections << section;
    }
    m_channelGroups->setTiles(sections);
    m_channelsEmpty->setVisible(groups.isEmpty());
    updateRefreshButtons();
}

void MarketingFeature::updateStaleChannels()
{
    if (m_refreshBatch)
        return;
    QStringList refresh;
    QStringList capture;
    for (const QJsonValue &value : m_summaryData.value(QStringLiteral("channels")).toArray()) {
        const QJsonObject item = value.toObject();
        const QJsonObject health = item.value(QStringLiteral("health")).toObject();
        const QString state = text(health, "state");
        if (state != QLatin1String("stale") && state != QLatin1String("attention") && state != QLatin1String("setup"))
            continue;
        bool fresh = false;
        for (const QJsonValue &check : health.value(QStringLiteral("checks")).toArray()) {
            if (text(check.toObject(), "id") == QLatin1String("fresh"))
                fresh = check.toObject().value(QStringLiteral("ok")).toBool();
        }
        const QString id = text(item, "channel");
        QString link;
        for (const ChannelRow &row : std::as_const(m_channelRows)) {
            if (row.id == id && row.profileUrl)
                link = row.profileUrl->text().trimmed();
        }
        if (fresh || (link.isEmpty() && id != QLatin1String("email")))
            continue;
        if (m_refreshableChannels.contains(id))
            refresh << id;
        else
            capture << id;
    }
    if (refresh.isEmpty() && capture.isEmpty())
        return setStatus(QStringLiteral("Every channel with a link was updated in the last 7 days."));
    if (refresh.isEmpty()) {
        captureWithExtension(capture.first());
        return;
    }
    m_refreshQueue = refresh;
    m_refreshBatchErrors.clear();
    m_refreshBatch = true;
    setStatus(QStringLiteral("Updating %1%2…").arg(plural(refresh.size(), QStringLiteral("channel"), QStringLiteral("channels")),
              capture.isEmpty() ? QString()
                  : QStringLiteral("; %1 more need the browser extension: press Update on their tiles").arg(capture.size())));
    advanceRefreshQueue();
}

bool MarketingFeature::adMatches(const QJsonObject &ad, bool withStatus) const
{
    if (!m_adChannel.isEmpty() && text(ad, "channel") != m_adChannel)
        return false;
    if (withStatus && m_adsStatus && m_adsStatus->checkedId() > 0 &&
        text(ad, "status") != adStatuses().value(m_adsStatus->checkedId() - 1).first)
        return false;
    const QString query = m_adsSearch ? m_adsSearch->text().trimmed().toLower() : QString();
    if (query.isEmpty())
        return true;
    QStringList fields{text(ad, "name"), text(ad, "campaign"), text(ad, "owner"), text(ad, "audience"),
                       text(ad, "headline"), text(ad, "platform")};
    fields << words(ad.value(QStringLiteral("tags")));
    for (const QString &field : std::as_const(fields)) {
        if (field.toLower().contains(query))
            return true;
    }
    return false;
}

QList<MarketingFeature::AdAttention> MarketingFeature::adAttention() const
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QList<AdAttention> attention;
    for (const QJsonValue &value : std::as_const(m_ads)) {
        const QJsonObject ad = value.toObject();
        const QString status = text(ad, "status");
        if (status == QLatin1String("archived"))
            continue;
        const QString adCurrency = text(ad, "currency");
        const QJsonObject pacing = ad.value(QStringLiteral("pacing")).toObject();
        const QString pace = text(pacing, "state");
        if (pace == QLatin1String("overspent"))
            attention.append({ad, QStringLiteral("Over budget"),
                              QStringLiteral("%1 spent of %2").arg(money(ad.value(QStringLiteral("spendCents")), adCurrency),
                                                                    money(pacing.value(QStringLiteral("totalBudgetCents")), adCurrency)), true});
        else if (pace == QLatin1String("over") || pace == QLatin1String("under"))
            attention.append({ad, pace == QLatin1String("over") ? QStringLiteral("Spending fast") : QStringLiteral("Spending slow"),
                              QStringLiteral("%1 spent · %2 expected by now").arg(money(ad.value(QStringLiteral("spendCents")), adCurrency),
                                                                                  money(pacing.value(QStringLiteral("expectedCents")), adCurrency))});
        const qint64 startsAt = qint64(number(ad, "startsAt"));
        const qint64 endsAt = qint64(number(ad, "endsAt"));
        const qint64 resultsAt = qint64(number(ad, "resultsAt"));
        if (status == QLatin1String("active") && endsAt > 0 && endsAt < now)
            attention.append({ad, QStringLiteral("Flight ended"), QStringLiteral("Ended %1 and still marked active").arg(relativeLabel(endsAt)), true});
        else if (status == QLatin1String("scheduled") && startsAt > 0 && startsAt < now)
            attention.append({ad, QStringLiteral("Start date passed"), QStringLiteral("Was due to start %1").arg(relativeLabel(startsAt)), true});
        else if (status == QLatin1String("active") && now - (resultsAt > 0 ? resultsAt : qint64(number(ad, "createdAt"))) > 7 * kDay)
            attention.append({ad, QStringLiteral("Results are stale"),
                              resultsAt > 0 ? QStringLiteral("Last logged %1").arg(relativeLabel(resultsAt)) : QStringLiteral("No results logged yet")});
    }
    return attention;
}

void MarketingFeature::renderAdsPage()
{
    if (!m_adsKpis)
        return;
    const DashboardColors colors = DashboardColors::from(this);
    const QJsonArray currencies = m_adsSummaryData.value(QStringLiteral("currencies")).toArray();
    QJsonObject totals;
    for (const QJsonValue &value : currencies) {
        if (text(value.toObject(), "currency") == m_adCurrency)
            totals = value.toObject();
    }
    if (totals.isEmpty() && !currencies.isEmpty())
        totals = currencies.first().toObject();
    const QString currency = totals.isEmpty() ? QStringLiteral("USD") : text(totals, "currency");
    {
        const QSignalBlocker blocker(m_adsCurrency);
        m_adsCurrency->clear();
        for (const QJsonValue &value : currencies) {
            const QJsonObject item = value.toObject();
            m_adsCurrency->addItem(QStringLiteral("%1 · %2").arg(text(item, "currency"),
                                       plural(qint64(number(item, "ads")), QStringLiteral("ad"), QStringLiteral("ads"))),
                                   text(item, "currency"));
        }
        m_adsCurrency->setCurrentIndex(qMax(0, m_adsCurrency->findData(currency)));
    }
    m_adsCurrency->setVisible(currencies.size() > 1);

    KpiStrip::Kpi spend;
    spend.label = QStringLiteral("Spend");
    spend.value = totals.isEmpty() ? kDash : compactMoney(number(totals, "spendCents"), currency);
    spend.tooltip = totals.isEmpty() ? QString() : money(totals.value(QStringLiteral("spendCents")), currency);
    spend.note = number(totals, "budgetCents") > 0
        ? QStringLiteral("%1% of %2 budget").arg(std::llround(number(totals, "spendCents") / number(totals, "budgetCents") * 100))
              .arg(compactMoney(number(totals, "budgetCents"), currency))
        : QStringLiteral("No budget set");
    KpiStrip::Kpi impressions;
    impressions.label = QStringLiteral("Impressions");
    impressions.value = compactCount(qint64(number(totals, "impressions")));
    impressions.tooltip = fullCount(number(totals, "impressions"));
    if (number(totals, "impressions") > 0)
        impressions.note = QStringLiteral("%1 per 1,000").arg(money(totals.value(QStringLiteral("cpmCents")), currency));
    KpiStrip::Kpi clicks;
    clicks.label = QStringLiteral("Clicks");
    clicks.value = compactCount(qint64(number(totals, "clicks")));
    clicks.tooltip = fullCount(number(totals, "clicks"));
    if (number(totals, "impressions") > 0)
        clicks.note = QStringLiteral("%1% CTR · %2 per click").arg(QString::number(number(totals, "ctr"), 'f', 2),
                                                                   money(totals.value(QStringLiteral("cpcCents")), currency));
    KpiStrip::Kpi conversions;
    conversions.label = QStringLiteral("Conversions");
    conversions.value = compactCount(qint64(number(totals, "conversions")));
    conversions.tooltip = fullCount(number(totals, "conversions"));
    if (number(totals, "conversions") > 0)
        conversions.note = QStringLiteral("%1 per conversion").arg(money(totals.value(QStringLiteral("cpaCents")), currency));
    KpiStrip::Kpi roas;
    roas.label = QStringLiteral("Return on ad spend");
    roas.value = number(totals, "spendCents") > 0 && number(totals, "revenueCents") > 0
        ? QString::number(number(totals, "roas"), 'f', 2) + QStringLiteral("×") : kDash;
    roas.note = number(totals, "revenueCents") > 0
        ? QStringLiteral("%1 revenue").arg(compactMoney(number(totals, "revenueCents"), currency))
        : QStringLiteral("No revenue logged");
    m_adsKpis->setKpis({spend, impressions, clicks, conversions, roas});

    m_adsSpendPanel->titleLabel()->setText(currencies.size() > 1 ? QStringLiteral("Spend by channel · %1").arg(currency)
                                                                 : QStringLiteral("Spend by channel"));
    const QJsonArray spendChannels = totals.value(QStringLiteral("channels")).toArray();
    double maxSpend = 1;
    for (const QJsonValue &value : spendChannels)
        maxSpend = qMax(maxSpend, number(value.toObject(), "spendCents"));
    QList<BarRows::Row> spendRows;
    QStringList spendIds;
    int selected = -1;
    for (const QJsonValue &value : spendChannels) {
        const QJsonObject item = value.toObject();
        const QString id = text(item, "channel");
        QStringList detail{plural(qint64(number(item, "ads")), QStringLiteral("ad"), QStringLiteral("ads"))};
        if (number(item, "impressions") > 0)
            detail << QStringLiteral("%1% CTR").arg(QString::number(number(item, "ctr"), 'f', 2));
        if (number(item, "conversions") > 0)
            detail << QStringLiteral("%1 CPA").arg(money(item.value(QStringLiteral("cpaCents")), currency));
        BarRows::Row row;
        row.label = channelLabel(id);
        row.value = number(item, "spendCents");
        row.max = maxSpend;
        row.text = compactMoney(number(item, "spendCents"), currency);
        row.color = colors.series[0];
        row.tooltip = QStringLiteral("%1: %2 spent · %3 impressions · %4 clicks · %5 conversions\n%6")
            .arg(row.label, money(item.value(QStringLiteral("spendCents")), currency),
                 fullCount(number(item, "impressions")), fullCount(number(item, "clicks")),
                 fullCount(number(item, "conversions")), detail.join(QStringLiteral(" · ")));
        if (id == m_adChannel)
            selected = int(spendRows.size());
        spendRows << row;
        spendIds << id;
    }
    m_adsSpend->setEmptyText(QStringLiteral("No ads yet. Plan the first one for any of your channels with New ad."));
    m_adsSpend->setRows(spendRows, 120, 84);
    m_adsSpend->setSelected(selected);
    m_adsSpend->setProperty("channels", spendIds);

    const QList<AdAttention> attention = adAttention();
    clearLayout(m_adsAttention);
    m_adsAttentionPanel->hint()->setText(attention.isEmpty() ? QString() : QStringLiteral("%1 to review").arg(attention.size()));
    if (attention.isEmpty()) {
        auto *clear = richLabel();
        clear->setText(icon(QStringLiteral("ok"), colors) + QStringLiteral("&nbsp; Every active ad is on pace and recently logged."));
        m_adsAttention->addWidget(clear);
    }
    for (int index = 0; index < qMin<qsizetype>(8, attention.size()); ++index) {
        const AdAttention &entry = attention.at(index);
        auto *line = richLabel();
        line->setText(QStringLiteral("%1&nbsp; <a style=\"color:%2; text-decoration:none\" href=\"ad:%3\"><b>%4</b> · %5</a><br><span style=\"color:%6; font-size:11px\">%7</span>")
                          .arg(icon(QStringLiteral("warning"), colors), colors.fg.name(), text(entry.ad, "id"), escaped(entry.label),
                               escaped(text(entry.ad, "name")), colors.muted.name(),
                               escaped(channelLabel(text(entry.ad, "channel")) + QStringLiteral(" · ") + entry.detail)));
        connect(line, &QLabel::linkActivated, this, [this](const QString &link) {
            const QJsonObject ad = byId(m_ads, link.mid(3));
            if (!ad.isEmpty())
                editAd(ad);
        });
        m_adsAttention->addWidget(line);
    }
    if (attention.size() > 8)
        m_adsAttention->addWidget(hintLabel(QStringLiteral("and %1 more").arg(attention.size() - 8)));
    m_adsAttention->addStretch(1);

    QStringList used;
    for (const QJsonValue &value : std::as_const(m_ads)) {
        const QString id = text(value.toObject(), "channel");
        if (!used.contains(id))
            used << id;
    }
    std::sort(used.begin(), used.end(), [this](const QString &a, const QString &b) {
        return QString::localeAwareCompare(channelLabel(a), channelLabel(b)) < 0;
    });
    if (!used.contains(m_adChannel))
        m_adChannel.clear();
    {
        const QSignalBlocker blocker(m_adsChannel);
        m_adsChannel->clear();
        m_adsChannel->addItem(QStringLiteral("All channels"), QString());
        for (const QString &id : std::as_const(used))
            m_adsChannel->addItem(channelLabel(id), id);
        m_adsChannel->setCurrentIndex(qMax(0, m_adsChannel->findData(m_adChannel)));
    }
    QHash<QString, int> statusCounts;
    int base = 0;
    for (const QJsonValue &value : std::as_const(m_ads)) {
        if (!adMatches(value.toObject(), false))
            continue;
        ++base;
        ++statusCounts[text(value.toObject(), "status")];
    }
    QStringList tabs{QStringLiteral("All %1").arg(base)};
    for (const auto &status : adStatuses())
        tabs << QStringLiteral("%1 %2").arg(status.second).arg(statusCounts.value(status.first));
    setSegmentLabels(m_adsStatus, tabs);
    m_adsPanel->hint()->setText(QStringLiteral("%1 · %2 active")
        .arg(plural(m_ads.size(), QStringLiteral("ad"), QStringLiteral("ads")))
        .arg(fullCount(number(m_adsSummaryData, "active"))));
    renderAds();
}

void MarketingFeature::renderGrowth()
{
    if (!m_growthKpis)
        return;
    const DashboardColors colors = DashboardColors::from(this);
    QHash<QString, QJsonObject> rates;
    for (const QJsonValue &value : m_funnelReport.value(QStringLiteral("rates")).toArray())
        rates.insert(text(value.toObject(), "key"), value.toObject());
    QHash<QString, QJsonObject> counts;
    for (const QJsonValue &value : m_funnelReport.value(QStringLiteral("counts")).toArray())
        counts.insert(text(value.toObject(), "key"), value.toObject());
    const qint64 weekStart = qint64(number(m_funnelReport, "weekStart"));
    KpiStrip::Kpi northStar;
    northStar.label = QStringLiteral("North star");
    northStar.value = countText(counts.value(QStringLiteral("northStar")).value(QStringLiteral("value")));
    std::tie(northStar.delta, northStar.tone) = changeDelta(counts.value(QStringLiteral("northStar")).value(QStringLiteral("change")), QString());
    northStar.note = weekStart > 0 ? QStringLiteral("week of %1").arg(weekLabel(weekStart)) : QStringLiteral("no week logged");
    QList<KpiStrip::Kpi> kpis{northStar};
    const QList<QPair<QString, QString>> rateKeys{
        {QStringLiteral("signupRate"), QStringLiteral("Signup rate")},
        {QStringLiteral("activationRate"), QStringLiteral("Activation rate")},
        {QStringLiteral("retentionRate"), QStringLiteral("Retention rate")},
        {QStringLiteral("referralRate"), QStringLiteral("Referral share")}};
    for (const auto &[key, label] : rateKeys) {
        const QJsonObject item = rates.value(key);
        KpiStrip::Kpi kpi;
        kpi.label = label;
        kpi.value = rateText(item.value(QStringLiteral("value")));
        if (present(item.value(QStringLiteral("denominator"))))
            kpi.tooltip = QStringLiteral("%1 of %2").arg(countText(item.value(QStringLiteral("numerator"))),
                                                         countText(item.value(QStringLiteral("denominator"))));
        std::tie(kpi.delta, kpi.tone) = changeDelta(item.value(QStringLiteral("change")), QStringLiteral(" pt"));
        if (item.value(QStringLiteral("small")).toBool() && present(item.value(QStringLiteral("denominator"))))
            kpi.note = QStringLiteral("base %1, too small").arg(countText(item.value(QStringLiteral("denominator"))));
        else if (number(item, "target") > 0)
            kpi.note = QStringLiteral("target %1").arg(rateText(item.value(QStringLiteral("target"))));
        kpis << kpi;
    }
    m_growthKpis->setKpis(kpis);

    const QString bottleneck = text(m_funnelReport, "bottleneck");
    const QList<std::tuple<QString, QString, QString>> stages{
        {QStringLiteral("visits"), QStringLiteral("Visits"), QString()},
        {QStringLiteral("signups"), QStringLiteral("Signups"), QStringLiteral("signupRate")},
        {QStringLiteral("activated"), QStringLiteral("Activated"), QStringLiteral("activationRate")},
        {QStringLiteral("referrals"), QStringLiteral("Referral signups"), QStringLiteral("referralRate")},
        {QStringLiteral("retained"), QStringLiteral("Retained"), QStringLiteral("retentionRate")}};
    double top = 1;
    for (const auto &stage : stages)
        top = qMax(top, number(counts.value(std::get<0>(stage)), "value"));
    QList<BarRows::Row> funnel;
    if (weekStart > 0) {
        for (const auto &[key, label, rateKey] : stages) {
            const QJsonValue value = counts.value(key).value(QStringLiteral("value"));
            const QJsonValue rate = rateKey.isEmpty() ? QJsonValue() : rates.value(rateKey).value(QStringLiteral("value"));
            BarRows::Row row;
            row.label = label;
            row.value = value.toDouble();
            row.max = top;
            row.text = countText(value) + (present(rate) ? QStringLiteral(" · ") + rateText(rate) : QString());
            row.color = !rateKey.isEmpty() && rateKey == bottleneck ? colors.warn : colors.series[0];
            row.tooltip = label + QStringLiteral(": ") + row.text;
            funnel << row;
        }
    }
    m_funnelBars->setEmptyText(QStringLiteral("No funnel week logged yet. Log week copies this week's numbers from your analytics."));
    m_funnelBars->setRows(funnel, 120, 110);
    const QString reason = text(m_funnelReport, "bottleneckReason");
    m_bottleneck->setText(reason.isEmpty() ? QString()
                              : (bottleneck.isEmpty() ? QString() : icon(QStringLiteral("warning"), colors) + QStringLiteral("&nbsp; ")) + escaped(reason));
    m_bottleneck->setVisible(!reason.isEmpty());
    m_funnelPanel->hint()->setText(text(m_funnelReport, "source").isEmpty() ? QString()
                                       : QStringLiteral("Source: ") + text(m_funnelReport, "source"));

    const QJsonObject attribution = m_settings.value(QStringLiteral("attribution")).toObject();
    const auto complete = [](QLineEdit *edit, const QStringList &values) {
        auto *completer = new QCompleter(values, edit);
        completer->setCaseSensitivity(Qt::CaseInsensitive);
        if (edit->completer())
            edit->completer()->deleteLater();
        edit->setCompleter(completer);
    };
    complete(m_linkSource, words(attribution.value(QStringLiteral("sources"))));
    complete(m_linkMedium, words(attribution.value(QStringLiteral("mediums"))));
    complete(m_linkCampaign, words(attribution.value(QStringLiteral("campaigns"))));
    m_linkUrl->setPlaceholderText(text(attribution, "landingUrl").isEmpty() ? QStringLiteral("https://…")
                                                                            : text(attribution, "landingUrl"));

    QStringList notes;
    const QJsonObject summary = m_experimentsSummary;
    if (summary.value(QStringLiteral("overLimit")).toBool())
        notes << QStringLiteral("%1 experiments are running. Run at most %2 at a time so each result can be read.")
                     .arg(int(number(summary, "running"))).arg(int(number(summary, "runningLimit")));
    for (const QJsonValue &id : summary.value(QStringLiteral("reviewsDue")).toArray()) {
        const QJsonObject item = byId(m_experiments, id.toString());
        if (!item.isEmpty())
            notes << QStringLiteral("Review due: %1%2.").arg(text(item, "title"),
                         item.value(QStringLiteral("ended")).toBool() ? QStringLiteral(" (its window has ended)") : QString());
    }
    if (number(summary, "unscored") > 0) {
        const int unscored = int(number(summary, "unscored"));
        notes << QStringLiteral("%1 open experiment%2 no score.").arg(unscored).arg(unscored == 1 ? QStringLiteral(" has") : QStringLiteral("s have"));
    }
    QStringList noteLines;
    for (const QString &note : std::as_const(notes))
        noteLines << icon(QStringLiteral("warning"), colors) + QStringLiteral("&nbsp; ") + escaped(note);
    m_experimentNotes->setText(noteLines.join(QStringLiteral("<br>")));
    m_experimentNotes->setVisible(!notes.isEmpty());

    QHash<QString, int> statusCounts;
    int open = 0;
    for (const QJsonValue &value : std::as_const(m_experiments)) {
        const QString status = text(value.toObject(), "status");
        ++statusCounts[status];
        open += status != QLatin1String("done") && status != QLatin1String("dropped") ? 1 : 0;
    }
    QStringList tabs{QStringLiteral("Open %1").arg(open)};
    for (const auto &status : experimentStatuses())
        tabs << QStringLiteral("%1 %2").arg(status.second).arg(statusCounts.value(status.first));
    setSegmentLabels(m_experimentStatus, tabs);
    const int tab = qMax(0, m_experimentStatus->checkedId());
    const QString wanted = tab > 0 ? experimentStatuses().at(tab - 1).first : QString();
    QList<QJsonObject> shown;
    for (const QJsonValue &value : std::as_const(m_experiments)) {
        const QJsonObject item = value.toObject();
        const QString status = text(item, "status");
        if (wanted.isEmpty() ? status != QLatin1String("done") && status != QLatin1String("dropped") : status == wanted)
            shown << item;
    }
    static const QStringList order{QStringLiteral("running"), QStringLiteral("review"), QStringLiteral("planned"),
                                   QStringLiteral("idea"), QStringLiteral("done"), QStringLiteral("dropped")};
    std::stable_sort(shown.begin(), shown.end(), [](const QJsonObject &a, const QJsonObject &b) {
        const auto rank = [](const QJsonObject &item) { return order.indexOf(text(item, "status")); };
        if (rank(a) != rank(b))
            return rank(a) < rank(b);
        if (number(a, "score") != number(b, "score"))
            return number(a, "score") > number(b, "score");
        return number(a, "updatedAt") > number(b, "updatedAt");
    });
    QSet<QString> topIds;
    for (const QJsonValue &id : summary.value(QStringLiteral("top")).toArray())
        topIds.insert(id.toString());
    const QString selected = rowId(m_experimentsTable);
    m_experimentsTable->setRowCount(0);
    for (const QJsonObject &item : std::as_const(shown)) {
        const auto score = [&item](const char *key) {
            return number(item, key) > 0 ? QString::number(int(number(item, key))) : kDash;
        };
        QStringList dates;
        if (number(item, "startsAt") > 0)
            dates << shortDate(qint64(number(item, "startsAt"))) + QStringLiteral(" – ") +
                         (number(item, "endsAt") > 0 ? shortDate(qint64(number(item, "endsAt"))) : QStringLiteral("open"));
        if (number(item, "reviewAt") > 0)
            dates << QStringLiteral("review ") + shortDate(qint64(number(item, "reviewAt")));
        const bool top = topIds.contains(text(item, "id")) && number(item, "score") > 0;
        const QString decision = text(item, "decision");
        addRow(m_experimentsTable,
               {text(item, "title"), choiceLabel(experimentStages(), text(item, "stage")),
                choiceLabel(experimentStatuses(), text(item, "status")) +
                    (decision.isEmpty() ? QString() : QStringLiteral(" · ") + choiceLabel(experimentDecisions(), decision)),
                score("impact"), score("confidence"), score("ease"),
                (number(item, "score") > 0 ? QString::number(int(number(item, "score"))) : kDash) + (top ? QStringLiteral(" ★") : QString()),
                dates.isEmpty() ? kDash : dates.join(QStringLiteral(" · "))},
               text(item, "id"), 3,
               {QStringList{text(item, "metric"), text(item, "owner"), text(item, "campaign")}
                    .filter(QRegularExpression(QStringLiteral("."))).join(QStringLiteral(" · ")),
                QString(), QString(), QString(), QString(), QString(), top ? QStringLiteral("Top three of the backlog") : QString()});
        const int row = m_experimentsTable->rowCount() - 1;
        m_experimentsTable->item(row, 7)->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        const QString status = text(item, "status");
        for (int column = 0; column < m_experimentsTable->columnCount(); ++column) {
            QTableWidgetItem *cell = m_experimentsTable->item(row, column);
            if (status == QLatin1String("done") || status == QLatin1String("dropped"))
                cell->setForeground(colors.muted);
            if (top && column == 6) {
                QFont font = cell->font();
                font.setBold(true);
                cell->setFont(font);
                cell->setForeground(colors.good);
            }
        }
        if (text(item, "id") == selected)
            m_experimentsTable->selectRow(row);
    }
    if (shown.isEmpty())
        addRow(m_experimentsTable, {QStringLiteral("No experiments in this view.")}, QString(), -1);
    fitTable(m_experimentsTable, 12);
    m_experimentsPanel->hint()->setText(QStringLiteral("%1 · %2 of %3 running")
        .arg(plural(m_experiments.size(), QStringLiteral("experiment"), QStringLiteral("experiments")))
        .arg(int(number(summary, "running")))
        .arg(number(summary, "runningLimit") > 0 ? int(number(summary, "runningLimit")) : 3));

    const QString selectedWeek = rowId(m_funnelTable);
    m_funnelTable->setRowCount(0);
    for (const QJsonValue &value : std::as_const(m_funnel)) {
        const QJsonObject week = value.toObject();
        const QJsonValue retained = week.value(QStringLiteral("retained"));
        const QJsonValue retainedBase = week.value(QStringLiteral("retainedBase"));
        addRow(m_funnelTable,
               {weekLabel(qint64(number(week, "weekStart"))),
                countText(week.value(QStringLiteral("northStar"))), countText(week.value(QStringLiteral("visits"))),
                countText(week.value(QStringLiteral("signups"))), countText(week.value(QStringLiteral("activated"))),
                present(retained) ? fullCount(retained.toDouble()) + (present(retainedBase) ? QStringLiteral(" / ") + fullCount(retainedBase.toDouble()) : QString())
                                  : kDash,
                countText(week.value(QStringLiteral("referrals"))),
                present(week.value(QStringLiteral("revenueCents"))) ? money(week.value(QStringLiteral("revenueCents")), text(week, "currency")) : kDash,
                text(week, "source")},
               text(week, "id"), 1);
        const int row = m_funnelTable->rowCount() - 1;
        m_funnelTable->item(row, 8)->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        if (text(week, "id") == selectedWeek)
            m_funnelTable->selectRow(row);
    }
    if (m_funnel.isEmpty())
        addRow(m_funnelTable, {QStringLiteral("No weeks logged. Copy this week's numbers from your analytics with Log week.")}, QString(), -1);
    fitTable(m_funnelTable, 12);
}

void MarketingFeature::tagChannelLink(const QString &id)
{
    if (id.isEmpty() || !m_linkUrl)
        return;
    reveal(m_linkUrl);
    m_linkSource->setText(id);
    m_linkMedium->setText(id == QLatin1String("email") || id == QLatin1String("newsletter")
                              ? QStringLiteral("email") : QStringLiteral("social"));
    m_linkUrl->setFocus();
}

void MarketingFeature::renderChannelDesk(const QString &id)
{
    if (!m_detailDesk)
        return;
    const DashboardColors colors = DashboardColors::from(this);
    const QString name = channelName(id, m_settings, m_channelCatalog);
    const QString dot = QString::fromUtf8(" \xC2\xB7 ");
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QList<QWidget *> tiles;
    const auto tile = [&](const QString &label, const QString &value, const QString &detail,
                          const QColor &tone = QColor(), const QString &tip = QString()) {
        auto *cell = richLabel();
        cell->setOpenExternalLinks(true);
        cell->setTextInteractionFlags(Qt::TextBrowserInteraction);
        cell->setText(QStringLiteral("<span style=\"color:%1; font-size:11px\">%2</span><br>"
                                     "<span style=\"color:%3; font-size:14px; font-weight:600\">%4</span>%5")
                          .arg(colors.muted.name(), escaped(label), (tone.isValid() ? tone : colors.fg).name(), value,
                               detail.isEmpty() ? QString()
                                   : QStringLiteral("<br><span style=\"color:%1; font-size:11px\">%2</span>")
                                         .arg(colors.muted.name(), detail)));
        cell->setToolTip(tip);
        tiles << cell;
    };

    QJsonObject settings;
    for (const QJsonValue &value : m_settings.value(QStringLiteral("channels")).toArray()) {
        if (text(value.toObject(), "id") == id)
            settings = value.toObject();
    }
    QJsonObject health;
    for (const QJsonValue &value : m_summaryData.value(QStringLiteral("channels")).toArray()) {
        if (text(value.toObject(), "channel") == id)
            health = value.toObject().value(QStringLiteral("health")).toObject();
    }

    const int cadence = qMax(1, int(number(health, "cadenceDays")));
    const qint64 lastPostAt = qint64(number(health, "lastPostAt"));
    if (lastPostAt > 0) {
        const qint64 due = lastPostAt + cadence * kDay;
        tile(QStringLiteral("Next post due"), escaped(relativeLabel(due)),
             QStringLiteral("every %1 · last %2").arg(plural(cadence, QStringLiteral("day"), QStringLiteral("days")),
                                                       escaped(shortDate(lastPostAt))),
             due < now ? colors.warn : colors.good);
    } else {
        tile(QStringLiteral("Next post due"), QStringLiteral("now"),
             QStringLiteral("nothing posted yet · aim every %1").arg(plural(cadence, QStringLiteral("day"), QStringLiteral("days"))),
             colors.warn);
    }

    QJsonObject queued;
    int drafts = 0;
    for (const QJsonValue &value : std::as_const(m_content)) {
        const QJsonObject entry = value.toObject();
        if (text(entry, "channel") != id)
            continue;
        const QString status = text(entry, "status");
        drafts += status == QLatin1String("draft") || status == QLatin1String("review") ? 1 : 0;
        if ((status == QLatin1String("queued") || status == QLatin1String("scheduled")) &&
            number(entry, "scheduledAt") >= now &&
            (queued.isEmpty() || number(entry, "scheduledAt") < number(queued, "scheduledAt")))
            queued = entry;
    }
    tile(QStringLiteral("Queued"),
         queued.isEmpty() ? QStringLiteral("nothing") : escaped(relativeLabel(qint64(number(queued, "scheduledAt")))),
         (queued.isEmpty() ? QString() : escaped(text(queued, "title").left(48)) + dot) +
             plural(drafts, QStringLiteral("draft"), QStringLiteral("drafts")),
         queued.isEmpty() ? colors.muted : QColor());

    int postCount = 0;
    double views = 0;
    double likes = 0;
    double replies = 0;
    QJsonObject best;
    for (const QJsonValue &value : std::as_const(m_posts)) {
        const QJsonObject post = value.toObject();
        if (text(post, "channel") != id)
            continue;
        ++postCount;
        views += number(post, "views");
        likes += number(post, "likes");
        replies += number(post, "replies");
        if (best.isEmpty() || number(post, "views") + number(post, "likes") > number(best, "views") + number(best, "likes"))
            best = post;
    }
    tile(QStringLiteral("Posted links"), fullCount(postCount),
         postCount ? QStringLiteral("%1 views · %2 likes · %3 replies")
                         .arg(compactCount(qint64(views)), compactCount(qint64(likes)), compactCount(qint64(replies)))
                   : QStringLiteral("Add link tracks a post's stats"),
         postCount ? QColor() : colors.muted);
    if (!best.isEmpty()) {
        const QString url = text(best, "url");
        const QString title = text(best, "title").isEmpty() ? QUrl(url).path() : text(best, "title");
        tile(QStringLiteral("Top post"),
             url.startsWith(QLatin1String("https://"))
                 ? QStringLiteral("<a style=\"color:%1; text-decoration:none\" href=\"%2\">%3</a>")
                       .arg(colors.primary.name(), escaped(url), escaped(title.left(40)))
                 : escaped(title.left(40)),
             QStringLiteral("%1 views · %2 likes").arg(countText(best.value(QStringLiteral("views"))),
                                                         countText(best.value(QStringLiteral("likes")))),
             QColor(), title);
    }

    QStringList live;
    QStringList waiting;
    QStringList tags;
    for (const QJsonValue &value : std::as_const(m_campaigns)) {
        const QJsonObject campaign = value.toObject();
        const QString status = text(campaign, "status");
        if (status != QLatin1String("active") && status != QLatin1String("planned"))
            continue;
        if (!campaign.value(QStringLiteral("channels")).toArray().contains(QJsonValue(id)))
            continue;
        live << text(campaign, "name");
        const QJsonObject stats = campaign.value(QStringLiteral("stats")).toObject();
        if (stats.value(QStringLiteral("uncoveredChannels")).toArray().contains(QJsonValue(id)))
            waiting << text(campaign, "name");
        for (const char *key : {"hashtags", "keywords"}) {
            for (const QJsonValue &tag : campaign.value(QLatin1String(key)).toArray()) {
                if (!tags.contains(tag.toString()))
                    tags << tag.toString();
            }
        }
    }
    tile(QStringLiteral("Campaigns"), live.isEmpty() ? QStringLiteral("none") : fullCount(live.size()),
         live.isEmpty() ? QStringLiteral("no live campaign lists %1").arg(escaped(name))
             : escaped(live.mid(0, 2).join(QStringLiteral(", "))) +
                   (waiting.isEmpty() ? QString()
                        : QStringLiteral("<br><span style=\"color:%1\">%2 waiting for a post here</span>")
                              .arg(colors.warn.name()).arg(waiting.size())),
         live.isEmpty() ? colors.muted : QColor(), live.join(QLatin1Char('\n')));
    if (!tags.isEmpty())
        tile(QStringLiteral("Talk about"), escaped(tags.first()),
             escaped(tags.mid(1, 5).join(QStringLiteral(" "))), QColor(), tags.join(QLatin1Char(' ')));

    QStringList hosts;
    for (const QJsonValue &value : std::as_const(m_channelCatalog)) {
        if (text(value.toObject(), "id") != id)
            continue;
        for (const QJsonValue &host : value.toObject().value(QStringLiteral("hosts")).toArray())
            hosts << host.toString();
    }
    QString profileHost = QUrl::fromUserInput(text(settings, "profileUrl")).host().toLower();
    if (profileHost.startsWith(QLatin1String("www.")))
        profileHost = profileHost.mid(4);
    if (!profileHost.isEmpty() && !hosts.contains(profileHost))
        hosts << profileHost;
    const auto matchesHost = [&hosts](QString host) {
        host = host.toLower();
        for (const QString &known : hosts) {
            if (host == known || host.endsWith(QLatin1Char('.') + known))
                return true;
        }
        return false;
    };
    double referrals = 0;
    qint64 lastReferral = 0;
    for (const QJsonValue &value : m_referringSites.value(QStringLiteral("board")).toArray()) {
        const QJsonObject row = value.toObject();
        if (!matchesHost(text(row, "host")))
            continue;
        referrals += number(row, "visits");
        lastReferral = qMax(lastReferral, qint64(number(row, "lastAt")));
    }
    if (referrals > 0)
        tile(QStringLiteral("Referrals"), fullCount(referrals),
             QStringLiteral("all time · last %1").arg(escaped(relativeLabel(lastReferral))), QColor(),
             QStringLiteral("Visits to this site whose referring page was on %1").arg(hosts.join(QStringLiteral(", "))));
    double tagged = 0;
    if (text(m_traffic, "status") == QLatin1String("ok")) {
        for (const QJsonValue &row : m_traffic.value(QStringLiteral("channels")).toArray()) {
            for (const QJsonValue &source : row.toObject().value(QStringLiteral("topSources")).toArray()) {
                const QString value = text(source.toObject(), "value").toLower();
                if (value == id || value.section(QLatin1Char('.'), 0, 0) == id || matchesHost(value))
                    tagged += number(source.toObject(), "count");
            }
        }
    }
    if (tagged > 0)
        tile(QStringLiteral("Analytics visits"), fullCount(tagged),
             QStringLiteral("source %1 · last %2 days").arg(escaped(id)).arg(m_rangeDays));

    int liveAds = 0;
    int adCount = 0;
    double clicks = 0;
    QMap<QString, double> spend;
    for (const QJsonValue &value : std::as_const(m_ads)) {
        const QJsonObject ad = value.toObject();
        if (text(ad, "channel") != id || text(ad, "status") == QLatin1String("archived"))
            continue;
        ++adCount;
        liveAds += text(ad, "status") == QLatin1String("active") ? 1 : 0;
        clicks += number(ad, "clicks");
        spend[text(ad, "currency")] += number(ad, "spendCents");
    }
    if (adCount > 0) {
        QStringList spent;
        for (auto it = spend.cbegin(); it != spend.cend(); ++it)
            spent << money(QJsonValue(it.value()), it.key());
        tile(QStringLiteral("Ads"), QStringLiteral("%1 live").arg(liveAds),
             QStringLiteral("of %1 · %2 spent · %3 clicks").arg(adCount).arg(escaped(spent.join(QStringLiteral(", "))),
                                                                   compactCount(qint64(clicks))));
    }

    if (id == QLatin1String("reddit")) {
        const QJsonObject summary = m_redditDesk.value(QStringLiteral("summary")).toObject();
        tile(QStringLiteral("Threads · 7D"),
             QStringLiteral("%1 logged").arg(fullCount(number(summary, "loggedThisWeek"))),
             QStringLiteral("%1 replied · %2 skipped").arg(fullCount(number(summary, "repliedThisWeek")),
                                                          fullCount(number(summary, "skippedThisWeek"))));
        tile(QStringLiteral("Waiting on you"), fullCount(number(summary, "open")),
             QStringLiteral("open threads · %1 monitored").arg(fullCount(number(summary, "monitored"))),
             number(summary, "open") > 0 ? colors.warn : colors.good);
        const int watched = int(m_redditDesk.value(QStringLiteral("subreddits")).toArray().size());
        tile(QStringLiteral("Rules on file"),
             QStringLiteral("%1 of %2").arg(fullCount(number(m_redditDesk, "rulesOnFile")), fullCount(watched)),
             QStringLiteral("%1 · %2").arg(plural(watched, QStringLiteral("subreddit"), QStringLiteral("subreddits")),
                                          plural(int(m_redditDesk.value(QStringLiteral("queries")).toArray().size()),
                                                 QStringLiteral("search"), QStringLiteral("searches"))),
             number(m_redditDesk, "rulesOnFile") < watched ? colors.warn : colors.good);
        QJsonObject top;
        for (const QJsonValue &value : m_performance.value(QStringLiteral("reddit")).toArray()) {
            if (number(value.toObject(), "sessions") > number(top, "sessions"))
                top = value.toObject();
        }
        if (!top.isEmpty())
            tile(QStringLiteral("Best subreddit"), QStringLiteral("r/") + escaped(text(top, "subreddit")),
                 QStringLiteral("%1 visits from tagged links").arg(fullCount(number(top, "sessions"))));
    }

    m_detailDesk->setTiles(tiles);
    m_detailDeskPanel->titleLabel()->setText(QStringLiteral("%1 desk").arg(name));
    m_detailDeskPanel->hint()->setText(QStringLiteral("What to do next and what it brought in"));
}

void MarketingFeature::renderReddit()
{
    if (!m_redditTable)
        return;
    const DashboardColors colors = DashboardColors::from(this);
    const QString link = QStringLiteral("<a style=\"color:%1; text-decoration:none\" href=\"%2\">%3</a>");
    const QString muted = QStringLiteral("<span style=\"color:%1\">%2</span>");
    const QString dot = QString::fromUtf8(" \xC2\xB7 ");
    QStringList subreddits;
    const QJsonArray watched = m_redditDesk.value(QStringLiteral("subreddits")).toArray();
    for (const QJsonValue &value : watched) {
        const QJsonObject item = value.toObject();
        subreddits << link.arg(colors.primary.name(), escaped(text(item, "url")), QStringLiteral("r/") + escaped(text(item, "name")));
    }
    m_redditSubreddits->setText(muted.arg(colors.muted.name(), QStringLiteral("Subreddits ")) +
        (subreddits.isEmpty() ? QStringLiteral("none yet") : subreddits.join(dot)));
    QStringList queries;
    for (const QJsonValue &value : m_redditDesk.value(QStringLiteral("queries")).toArray()) {
        const QJsonObject item = value.toObject();
        queries << link.arg(colors.primary.name(), escaped(text(item, "url")), escaped(text(item, "query")));
    }
    m_redditQueries->setText(muted.arg(colors.muted.name(), QStringLiteral("Searches ")) +
        (queries.isEmpty() ? QStringLiteral("none yet") : queries.join(dot)));
    QStringList accounts;
    for (const QJsonValue &value : m_redditDesk.value(QStringLiteral("accounts")).toArray()) {
        const QJsonObject item = value.toObject();
        accounts << link.arg(colors.primary.name(), escaped(text(item, "url")), QStringLiteral("u/") + escaped(text(item, "name")));
    }
    m_redditAccounts->setText(muted.arg(colors.muted.name(), QStringLiteral("Your accounts ")) +
        (accounts.isEmpty() ? QStringLiteral("none yet") : accounts.join(dot)));

    QHash<QString, QJsonObject> results;
    for (const QJsonValue &value : m_performance.value(QStringLiteral("reddit")).toArray())
        results.insert(text(value.toObject(), "subreddit").toLower(), value.toObject());
    QHash<QString, QJsonObject> watchedByName;
    for (const QJsonValue &value : watched)
        watchedByName.insert(text(value.toObject(), "name").toLower(), value.toObject());
    const QJsonObject ideas = m_redditDesk.value(QStringLiteral("ideas")).toObject();
    const QJsonArray places = ideas.value(QStringLiteral("subreddits")).toArray();
    const QString small = QStringLiteral("<span style=\"color:%1; font-size:11px\">%2</span>");
    const QString row = QStringLiteral("<div style=\"margin-bottom:5px\">%1</div>");
    const auto action = [&link, &colors](const QString &href, const QString &label) {
        return link.arg(colors.primary.name(), escaped(href), label);
    };
    m_redditIdeasPanel->hint()->setText(watched.isEmpty() ? QString()
        : QStringLiteral("%1 ready · rules on file for %2 of %3 · %4 discovered")
              .arg(fullCount(ideas.value(QStringLiteral("ready")).toArray().size()),
                   fullCount(number(m_redditDesk, "rulesOnFile")), fullCount(watched.size()),
                   fullCount(ideas.value(QStringLiteral("discover")).toArray().size())));
    const QString draftUrl = text(ideas, "draftUrl");
    QStringList placeLines;
    for (const QJsonValue &value : places) {
        const QJsonObject item = value.toObject();
        const QJsonObject watch = watchedByName.value(text(item, "name").toLower());
        QStringList blockers;
        for (const QJsonValue &blocker : item.value(QStringLiteral("blockers")).toArray()) {
            if (!blocker.toString().startsWith(QLatin1String("Read r/")))
                blockers << escaped(blocker.toString());
        }
        QString counts = QStringLiteral("%1 logged · %2 open · %3 replied")
                             .arg(fullCount(number(item, "threads")), fullCount(number(item, "open")),
                                  fullCount(number(item, "replied")));
        const QJsonObject result = results.value(text(item, "name").toLower());
        if (number(result, "sessions") > 0)
            counts += QStringLiteral(" · %1 visits").arg(fullCount(number(result, "sessions")));
        if (!blockers.isEmpty())
            counts += dot + QStringLiteral("<span style=\"color:%1\">%2</span>").arg(colors.warn.name(), blockers.join(dot));
        const QString prompt = text(item, "draftPrompt");
        if (!prompt.isEmpty() && !draftUrl.isEmpty())
            counts += dot + action(draftUrl + QString::fromLatin1(QUrl::toPercentEncoding(prompt)), QStringLiteral("Draft with ChatGPT ↗"));
        QString rules;
        if (item.value(QStringLiteral("rulesRead")).toBool()) {
            const QJsonArray entries = watch.value(QStringLiteral("rules")).toArray();
            QStringList titles;
            for (int index = 0; index < qMin<qsizetype>(3, entries.size()); ++index) {
                QString title = text(entries.at(index).toObject(), "title").simplified();
                if (title.size() > 38)
                    title = title.left(37) + QStringLiteral("…");
                titles << escaped(title.isEmpty() ? QStringLiteral("Untitled rule") : title);
            }
            rules = QStringLiteral("<span style=\"color:%1\">%2</span>%3%4%5")
                        .arg(colors.good.name(), plural(int(entries.size()), QStringLiteral("rule"), QStringLiteral("rules")),
                             titles.isEmpty() ? QString() : QStringLiteral(": ") + titles.join(dot) + (entries.size() > 3 ? QStringLiteral(" …") : QString()),
                             dot + QStringLiteral("read ") + relativeLabel(qint64(number(watch, "rulesReadAt"))),
                             dot + action(text(item, "rulesUrl"), QStringLiteral("Open ↗")));
        } else {
            rules = QStringLiteral("<span style=\"color:%1\">Rules not read</span>%2%3")
                        .arg(colors.warn.name(), dot + action(QStringLiteral("fm:read-rules"), QStringLiteral("Read with the extension")),
                             dot + action(text(item, "rulesUrl"), QStringLiteral("Open ↗")));
        }
        placeLines << row.arg(QStringLiteral("%1 %2<br>%3<br>%4")
                                  .arg(action(text(item, "url"), QStringLiteral("<b>r/%1</b>").arg(escaped(text(item, "name")))),
                                       small.arg(colors.muted.name(), escaped(text(item, "reason"))),
                                       small.arg(colors.muted.name(), counts), small.arg(colors.muted.name(), rules)));
    }
    m_redditPlaces->setText(placeLines.isEmpty()
        ? QStringLiteral("Add a subreddit, or one from Discovered, to get suggestions.")
        : placeLines.join(QString()));

    QStringList discoverLines;
    for (const QJsonValue &value : ideas.value(QStringLiteral("discover")).toArray()) {
        const QJsonObject item = value.toObject();
        discoverLines << row.arg(QStringLiteral("%1 %2<br>%3")
                                     .arg(action(text(item, "url"), QStringLiteral("<b>r/%1</b>").arg(escaped(text(item, "name")))),
                                          action(QStringLiteral("fm:add-subreddit:") + text(item, "name"), QStringLiteral("+ Add")),
                                          small.arg(colors.muted.name(), escaped(text(item, "reason")) + dot +
                                                    action(text(item, "rulesUrl"), QStringLiteral("Rules ↗")))));
    }
    QStringList finders;
    for (const QJsonValue &value : ideas.value(QStringLiteral("topics")).toArray()) {
        const QJsonObject item = value.toObject();
        if (finders.size() < 3 && !text(item, "communitiesUrl").isEmpty())
            finders << action(text(item, "communitiesUrl"), escaped(text(item, "topic")) + QStringLiteral(" ↗"));
    }
    m_redditDiscover->setText(
        (discoverLines.isEmpty() ? row.arg(QStringLiteral("Nothing new yet: log threads from your searches, or add topics and keywords."))
                                 : discoverLines.join(QString())) +
        (finders.isEmpty() ? QString() : small.arg(colors.muted.name(), QStringLiteral("Find communities on Reddit: ") + finders.join(dot))));

    QStringList topicLines;
    for (const QJsonValue &value : ideas.value(QStringLiteral("topics")).toArray()) {
        const QJsonObject item = value.toObject();
        const int threads = int(number(item, "threads"));
        QString counts = threads > 0
            ? QStringLiteral("%1 logged, %2 answered").arg(fullCount(threads), fullCount(number(item, "replied")))
            : text(item, "source") == QLatin1String("watch") ? QStringLiteral("watched, nothing logged")
            : text(item, "source") == QLatin1String("keywords") ? QStringLiteral("a tracked search keyword")
                                                                : QStringLiteral("nothing logged");
        const QStringList seen = words(item.value(QStringLiteral("subreddits")));
        if (!seen.isEmpty())
            counts += QStringLiteral(" in r/") + escaped(seen.join(QStringLiteral(", r/")));
        QString links = action(text(item, "searchUrl"), QStringLiteral("Search ↗"));
        if (text(item, "source") != QLatin1String("watch"))
            links += dot + action(QStringLiteral("fm:add-topic:") + QString::fromLatin1(QUrl::toPercentEncoding(text(item, "topic"))),
                                  QStringLiteral("+ Watch"));
        topicLines << row.arg(QStringLiteral("<b>%1</b><br>%2")
                                  .arg(escaped(text(item, "topic")), small.arg(colors.muted.name(), counts + dot + links)));
    }
    m_redditTopics->setText(topicLines.isEmpty()
        ? QStringLiteral("No topics yet: log a few threads or add searches to the watch list.")
        : topicLines.join(QString()));

    QHash<QString, int> statusCounts;
    int open = 0;
    for (const QJsonValue &value : std::as_const(m_redditThreads)) {
        const QString status = text(value.toObject(), "status");
        ++statusCounts[status];
        open += status == QLatin1String("to_review") || status == QLatin1String("drafting") ? 1 : 0;
    }
    int monitoredCount = 0;
    for (const QJsonValue &value : std::as_const(m_redditThreads))
        monitoredCount += value.toObject().value(QStringLiteral("monitored")).toBool() ? 1 : 0;
    QStringList tabs{QStringLiteral("Open %1").arg(open)};
    for (const auto &status : redditStatuses())
        tabs << QStringLiteral("%1 %2").arg(status.second).arg(statusCounts.value(status.first));
    tabs << QStringLiteral("Monitored %1").arg(monitoredCount);
    setSegmentLabels(m_redditStatus, tabs);
    const int tab = qMax(0, m_redditStatus->checkedId());
    const bool monitoredTab = tab == int(redditStatuses().size()) + 1;
    const QString wanted = tab > 0 && !monitoredTab ? redditStatuses().at(tab - 1).first : QString();
    QList<QJsonObject> shown;
    for (const QJsonValue &value : std::as_const(m_redditThreads)) {
        const QJsonObject item = value.toObject();
        const QString status = text(item, "status");
        if (monitoredTab ? item.value(QStringLiteral("monitored")).toBool()
            : wanted.isEmpty() ? status == QLatin1String("to_review") || status == QLatin1String("drafting") : status == wanted)
            shown << item;
    }
    std::stable_sort(shown.begin(), shown.end(), [](const QJsonObject &a, const QJsonObject &b) {
        const auto rank = [](const QJsonObject &item) { return choiceIndex(redditStatuses(), text(item, "status")); };
        if (rank(a) != rank(b))
            return rank(a) < rank(b);
        return number(a, "updatedAt") > number(b, "updatedAt");
    });
    const QString selected = rowId(m_redditTable);
    m_redditTable->setRowCount(0);
    for (const QJsonObject &item : std::as_const(shown)) {
        QStringList read;
        if (!text(item, "author").isEmpty())
            read << QStringLiteral("u/") + text(item, "author");
        if (present(item.value(QStringLiteral("comments"))))
            read << plural(qint64(number(item, "comments")), QStringLiteral("comment"), QStringLiteral("comments"));
        if (present(item.value(QStringLiteral("score"))))
            read << QStringLiteral("score ") + QLocale().toString(qint64(number(item, "score")));
        if (number(item, "postedAt") > 0)
            read << QStringLiteral("posted ") + relativeLabel(qint64(number(item, "postedAt")));
        if (text(item, "source") == QLatin1String("extension"))
            read << QStringLiteral("from the browser extension");
        QString monitored;
        if (item.value(QStringLiteral("monitored")).toBool()) {
            monitored = QStringLiteral(" · monitored");
            read << QStringLiteral("Monitored: ") + redditMonitorSummary(item);
        }
        QStringList tip{text(item, "url"),
                        QStringLiteral("Logged by %1 %2").arg(text(item, "createdBy").isEmpty() ? QStringLiteral("a teammate") : text(item, "createdBy"),
                                                              relativeLabel(qint64(number(item, "createdAt"))))};
        if (!read.isEmpty())
            tip << read.join(QStringLiteral(" · "));
        if (!text(item, "note").isEmpty())
            tip << text(item, "note");
        QStringList checks;
        for (const QJsonValue &check : item.value(QStringLiteral("linkChecks")).toArray())
            checks << QStringLiteral("⚠ ") + text(check.toObject(), "message");
        const bool rulesRead = item.value(QStringLiteral("rulesChecked")).toBool();
        const bool disclosed = item.value(QStringLiteral("disclosed")).toBool();
        QStringList activity;
        if (present(item.value(QStringLiteral("comments"))))
            activity << plural(qint64(number(item, "comments")), QStringLiteral("comment"), QStringLiteral("comments"));
        if (present(item.value(QStringLiteral("score"))))
            activity << plural(qint64(number(item, "score")), QStringLiteral("point"), QStringLiteral("points"));
        if (present(item.value(QStringLiteral("views")))) {
            const QString change = redditViewsChange(item);
            activity << plural(qint64(number(item, "views")), QStringLiteral("view"), QStringLiteral("views"))
                            + (change.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(change));
        }
        activity << redditCommentActivity(item);
        const qint64 readAt = qint64(number(item, "checkedAt"));
        activity << (readAt > 0 ? QStringLiteral("read ") + relativeLabel(readAt)
                                      + (text(item, "readAs").isEmpty() ? QString() : QStringLiteral(" as u/") + text(item, "readAs"))
                                : QStringLiteral("not read yet"));
        const QString threadId = text(item, "id");
        addRow(m_redditTable,
               {text(item, "title").isEmpty() ? text(item, "url") : text(item, "title"),
                QStringLiteral("r/") + text(item, "subreddit"),
                choiceLabel(redditStatuses(), text(item, "status")) + (text(item, "replyUrl").isEmpty() ? QString() : QStringLiteral(" · reply saved")) + monitored,
                activity.join(QStringLiteral(" · ")),
                QStringLiteral("%1 Rules read   %2 Disclosed").arg(rulesRead ? QStringLiteral("✓") : QStringLiteral("✗"),
                                                                    disclosed ? QStringLiteral("✓") : QStringLiteral("✗")),
                (checks.isEmpty() ? QString() : QStringLiteral("⚠ ")) + text(item, "taggedLink")},
               threadId, -1,
               {tip.join(QLatin1Char('\n')), QString(), text(item, "replyUrl"),
                readAt > 0 ? QStringLiteral("Read by the browser extension %1").arg(relativeLabel(readAt))
                           : QStringLiteral("Not read by the browser extension yet: press Update"),
                QString(), checks.join(QLatin1Char('\n'))});
        const int row = m_redditTable->rowCount() - 1;
        auto *update = linkButton(QStringLiteral("Update"),
            QStringLiteral("Opens the Reddit desk in your browser, where the ForkMesh extension reads this thread's comments, points and views in a background tab and updates this row."));
        update->setAccessibleName(QStringLiteral("Update %1 with the browser extension").arg(text(item, "title").isEmpty() ? text(item, "url") : text(item, "title")));
        connect(update, &QPushButton::clicked, this, [this, threadId] { updateRedditThreads(threadId); });
        m_redditTable->setCellWidget(row, 6, update);
        if (text(item, "status") == QLatin1String("skipped")) {
            for (int column = 0; column < m_redditTable->columnCount(); ++column) {
                if (QTableWidgetItem *cell = m_redditTable->item(row, column))
                    cell->setForeground(colors.muted);
            }
        }
        if (readAt <= 0)
            m_redditTable->item(row, 3)->setForeground(colors.muted);
        m_redditTable->item(row, 4)->setForeground(rulesRead && disclosed ? colors.good : colors.muted);
        if (text(item, "id") == selected)
            m_redditTable->selectRow(row);
    }
    if (shown.isEmpty())
        addRow(m_redditTable, {QStringLiteral("No threads in this view. Open a watch link, read the thread on Reddit, then log it here.")}, QString(), -1);
    fitTable(m_redditTable, 12);
    m_redditPanel->hint()->setText(QStringLiteral("%1 · %2 open")
        .arg(plural(m_redditThreads.size(), QStringLiteral("thread"), QStringLiteral("threads"))).arg(open));
}

void MarketingFeature::loadAppStore()
{
    const QString organization = m_organization->currentData().toString();
    if (!m_installed || organization.isEmpty())
        return;
    const int serial = ++m_appStoreSerial;
    if (m_appStoreOrg != organization) {
        m_appStore = {};
        m_appStoreError.clear();
    }
    m_appStoreOrg = organization;
    m_appStoreLoading = true;
    if (m_detailChannel == QLatin1String("app-store"))
        renderAppStore();
    request(QByteArrayLiteral("GET"), organizationPath(QStringLiteral("/app-store")), {},
            [this, serial, organization](bool ok, const QJsonObject &payload, const QString &error) {
        if (serial != m_appStoreSerial)
            return;
        m_appStoreLoading = false;
        if (organization != m_organization->currentData().toString())
            return;
        if (ok) {
            m_appStore = payload.value(QStringLiteral("appStore")).toObject();
            m_appStoreError.clear();
        } else {
            m_appStoreError = error.isEmpty() ? text(payload, "error") : error;
        }
        if (m_detailChannel == QLatin1String("app-store"))
            renderAppStore();
    });
}

void MarketingFeature::showAppStore()
{
    if (!m_detailAppStoreBox || !m_detailScroll || m_detailChannel != QLatin1String("app-store"))
        return;
    const QPointer<QWidget> target(m_detailAppStoreBox);
    const QPointer<QScrollArea> area(m_detailScroll);
    QTimer::singleShot(0, this, [target, area] {
        if (!target || !area || !area->widget())
            return;
        area->widget()->layout()->activate();
        const int top = target->mapTo(area->widget(), QPoint(0, 0)).y();
        area->verticalScrollBar()->setValue(qMax(0, top - 8));
    });
}

void MarketingFeature::renderAppStore()
{
    if (!m_appStoreKpis)
        return;
    const DashboardColors colors = DashboardColors::from(this);
    const QString dot = QString::fromUtf8(" \xC2\xB7 ");
    const QString muted = QStringLiteral("<span style=\"color:%1\">%2</span>");
    const auto dayAt = [](const QString &day) {
        const QDate date = QDate::fromString(day, Qt::ISODate);
        return date.isValid() ? utcMidnight(date) : qint64(0);
    };
    const auto dayLabel = [&dayAt](const QString &day) {
        const qint64 at = dayAt(day);
        return at > 0 ? weekLabel(at) : day;
    };
    const auto dateLabel = [](const QJsonValue &value) {
        return value.toDouble() > 0 ? weekLabel(qint64(value.toDouble())) : QString();
    };

    const QJsonObject lookup = m_appStore.value(QStringLiteral("lookup")).toObject();
    QString storeUrl = text(lookup, "url");
    if (!storeUrl.startsWith(QLatin1String("https://"))) {
        storeUrl.clear();
        for (const QJsonValue &value : m_settings.value(QStringLiteral("channels")).toArray()) {
            const QString profile = text(value.toObject(), "profileUrl").trimmed();
            if (text(value.toObject(), "id") == QLatin1String("app-store") &&
                profile.startsWith(QLatin1String("https://")))
                storeUrl = profile;
        }
    }
    m_appStoreOpen->setProperty("url", storeUrl);
    m_appStoreOpen->setVisible(!storeUrl.isEmpty());

    QStringList facts;
    if (!text(lookup, "name").isEmpty())
        facts << QStringLiteral("<b>%1</b>").arg(escaped(text(lookup, "name")));
    if (!text(lookup, "version").isEmpty())
        facts << QStringLiteral("Version %1").arg(escaped(text(lookup, "version")));
    if (number(lookup, "releasedAt") > 0)
        facts << QStringLiteral("released %1").arg(dateLabel(lookup.value(QStringLiteral("releasedAt"))));
    if (!text(lookup, "genre").isEmpty())
        facts << escaped(text(lookup, "genre"));
    if (!text(lookup, "price").isEmpty())
        facts << escaped(text(lookup, "price"));
    if (!text(lookup, "minimumOs").isEmpty())
        facts << QStringLiteral("iOS %1 or later").arg(escaped(text(lookup, "minimumOs")));
    QStringList lines;
    if (!facts.isEmpty())
        lines << facts.join(dot);
    const qint64 publishedAt = qint64(number(m_appStore, "publishedAt"));
    if (publishedAt > 0) {
        QString sent = QStringLiteral("Sent from App Store Connect %1").arg(relativeLabel(publishedAt));
        if (!text(m_appStore, "publishedBy").isEmpty())
            sent += QStringLiteral(" by ") + text(m_appStore, "publishedBy");
        lines << QStringLiteral("<span style=\"color:%1\">%2</span>")
                     .arg((m_appStore.value(QStringLiteral("fresh")).toBool() ? colors.muted : colors.warn).name(),
                          escaped(sent));
    }
    m_appStoreFacts->setText(lines.join(QStringLiteral("<br>")));
    m_appStoreFacts->setVisible(!lines.isEmpty());

    const QString lastDay = text(m_appStore, "lastDay");
    const bool hasDays = !lastDay.isEmpty();
    if (m_appStore.isEmpty() && m_appStoreLoading)
        m_appStoreSetup->setText(QStringLiteral("Reading the App Store numbers…"));
    else if (m_appStore.isEmpty() && !m_appStoreError.isEmpty())
        m_appStoreSetup->setText(QStringLiteral("Could not read the App Store numbers: %1").arg(m_appStoreError));
    else
        m_appStoreSetup->setText(QStringLiteral(
            "No downloads yet. Run native/ios/tools/asc_store_report.py --publish on the computer that holds the App Store Connect key; it sends each day's downloads here."));
    m_appStoreSetup->setVisible(!hasDays);

    QJsonObject window;
    for (const QJsonValue &value : m_appStore.value(QStringLiteral("windows")).toArray()) {
        if (int(number(value.toObject(), "days")) == m_appStoreDays)
            window = value.toObject();
    }
    const QString from = window.isEmpty() ? QString() : text(window, "from");
    const qint64 start = dayAt(from);
    const qint64 end = dayAt(lastDay);
    QList<QJsonObject> days;
    for (const QJsonValue &value : m_appStore.value(QStringLiteral("days")).toArray()) {
        const QJsonObject day = value.toObject();
        const qint64 at = dayAt(text(day, "day"));
        if (at > 0 && (start <= 0 || at >= start) && at <= end)
            days << day;
    }
    const QString rangeLabel = from.isEmpty() ? QString()
        : QStringLiteral("%1 – %2").arg(dayLabel(from), dayLabel(lastDay));

    QList<KpiStrip::Kpi> kpis;
    if (hasDays) {
        const QJsonObject previous = window.value(QStringLiteral("previous")).toObject();
        const bool comparable = window.value(QStringLiteral("comparable")).toBool();
        KpiStrip::Kpi downloads;
        downloads.label = QStringLiteral("Downloads");
        downloads.value = fullCount(number(window, "downloads"));
        downloads.tooltip = QStringLiteral("First-time downloads, %1").arg(rangeLabel);
        if (comparable) {
            std::tie(downloads.delta, downloads.tone) =
                windowDelta(window.value(QStringLiteral("downloads")), previous.value(QStringLiteral("downloads")));
            downloads.note = QStringLiteral("vs previous %1 days").arg(m_appStoreDays);
        } else {
            downloads.note = rangeLabel;
        }
        for (const QJsonObject &day : std::as_const(days))
            downloads.spark << number(day, "downloads");
        KpiStrip::Kpi redownloads;
        redownloads.label = QStringLiteral("Redownloads");
        redownloads.value = fullCount(number(window, "redownloads"));
        redownloads.tooltip = QStringLiteral("Downloads by people who had the app before, %1").arg(rangeLabel);
        redownloads.note = QStringLiteral("%1 days").arg(m_appStoreDays);
        KpiStrip::Kpi updates;
        updates.label = QStringLiteral("Updates");
        updates.value = fullCount(number(window, "updates"));
        updates.tooltip = QStringLiteral("App updates installed, %1").arg(rangeLabel);
        updates.note = QStringLiteral("%1 days").arg(m_appStoreDays);
        kpis << downloads << redownloads << updates;
    }
    if (hasDays || number(lookup, "ratings") > 0 || number(lookup, "rating") > 0) {
        KpiStrip::Kpi rating;
        rating.label = QStringLiteral("Rating");
        rating.value = number(lookup, "rating") > 0
            ? QString::number(number(lookup, "rating"), 'f', 1) + QChar(0x2605) : kDash;
        if (number(lookup, "versionRatings") > 0)
            rating.note = QStringLiteral("%1%2 this version").arg(QString::number(number(lookup, "versionRating"), 'f', 1))
                              .arg(QChar(0x2605));
        rating.tooltip = QStringLiteral("Average rating in the %1 App Store").arg(text(lookup, "country").isEmpty()
                                                                                    ? QStringLiteral("US") : text(lookup, "country"));
        KpiStrip::Kpi ratings;
        ratings.label = QStringLiteral("Ratings");
        ratings.value = fullCount(number(lookup, "ratings"));
        ratings.tooltip = rating.tooltip;
        kpis << rating << ratings;
    }
    if (hasDays) {
        const QJsonObject total = m_appStore.value(QStringLiteral("total")).toObject();
        KpiStrip::Kpi allTime;
        allTime.label = QStringLiteral("All-time downloads");
        allTime.value = fullCount(number(total, "downloads"));
        allTime.note = QStringLiteral("since %1").arg(dayLabel(text(m_appStore, "firstDay")));
        allTime.tooltip = QStringLiteral("%1 redownloads and %2 updates since %3")
                              .arg(fullCount(number(total, "redownloads")), fullCount(number(total, "updates")),
                                   dayLabel(text(m_appStore, "firstDay")));
        kpis << allTime;
        const QJsonObject best = m_appStore.value(QStringLiteral("bestDay")).toObject();
        if (!best.isEmpty()) {
            KpiStrip::Kpi bestDay;
            bestDay.label = QStringLiteral("Best day");
            bestDay.value = fullCount(number(best, "downloads"));
            bestDay.note = dayLabel(text(best, "day"));
            kpis << bestDay;
        }
    }
    m_appStoreKpis->setKpis(kpis);
    m_appStoreKpis->setVisible(!kpis.isEmpty());

    m_appStoreChartPanel->setVisible(hasDays);
    m_appStoreCountriesPanel->setVisible(hasDays);
    m_appStoreDevicesPanel->setVisible(hasDays);
    if (hasDays) {
        m_appStoreChartPanel->hint()->setText(rangeLabel);
        QList<LineChart::Series> series;
        const std::pair<const char *, QString> kinds[] = {
            {"downloads", QStringLiteral("Downloads")},
            {"redownloads", QStringLiteral("Redownloads")},
            {"updates", QStringLiteral("Updates")}};
        for (int index = 0; index < 3; ++index) {
            LineChart::Series line;
            line.name = kinds[index].second;
            line.color = colors.series[index];
            for (const QJsonObject &day : std::as_const(days))
                line.points.append({dayAt(text(day, "day")), number(day, kinds[index].first), true});
            series << line;
        }
        m_appStoreChart->setSeries(days.isEmpty() ? QList<LineChart::Series>() : series,
                                   start > 0 ? start : end - (m_appStoreDays - 1) * kDay, end,
                                   QStringLiteral("No downloads reported in this range"));

        const auto bars = [&colors](const QJsonArray &items, const char *label) {
            double peak = 0;
            for (const QJsonValue &value : items)
                peak = std::max(peak, number(value.toObject(), "downloads"));
            QList<BarRows::Row> rows;
            for (const QJsonValue &value : items) {
                const QJsonObject item = value.toObject();
                BarRows::Row row;
                row.label = text(item, label);
                row.value = number(item, "downloads");
                row.max = peak;
                row.text = compactCount(qint64(row.value));
                row.color = colors.series[0];
                row.tooltip = QStringLiteral("%1: %2 downloads").arg(row.label, fullCount(row.value));
                rows << row;
            }
            return rows;
        };
        m_appStoreCountries->setRows(bars(window.value(QStringLiteral("territories")).toArray(), "code"), 56);
        const int territories = int(number(window, "territoryCount"));
        m_appStoreCountriesPanel->hint()->setText(territories > 0
            ? plural(territories, QStringLiteral("country"), QStringLiteral("countries")) : QString());
        m_appStoreDevices->setRows(bars(window.value(QStringLiteral("devices")).toArray(), "name"), 96);
        m_appStoreDevicesPanel->hint()->setText(QStringLiteral("%1 days").arg(m_appStoreDays));
    }

    QStringList versions;
    for (const QJsonValue &value : m_appStore.value(QStringLiteral("versions")).toArray()) {
        const QJsonObject version = value.toObject();
        const QJsonObject status = version.value(QStringLiteral("status")).toObject();
        const QString tone = text(status, "tone");
        const QColor toneColor = tone == QLatin1String("good") ? colors.good
            : tone == QLatin1String("action") ? colors.warn : colors.muted;
        QStringList parts{QStringLiteral("<b>%1</b>").arg(escaped(text(version, "version")))};
        if (!text(version, "build").isEmpty())
            parts.first() += muted.arg(colors.muted.name(), QStringLiteral(" (build %1)").arg(escaped(text(version, "build"))));
        if (!text(status, "label").isEmpty())
            parts << QStringLiteral("<span style=\"color:%1\">%2</span>").arg(toneColor.name(), escaped(text(status, "label")));
        if (number(version, "createdAt") > 0)
            parts << muted.arg(colors.muted.name(), dateLabel(version.value(QStringLiteral("createdAt"))));
        versions << parts.join(dot);
    }
    m_appStoreVersions->setText(versions.isEmpty()
        ? muted.arg(colors.muted.name(), QStringLiteral("No versions sent yet"))
        : versions.join(QStringLiteral("<br>")));

    const QJsonArray reviews = m_appStore.value(QStringLiteral("reviews")).toArray();
    QStringList reviewLines;
    for (const QJsonValue &value : reviews) {
        const QJsonObject review = value.toObject();
        const int stars = qBound(0, int(number(review, "rating")), 5);
        QStringList head{QStringLiteral("<span style=\"color:%1\">%2</span>%3")
                             .arg(colors.warn.name(), QString(stars, QChar(0x2605)),
                                  muted.arg(colors.muted.name(), QString(5 - stars, QChar(0x2606))))};
        if (!text(review, "title").isEmpty())
            head << QStringLiteral("<b>%1</b>").arg(escaped(text(review, "title")));
        QStringList where;
        if (!text(review, "territory").isEmpty())
            where << escaped(text(review, "territory"));
        if (number(review, "createdAt") > 0)
            where << dateLabel(review.value(QStringLiteral("createdAt")));
        if (!where.isEmpty())
            head << muted.arg(colors.muted.name(), where.join(dot));
        QString body = text(review, "body");
        if (body.size() > 280)
            body = body.left(279).trimmed() + QChar(0x2026);
        reviewLines << head.join(QStringLiteral(" ")) +
                           (body.isEmpty() ? QString() : QStringLiteral("<br><span style=\"font-size:11px\">%1</span>").arg(escaped(body)));
    }
    m_appStoreReviews->setText(reviewLines.isEmpty()
        ? muted.arg(colors.muted.name(), QStringLiteral("No reviews yet"))
        : reviewLines.join(QStringLiteral("<br><br>")));
    m_appStoreReviewsPanel->hint()->setText(reviews.isEmpty() ? QString()
        : plural(reviews.size(), QStringLiteral("review"), QStringLiteral("reviews")));
}

void MarketingFeature::editExperiment(const QJsonObject &item)
{
    QDialog dialog(this);
    dialog.setWindowTitle(item.isEmpty() ? QStringLiteral("New experiment") : QStringLiteral("Edit experiment"));
    dialog.resize(640, 780);
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(dialogIntro(QStringLiteral(
        "Score each idea from 1 to 10 for impact, confidence and ease; the score is their product and the top three are highlighted.")));
    auto *form = new QFormLayout;
    auto *title = lineEdit(text(item, "title"));
    title->setMaxLength(200);
    auto *stage = choiceBox(experimentStages(), text(item, "stage"));
    auto *status = choiceBox(experimentStatuses(), text(item, "status"));
    const auto score = [&item](const char *key) {
        auto *spin = new QSpinBox;
        spin->setRange(0, 10);
        spin->setSpecialValueText(kDash);
        spin->setValue(int(number(item, key)));
        return spin;
    };
    auto *impact = score("impact");
    auto *confidence = score("confidence");
    auto *ease = score("ease");
    auto *scores = new QHBoxLayout;
    for (const auto &[label, spin] : {std::pair{QStringLiteral("Impact"), impact},
                                      std::pair{QStringLiteral("Confidence"), confidence},
                                      std::pair{QStringLiteral("Ease"), ease}}) {
        scores->addWidget(new QLabel(label));
        scores->addWidget(spin);
    }
    scores->addStretch(1);
    auto *hypothesis = new QPlainTextEdit(text(item, "hypothesis"));
    hypothesis->setFixedHeight(80);
    auto *audience = lineEdit(text(item, "audience"));
    auto *metric = lineEdit(text(item, "metric"));
    auto *baseline = lineEdit(text(item, "baseline"));
    auto *target = lineEdit(text(item, "target"));
    auto *owner = lineEdit(text(item, "owner"));
    auto *campaign = lineEdit(text(item, "campaign"));
    const OptionalDate starts = optionalDate(item, "startsAt");
    const OptionalDate ends = optionalDate(item, "endsAt");
    const OptionalDate review = optionalDate(item, "reviewAt");
    auto *decision = choiceBox(experimentDecisions(), text(item, "decision"));
    auto *result = new QPlainTextEdit(text(item, "result"));
    result->setFixedHeight(64);
    auto *notes = new QPlainTextEdit(text(item, "notes"));
    notes->setFixedHeight(64);
    form->addRow(QStringLiteral("Idea"), title);
    form->addRow(QStringLiteral("Funnel stage"), stage);
    form->addRow(QStringLiteral("Status"), status);
    form->addRow(QStringLiteral("Score"), scores);
    form->addRow(QStringLiteral("Hypothesis"), hypothesis);
    form->addRow(QStringLiteral("Audience"), audience);
    form->addRow(QStringLiteral("Success measure"), metric);
    form->addRow(QStringLiteral("Baseline"), baseline);
    form->addRow(QStringLiteral("Target"), target);
    form->addRow(QStringLiteral("Owner"), owner);
    form->addRow(QStringLiteral("Campaign"), campaign);
    form->addRow(QStringLiteral("Starts"), starts.row);
    form->addRow(QStringLiteral("Ends"), ends.row);
    form->addRow(QStringLiteral("Review on"), review.row);
    form->addRow(QStringLiteral("Decision"), decision);
    form->addRow(QStringLiteral("Result"), result);
    form->addRow(QStringLiteral("Notes"), notes);
    layout->addLayout(form);
    layout->addWidget(dialogButtons(&dialog, QStringLiteral("Save"), !item.isEmpty()));
    const int outcome = dialog.exec();
    const QString id = text(item, "id");
    if (outcome == 2) {
        if (QMessageBox::question(this, QStringLiteral("Delete experiment"), QStringLiteral("Delete this experiment?")) != QMessageBox::Yes)
            return;
        request(QByteArrayLiteral("DELETE"), organizationPath(QStringLiteral("/experiments/") + id), {},
                [this](bool ok, const QJsonObject &, const QString &error) {
            if (!ok)
                return setStatus(QStringLiteral("Could not delete the experiment: %1").arg(error), true);
            setStatus(QStringLiteral("Experiment deleted."));
            loadOrganization();
        });
        return;
    }
    if (outcome != QDialog::Accepted)
        return;
    const QJsonObject body{
        {QStringLiteral("title"), title->text()},
        {QStringLiteral("stage"), stage->currentData().toString()},
        {QStringLiteral("status"), status->currentData().toString()},
        {QStringLiteral("impact"), impact->value()},
        {QStringLiteral("confidence"), confidence->value()},
        {QStringLiteral("ease"), ease->value()},
        {QStringLiteral("hypothesis"), hypothesis->toPlainText()},
        {QStringLiteral("audience"), audience->text()},
        {QStringLiteral("metric"), metric->text()},
        {QStringLiteral("baseline"), baseline->text()},
        {QStringLiteral("target"), target->text()},
        {QStringLiteral("owner"), owner->text()},
        {QStringLiteral("campaign"), campaign->text()},
        {QStringLiteral("startsAt"), starts.value()},
        {QStringLiteral("endsAt"), ends.value()},
        {QStringLiteral("reviewAt"), review.value()},
        {QStringLiteral("decision"), decision->currentData().toString()},
        {QStringLiteral("result"), result->toPlainText()},
        {QStringLiteral("notes"), notes->toPlainText()},
    };
    request(id.isEmpty() ? QByteArrayLiteral("POST") : QByteArrayLiteral("PATCH"),
            organizationPath(QStringLiteral("/experiments") + (id.isEmpty() ? QString() : QLatin1Char('/') + id)), body,
            [this](bool ok, const QJsonObject &, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("The experiment was not saved: %1").arg(error), true);
        setStatus(QStringLiteral("Experiment saved."));
        loadOrganization();
    });
}

void MarketingFeature::editFunnelWeek(const QJsonObject &week)
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Log funnel week"));
    dialog.resize(560, 640);
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(dialogIntro(QStringLiteral(
        "Copy the week's numbers from your own analytics. Leave a field empty when nobody measured it: it shows as a dash, never as zero.")));
    auto *form = new QFormLayout;
    const qint64 start = number(week, "weekStart") > 0 ? qint64(number(week, "weekStart"))
                                                       : weekStartOf(QDateTime::currentMSecsSinceEpoch());
    auto *day = new QDateEdit(QDateTime::fromMSecsSinceEpoch(start, QTimeZone::UTC).date());
    day->setCalendarPopup(true);
    day->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    form->addRow(QStringLiteral("Week containing"), day);
    const QList<QPair<QString, QString>> fields{
        {QStringLiteral("northStar"), QStringLiteral("North star")},
        {QStringLiteral("visits"), QStringLiteral("Visits")},
        {QStringLiteral("signups"), QStringLiteral("Signups")},
        {QStringLiteral("activated"), QStringLiteral("Activated")},
        {QStringLiteral("referrals"), QStringLiteral("Referral signups")},
        {QStringLiteral("retained"), QStringLiteral("Retained")},
        {QStringLiteral("retainedBase"), QStringLiteral("…out of accounts eligible")}};
    QHash<QString, QLineEdit *> edits;
    for (const auto &[key, label] : fields) {
        auto *edit = optionalNumber(QJsonValue());
        edits.insert(key, edit);
        form->addRow(label, edit);
    }
    auto *revenue = optionalNumber(QJsonValue(), true);
    auto *currency = lineEdit(QStringLiteral("USD"));
    currency->setMaxLength(3);
    auto *revenueRow = new QHBoxLayout;
    revenueRow->addWidget(revenue, 1);
    revenueRow->addWidget(currency);
    form->addRow(QStringLiteral("Revenue"), revenueRow);
    auto *source = lineEdit();
    source->setPlaceholderText(QStringLiteral("Where the numbers came from"));
    form->addRow(QStringLiteral("Source"), source);
    auto *note = new QPlainTextEdit;
    note->setFixedHeight(64);
    form->addRow(QStringLiteral("Note"), note);
    layout->addLayout(form);
    auto *fillRow = new QHBoxLayout;
    auto *fill = new QPushButton(QStringLiteral("Fill visits from Analytics"));
    auto *fillNote = hintLabel();
    fillRow->addWidget(fill);
    fillRow->addWidget(fillNote, 1);
    layout->addLayout(fillRow);
    const auto load = [&](const QJsonObject &entry) {
        for (auto it = edits.cbegin(); it != edits.cend(); ++it) {
            const QJsonValue value = entry.value(it.key());
            it.value()->setText(present(value) ? QString::number(qint64(value.toDouble())) : QString());
        }
        const QJsonValue cents = entry.value(QStringLiteral("revenueCents"));
        revenue->setText(present(cents) ? QString::number(cents.toDouble() / 100.0) : QString());
        currency->setText(text(entry, "currency").isEmpty() ? QStringLiteral("USD") : text(entry, "currency"));
        source->setText(text(entry, "source"));
        note->setPlainText(text(entry, "note"));
    };
    const auto weekFor = [this](qint64 at) {
        const qint64 wanted = weekStartOf(at);
        for (const QJsonValue &value : std::as_const(m_funnel)) {
            if (qint64(number(value.toObject(), "weekStart")) == wanted)
                return value.toObject();
        }
        return QJsonObject();
    };
    load(week.isEmpty() ? weekFor(start) : week);
    connect(day, &QDateEdit::dateChanged, &dialog, [&](const QDate &date) {
        load(weekFor(utcMidnight(date)));
        fillNote->clear();
    });
    const QPointer<QDialog> guard(&dialog);
    connect(fill, &QPushButton::clicked, &dialog, [&, guard] {
        const qint64 from = weekStartOf(utcMidnight(day->date()));
        const qint64 to = qMin(from + 7 * kDay, QDateTime::currentMSecsSinceEpoch());
        fillNote->setText(QStringLiteral("Reading Analytics…"));
        request(QByteArrayLiteral("GET"),
                organizationPath(QStringLiteral("/performance")) + QStringLiteral("?from=%1&to=%2").arg(from).arg(to), {},
                [guard, from, to, fillNote, source, visits = edits.value(QStringLiteral("visits"))](
                    bool ok, const QJsonObject &payload, const QString &error) {
            if (!guard)
                return;
            if (!ok)
                return fillNote->setText(QStringLiteral("Could not read Analytics: %1").arg(error));
            const QJsonObject perf = payload.value(QStringLiteral("performance")).toObject();
            if (text(perf, "status") != QLatin1String("ok")) {
                const QJsonArray insights = perf.value(QStringLiteral("insights")).toArray();
                return fillNote->setText(insights.isEmpty() ? QStringLiteral("Analytics is not set up.")
                                                            : text(insights.first().toObject(), "title"));
            }
            const QJsonObject totals = perf.value(QStringLiteral("totals")).toObject();
            visits->setText(QString::number(qint64(number(totals, "sessions"))));
            if (source->text().trimmed().isEmpty())
                source->setText(QStringLiteral("Analytics: ") + text(perf.value(QStringLiteral("project")).toObject(), "name"));
            fillNote->setText(QStringLiteral("%1 visits from %2 visitors%3.")
                                  .arg(fullCount(number(totals, "sessions")), fullCount(number(totals, "visitors")),
                                       to < from + 7 * kDay ? QStringLiteral(" so far this week") : QString()));
        });
    });
    layout->addWidget(dialogButtons(&dialog, QStringLiteral("Save week"), false));
    if (dialog.exec() != QDialog::Accepted)
        return;
    QJsonObject body{
        {QStringLiteral("weekStart"), double(utcMidnight(day->date()))},
        {QStringLiteral("currency"), currency->text().trimmed().toUpper()},
        {QStringLiteral("source"), source->text()},
        {QStringLiteral("note"), note->toPlainText()},
    };
    for (auto it = edits.cbegin(); it != edits.cend(); ++it)
        body.insert(it.key(), optionalValue(it.value()));
    const QJsonValue amount = optionalValue(revenue);
    body.insert(QStringLiteral("revenueCents"), present(amount) ? QJsonValue(double(std::llround(amount.toDouble() * 100))) : QJsonValue::Null);
    request(QByteArrayLiteral("POST"), organizationPath(QStringLiteral("/funnel")), body,
            [this](bool ok, const QJsonObject &, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("The funnel week was not saved: %1").arg(error), true);
        setStatus(QStringLiteral("Funnel week saved."));
        loadOrganization();
    });
}

void MarketingFeature::deleteFunnelWeek(const QJsonObject &week)
{
    if (QMessageBox::question(this, QStringLiteral("Delete funnel week"),
                              QStringLiteral("Delete the funnel numbers for the week of %1?")
                                  .arg(weekLabel(qint64(number(week, "weekStart"))))) != QMessageBox::Yes)
        return;
    request(QByteArrayLiteral("DELETE"), organizationPath(QStringLiteral("/funnel/") + text(week, "id")), {},
            [this](bool ok, const QJsonObject &, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("Could not delete the funnel week: %1").arg(error), true);
        setStatus(QStringLiteral("Funnel week deleted."));
        loadOrganization();
    });
}

void MarketingFeature::buildLink()
{
    const QJsonObject body{
        {QStringLiteral("url"), m_linkUrl->text()},
        {QStringLiteral("source"), m_linkSource->text()},
        {QStringLiteral("medium"), m_linkMedium->text()},
        {QStringLiteral("campaign"), m_linkCampaign->text()},
        {QStringLiteral("content"), m_linkContent->text()},
    };
    request(QByteArrayLiteral("POST"), organizationPath(QStringLiteral("/links")), body,
            [this](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("Could not build the link: %1").arg(error), true);
        const DashboardColors colors = DashboardColors::from(this);
        m_linkOutput->setText(text(payload, "url"));
        m_linkOutput->show();
        m_linkCopy->show();
        m_linkChecks->setText(checkList(payload.value(QStringLiteral("checks")).toArray(), colors,
                                        QStringLiteral("Tagged, on the allowlists and landing on a tracked page.")));
        m_linkChecks->show();
    });
}

void MarketingFeature::showReport()
{
    setStatus(QStringLiteral("Building the weekly report…"));
    request(QByteArrayLiteral("GET"), organizationPath(QStringLiteral("/report")), {},
            [this](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("Could not build the report: %1").arg(error), true);
        setStatus(QStringLiteral("Weekly report ready."));
        const QString markdown = payload.value(QStringLiteral("markdown")).toString();
        QDialog dialog(this);
        dialog.setWindowTitle(QStringLiteral("Weekly marketing report"));
        dialog.resize(760, 680);
        auto *layout = new QVBoxLayout(&dialog);
        layout->addWidget(dialogIntro(QStringLiteral(
            "Scorecard, experiments, ads, content and the Reddit desk in Markdown, ready to paste into a plan, an issue or a special agent's run.")));
        auto *view = new QPlainTextEdit(markdown);
        view->setReadOnly(true);
        view->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        layout->addWidget(view, 1);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
        auto *copy = buttons->addButton(QStringLiteral("Copy report"), QDialogButtonBox::ActionRole);
        auto *save = buttons->addButton(QStringLiteral("Save .md…"), QDialogButtonBox::ActionRole);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        connect(copy, &QPushButton::clicked, &dialog, [this, markdown] {
            QGuiApplication::clipboard()->setText(markdown);
            setStatus(QStringLiteral("Report copied."));
        });
        connect(save, &QPushButton::clicked, &dialog, [this, &dialog, markdown] {
            const QString name = QStringLiteral("marketing-report-%1-%2.md")
                                     .arg(m_organization->currentData().toString(), utcDate(QDateTime::currentMSecsSinceEpoch()));
            const QString path = QFileDialog::getSaveFileName(&dialog, QStringLiteral("Save report"), name,
                                                              QStringLiteral("Markdown (*.md)"));
            if (path.isEmpty())
                return;
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(markdown.toUtf8()) < 0)
                return setStatus(QStringLiteral("Could not save the report to %1.").arg(path), true);
            setStatus(QStringLiteral("Report saved to %1.").arg(path));
        });
        layout->addWidget(buttons);
        dialog.exec();
    });
}

void MarketingFeature::editGoals()
{
    const QJsonObject analytics = m_settings.value(QStringLiteral("analytics")).toObject();
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Conversion goals"));
    dialog.resize(760, 520);
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(dialogIntro(QStringLiteral(
        "A visit converts when it records one of these Analytics events. Use the event your site already sends: a pageview of the signup or download page, or a button's product action. Up to 5 goals.")));
    auto *list = new QVBoxLayout;
    list->setSpacing(6);
    layout->addLayout(list);
    struct GoalRow {
        QWidget *widget;
        QLineEdit *label;
        QLineEdit *event;
        QLineEdit *property;
        QComboBox *operation;
        QLineEdit *value;
    };
    auto rows = std::make_shared<QList<GoalRow>>();
    const bool canSave = m_canManageSettings;
    const auto addGoal = [rows, list, canSave](const QJsonObject &goal) {
        if (rows->size() >= 5)
            return;
        auto *widget = new QWidget;
        auto *row = new QHBoxLayout(widget);
        row->setContentsMargins(0, 0, 0, 0);
        const auto field = [&row](const QString &value, const QString &placeholder, const QString &name) {
            auto *edit = lineEdit(value);
            edit->setMaxLength(200);
            edit->setPlaceholderText(placeholder);
            edit->setAccessibleName(name);
            row->addWidget(edit, 1);
            return edit;
        };
        GoalRow goalRow{widget, field(text(goal, "label"), QStringLiteral("Name, e.g. Signup"), QStringLiteral("Name")),
                        field(text(goal, "event"), QStringLiteral("Event, e.g. $pageview"), QStringLiteral("Event")),
                        field(text(goal, "property"), QStringLiteral("Property (optional)"), QStringLiteral("Property")),
                        new QComboBox, nullptr};
        goalRow.operation->addItem(QStringLiteral("is"), QStringLiteral("exact"));
        goalRow.operation->addItem(QStringLiteral("contains"), QStringLiteral("icontains"));
        goalRow.operation->setCurrentIndex(qMax(0, goalRow.operation->findData(text(goal, "operator"))));
        goalRow.operation->setAccessibleName(QStringLiteral("Match"));
        row->addWidget(goalRow.operation);
        goalRow.value = field(text(goal, "value"), QStringLiteral("Value, e.g. /signup"), QStringLiteral("Value"));
        auto *remove = new QPushButton(QStringLiteral("✕"));
        remove->setAccessibleName(QStringLiteral("Remove goal"));
        row->addWidget(remove);
        widget->setEnabled(canSave);
        list->addWidget(widget);
        rows->append(goalRow);
        QObject::connect(remove, &QPushButton::clicked, widget, [rows, widget] {
            rows->removeIf([widget](const GoalRow &entry) { return entry.widget == widget; });
            widget->deleteLater();
        });
    };
    for (const QJsonValue &goal : analytics.value(QStringLiteral("goals")).toArray())
        addGoal(goal.toObject());
    auto *presets = new QHBoxLayout;
    const QList<QPair<QString, QJsonObject>> presetGoals{
        {QStringLiteral("Add page goal"),
         QJsonObject{{QStringLiteral("label"), QStringLiteral("Signup page")}, {QStringLiteral("event"), QStringLiteral("$pageview")},
                     {QStringLiteral("property"), QStringLiteral("$pathname")}, {QStringLiteral("operator"), QStringLiteral("icontains")},
                     {QStringLiteral("value"), QStringLiteral("/signup")}}},
        {QStringLiteral("Add product action goal"),
         QJsonObject{{QStringLiteral("label"), QStringLiteral("Install click")}, {QStringLiteral("event"), QStringLiteral("forkmesh_product_action")},
                     {QStringLiteral("property"), QStringLiteral("action")}, {QStringLiteral("operator"), QStringLiteral("icontains")},
                     {QStringLiteral("value"), QStringLiteral("install")}}},
        {QStringLiteral("Add custom event goal"), QJsonObject{{QStringLiteral("operator"), QStringLiteral("exact")}}}};
    for (const auto &[label, goal] : presetGoals) {
        auto *add = new QPushButton(label);
        add->setEnabled(canSave);
        connect(add, &QPushButton::clicked, &dialog, [this, rows, addGoal, goal] {
            if (rows->size() >= 5)
                return setStatus(QStringLiteral("Up to 5 goals."), true);
            addGoal(goal);
        });
        presets->addWidget(add);
    }
    presets->addStretch(1);
    layout->addLayout(presets);
    auto *form = new QFormLayout;
    auto *project = new QComboBox;
    project->addItem(QStringLiteral("Site default, else the first project"), QString());
    for (const QJsonValue &value : m_performance.value(QStringLiteral("projects")).toArray())
        project->addItem(text(value.toObject(), "name"), text(value.toObject(), "id"));
    if (project->findData(text(analytics, "project")) < 0 && !text(analytics, "project").isEmpty())
        project->addItem(text(analytics, "project"), text(analytics, "project"));
    project->setCurrentIndex(qMax(0, project->findData(text(analytics, "project"))));
    project->setEnabled(canSave);
    auto *host = lineEdit(text(analytics, "host"));
    host->setMaxLength(253);
    host->setPlaceholderText(QStringLiteral("forkmesh.com"));
    host->setEnabled(canSave);
    form->addRow(QStringLiteral("Default Analytics project"), project);
    form->addRow(QStringLiteral("Your site's host"), host);
    layout->addLayout(form);
    layout->addWidget(dialogIntro(QStringLiteral(
        "Visits referred by your own host count as direct, not as referrals. Left empty, the host of the default landing page is used.")));
    if (!canSave)
        layout->addWidget(dialogIntro(QStringLiteral("Only organization owners and admins can change goals.")));
    layout->addStretch(1);
    auto *buttons = dialogButtons(&dialog, QStringLiteral("Save goals"), false);
    for (QAbstractButton *item : buttons->buttons()) {
        if (buttons->buttonRole(item) == QDialogButtonBox::AcceptRole)
            item->setEnabled(canSave);
    }
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted)
        return;
    QJsonArray goals;
    for (const GoalRow &row : std::as_const(*rows)) {
        if (row.event->text().trimmed().isEmpty())
            continue;
        goals.append(QJsonObject{{QStringLiteral("label"), row.label->text().trimmed()},
                                 {QStringLiteral("event"), row.event->text().trimmed()},
                                 {QStringLiteral("property"), row.property->text().trimmed()},
                                 {QStringLiteral("operator"), row.operation->currentData().toString()},
                                 {QStringLiteral("value"), row.value->text().trimmed()}});
    }
    m_performance = QJsonObject();
    saveSettings(QJsonObject{{QStringLiteral("analytics"),
                              QJsonObject{{QStringLiteral("goals"), goals},
                                          {QStringLiteral("project"), project->currentData().toString()},
                                          {QStringLiteral("host"), host->text().trimmed()}}}},
                 QStringLiteral("Goals saved."));
}

void MarketingFeature::saveSettings(const QJsonObject &body, const QString &done)
{
    request(QByteArrayLiteral("PUT"), organizationPath(QStringLiteral("/settings")), body,
            [this, done](bool ok, const QJsonObject &, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("Settings were not saved: %1").arg(error), true);
        setStatus(done);
        loadOrganization();
    });
}

void MarketingFeature::editRedditThread(const QJsonObject &item)
{
    QDialog dialog(this);
    dialog.setWindowTitle(item.isEmpty() ? QStringLiteral("Log thread") : QStringLiteral("Edit thread"));
    dialog.resize(640, 700);
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(dialogIntro(QStringLiteral(
        "Paste a Reddit thread you read in your browser. Each thread gets a forkmesh.com link tagged for its subreddit; nothing here posts, votes or sends anything to Reddit.")));
    auto *form = new QFormLayout;
    auto *url = lineEdit(text(item, "url"));
    url->setPlaceholderText(QStringLiteral("https://www.reddit.com/r/…/comments/…"));
    auto *title = lineEdit(text(item, "title"));
    auto *status = choiceBox(redditStatuses(), text(item, "status"));
    auto *landing = lineEdit(text(item, "landingPath").isEmpty() ? QStringLiteral("/") : text(item, "landingPath"));
    landing->setPlaceholderText(QStringLiteral("/"));
    auto *note = new QPlainTextEdit(text(item, "note"));
    note->setFixedHeight(72);
    auto *rules = new QCheckBox(QStringLiteral("Rules read: I read the subreddit's rules on self-promotion"));
    rules->setChecked(item.value(QStringLiteral("rulesChecked")).toBool());
    auto *disclosed = new QCheckBox(QStringLiteral("Disclosed: the reply says I work on ForkMesh"));
    disclosed->setChecked(item.value(QStringLiteral("disclosed")).toBool());
    auto *reply = lineEdit(text(item, "replyUrl"));
    reply->setPlaceholderText(QStringLiteral("Address of your reply, a comment in the same thread"));
    auto *monitor = new QCheckBox(QStringLiteral("Monitor views with the browser extension"));
    monitor->setChecked(item.value(QStringLiteral("monitored")).toBool());
    auto *comment = lineEdit(text(item, "commentUrl"));
    comment->setPlaceholderText(QStringLiteral("A comment in this thread to follow; your reply when empty"));
    form->addRow(QStringLiteral("Thread address"), url);
    form->addRow(QStringLiteral("Title"), title);
    form->addRow(QStringLiteral("Status"), status);
    form->addRow(QStringLiteral("Landing page"), landing);
    form->addRow(QStringLiteral("Note"), note);
    form->addRow(QString(), rules);
    form->addRow(QString(), disclosed);
    form->addRow(QStringLiteral("Your reply's address"), reply);
    form->addRow(QString(), monitor);
    form->addRow(QStringLiteral("Comment to monitor"), comment);
    if (item.value(QStringLiteral("monitored")).toBool())
        form->addRow(QStringLiteral("Last reading"), hintLabel(redditMonitorSummary(item)));
    layout->addLayout(form);
    layout->addWidget(dialogIntro(QStringLiteral(
        "A thread can be marked replied only after both checks are ticked and the address of your reply is saved. A monitored thread's views, points and comments are read by the ForkMesh browser extension in your signed-in browser: Reddit shows view counts only to the account that posted.")));
    layout->addStretch(1);
    layout->addWidget(dialogButtons(&dialog, QStringLiteral("Save thread"), !item.isEmpty()));
    const int outcome = dialog.exec();
    const QString id = text(item, "id");
    if (outcome == 2) {
        if (QMessageBox::question(this, QStringLiteral("Delete thread"), QStringLiteral("Delete this thread from the log?")) != QMessageBox::Yes)
            return;
        request(QByteArrayLiteral("DELETE"), organizationPath(QStringLiteral("/reddit/") + id), {},
                [this](bool ok, const QJsonObject &, const QString &error) {
            if (!ok)
                return setStatus(QStringLiteral("Could not delete the thread: %1").arg(error), true);
            setStatus(QStringLiteral("Thread deleted."));
            loadOrganization();
        });
        return;
    }
    if (outcome != QDialog::Accepted)
        return;
    const QJsonObject body{
        {QStringLiteral("url"), url->text().trimmed()},
        {QStringLiteral("title"), title->text()},
        {QStringLiteral("status"), status->currentData().toString()},
        {QStringLiteral("landingPath"), landing->text().trimmed()},
        {QStringLiteral("note"), note->toPlainText()},
        {QStringLiteral("replyUrl"), reply->text().trimmed()},
        {QStringLiteral("rulesChecked"), rules->isChecked()},
        {QStringLiteral("disclosed"), disclosed->isChecked()},
        {QStringLiteral("monitored"), monitor->isChecked()},
        {QStringLiteral("commentUrl"), comment->text().trimmed()},
    };
    request(id.isEmpty() ? QByteArrayLiteral("POST") : QByteArrayLiteral("PATCH"),
            organizationPath(QStringLiteral("/reddit") + (id.isEmpty() ? QString() : QLatin1Char('/') + id)), body,
            [this](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok) {
            const QJsonObject existing = byId(m_redditThreads, text(payload, "threadId"));
            if (text(payload, "error") == QLatin1String("reddit_thread_exists") && !existing.isEmpty()) {
                setStatus(QStringLiteral("That thread is already logged; here it is."));
                return editRedditThread(existing);
            }
            return setStatus(QStringLiteral("The thread was not saved: %1").arg(error), true);
        }
        setStatus(QStringLiteral("Thread saved."));
        loadOrganization();
    });
}

int MarketingFeature::fillPostedLinks(QTableWidget *table, const QString &channel)
{
    table->setRowCount(0);
    table->setIconSize(QSize(32, 32));
    table->setColumnHidden(1, !channel.isEmpty());
    for (const QJsonValue &value : std::as_const(m_posts)) {
        const QJsonObject post = value.toObject();
        if (!channel.isEmpty() && text(post, "channel") != channel)
            continue;
        const qint64 posted = qint64(number(post, "postedAt"));
        const qint64 checked = qint64(number(post, "checkedAt"));
        const QString url = text(post, "url");
        QString tip = url;
        if (!text(post, "note").isEmpty())
            tip += QStringLiteral("\n") + text(post, "note");
        const QJsonArray images = post.value(QStringLiteral("images")).toArray();
        if (!images.isEmpty())
            tip += QStringLiteral("\n") + plural(int(images.size()), QStringLiteral("image"), QStringLiteral("images"));
        addRow(table,
               {text(post, "title").isEmpty() ? url : text(post, "title"), channelLabel(text(post, "channel")),
                posted > 0 ? shortDate(posted) : kDash, countText(post.value(QStringLiteral("views"))),
                countText(post.value(QStringLiteral("likes"))), countText(post.value(QStringLiteral("replies"))),
                countText(post.value(QStringLiteral("reposts"))), checked > 0 ? relativeLabel(checked) : kDash,
                text(post, "source") == QLatin1String("extension") ? QStringLiteral("Extension")
                                                                   : text(post, "createdBy")},
               text(post, "id"), 3, {tip, QString(), posted > 0 ? timestamp(posted) : QString(), QString(),
                                     QString(), QString(), QString(), checked > 0 ? timestamp(checked) : QString()});
        const QString image = images.isEmpty() ? QString() : images.first().toString();
        if (image.isEmpty())
            continue;
        const QPixmap pixmap = m_postImages.value(image);
        if (!pixmap.isNull())
            table->item(table->rowCount() - 1, 0)->setIcon(QIcon(pixmap));
        else if (!m_postImages.contains(image))
            loadPostImage(image);
    }
    return table->rowCount();
}

void MarketingFeature::loadPostImage(const QString &url)
{
    const QUrl target(url);
    if (!m_network || m_postImagesPending.contains(url) || target.scheme() != QLatin1String("https") ||
        target.host() != QLatin1String("pbs.twimg.com"))
        return;
    m_postImagesPending.insert(url);
    QNetworkRequest request(target);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setRawHeader(QByteArrayLiteral("Accept"), QByteArrayLiteral("image/*"));
    request.setTransferTimeout(15000);
    QNetworkReply *reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, url] {
        reply->deleteLater();
        m_postImagesPending.remove(url);
        QImage image;
        if (reply->error() == QNetworkReply::NoError)
            image.loadFromData(reply->readAll());
        if (!image.isNull()) {
            image = image.scaled(64, 64, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
            image = image.copy(QRect((image.width() - 64) / 2, (image.height() - 64) / 2, 64, 64));
        }
        m_postImages.insert(url, QPixmap::fromImage(image));
        if (image.isNull() || m_postImagesRender)
            return;
        m_postImagesRender = true;
        QTimer::singleShot(200, this, &MarketingFeature::postImagesArrived);
    });
}

void MarketingFeature::postImagesArrived()
{
    m_postImagesRender = false;
    renderPostedLinks();
    if (m_detailPosts && !m_detailChannel.isEmpty()) {
        fillPostedLinks(m_detailPosts, m_detailChannel);
        fitTable(m_detailPosts, 12);
    }
}

void MarketingFeature::renderPostedLinks()
{
    if (!m_postsTable)
        return;
    const int rows = fillPostedLinks(m_postsTable, QString());
    m_postsPanel->hint()->setText(rows
        ? QStringLiteral("%1 · %2 views · %3 likes, replies and reposts")
              .arg(plural(rows, QStringLiteral("link"), QStringLiteral("links")),
                   fullCount(number(m_postsSummary, "views")), fullCount(number(m_postsSummary, "engagements")))
        : QStringLiteral("Nothing tracked yet"));
    m_postsTable->setVisible(rows > 0);
    m_postsEmpty->setVisible(rows == 0);
    fitTable(m_postsTable, 12);
}

void MarketingFeature::editPostedLink(const QJsonObject &item, const QString &channel)
{
    QDialog dialog(this);
    dialog.setWindowTitle(item.isEmpty() ? QStringLiteral("Add posted link") : QStringLiteral("Edit posted link"));
    dialog.resize(560, 480);
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(dialogIntro(QStringLiteral(
        "Paste the address of something you posted on any channel and type the numbers it shows. X posts are filled in by the ForkMesh browser extension: scroll your profile, then press Sync now in its popup.")));
    auto *form = new QFormLayout;
    auto *url = lineEdit(text(item, "url"));
    url->setPlaceholderText(QStringLiteral("https://…"));
    const QString current = item.isEmpty() ? channel : text(item, "channel");
    auto *channelBox = new QComboBox;
    channelBox->addItem(QStringLiteral("From the link's site"), QString());
    QSet<QString> listed;
    for (const QJsonValue &value : m_settings.value(QStringLiteral("channels")).toArray()) {
        const QString id = text(value.toObject(), "id");
        if (!id.isEmpty() && !listed.contains(id)) {
            listed.insert(id);
            channelBox->addItem(channelLabel(id), id);
        }
    }
    channelBox->insertSeparator(channelBox->count());
    for (const QJsonValue &value : std::as_const(m_channelCatalog)) {
        const QString id = text(value.toObject(), "id");
        if (!id.isEmpty() && !listed.contains(id)) {
            listed.insert(id);
            channelBox->addItem(channelLabel(id), id);
        }
    }
    if (!current.isEmpty() && channelBox->findData(current) < 0)
        channelBox->addItem(channelLabel(current), current);
    channelBox->setCurrentIndex(qMax(0, channelBox->findData(current)));
    auto *title = lineEdit(text(item, "title"));
    title->setPlaceholderText(QStringLiteral("What the post was about"));
    const QString linkedCampaign = text(item, "campaign");
    auto *campaignBox = new QComboBox;
    campaignBox->addItem(QStringLiteral("None"), QString());
    for (const QJsonValue &value : std::as_const(m_campaigns)) {
        const QString name = text(value.toObject(), "name");
        if (!name.isEmpty() && campaignBox->findData(name) < 0)
            campaignBox->addItem(name, name);
    }
    if (!linkedCampaign.isEmpty() && campaignBox->findData(linkedCampaign) < 0)
        campaignBox->addItem(linkedCampaign, linkedCampaign);
    campaignBox->setCurrentIndex(qMax(0, campaignBox->findData(linkedCampaign)));
    const OptionalDate posted = optionalDate(item, "postedAt");
    QList<QPair<const char *, QLineEdit *>> stats;
    for (const char *key : {"views", "likes", "replies", "reposts"})
        stats.append({key, optionalNumber(item.value(QLatin1String(key)))});
    auto *note = new QPlainTextEdit(text(item, "note"));
    note->setFixedHeight(64);
    form->addRow(QStringLiteral("Link"), url);
    form->addRow(QStringLiteral("Channel"), channelBox);
    form->addRow(QStringLiteral("Title"), title);
    form->addRow(QStringLiteral("Campaign"), campaignBox);
    form->addRow(QStringLiteral("Posted on"), posted.row);
    form->addRow(QStringLiteral("Views"), stats.at(0).second);
    form->addRow(QStringLiteral("Likes"), stats.at(1).second);
    form->addRow(QStringLiteral("Replies"), stats.at(2).second);
    form->addRow(QStringLiteral("Reposts"), stats.at(3).second);
    form->addRow(QStringLiteral("Note"), note);
    const qint64 checked = qint64(number(item, "checkedAt"));
    if (checked > 0) {
        const int readings = int(item.value(QStringLiteral("history")).toArray().size());
        form->addRow(QStringLiteral("Last reading"), hintLabel(QStringLiteral("%1 · %2")
            .arg(timestamp(checked), plural(readings, QStringLiteral("reading"), QStringLiteral("readings")))));
    }
    layout->addLayout(form);
    layout->addStretch(1);
    auto *buttons = dialogButtons(&dialog, item.isEmpty() ? QStringLiteral("Add link") : QStringLiteral("Save link"),
                                  !item.isEmpty());
    if (!text(item, "url").isEmpty()) {
        auto *open = buttons->addButton(QStringLiteral("Open ↗"), QDialogButtonBox::ActionRole);
        const QUrl link(text(item, "url"));
        connect(open, &QPushButton::clicked, &dialog, [link] { QDesktopServices::openUrl(link); });
    }
    layout->addWidget(buttons);
    const int outcome = dialog.exec();
    const QString id = text(item, "id");
    if (outcome == 2) {
        if (QMessageBox::question(this, QStringLiteral("Delete posted link"),
                                  QStringLiteral("Stop tracking this link and forget its stats?")) != QMessageBox::Yes)
            return;
        request(QByteArrayLiteral("DELETE"), organizationPath(QStringLiteral("/posts/") + id), {},
                [this](bool ok, const QJsonObject &, const QString &error) {
            if (!ok)
                return setStatus(QStringLiteral("The link was not deleted: %1").arg(error), true);
            setStatus(QStringLiteral("Link deleted."));
            loadOrganization();
        });
        return;
    }
    if (outcome != QDialog::Accepted)
        return;
    QJsonObject body{
        {QStringLiteral("url"), url->text().trimmed()},
        {QStringLiteral("title"), title->text()},
        {QStringLiteral("note"), note->toPlainText()},
        {QStringLiteral("campaign"), campaignBox->currentData().toString()},
        {QStringLiteral("postedAt"), posted.value()},
    };
    if (!channelBox->currentData().toString().isEmpty())
        body.insert(QStringLiteral("channel"), channelBox->currentData().toString());
    for (const auto &stat : std::as_const(stats)) {
        const QJsonValue value = optionalValue(stat.second);
        const QJsonValue before = item.value(QLatin1String(stat.first));
        if (present(value) && (!present(before) || qint64(before.toDouble()) != qint64(value.toDouble())))
            body.insert(QLatin1String(stat.first), value);
    }
    request(id.isEmpty() ? QByteArrayLiteral("POST") : QByteArrayLiteral("PATCH"),
            organizationPath(QStringLiteral("/posts") + (id.isEmpty() ? QString() : QLatin1Char('/') + id)), body,
            [this](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok) {
            const QJsonObject existing = byId(m_posts, text(payload, "postId"));
            if (text(payload, "error") == QLatin1String("post_exists") && !existing.isEmpty()) {
                setStatus(QStringLiteral("That link is already tracked; here it is."));
                return editPostedLink(existing);
            }
            if (text(payload, "error") == QLatin1String("post_channel_required"))
                return setStatus(QStringLiteral("The link was not saved: choose the channel it was posted on."), true);
            return setStatus(QStringLiteral("The link was not saved: %1").arg(error), true);
        }
        setStatus(QStringLiteral("Link saved."));
        loadOrganization();
    });
}

void MarketingFeature::monitorRedditThread()
{
    const QPointer<MarketingFeature> guard(this);
    bool accepted = false;
    const QString link = QInputDialog::getText(
        this, QStringLiteral("Monitor a Reddit thread"),
        QStringLiteral("Thread or comment link. The ForkMesh browser extension reads its views, points and comments in your signed-in browser:"),
        QLineEdit::Normal, QString(), &accepted).trimmed();
    if (!guard || !accepted || link.isEmpty())
        return;
    const QJsonObject body{
        {QStringLiteral("url"), link},
        {QStringLiteral("status"), QStringLiteral("to_review")},
        {QStringLiteral("monitored"), true},
    };
    request(QByteArrayLiteral("POST"), organizationPath(QStringLiteral("/reddit")), body,
            [this, link](bool ok, const QJsonObject &payload, const QString &error) {
        if (ok) {
            setStatus(QStringLiteral("Thread logged and monitored. The ForkMesh browser extension reads its views within the hour, or press Update to read them now."));
            return loadOrganization();
        }
        const QString threadId = text(payload, "threadId");
        if (text(payload, "error") != QLatin1String("reddit_thread_exists") || threadId.isEmpty())
            return setStatus(QStringLiteral("The thread was not monitored: %1").arg(error), true);
        QJsonObject patch{{QStringLiteral("monitored"), true}};
        if (link.contains(QRegularExpression(QStringLiteral("/comments/[A-Za-z0-9]+/[^/?#]+/[A-Za-z0-9]+"))))
            patch.insert(QStringLiteral("commentUrl"), link);
        request(QByteArrayLiteral("PATCH"), organizationPath(QStringLiteral("/reddit/") + threadId), patch,
                [this](bool patched, const QJsonObject &, const QString &patchError) {
            if (!patched)
                return setStatus(QStringLiteral("The thread was not monitored: %1").arg(patchError), true);
            setStatus(QStringLiteral("That thread was already logged; it is monitored now."));
            loadOrganization();
        });
    });
}

void MarketingFeature::updateRedditThreads(const QString &threadId)
{
    if (m_redditThreads.isEmpty())
        return setStatus(QStringLiteral("Log a thread first."), true);
    openWebDashboard(QStringLiteral("page=reddit&update=") +
                     (threadId.isEmpty() ? QStringLiteral("all") : QString::fromUtf8(QUrl::toPercentEncoding(threadId))));
    setStatus(threadId.isEmpty()
        ? QStringLiteral("Opened the Reddit desk in your browser: the ForkMesh extension reads each logged thread in a background tab, and this log updates as they are read.")
        : QStringLiteral("Opened the Reddit desk in your browser: the ForkMesh extension reads the thread in a background tab, and its row updates here when it is read."));
    if (!m_redditFollow) {
        m_redditFollow = new QTimer(this);
        m_redditFollow->setInterval(15 * 1000);
        connect(m_redditFollow, &QTimer::timeout, this, [this] {
            if (++m_redditFollowPolls > 16)
                return m_redditFollow->stop();
            m_changedReload->start();
        });
    }
    m_redditFollowPolls = 0;
    m_redditFollow->start();
}

void MarketingFeature::editRedditWatch()
{
    const QJsonObject watch = m_settings.value(QStringLiteral("reddit")).toObject();
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Reddit watch list"));
    dialog.resize(520, 520);
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(dialogIntro(QStringLiteral(
        "Up to 40 subreddits and 40 searches. Each one becomes a link that opens Reddit's newest posts in your own browser. "
        "Reddit shows view counts only to the account that posted, so list each of your accounts: the browser extension reads "
        "an account's threads, posts and comments whenever you are signed in to Reddit as it.")));
    auto *form = new QFormLayout;
    auto *subreddits = new QPlainTextEdit(words(watch.value(QStringLiteral("subreddits"))).join(QLatin1Char('\n')));
    subreddits->setPlaceholderText(QStringLiteral("selfhosted\ngit"));
    auto *queries = new QPlainTextEdit(words(watch.value(QStringLiteral("queries"))).join(QLatin1Char('\n')));
    queries->setPlaceholderText(QStringLiteral("git mirror"));
    auto *accounts = new QPlainTextEdit(words(watch.value(QStringLiteral("accounts"))).join(QLatin1Char('\n')));
    accounts->setPlaceholderText(QStringLiteral("forkmesh"));
    subreddits->setEnabled(m_canManageSettings);
    queries->setEnabled(m_canManageSettings);
    accounts->setEnabled(m_canManageSettings);
    form->addRow(QStringLiteral("Subreddits, one per line"), subreddits);
    form->addRow(QStringLiteral("Searches, one per line"), queries);
    form->addRow(QStringLiteral("Your Reddit accounts, up to 10"), accounts);
    layout->addLayout(form, 1);
    if (!m_canManageSettings)
        layout->addWidget(dialogIntro(QStringLiteral("Only organization owners and admins can change the watch list.")));
    auto *buttons = dialogButtons(&dialog, QStringLiteral("Save watch list"), false);
    for (QAbstractButton *item : buttons->buttons()) {
        if (buttons->buttonRole(item) == QDialogButtonBox::AcceptRole)
            item->setEnabled(m_canManageSettings);
    }
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const auto lines = [](const QPlainTextEdit *edit) {
        QJsonArray result;
        for (const QString &line : edit->toPlainText().split(QLatin1Char('\n'))) {
            if (!line.trimmed().isEmpty())
                result.append(line.trimmed());
        }
        return result;
    };
    saveSettings(QJsonObject{{QStringLiteral("reddit"), QJsonObject{{QStringLiteral("subreddits"), lines(subreddits)},
                                                                     {QStringLiteral("queries"), lines(queries)},
                                                                     {QStringLiteral("accounts"), lines(accounts)}}}},
                 QStringLiteral("Watch list saved."));
}

void MarketingFeature::copyRedditRules()
{
    const QJsonArray subreddits = m_redditDesk.value(QStringLiteral("subreddits")).toArray();
    const auto flat = [](const QString &value) { return value.simplified(); };
    QStringList lines{QStringLiteral("# Subreddit rules from the Reddit desk (copied %1)").arg(utcDate(QDateTime::currentMSecsSinceEpoch())), QString()};
    QStringList unread;
    bool any = false;
    for (const QJsonValue &value : subreddits) {
        const QJsonObject item = value.toObject();
        if (!item.value(QStringLiteral("rulesRead")).toBool()) {
            unread << QStringLiteral("r/") + text(item, "name");
            continue;
        }
        any = true;
        lines << QStringLiteral("## r/") + text(item, "name") << QString()
              << QStringLiteral("Read %1 from %2").arg(utcDate(qint64(number(item, "rulesReadAt"))), text(item, "rulesUrl")) << QString();
        const QJsonArray rules = item.value(QStringLiteral("rules")).toArray();
        if (rules.isEmpty())
            lines << QStringLiteral("The rules page listed no rules.");
        for (int index = 0; index < rules.size(); ++index) {
            const QJsonObject rule = rules.at(index).toObject();
            const QString title = flat(text(rule, "title"));
            lines << QStringLiteral("%1. %2%3").arg(index + 1).arg(title.isEmpty() ? QStringLiteral("Untitled rule") : title,
                                                               text(rule, "description").isEmpty() ? QString()
                                                                   : QStringLiteral(": ") + flat(text(rule, "description")));
        }
        lines << QString();
    }
    if (!any)
        return setStatus(QStringLiteral("No rules on file yet: press Read missing rules, or open a watched subreddit's rules page with the ForkMesh browser extension."), true);
    lines << QStringLiteral("Not read yet: ") + (unread.isEmpty() ? QStringLiteral("none") : unread.join(QStringLiteral(", ")));
    QGuiApplication::clipboard()->setText(lines.join(QLatin1Char('\n')));
    setStatus(QStringLiteral("Rules copied for the scout."));
}

void MarketingFeature::addRedditPlace()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Add to where to post next"));
    dialog.resize(440, 200);
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(dialogIntro(QStringLiteral(
        "A subreddit joins the watch list and gets ranked, drafted and its rules read. A topic becomes a watched search.")));
    auto *form = new QFormLayout;
    auto *kind = new QComboBox;
    kind->addItem(QStringLiteral("Subreddit"), QStringLiteral("subreddits"));
    kind->addItem(QStringLiteral("Topic to talk about"), QStringLiteral("queries"));
    auto *value = lineEdit();
    value->setPlaceholderText(QStringLiteral("r/selfhosted"));
    connect(kind, &QComboBox::activated, value, [kind, value](int) {
        value->setPlaceholderText(kind->currentData().toString() == QLatin1String("queries")
                                      ? QStringLiteral("self-hosted git") : QStringLiteral("r/selfhosted"));
    });
    form->addRow(QStringLiteral("Add"), kind);
    form->addRow(QStringLiteral("Name"), value);
    layout->addLayout(form);
    if (!m_canManageSettings)
        layout->addWidget(dialogIntro(QStringLiteral("Only organization owners and admins can change the watch list.")));
    auto *buttons = dialogButtons(&dialog, QStringLiteral("Add"), false);
    for (QAbstractButton *item : buttons->buttons()) {
        if (buttons->buttonRole(item) == QDialogButtonBox::AcceptRole)
            item->setEnabled(m_canManageSettings);
    }
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted)
        return;
    addRedditWatch(kind->currentData().toString(), value->text());
}

void MarketingFeature::addRedditWatch(const QString &kind, const QString &value)
{
    if (!m_canManageSettings)
        return setStatus(QStringLiteral("Only organization owners and admins can change the watch list."), true);
    QString entry = value.simplified();
    if (kind == QLatin1String("subreddits"))
        entry.remove(QRegularExpression(QStringLiteral("^(?:https?://(?:www\\.|old\\.)?reddit\\.com)?/?r/"),
                     QRegularExpression::CaseInsensitiveOption)).remove(QLatin1Char('/'));
    if (entry.isEmpty())
        return setStatus(kind == QLatin1String("subreddits") ? QStringLiteral("Type a subreddit name.")
                                                             : QStringLiteral("Type a topic to watch."), true);
    const QJsonObject watch = m_settings.value(QStringLiteral("reddit")).toObject();
    QJsonArray list = watch.value(kind).toArray();
    for (const QJsonValue &item : std::as_const(list)) {
        if (item.toString().compare(entry, Qt::CaseInsensitive) == 0)
            return setStatus(QStringLiteral("%1 is already on the watch list.")
                                 .arg(kind == QLatin1String("subreddits") ? QStringLiteral("r/") + entry : entry));
    }
    list.append(entry);
    saveSettings(QJsonObject{{QStringLiteral("reddit"), QJsonObject{{kind, list}}}},
                 kind == QLatin1String("subreddits") ? QStringLiteral("r/%1 added to where to post next.").arg(entry)
                                                     : QStringLiteral("“%1” added to the watched searches.").arg(entry));
}

void MarketingFeature::readRedditRules()
{
    const QJsonArray missing = m_redditDesk.value(QStringLiteral("ideas")).toObject().value(QStringLiteral("rulesMissing")).toArray();
    if (m_redditDesk.value(QStringLiteral("subreddits")).toArray().isEmpty())
        return setStatus(QStringLiteral("Add a subreddit first."), true);
    if (missing.isEmpty() && number(m_redditDesk, "rulesOnFile") >= m_redditDesk.value(QStringLiteral("subreddits")).toArray().size())
        return setStatus(QStringLiteral("Every watched subreddit's rules are on file."));
    openWebDashboard(QStringLiteral("page=reddit&rules=1"));
    setStatus(QStringLiteral("Opened the Reddit desk in your browser: the ForkMesh extension opens each missing rules page in one tab, reads it and saves the rules here. Press Refresh when it finishes."));
}

void MarketingFeature::openRedditLink(const QString &link)
{
    if (link == QLatin1String("fm:read-rules"))
        return readRedditRules();
    if (link.startsWith(QLatin1String("fm:add-subreddit:")))
        return addRedditWatch(QStringLiteral("subreddits"), link.mid(17));
    if (link.startsWith(QLatin1String("fm:add-topic:")))
        return addRedditWatch(QStringLiteral("queries"), QUrl::fromPercentEncoding(link.mid(13).toUtf8()));
    const QUrl url(link);
    if (url.isValid() && url.scheme() == QLatin1String("https"))
        QDesktopServices::openUrl(url);
}

void MarketingFeature::editXConnection()
{
    if (!m_canManageSettings)
        return setStatus(QStringLiteral("An owner or admin can connect X to post from here."), true);
    const QJsonObject x = m_settings.value(QStringLiteral("x")).toObject();
    const bool connected = x.value(QStringLiteral("connected")).toBool();
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Connect X (Twitter)"));
    dialog.resize(560, 620);
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(dialogIntro(QStringLiteral(
        "Owners and admins can post from Marketing with the organization's own X app. The keys are stored encrypted and are never shown again.")));
    auto *guideBox = new QFrame;
    guideBox->setFrameShape(QFrame::StyledPanel);
    auto *guideLayout = new QVBoxLayout(guideBox);
    auto *guideText = new QLabel(QStringLiteral(
        "<b>Let the browser extension walk you through it.</b> Marketing opens in your browser, and the ForkMesh extension opens X's developer console "
        "and guides you page by page: it fills in X's forms, presses the generate buttons, reads the four keys off the page and saves them here. "
        "You only sign in to X, tick X's agreement boxes and add API credits yourself. This page updates by itself once X is connected."));
    guideText->setTextFormat(Qt::RichText);
    guideText->setWordWrap(true);
    guideLayout->addWidget(guideText);
    auto *guide = new QPushButton(QStringLiteral("Guide me in the browser"));
    guide->setAccessibleName(QStringLiteral("Guide me through X setup in the browser"));
    connect(guide, &QPushButton::clicked, &dialog, [&dialog] { dialog.done(3); });
    guideLayout->addWidget(guide, 0, Qt::AlignLeft);
    layout->addWidget(guideBox);
    layout->addWidget(dialogIntro(QStringLiteral("Or do it yourself:")));
    auto *steps = new QLabel(QStringLiteral(
        "<ol style=\"margin-left:-20px\"><li>Open <a href=\"https://console.x.com/\">X's developer console</a> and create an app.</li>"
        "<li>Under User authentication settings, give the app <b>Read and write</b> permission.</li>"
        "<li>Under Keys and tokens, copy the API key and secret, then generate an Access token and secret for the account that should post.</li></ol>"));
    steps->setTextFormat(Qt::RichText);
    steps->setWordWrap(true);
    steps->setOpenExternalLinks(true);
    layout->addWidget(steps);
    if (connected) {
        QString state = QStringLiteral("Connected as @%1").arg(text(x, "username"));
        if (!text(x, "connectedBy").isEmpty())
            state += QStringLiteral(" by ") + text(x, "connectedBy");
        if (number(x, "connectedAt") > 0)
            state += QStringLiteral(" on ") + shortDate(qint64(number(x, "connectedAt")));
        layout->addWidget(dialogIntro(state + QStringLiteral(". Enter new keys to replace them.")));
    }
    auto *form = new QFormLayout;
    QList<QPair<QString, QLineEdit *>> keys;
    for (const auto &[key, label, secret] : {std::tuple{QStringLiteral("apiKey"), QStringLiteral("API key"), false},
                                             std::tuple{QStringLiteral("apiSecret"), QStringLiteral("API key secret"), true},
                                             std::tuple{QStringLiteral("accessToken"), QStringLiteral("Access token"), false},
                                             std::tuple{QStringLiteral("accessSecret"), QStringLiteral("Access token secret"), true}}) {
        auto *edit = new QLineEdit;
        if (secret)
            edit->setEchoMode(QLineEdit::Password);
        form->addRow(label, edit);
        keys.append({key, edit});
    }
    layout->addLayout(form);
    layout->addStretch(1);
    auto *buttons = dialogButtons(&dialog, QStringLiteral("Check and save"), connected);
    if (connected) {
        for (QAbstractButton *item : buttons->buttons()) {
            if (buttons->buttonRole(item) == QDialogButtonBox::DestructiveRole)
                item->setText(QStringLiteral("Disconnect"));
        }
    }
    layout->addWidget(buttons);
    const int outcome = dialog.exec();
    if (outcome == 3)
        return startXGuide();
    if (outcome == 2) {
        if (QMessageBox::question(this, QStringLiteral("Disconnect X"), QStringLiteral("Remove the X keys from this organization?")) != QMessageBox::Yes)
            return;
        request(QByteArrayLiteral("DELETE"), organizationPath(QStringLiteral("/x-connection")), {},
                [this](bool ok, const QJsonObject &payload, const QString &error) {
            if (!ok)
                return setStatus(QStringLiteral("Could not disconnect X: %1").arg(error), true);
            m_settings.insert(QStringLiteral("x"), payload.value(QStringLiteral("x")));
            setStatus(QStringLiteral("X disconnected."));
            renderX();
        });
        return;
    }
    if (outcome != QDialog::Accepted)
        return;
    QJsonObject body;
    for (const auto &[key, edit] : std::as_const(keys))
        body.insert(key, edit->text().trimmed());
    setStatus(QStringLiteral("Checking the keys with X…"));
    request(QByteArrayLiteral("POST"), organizationPath(QStringLiteral("/x-connection")), body,
            [this](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("X refused these keys: %1%2. Check the app has Read and write permission and regenerate the access token after changing it.")
                                 .arg(error, text(payload, "detail").isEmpty() ? QString() : QStringLiteral(" (%1)").arg(text(payload, "detail"))),
                             true);
        m_settings.insert(QStringLiteral("x"), payload.value(QStringLiteral("x")));
        setStatus(QStringLiteral("X connected as @%1.").arg(text(payload.value(QStringLiteral("x")).toObject(), "username")));
        renderX();
    });
}

void MarketingFeature::startXGuide()
{
    if (m_organization->currentData().toString().isEmpty())
        return;
    m_xGuidePath = organizationPath(QStringLiteral("/x-connection"));
    m_xGuideSince = qint64(number(m_settings.value(QStringLiteral("x")).toObject(), "connectedAt"));
    m_xGuidePolls = 0;
    if (!m_xGuideTimer) {
        m_xGuideTimer = new QTimer(this);
        m_xGuideTimer->setInterval(5000);
        connect(m_xGuideTimer, &QTimer::timeout, this, &MarketingFeature::pollXGuide);
    }
    m_xGuideTimer->start();
    openWebDashboard(QStringLiteral("xsetup=1"));
    setStatus(QStringLiteral("Opened Marketing in your browser: the ForkMesh extension guides you through X's developer console. "
                             "This page updates by itself once X is connected."));
}

void MarketingFeature::pollXGuide()
{
    if (++m_xGuidePolls > 360 || m_xGuidePath != organizationPath(QStringLiteral("/x-connection"))) {
        m_xGuideTimer->stop();
        return;
    }
    request(QByteArrayLiteral("GET"), m_xGuidePath, {},
            [this, path = m_xGuidePath](bool ok, const QJsonObject &payload, const QString &) {
        const QJsonObject x = payload.value(QStringLiteral("x")).toObject();
        if (!ok || path != m_xGuidePath || !m_xGuideTimer->isActive() || !x.value(QStringLiteral("connected")).toBool()
            || qint64(number(x, "connectedAt")) <= m_xGuideSince)
            return;
        m_xGuideTimer->stop();
        m_settings.insert(QStringLiteral("x"), x);
        setStatus(QStringLiteral("X connected as @%1 through the browser extension.").arg(text(x, "username")));
        renderX();
    });
}

void MarketingFeature::importKeywords()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Import keywords"));
    dialog.resize(680, 520);
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(dialogIntro(QStringLiteral(
        "Paste Search Console's Queries export (Top queries, Clicks, Impressions, CTR, Position), Bing's keyword report, or one keyword per line. Known keywords get a new reading; new ones are added.")));
    auto *paste = new QPlainTextEdit;
    paste->setPlaceholderText(QStringLiteral("Top queries,Clicks,Impressions,CTR,Position\ngit mirror,12,340,3.5%,8.2"));
    layout->addWidget(paste, 1);
    auto *form = new QHBoxLayout;
    auto *date = new QDateEdit(QDate::currentDate());
    date->setCalendarPopup(true);
    date->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    auto *source = lineEdit(QStringLiteral("Search Console"));
    source->setMaxLength(60);
    auto *limit = new QSpinBox;
    limit->setRange(1, 500);
    limit->setValue(100);
    form->addWidget(new QLabel(QStringLiteral("Reading date")));
    form->addWidget(date);
    form->addWidget(new QLabel(QStringLiteral("Source")));
    form->addWidget(source, 1);
    form->addWidget(new QLabel(QStringLiteral("Keep the top")));
    form->addWidget(limit);
    layout->addLayout(form);
    auto *preview = hintLabel();
    layout->addWidget(preview);
    const auto updatePreview = [paste, limit, preview] {
        const QList<QJsonObject> rows = parseKeywordImport(paste->toPlainText());
        preview->setText(rows.isEmpty() ? QString()
            : QStringLiteral("%1 found%2. First: “%3”.")
                  .arg(plural(rows.size(), QStringLiteral("keyword"), QStringLiteral("keywords")),
                       rows.size() > limit->value() ? QStringLiteral("; the top %1 by clicks and impressions will be imported").arg(limit->value()) : QString(),
                       text(rows.first(), "term")));
    };
    connect(paste, &QPlainTextEdit::textChanged, &dialog, updatePreview);
    connect(limit, &QSpinBox::valueChanged, &dialog, updatePreview);
    layout->addWidget(dialogButtons(&dialog, QStringLiteral("Import"), false));
    if (dialog.exec() != QDialog::Accepted)
        return;
    QList<QJsonObject> rows = parseKeywordImport(paste->toPlainText());
    std::stable_sort(rows.begin(), rows.end(), [](const QJsonObject &a, const QJsonObject &b) {
        if (number(a, "clicks") != number(b, "clicks"))
            return number(a, "clicks") > number(b, "clicks");
        return number(a, "impressions") > number(b, "impressions");
    });
    if (rows.size() > limit->value())
        rows = rows.mid(0, limit->value());
    if (rows.isEmpty())
        return setStatus(QStringLiteral("No keywords found in that text."), true);
    QJsonArray payloadRows;
    for (const QJsonObject &row : std::as_const(rows))
        payloadRows.append(row);
    const QJsonObject body{
        {QStringLiteral("rows"), payloadRows},
        {QStringLiteral("source"), source->text()},
        {QStringLiteral("observedAt"), double(utcMidnight(date->date()) + 12 * 3600 * 1000LL)},
    };
    request(QByteArrayLiteral("POST"), organizationPath(QStringLiteral("/keywords")), body,
            [this](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("Could not import keywords: %1").arg(error), true);
        setStatus(QStringLiteral("Imported keywords: %1 new, %2 updated%3.")
                      .arg(int(number(payload, "created"))).arg(int(number(payload, "updated")))
                      .arg(number(payload, "skipped") > 0 ? QStringLiteral(", %1 skipped").arg(int(number(payload, "skipped"))) : QString()));
        loadOrganization();
    });
}

void MarketingFeature::editTracking()
{
    const QJsonObject attribution = m_settings.value(QStringLiteral("attribution")).toObject();
    const QJsonObject targets = m_settings.value(QStringLiteral("targets")).toObject();
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Link tracking and funnel targets"));
    dialog.resize(620, 560);
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(dialogIntro(QStringLiteral(
        "The link builder and the Ads manager flag a link whose source, medium or campaign is not on these lists, or that lands on a path your analytics does not record. Copy the lists from your analytics configuration; separate values with commas.")));
    auto *form = new QFormLayout;
    QList<QPair<QString, QLineEdit *>> fields;
    for (const auto &[key, label, placeholder] : {std::tuple{QStringLiteral("sources"), QStringLiteral("Allowed sources"), QString()},
                                                  std::tuple{QStringLiteral("mediums"), QStringLiteral("Allowed mediums"), QString()},
                                                  std::tuple{QStringLiteral("campaigns"), QStringLiteral("Allowed campaigns"), QString()},
                                                  std::tuple{QStringLiteral("untrackedPaths"), QStringLiteral("Untracked paths"), QStringLiteral("/blog, /docs")},
                                                  std::tuple{QStringLiteral("domains"), QStringLiteral("Tracked domains"), QStringLiteral("example.com")},
                                                  std::tuple{QStringLiteral("landingUrl"), QStringLiteral("Default landing page"), QStringLiteral("https://…")}}) {
        const QJsonValue value = attribution.value(key);
        auto *edit = lineEdit(value.isArray() ? words(value).join(QStringLiteral(", ")) : value.toString());
        edit->setPlaceholderText(placeholder);
        edit->setEnabled(m_canManageSettings);
        form->addRow(label, edit);
        fields.append({key, edit});
    }
    layout->addLayout(form);
    layout->addWidget(dialogIntro(QStringLiteral(
        "Targets, in percent. The Growth page names the rate furthest below its target as the bottleneck; with no targets it names none.")));
    auto *targetForm = new QFormLayout;
    QList<QPair<QString, QDoubleSpinBox *>> rates;
    for (const auto &[key, label] : {std::pair{QStringLiteral("signupRate"), QStringLiteral("Signup")},
                                     std::pair{QStringLiteral("activationRate"), QStringLiteral("Activation")},
                                     std::pair{QStringLiteral("retentionRate"), QStringLiteral("Retention")},
                                     std::pair{QStringLiteral("referralRate"), QStringLiteral("Referral")}}) {
        auto *spin = new QDoubleSpinBox;
        spin->setRange(0, 100);
        spin->setDecimals(2);
        spin->setSuffix(QStringLiteral(" %"));
        spin->setSpecialValueText(QStringLiteral("No target"));
        spin->setValue(targets.value(key).toDouble());
        spin->setEnabled(m_canManageSettings);
        targetForm->addRow(label, spin);
        rates.append({key, spin});
    }
    layout->addLayout(targetForm);
    if (!m_canManageSettings)
        layout->addWidget(dialogIntro(QStringLiteral("Only organization owners and admins can change these settings.")));
    layout->addStretch(1);
    auto *buttons = dialogButtons(&dialog, QStringLiteral("Save"), false);
    for (QAbstractButton *item : buttons->buttons()) {
        if (buttons->buttonRole(item) == QDialogButtonBox::AcceptRole)
            item->setEnabled(m_canManageSettings);
    }
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted)
        return;
    QJsonObject attributionBody;
    for (const auto &[key, edit] : std::as_const(fields))
        attributionBody.insert(key, edit->text());
    QJsonObject targetBody;
    for (const auto &[key, spin] : std::as_const(rates))
        targetBody.insert(key, spin->value());
    saveSettings(QJsonObject{{QStringLiteral("attribution"), attributionBody}, {QStringLiteral("targets"), targetBody}},
                 QStringLiteral("Link tracking and targets saved."));
}

void MarketingFeature::loadAdvisor()
{
    if (!m_installed)
        return;
    const int serial = ++m_advisorSerial;
    if (m_advisorSummary)
        m_advisorSummary->setText(QStringLiteral("Reading…"));
    request(QByteArrayLiteral("GET"), organizationPath(QStringLiteral("/advisor")), {},
            [this, serial](bool ok, const QJsonObject &payload, const QString &error) {
        if (serial != m_advisorSerial)
            return;
        if (!ok) {
            m_advisorSummary->setText(QString());
            return setStatus(QStringLiteral("The advisor could not read your marketing: %1").arg(error), true);
        }
        m_advisor = payload.value(QStringLiteral("advisor")).toObject();
        m_adPlan = payload.value(QStringLiteral("adPlan")).toObject();
        renderAdvisor();
        renderAdPlan();
    });
}

QJsonObject MarketingFeature::advisorDone() const
{
    const QJsonObject stored = QJsonDocument::fromJson(
        QSettings().value(advisorDoneKey(m_organization->currentData().toString())).toByteArray()).object();
    QJsonObject fresh;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (auto it = stored.begin(); it != stored.end(); ++it)
        if (now - qint64(it.value().toDouble()) < 7 * kDay)
            fresh.insert(it.key(), it.value());
    return fresh;
}

void MarketingFeature::toggleAdvisorDone(const QString &id)
{
    QJsonObject done = advisorDone();
    if (done.contains(id))
        done.remove(id);
    else
        done.insert(id, double(QDateTime::currentMSecsSinceEpoch()));
    QSettings().setValue(advisorDoneKey(m_organization->currentData().toString()),
                         QJsonDocument(done).toJson(QJsonDocument::Compact));
    renderAdvisor();
}

void MarketingFeature::renderAdvisor()
{
    if (!m_advisorTable)
        return;
    const QJsonArray items = m_advisor.value(QStringLiteral("items")).toArray();
    const QJsonObject done = advisorDone();
    const int areaIndex = qBound(0, m_advisorArea->checkedId(), int(advisorAreas().size()) - 1);
    const QString area = advisorAreas().at(areaIndex).first;
    const QString priority = m_advisorPriority->currentData().toString();
    QHash<QString, int> counts;
    for (const QJsonValue &value : items)
        ++counts[text(value.toObject(), "area")];
    QStringList labels;
    for (const auto &[key, label] : advisorAreas())
        labels << (key.isEmpty() ? QStringLiteral("All %1").arg(items.size())
                                 : counts.value(key) ? QStringLiteral("%1 %2").arg(label).arg(counts.value(key)) : label);
    setSegmentLabels(m_advisorArea, labels);
    const QString previous = rowId(m_advisorTable);
    const QSignalBlocker blocker(m_advisorTable);
    m_advisorTable->setRowCount(0);
    const bool dark = palette().color(QPalette::Window).lightness() < 128;
    for (const QJsonValue &value : items) {
        const QJsonObject item = value.toObject();
        const QString id = text(item, "id");
        if ((!area.isEmpty() && text(item, "area") != area) || (!priority.isEmpty() && text(item, "priority") != priority)
            || (!m_advisorShowDone->isChecked() && done.contains(id)))
            continue;
        const QString level = text(item, "priority");
        QString areaLabel = text(item, "area");
        for (const auto &[key, label] : advisorAreas())
            if (key == areaLabel)
                areaLabel = label;
        addRow(m_advisorTable,
               {level == QLatin1String("high") ? QStringLiteral("High")
                : level == QLatin1String("medium") ? QStringLiteral("Medium") : QStringLiteral("Low"),
                text(item, "title"), text(item, "detail"), areaLabel,
                text(item, "channel").isEmpty() ? QString() : channelLabel(text(item, "channel"))},
               id, -1, {QString(), text(item, "title"), text(item, "detail")});
        const int row = m_advisorTable->rowCount() - 1;
        QTableWidgetItem *badge = m_advisorTable->item(row, 0);
        badge->setForeground(level == QLatin1String("high")     ? QColor(dark ? "#f0685f" : "#d1453b")
                             : level == QLatin1String("medium") ? QColor(dark ? "#e3a008" : "#b26a00")
                                                                : palette().color(QPalette::PlaceholderText));
        if (done.contains(id))
            for (int column = 0; column < m_advisorTable->columnCount(); ++column)
                m_advisorTable->item(row, column)->setForeground(palette().color(QPalette::PlaceholderText));
    }
    m_advisorTable->resizeRowsToContents();
    int select = 0;
    for (int row = 0; row < m_advisorTable->rowCount(); ++row)
        if (m_advisorTable->item(row, 0)->data(Qt::UserRole).toString() == previous)
            select = row;
    if (m_advisorTable->rowCount())
        m_advisorTable->setCurrentCell(select, 0);
    const QJsonObject byPriority = m_advisor.value(QStringLiteral("byPriority")).toObject();
    m_advisorSummary->setText(m_advisor.isEmpty() ? QStringLiteral("Reading…")
                              : items.isEmpty()    ? QStringLiteral("Nothing to do: everything is on track")
                                                   : QStringLiteral("%1 to-dos · %2 high · %3 medium")
                                                         .arg(int(number(m_advisor, "total")))
                                                         .arg(int(number(byPriority, "high")))
                                                         .arg(int(number(byPriority, "medium"))));
    const QString current = rowId(m_advisorTable);
    QJsonObject selected;
    for (const QJsonValue &value : items)
        if (!current.isEmpty() && text(value.toObject(), "id") == current)
            selected = value.toObject();
    m_advisorRun->setEnabled(!selected.value(QStringLiteral("action")).toObject().isEmpty());
    m_advisorDone->setEnabled(!selected.isEmpty());
    m_advisorDone->setText(done.contains(current) ? QStringLiteral("Undo done") : QStringLiteral("Mark done"));
}

void MarketingFeature::renderAdPlan()
{
    if (!m_adPlanTable)
        return;
    const QJsonArray items = m_adPlan.value(QStringLiteral("recommendations")).toArray();
    const int previous = m_adPlanTable->currentRow();
    const QString previousId = rowId(m_adPlanTable);
    {
        const QSignalBlocker blocker(m_adPlanTable);
        m_adPlanTable->setRowCount(0);
        for (const QJsonValue &value : items) {
            const QJsonObject item = value.toObject();
            const QJsonObject budget = item.value(QStringLiteral("suggestedDailyBudget")).toObject();
            QStringList targeting;
            for (const QJsonValue &entry : item.value(QStringLiteral("targeting")).toArray())
                targeting << entry.toString();
            addRow(m_adPlanTable,
                   {QString::number(int(number(item, "score"))), text(item, "name"),
                    QStringLiteral("%1–%2 %3").arg(int(number(budget, "low"))).arg(int(number(budget, "high")))
                        .arg(text(budget, "currency")),
                    text(item, "measure"), targeting.join(QStringLiteral(", "))},
                   text(item, "id"), -1, {QString(), text(item, "how")});
        }
    }
    m_adPlanPanel->hint()->setText(text(m_adPlan, "landingPage").isEmpty()
                                       ? QStringLiteral("Ranked from your visits, conversions and logged ads")
                                       : QStringLiteral("Landing page %1").arg(text(m_adPlan, "landingPage")));
    int select = qBound(0, previous, m_adPlanTable->rowCount() - 1);
    for (int row = 0; row < m_adPlanTable->rowCount(); ++row) {
        if (!previousId.isEmpty() && m_adPlanTable->item(row, 0)->data(Qt::UserRole).toString() == previousId)
            select = row;
    }
    if (m_adPlanTable->rowCount())
        m_adPlanTable->setCurrentCell(select, 0);
    else
        m_adPlanWhy->setText(m_adPlan.isEmpty() ? QStringLiteral("Reading visits, conversions and logged ads…")
                                                : QString());
    m_adPlanAd->setEnabled(m_adPlanTable->rowCount() > 0);
    const QJsonArray checks = m_adPlan.value(QStringLiteral("readiness")).toArray();
    int ready = 0;
    m_readinessTable->setRowCount(0);
    for (const QJsonValue &value : checks) {
        const QJsonObject check = value.toObject();
        const bool done = check.value(QStringLiteral("done")).toBool();
        ready += done;
        addRow(m_readinessTable, {text(check, "label"), done ? QStringLiteral("Ready") : QStringLiteral("To do")},
               text(check, "id"), -1, {text(check, "detail"), text(check, "detail")});
        m_readinessTable->item(m_readinessTable->rowCount() - 1, 1)->setForeground(
            done ? QColor("#1baf7a") : palette().color(QPalette::PlaceholderText));
    }
    m_readinessPanel->hint()->setText(checks.isEmpty() ? QString()
                                                       : QStringLiteral("%1 of %2 ready").arg(ready).arg(checks.size()));
}

void MarketingFeature::planAd(const QJsonObject &recommendation)
{
    if (recommendation.isEmpty())
        return;
    QStringList targeting;
    for (const QJsonValue &entry : recommendation.value(QStringLiteral("targeting")).toArray())
        targeting << entry.toString();
    QJsonObject preset{{QStringLiteral("name"), text(recommendation, "name") + QStringLiteral(" test")},
                       {QStringLiteral("destinationUrl"), text(recommendation, "link")},
                       {QStringLiteral("audience"), targeting.join(QStringLiteral(", "))},
                       {QStringLiteral("currency"), text(recommendation.value(QStringLiteral("suggestedDailyBudget"))
                                                             .toObject(), "currency")},
                       {QStringLiteral("budgetType"), QStringLiteral("daily")},
                       {QStringLiteral("status"), QStringLiteral("draft")}};
    if (!text(recommendation, "channel").isEmpty())
        preset.insert(QStringLiteral("channel"), text(recommendation, "channel"));
    editAd(preset);
}

void MarketingFeature::runAdvisorAction(const QJsonObject &action)
{
    const QString type = text(action, "type");
    if (type == QLatin1String("campaign")) {
        editCampaign(QJsonObject());
    } else if (type == QLatin1String("campaign-edit")) {
        editCampaign(campaignById(text(action, "id")));
    } else if (type == QLatin1String("content")) {
        editContent(QJsonObject{{QStringLiteral("campaign"), text(action, "campaign")},
                                {QStringLiteral("channel"), text(action, "channel")},
                                {QStringLiteral("status"), QStringLiteral("draft")}});
    } else if (type == QLatin1String("content-open")) {
        editContent(byId(m_content, text(action, "id")));
    } else if (type == QLatin1String("channel")) {
        openChannel(text(action, "channel"));
    } else if (type == QLatin1String("keyword")) {
        editKeyword(QJsonObject());
    } else if (type == QLatin1String("ad")) {
        editAd(byId(m_ads, text(action, "id")));
    } else if (type == QLatin1String("ad-new")) {
        planAd(QJsonObject{{QStringLiteral("name"), QStringLiteral("New ad")},
                           {QStringLiteral("channel"), text(action, "channel")}});
    } else if (type == QLatin1String("experiment")) {
        editExperiment(byId(m_experiments, text(action, "id")));
    } else if (type == QLatin1String("funnel-new")) {
        editFunnelWeek();
    } else if (type == QLatin1String("goals")) {
        editGoals();
    } else if (type == QLatin1String("page")) {
        const QString page = text(action, "page");
        if (page == QLatin1String("growth"))
            showPage(3);
        else if (page == QLatin1String("reddit"))
            openChannel(QStringLiteral("reddit"));
        else
            openWebDashboard(QStringLiteral("page=") + QString::fromUtf8(QUrl::toPercentEncoding(page)));
    } else if (type == QLatin1String("analytics")) {
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://app.forkmesh.com/dashboard/analytics")));
    }
}

} // namespace forkmesh::marketing
