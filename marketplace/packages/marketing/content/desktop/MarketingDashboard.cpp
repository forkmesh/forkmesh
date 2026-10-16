#include "MarketingDashboard.h"

#include <QAbstractButton>
#include <QApplication>
#include <QButtonGroup>
#include <QDateTime>
#include <QEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHash>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QStyle>
#include <QTime>
#include <QToolTip>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace forkmesh::marketing {
namespace {

QString rgba(const QColor &color)
{
    return color.name(QColor::HexArgb);
}

QColor alpha(const QColor &color, double value)
{
    QColor result = color;
    result.setAlphaF(value);
    return result;
}

QFont sizedFont(const QFont &base, int pixels, int weight = QFont::Normal)
{
    QFont font = base;
    font.setPixelSize(pixels);
    font.setWeight(QFont::Weight(weight));
    return font;
}

double niceStep(double max, int ticks = 4)
{
    const double raw = std::max(max, 1.0) / ticks;
    const double power = std::pow(10.0, std::floor(std::log10(raw)));
    const double unit = raw / power;
    return (unit <= 1 ? 1 : unit <= 2 ? 2 : unit <= 2.5 ? 2.5 : unit <= 5 ? 5 : 10) * power;
}

QString compactNumber(double value)
{
    const auto shortened = [](double scaled, const char *suffix) {
        QString number = QString::number(scaled, 'f', scaled < 100 ? 1 : 0);
        if (number.endsWith(QLatin1String(".0")))
            number.chop(2);
        return number + QLatin1String(suffix);
    };
    if (std::abs(value) >= 1000000)
        return shortened(value / 1000000.0, "M");
    if (std::abs(value) >= 1000)
        return shortened(value / 1000.0, "k");
    return QString::number(qint64(std::llround(value)));
}

} // namespace

DashboardColors DashboardColors::from(const QWidget *widget)
{
    DashboardColors colors;
    colors.dark = !widget || widget->palette().color(QPalette::WindowText).lightness() >= 128;
    if (colors.dark) {
        colors.fg = QColor(QStringLiteral("#e6edf3"));
        colors.muted = QColor(QStringLiteral("#8b949e"));
        colors.line = QColor(QStringLiteral("#30363d"));
        colors.lineSoft = QColor(QStringLiteral("#21262d"));
        colors.card = QColor(QStringLiteral("#161b22"));
        colors.raised = QColor(QStringLiteral("#1c2128"));
        colors.page = QColor(QStringLiteral("#0d1117"));
        colors.good = QColor(QStringLiteral("#3fb950"));
        colors.goodSoft = QColor(46, 160, 67, 38);
        colors.warn = QColor(QStringLiteral("#d29922"));
        colors.warnSoft = QColor(187, 128, 9, 38);
        colors.bad = QColor(QStringLiteral("#f85149"));
        colors.badSoft = QColor(248, 81, 73, 31);
        colors.primary = QColor(QStringLiteral("#4493f8"));
        colors.series[0] = QColor(QStringLiteral("#4493f8"));
        colors.series[1] = QColor(QStringLiteral("#cf6f24"));
        colors.series[2] = QColor(QStringLiteral("#a877ea"));
        colors.series[3] = QColor(QStringLiteral("#22a697"));
    } else {
        colors.fg = QColor(QStringLiteral("#1f2328"));
        colors.muted = QColor(QStringLiteral("#59636e"));
        colors.line = QColor(QStringLiteral("#d0d7de"));
        colors.lineSoft = QColor(QStringLiteral("#eaeef2"));
        colors.card = QColor(QStringLiteral("#ffffff"));
        colors.raised = QColor(QStringLiteral("#f6f8fa"));
        colors.page = QColor(QStringLiteral("#f6f8fa"));
        colors.good = QColor(QStringLiteral("#1a7f37"));
        colors.goodSoft = QColor(QStringLiteral("#dafbe1"));
        colors.warn = QColor(QStringLiteral("#9a6700"));
        colors.warnSoft = QColor(QStringLiteral("#fff8c5"));
        colors.bad = QColor(QStringLiteral("#cf222e"));
        colors.badSoft = QColor(QStringLiteral("#ffebe9"));
        colors.primary = QColor(QStringLiteral("#0969da"));
        colors.series[0] = QColor(QStringLiteral("#1f6feb"));
        colors.series[1] = QColor(QStringLiteral("#bc4c00"));
        colors.series[2] = QColor(QStringLiteral("#8250df"));
        colors.series[3] = QColor(QStringLiteral("#00897a"));
    }
    return colors;
}

QColor brandColor(const QString &channel, const DashboardColors &colors)
{
    static const QHash<QString, QString> brands{
        {QStringLiteral("x"), QStringLiteral("#14171a")},
        {QStringLiteral("youtube"), QStringLiteral("#e62117")},
        {QStringLiteral("reddit"), QStringLiteral("#ff4500")},
        {QStringLiteral("linkedin"), QStringLiteral("#0a66c2")},
        {QStringLiteral("facebook"), QStringLiteral("#1877f2")},
        {QStringLiteral("instagram"), QStringLiteral("#d62976")},
        {QStringLiteral("mastodon"), QStringLiteral("#6364ff")},
        {QStringLiteral("discord"), QStringLiteral("#5865f2")},
        {QStringLiteral("github"), QStringLiteral("#24292f")},
        {QStringLiteral("bluesky"), QStringLiteral("#1185fe")},
        {QStringLiteral("tiktok"), QStringLiteral("#111111")},
        {QStringLiteral("threads"), QStringLiteral("#111111")},
        {QStringLiteral("hackernews"), QStringLiteral("#ff6600")},
        {QStringLiteral("producthunt"), QStringLiteral("#da552f")},
        {QStringLiteral("telegram"), QStringLiteral("#229ed9")},
        {QStringLiteral("whatsapp"), QStringLiteral("#25d366")},
        {QStringLiteral("pinterest"), QStringLiteral("#e60023")},
        {QStringLiteral("twitch"), QStringLiteral("#9146ff")},
        {QStringLiteral("medium"), QStringLiteral("#111111")},
        {QStringLiteral("substack"), QStringLiteral("#ff6719")},
        {QStringLiteral("devto"), QStringLiteral("#0a0a0a")},
        {QStringLiteral("stackoverflow"), QStringLiteral("#f48024")},
        {QStringLiteral("snapchat"), QStringLiteral("#e6c800")},
        {QStringLiteral("spotify"), QStringLiteral("#1db954")},
        {QStringLiteral("google-search"), QStringLiteral("#1a73e8")},
    };
    const QString brand = brands.value(channel);
    return brand.isEmpty() ? colors.series[0] : QColor(brand);
}

QColor brandInk(const QString &channel, const DashboardColors &colors)
{
    const QColor color = brandColor(channel, colors);
    return colors.dark && color.lightness() < 70 ? colors.fg : color;
}

QString initials(const QString &value)
{
    static const QRegularExpression separators(QStringLiteral("[\\s@._-]+"));
    const QStringList words = value.trimmed().split(separators, Qt::SkipEmptyParts);
    if (words.isEmpty())
        return QStringLiteral("?");
    const QString result = words.size() > 1
        ? QString(words.at(0).at(0)) + words.at(1).at(0)
        : QString(words.at(0).at(0));
    return result.toUpper();
}

QString dashboardStyleSheet(const DashboardColors &c)
{
    return QStringLiteral(
        "QFrame#marketingPanel { background-color: %1; border: 1px solid %2; border-radius: 10px; }"
        "QFrame#marketingPanel[marketingDropTarget=\"true\"] { border: 2px dashed %13; }"
        "QFrame#marketingPanel QLabel, QFrame#marketingPanel QCheckBox, QWidget#marketingClear,"
        " QFrame#marketingCard QLabel, QFrame#marketingCard QCheckBox, QFrame#marketingTile QLabel,"
        " QFrame#marketingIdea QLabel { background: transparent; }"
        "QFrame#marketingPanelHead { background: transparent; border: none; border-bottom: 1px solid %3; border-radius: 0; }"
        "QLabel#marketingPanelTitle { color: %4; font-size: 13px; font-weight: 600; }"
        "QLabel#marketingHint, QLabel#marketingSmall { color: %5; font-size: 12px; }"
        "QLabel#marketingNote { color: %4; font-size: 12px; }"
        "QPushButton#marketingLink { background: transparent; border: none; color: %5;"
        " padding: 3px 7px; border-radius: 6px; font-size: 12px; min-height: 0; }"
        "QPushButton#marketingLink:hover { background: %3; color: %4; }"
        "QPushButton#marketingLink:disabled { color: %2; }"
        "QFrame#marketingSeg { background: %6; border: 1px solid %2; border-radius: 8px; }"
        "QFrame#marketingSeg QPushButton { background: transparent; border: 1px solid transparent;"
        " border-radius: 6px; padding: 2px 10px; font-size: 12px; color: %5; min-height: 0; }"
        "QFrame#marketingSeg QPushButton:checked { background: %1; color: %4; border-color: %2; }"
        "QFrame#marketingChannelChip { background: %1; border: 1px solid %3; border-radius: 8px; }"
        "QFrame#marketingChannelChip:hover { background: %6; }"
        "QFrame#marketingChannelChip:focus { border-color: %13; }"
        "QFrame#marketingChannelChip[current=\"true\"] { background: %3; border-color: %13; }"
        "QFrame#marketingChannelChip[ghost=\"true\"] { background: transparent; border: 1px dashed %2; }"
        "QFrame#marketingChannelChip QLabel { background: transparent; }"
        "QLabel#marketingChipName { color: %4; font-size: 12px; font-weight: 600; }"
        "QLabel#marketingChipValue { color: %4; font-size: 13px; font-weight: 600; }"
        "QLabel#marketingChipName[paused=\"true\"], QLabel#marketingChipValue[paused=\"true\"] { color: %5; }"
        "QLabel#marketingChipFacts { color: %5; font-size: 11px; }"
        "QFrame#marketingTile { background: %1; border: 1px solid %3; border-radius: 8px; }"
        "QFrame#marketingTile:hover { background: %6; }"
        "QFrame#marketingTile:focus { border-color: %13; }"
        "QLabel#marketingTileValue { color: %4; font-size: 21px; font-weight: 500; }"
        "QLabel#marketingTileValue[paused=\"true\"], QLabel#marketingTileLabel[paused=\"true\"] { color: %5; }"
        "QLabel#marketingTileLabel { color: %5; font-size: 12px; }"
        "QLabel#marketingFacts { color: %5; font-size: 11px; border-top: 1px dashed %2; padding-top: 5px; }"
        "QFrame#marketingCard { background: %1; border: 1px solid %2; border-radius: 10px; }"
        "QFrame#marketingCard[ghost=\"true\"] { background: transparent; border-style: dashed; }"
        "QLabel#marketingCardTitle { color: %4; font-size: 14px; font-weight: 600; }"
        "QFrame#marketingCardFoot { background: transparent; border: none; border-top: 1px solid %3; border-radius: 0; }"
        "QLabel#marketingPill { border: 1px solid %2; border-radius: 9px; padding: 1px 8px; font-size: 11px; font-weight: 500; color: %5; }"
        "QLabel#marketingPill[tone=\"good\"] { color: %7; background: %8; border-color: transparent; }"
        "QLabel#marketingPill[tone=\"warn\"] { color: %9; background: %10; border-color: transparent; }"
        "QLabel#marketingPill[tone=\"bad\"] { color: %11; background: %12; border-color: transparent; }"
        "QLabel#marketingPill[tone=\"primary\"] { color: %13; background: %3; border-color: transparent; }"
        "QLabel#marketingPill[tone=\"idea\"] { border-style: dashed; }"
        "QFrame#marketingFact { background: %6; border: none; border-radius: 8px; }"
        "QFrame#marketingFact QLabel { background: transparent; }"
        "QLabel#marketingFactLabel { color: %5; font-size: 10px; }"
        "QLabel#marketingFactValue { color: %4; font-size: 14px; font-weight: 500; }"
        "QFrame#marketingDay { background: transparent; border: none; border-bottom: 1px solid %3; border-radius: 0; }"
        "QFrame#marketingDay[last=\"true\"] { border-bottom: none; }"
        "QLabel#marketingDayName { color: %5; font-size: 10px; font-weight: 500; }"
        "QLabel#marketingDayNumber { color: %4; font-size: 15px; font-weight: 500; }"
        "QLabel#marketingDayNumber[today=\"true\"] { color: %7; }"
        "QPushButton#marketingSlot { text-align: left; background: transparent; border: none;"
        " border-radius: 5px; padding: 2px 6px; font-size: 12px; color: %4; min-height: 0; }"
        "QPushButton#marketingSlot:hover { background: %3; }"
        "QFrame#marketingIdea { background: transparent; border: 1px solid %2; border-radius: 8px; }"
        "QLabel#marketingQuote { background: %6; border-left: 2px solid %14; padding: 6px 8px; font-size: 12px; color: %4; }"
        "QFrame#marketingBanner { background: transparent; border: 1px dashed %2; border-radius: 10px; }"
        "QFrame#marketingBanner QLabel { background: transparent; }"
        "QLabel#marketingWarn { color: %9; font-size: 12px; }"
        "QLabel#marketingBad { color: %11; font-size: 12px; }"
        "QFrame#marketingPanel QTableWidget { background: %1; alternate-background-color: %6; border: none; color: %4; }"
        "QFrame#marketingPanel QTableWidget::item { padding: 0 6px; }"
        "QFrame#marketingPanel QHeaderView::section { background: %1; color: %5; border: none;"
        " border-bottom: 1px solid %3; padding: 5px 6px; font-size: 11px; font-weight: 500; }"
        "QFrame#marketingPanel QTableCornerButton::section { background: %1; border: none; }"
        "QFrame#marketingTile QPushButton#marketingLink, QFrame#marketingCardFoot QPushButton#marketingLink,"
        " QFrame#marketingPanel QTableWidget QPushButton#marketingLink { padding: 2px 4px; font-size: 11px; }"
        "QPushButton#marketingSlot { font-weight: 400; }")
        .arg(rgba(c.card), rgba(c.line), rgba(c.lineSoft), rgba(c.fg), rgba(c.muted),
             rgba(c.raised), rgba(c.good), rgba(c.goodSoft), rgba(c.warn))
        .arg(rgba(c.warnSoft), rgba(c.bad), rgba(c.badSoft), rgba(c.primary),
             rgba(c.series[2]));
}

QWidget *clearWidget(QWidget *parent)
{
    auto *widget = new QWidget(parent);
    widget->setObjectName(QStringLiteral("marketingClear"));
    return widget;
}

QPushButton *linkButton(const QString &text, const QString &tip)
{
    auto *button = new QPushButton(text);
    button->setObjectName(QStringLiteral("marketingLink"));
    button->setCursor(Qt::PointingHandCursor);
    if (!tip.isEmpty())
        button->setToolTip(tip);
    return button;
}

QWidget *segmented(const QStringList &labels, QButtonGroup *group, int checked,
                   const QString &accessibleName)
{
    auto *frame = new QFrame;
    frame->setObjectName(QStringLiteral("marketingSeg"));
    frame->setAccessibleName(accessibleName);
    auto *layout = new QHBoxLayout(frame);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->setSpacing(2);
    for (int index = 0; index < labels.size(); ++index) {
        auto *button = new QPushButton(labels.at(index));
        button->setCheckable(true);
        button->setChecked(index == checked);
        button->setCursor(Qt::PointingHandCursor);
        button->setAccessibleName(accessibleName + QStringLiteral(": ") + labels.at(index));
        group->addButton(button, index);
        layout->addWidget(button);
    }
    group->setExclusive(true);
    return frame;
}

DashboardPanel::DashboardPanel(const QString &title, QWidget *parent) : QFrame(parent)
{
    setObjectName(QStringLiteral("marketingPanel"));
    setAccessibleName(title);
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(1, 1, 1, 1);
    outer->setSpacing(0);
    auto *head = new QFrame;
    head->setObjectName(QStringLiteral("marketingPanelHead"));
    auto *headLayout = new QHBoxLayout(head);
    headLayout->setContentsMargins(14, 8, 10, 8);
    headLayout->setSpacing(8);
    m_head = headLayout;
    m_title = new QLabel(title);
    m_title->setObjectName(QStringLiteral("marketingPanelTitle"));
    m_title->setTextFormat(Qt::PlainText);
    headLayout->addWidget(m_title);
    m_hint = new QLabel;
    m_hint->setObjectName(QStringLiteral("marketingHint"));
    m_hint->setTextFormat(Qt::PlainText);
    m_hint->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    headLayout->addWidget(m_hint, 1);
    m_tools = new QHBoxLayout;
    m_tools->setSpacing(4);
    headLayout->addLayout(m_tools);
    outer->addWidget(head);
    m_body = new QVBoxLayout;
    m_body->setContentsMargins(14, 10, 14, 12);
    m_body->setSpacing(8);
    outer->addLayout(m_body);
    outer->addStretch(1);
}

void DashboardPanel::setCompact()
{
    m_head->setContentsMargins(10, 3, 6, 3);
    m_body->setContentsMargins(10, 5, 10, 6);
    m_body->setSpacing(4);
}

DashboardGrid::DashboardGrid(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("marketingClear"));
    auto *grid = new QGridLayout(this);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(14);
    grid->setVerticalSpacing(14);
}

void DashboardGrid::addPanel(QWidget *panel, int span)
{
    panel->setParent(this);
    m_entries.append({panel, qBound(1, span, 3)});
    m_mode = -1;
    place(width() > 0 ? columnsFor(width()) : 3);
}

void DashboardGrid::setMovable(const QString &settingsKey)
{
    m_orderKey = settingsKey;
    QStringList defaults;
    for (Entry &entry : m_entries) {
        QString key = entry.panel->accessibleName();
        if (key.isEmpty() || defaults.contains(key))
            key = QStringLiteral("panel%1").arg(defaults.size());
        entry.key = key;
        defaults.append(key);
        QWidget *handle = entry.panel->findChild<QWidget *>(QStringLiteral("marketingPanelHead"),
                                                            Qt::FindDirectChildrenOnly);
        if (!handle)
            handle = entry.panel;
        handle->setCursor(Qt::OpenHandCursor);
        handle->setToolTip(QStringLiteral("Drag to move this panel"));
        for (QAbstractButton *button : handle->findChildren<QAbstractButton *>())
            if (!button->testAttribute(Qt::WA_SetCursor))
                button->setCursor(Qt::PointingHandCursor);
        handle->installEventFilter(this);
    }
    QStringList order;
    for (const QString &key : QSettings().value(settingsKey).toStringList())
        if (defaults.contains(key) && !order.contains(key))
            order.append(key);
    for (int index = 0; index < defaults.size(); ++index)
        if (!order.contains(defaults.at(index)))
            order.insert(index ? order.indexOf(defaults.at(index - 1)) + 1 : 0, defaults.at(index));
    QList<Entry> sorted;
    for (const QString &key : order)
        for (const Entry &entry : m_entries)
            if (entry.key == key)
                sorted.append(entry);
    m_entries = sorted;
    m_mode = -1;
    place(width() > 0 ? columnsFor(width()) : 3);
}

int DashboardGrid::entryAt(const QPoint &globalPos) const
{
    for (int index = 0; index < m_entries.size(); ++index) {
        const QWidget *panel = m_entries.at(index).panel;
        if (panel->isVisible() && QRect(panel->mapToGlobal(QPoint(0, 0)), panel->size()).contains(globalPos))
            return index;
    }
    return -1;
}

void DashboardGrid::markDropTarget(int index)
{
    if (index == m_dropIndex)
        return;
    const auto mark = [this](int at, bool on) {
        if (at < 0 || at >= m_entries.size())
            return;
        QWidget *panel = m_entries.at(at).panel;
        panel->setProperty("marketingDropTarget", on);
        panel->style()->unpolish(panel);
        panel->style()->polish(panel);
    };
    mark(m_dropIndex, false);
    m_dropIndex = index;
    mark(m_dropIndex, true);
}

void DashboardGrid::finishDrag(bool drop)
{
    if (m_dragging)
        QApplication::restoreOverrideCursor();
    const int from = m_dragIndex;
    const int to = m_dropIndex;
    markDropTarget(-1);
    m_dragIndex = -1;
    m_dragging = false;
    if (!drop || from < 0 || to < 0 || from == to)
        return;
    m_entries.move(from, to);
    m_mode = -1;
    place(columnsFor(width()));
    QStringList keys;
    for (const Entry &entry : m_entries)
        keys.append(entry.key);
    QSettings().setValue(m_orderKey, keys);
}

bool DashboardGrid::eventFilter(QObject *watched, QEvent *event)
{
    if (m_orderKey.isEmpty())
        return QWidget::eventFilter(watched, event);
    switch (event->type()) {
    case QEvent::MouseButtonPress: {
        auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() != Qt::LeftButton)
            break;
        m_dragStart = mouse->globalPosition().toPoint();
        m_dragIndex = entryAt(m_dragStart);
        m_dragging = false;
        return m_dragIndex >= 0;
    }
    case QEvent::MouseMove: {
        auto *mouse = static_cast<QMouseEvent *>(event);
        if (m_dragIndex < 0 || !(mouse->buttons() & Qt::LeftButton))
            break;
        const QPoint pos = mouse->globalPosition().toPoint();
        if (!m_dragging) {
            if ((pos - m_dragStart).manhattanLength() < QApplication::startDragDistance())
                return true;
            m_dragging = true;
            QApplication::setOverrideCursor(Qt::ClosedHandCursor);
        }
        for (QWidget *parent = parentWidget(); parent; parent = parent->parentWidget()) {
            auto *area = qobject_cast<QScrollArea *>(parent);
            if (!area)
                continue;
            const QPoint local = area->viewport()->mapFromGlobal(pos);
            QScrollBar *bar = area->verticalScrollBar();
            if (local.y() < 40)
                bar->setValue(bar->value() - 24);
            else if (local.y() > area->viewport()->height() - 40)
                bar->setValue(bar->value() + 24);
            break;
        }
        const int target = entryAt(pos);
        markDropTarget(target == m_dragIndex ? -1 : target);
        return true;
    }
    case QEvent::MouseButtonRelease:
        if (m_dragIndex < 0)
            break;
        finishDrag(m_dragging);
        return true;
    default:
        break;
    }
    return QWidget::eventFilter(watched, event);
}

int DashboardGrid::columnsFor(int width) const
{
    return width >= m_narrowWidth ? 3 : width >= m_narrowWidth * 5 / 8 ? 2 : 1;
}

void DashboardGrid::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    const int columns = columnsFor(event->size().width());
    if (columns != m_mode)
        place(columns);
}

void DashboardGrid::place(int columns)
{
    auto *grid = static_cast<QGridLayout *>(layout());
    while (grid->count() > 0)
        delete grid->takeAt(0);
    m_mode = columns;
    int row = 0;
    int column = 0;
    for (int index = 0; index < m_entries.size(); ++index) {
        const Entry &entry = m_entries.at(index);
        int span = qMin(entry.span, columns);
        if (columns == 2 && entry.span == 1 && column == 0 &&
            (index + 1 >= m_entries.size() || m_entries.at(index + 1).span != 1))
            span = 2;
        if (columns == 2 && entry.span > 1)
            span = 2;
        if (column + span > columns) {
            ++row;
            column = 0;
        }
        grid->addWidget(entry.panel, row, column, 1, span);
        column += span;
        if (column >= columns) {
            ++row;
            column = 0;
        }
    }
    for (int index = 0; index < 3; ++index)
        grid->setColumnStretch(index, index < columns ? 1 : 0);
}

KpiStrip::KpiStrip(QWidget *parent) : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setMouseTracking(true);
    setAccessibleName(QStringLiteral("Key metrics"));
}

void KpiStrip::setKpis(const QList<Kpi> &kpis)
{
    m_kpis = kpis;
    QStringList description;
    for (const Kpi &kpi : kpis)
        description << QStringLiteral("%1 %2 %3").arg(kpi.label, kpi.value, kpi.delta);
    setAccessibleDescription(description.join(QStringLiteral("; ")));
    updateGeometry();
    update();
}

void KpiStrip::setCompact(bool compact)
{
    m_compact = compact;
    updateGeometry();
    update();
}

int KpiStrip::columns(int width) const
{
    const int count = qMax(1, int(m_kpis.size()));
    if (width >= (m_compact ? 112 : 140) * count)
        return count;
    if (width >= 480)
        return qMin(count, 3);
    return qMin(count, 2);
}

int KpiStrip::heightForWidth(int width) const
{
    const int cols = columns(width);
    const int rows = (qMax(1, int(m_kpis.size())) + cols - 1) / cols;
    return rows * (m_compact ? 58 : 96);
}

QSize KpiStrip::sizeHint() const
{
    return {900, heightForWidth(900)};
}

QSize KpiStrip::minimumSizeHint() const
{
    return {280, m_compact ? 58 : 96};
}

QRect KpiStrip::cellRect(int index) const
{
    const int cols = columns(width());
    const int rows = (qMax(1, int(m_kpis.size())) + cols - 1) / cols;
    const int cellWidth = width() / cols;
    const int cellHeight = rows > 0 ? height() / rows : height();
    const int column = index % cols;
    const int row = index / cols;
    const int right = column == cols - 1 ? width() : (column + 1) * cellWidth;
    return QRect(column * cellWidth, row * cellHeight, right - column * cellWidth, cellHeight);
}

void KpiStrip::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const DashboardColors c = DashboardColors::from(this);
    const QRectF frame = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QPainterPath clip;
    clip.addRoundedRect(frame, 10, 10);
    painter.fillPath(clip, c.card);
    painter.save();
    painter.setClipPath(clip);
    for (int index = 0; index < m_kpis.size(); ++index) {
        const Kpi &kpi = m_kpis.at(index);
        const QRect cell = cellRect(index);
        if (index == 0) {
            QLinearGradient glow(cell.topLeft(), cell.bottomLeft());
            glow.setColorAt(0, c.goodSoft);
            glow.setColorAt(0.85, alpha(c.goodSoft, 0));
            painter.fillRect(cell, glow);
        }
        painter.setPen(QPen(c.lineSoft, 1));
        if (cell.left() > 0)
            painter.drawLine(QPointF(cell.left() + 0.5, cell.top()), QPointF(cell.left() + 0.5, cell.bottom() + 1));
        if (cell.top() > 0)
            painter.drawLine(QPointF(cell.left(), cell.top() + 0.5), QPointF(cell.right() + 1, cell.top() + 0.5));

        const QRect inner = m_compact ? cell.adjusted(10, 5, -8, -4) : cell.adjusted(16, 12, -14, -10);
        painter.setFont(sizedFont(font(), m_compact ? 11 : 12));
        painter.setPen(c.muted);
        painter.drawText(QRect(inner.left(), inner.top(), inner.width(), m_compact ? 14 : 16), Qt::AlignLeft | Qt::AlignVCenter,
                         painter.fontMetrics().elidedText(kpi.label, Qt::ElideRight, inner.width()));
        painter.setFont(sizedFont(font(), m_compact ? 17 : 24, QFont::Medium));
        painter.setPen(c.fg);
        painter.drawText(QRect(inner.left(), inner.top() + (m_compact ? 13 : 18), inner.width(), m_compact ? 22 : 32),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         painter.fontMetrics().elidedText(kpi.value, Qt::ElideRight, inner.width()));

        const int baseline = inner.top() + (m_compact ? 35 : 54);
        const int line = m_compact ? 15 : 18;
        int x = inner.left();
        painter.setFont(sizedFont(font(), m_compact ? 10 : 11, QFont::Medium));
        if (!kpi.delta.isEmpty()) {
            const int deltaWidth = painter.fontMetrics().horizontalAdvance(kpi.delta) + 12;
            const QRectF pill(x, baseline, deltaWidth, line);
            const QColor fill = kpi.tone > 0 ? c.goodSoft : kpi.tone < 0 ? c.badSoft : c.lineSoft;
            const QColor ink = kpi.tone > 0 ? c.good : kpi.tone < 0 ? c.bad : c.muted;
            painter.setPen(Qt::NoPen);
            painter.setBrush(fill);
            painter.drawRoundedRect(pill, line / 2.0, line / 2.0);
            painter.setPen(ink);
            painter.drawText(pill, Qt::AlignCenter, kpi.delta);
            x += deltaWidth + 8;
        }
        const bool spark = kpi.spark.size() >= 2 && inner.width() > 180;
        const int sparkWidth = spark ? 84 : 0;
        if (!kpi.note.isEmpty()) {
            painter.setFont(sizedFont(font(), m_compact ? 10 : 11));
            painter.setPen(c.muted);
            const int room = inner.right() - x - (spark ? sparkWidth + 8 : 0);
            painter.drawText(QRect(x, baseline, qMax(0, room), line), Qt::AlignLeft | Qt::AlignVCenter,
                             painter.fontMetrics().elidedText(kpi.note, Qt::ElideRight, qMax(0, room)));
        }
        if (spark) {
            const QRectF box(inner.right() - sparkWidth, baseline - 8, sparkWidth, 26);
            const auto [lo, hi] = std::minmax_element(kpi.spark.cbegin(), kpi.spark.cend());
            const double span = (*hi - *lo) > 0 ? *hi - *lo : 1;
            QPainterPath line;
            for (int point = 0; point < kpi.spark.size(); ++point) {
                const QPointF at(box.left() + box.width() * point / (kpi.spark.size() - 1),
                                 box.bottom() - 3 - (kpi.spark.at(point) - *lo) / span * (box.height() - 6));
                point ? line.lineTo(at) : line.moveTo(at);
            }
            QPainterPath area = line;
            area.lineTo(box.bottomRight());
            area.lineTo(box.bottomLeft());
            area.closeSubpath();
            QLinearGradient fill(box.topLeft(), box.bottomLeft());
            fill.setColorAt(0, alpha(c.good, 0.22));
            fill.setColorAt(1, alpha(c.good, 0));
            painter.fillPath(area, fill);
            painter.setPen(QPen(c.good, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(line);
            painter.setPen(QPen(c.card, 1.5));
            painter.setBrush(c.good);
            painter.drawEllipse(line.currentPosition(), 2.5, 2.5);
        }
    }
    painter.restore();
    painter.setPen(QPen(c.line, 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(frame, 10, 10);
}

bool KpiStrip::event(QEvent *event)
{
    if (event->type() != QEvent::ToolTip)
        return QWidget::event(event);
    const auto *help = static_cast<QHelpEvent *>(event);
    for (int index = 0; index < m_kpis.size(); ++index) {
        if (!cellRect(index).contains(help->pos()))
            continue;
        const Kpi &kpi = m_kpis.at(index);
        QStringList lines{kpi.label + QStringLiteral(": ") +
                          (kpi.tooltip.isEmpty() ? kpi.value : kpi.tooltip)};
        if (!kpi.delta.isEmpty())
            lines << kpi.delta;
        if (!kpi.note.isEmpty())
            lines << kpi.note;
        QToolTip::showText(help->globalPos(), lines.join(QLatin1Char('\n')), this);
        return true;
    }
    QToolTip::hideText();
    return true;
}

BarRows::BarRows(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("marketingClear"));
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void BarRows::setRows(const QList<Row> &rows, int labelWidth, int textWidth)
{
    m_rows = rows;
    m_labelWidth = labelWidth;
    m_textWidth = textWidth;
    m_hover = -1;
    QStringList description;
    for (const Row &row : rows)
        description << QStringLiteral("%1 %2").arg(row.label, row.text);
    setAccessibleDescription(description.join(QStringLiteral("; ")));
    setCursor(onClicked ? Qt::PointingHandCursor : Qt::ArrowCursor);
    updateGeometry();
    update();
}

void BarRows::setSelected(int index)
{
    m_selected = index;
    update();
}

void BarRows::setEmptyText(const QString &text)
{
    m_empty = text;
    update();
}

QSize BarRows::sizeHint() const
{
    return {240, qMax(26, int(m_rows.size()) * 26)};
}

QSize BarRows::minimumSizeHint() const
{
    return {160, sizeHint().height()};
}

int BarRows::rowAt(const QPoint &point) const
{
    const int index = point.y() / 26;
    return index >= 0 && index < m_rows.size() ? index : -1;
}

void BarRows::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const DashboardColors c = DashboardColors::from(this);
    painter.setFont(sizedFont(font(), 12));
    if (m_rows.isEmpty()) {
        painter.setPen(c.muted);
        painter.drawText(rect(), Qt::AlignLeft | Qt::AlignVCenter, m_empty);
        return;
    }
    for (int index = 0; index < m_rows.size(); ++index) {
        const Row &row = m_rows.at(index);
        const QRectF band(0, index * 26 + 1, width(), 24);
        if (onClicked && (index == m_hover || index == m_selected)) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(c.lineSoft);
            painter.drawRoundedRect(band, 6, 6);
        }
        qreal left = 6;
        const int labelWidth = m_labelWidth < 0 ? width() * -m_labelWidth / 100 : m_labelWidth;
        if (labelWidth > 0) {
            painter.setPen(index == m_selected ? c.fg : c.muted);
            painter.drawText(QRectF(left, band.top(), labelWidth - 8, band.height()),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             painter.fontMetrics().elidedText(row.label, Qt::ElideMiddle, labelWidth - 8));
            left += labelWidth;
        }
        const qreal right = width() - 6 - (m_textWidth > 0 ? m_textWidth + 8 : 0);
        const QRectF track(left, band.center().y() - 4, qMax<qreal>(10, right - left), 8);
        painter.setPen(Qt::NoPen);
        painter.setBrush(c.lineSoft);
        painter.drawRoundedRect(track, 3, 3);
        const double fraction = row.max > 0 ? qBound(0.0, row.value / row.max, 1.0) : 0;
        if (fraction > 0) {
            painter.setBrush(row.color.isValid() ? row.color : c.muted);
            painter.drawRoundedRect(QRectF(track.left(), track.top(),
                                           qMax<qreal>(3, track.width() * fraction), track.height()), 3, 3);
        }
        if (row.marker >= 0) {
            const qreal x = track.left() + track.width() * qBound(0.0, row.marker, 1.0) - 1;
            painter.setBrush(alpha(c.fg, 0.75));
            painter.drawRoundedRect(QRectF(qMin(x, track.right() - 2), track.top() - 3, 2, 14), 1, 1);
        }
        if (m_textWidth > 0) {
            painter.setPen(c.fg);
            painter.drawText(QRectF(width() - 6 - m_textWidth, band.top(), m_textWidth, band.height()),
                             Qt::AlignRight | Qt::AlignVCenter,
                             painter.fontMetrics().elidedText(row.text, Qt::ElideLeft, m_textWidth));
        }
    }
}

void BarRows::mouseMoveEvent(QMouseEvent *event)
{
    const int index = rowAt(event->position().toPoint());
    if (index != m_hover) {
        m_hover = index;
        update();
    }
}

void BarRows::mousePressEvent(QMouseEvent *event)
{
    const int index = rowAt(event->position().toPoint());
    if (index >= 0 && onClicked && event->button() == Qt::LeftButton)
        onClicked(index);
}

void BarRows::leaveEvent(QEvent *event)
{
    m_hover = -1;
    update();
    QWidget::leaveEvent(event);
}

bool BarRows::event(QEvent *event)
{
    if (event->type() != QEvent::ToolTip)
        return QWidget::event(event);
    const auto *help = static_cast<QHelpEvent *>(event);
    const int index = rowAt(help->pos());
    if (index < 0) {
        QToolTip::hideText();
        return true;
    }
    const Row &row = m_rows.at(index);
    QToolTip::showText(help->globalPos(),
                       row.tooltip.isEmpty() ? QStringLiteral("%1: %2").arg(row.label, row.text)
                                             : row.tooltip, this);
    return true;
}

ProgressLine::ProgressLine(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("marketingClear"));
    setFixedHeight(10);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void ProgressLine::setProgress(double fraction, const QColor &color, double marker)
{
    m_fraction = qBound(0.0, fraction, 1.0);
    m_color = color;
    m_marker = marker;
    update();
}

void ProgressLine::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const DashboardColors c = DashboardColors::from(this);
    const QRectF track(0, height() / 2.0 - 2, width(), 4);
    painter.setPen(Qt::NoPen);
    painter.setBrush(c.lineSoft);
    painter.drawRoundedRect(track, 2, 2);
    if (m_fraction > 0) {
        painter.setBrush(m_color.isValid() ? m_color : c.muted);
        painter.drawRoundedRect(QRectF(0, track.top(), qMax<qreal>(2, track.width() * m_fraction), 4), 2, 2);
    }
    if (m_marker >= 0) {
        painter.setBrush(c.fg);
        const qreal x = qBound<qreal>(0, track.width() * m_marker - 1, track.width() - 2);
        painter.drawRoundedRect(QRectF(x, 0, 2, height()), 1, 1);
    }
}

Sparkline::Sparkline(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("marketingClear"));
    setFixedHeight(22);
    setMinimumWidth(48);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setAttribute(Qt::WA_TransparentForMouseEvents);
}

void Sparkline::setValues(const QVector<double> &values, const QColor &color)
{
    m_values = values;
    m_color = color;
    update();
}

void Sparkline::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const DashboardColors c = DashboardColors::from(this);
    const QColor ink = m_color.isValid() ? m_color : c.primary;
    const QRectF box = QRectF(rect()).adjusted(2, 3, -3, -3);
    if (m_values.size() < 2) {
        painter.setPen(QPen(c.lineSoft, 1, Qt::DashLine));
        painter.drawLine(QPointF(box.left(), box.bottom()), box.bottomRight());
        return;
    }
    const auto [lo, hi] = std::minmax_element(m_values.cbegin(), m_values.cend());
    const double span = *hi - *lo;
    QPainterPath line;
    for (int point = 0; point < m_values.size(); ++point) {
        const double level = span > 0 ? (m_values.at(point) - *lo) / span : 0.5;
        const QPointF at(box.left() + box.width() * point / (m_values.size() - 1),
                         box.bottom() - level * box.height());
        point ? line.lineTo(at) : line.moveTo(at);
    }
    QPainterPath area = line;
    area.lineTo(box.right(), rect().bottom());
    area.lineTo(box.left(), rect().bottom());
    area.closeSubpath();
    QLinearGradient fill(box.topLeft(), QPointF(box.left(), rect().bottom()));
    fill.setColorAt(0, alpha(ink, 0.24));
    fill.setColorAt(1, alpha(ink, 0));
    painter.fillPath(area, fill);
    painter.setPen(QPen(ink, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(line);
    painter.setPen(Qt::NoPen);
    painter.setBrush(ink);
    painter.drawEllipse(line.currentPosition(), 2, 2);
}

LineChart::LineChart(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("marketingClear"));
    setMouseTracking(true);
    setMinimumHeight(220);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
}

void LineChart::setSeries(const QList<Series> &series, qint64 start, qint64 end,
                          const QString &empty)
{
    m_series = series;
    m_start = start;
    m_end = qMax(start + 1, end);
    m_empty = empty;
    double peak = 0;
    for (const Series &line : series) {
        for (const Point &point : line.points)
            peak = std::max(peak, point.value);
    }
    m_step = niceStep(peak);
    m_top = std::max(m_step, std::ceil(peak / m_step) * m_step);
    QStringList description;
    for (const Series &line : series) {
        if (!line.points.isEmpty())
            description << QStringLiteral("%1 %2").arg(line.name)
                               .arg(QLocale().toString(qint64(line.points.constLast().value)));
    }
    setAccessibleDescription(description.join(QStringLiteral("; ")));
    m_hoverX = -1;
    update();
}

QRectF LineChart::plotRect() const
{
    return QRectF(48, 10, qMax(40, width() - 48 - 122), qMax(40, height() - 10 - 26));
}

void LineChart::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const DashboardColors c = DashboardColors::from(this);
    painter.setFont(sizedFont(font(), 11));
    if (m_series.isEmpty()) {
        painter.setPen(c.muted);
        painter.drawText(rect(), Qt::AlignCenter | Qt::TextWordWrap, m_empty);
        return;
    }
    const QRectF plot = plotRect();
    const auto x = [&](qint64 at) {
        return plot.left() + double(at - m_start) / double(m_end - m_start) * plot.width();
    };
    const auto y = [&](double value) { return plot.top() + (1 - value / m_top) * plot.height(); };
    for (double value = 0; value <= m_top + m_step / 2; value += m_step) {
        painter.setPen(QPen(value > 0 ? c.lineSoft : c.line, 1));
        painter.drawLine(QPointF(plot.left(), y(value)), QPointF(plot.right(), y(value)));
        painter.setPen(c.muted);
        painter.drawText(QRectF(0, y(value) - 8, plot.left() - 8, 16), Qt::AlignRight | Qt::AlignVCenter,
                         compactNumber(value));
    }
    for (int index = 0; index < 5; ++index) {
        const qint64 at = m_start + (m_end - m_start) * index / 4;
        const QString label = QDateTime::fromMSecsSinceEpoch(at).toString(QStringLiteral("MMM d"));
        const Qt::Alignment align = index == 0 ? Qt::AlignLeft : index == 4 ? Qt::AlignRight : Qt::AlignHCenter;
        const qreal center = x(at);
        const QRectF box = index == 0 ? QRectF(center, plot.bottom() + 6, 80, 16)
                         : index == 4 ? QRectF(center - 80, plot.bottom() + 6, 80, 16)
                                      : QRectF(center - 40, plot.bottom() + 6, 80, 16);
        painter.drawText(box, align | Qt::AlignVCenter, label);
    }
    struct Label {
        const Series *line;
        qreal y;
    };
    QList<Label> labels;
    for (const Series &line : std::as_const(m_series)) {
        if (line.points.isEmpty())
            continue;
        QPainterPath path;
        for (int index = 0; index < line.points.size(); ++index) {
            const QPointF at(x(line.points.at(index).at), y(line.points.at(index).value));
            index ? path.lineTo(at) : path.moveTo(at);
        }
        painter.setPen(QPen(line.color, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(path);
        for (const Point &point : line.points) {
            const QPointF at(x(point.at), y(point.value));
            if (point.logged) {
                painter.setPen(QPen(c.card, 2));
                painter.setBrush(line.color);
                painter.drawEllipse(at, 3, 3);
            } else {
                painter.setPen(QPen(line.color, 1.5));
                painter.setBrush(c.card);
                painter.drawEllipse(at, 2.5, 2.5);
            }
        }
        labels.append({&line, y(line.points.constLast().value)});
    }
    std::sort(labels.begin(), labels.end(), [](const Label &a, const Label &b) { return a.y < b.y; });
    for (int index = 1; index < labels.size(); ++index)
        labels[index].y = std::max(labels[index].y, labels[index - 1].y + 14);
    for (const Label &label : std::as_const(labels)) {
        QString name = label.line->name;
        if (name.size() > 10)
            name = name.left(9) + QChar(0x2026);
        const QString value = compactNumber(label.line->points.constLast().value);
        const QRectF box(plot.right() + 8, std::min(label.y, plot.bottom()) - 8, 114, 16);
        painter.setPen(label.line->color);
        painter.drawText(box, Qt::AlignLeft | Qt::AlignVCenter, name);
        painter.setPen(c.fg);
        painter.drawText(box.adjusted(painter.fontMetrics().horizontalAdvance(name) + 6, 0, 0, 0),
                         Qt::AlignLeft | Qt::AlignVCenter, value);
    }
    if (m_hoverX >= plot.left() && m_hoverX <= plot.right()) {
        QPen dash(c.muted, 1, Qt::DashLine);
        painter.setPen(dash);
        painter.drawLine(QPointF(m_hoverX + 0.5, plot.top()), QPointF(m_hoverX + 0.5, plot.bottom()));
    }
}

void LineChart::mouseMoveEvent(QMouseEvent *event)
{
    const QRectF plot = plotRect();
    const qreal px = event->position().x();
    if (m_series.isEmpty() || px < plot.left() || px > plot.right()) {
        if (m_hoverX >= 0) {
            m_hoverX = -1;
            update();
            QToolTip::hideText();
        }
        return;
    }
    m_hoverX = int(px);
    update();
    const qint64 at = m_start + qint64((px - plot.left()) / plot.width() * double(m_end - m_start));
    QDateTime day = QDateTime::fromMSecsSinceEpoch(at);
    day.setTime(QTime(0, 0));
    const qint64 dayStart = day.toMSecsSinceEpoch();
    const qint64 dayEnd = day.addDays(1).toMSecsSinceEpoch() - 1;
    QStringList lines{day.toString(QStringLiteral("MMM d, yyyy"))};
    for (const Series &line : std::as_const(m_series)) {
        const Point *found = nullptr;
        for (const Point &point : line.history) {
            if (point.at <= dayEnd)
                found = &point;
            else
                break;
        }
        const bool logged = found && found->at >= dayStart;
        lines << QStringLiteral("%1: %2%3").arg(line.name,
            found ? QLocale().toString(qint64(found->value)) : QString::fromUtf8("\xE2\x80\x94"),
            found && !logged ? QStringLiteral(" (carried)") : QString());
    }
    QToolTip::showText(event->globalPosition().toPoint() + QPoint(14, 8),
                       lines.join(QLatin1Char('\n')), this);
}

void LineChart::leaveEvent(QEvent *event)
{
    m_hoverX = -1;
    update();
    QWidget::leaveEvent(event);
}

ClickFrame::ClickFrame(QWidget *parent)
    : QFrame(parent)
{
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
}

void ClickFrame::mousePressEvent(QMouseEvent *event)
{
    m_pressed = event->button() == Qt::LeftButton;
    event->setAccepted(m_pressed);
}

void ClickFrame::mouseReleaseEvent(QMouseEvent *event)
{
    const bool clicked = m_pressed && event->button() == Qt::LeftButton &&
                         rect().contains(event->position().toPoint());
    m_pressed = false;
    if (clicked && onClicked)
        onClicked();
}

void ClickFrame::keyPressEvent(QKeyEvent *event)
{
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter ||
         event->key() == Qt::Key_Space) && onClicked) {
        onClicked();
        return;
    }
    QFrame::keyPressEvent(event);
}

TileGrid::TileGrid(int minimumTileWidth, int spacing, QWidget *parent)
    : QWidget(parent), m_minimum(minimumTileWidth), m_spacing(spacing)
{
    setObjectName(QStringLiteral("marketingClear"));
    QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
}

void TileGrid::setTiles(const QList<QWidget *> &tiles)
{
    for (QWidget *tile : std::as_const(m_tiles)) {
        if (!tiles.contains(tile))
            tile->deleteLater();
    }
    m_tiles = tiles;
    for (QWidget *tile : std::as_const(m_tiles)) {
        tile->setParent(this);
        tile->show();
    }
    relayout();
    updateGeometry();
}

int TileGrid::columnsFor(int width) const
{
    return qMax(1, (width + m_spacing) / (m_minimum + m_spacing));
}

int TileGrid::tileHeight(QWidget *tile, int width) const
{
    const int preferred = tile->hasHeightForWidth() ? tile->heightForWidth(width)
                                                    : tile->sizeHint().height();
    return qMax(preferred, tile->minimumSizeHint().height());
}

int TileGrid::heightForWidth(int width) const
{
    if (m_tiles.isEmpty())
        return 0;
    const int columns = columnsFor(width);
    const int tileWidth = (width - (columns - 1) * m_spacing) / columns;
    int total = 0;
    for (int start = 0; start < m_tiles.size(); start += columns) {
        int rowHeight = 0;
        for (int index = start; index < qMin(int(m_tiles.size()), start + columns); ++index)
            rowHeight = qMax(rowHeight, tileHeight(m_tiles.at(index), tileWidth));
        total += rowHeight + (start ? m_spacing : 0);
    }
    return total;
}

QSize TileGrid::sizeHint() const
{
    const int width = qMax(this->width(), m_minimum * 2 + m_spacing);
    return {width, heightForWidth(width)};
}

QSize TileGrid::minimumSizeHint() const
{
    return {m_minimum, 0};
}

void TileGrid::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    relayout();
    if (heightForWidth(width()) != height())
        updateGeometry();
}

void TileGrid::relayout()
{
    if (m_tiles.isEmpty() || width() <= 0)
        return;
    const int columns = columnsFor(width());
    const int tileWidth = (width() - (columns - 1) * m_spacing) / columns;
    int top = 0;
    for (int start = 0; start < m_tiles.size(); start += columns) {
        int rowHeight = 0;
        const int end = qMin(int(m_tiles.size()), start + columns);
        for (int index = start; index < end; ++index)
            rowHeight = qMax(rowHeight, tileHeight(m_tiles.at(index), tileWidth));
        for (int index = start; index < end; ++index) {
            const int column = index - start;
            m_tiles.at(index)->setGeometry(column * (tileWidth + m_spacing), top, tileWidth, rowHeight);
        }
        top += rowHeight + m_spacing;
    }
}

} // namespace forkmesh::marketing
