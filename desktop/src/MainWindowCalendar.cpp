#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "RepositoryCalendar.h"
#include "ClaudeStreamSession.h"
#include "CodexAppServerSession.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QDateTimeEdit>
#include <QDialog>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QEvent>
#include <QFormLayout>
#include <QFrame>
#include <QHeaderView>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSharedPointer>
#include <QSplitter>
#include <QStringListModel>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QTextEdit>
#include <QTimer>
#include <QTimeZone>
#include <QUrlQuery>
#include <QVBoxLayout>

#include <functional>
#include <initializer_list>

using namespace forkmesh::ui;

namespace {
constexpr int kCalendarIdRole = Qt::UserRole + 1;
constexpr int kCalendarScopeRole = Qt::UserRole + 2;
constexpr int kCalendarMoreRole = Qt::UserRole + 3;
constexpr int kCalendarOutsideMonthRole = Qt::UserRole + 4;
constexpr int kCalendarAgendaPartsRole = Qt::UserRole + 5;
// Every commit and merge in every configured repository lands on this page, so
// each view shows a bounded slice and points at the fuller one behind it.
constexpr int kCalendarMonthCellEvents = 4;
constexpr int kCalendarWeekCellEvents = 6;
constexpr int kCalendarAgendaPage = 300;
// A slice of repository reads per event-loop turn, so the window keeps
// painting while the rest of the months are walked.
constexpr int kRepositoryCalendarSliceMs = 60;
constexpr int kRepositoryCalendarCacheEntries = 240;

QPair<QDate, QDate> calendarRange(const QDate &anchor, const QString &mode)
{
    if (mode == QLatin1String("week")) {
        const QDate first = anchor.addDays(1 - anchor.dayOfWeek());
        return {first, first.addDays(7)};
    }
    if (mode == QLatin1String("day"))
        return {anchor, anchor.addDays(1)};
    if (mode == QLatin1String("agenda"))
        return {anchor, anchor.addDays(90)};
    const QDate monthFirst(anchor.year(), anchor.month(), 1);
    const QDate first = monthFirst.addDays(1 - monthFirst.dayOfWeek());
    return {first, first.addDays(42)};
}

QString calendarRequestProof(const QByteArray &method, const QString &path,
                             QString *resource)
{
    const QString cleanPath = QUrl(path).path();
    const bool collection = cleanPath == QLatin1String("/api/calendar") ||
                            cleanPath == QLatin1String("/api/calendar/");
    static const QRegularExpression eventRe(
        QStringLiteral("^/api/calendar/([a-f0-9]{32})/?$"));
    static const QRegularExpression rsvpRe(
        QStringLiteral("^/api/calendar/([a-f0-9]{32})/rsvp/?$"));
    const QRegularExpressionMatch event = eventRe.match(cleanPath);
    const QRegularExpressionMatch rsvp = rsvpRe.match(cleanPath);
    if (collection) {
        if (method == QByteArrayLiteral("GET"))
            return kCalendarListProof;
        if (method == QByteArrayLiteral("POST"))
            return kCalendarCreateProof;
        return {};
    }
    if (rsvp.hasMatch() && method == QByteArrayLiteral("POST")) {
        *resource = rsvp.captured(1);
        return kCalendarRsvpProof;
    }
    if (!event.hasMatch())
        return {};
    *resource = event.captured(1);
    if (method == QByteArrayLiteral("GET"))
        return kCalendarReadProof;
    if (method == QByteArrayLiteral("PATCH"))
        return kCalendarUpdateProof;
    if (method == QByteArrayLiteral("DELETE"))
        return kCalendarDeleteProof;
    return {};
}

QString calendarErrorText(const QJsonObject &payload, const QString &fallback)
{
    QString error = payload.value(QStringLiteral("error")).toString().trimmed();
    if (error.isEmpty())
        return fallback;
    error.replace(QLatin1Char('_'), QLatin1Char(' '));
    return error;
}

QTimeZone calendarTimeZone(const QString &name)
{
    QTimeZone zone(name.trimmed().toUtf8());
    if (!zone.isValid())
        zone = QTimeZone::systemTimeZone();
    return zone;
}

QString calendarTimeZoneLabel(const QTimeZone &zone, const QDateTime &at)
{
    const int offset = zone.offsetFromUtc(at);
    const QChar sign = offset < 0 ? QLatin1Char('-') : QLatin1Char('+');
    const int absolute = qAbs(offset);
    return QStringLiteral("%1 (UTC%2%3:%4)")
        .arg(QString::fromUtf8(zone.id()).replace(QLatin1Char('_'), QLatin1Char(' ')))
        .arg(sign)
        .arg(absolute / 3600, 2, 10, QLatin1Char('0'))
        .arg((absolute % 3600) / 60, 2, 10, QLatin1Char('0'));
}

QJsonObject calendarAgentSuggestion(const QString &text)
{
    const int first = text.indexOf(QLatin1Char('{'));
    const int last = text.lastIndexOf(QLatin1Char('}'));
    if (first < 0 || last <= first)
        return {};
    return QJsonDocument::fromJson(text.mid(first, last - first + 1).toUtf8()).object();
}

qint64 eventStamp(const QJsonObject &event, const QString &name)
{
    return static_cast<qint64>(event.value(name).toDouble());
}

QDateTime localEventTime(const QJsonObject &event, const QString &name)
{
    return QDateTime::fromMSecsSinceEpoch(eventStamp(event, name)).toLocalTime();
}

QString eventSecondaryLabel(const QJsonObject &event)
{
    if (event.value(QStringLiteral("source")).toString() ==
        QLatin1String("repository")) {
        QStringList pieces{
            event.value(QStringLiteral("repository")).toString(),
            event.value(QStringLiteral("owner")).toString(),
            event.value(QStringLiteral("hash")).toString().left(12)};
        pieces.removeAll(QString());
        return pieces.join(QString::fromUtf8(" · "));
    }
    QStringList pieces;
    const QString location = event.value(QStringLiteral("location")).toString();
    if (!location.isEmpty())
        pieces << location;
    pieces << (event.value(QStringLiteral("scope")).toString() ==
                       QLatin1String("organization")
                   ? event.value(QStringLiteral("calendar")).toString()
                   : QStringLiteral("Private"));
    const QJsonArray attendees = event.value(QStringLiteral("attendees")).toArray();
    if (!attendees.isEmpty())
        pieces << QStringLiteral("%1 invited").arg(attendees.size());
    return pieces.join(QString::fromUtf8(" · "));
}

QJsonObject calendarEvent(const QJsonArray &events, const QString &id)
{
    for (const QJsonValue &value : events) {
        const QJsonObject event = value.toObject();
        if (event.value(QStringLiteral("id")).toString() == id)
            return event;
    }
    return {};
}

CalendarOccurrence calendarOccurrence(const QJsonObject &event, qint64 startAtMs,
                                      qint64 endAtMs)
{
    CalendarOccurrence occurrence;
    occurrence.event = event;
    occurrence.startAtMs = startAtMs;
    occurrence.endAtMs = endAtMs;
    const QDateTime local =
        QDateTime::fromMSecsSinceEpoch(startAtMs).toLocalTime();
    occurrence.date = local.date();
    occurrence.time = local.time();
    return occurrence;
}

// An occurrence keeps a shared handle on its source event rather than a copy
// with two fields inserted; inserting detaches, and with every commit on the
// calendar that deep-copied thousands of objects on every redraw.
void collectOccurrences(const QJsonArray &events,
                        const QDateTime &rangeStart,
                        const QDateTime &rangeEnd,
                        const QSet<QString> &calendars,
                        const QString &query,
                        QList<CalendarOccurrence> *result)
{
    const QString needle = query.trimmed().toLower();
    const qint64 startMs = rangeStart.toMSecsSinceEpoch();
    const qint64 endMs = rangeEnd.toMSecsSinceEpoch();
    for (const QJsonValue &value : events) {
        const QJsonObject event = value.toObject();
        if (!calendars.contains(
                event.value(QStringLiteral("calendar")).toString(
                    QStringLiteral("personal"))))
            continue;
        if (!needle.isEmpty()) {
            const QString haystack =
                event.value(QStringLiteral("title")).toString() + QLatin1Char(' ') +
                event.value(QStringLiteral("description")).toString() + QLatin1Char(' ') +
                event.value(QStringLiteral("location")).toString();
            if (!haystack.contains(needle, Qt::CaseInsensitive))
                continue;
        }
        const qint64 first = eventStamp(event, QStringLiteral("startAt"));
        const qint64 duration = qMax<qint64>(
            1, eventStamp(event, QStringLiteral("endAt")) - first);
        const QString recurrence =
            event.value(QStringLiteral("recurrence")).toString(
                QStringLiteral("none"));
        const qint64 until = static_cast<qint64>(
            event.value(QStringLiteral("recurrenceUntil")).toDouble());
        const bool repeats = recurrence == QLatin1String("daily") ||
                             recurrence == QLatin1String("weekly") ||
                             recurrence == QLatin1String("monthly") ||
                             recurrence == QLatin1String("yearly");
        // Repository activity never recurs, so it skips the calendar
        // arithmetic entirely and never builds a QDateTime it does not use.
        if (!repeats) {
            if (first < endMs && first + duration > startMs &&
                (until <= 0 || first <= until))
                result->append(calendarOccurrence(event, first, first + duration));
            continue;
        }
        QDateTime occurrence =
            QDateTime::fromMSecsSinceEpoch(first).toLocalTime();
        for (int guard = 0; guard < 500 && occurrence < rangeEnd; ++guard) {
            const qint64 at = occurrence.toMSecsSinceEpoch();
            if (at + duration > startMs && (until <= 0 || at <= until))
                result->append(calendarOccurrence(event, at, at + duration));
            if (recurrence == QLatin1String("daily"))
                occurrence = occurrence.addDays(1);
            else if (recurrence == QLatin1String("weekly"))
                occurrence = occurrence.addDays(7);
            else if (recurrence == QLatin1String("monthly"))
                occurrence = occurrence.addMonths(1);
            else
                occurrence = occurrence.addYears(1);
        }
    }
}

void sortOccurrences(QList<CalendarOccurrence> *events)
{
    std::sort(events->begin(), events->end(), [](const CalendarOccurrence &a,
                                                 const CalendarOccurrence &b) {
        return a.startAtMs < b.startAtMs;
    });
}

QList<CalendarOccurrence> expandedEvents(const QJsonArray &events,
                                         const QDateTime &rangeStart,
                                         const QDateTime &rangeEnd,
                                         const QSet<QString> &calendars,
                                         const QString &query)
{
    QList<CalendarOccurrence> result;
    collectOccurrences(events, rangeStart, rangeEnd, calendars, query, &result);
    sortOccurrences(&result);
    return result;
}

QListWidgetItem *agendaItem(const CalendarOccurrence &occurrence, bool showDate)
{
    const QJsonObject &event = occurrence.event;
    QStringList lines;
    if (showDate)
        lines << occurrence.date.toString(QStringLiteral("dddd, MMMM d"));
    const QString when = event.value(QStringLiteral("allDay")).toBool()
        ? QStringLiteral("All day")
        : QStringLiteral("%1–%2").arg(
              occurrence.time.toString(QStringLiteral("h:mm AP")),
              QDateTime::fromMSecsSinceEpoch(occurrence.endAtMs)
                  .toLocalTime().time().toString(QStringLiteral("h:mm AP")));
    lines << QStringLiteral("%1  %2").arg(
        when, event.value(QStringLiteral("title")).toString());
    const QString secondary = eventSecondaryLabel(event);
    if (!secondary.isEmpty())
        lines << secondary;
    auto *item = new QListWidgetItem(lines.join(QLatin1Char('\n')));
    item->setData(kCalendarIdRole,
                  event.value(QStringLiteral("id")).toString());
    item->setData(kCalendarAgendaPartsRole,
                  QStringList{showDate ? lines.constFirst() : QString(), when,
                              event.value(QStringLiteral("title")).toString(),
                              secondary});
    item->setToolTip(lines.join(QLatin1Char('\n')));
    item->setSizeHint(QSize(0, showDate ? 66 : 50));
    return item;
}

void showRepositoryActivity(const QJsonObject &event, QWidget *parent)
{
    const bool merge =
        event.value(QStringLiteral("kind")).toString() == QLatin1String("merge");
    QDialog dialog(parent);
    dialog.setWindowTitle(merge ? QStringLiteral("Merge details")
                                : QStringLiteral("Commit details"));
    dialog.setMinimumWidth(560);
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(20, 18, 20, 18);
    layout->setSpacing(14);

    auto *heading = new QLabel(
        event.value(QStringLiteral("title")).toString());
    heading->setObjectName(QStringLiteral("sectionTitle"));
    heading->setWordWrap(true);
    layout->addWidget(heading);

    auto *form = new QFormLayout;
    form->addRow(QStringLiteral("Type"),
                 new QLabel(merge ? QStringLiteral("Merge")
                                  : QStringLiteral("Commit")));
    form->addRow(QStringLiteral("Repository"),
                 new QLabel(event.value(QStringLiteral("repository")).toString()));
    form->addRow(QStringLiteral("Author"),
                 new QLabel(event.value(QStringLiteral("owner")).toString()));
    form->addRow(QStringLiteral("Committed"),
                 new QLabel(localEventTime(event, QStringLiteral("startAt"))
                                .toString(QStringLiteral("yyyy-MM-dd h:mm:ss AP"))));
    auto *hash = new QLineEdit(
        event.value(QStringLiteral("hash")).toString());
    hash->setReadOnly(true);
    form->addRow(QStringLiteral("Commit"), hash);
    layout->addLayout(form);

    auto *notice = new QLabel(QStringLiteral(
        "Repository activity is read-only and comes from Git history across "
        "every branch and tag."));
    notice->setObjectName(QStringLiteral("mutedLabel"));
    notice->setWordWrap(true);
    layout->addWidget(notice);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog,
                     &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec();
}

constexpr qreal kCalendarGridRadius = 12.0;
constexpr int kCalendarChipHeight = 18;
constexpr int kCalendarChipGap = 2;
constexpr int kCalendarMoreHeight = 14;
constexpr int kCalendarAgendaDayHeader = 30;
constexpr int kCalendarAgendaRowHeight = 50;
constexpr int kCalendarAgendaCompactHeight = 36;

QFont calendarFont(const QFont &base, int pixelSize, QFont::Weight weight)
{
    QFont font(base);
    font.setPixelSize(pixelSize);
    font.setWeight(weight);
    return font;
}

int calendarMonthRowHeight(const QTableWidget *table)
{
    return qMax(88, table->viewport()->height() / 6);
}

int calendarOverflowCount(const QString &line)
{
    static const QRegularExpression pattern(
        QStringLiteral("^\\+(\\d+) more$"));
    const QRegularExpressionMatch match = pattern.match(line);
    return match.hasMatch() ? match.captured(1).toInt() : 0;
}

QPainterPath calendarCornerPath(const QRectF &rect, qreal topLeft,
                                qreal topRight, qreal bottomRight,
                                qreal bottomLeft)
{
    QPainterPath path;
    path.moveTo(rect.left() + topLeft, rect.top());
    path.lineTo(rect.right() - topRight, rect.top());
    if (topRight > 0) {
        path.arcTo(QRectF(rect.right() - 2 * topRight, rect.top(),
                          2 * topRight, 2 * topRight),
                   90, -90);
    }
    path.lineTo(rect.right(), rect.bottom() - bottomRight);
    if (bottomRight > 0) {
        path.arcTo(QRectF(rect.right() - 2 * bottomRight,
                          rect.bottom() - 2 * bottomRight, 2 * bottomRight,
                          2 * bottomRight),
                   0, -90);
    }
    path.lineTo(rect.left() + bottomLeft, rect.bottom());
    if (bottomLeft > 0) {
        path.arcTo(QRectF(rect.left(), rect.bottom() - 2 * bottomLeft,
                          2 * bottomLeft, 2 * bottomLeft),
                   270, -90);
    }
    path.lineTo(rect.left(), rect.top() + topLeft);
    if (topLeft > 0) {
        path.arcTo(QRectF(rect.left(), rect.top(), 2 * topLeft, 2 * topLeft),
                   180, -90);
    }
    path.closeSubpath();
    return path;
}

void paintCalendarChip(QPainter *painter, const QRect &rect,
                       const QString &label, const QString &title,
                       const QFont &base)
{
    if (rect.width() < 16 || rect.height() < 8)
        return;
    painter->setPen(Qt::NoPen);
    painter->setBrush(Theme::color(Theme::Role::AccentSubtle));
    painter->drawRoundedRect(rect, 4, 4);
    const QRect inner = rect.adjusted(6, 0, -6, 0);
    int left = inner.left();
    if (!label.isEmpty()) {
        const QFont labelFont = calendarFont(base, 11, QFont::Normal);
        const int width = qMin(QFontMetrics(labelFont).horizontalAdvance(label),
                               inner.width());
        painter->setFont(labelFont);
        painter->setPen(Theme::color(Theme::Role::FgMuted));
        painter->drawText(QRect(left, inner.top(), width, inner.height()),
                          Qt::AlignLeft | Qt::AlignVCenter, label);
        left += width + 5;
    }
    const int room = inner.right() - left + 1;
    if (room < 8 || title.isEmpty())
        return;
    const QFont titleFont = calendarFont(base, 11, QFont::Medium);
    painter->setFont(titleFont);
    painter->setPen(Theme::color(Theme::Role::Fg));
    painter->drawText(QRect(left, inner.top(), room, inner.height()),
                      Qt::AlignLeft | Qt::AlignVCenter,
                      QFontMetrics(titleFont).elidedText(title, Qt::ElideRight,
                                                         room));
}

void paintCalendarRowFill(QPainter *painter, const QRect &rect, bool selected)
{
    painter->setPen(Qt::NoPen);
    painter->setBrush(Theme::color(selected ? Theme::Role::SurfaceActive
                                            : Theme::Role::SurfaceRaised));
    painter->drawRoundedRect(rect, 8, 8);
}

void paintCalendarOutline(QPainter *painter, const QPainterPath &path)
{
    painter->setPen(QPen(Theme::color(Theme::Role::Accent), 1));
    painter->setBrush(Qt::NoBrush);
    painter->drawPath(path);
}

class CalendarMonthDelegate : public QStyledItemDelegate
{
public:
    explicit CalendarMonthDelegate(QTableView *table)
        : QStyledItemDelegate(table), m_table(table)
    {
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        if (!Theme::isNext()) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }
        const QRect cell = option.rect;
        const bool ownGrid = !m_table->showGrid();
        const bool firstRow = index.row() == 0;
        const bool firstColumn = index.column() == 0;
        const bool lastRow = index.row() == index.model()->rowCount() - 1;
        const bool lastColumn =
            index.column() == index.model()->columnCount() - 1;
        const qreal radius = ownGrid ? kCalendarGridRadius : 0.0;
        const qreal topLeft = firstRow && firstColumn ? radius : 0.0;
        const qreal topRight = firstRow && lastColumn ? radius : 0.0;
        const qreal bottomRight = lastRow && lastColumn ? radius : 0.0;
        const qreal bottomLeft = lastRow && firstColumn ? radius : 0.0;
        const bool outside = index.data(kCalendarOutsideMonthRole).toBool();
        const bool hovered = option.state.testFlag(QStyle::State_MouseOver);
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        const QColor line = Theme::color(Theme::Role::Border);

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setClipRect(cell, Qt::IntersectClip);
        painter->fillPath(
            calendarCornerPath(QRectF(cell), topLeft, topRight, bottomRight,
                               bottomLeft),
            Theme::color(hovered ? Theme::Role::SurfaceRaised
                         : outside ? Theme::Role::BgSubtle
                                   : Theme::Role::Bg));
        if (ownGrid) {
            if (!lastColumn) {
                painter->fillRect(
                    QRect(cell.right(), cell.top(), 1, cell.height()), line);
            }
            if (!lastRow) {
                painter->fillRect(
                    QRect(cell.left(), cell.bottom(), cell.width(), 1), line);
            }
            const QRectF grid(m_table->columnViewportPosition(0),
                              m_table->rowViewportPosition(0),
                              m_table->horizontalHeader()->length(),
                              m_table->verticalHeader()->length());
            painter->setPen(QPen(line, 1));
            painter->setBrush(Qt::NoBrush);
            painter->drawRoundedRect(grid.adjusted(0.5, 0.5, -0.5, -0.5),
                                     radius - 0.5, radius - 0.5);
        }

        const QStringList lines =
            index.data(Qt::DisplayRole).toString().split(QLatin1Char('\n'));
        const QDate date = index.data(Qt::UserRole).toDate();
        const bool today = date.isValid() && date == QDate::currentDate();
        const QRect badge(cell.left() + 6, cell.top() + 5, 22, 22);
        if (today) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(Theme::color(Theme::Role::Accent));
            painter->drawEllipse(badge);
        }
        painter->setFont(calendarFont(option.font, 12, QFont::Medium));
        painter->setPen(Theme::color(today ? Theme::Role::OnAccent
                                     : outside ? Theme::Role::FgSubtle
                                               : Theme::Role::Fg));
        painter->drawText(badge, Qt::AlignCenter, lines.value(0));

        QStringList events = lines.mid(1);
        int overflow = 0;
        if (!events.isEmpty()) {
            overflow = calendarOverflowCount(events.constLast());
            if (overflow > 0)
                events.removeLast();
        }
        const QRect area = cell.adjusted(6, 29, ownGrid ? -7 : -6, -4);
        const int step = kCalendarChipHeight + kCalendarChipGap;
        int shown = int(events.size());
        if (overflow > 0 || shown > (area.height() + kCalendarChipGap) / step) {
            shown = qBound(0, (area.height() - kCalendarMoreHeight) / step,
                           shown);
        }
        const int hidden = int(events.size()) - shown + overflow;
        int top = area.top();
        for (int position = 0; position < shown; ++position) {
            const QString &entry = events.at(position);
            const int space = entry.indexOf(QLatin1Char(' '));
            paintCalendarChip(
                painter,
                QRect(area.left(), top, area.width(), kCalendarChipHeight),
                space < 0 ? QString() : entry.left(space),
                space < 0 ? entry : entry.mid(space + 1), option.font);
            top += step;
        }
        if (hidden > 0 && area.bottom() - top + 1 >= kCalendarMoreHeight) {
            painter->setFont(calendarFont(option.font, 11, QFont::Medium));
            painter->setPen(Theme::color(Theme::Role::FgMuted));
            painter->drawText(QRect(area.left() + 6, top, area.width() - 6,
                                    kCalendarMoreHeight),
                              Qt::AlignLeft | Qt::AlignVCenter,
                              QStringLiteral("+%1 more").arg(hidden));
        }

        if (selected) {
            const qreal inset = 1.5;
            const qreal lineInset = ownGrid ? 2.5 : 1.5;
            const QRectF outline = QRectF(cell).adjusted(
                ownGrid && firstColumn ? lineInset : inset,
                ownGrid && firstRow ? lineInset : inset, -lineInset,
                -lineInset);
            const auto corner = [](qreal gridCorner) {
                return gridCorner > 0 ? gridCorner - 2.5 : 6.0;
            };
            paintCalendarOutline(
                painter,
                calendarCornerPath(outline, corner(topLeft), corner(topRight),
                                   corner(bottomRight), corner(bottomLeft)));
        }
        painter->restore();
    }

private:
    QTableView *m_table;
};

class CalendarWeekDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        if (!Theme::isNext()) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }
        const QRect cell = option.rect;
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setClipRect(cell, Qt::IntersectClip);
        painter->fillRect(
            cell, Theme::color(option.state.testFlag(QStyle::State_MouseOver)
                                   ? Theme::Role::SurfaceRaised
                                   : Theme::Role::Bg));
        if (!index.data(kCalendarIdRole).toString().isEmpty()) {
            QStringList titles =
                index.data(Qt::DisplayRole).toString().split(QLatin1Char('\n'));
            int overflow = 0;
            if (titles.size() > 1) {
                overflow = calendarOverflowCount(titles.constLast());
                if (overflow > 0)
                    titles.removeLast();
            }
            const int hidden = int(titles.size()) - 1 + overflow;
            const int height = qMin(22, cell.height() - 4);
            QRect chip(cell.left() + 3,
                       cell.top() + (cell.height() - height) / 2,
                       cell.width() - 6, height);
            if (hidden > 0) {
                const QFont countFont =
                    calendarFont(option.font, 11, QFont::Medium);
                const QString count = QStringLiteral("+%1").arg(hidden);
                const int width =
                    QFontMetrics(countFont).horizontalAdvance(count) + 8;
                if (chip.width() - width >= 32) {
                    painter->setFont(countFont);
                    painter->setPen(Theme::color(Theme::Role::FgMuted));
                    painter->drawText(QRect(chip.right() - width + 1,
                                            chip.top(), width, chip.height()),
                                      Qt::AlignCenter, count);
                    chip.setWidth(chip.width() - width);
                }
            }
            paintCalendarChip(painter, chip, QString(), titles.constFirst(),
                              option.font);
        }
        if (option.state.testFlag(QStyle::State_Selected)) {
            paintCalendarOutline(
                painter,
                calendarCornerPath(QRectF(cell).adjusted(1.5, 1.5, -1.5, -1.5),
                                   6.0, 6.0, 6.0, 6.0));
        }
        painter->restore();
    }
};

bool calendarAgendaStartsDay(const QModelIndex &index, const QStringList &parts)
{
    if (parts.value(0).isEmpty())
        return false;
    if (index.row() == 0)
        return true;
    return index.siblingAtRow(index.row() - 1)
               .data(kCalendarAgendaPartsRole)
               .toStringList()
               .value(0) != parts.at(0);
}

class CalendarAgendaDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override
    {
        if (!Theme::isNext())
            return QStyledItemDelegate::sizeHint(option, index);
        const QStringList parts =
            index.data(kCalendarAgendaPartsRole).toStringList();
        if (parts.size() < 4)
            return QSize(0, kCalendarAgendaCompactHeight);
        int height = parts.at(3).isEmpty() ? kCalendarAgendaCompactHeight
                                           : kCalendarAgendaRowHeight;
        if (calendarAgendaStartsDay(index, parts))
            height += kCalendarAgendaDayHeader;
        return QSize(0, height);
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        if (!Theme::isNext()) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }
        const QStringList parts =
            index.data(kCalendarAgendaPartsRole).toStringList();
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        const bool hovered = option.state.testFlag(QStyle::State_MouseOver);
        QRect row = option.rect;
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setClipRect(row, Qt::IntersectClip);
        if (parts.size() < 4) {
            const bool more = index.data(kCalendarMoreRole).toBool();
            if (more && (selected || hovered))
                paintCalendarRowFill(painter, row, selected);
            const QFont font = calendarFont(
                option.font, 13, more ? QFont::Medium : QFont::Normal);
            const QRect text = row.adjusted(12, 0, -12, 0);
            painter->setFont(font);
            painter->setPen(Theme::color(more ? Theme::Role::AccentFg
                                              : Theme::Role::FgMuted));
            painter->drawText(
                text, Qt::AlignLeft | Qt::AlignVCenter,
                QFontMetrics(font).elidedText(
                    index.data(Qt::DisplayRole).toString(), Qt::ElideRight,
                    text.width()));
            painter->restore();
            return;
        }
        if (calendarAgendaStartsDay(index, parts)) {
            painter->setFont(calendarFont(option.font, 12, QFont::Medium));
            painter->setPen(Theme::color(Theme::Role::FgMuted));
            painter->drawText(QRect(row.left() + 12, row.top() + 8,
                                    row.width() - 24, 18),
                              Qt::AlignLeft | Qt::AlignVCenter, parts.at(0));
            row.setTop(row.top() + kCalendarAgendaDayHeader);
        }
        if (selected || hovered)
            paintCalendarRowFill(painter, row, selected);
        const QFont whenFont = calendarFont(option.font, 12, QFont::Normal);
        const QFont titleFont = calendarFont(option.font, 13, QFont::Medium);
        const QFontMetrics whenMetrics(whenFont);
        const QRect inner = row.adjusted(12, 0, -12, 0);
        const int whenWidth =
            qMin(inner.width() / 2,
                 whenMetrics.horizontalAdvance(
                     QStringLiteral("12:00 AM–12:00 AM")) + 4);
        const bool detailed = !parts.at(3).isEmpty();
        const int lineTop =
            detailed ? row.top() + 8 : row.top() + (row.height() - 18) / 2;
        painter->setFont(whenFont);
        painter->setPen(Theme::color(Theme::Role::FgMuted));
        painter->drawText(QRect(inner.left(), lineTop, whenWidth, 18),
                          Qt::AlignLeft | Qt::AlignVCenter,
                          whenMetrics.elidedText(parts.at(1), Qt::ElideRight,
                                                 whenWidth));
        const int textLeft = inner.left() + whenWidth + 12;
        const int textWidth = qMax(0, inner.right() - textLeft + 1);
        painter->setFont(titleFont);
        painter->setPen(Theme::color(Theme::Role::Fg));
        painter->drawText(QRect(textLeft, lineTop, textWidth, 18),
                          Qt::AlignLeft | Qt::AlignVCenter,
                          QFontMetrics(titleFont).elidedText(
                              parts.at(2), Qt::ElideRight, textWidth));
        if (detailed) {
            painter->setFont(whenFont);
            painter->setPen(Theme::color(Theme::Role::FgMuted));
            painter->drawText(QRect(textLeft, lineTop + 19, textWidth, 16),
                              Qt::AlignLeft | Qt::AlignVCenter,
                              whenMetrics.elidedText(parts.at(3),
                                                     Qt::ElideRight,
                                                     textWidth));
        }
        painter->restore();
    }
};

class CalendarEventHook : public QObject
{
public:
    CalendarEventHook(QObject *watched,
                      std::initializer_list<QEvent::Type> types,
                      std::function<void()> action)
        : QObject(watched), m_types(types), m_action(std::move(action))
    {
        watched->installEventFilter(this);
        m_action();
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (m_types.contains(event->type()))
            m_action();
        return QObject::eventFilter(watched, event);
    }

private:
    QList<QEvent::Type> m_types;
    std::function<void()> m_action;
};
} // namespace

QWidget *MainWindow::buildCalendarSection()
{
    m_calendarAnchor = QDate::currentDate();
    auto *page = new QWidget;
    page->setObjectName(QStringLiteral("calendarSection"));
    auto *outer = new QVBoxLayout(page);
    outer->setContentsMargins(20, 16, 20, 20);
    outer->setSpacing(10);

    auto *toolbar = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("Calendar"));
    title->setObjectName(QStringLiteral("pageTitle"));
    toolbar->addWidget(title);
    auto *zone = new QLabel(QString::fromUtf8(QTimeZone::systemTimeZoneId()));
    zone->setObjectName(QStringLiteral("statusChip"));
    zone->setToolTip(QStringLiteral("Events are shown in this computer's time zone"));
    toolbar->addWidget(zone);
    toolbar->addStretch();
    auto *today = new QPushButton(QStringLiteral("Today"));
    today->setObjectName(QStringLiteral("ghostButton"));
    today->setProperty("calendarNav", QStringLiteral("today"));
    toolbar->addWidget(today);
    auto *previous = new QPushButton;
    previous->setObjectName(QStringLiteral("ghostButton"));
    previous->setProperty("calendarNav", QStringLiteral("step"));
    previous->setToolTip(QStringLiteral("Previous period"));
    setOcticon(previous, "chevron-left", 14);
    toolbar->addWidget(previous);
    auto *next = new QPushButton;
    next->setObjectName(QStringLiteral("ghostButton"));
    next->setProperty("calendarNav", QStringLiteral("step"));
    next->setToolTip(QStringLiteral("Next period"));
    setOcticon(next, "chevron-right", 14);
    toolbar->addWidget(next);
    auto *mode = new QComboBox;
    mode->addItem(QStringLiteral("Month"), QStringLiteral("month"));
    mode->addItem(QStringLiteral("Week"), QStringLiteral("week"));
    mode->addItem(QStringLiteral("Day"), QStringLiteral("day"));
    mode->addItem(QStringLiteral("Agenda"), QStringLiteral("agenda"));
    toolbar->addWidget(mode);
    auto *add = new QPushButton(QStringLiteral("New event"));
    add->setObjectName(QStringLiteral("primaryButton"));
    setOcticon(add, "calendar", 14);
    toolbar->addWidget(add);
    outer->addLayout(toolbar);

    auto *headingRow = new QHBoxLayout;
    m_calendarHeading = new QLabel;
    m_calendarHeading->setObjectName(QStringLiteral("sectionTitle"));
    headingRow->addWidget(m_calendarHeading);
    headingRow->addStretch();
    m_calendarStatus = new QLabel(QStringLiteral(
        "Personal events are private; organization calendars are member-only."));
    m_calendarStatus->setObjectName(QStringLiteral("mutedLabel"));
    headingRow->addWidget(m_calendarStatus);
    outer->addLayout(headingRow);

    auto *splitter = new QSplitter(Qt::Horizontal);
    splitter->setChildrenCollapsible(false);
    auto *sidebar = new QWidget;
    auto *sideLayout = new QVBoxLayout(sidebar);
    sideLayout->setContentsMargins(0, 0, 10, 0);
    auto *calendarsLabel = new QLabel(QStringLiteral("CALENDARS"));
    calendarsLabel->setObjectName(QStringLiteral("mutedLabel"));
    calendarsLabel->setProperty("calendarSidebar", QStringLiteral("heading"));
    sideLayout->addWidget(calendarsLabel);
    m_calendarList = new QListWidget;
    m_calendarList->setObjectName(QStringLiteral("calendarList"));
    m_calendarList->setMinimumWidth(190);
    m_calendarList->setMaximumWidth(240);
    sideLayout->addWidget(m_calendarList, 1);
    m_calendarSearch = new QLineEdit;
    m_calendarSearch->setPlaceholderText(QStringLiteral("Find events"));
    sideLayout->addWidget(m_calendarSearch);
    auto *privacy = new QLabel(QStringLiteral(
        "Personal event details are encrypted at rest. Repository commits and "
        "merges are read-only Git activity."));
    privacy->setObjectName(QStringLiteral("mutedLabel"));
    privacy->setProperty("calendarSidebar", QStringLiteral("note"));
    privacy->setWordWrap(true);
    sideLayout->addWidget(privacy);
    splitter->addWidget(sidebar);

    m_calendarViews = new QStackedWidget;
    m_calendarMonthTable = new QTableWidget(6, 7);
    m_calendarMonthTable->setObjectName(QStringLiteral("calendarMonthTable"));
    m_calendarMonthTable->setItemDelegate(
        new CalendarMonthDelegate(m_calendarMonthTable));
    m_calendarMonthTable->setHorizontalHeaderLabels(
        {QStringLiteral("Mon"), QStringLiteral("Tue"), QStringLiteral("Wed"),
         QStringLiteral("Thu"), QStringLiteral("Fri"), QStringLiteral("Sat"),
         QStringLiteral("Sun")});
    m_calendarMonthTable->verticalHeader()->hide();
    m_calendarMonthTable->horizontalHeader()->setSectionResizeMode(
        QHeaderView::Stretch);
    m_calendarMonthTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_calendarMonthTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_calendarMonthTable->setWordWrap(true);
    m_calendarViews->addWidget(m_calendarMonthTable);

    m_calendarWeekTable = new QTableWidget(24, 7);
    m_calendarWeekTable->setObjectName(QStringLiteral("calendarWeekTable"));
    m_calendarWeekTable->setItemDelegate(
        new CalendarWeekDelegate(m_calendarWeekTable));
    m_calendarWeekTable->verticalHeader()->setDefaultSectionSize(36);
    for (int hour = 0; hour < 24; ++hour) {
        m_calendarWeekTable->setVerticalHeaderItem(
            hour, new QTableWidgetItem(
                      QTime(hour, 0).toString(QStringLiteral("h AP"))));
    }
    m_calendarWeekTable->horizontalHeader()->setSectionResizeMode(
        QHeaderView::Stretch);
    m_calendarWeekTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_calendarWeekTable->setWordWrap(true);
    m_calendarViews->addWidget(m_calendarWeekTable);

    m_calendarDayList = new QListWidget;
    m_calendarDayList->setObjectName(QStringLiteral("calendarDayList"));
    m_calendarDayList->setItemDelegate(
        new CalendarAgendaDelegate(m_calendarDayList));
    m_calendarDayList->setSpacing(2);
    m_calendarViews->addWidget(m_calendarDayList);
    m_calendarAgendaList = new QListWidget;
    m_calendarAgendaList->setObjectName(QStringLiteral("calendarAgendaList"));
    m_calendarAgendaList->setItemDelegate(
        new CalendarAgendaDelegate(m_calendarAgendaList));
    m_calendarAgendaList->setSpacing(2);
    m_calendarViews->addWidget(m_calendarAgendaList);
    splitter->addWidget(m_calendarViews);
    splitter->setStretchFactor(1, 1);
    outer->addWidget(splitter, 1);

    QLineEdit *searchField = m_calendarSearch;
    QListWidget *calendarList = m_calendarList;
    QTableWidget *monthTable = m_calendarMonthTable;
    QTableWidget *weekTable = m_calendarWeekTable;
    QListWidget *dayList = m_calendarDayList;
    QListWidget *agendaList = m_calendarAgendaList;
    const bool classicGrid = monthTable->showGrid();
    const bool classicHighlight =
        monthTable->horizontalHeader()->highlightSections();
    const int classicListWidth = calendarList->maximumWidth();
    const auto fitMonthRows = [monthTable] {
        if (!Theme::isNext())
            return;
        const int rowHeight = calendarMonthRowHeight(monthTable);
        for (int row = 0; row < monthTable->rowCount(); ++row) {
            if (monthTable->rowHeight(row) != rowHeight)
                monthTable->setRowHeight(row, rowHeight);
        }
    };
    new CalendarEventHook(monthTable->viewport(), {QEvent::Resize},
                          fitMonthRows);
    new CalendarEventHook(
        page, {QEvent::Polish, QEvent::StyleChange, QEvent::Show},
        [sideLayout, searchField, calendarList, monthTable, weekTable, dayList,
         agendaList, classicGrid, classicHighlight, classicListWidth,
         fitMonthRows] {
            const bool nextDesign = Theme::isNext();
            const int listWidth =
                nextDesign ? QWIDGETSIZE_MAX : classicListWidth;
            if (calendarList->maximumWidth() != listWidth)
                calendarList->setMaximumWidth(listWidth);
            const int searchIndex = nextDesign ? 0 : 2;
            if (sideLayout->indexOf(searchField) != searchIndex) {
                sideLayout->removeWidget(searchField);
                sideLayout->insertWidget(searchIndex, searchField);
                if (nextDesign)
                    QWidget::setTabOrder(searchField, calendarList);
                else
                    QWidget::setTabOrder(calendarList, searchField);
            }
            const bool grid = !nextDesign && classicGrid;
            if (monthTable->showGrid() != grid)
                monthTable->setShowGrid(grid);
            const bool highlight = !nextDesign && classicHighlight;
            for (QHeaderView *header :
                 {monthTable->horizontalHeader(), weekTable->horizontalHeader(),
                  weekTable->verticalHeader()}) {
                if (header->highlightSections() != highlight)
                    header->setHighlightSections(highlight);
            }
            if (!nextDesign)
                return;
            for (QAbstractItemView *view :
                 std::initializer_list<QAbstractItemView *>{
                     calendarList, monthTable, weekTable, dayList, agendaList})
                view->viewport()->setAttribute(Qt::WA_Hover, true);
            fitMonthRows();
        });

    connect(add, &QPushButton::clicked, this,
            [this] { openCalendarEventDialog(); });
    connect(today, &QPushButton::clicked, this, [this] {
        m_calendarAnchor = QDate::currentDate();
        m_calendarAgendaLimit = kCalendarAgendaPage;
        refreshRepositoryCalendarEvents();
        renderCalendar();
    });
    auto shift = [this](int direction) {
        if (m_calendarMode == QLatin1String("month"))
            m_calendarAnchor = m_calendarAnchor.addMonths(direction);
        else if (m_calendarMode == QLatin1String("week"))
            m_calendarAnchor = m_calendarAnchor.addDays(direction * 7);
        else if (m_calendarMode == QLatin1String("agenda"))
            m_calendarAnchor = m_calendarAnchor.addDays(direction * 30);
        else
            m_calendarAnchor = m_calendarAnchor.addDays(direction);
        m_calendarAgendaLimit = kCalendarAgendaPage;
        refreshRepositoryCalendarEvents();
        renderCalendar();
    };
    connect(previous, &QPushButton::clicked, this, [shift] { shift(-1); });
    connect(next, &QPushButton::clicked, this, [shift] { shift(1); });
    connect(mode, &QComboBox::currentIndexChanged, this, [this, mode] {
        m_calendarMode = mode->currentData().toString();
        m_calendarAgendaLimit = kCalendarAgendaPage;
        refreshRepositoryCalendarEvents();
        renderCalendar();
    });
    // Each keystroke re-filters every commit in the window, so let the typist
    // finish the word first.
    m_calendarSearchTimer = new QTimer(this);
    m_calendarSearchTimer->setSingleShot(true);
    m_calendarSearchTimer->setInterval(200);
    connect(m_calendarSearchTimer, &QTimer::timeout, this, [this] {
        m_calendarAgendaLimit = kCalendarAgendaPage;
        renderCalendar();
    });
    connect(m_calendarSearch, &QLineEdit::textChanged, this,
            [this] { m_calendarSearchTimer->start(); });
    connect(m_calendarList, &QListWidget::itemChanged, this, [this] {
        m_calendarAgendaLimit = kCalendarAgendaPage;
        renderCalendar();
    });
    auto openItem = [this](QListWidgetItem *item) {
        if (!item)
            return;
        if (item->data(kCalendarMoreRole).toBool()) {
            m_calendarAgendaLimit += kCalendarAgendaPage;
            renderCalendar();
            return;
        }
        openCalendarEventDialog(item->data(kCalendarIdRole).toString());
    };
    connect(m_calendarDayList, &QListWidget::itemDoubleClicked, this, openItem);
    connect(m_calendarAgendaList, &QListWidget::itemDoubleClicked, this, openItem);
    connect(m_calendarMonthTable, &QTableWidget::itemDoubleClicked, this,
            [this](QTableWidgetItem *item) {
                if (!item)
                    return;
                const QString id = item->data(kCalendarIdRole).toString();
                if (!id.isEmpty())
                    openCalendarEventDialog(id);
                else if (item->data(Qt::UserRole).canConvert<QDate>())
                    openCalendarEventDialog(
                        QString(), item->data(Qt::UserRole).toDate());
            });
    connect(m_calendarWeekTable, &QTableWidget::itemDoubleClicked, this,
            [this](QTableWidgetItem *item) {
                if (item && !item->data(kCalendarIdRole).toString().isEmpty())
                    openCalendarEventDialog(
                        item->data(kCalendarIdRole).toString());
            });
    rebuildCalendarList();
    return page;
}

void MainWindow::requestCalendar(const QByteArray &method, const QString &path,
                                 const QJsonObject &body,
                                 CalendarReplyHandler handler)
{
    if (!m_networkAccess) {
        handler(false, {}, QStringLiteral("Calendar network access is unavailable."));
        return;
    }
    const QUrl relative(path);
    QUrl url = catalogApiUrl();
    url.setPath(relative.path());
    url.setQuery(relative.query());
    const QString token = accountSessionTokenForUrl(url);
    const QByteArray encoded = body.isEmpty()
        ? QByteArray()
        : QJsonDocument(body).toJson(QJsonDocument::Compact);
    QNetworkRequest request(url);
    request.setTransferTimeout(15000);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork);
    request.setRawHeader(QByteArrayLiteral("Accept"),
                         QByteArrayLiteral("application/json"));
    if (!token.isEmpty()) {
        request.setRawHeader(QByteArrayLiteral("Authorization"),
                             QByteArrayLiteral("Bearer ") + token.toUtf8());
    } else if (relative.path() != QLatin1String("/api/accounts/users")) {
        QString resource;
        const QString proof = calendarRequestProof(method, path, &resource);
        if (method == QByteArrayLiteral("POST") ||
            method == QByteArrayLiteral("PATCH")) {
            const QString digest = QString::fromLatin1(
                QCryptographicHash::hash(encoded, QCryptographicHash::Sha256)
                    .toHex());
            if (!resource.isEmpty())
                resource += QLatin1Char('\n');
            resource += digest;
        }
        QString node, ts, sig;
        if (proof.isEmpty() ||
            !signOrgTaskProof(proof, resource, &node, &ts, &sig)) {
            handler(false, {}, QStringLiteral(
                "Sign in to your ForkMesh account on this device to use calendars."));
            return;
        }
        QUrlQuery query(url);
        query.addQueryItem(QStringLiteral("node"), node);
        query.addQueryItem(QStringLiteral("ts"), ts);
        query.addQueryItem(QStringLiteral("sig"), sig);
        url.setQuery(query);
        request.setUrl(url);
    }
    if (!encoded.isEmpty())
        request.setHeader(QNetworkRequest::ContentTypeHeader,
                          QStringLiteral("application/json"));
    QNetworkReply *reply = method == QByteArrayLiteral("GET")
        ? m_networkAccess->get(request)
        : method == QByteArrayLiteral("POST")
        ? m_networkAccess->post(request, encoded)
        : m_networkAccess->sendCustomRequest(request, method, encoded);
    connect(reply, &QNetworkReply::finished, this,
            [reply, handler = std::move(handler)]() mutable {
        const QJsonObject payload =
            QJsonDocument::fromJson(reply->readAll()).object();
        const int status = reply->attribute(
            QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool ok = reply->error() == QNetworkReply::NoError &&
                        status >= 200 && status < 300 &&
                        payload.value(QStringLiteral("ok")).toBool(true);
        const QString error = ok ? QString() : calendarErrorText(
            payload, status ? QStringLiteral("HTTP %1").arg(status)
                            : reply->errorString());
        reply->deleteLater();
        handler(ok, payload, error);
    });
}

void MainWindow::refreshCalendar()
{
    if (!m_calendarStatus)
        return;
    // Opening the page re-reads Git; navigation between views does not. What
    // is already cached stays on screen while that happens, so activity does
    // not blink out and back on every visit.
    m_repositoryCalendarStale = true;
    m_repositoryCalendarRequest.clear();
    refreshRepositoryCalendarEvents();
    rebuildCalendarList();
    renderCalendar();
    if (m_calendarLoading)
        return;
    m_calendarLoading = true;
    m_calendarStatus->setText(QStringLiteral("Loading private calendars…"));
    const qint64 start = QDateTime(QDate::currentDate().addYears(-1),
                                   QTime(0, 0)).toMSecsSinceEpoch();
    const qint64 end = QDateTime(QDate::currentDate().addYears(2),
                                 QTime(23, 59, 59)).toMSecsSinceEpoch();
    requestCalendar(QByteArrayLiteral("GET"),
                    QStringLiteral("/api/calendar?start=%1&end=%2")
                        .arg(start).arg(end), {},
                    [this](bool ok, const QJsonObject &payload,
                           const QString &error) {
        m_calendarLoading = false;
        if (!ok) {
            m_calendarEvents = {};
            m_calendars = {};
            rebuildCalendarList();
            renderCalendar();
            if (m_repositoryCalendarEvents.isEmpty())
                m_calendarStatus->setText(error);
            return;
        }
        m_calendarEvents = payload.value(QStringLiteral("events")).toArray();
        m_calendars = payload.value(QStringLiteral("calendars")).toArray();
        m_calendarActor = payload.value(QStringLiteral("actor")).toString();
        rebuildCalendarList();
        renderCalendar();
        nativePackageSearchIndexChanged();
    });
}

// Work out which repository-months the current view needs, publish the ones
// already cached, and queue the rest. This used to walk every repository's Git
// history before the page could paint, on every navigation and every view
// change - which is what made the Calendar unusable once it showed every
// commit and merge.
void MainWindow::refreshRepositoryCalendarEvents()
{
    if (!m_calendarAnchor.isValid())
        return;
    const auto range = calendarRange(m_calendarAnchor, m_calendarMode);
    const auto months = repositoryCalendarMonths(range.first, range.second);

    QList<RepositoryCalendarScan> scans;
    QSet<QString> seenPaths;
    for (const RepositoryRecord &repo : std::as_const(m_repositories)) {
        QString gitPath = repo.localPath.trimmed();
        if (gitPath.isEmpty() || !QDir(gitPath).exists())
            gitPath = repo.mirrorPath.trimmed();
        if (gitPath.isEmpty() || !QDir(gitPath).exists())
            continue;
        QString pathKey = QFileInfo(gitPath).canonicalFilePath();
        if (pathKey.isEmpty())
            pathKey = QDir(gitPath).absolutePath();
        if (seenPaths.contains(pathKey))
            continue;
        seenPaths.insert(pathKey);
        QString label = repo.owner.trimmed();
        if (!label.isEmpty())
            label += QLatin1Char('/');
        label += repo.name.trimmed();
        for (const auto &month : months) {
            RepositoryCalendarScan scan;
            scan.gitPath = gitPath;
            scan.label = label;
            scan.key = pathKey + QLatin1Char('|') +
                       QString::number(month.first);
            scan.startAtMs = month.first;
            scan.endAtMs = month.second;
            scans.append(scan);
        }
    }

    QStringList signature;
    signature.reserve(scans.size());
    for (const RepositoryCalendarScan &scan : std::as_const(scans))
        signature << scan.key;
    const QString request = signature.join(QLatin1Char(','));
    if (request == m_repositoryCalendarRequest)
        return;
    m_repositoryCalendarRequest = request;

    m_repositoryCalendarScans = scans;
    m_repositoryCalendarQueue.clear();
    const bool stale = m_repositoryCalendarStale;
    m_repositoryCalendarStale = false;
    for (const RepositoryCalendarScan &scan : std::as_const(scans)) {
        if (stale || !m_repositoryCalendarMonths.contains(scan.key))
            m_repositoryCalendarQueue.append(scan);
    }
    m_repositoryCalendarCursor = 0;
    publishRepositoryCalendarEvents();
    if (!m_repositoryCalendarQueue.isEmpty())
        QTimer::singleShot(0, this, [this] { scanRepositoryCalendarSlice(); });
}

void MainWindow::scanRepositoryCalendarSlice()
{
    if (m_repositoryCalendarScanning ||
        m_repositoryCalendarCursor >= m_repositoryCalendarQueue.size())
        return;
    m_repositoryCalendarScanning = true;
    const QString request = m_repositoryCalendarRequest;
    const QList<RepositoryCalendarScan> pending =
        m_repositoryCalendarQueue.mid(m_repositoryCalendarCursor);
    runOffThread<QList<QJsonArray>>(
        [pending] {
            QList<QJsonArray> read;
            QElapsedTimer slice;
            slice.start();
            for (const RepositoryCalendarScan &scan : pending) {
                read.append(repositoryCalendarEvents(scan.gitPath, scan.label,
                                                     scan.startAtMs,
                                                     scan.endAtMs));
                if (slice.elapsed() >= kRepositoryCalendarSliceMs)
                    break;
            }
            return read;
        },
        [this, request, pending](QList<QJsonArray> read) {
            m_repositoryCalendarScanning = false;
            for (qsizetype i = 0; i < read.size() && i < pending.size(); ++i) {
                const QString &key = pending.at(i).key;
                if (!m_repositoryCalendarMonths.contains(key))
                    m_repositoryCalendarMonthOrder.append(key);
                m_repositoryCalendarMonths.insert(key, read.at(i));
                while (m_repositoryCalendarMonthOrder.size() >
                       kRepositoryCalendarCacheEntries) {
                    m_repositoryCalendarMonths.remove(
                        m_repositoryCalendarMonthOrder.takeFirst());
                }
            }
            if (request != m_repositoryCalendarRequest) {
                if (m_repositoryCalendarCursor < m_repositoryCalendarQueue.size())
                    QTimer::singleShot(0, this,
                                       [this] { scanRepositoryCalendarSlice(); });
                return;
            }
            if (read.isEmpty())
                return;
            m_repositoryCalendarCursor += int(read.size());
            publishRepositoryCalendarEvents();
            renderCalendar();
            if (m_repositoryCalendarCursor < m_repositoryCalendarQueue.size())
                QTimer::singleShot(0, this,
                                   [this] { scanRepositoryCalendarSlice(); });
        });
}

void MainWindow::publishRepositoryCalendarEvents()
{
    QJsonArray events;
    for (const RepositoryCalendarScan &scan :
         std::as_const(m_repositoryCalendarScans)) {
        const auto month = m_repositoryCalendarMonths.constFind(scan.key);
        if (month == m_repositoryCalendarMonths.constEnd())
            continue;
        for (const QJsonValue &event : month.value())
            events.append(event);
    }
    m_repositoryCalendarEvents = events;
}

void MainWindow::rebuildCalendarList()
{
    if (!m_calendarList)
        return;
    QHash<QString, Qt::CheckState> prior;
    for (int i = 0; i < m_calendarList->count(); ++i) {
        const QListWidgetItem *item = m_calendarList->item(i);
        prior.insert(item->data(kCalendarIdRole).toString(), item->checkState());
    }
    m_calendarList->blockSignals(true);
    m_calendarList->clear();
    auto append = [this, &prior](const QString &id, const QString &name,
                                 const QString &scope,
                                 const QString &tooltip) {
        auto *item = new QListWidgetItem(name);
        item->setData(kCalendarIdRole, id);
        item->setData(kCalendarScopeRole, scope);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(prior.value(id, Qt::Checked));
        item->setToolTip(tooltip);
        m_calendarList->addItem(item);
    };
    for (const QJsonValue &value : m_calendars) {
        const QJsonObject calendar = value.toObject();
        const QString scope = calendar.value(QStringLiteral("scope")).toString();
        append(calendar.value(QStringLiteral("id")).toString(),
               calendar.value(QStringLiteral("name")).toString(), scope,
               scope == QLatin1String("organization")
                   ? QStringLiteral("Visible only to organization members")
                   : QStringLiteral("Private to you and invited guests"));
    }
    append(kRepositoryCommitsCalendarId, QStringLiteral("Commits"),
           QStringLiteral("repository"),
           QStringLiteral("Read-only commits from every branch and tag in configured repositories"));
    append(kRepositoryMergesCalendarId, QStringLiteral("Merges"),
           QStringLiteral("repository"),
           QStringLiteral("Read-only merges from every branch and tag in configured repositories"));
    m_calendarList->blockSignals(false);
}

void MainWindow::renderCalendar()
{
    if (!m_calendarViews || !m_calendarMonthTable)
        return;
    QSet<QString> selected;
    for (int i = 0; i < m_calendarList->count(); ++i) {
        QListWidgetItem *item = m_calendarList->item(i);
        if (item->checkState() == Qt::Checked)
            selected.insert(item->data(kCalendarIdRole).toString());
    }
    const auto range = calendarRange(m_calendarAnchor, m_calendarMode);
    QDate rangeFirst = range.first;
    QDate rangeLast = range.second;
    int stackIndex = 0;
    if (m_calendarMode == QLatin1String("week")) {
        stackIndex = 1;
        m_calendarHeading->setText(QStringLiteral("Week of %1").arg(
            rangeFirst.toString(QStringLiteral("MMMM d, yyyy"))));
    } else if (m_calendarMode == QLatin1String("day")) {
        stackIndex = 2;
        m_calendarHeading->setText(
            rangeFirst.toString(QStringLiteral("dddd, MMMM d, yyyy")));
    } else if (m_calendarMode == QLatin1String("agenda")) {
        stackIndex = 3;
        m_calendarHeading->setText(QStringLiteral("Agenda from %1").arg(
            rangeFirst.toString(QStringLiteral("MMMM d, yyyy"))));
    } else {
        m_calendarHeading->setText(
            m_calendarAnchor.toString(QStringLiteral("MMMM yyyy")));
    }
    const QDateTime start(rangeFirst, QTime(0, 0));
    const QDateTime end(rangeLast, QTime(0, 0));
    // Read both lists in place. Concatenating them into one array first
    // copied every commit in the window on every redraw.
    const QString query = m_calendarSearch ? m_calendarSearch->text() : QString();
    QList<CalendarOccurrence> events;
    collectOccurrences(m_calendarEvents, start, end, selected, query, &events);
    collectOccurrences(m_repositoryCalendarEvents, start, end, selected, query,
                       &events);
    sortOccurrences(&events);
    // One pass into day buckets. Month and week used to re-scan the whole list
    // per rendered cell, converting a UTC stamp to local time each time.
    QHash<QDate, QList<const CalendarOccurrence *>> byDay;
    byDay.reserve(events.isEmpty() ? 1 : qMin<qsizetype>(events.size(), 128));
    for (const CalendarOccurrence &occurrence : events)
        byDay[occurrence.date].append(&occurrence);
    m_calendarViews->setCurrentIndex(stackIndex);
    m_calendarStatus->setText(QStringLiteral("%1 event%2 · %3").arg(
        events.size()).arg(events.size() == 1 ? QString() : QStringLiteral("s"),
                           QString::fromUtf8(QTimeZone::systemTimeZoneId())));

    if (stackIndex == 0) {
        m_calendarMonthTable->clearContents();
        const int rowHeight = calendarMonthRowHeight(m_calendarMonthTable);
        for (int row = 0; row < 6; ++row)
            m_calendarMonthTable->setRowHeight(row, rowHeight);
        for (int cell = 0; cell < 42; ++cell) {
            const QDate date = rangeFirst.addDays(cell);
            const QList<const CalendarOccurrence *> daily = byDay.value(date);
            QStringList lines{date.toString(QStringLiteral("d"))};
            QString firstId;
            // A day of repository activity can run to hundreds of commits; the
            // cell shows a readable preview and defers the rest to Day view.
            const int shown = int(qMin<qsizetype>(daily.size(), kCalendarMonthCellEvents));
            for (int index = 0; index < shown; ++index) {
                const CalendarOccurrence *occurrence = daily.at(index);
                if (firstId.isEmpty())
                    firstId = occurrence->event.value(
                        QStringLiteral("id")).toString();
                lines << QStringLiteral("%1 %2").arg(
                    occurrence->event.value(QStringLiteral("allDay")).toBool()
                        ? QStringLiteral("•")
                        : occurrence->time.toString(QStringLiteral("h:mm")),
                    occurrence->event.value(QStringLiteral("title")).toString());
            }
            if (daily.size() > shown)
                lines << QStringLiteral("+%1 more").arg(daily.size() - shown);
            auto *item = new QTableWidgetItem(lines.join(QLatin1Char('\n')));
            item->setData(Qt::UserRole, date);
            item->setData(kCalendarIdRole, firstId);
            item->setTextAlignment(Qt::AlignLeft | Qt::AlignTop);
            if (date.month() != m_calendarAnchor.month()) {
                item->setForeground(QColor(Theme::kTextTertiary));
                item->setData(kCalendarOutsideMonthRole, true);
            }
            m_calendarMonthTable->setItem(cell / 7, cell % 7, item);
        }
    } else if (stackIndex == 1) {
        m_calendarWeekTable->clearContents();
        QStringList labels;
        for (int day = 0; day < 7; ++day)
            labels << rangeFirst.addDays(day).toString(QStringLiteral("ddd d"));
        m_calendarWeekTable->setHorizontalHeaderLabels(labels);
        // Build each cell's text once. Appending to a QTableWidgetItem per
        // event re-copied the whole cell string for every commit in that hour,
        // and an hour of repository activity is not a handful of entries.
        struct WeekCell {
            QStringList lines;
            QString firstId;
            int total = 0;
        };
        QHash<int, WeekCell> cells;
        for (const CalendarOccurrence &occurrence : events) {
            const int column = rangeFirst.daysTo(occurrence.date);
            if (column < 0 || column > 6)
                continue;
            const int row =
                occurrence.event.value(QStringLiteral("allDay")).toBool()
                    ? 0 : occurrence.time.hour();
            WeekCell &cell = cells[row * 7 + column];
            cell.total += 1;
            if (cell.lines.size() < kCalendarWeekCellEvents) {
                cell.lines << occurrence.event.value(
                    QStringLiteral("title")).toString();
            }
            if (cell.firstId.isEmpty()) {
                cell.firstId =
                    occurrence.event.value(QStringLiteral("id")).toString();
            }
        }
        for (auto entry = cells.cbegin(); entry != cells.cend(); ++entry) {
            QStringList lines = entry.value().lines;
            if (entry.value().total > lines.size()) {
                lines << QStringLiteral("+%1 more").arg(
                    entry.value().total - lines.size());
            }
            auto *item = new QTableWidgetItem(lines.join(QLatin1Char('\n')));
            item->setData(kCalendarIdRole, entry.value().firstId);
            m_calendarWeekTable->setItem(entry.key() / 7, entry.key() % 7, item);
        }
    } else {
        QListWidget *list = stackIndex == 2 ? m_calendarDayList
                                            : m_calendarAgendaList;
        list->clear();
        if (m_calendarAgendaLimit <= 0)
            m_calendarAgendaLimit = kCalendarAgendaPage;
        const int shown = int(qMin<qsizetype>(events.size(), m_calendarAgendaLimit));
        list->setUpdatesEnabled(false);
        for (int index = 0; index < shown; ++index)
            list->addItem(agendaItem(events.at(index), stackIndex == 3));
        // The agenda spans 90 days, so with every commit on it the full list is
        // tens of thousands of rows. Offer the rest rather than build them all.
        if (events.size() > shown) {
            auto *more = new QListWidgetItem(QStringLiteral(
                "Show %1 more · %2 remaining")
                    .arg(qMin<qsizetype>(events.size() - shown,
                                         kCalendarAgendaPage))
                    .arg(events.size() - shown));
            more->setData(kCalendarMoreRole, true);
            more->setForeground(
                Theme::nextOr(Theme::Role::AccentFg, QColor(Theme::kRunning)));
            list->addItem(more);
        }
        list->setUpdatesEnabled(true);
        if (events.isEmpty()) {
            auto *empty = new QListWidgetItem(QStringLiteral(
                "No events in this period. Double-click a date in Month view "
                "or choose New event."));
            empty->setFlags(Qt::NoItemFlags);
            empty->setForeground(QColor(Theme::kTextTertiary));
            list->addItem(empty);
        }
    }
}

void MainWindow::openCalendarEventDialog(const QString &eventId,
                                         const QDate &date)
{
    QJsonObject event = calendarEvent(m_calendarEvents, eventId);
    if (event.isEmpty())
        event = calendarEvent(m_repositoryCalendarEvents, eventId);
    if (event.value(QStringLiteral("readOnly")).toBool()) {
        showRepositoryActivity(event, this);
        return;
    }
    const bool editing = !event.isEmpty();
    bool canManage = !editing ||
        event.value(QStringLiteral("owner")).toString() == m_calendarActor;
    if (editing && !canManage) {
        const QString eventCalendar =
            event.value(QStringLiteral("calendar")).toString();
        for (const QJsonValue &value : m_calendars) {
            const QJsonObject item = value.toObject();
            const QString role = item.value(QStringLiteral("role")).toString();
            if (item.value(QStringLiteral("id")).toString() == eventCalendar &&
                (role == QLatin1String("owner") || role == QLatin1String("admin"))) {
                canManage = true;
                break;
            }
        }
    }
    QDialog dialog(this);
    dialog.setWindowTitle(editing ? QStringLiteral("Edit event")
                                  : QStringLiteral("New event"));
    dialog.setWindowIcon(windowIcon());
    dialog.setMinimumWidth(640);
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(20, 18, 20, 18);
    layout->setSpacing(14);
    auto *formHost = new QWidget;
    auto *form = new QFormLayout(formHost);
    form->setContentsMargins(4, 4, 10, 4);
    form->setHorizontalSpacing(18);
    form->setVerticalSpacing(10);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    auto *title = new QLineEdit(event.value(QStringLiteral("title")).toString());
    title->setMaxLength(200);
    form->addRow(QStringLiteral("Title"), title);
    const QDateTime proposed = date.isValid()
        ? QDateTime(date, QTime(9, 0))
        : QDateTime::currentDateTime().addSecs(3600);
    const QTimeZone initialZone = calendarTimeZone(
        event.value(QStringLiteral("timezone")).toString(
            QString::fromUtf8(QTimeZone::systemTimeZoneId())));
    auto *start = new QDateTimeEdit(
        editing ? QDateTime::fromMSecsSinceEpoch(
                      eventStamp(event, QStringLiteral("startAt")), initialZone)
                : QDateTime(proposed.date(), proposed.time(), initialZone));
    start->setCalendarPopup(true);
    start->setDisplayFormat(QStringLiteral("yyyy-MM-dd  h:mm AP"));
    auto *end = new QDateTimeEdit(
        editing ? QDateTime::fromMSecsSinceEpoch(
                      eventStamp(event, QStringLiteral("endAt")), initialZone)
                : QDateTime(proposed.addSecs(3600).date(),
                            proposed.addSecs(3600).time(), initialZone));
    end->setCalendarPopup(true);
    end->setDisplayFormat(QStringLiteral("yyyy-MM-dd  h:mm AP"));
    form->addRow(QStringLiteral("Starts"), start);
    form->addRow(QStringLiteral("Ends"), end);
    auto *timezone = new QComboBox;
    const QDateTime zoneReference = QDateTime::currentDateTimeUtc();
    const QList<QByteArray> zoneIds = QTimeZone::availableTimeZoneIds();
    for (const QByteArray &zoneId : zoneIds) {
        const QTimeZone zone(zoneId);
        timezone->addItem(calendarTimeZoneLabel(zone, zoneReference),
                          QString::fromUtf8(zoneId));
    }
    int timezoneIndex = timezone->findData(QString::fromUtf8(initialZone.id()));
    timezone->setCurrentIndex(qMax(0, timezoneIndex));
    timezone->setMaxVisibleItems(20);
    timezone->setToolTip(QStringLiteral(
        "Start and end times are displayed and saved in this time zone."));
    form->addRow(QStringLiteral("Time zone"), timezone);
    connect(timezone, &QComboBox::currentIndexChanged, &dialog,
            [start, end, timezone] {
        const QTimeZone nextZone(timezone->currentData().toString().toUtf8());
        if (!nextZone.isValid())
            return;
        start->setDateTime(start->dateTime().toTimeZone(nextZone));
        end->setDateTime(end->dateTime().toTimeZone(nextZone));
    });
    auto *allDay = new QCheckBox(QStringLiteral("All-day event"));
    allDay->setChecked(event.value(QStringLiteral("allDay")).toBool());
    form->addRow(QString(), allDay);
    auto *calendar = new QComboBox;
    // Calendar loading is asynchronous, so New event can be clicked before
    // m_calendars arrives. Personal is an API invariant and gives the dialog a
    // valid, private destination instead of submitting an empty calendar id.
    calendar->addItem(QStringLiteral("My calendar · private"),
                      QStringLiteral("personal"));
    for (const QJsonValue &value : m_calendars) {
        const QJsonObject item = value.toObject();
        if (item.value(QStringLiteral("id")).toString() ==
            QLatin1String("personal"))
            continue;
        calendar->addItem(
            item.value(QStringLiteral("name")).toString() +
                (item.value(QStringLiteral("scope")).toString() ==
                         QLatin1String("organization")
                     ? QStringLiteral(" · team") : QStringLiteral(" · private")),
            item.value(QStringLiteral("id")).toString());
    }
    const int calendarIndex = calendar->findData(
        event.value(QStringLiteral("calendar")).toString(
            QStringLiteral("personal")));
    calendar->setCurrentIndex(qMax(0, calendarIndex));
    calendar->setEnabled(!editing);
    form->addRow(QStringLiteral("Calendar"), calendar);
    auto *recurrence = new QComboBox;
    recurrence->addItem(QStringLiteral("Does not repeat"), QStringLiteral("none"));
    recurrence->addItem(QStringLiteral("Daily"), QStringLiteral("daily"));
    recurrence->addItem(QStringLiteral("Weekly"), QStringLiteral("weekly"));
    recurrence->addItem(QStringLiteral("Monthly"), QStringLiteral("monthly"));
    recurrence->addItem(QStringLiteral("Yearly"), QStringLiteral("yearly"));
    recurrence->setCurrentIndex(qMax(0, recurrence->findData(
        event.value(QStringLiteral("recurrence")).toString(
            QStringLiteral("none")))));
    form->addRow(QStringLiteral("Repeats"), recurrence);
    auto *location = new QLineEdit(
        event.value(QStringLiteral("location")).toString());
    form->addRow(QStringLiteral("Location"), location);
    auto *meeting = new QLineEdit(
        event.value(QStringLiteral("meetingUrl")).toString());
    meeting->setPlaceholderText(QStringLiteral("https://…"));
    form->addRow(QStringLiteral("Meeting link"), meeting);
    QStringList attendeeNames;
    for (const QJsonValue &value : event.value(QStringLiteral("attendees")).toArray())
        attendeeNames << value.toObject().value(QStringLiteral("name")).toString();
    auto *attendees = new QLineEdit(attendeeNames.join(QStringLiteral(", ")));
    attendees->setPlaceholderText(QStringLiteral(
        "Type an organization member or ForkMesh user"));
    form->addRow(QStringLiteral("Invitees"), attendees);
    auto *inviteeModel = new QStringListModel(&dialog);
    auto *inviteeCompleter = new QCompleter(inviteeModel, &dialog);
    inviteeCompleter->setWidget(attendees);
    inviteeCompleter->setCaseSensitivity(Qt::CaseInsensitive);
    inviteeCompleter->setFilterMode(Qt::MatchContains);
    inviteeCompleter->setCompletionMode(QCompleter::PopupCompletion);
    auto systemInvitees = QSharedPointer<QStringList>::create();
    auto organizationInvitees = QSharedPointer<QStringList>::create();
    auto refreshInviteeModel =
        [inviteeModel, systemInvitees, organizationInvitees, this] {
        QStringList names = *organizationInvitees;
        names.append(*systemInvitees);
        names.removeAll(m_calendarActor);
        names.removeDuplicates();
        const QSet<QString> members(organizationInvitees->cbegin(),
                                    organizationInvitees->cend());
        std::sort(names.begin(), names.end(), [&members](const QString &left,
                                                         const QString &right) {
            if (members.contains(left) != members.contains(right))
                return members.contains(left);
            return left.localeAwareCompare(right) < 0;
        });
        inviteeModel->setStringList(names);
    };
    auto loadInvitees = [this, guard = QPointer<QDialog>(&dialog),
                         systemInvitees, organizationInvitees,
                         refreshInviteeModel](const QString &calendarId) {
        organizationInvitees->clear();
        if (calendarId != QLatin1String("personal")) {
            for (const QJsonValue &value : m_calendars) {
                const QJsonObject item = value.toObject();
                if (item.value(QStringLiteral("id")).toString() != calendarId)
                    continue;
                for (const QJsonValue &member :
                     item.value(QStringLiteral("members")).toArray()) {
                    const QString name = member.toString().trimmed().toLower();
                    if (!name.isEmpty())
                        organizationInvitees->append(name);
                }
                break;
            }
        }
        refreshInviteeModel();
        requestCalendar(
            QByteArrayLiteral("GET"), QStringLiteral("/api/accounts/users"), {},
            [guard, systemInvitees, refreshInviteeModel](
                bool ok, const QJsonObject &payload, const QString &) {
                if (!guard || !ok)
                    return;
                systemInvitees->clear();
                for (const QJsonValue &value : payload.value(
                         QStringLiteral("users")).toArray()) {
                    const QString name = value.toObject()
                                             .value(QStringLiteral("name"))
                                             .toString().trimmed().toLower();
                    if (!name.isEmpty())
                        systemInvitees->append(name);
                }
                refreshInviteeModel();
            });
    };
    connect(attendees, &QLineEdit::textEdited, &dialog,
            [attendees, inviteeCompleter](const QString &text) {
        const QString prefix = text.section(QLatin1Char(','), -1).trimmed();
        if (prefix.isEmpty()) {
            inviteeCompleter->popup()->hide();
            return;
        }
        inviteeCompleter->setCompletionPrefix(prefix);
        inviteeCompleter->complete();
    });
    connect(inviteeCompleter,
            QOverload<const QString &>::of(&QCompleter::activated), &dialog,
            [attendees](const QString &name) {
        QStringList names = attendees->text().split(QLatin1Char(','),
                                                     Qt::KeepEmptyParts);
        if (names.isEmpty())
            names.append(name);
        else
            names.last() = name;
        for (QString &value : names)
            value = value.trimmed();
        names.removeAll(QString());
        attendees->setText(names.join(QStringLiteral(", ")) +
                           QStringLiteral(", "));
        attendees->setFocus();
    });
    connect(calendar, &QComboBox::currentIndexChanged, &dialog,
            [calendar, loadInvitees] {
        loadInvitees(calendar->currentData().toString());
    });
    loadInvitees(calendar->currentData().toString());
    auto *description = new QTextEdit;
    description->setPlainText(
        event.value(QStringLiteral("description")).toString());
    description->setFixedHeight(74);
    form->addRow(QStringLiteral("Description"), description);
    auto *agentHost = new QFrame;
    agentHost->setObjectName(QStringLiteral("calendarAgentCard"));
    auto *agentLayout = new QVBoxLayout(agentHost);
    agentLayout->setContentsMargins(16, 14, 16, 14);
    agentLayout->setSpacing(8);
    auto *agentHeading = new QLabel(QStringLiteral("Describe your event"));
    agentHeading->setObjectName(QStringLiteral("calendarAgentHeading"));
    agentLayout->addWidget(agentHeading);
    auto *agentHint = new QLabel(QStringLiteral(
        "Use natural language and Codex or Claude Code will fill the draft for you."));
    agentHint->setObjectName(QStringLiteral("mutedLabel"));
    agentLayout->addWidget(agentHint);
    auto *agentPrompt = new QTextEdit;
    agentPrompt->setObjectName(QStringLiteral("calendarAgentPrompt"));
    agentPrompt->setAcceptRichText(false);
    agentPrompt->setFixedHeight(62);
    agentPrompt->setPlaceholderText(QStringLiteral(
        "Schedule a meeting tomorrow with Kaif at 3:30 pm IST"));
    agentLayout->addWidget(agentPrompt);
    auto *agentRow = new QHBoxLayout;
    agentRow->setSpacing(8);
    auto *agentProviderLabel = new QLabel(QStringLiteral("Plan with"));
    agentProviderLabel->setObjectName(QStringLiteral("mutedLabel"));
    auto *agentModel = new QComboBox;
    agentModel->setObjectName(QStringLiteral("calendarAgentProvider"));
    agentModel->addItem(QStringLiteral("Codex"), QStringLiteral("codex"));
    agentModel->addItem(QStringLiteral("Claude Code"),
                        QStringLiteral("claude-code"));
    QSettings agentSettings;
    const QString defaultAgent = agentSettings
                                     .value(kDefaultAgentProviderSetting,
                                            QStringLiteral("codex"))
                                     .toString();
    const QString preferredAgent = agentSettings
                                       .value(QStringLiteral(
                                                  "calendar/agentProvider"),
                                              defaultAgent)
                                       .toString();
    const int preferredAgentIndex = agentModel->findData(preferredAgent);
    if (preferredAgentIndex >= 0)
        agentModel->setCurrentIndex(preferredAgentIndex);
    connect(agentModel, &QComboBox::currentIndexChanged, &dialog,
            [agentModel] {
        QSettings().setValue(QStringLiteral("calendar/agentProvider"),
                             agentModel->currentData().toString());
    });
    auto *agentSuggest = new QPushButton(QStringLiteral("Fill event"));
    agentSuggest->setObjectName(QStringLiteral("scmSyncButton"));
    setOcticon(agentSuggest, "calendar", 14);
    agentRow->addWidget(agentProviderLabel);
    agentRow->addWidget(agentModel);
    agentRow->addStretch();
    agentRow->addWidget(agentSuggest);
    agentLayout->addLayout(agentRow);
    auto *agentStatus = new QLabel(QStringLiteral(
        "Uses your signed-in local CLI account. Nothing is created until you save."));
    agentStatus->setObjectName(QStringLiteral("mutedLabel"));
    agentStatus->setWordWrap(true);
    agentLayout->addWidget(agentStatus);
    layout->addWidget(agentHost);
    connect(agentSuggest, &QPushButton::clicked, &dialog,
            [&, this] {
        const QString requestText = agentPrompt->toPlainText().trimmed();
        if (requestText.isEmpty()) {
            agentStatus->setText(QStringLiteral(
                "Describe the event you want to schedule first."));
            agentPrompt->setFocus();
            return;
        }
        const bool claude = agentModel->currentData().toString() ==
                            QLatin1String("claude-code");
        const QString zoneName = timezone->currentData().toString();
        QJsonArray busy;
        QSet<QString> visibleCalendars;
        for (const QJsonValue &value : m_calendars)
            visibleCalendars.insert(
                value.toObject().value(QStringLiteral("id")).toString());
        const QDateTime busyRangeStart = QDateTime::currentDateTime();
        const QList<CalendarOccurrence> busyEvents = expandedEvents(
            m_calendarEvents, busyRangeStart, busyRangeStart.addYears(1),
            visibleCalendars, QString());
        for (const CalendarOccurrence &item : busyEvents) {
            if (item.event.value(QStringLiteral("id")).toString() == eventId)
                continue;
            const qint64 busyStart = item.startAtMs;
            const qint64 busyEnd = item.endAtMs;
            if (busyStart <= 0 || busyEnd <= busyStart)
                continue;
            busy.append(QJsonObject{
                {QStringLiteral("startAt"), static_cast<double>(busyStart)},
                {QStringLiteral("endAt"), static_cast<double>(busyEnd)},
            });
            if (busy.size() >= 100)
                break;
        }
        const QJsonObject currentDraft{
            {QStringLiteral("title"), title->text().trimmed()},
            {QStringLiteral("start"), start->dateTime().toString(Qt::ISODate)},
            {QStringLiteral("end"), end->dateTime().toString(Qt::ISODate)},
            {QStringLiteral("timezone"), zoneName},
            {QStringLiteral("invitees"), attendees->text().trimmed()},
            {QStringLiteral("location"), location->text().trimmed()},
            {QStringLiteral("meeting_url"), meeting->text().trimmed()},
            {QStringLiteral("description"), description->toPlainText().trimmed()},
            {QStringLiteral("all_day"), allDay->isChecked()},
            {QStringLiteral("recurrence"), recurrence->currentData().toString()},
        };
        const QString task = QStringLiteral(
            "You are filling a calendar event draft. Follow the user's request "
            "and avoid every busy interval. Preserve useful existing draft values "
            "unless the request replaces them. Infer a concise title and invitee "
            "names. When no duration is given, use one hour. Resolve common time "
            "zone abbreviations from context; for an India-context IST request use "
            "Asia/Kolkata. Return only one JSON object with exactly these fields: "
            "title (string), start (ISO 8601 with numeric UTC offset), end (ISO "
            "8601 with numeric UTC offset), timezone (IANA identifier), invitees "
            "(array of strings), location (string), meeting_url (string), "
            "description (string), all_day (boolean), recurrence (one of none, "
            "daily, weekly, monthly, yearly), and explanation (string). Use empty "
            "strings or an empty array for details the user did not provide. Do "
            "not use tools or Markdown.\nCurrent time: %1\nDisplay time zone: "
            "%2\nRequest: %3\nCurrent draft: %4\nBusy intervals (Unix "
            "milliseconds): %5")
            .arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs),
                 zoneName, requestText,
                 QString::fromUtf8(QJsonDocument(currentDraft).toJson(
                     QJsonDocument::Compact)),
                 QString::fromUtf8(QJsonDocument(busy).toJson(
                     QJsonDocument::Compact)));
        agentSuggest->setEnabled(false);
        agentSuggest->setText(QStringLiteral("Planning…"));
        agentStatus->setText(QStringLiteral(
            "Asking %1 to fill the draft from your request…")
                                 .arg(agentModel->currentText()));
        QPointer<QPushButton> buttonGuard(agentSuggest);
        QPointer<QLabel> statusGuard(agentStatus);
        QPointer<QLineEdit> titleGuard(title);
        QPointer<QDateTimeEdit> startGuard(start);
        QPointer<QDateTimeEdit> endGuard(end);
        QPointer<QComboBox> timezoneGuard(timezone);
        QPointer<QLineEdit> attendeesGuard(attendees);
        QPointer<QLineEdit> locationGuard(location);
        QPointer<QLineEdit> meetingGuard(meeting);
        QPointer<QTextEdit> descriptionGuard(description);
        QPointer<QCheckBox> allDayGuard(allDay);
        QPointer<QComboBox> recurrenceGuard(recurrence);
        auto handled = QSharedPointer<bool>::create(false);
        auto errorText = QSharedPointer<QString>::create();
        auto session = QSharedPointer<QPointer<QObject>>::create();
        auto *requestTimeout = new QTimer(&dialog);
        requestTimeout->setSingleShot(true);
        auto finishRequest = [buttonGuard, statusGuard, titleGuard, startGuard,
                              endGuard, timezoneGuard, attendeesGuard,
                              locationGuard, meetingGuard, descriptionGuard,
                              allDayGuard, recurrenceGuard, handled,
                              session, requestTimeout](const QJsonObject &result) {
            if (*handled)
                return;
            *handled = true;
            requestTimeout->stop();
            QTimer::singleShot(0, [session] {
                if (auto *codexSession = qobject_cast<CodexAppServerSession *>(
                        session->data()))
                    codexSession->stop();
                else if (auto *claudeSession =
                             qobject_cast<ClaudeStreamSession *>(session->data()))
                    claudeSession->stop();
            });
            if (buttonGuard) {
                buttonGuard->setEnabled(true);
                buttonGuard->setText(QStringLiteral("Fill event"));
            }
            if (!statusGuard || !titleGuard || !startGuard || !endGuard ||
                !timezoneGuard || !attendeesGuard || !locationGuard ||
                !meetingGuard || !descriptionGuard || !allDayGuard ||
                !recurrenceGuard)
                return;
            if (result.value(QStringLiteral("is_error")).toBool()) {
                QString failure = result.value(QStringLiteral("result"))
                                      .toString().trimmed();
                if (failure.isEmpty())
                    failure = QStringLiteral(
                        "The local agent could not plan this event.");
                statusGuard->setText(failure);
                return;
            }
            const QJsonObject suggestion = calendarAgentSuggestion(
                result.value(QStringLiteral("result")).toString());
            const QDateTime proposedStart = QDateTime::fromString(
                suggestion.value(QStringLiteral("start")).toString(), Qt::ISODate);
            const QDateTime proposedEnd = QDateTime::fromString(
                suggestion.value(QStringLiteral("end")).toString(), Qt::ISODate);
            QTimeZone proposedZone(
                suggestion.value(QStringLiteral("timezone")).toString().toUtf8());
            if (!proposedZone.isValid())
                proposedZone = QTimeZone(
                    timezoneGuard->currentData().toString().toUtf8());
            if (!proposedStart.isValid() || !proposedEnd.isValid() ||
                proposedEnd <= proposedStart) {
                statusGuard->setText(QStringLiteral(
                    "The agent did not return a usable time. Try a more specific request."));
                return;
            }
            const QString proposedTitle = suggestion
                                              .value(QStringLiteral("title"))
                                              .toString().trimmed();
            if (!proposedTitle.isEmpty())
                titleGuard->setText(proposedTitle);
            const QString zoneId = QString::fromUtf8(proposedZone.id());
            const int zoneIndex = timezoneGuard->findData(zoneId);
            if (zoneIndex >= 0)
                timezoneGuard->setCurrentIndex(zoneIndex);
            startGuard->setDateTime(proposedStart.toTimeZone(proposedZone));
            endGuard->setDateTime(proposedEnd.toTimeZone(proposedZone));
            QStringList inviteeNames;
            for (const QJsonValue &value : suggestion
                                               .value(QStringLiteral("invitees"))
                                               .toArray()) {
                const QString name = value.toString().trimmed();
                if (!name.isEmpty())
                    inviteeNames.append(name);
            }
            if (!inviteeNames.isEmpty())
                attendeesGuard->setText(inviteeNames.join(QStringLiteral(", ")));
            const QString proposedLocation = suggestion
                                                 .value(QStringLiteral("location"))
                                                 .toString().trimmed();
            if (!proposedLocation.isEmpty())
                locationGuard->setText(proposedLocation);
            const QString proposedMeeting = suggestion
                                                .value(QStringLiteral("meeting_url"))
                                                .toString().trimmed();
            if (!proposedMeeting.isEmpty())
                meetingGuard->setText(proposedMeeting);
            const QString proposedDescription = suggestion
                                                    .value(QStringLiteral("description"))
                                                    .toString().trimmed();
            if (!proposedDescription.isEmpty())
                descriptionGuard->setPlainText(proposedDescription);
            if (suggestion.contains(QStringLiteral("all_day")))
                allDayGuard->setChecked(
                    suggestion.value(QStringLiteral("all_day")).toBool());
            const QString proposedRecurrence = suggestion
                                                   .value(QStringLiteral("recurrence"))
                                                   .toString().trimmed().toLower();
            if (!proposedRecurrence.isEmpty()) {
                const int recurrenceIndex = recurrenceGuard->findData(
                    proposedRecurrence);
                if (recurrenceIndex >= 0)
                    recurrenceGuard->setCurrentIndex(recurrenceIndex);
            }
            const QString explanation = suggestion
                .value(QStringLiteral("explanation")).toString().trimmed();
            statusGuard->setText(explanation.isEmpty()
                ? QStringLiteral("Event draft filled. Review it before saving.")
                : explanation + QStringLiteral(" Review before saving."));
        };
        auto onEvent = [finishRequest](const QJsonObject &agentEvent) {
            if (agentEvent.value(QStringLiteral("type")).toString() ==
                QLatin1String("result"))
                finishRequest(agentEvent);
        };
        auto onError = [errorText](const QString &text) {
            if (errorText->size() < 1200)
                errorText->append(text);
        };
        auto onFinished = [buttonGuard, statusGuard, handled, errorText,
                           requestTimeout](int) {
            requestTimeout->stop();
            if (*handled)
                return;
            *handled = true;
            if (buttonGuard) {
                buttonGuard->setEnabled(true);
                buttonGuard->setText(QStringLiteral("Fill event"));
            }
            if (!statusGuard)
                return;
            QString failure = errorText->trimmed();
            if (failure.isEmpty())
                failure = QStringLiteral(
                    "The local agent did not return an event. Make sure its CLI is installed and signed in.");
            statusGuard->setText(failure);
        };
        connect(requestTimeout, &QTimer::timeout, &dialog,
                [session, errorText, handled, buttonGuard, statusGuard] {
            if (*handled)
                return;
            *handled = true;
            *errorText = QStringLiteral(
                "The local agent took too long to respond. Try again in a moment.");
            if (buttonGuard) {
                buttonGuard->setEnabled(true);
                buttonGuard->setText(QStringLiteral("Fill event"));
            }
            if (statusGuard)
                statusGuard->setText(*errorText);
            if (auto *codexSession = qobject_cast<CodexAppServerSession *>(
                    session->data()))
                codexSession->stop();
            else if (auto *claudeSession =
                         qobject_cast<ClaudeStreamSession *>(session->data()))
                claudeSession->stop();
        });
        requestTimeout->start(120000);
        const QString workdir = workingClientDir();
        if (claude) {
            auto *transport = new ClaudeStreamSession(&dialog);
            *session = transport;
            connect(transport, &ClaudeStreamSession::event, &dialog, onEvent);
            connect(transport, &ClaudeStreamSession::stderrText, &dialog, onError);
            connect(transport, &ClaudeStreamSession::finished, &dialog, onFinished);
            QString model = QSettings().value(kClaudeCodeModelSetting)
                                .toString().trimmed();
            if (model == kClaudeAutoModelId ||
                !agentModelMatchesProvider(QStringLiteral("claude-code"), model))
                model.clear();
            QStringList env{QStringLiteral("ANTHROPIC_API_KEY"),
                            QStringLiteral("ANTHROPIC_AUTH_TOKEN"),
                            QStringLiteral("ANTHROPIC_ADMIN_KEY"),
                            QStringLiteral("CLAUDE_CODE_OAUTH_TOKEN")};
            env << activeAgentAccountEnv(QStringLiteral("claude-code"));
            transport->start(workdir, env, task, QStringLiteral("plan"),
                             QString(), model, QStringLiteral("low"));
        } else {
            auto *transport = new CodexAppServerSession(&dialog);
            *session = transport;
            connect(transport, &CodexAppServerSession::event, &dialog, onEvent);
            connect(transport, &CodexAppServerSession::stderrText, &dialog, onError);
            connect(transport, &CodexAppServerSession::finished, &dialog, onFinished);
            QString model = codexChatGptModelId(
                QSettings().value(kCodexModelSetting).toString().trimmed());
            if (!agentModelMatchesProvider(QStringLiteral("codex"), model))
                model.clear();
            QStringList env{QStringLiteral("OPENAI_API_KEY"),
                            QStringLiteral("CODEX_API_KEY"),
                            QStringLiteral("OPENAI_ACCESS_TOKEN"),
                            QStringLiteral("OPENAI_ADMIN_KEY")};
            env << activeAgentAccountEnv(QStringLiteral("codex"));
            transport->start(workdir, env, task, QString(), model,
                             QStringLiteral("Plan"), QStringLiteral("low"));
        }
    });
    connect(agentPrompt, &QTextEdit::textChanged, &dialog,
            [agentPrompt, agentSuggest] {
        agentSuggest->setEnabled(!agentPrompt->toPlainText().trimmed().isEmpty());
    });
    agentSuggest->setEnabled(false);
    auto *reminderHost = new QWidget;
    auto *reminderLayout = new QHBoxLayout(reminderHost);
    reminderLayout->setContentsMargins(0, 0, 0, 0);
    auto *dayReminder = new QCheckBox(QStringLiteral("24 hours"));
    auto *hourReminder = new QCheckBox(QStringLiteral("1 hour"));
    auto *tenReminder = new QCheckBox(QStringLiteral("10 minutes"));
    const QJsonArray reminders = editing
        ? event.value(QStringLiteral("reminders")).toArray()
        : QJsonArray{1440, 60, 10};
    auto hasReminder = [&reminders](int minutes) {
        for (const QJsonValue &value : reminders)
            if (value.toInt() == minutes)
                return true;
        return false;
    };
    dayReminder->setChecked(hasReminder(1440));
    hourReminder->setChecked(hasReminder(60));
    tenReminder->setChecked(hasReminder(10));
    reminderLayout->addWidget(dayReminder);
    reminderLayout->addWidget(hourReminder);
    reminderLayout->addWidget(tenReminder);
    form->addRow(QStringLiteral("Notifications"), reminderHost);
    auto *detailsScroll = new QScrollArea;
    detailsScroll->setObjectName(QStringLiteral("calendarDetailsScroll"));
    detailsScroll->setFrameShape(QFrame::NoFrame);
    detailsScroll->setWidgetResizable(true);
    detailsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    detailsScroll->setWidget(formHost);
    layout->addWidget(detailsScroll, 1);
    auto *privacy = new QLabel(QStringLiteral(
        "Times are displayed in %1. Event details are encrypted at rest.")
                                   .arg(timezone->currentData().toString()));
    privacy->setObjectName(QStringLiteral("mutedLabel"));
    layout->addWidget(privacy);
    connect(timezone, &QComboBox::currentIndexChanged, &dialog,
            [privacy, timezone] {
        privacy->setText(QStringLiteral(
            "Times are displayed in %1. Event details are encrypted at rest.")
                             .arg(timezone->currentData().toString()));
    });

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    auto *save = buttons->addButton(QStringLiteral("Save event"),
                                    QDialogButtonBox::AcceptRole);
    save->setObjectName(QStringLiteral("primaryButton"));
    save->setVisible(canManage);
    QPushButton *remove = nullptr;
    if (editing && canManage) {
        remove = buttons->addButton(QStringLiteral("Delete"),
                                    QDialogButtonBox::DestructiveRole);
        remove->setObjectName(QStringLiteral("dangerButton"));
    }
    layout->addWidget(buttons);
    if (editing && !canManage) {
        const QList<QWidget *> readOnlyFields{
            title, start, end, timezone, allDay, calendar, recurrence, location,
            meeting, attendees, description, agentHost, reminderHost};
        for (QWidget *field : readOnlyFields)
            field->setEnabled(false);
    }
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(save, &QPushButton::clicked, &dialog, [&, this] {
        if (title->text().trimmed().isEmpty() || end->dateTime() <= start->dateTime()) {
            QMessageBox::warning(&dialog, QStringLiteral("Invalid event"),
                                 QStringLiteral(
                                     "Add a title and choose an end after the start."));
            return;
        }
        const QString calendarId = calendar->currentData().toString();
        QJsonArray invitees;
        for (const QString &name : attendees->text().split(QLatin1Char(','),
                                                           Qt::SkipEmptyParts))
            invitees.append(name.trimmed().toLower());
        QJsonArray selectedReminders;
        if (dayReminder->isChecked()) selectedReminders.append(1440);
        if (hourReminder->isChecked()) selectedReminders.append(60);
        if (tenReminder->isChecked()) selectedReminders.append(10);
        const QJsonObject payload{
            {QStringLiteral("title"), title->text().trimmed()},
            {QStringLiteral("startAt"), static_cast<double>(
                start->dateTime().toMSecsSinceEpoch())},
            {QStringLiteral("endAt"), static_cast<double>(
                end->dateTime().toMSecsSinceEpoch())},
            {QStringLiteral("allDay"), allDay->isChecked()},
            {QStringLiteral("scope"), calendarId == QLatin1String("personal")
                ? QStringLiteral("personal") : QStringLiteral("organization")},
            {QStringLiteral("calendar"), calendarId},
            {QStringLiteral("timezone"), timezone->currentData().toString()},
            {QStringLiteral("recurrence"), recurrence->currentData().toString()},
            {QStringLiteral("location"), location->text().trimmed()},
            {QStringLiteral("meetingUrl"), meeting->text().trimmed()},
            {QStringLiteral("attendees"), invitees},
            {QStringLiteral("description"), description->toPlainText()},
            {QStringLiteral("reminders"), selectedReminders},
        };
        save->setEnabled(false);
        QPointer<QDialog> guard(&dialog);
        QPointer<QPushButton> saveGuard(save);
        requestCalendar(editing ? QByteArrayLiteral("PATCH")
                                : QByteArrayLiteral("POST"),
                        editing ? QStringLiteral("/api/calendar/%1").arg(eventId)
                                : QStringLiteral("/api/calendar"),
                        payload,
                        [this, guard, saveGuard](bool ok, const QJsonObject &,
                                                 const QString &error) {
            if (!guard)
                return;
            if (!ok) {
                if (saveGuard)
                    saveGuard->setEnabled(true);
                QMessageBox::warning(guard, QStringLiteral("Calendar"), error);
                return;
            }
            guard->accept();
            refreshCalendar();
        });
    });
    if (remove) {
        connect(remove, &QPushButton::clicked, &dialog, [&, this] {
            if (QMessageBox::question(
                    &dialog, QStringLiteral("Delete event"),
                    QStringLiteral("Delete this event for everyone who can see it?"))
                != QMessageBox::Yes)
                return;
            QPointer<QDialog> guard(&dialog);
            requestCalendar(QByteArrayLiteral("DELETE"),
                            QStringLiteral("/api/calendar/%1").arg(eventId), {},
                            [this, guard](bool ok, const QJsonObject &,
                                          const QString &error) {
                if (!guard)
                    return;
                if (!ok) {
                    QMessageBox::warning(guard, QStringLiteral("Calendar"), error);
                    return;
                }
                guard->accept();
                refreshCalendar();
            });
        });
    }

    QString invitationStatus;
    for (const QJsonValue &value : event.value(QStringLiteral("attendees")).toArray()) {
        const QJsonObject attendee = value.toObject();
        if (attendee.value(QStringLiteral("name")).toString() == m_calendarActor) {
            invitationStatus = attendee.value(QStringLiteral("status")).toString();
            break;
        }
    }
    if (!invitationStatus.isEmpty()) {
        auto *rsvp = new QHBoxLayout;
        rsvp->addWidget(new QLabel(QStringLiteral("Your response: %1").arg(
            invitationStatus)));
        rsvp->addStretch();
        for (const auto &choice : {
                 qMakePair(QStringLiteral("Accept"), QStringLiteral("accepted")),
                 qMakePair(QStringLiteral("Tentative"), QStringLiteral("tentative")),
                 qMakePair(QStringLiteral("Decline"), QStringLiteral("declined"))}) {
            auto *button = new QPushButton(choice.first);
            button->setObjectName(QStringLiteral("ghostButton"));
            connect(button, &QPushButton::clicked, &dialog,
                    [&, this, status = choice.second] {
                requestCalendar(
                    QByteArrayLiteral("POST"),
                    QStringLiteral("/api/calendar/%1/rsvp").arg(eventId),
                    QJsonObject{{QStringLiteral("status"), status}},
                    [this, guard = QPointer<QDialog>(&dialog)](
                        bool ok, const QJsonObject &, const QString &error) {
                    if (!guard)
                        return;
                    if (!ok) {
                        QMessageBox::warning(guard, QStringLiteral("Calendar"),
                                             error);
                        return;
                    }
                    guard->accept();
                    refreshCalendar();
                });
            });
            rsvp->addWidget(button);
        }
        layout->insertLayout(layout->count() - 1, rsvp);
    }
    const int availableHeight = dialog.screen()
        ? dialog.screen()->availableGeometry().height()
        : 800;
    dialog.resize(700, qBound(560, availableHeight - 80, 760));
    if (editing)
        title->setFocus();
    else
        agentPrompt->setFocus();
    dialog.exec();
}
