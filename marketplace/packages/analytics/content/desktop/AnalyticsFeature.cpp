#include "AnalyticsFeature.h"
#include "AnalyticsReplay.h"
#include "DesignTokens.h"

#include <QAbstractButton>
#include <QApplication>
#include <QButtonGroup>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QFile>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHelpEvent>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QSvgRenderer>
#include <QTableWidget>
#include <QToolButton>
#include <QToolTip>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <utility>

namespace forkmesh::analytics {
namespace {

constexpr char kAnalyticsOrigin[] = "https://app.forkmesh.com";
constexpr qint64 kHourMs = 3600000;
constexpr int kCompactRow = 22;
constexpr int kCompactRows = 7;

QString text(const QJsonObject &object, const char *key)
{
    return object.value(QLatin1String(key)).toString();
}

double number(const QJsonObject &object, const char *key)
{
    return object.value(QLatin1String(key)).toDouble();
}

QLabel *mutedLabel(const QString &value)
{
    auto *label = new QLabel(value);
    label->setObjectName(QStringLiteral("mutedLabel"));
    label->setWordWrap(true);
    return label;
}

QLabel *sectionLabel(const QString &value)
{
    auto *label = new QLabel(value);
    if (Theme::isNext())
        label->setProperty("analyticsRole", QStringLiteral("cardTitle"));
    else
        label->setStyleSheet(QStringLiteral("font-weight: 600; font-size: 12px;"));
    return label;
}

QTableWidget *topTable(const QStringList &headers, const QString &name)
{
    auto *table = new QTableWidget(0, int(headers.size()));
    table->setObjectName(name);
    table->setAccessibleName(headers.first());
    table->setHorizontalHeaderLabels(headers);
    table->verticalHeader()->setVisible(false);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionMode(QAbstractItemView::NoSelection);
    table->setFocusPolicy(Qt::NoFocus);
    table->setAlternatingRowColors(true);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->horizontalHeader()->setMinimumSectionSize(28);
    table->verticalHeader()->setDefaultSectionSize(kCompactRow);
    table->setWordWrap(false);
    table->setTextElideMode(Qt::ElideMiddle);
    table->setFixedHeight(kCompactRow * kCompactRows + kCompactRow + 6);
    return table;
}

QFont compactFont(QFont font)
{
    if (font.pointSizeF() > 0)
        font.setPointSizeF(std::max(8.0, font.pointSizeF() - 0.5));
    return font;
}

QColor visitorColor(bool dark) { return QColor(dark ? "#3aa7e0" : "#1f8fd1"); }
QColor pageviewColor(bool dark) { return QColor(dark ? "#c98500" : "#d08a00"); }

constexpr int kNextRankedRow = 34;
constexpr int kNextRankedRows = 6;
constexpr int kNextGridRow = 36;

QFont pixelFont(QFont font, int pixels, QFont::Weight weight = QFont::Normal)
{
    font.setPixelSize(pixels);
    font.setWeight(weight);
    return font;
}

QIcon analyticsIcon(const QString &name, const QColor &color, int side)
{
    QString path = QStringLiteral(":/icons/octicons/%1.svg").arg(name);
    if (!QFile::exists(path))
        path = QStringLiteral(":/icons/codicons/%1.svg").arg(name);
    QSvgRenderer renderer(path);
    QIcon icon;
    for (const qreal scale : {1.0, 2.0}) {
        QPixmap pixmap(QSize(side, side) * scale);
        pixmap.setDevicePixelRatio(scale);
        pixmap.fill(Qt::transparent);
        if (renderer.isValid()) {
            QPainter painter(&pixmap);
            painter.setRenderHint(QPainter::Antialiasing);
            renderer.render(&painter, QRectF(0, 0, side, side));
            painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
            painter.fillRect(QRectF(0, 0, side, side), color);
        }
        icon.addPixmap(pixmap);
    }
    return icon;
}

QIcon chartKindGlyph(bool bars, const QColor &color, int side)
{
    QIcon icon;
    for (const qreal scale : {1.0, 2.0}) {
        QPixmap pixmap(QSize(side, side) * scale);
        pixmap.setDevicePixelRatio(scale);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.scale(side / 16.0, side / 16.0);
        painter.setPen(QPen(color, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        painter.drawLine(QPointF(2, 2), QPointF(2, 14));
        painter.drawLine(QPointF(2, 14), QPointF(14, 14));
        if (bars) {
            const qreal heights[][2] = {{5.5, 8}, {8.5, 4.5}, {11.5, 9.5}};
            for (const auto &bar : heights)
                painter.drawLine(QPointF(bar[0], 11.5), QPointF(bar[0], bar[1]));
        } else {
            QPainterPath line;
            line.moveTo(4.5, 10.5);
            line.lineTo(7.5, 7);
            line.lineTo(9.5, 9);
            line.lineTo(13.5, 4.5);
            painter.drawPath(line);
        }
        painter.end();
        icon.addPixmap(pixmap);
    }
    return icon;
}

void repolishTree(QWidget *widget)
{
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    for (QWidget *child : widget->findChildren<QWidget *>()) {
        child->style()->unpolish(child);
        child->style()->polish(child);
    }
    widget->update();
}

QString rangePhrase(int hours)
{
    if (hours <= 24)
        return QStringLiteral("%1 hours").arg(hours);
    return QStringLiteral("%1 days").arg(hours / 24);
}

double niceChartMax(double value)
{
    const double step = std::max(1.0, value / 3);
    const double magnitude = std::pow(10, std::floor(std::log10(step)));
    for (const double nice : {1.0, 2.0, 2.5, 5.0, 10.0}) {
        if (nice == 2.5 && magnitude < 10)
            continue;
        if (nice * magnitude >= step)
            return nice * magnitude * 3;
    }
    return magnitude * 30;
}

class StatCard final : public QFrame
{
public:
    explicit StatCard(QWidget *parent = nullptr) : QFrame(parent)
    {
        setObjectName(QStringLiteral("analyticsStatCard"));
    }

    std::function<void()> activated;

protected:
    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (activated && event->button() == Qt::LeftButton && rect().contains(event->position().toPoint()))
            activated();
        QFrame::mouseReleaseEvent(event);
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if (activated && (event->key() == Qt::Key_Space || event->key() == Qt::Key_Return
                          || event->key() == Qt::Key_Enter)) {
            activated();
            return;
        }
        QFrame::keyPressEvent(event);
    }
};

class RankedRowDelegate final : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        const QSize base = QStyledItemDelegate::sizeHint(option, index);
        if (!Theme::isNext() || index.column() == 0)
            return base;
        const bool primary = index.column() == 1;
        const QFont font = pixelFont(option.font, primary ? 13 : 12);
        return {QFontMetrics(font).horizontalAdvance(display(option, index)) + (primary ? 30 : 20),
                base.height()};
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        const auto *view = qobject_cast<const QTableView *>(option.widget);
        if (!Theme::isNext() || !view) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        const QRect cell = option.rect;
        const QRectF row(6, cell.top() + 1, std::max(0, view->viewport()->width() - 12), cell.height() - 2);
        double peak = 0;
        for (int line = 0; line < index.model()->rowCount(); ++line)
            peak = std::max(peak, index.model()->index(line, 1).data(Qt::UserRole).toDouble());
        const double value = index.siblingAtColumn(1).data(Qt::UserRole).toDouble();
        if (peak > 0 && value > 0) {
            painter->setClipRect(cell);
            QPainterPath bar;
            bar.addRoundedRect(QRectF(row.left(), row.top(), std::max<qreal>(6, row.width() * value / peak),
                                      row.height()), 6, 6);
            painter->fillPath(bar, Theme::color(Theme::Role::AccentSubtle));
            painter->setClipping(false);
        }
        const QString label = display(option, index);
        const QHeaderView *header = view->horizontalHeader();
        const bool last = header->visualIndex(index.column()) == header->count() - 1;
        QRect area;
        QFont font;
        QColor ink;
        Qt::Alignment align = Qt::AlignRight | Qt::AlignVCenter;
        if (index.column() == 0) {
            area = QRect(int(row.left()) + 10, cell.top(), cell.right() - int(row.left()) - 16, cell.height());
            font = pixelFont(option.font, 13);
            ink = Theme::color(index.siblingAtColumn(1).data(Qt::UserRole).isValid() ? Theme::Role::Fg
                                                                                    : Theme::Role::FgSubtle);
            align = Qt::AlignLeft | Qt::AlignVCenter;
        } else if (index.column() == 1) {
            area = cell.adjusted(4, 0, last ? -16 : -8, 0);
            font = pixelFont(option.font, 13);
            ink = Theme::color(Theme::Role::FgSecondary);
        } else {
            area = cell.adjusted(4, 0, last ? -16 : -10, 0);
            font = pixelFont(option.font, 12);
            ink = Theme::color(Theme::Role::FgMuted);
        }
        painter->setFont(font);
        painter->setPen(ink);
        painter->drawText(area, align, QFontMetrics(font).elidedText(label, Qt::ElideMiddle, area.width()));
        painter->restore();
    }

private:
    static QString display(const QStyleOptionViewItem &option, const QModelIndex &index)
    {
        const QString value = index.data(Qt::DisplayRole).toString();
        if (index.column() < 2 || value.isEmpty())
            return value;
        const auto *view = qobject_cast<const QTableWidget *>(option.widget);
        const QTableWidgetItem *head = view ? view->horizontalHeaderItem(index.column()) : nullptr;
        return head ? value + QLatin1Char(' ') + head->text().toLower() : value;
    }
};

void styleRankedTable(QTableWidget *table)
{
    table->setProperty("analyticsRole", QStringLiteral("ranked"));
    table->horizontalHeader()->setVisible(false);
    table->setShowGrid(false);
    table->setAlternatingRowColors(false);
    table->setFrameShape(QFrame::NoFrame);
    table->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    table->verticalHeader()->setDefaultSectionSize(kNextRankedRow);
    table->setItemDelegate(new RankedRowDelegate(table));
    if (table->columnCount() == 3)
        table->horizontalHeader()->moveSection(2, 1);
    table->setFixedHeight(kNextRankedRow * kNextRankedRows + 2);
}

void styleGridTable(QTableWidget *table)
{
    table->setProperty("analyticsRole", QStringLiteral("grid"));
    table->setShowGrid(false);
    table->setAlternatingRowColors(false);
    table->setFrameShape(QFrame::NoFrame);
    table->verticalHeader()->setDefaultSectionSize(kNextGridRow);
}

QFrame *nextCard(QLabel *title, const QString &column, QWidget *content, int margin)
{
    auto *card = new QFrame;
    card->setProperty("analyticsRole", QStringLiteral("card"));
    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto *head = new QWidget;
    head->setAttribute(Qt::WA_StyledBackground, true);
    head->setProperty("analyticsRole", QStringLiteral("cardHead"));
    head->setFixedHeight(40);
    auto *headLayout = new QHBoxLayout(head);
    headLayout->setContentsMargins(16, 0, 16, 0);
    headLayout->setSpacing(8);
    headLayout->addWidget(title);
    headLayout->addStretch(1);
    if (!column.isEmpty()) {
        auto *caption = new QLabel(column);
        caption->setProperty("analyticsRole", QStringLiteral("cardColumn"));
        QFont font = caption->font();
        font.setCapitalization(QFont::AllUppercase);
        font.setLetterSpacing(QFont::AbsoluteSpacing, 0.8);
        caption->setFont(font);
        headLayout->addWidget(caption);
    }
    layout->addWidget(head);
    auto *inner = new QVBoxLayout;
    inner->setContentsMargins(margin, margin, margin, margin);
    inner->addWidget(content);
    layout->addLayout(inner, 1);
    return card;
}

void setNextDelta(QLabel *change, QFrame *card, const QString &delta)
{
    change->setText(delta);
    const QString tone = delta.startsWith(QLatin1Char('-'))                        ? QStringLiteral("down")
                         : delta.isEmpty() || delta == QLatin1String("+0%") ? QStringLiteral("flat")
                                                                              : QStringLiteral("up");
    change->setProperty("analyticsTone", tone);
    change->style()->unpolish(change);
    change->style()->polish(change);
    change->setVisible(!delta.isEmpty());
    if (QLabel *hint = card ? card->findChild<QLabel *>(QStringLiteral("analyticsKpiHint")) : nullptr)
        hint->setVisible(!delta.isEmpty());
}

} // namespace

class AnalyticsChart final : public QWidget
{
public:
    explicit AnalyticsChart(QWidget *parent = nullptr) : QWidget(parent)
    {
        setObjectName(QStringLiteral("analyticsTrend"));
        setMinimumHeight(120);
        setMouseTracking(true);
    }

    void setSeries(const QJsonObject &timeseries)
    {
        m_labels = timeseries.value(QStringLiteral("labels")).toArray();
        m_hourly = text(timeseries, "interval") == QLatin1String("hour");
        m_visitors = QJsonArray();
        m_pageviews = QJsonArray();
        for (const QJsonValue &value : timeseries.value(QStringLiteral("series")).toArray()) {
            const QJsonObject series = value.toObject();
            if (text(series, "name") == QLatin1String("Visitors"))
                m_visitors = series.value(QStringLiteral("data")).toArray();
            else if (text(series, "name") == QLatin1String("Pageviews"))
                m_pageviews = series.value(QStringLiteral("data")).toArray();
        }
        setAccessibleName(QStringLiteral("Visitors and pageviews over the selected range"));
        if (Theme::isNext())
            setAccessibleName(QStringLiteral("%1 over the selected range").arg(metricName()));
        m_hover = -1;
        update();
    }

    void setMetric(int metric)
    {
        m_metric = metric;
        if (Theme::isNext())
            setAccessibleName(QStringLiteral("%1 over the selected range").arg(metricName()));
        update();
    }

    void setBars(bool bars)
    {
        m_bars = bars;
        update();
    }

    bool hourly() const { return m_hourly; }

protected:
    void paintEvent(QPaintEvent *) override
    {
        if (Theme::isNext())
            return paintNext();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const bool dark = palette().color(QPalette::Window).lightness() < 128;
        const QColor muted = palette().color(QPalette::PlaceholderText);
        QColor grid = muted;
        grid.setAlphaF(0.35);
        const QRectF plot = plotRect();
        double peak = 4;
        for (const QJsonArray *series : {&m_visitors, &m_pageviews})
            for (const QJsonValue &value : *series)
                peak = std::max(peak, value.toDouble() * 1.1);
        peak = std::ceil(peak / 4) * 4;
        for (int step = 0; step <= 4; ++step) {
            const qreal y = plot.bottom() - plot.height() * step / 4;
            painter.setPen(QPen(grid, 1));
            painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
            painter.setPen(muted);
            painter.drawText(QRectF(0, y - 8, plot.left() - 6, 16), Qt::AlignRight | Qt::AlignVCenter,
                             AnalyticsFeature::countText(peak * step / 4));
        }
        const int count = int(m_labels.size());
        if (count == 0)
            return;
        const qreal slot = plot.width() / count;
        const qreal barWidth = std::min<qreal>(22, std::max<qreal>(2, slot - 3));
        painter.setPen(Qt::NoPen);
        painter.setBrush(visitorColor(dark));
        for (int index = 0; index < count && index < m_visitors.size(); ++index) {
            const double value = m_visitors.at(index).toDouble();
            if (value <= 0)
                continue;
            const qreal h = std::max<qreal>(2, plot.height() * value / peak);
            painter.drawRoundedRect(QRectF(plot.left() + index * slot + (slot - barWidth) / 2,
                                           plot.bottom() - h, barWidth, h), 2, 2);
        }
        if (!m_pageviews.isEmpty()) {
            QPainterPath path;
            for (int index = 0; index < count && index < m_pageviews.size(); ++index) {
                const QPointF point(plot.left() + index * slot + slot / 2,
                                    plot.bottom() - plot.height() * m_pageviews.at(index).toDouble() / peak);
                if (index == 0)
                    path.moveTo(point);
                else
                    path.lineTo(point);
            }
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(pageviewColor(dark), 2));
            painter.drawPath(path);
        }
        const int every = std::max(1, int(std::ceil(count / std::max<qreal>(1, plot.width() / 70))));
        painter.setPen(muted);
        for (int index = 0; index < count; index += every) {
            const qreal x = plot.left() + index * slot + slot / 2;
            painter.drawText(QRectF(x - 35, plot.bottom() + 4, 70, 16), Qt::AlignHCenter | Qt::AlignTop,
                             bucketLabel(index));
        }
        const QRectF legend(plot.right() - 190, 0, 190, 14);
        painter.setPen(Qt::NoPen);
        painter.setBrush(visitorColor(dark));
        painter.drawRoundedRect(QRectF(legend.left(), legend.top() + 3, 10, 10), 2, 2);
        painter.setPen(muted);
        painter.drawText(QRectF(legend.left() + 14, legend.top(), 70, 16), Qt::AlignLeft | Qt::AlignVCenter,
                         QStringLiteral("Visitors"));
        painter.setPen(QPen(pageviewColor(dark), 2));
        painter.drawLine(QPointF(legend.left() + 90, legend.top() + 8), QPointF(legend.left() + 102, legend.top() + 8));
        painter.setPen(muted);
        painter.drawText(QRectF(legend.left() + 106, legend.top(), 84, 16), Qt::AlignLeft | Qt::AlignVCenter,
                         QStringLiteral("Pageviews"));
    }

    bool event(QEvent *event) override
    {
        if (event->type() != QEvent::ToolTip)
            return QWidget::event(event);
        const auto *help = static_cast<QHelpEvent *>(event);
        const QRectF plot = Theme::isNext() ? nextPlot() : plotRect();
        if (m_labels.isEmpty() || help->pos().x() < plot.left() || help->pos().x() > plot.right())
            return true;
        const int index = Theme::isNext()
            ? nextIndexAt(help->pos().x())
            : std::clamp(int((help->pos().x() - plot.left()) / (plot.width() / m_labels.size())), 0,
                         int(m_labels.size()) - 1);
        const QDateTime at = QDateTime::fromMSecsSinceEpoch(qint64(m_labels.at(index).toDouble()));
        QToolTip::showText(help->globalPos(),
                           QStringLiteral("%1\n%2 visitors · %3 pageviews")
                               .arg(QLocale().toString(at, m_hourly ? QStringLiteral("MMM d, h AP")
                                                                    : QStringLiteral("ddd MMM d")),
                                    AnalyticsFeature::countText(m_visitors.at(index).toDouble()),
                                    AnalyticsFeature::countText(m_pageviews.at(index).toDouble())),
                           this);
        return true;
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        QWidget::mouseMoveEvent(event);
        if (!Theme::isNext())
            return;
        const QRectF plot = nextPlot();
        const qreal x = event->position().x();
        const int hover = m_labels.isEmpty() || x < plot.left() - 8 || x > plot.right() + 8 ? -1 : nextIndexAt(x);
        if (hover != m_hover) {
            m_hover = hover;
            update();
        }
    }

    void leaveEvent(QEvent *event) override
    {
        QWidget::leaveEvent(event);
        if (m_hover >= 0) {
            m_hover = -1;
            update();
        }
    }

private:
    QString metricName() const
    {
        return m_metric == 0 ? QStringLiteral("Visitors") : QStringLiteral("Pageviews");
    }

    const QJsonArray &metricSeries() const { return m_metric == 0 ? m_visitors : m_pageviews; }

    QRectF nextPlot() const
    {
        return QRectF(40, 12, std::max<qreal>(20, width() - 48), std::max<qreal>(20, height() - 38));
    }

    qreal nextX(int index) const
    {
        const QRectF plot = nextPlot();
        const int count = int(m_labels.size());
        if (m_bars)
            return plot.left() + plot.width() / std::max(1, count) * (index + 0.5);
        return count > 1 ? plot.left() + plot.width() * index / (count - 1) : plot.center().x();
    }

    int nextIndexAt(qreal x) const
    {
        const QRectF plot = nextPlot();
        const int count = int(m_labels.size());
        if (count <= 1)
            return 0;
        const int index = m_bars ? int(std::floor((x - plot.left()) / (plot.width() / count)))
                                 : int(std::lround((x - plot.left()) / (plot.width() / (count - 1))));
        return std::clamp(index, 0, count - 1);
    }

    void paintNext()
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF plot = nextPlot();
        const QJsonArray &series = metricSeries();
        const int count = int(m_labels.size());
        double top = 0;
        for (int index = 0; index < count && index < series.size(); ++index)
            top = std::max(top, series.at(index).toDouble());
        const double peak = niceChartMax(top);
        const QColor faint = Theme::color(Theme::Role::FgSubtle);
        painter.setFont(pixelFont(font(), 10));
        for (int step = 0; step <= 3; ++step) {
            const qreal y = std::round(plot.bottom() - plot.height() * step / 3) + 0.5;
            QPen pen(Theme::color(Theme::Role::Border), 1);
            if (step > 0)
                pen.setDashPattern({2, 4});
            painter.setPen(pen);
            painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
            painter.setPen(faint);
            painter.drawText(QRectF(0, y - 8, plot.left() - 8, 16), Qt::AlignRight | Qt::AlignVCenter,
                             AnalyticsFeature::countText(peak * step / 3));
        }
        if (count == 0)
            return;
        const int every = std::max(1, int(std::ceil(count / std::max<qreal>(1, std::floor(plot.width() / 64)))));
        painter.setPen(faint);
        for (int index = 0; index < count; index += every)
            painter.drawText(QRectF(nextX(index) - 40, plot.bottom() + 8, 80, 14), Qt::AlignHCenter | Qt::AlignTop,
                             nextBucketLabel(index));
        const QColor accent = Theme::color(Theme::Role::Accent);
        const auto yFor = [&](int index) {
            const double value = index < series.size() ? series.at(index).toDouble() : 0;
            return plot.bottom() - plot.height() * value / peak;
        };
        if (m_bars) {
            const qreal slot = plot.width() / count;
            const qreal barWidth = std::min<qreal>(28, std::max<qreal>(4, slot * 0.56));
            painter.setPen(Qt::NoPen);
            for (int index = 0; index < count; ++index) {
                const qreal h = std::max<qreal>(2, plot.bottom() - yFor(index));
                const QRectF bar(nextX(index) - barWidth / 2, plot.bottom() - h, barWidth, h);
                QColor fill = accent;
                if (m_hover >= 0 && m_hover != index)
                    fill.setAlphaF(0.45);
                QPainterPath path;
                const qreal radius = std::min<qreal>(4, std::min(barWidth, h) / 2);
                path.addRoundedRect(bar, radius, radius);
                path.addRect(QRectF(bar.left(), bar.bottom() - h / 2, barWidth, h / 2));
                path.setFillRule(Qt::WindingFill);
                painter.fillPath(path.simplified(), fill);
            }
            return;
        }
        QPainterPath path;
        for (int index = 0; index < count; ++index) {
            const QPointF point(nextX(index), yFor(index));
            if (index == 0)
                path.moveTo(point);
            else
                path.lineTo(point);
        }
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(accent, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawPath(path);
        if (m_hover >= 0 && m_hover < count) {
            const qreal x = nextX(m_hover);
            painter.setPen(QPen(Theme::color(Theme::Role::BorderStrong), 1));
            painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
            painter.setPen(QPen(Theme::color(Theme::Role::Surface), 2));
            painter.setBrush(accent);
            painter.drawEllipse(QPointF(x, yFor(m_hover)), 4.5, 4.5);
        }
    }

    QString nextBucketLabel(int index) const
    {
        const QDateTime at = QDateTime::fromMSecsSinceEpoch(qint64(m_labels.at(index).toDouble()));
        return m_hourly ? QLocale().toString(at.time(), QStringLiteral("h AP"))
                        : QLocale().toString(at.date(), QStringLiteral("MMM d"));
    }

    QRectF plotRect() const
    {
        return QRectF(52, 22, std::max<qreal>(20, width() - 62), std::max<qreal>(20, height() - 46));
    }

    QString bucketLabel(int index) const
    {
        const QDateTime at = QDateTime::fromMSecsSinceEpoch(qint64(m_labels.at(index).toDouble()));
        return m_hourly ? QLocale().toString(at.time(), QStringLiteral("h AP"))
                        : at.date().toString(QStringLiteral("M/d"));
    }

    QJsonArray m_labels;
    QJsonArray m_visitors;
    QJsonArray m_pageviews;
    bool m_hourly = false;
    int m_metric = 1;
    bool m_bars = false;
    int m_hover = -1;
};

QString AnalyticsFeature::countText(double value)
{
    const double rounded = std::round(value);
    if (std::abs(rounded) >= 1000000)
        return QLocale().toString(rounded / 1000000, 'f', 1) + QStringLiteral("M");
    if (std::abs(rounded) >= 10000)
        return QLocale().toString(rounded / 1000, 'f', 1) + QStringLiteral("k");
    return QLocale().toString(qint64(rounded));
}

QString AnalyticsFeature::durationText(qint64 milliseconds)
{
    const qint64 seconds = std::max<qint64>(0, milliseconds / 1000);
    if (seconds < 60)
        return QStringLiteral("%1s").arg(seconds);
    if (seconds < 3600)
        return QStringLiteral("%1m %2s").arg(seconds / 60).arg(seconds % 60);
    return QStringLiteral("%1h %2m").arg(seconds / 3600).arg(seconds % 3600 / 60);
}

QString AnalyticsFeature::changeText(double current, double previous)
{
    if (previous <= 0)
        return current > 0 ? QStringLiteral("new") : QString();
    const long change = std::lround((current - previous) / previous * 100);
    return (change >= 0 ? QStringLiteral("+") : QString()) + QString::number(change) + QLatin1Char('%');
}

QJsonObject AnalyticsFeature::overviewQuery(qint64 nowMs, int rangeHours, int tzOffsetMinutes)
{
    return QJsonObject{
        {QStringLiteral("kind"), QStringLiteral("overview")},
        {QStringLiteral("from"), double(nowMs - rangeHours * kHourMs)},
        {QStringLiteral("to"), double(nowMs)},
        {QStringLiteral("tzOffsetMinutes"), tzOffsetMinutes},
        {QStringLiteral("filterTestAccounts"), true},
    };
}

QJsonObject AnalyticsFeature::campaignQuery(qint64 nowMs, int rangeHours, int tzOffsetMinutes, const QString &groupBy,
                                            const QString &host)
{
    QJsonObject query = overviewQuery(nowMs, rangeHours, tzOffsetMinutes);
    query.insert(QStringLiteral("kind"), QStringLiteral("campaigns"));
    query.insert(QStringLiteral("groupBy"), groupBy);
    query.insert(QStringLiteral("limit"), 15);
    if (!host.isEmpty())
        query.insert(QStringLiteral("host"), host);
    return query;
}

QString AnalyticsFeature::snippetText(const QString &origin, const QString &writeKey)
{
    return QStringLiteral(
               "<script src=\"%1/analytics-sdk.js\" defer></script>\n"
               "<script>\n"
               "  window.addEventListener(\"DOMContentLoaded\", function () {\n"
               "    window.analytics = ForkMeshAnalyticsSDK.init({\n"
               "      key: \"%2\",\n"
               "      apiHost: \"%1\"\n"
               "    });\n"
               "  });\n"
               "</script>")
        .arg(origin, writeKey);
}

QString AnalyticsFeature::projectName(const QJsonObject &project)
{
    const QString name = text(project.value(QStringLiteral("settings")).toObject(), "name").trimmed();
    if (!name.isEmpty())
        return name;
    return project.value(QStringLiteral("siteDefault")).toBool() ? QStringLiteral("Website")
                                                                 : QStringLiteral("Project");
}

AnalyticsFeature::AnalyticsFeature(const forkmesh::market::NativePackageHost &host, QWidget *parent)
    : forkmesh::market::NativePackageWidget(parent), m_request(host.request),
      m_searchIndexChanged(host.searchIndexChanged), m_packageId(host.packageId)
{
    setObjectName(QStringLiteral("analyticsSection"));
    const bool next = Theme::isNext();
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(16, 12, 16, 12);
    outer->setSpacing(6);
    if (next) {
        outer->setContentsMargins(0, 0, 0, 0);
        outer->setSpacing(0);
    }

    auto *heading = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("Analytics"));
    title->setObjectName(QStringLiteral("pageTitle"));
    QVBoxLayout *titleBlock = nullptr;
    QHBoxLayout *tabs = nullptr;
    if (next) {
        heading->setContentsMargins(24, 12, 24, 10);
        heading->setSpacing(6);
        titleBlock = new QVBoxLayout;
        titleBlock->setContentsMargins(0, 0, 0, 0);
        titleBlock->setSpacing(2);
        titleBlock->addWidget(title);
        heading->addLayout(titleBlock, 1);
        tabs = new QHBoxLayout;
        tabs->setContentsMargins(14, 0, 24, 0);
        tabs->setSpacing(4);
    } else {
        heading->addWidget(title);
    }
    heading->addStretch();
    m_organization = new QComboBox;
    m_organization->setObjectName(QStringLiteral("analyticsOrganization"));
    m_organization->setMinimumWidth(170);
    m_organization->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_organization->setAccessibleName(QStringLiteral("Organization"));
    heading->addWidget(m_organization);
    m_project = new QComboBox;
    m_project->setObjectName(QStringLiteral("analyticsProject"));
    m_project->setMinimumWidth(150);
    m_project->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_project->setAccessibleName(QStringLiteral("Project"));
    heading->addWidget(m_project);
    m_sectionGroup = new QButtonGroup(this);
    m_sectionGroup->setExclusive(true);
    const QStringList sectionNames{QStringLiteral("Overview"), QStringLiteral("Conversions"),
                                   QStringLiteral("Experiments")};
    for (int index = 0; index < sectionNames.size(); ++index) {
        auto *button = new QPushButton(sectionNames.at(index));
        button->setCheckable(true);
        button->setChecked(index == 0);
        button->setObjectName(QStringLiteral("ghostButton"));
        button->setAccessibleName(sectionNames.at(index));
        m_sectionGroup->addButton(button, index);
        if (next) {
            button->setProperty("analyticsRole", QStringLiteral("tab"));
            button->setCursor(Qt::PointingHandCursor);
            tabs->addWidget(button);
            continue;
        }
        heading->addWidget(button);
    }
    if (next)
        tabs->addStretch(1);
    else
        heading->addSpacing(8);
    QFrame *rangeSegment = nullptr;
    QHBoxLayout *rangeLayout = nullptr;
    if (next) {
        rangeSegment = new QFrame;
        rangeSegment->setObjectName(QStringLiteral("analyticsRangeSegment"));
        rangeLayout = new QHBoxLayout(rangeSegment);
        rangeLayout->setContentsMargins(2, 2, 2, 2);
        rangeLayout->setSpacing(2);
        heading->addWidget(rangeSegment);
    }
    m_rangeGroup = new QButtonGroup(this);
    m_rangeGroup->setExclusive(true);
    static constexpr std::pair<const char *, int> ranges[] = {
        {"24h", 24}, {"7d", 24 * 7}, {"30d", 24 * 30}, {"90d", 24 * 90}};
    for (const auto &[label, hours] : ranges) {
        auto *button = new QPushButton(QString::fromLatin1(label));
        button->setCheckable(true);
        button->setChecked(hours == m_rangeHours);
        button->setObjectName(QStringLiteral("ghostButton"));
        button->setAccessibleName(QStringLiteral("Last %1").arg(QString::fromLatin1(label)));
        m_rangeGroup->addButton(button, hours);
        if (next) {
            button->setProperty("analyticsRole", QStringLiteral("segment"));
            button->setCursor(Qt::PointingHandCursor);
            rangeLayout->addWidget(button);
            continue;
        }
        heading->addWidget(button);
    }
    auto *refreshButton = new QPushButton(QStringLiteral("Refresh"));
    auto *webButton = new QPushButton(QStringLiteral("Open in browser"));
    webButton->setToolTip(QStringLiteral(
        "Retention, page-level recording replays, heatmaps, feature flags and surveys open on the web dashboard"));
    for (QPushButton *button : {refreshButton, webButton}) {
        button->setObjectName(QStringLiteral("ghostButton"));
        heading->addWidget(button);
    }
    if (next) {
        const QColor iconInk = Theme::color(Theme::Role::FgMuted);
        refreshButton->setIcon(analyticsIcon(QStringLiteral("sync"), iconInk, 14));
        webButton->setIcon(analyticsIcon(QStringLiteral("link-external"), iconInk, 14));
        refreshButton->setToolTip(QStringLiteral("Refresh"));
        webButton->setToolTip(QStringLiteral("Open in browser\n") + webButton->toolTip());
        for (QPushButton *button : {refreshButton, webButton}) {
            button->setAccessibleName(button->text());
            button->setText(QString());
            button->setIconSize(QSize(14, 14));
            button->setProperty("analyticsRole", QStringLiteral("icon"));
            button->setCursor(Qt::PointingHandCursor);
        }
    }
    outer->addLayout(heading);
    if (next) {
        outer->addLayout(tabs);
        auto *divider = new QFrame;
        divider->setObjectName(QStringLiteral("analyticsHeaderDivider"));
        divider->setFixedHeight(1);
        outer->addWidget(divider);
    }

    m_status = mutedLabel(QStringLiteral("Loading Analytics…"));
    if (next) {
        m_status->setProperty("analyticsRole", QStringLiteral("summary"));
        titleBlock->addWidget(m_status);
    } else {
        outer->addWidget(m_status);
    }

    m_installPanel = new QWidget;
    auto *installLayout = new QVBoxLayout(m_installPanel);
    installLayout->setContentsMargins(8, 36, 8, 8);
    m_installNote = new QLabel;
    m_installNote->setAlignment(Qt::AlignHCenter);
    m_installNote->setWordWrap(true);
    installLayout->addWidget(m_installNote);
    auto *installRow = new QHBoxLayout;
    installRow->addStretch();
    m_installButton = new QPushButton(QStringLiteral("Install Analytics"));
    m_installButton->setObjectName(QStringLiteral("primaryButton"));
    installRow->addWidget(m_installButton);
    installRow->addStretch();
    installLayout->addLayout(installRow);
    installLayout->addStretch();
    outer->addWidget(m_installPanel, 1);

    m_emptyPanel = new QWidget;
    auto *emptyLayout = new QVBoxLayout(m_emptyPanel);
    emptyLayout->setContentsMargins(8, 36, 8, 8);
    m_emptyNote = new QLabel;
    m_emptyNote->setAlignment(Qt::AlignHCenter);
    m_emptyNote->setWordWrap(true);
    emptyLayout->addWidget(m_emptyNote);
    auto *createRow = new QHBoxLayout;
    createRow->addStretch();
    m_projectName = new QLineEdit;
    m_projectName->setPlaceholderText(QStringLiteral("Project name, e.g. Marketing site"));
    m_projectName->setAccessibleName(QStringLiteral("New project name"));
    m_projectName->setMaxLength(80);
    m_projectName->setMinimumWidth(240);
    createRow->addWidget(m_projectName);
    m_createButton = new QPushButton(QStringLiteral("Create project"));
    m_createButton->setObjectName(QStringLiteral("primaryButton"));
    createRow->addWidget(m_createButton);
    createRow->addStretch();
    emptyLayout->addLayout(createRow);
    emptyLayout->addStretch();
    outer->addWidget(m_emptyPanel, 1);

    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    m_content = scroll;
    auto *body = new QWidget;
    auto *columns = new QHBoxLayout(body);
    columns->setContentsMargins(0, 4, 0, 0);
    columns->setSpacing(14);
    auto *layout = new QVBoxLayout;
    layout->setSpacing(8);
    columns->addLayout(layout, 1);
    if (next) {
        columns->setContentsMargins(24, 20, 24, 24);
        layout->setSpacing(16);
    }

    m_kpis = new QGridLayout;
    m_kpis->setHorizontalSpacing(20);
    m_kpis->setVerticalSpacing(0);
    const QStringList kpiNames{QStringLiteral("Visitors"), QStringLiteral("Pageviews"), QStringLiteral("Sessions"),
                               QStringLiteral("Bounce rate"), QStringLiteral("Avg. session")};
    if (next)
        m_kpis->setHorizontalSpacing(12);
    for (int column = 0; next && column < kpiNames.size(); ++column) {
        auto *card = new StatCard;
        auto *cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(16, 12, 16, 12);
        cardLayout->setSpacing(4);
        QLabel *name = mutedLabel(kpiNames.at(column));
        name->setWordWrap(false);
        name->setProperty("analyticsRole", QStringLiteral("statLabel"));
        cardLayout->addWidget(name);
        auto *value = new QLabel(QStringLiteral("–"));
        value->setObjectName(QStringLiteral("analyticsKpi"));
        value->setAccessibleName(kpiNames.at(column));
        cardLayout->addWidget(value);
        auto *deltaRow = new QHBoxLayout;
        deltaRow->setContentsMargins(0, 0, 0, 0);
        deltaRow->setSpacing(6);
        auto *change = new QLabel;
        change->setObjectName(QStringLiteral("analyticsKpiChange"));
        change->setTextFormat(Qt::PlainText);
        change->setVisible(false);
        deltaRow->addWidget(change);
        auto *hint = new QLabel(QStringLiteral("vs previous"));
        hint->setObjectName(QStringLiteral("analyticsKpiHint"));
        hint->setVisible(false);
        deltaRow->addWidget(hint);
        deltaRow->addStretch(1);
        cardLayout->addLayout(deltaRow);
        cardLayout->addStretch(1);
        m_kpis->addWidget(card, 0, column);
        m_kpis->setColumnStretch(column, 1);
        m_kpiValues.append(value);
        m_kpiChanges.append(change);
        m_kpiCards.append(card);
        if (column > 1)
            continue;
        card->setProperty("analyticsClickable", true);
        card->setProperty("analyticsSelected", column == m_chartMetric);
        card->setCursor(Qt::PointingHandCursor);
        card->setFocusPolicy(Qt::TabFocus);
        card->setAccessibleName(QStringLiteral("Show %1 in the chart").arg(kpiNames.at(column).toLower()));
        card->setToolTip(card->accessibleName());
        card->activated = [this, column] { selectChartMetric(column); };
    }
    for (int column = 0; !next && column < kpiNames.size(); ++column) {
        m_kpis->addWidget(mutedLabel(kpiNames.at(column)), 0, column);
        auto *value = new QLabel(QStringLiteral("–"));
        value->setObjectName(QStringLiteral("analyticsKpi"));
        value->setAccessibleName(kpiNames.at(column));
        value->setStyleSheet(QStringLiteral("font-size: 18px; font-weight: 600;"));
        m_kpis->addWidget(value, 1, column);
        m_kpiValues.append(value);
        auto *change = new QLabel;
        change->setObjectName(QStringLiteral("analyticsKpiChange"));
        m_kpis->addWidget(change, 2, column);
        m_kpiChanges.append(change);
    }
    if (!next)
        m_kpis->setColumnStretch(kpiNames.size(), 1);
    layout->addLayout(m_kpis);

    m_chart = new AnalyticsChart;
    m_chart->setFixedHeight(150);
    if (next) {
        m_chart->setFixedHeight(240);
        m_chart->setMetric(m_chartMetric);
        auto *chartCard = new QFrame;
        chartCard->setObjectName(QStringLiteral("analyticsChartCard"));
        chartCard->setProperty("analyticsRole", QStringLiteral("card"));
        auto *chartLayout = new QVBoxLayout(chartCard);
        chartLayout->setContentsMargins(16, 14, 16, 12);
        chartLayout->setSpacing(10);
        auto *chartHead = new QHBoxLayout;
        chartHead->setSpacing(12);
        auto *chartWords = new QVBoxLayout;
        chartWords->setSpacing(2);
        m_chartTitle = new QLabel;
        m_chartTitle->setProperty("analyticsRole", QStringLiteral("cardTitle"));
        m_chartHint = new QLabel;
        m_chartHint->setProperty("analyticsRole", QStringLiteral("cardHint"));
        chartWords->addWidget(m_chartTitle);
        chartWords->addWidget(m_chartHint);
        chartHead->addLayout(chartWords, 1);
        auto *kindFrame = new QFrame;
        kindFrame->setObjectName(QStringLiteral("analyticsChartKind"));
        auto *kindLayout = new QHBoxLayout(kindFrame);
        kindLayout->setContentsMargins(2, 2, 2, 2);
        kindLayout->setSpacing(2);
        auto *kindGroup = new QButtonGroup(kindFrame);
        kindGroup->setExclusive(true);
        const QColor kindInk = Theme::color(Theme::Role::FgMuted);
        for (int kind = 0; kind < 2; ++kind) {
            auto *button = new QToolButton;
            button->setProperty("analyticsRole", QStringLiteral("chartKind"));
            button->setCheckable(true);
            button->setChecked(kind == 0);
            button->setAutoRaise(true);
            button->setFixedSize(28, 28);
            button->setIconSize(QSize(14, 14));
            button->setIcon(chartKindGlyph(kind == 1, kindInk, 14));
            button->setCursor(Qt::PointingHandCursor);
            const QString label = kind == 0 ? QStringLiteral("Line chart") : QStringLiteral("Bar chart");
            button->setToolTip(label);
            button->setAccessibleName(label);
            kindGroup->addButton(button, kind);
            kindLayout->addWidget(button);
        }
        connect(kindGroup, &QButtonGroup::idClicked, this, [this](int kind) { m_chart->setBars(kind == 1); });
        chartHead->addWidget(kindFrame, 0, Qt::AlignTop);
        chartLayout->addLayout(chartHead);
        chartLayout->addWidget(m_chart);
        layout->addWidget(chartCard);
        renderChartHeading();
    } else {
        layout->addWidget(m_chart);
    }

    auto *tables = new QGridLayout;
    tables->setHorizontalSpacing(10);
    tables->setVerticalSpacing(3);
    m_pages = topTable({QStringLiteral("Page"), QStringLiteral("Views"), QStringLiteral("Visitors")},
                       QStringLiteral("analyticsPages"));
    m_referrers = topTable({QStringLiteral("Referrer"), QStringLiteral("Views")}, QStringLiteral("analyticsReferrers"));
    m_countries = topTable({QStringLiteral("Country"), QStringLiteral("Views")}, QStringLiteral("analyticsCountries"));
    m_devices = topTable({QStringLiteral("Device"), QStringLiteral("Views")}, QStringLiteral("analyticsDevices"));
    m_browsers = topTable({QStringLiteral("Browser"), QStringLiteral("Views")}, QStringLiteral("analyticsBrowsers"));
    m_events = topTable({QStringLiteral("Event"), QStringLiteral("Count")}, QStringLiteral("analyticsEvents"));
    m_landing = topTable({QStringLiteral("Landing page"), QStringLiteral("Visits"), QStringLiteral("Bounce")},
                         QStringLiteral("analyticsLanding"));
    m_terms = topTable({QStringLiteral("Search term"), QStringLiteral("Visits"), QStringLiteral("Bounce")},
                       QStringLiteral("analyticsSearchTerms"));
    const QList<std::pair<QString, QTableWidget *>> sections{
        {QStringLiteral("Top pages"), m_pages},     {QStringLiteral("Referrers"), m_referrers},
        {QStringLiteral("Landing pages"), m_landing}, {QStringLiteral("Search terms"), m_terms},
        {QStringLiteral("Countries"), m_countries}, {QStringLiteral("Devices"), m_devices},
        {QStringLiteral("Browsers"), m_browsers},   {QStringLiteral("Events"), m_events}};
    constexpr int kTableColumns = 4;
    if (next) {
        tables->setHorizontalSpacing(12);
        tables->setVerticalSpacing(12);
        const QList<std::pair<QString, QTableWidget *>> cards{
            {QStringLiteral("Top pages"), m_pages},     {QStringLiteral("Referrers"), m_referrers},
            {QStringLiteral("Landing pages"), m_landing}, {QStringLiteral("Countries"), m_countries},
            {QStringLiteral("Devices"), m_devices},     {QStringLiteral("Events"), m_events},
            {QStringLiteral("Browsers"), m_browsers},   {QStringLiteral("Search terms"), m_terms}};
        constexpr int kCardColumns = 3;
        for (int index = 0; index < cards.size(); ++index) {
            QTableWidget *table = cards.at(index).second;
            styleRankedTable(table);
            const QTableWidgetItem *column = table->horizontalHeaderItem(1);
            tables->addWidget(nextCard(sectionLabel(cards.at(index).first), column ? column->text() : QString(),
                                       table, 6),
                              index / kCardColumns, index % kCardColumns);
        }
        for (int column = 0; column < kCardColumns; ++column)
            tables->setColumnStretch(column, 1);
    }
    for (int index = 0; !next && index < sections.size(); ++index) {
        const int row = index / kTableColumns * 2;
        const int column = index % kTableColumns;
        tables->addWidget(sectionLabel(sections.at(index).first), row, column);
        tables->addWidget(sections.at(index).second, row + 1, column);
        tables->setColumnStretch(column, 1);
    }
    m_campaigns = topTable({QStringLiteral("Campaign"), QStringLiteral("Source / medium"), QStringLiteral("Visits"),
                            QStringLiteral("Change"), QStringLiteral("Bounce"), QStringLiteral("Avg. time"),
                            QStringLiteral("Lands on")},
                           QStringLiteral("analyticsCampaigns"));
    const int campaignRow = int(sections.size() + kTableColumns - 1) / kTableColumns * 2;
    auto *campaignsLabel = sectionLabel(QStringLiteral("Campaigns"));
    campaignsLabel->setToolTip(QStringLiteral(
        "Visits grouped by their first page's campaign tags (utm_source, utm_medium, utm_campaign), or by the referring site when untagged"));
    if (next) {
        styleGridTable(m_campaigns);
        m_campaigns->setFixedHeight(kNextGridRow * kCompactRows + kNextGridRow + 2);
        layout->addLayout(tables);
        layout->addWidget(nextCard(campaignsLabel, QString(), m_campaigns, 0));
    } else {
        tables->addWidget(campaignsLabel, campaignRow, 0, 1, kTableColumns);
        tables->addWidget(m_campaigns, campaignRow + 1, 0, 1, kTableColumns);
        layout->addLayout(tables);
    }

    m_truncated = mutedLabel(QStringLiteral(
        "This range holds more events than one query scans, so only the most recent ones were counted."));
    m_truncated->setVisible(false);
    layout->addWidget(m_truncated);

    QHBoxLayout *bottomRow = nullptr;
    if (next) {
        m_writeKey = mutedLabel(QString());
        m_writeKey->setTextInteractionFlags(Qt::TextSelectableByMouse);
    }
    auto *copyButton = new QPushButton(QStringLiteral("Copy snippet"));
    copyButton->setObjectName(QStringLiteral("ghostButton"));
    copyButton->setToolTip(QStringLiteral("Copy the script tags to paste before </head> on your site"));
    if (next) {
        m_writeKey->setTextFormat(Qt::PlainText);
        m_writeKey->setWordWrap(false);
        m_writeKey->setProperty("analyticsRole", QStringLiteral("key"));
        m_writeKey->setFont(Theme::monospaceFont(pixelFont(m_writeKey->font(), 11)));
        copyButton->setProperty("analyticsRole", QStringLiteral("action"));
        copyButton->setIcon(analyticsIcon(QStringLiteral("copy"), Theme::color(Theme::Role::FgMuted), 14));
        copyButton->setIconSize(QSize(14, 14));
        auto *setupCard = new QFrame;
        setupCard->setObjectName(QStringLiteral("analyticsSetupCard"));
        setupCard->setProperty("analyticsRole", QStringLiteral("card"));
        auto *setupLayout = new QVBoxLayout(setupCard);
        setupLayout->setContentsMargins(16, 12, 16, 14);
        setupLayout->setSpacing(8);
        auto *setupHead = new QHBoxLayout;
        setupHead->addWidget(sectionLabel(QStringLiteral("Add this project to a site")));
        setupHead->addStretch(1);
        setupHead->addWidget(copyButton);
        setupLayout->addLayout(setupHead);
        setupLayout->addWidget(m_writeKey);
        setupLayout->addStretch(1);
        bottomRow = new QHBoxLayout;
        bottomRow->setSpacing(12);
        bottomRow->addWidget(setupCard, 1);
        layout->addLayout(bottomRow);
    } else {
        auto *setupRow = new QHBoxLayout;
        setupRow->addWidget(sectionLabel(QStringLiteral("Add this project to a site")));
        m_writeKey = mutedLabel(QString());
        m_writeKey->setTextInteractionFlags(Qt::TextSelectableByMouse);
        setupRow->addWidget(m_writeKey, 1);
        setupRow->addWidget(copyButton);
        layout->addLayout(setupRow);
    }
    layout->addStretch();

    auto *recordingsPanel = new QWidget;
    recordingsPanel->setObjectName(QStringLiteral("analyticsRecordingsPanel"));
    if (!next)
        recordingsPanel->setFixedWidth(380);
    auto *recordingsLayout = new QVBoxLayout(recordingsPanel);
    recordingsLayout->setContentsMargins(0, 0, 0, 0);
    recordingsLayout->setSpacing(3);
    if (next) {
        recordingsPanel->setAttribute(Qt::WA_StyledBackground, true);
        recordingsPanel->setProperty("analyticsRole", QStringLiteral("card"));
        recordingsLayout->setContentsMargins(16, 12, 16, 12);
        recordingsLayout->setSpacing(8);
    }
    auto *recordingsTitle = sectionLabel(QStringLiteral("Recordings"));
    recordingsTitle->setToolTip(QStringLiteral(
        "The latest recorded visits. Click one to play back the visitor's mouse movement, clicks and scrolling."));
    recordingsLayout->addWidget(recordingsTitle);
    m_recordingsNote = mutedLabel(QString());
    m_recordingsNote->setVisible(false);
    recordingsLayout->addWidget(m_recordingsNote);
    m_recordingsEnable = new QPushButton(QStringLiteral("Turn on recordings"));
    m_recordingsEnable->setObjectName(QStringLiteral("ghostButton"));
    m_recordingsEnable->setVisible(false);
    recordingsLayout->addWidget(m_recordingsEnable, 0, Qt::AlignLeft);
    m_recordings = topTable({QStringLiteral(""), QStringLiteral("Entry page"), QStringLiteral("Pages"),
                             QStringLiteral("Length"), QStringLiteral("When")},
                            QStringLiteral("analyticsRecordings"));
    m_recordings->setAccessibleName(QStringLiteral("Recordings"));
    m_recordings->setMaximumHeight(QWIDGETSIZE_MAX);
    m_recordings->setMinimumHeight(240);
    m_recordings->setSelectionMode(QAbstractItemView::SingleSelection);
    m_recordings->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_recordings->setFocusPolicy(Qt::StrongFocus);
    m_recordings->setCursor(Qt::PointingHandCursor);
    m_recordings->setFont(compactFont(m_recordings->font()));
    m_recordings->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_recordings->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    recordingsLayout->addWidget(m_recordings, 1);
    if (next) {
        m_recordingsEnable->setProperty("analyticsRole", QStringLiteral("action"));
        styleGridTable(m_recordings);
        m_recordings->setVisible(false);
        bottomRow->insertWidget(0, recordingsPanel, 1);
    } else {
        columns->addWidget(recordingsPanel);
    }
    scroll->setWidget(body);
    m_sections = new QStackedWidget;
    m_sections->addWidget(m_content);
    m_sections->addWidget(buildConversionsPage());
    m_sections->addWidget(buildExperimentsPage());
    outer->addWidget(m_sections, 1);
    for (int index = 1; next && index < m_sections->count(); ++index) {
        auto *area = qobject_cast<QScrollArea *>(m_sections->widget(index));
        if (area && area->widget() && area->widget()->layout())
            area->widget()->layout()->setContentsMargins(24, 16, 24, 24);
    }

    connect(m_organization, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index < 0 || m_loading)
            return;
        m_projects = QJsonObject();
        m_overview = QJsonObject();
        clearManaged();
        loadOrganization();
    });
    connect(m_project, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index < 0 || m_loading)
            return;
        m_overview = QJsonObject();
        clearManaged();
        applyState();
        reloadSection();
    });
    connect(m_rangeGroup, &QButtonGroup::idClicked, this, [this](int hours) {
        m_rangeHours = hours;
        reloadSection();
    });
    connect(m_sectionGroup, &QButtonGroup::idClicked, this, [this](int index) { showSection(index); });
    connect(refreshButton, &QPushButton::clicked, this, [this] { refreshOrganizations(); });
    connect(webButton, &QPushButton::clicked, this, [this] { openWebDashboard(); });
    connect(m_installButton, &QPushButton::clicked, this, [this] { installPlugin(); });
    connect(m_createButton, &QPushButton::clicked, this, [this] { createProject(); });
    connect(m_projectName, &QLineEdit::returnPressed, this, [this] { createProject(); });
    connect(copyButton, &QPushButton::clicked, this, [this] { copySnippet(); });
    connect(m_recordings, &QTableWidget::cellClicked, this, [this](int row, int) { openRecording(row); });
    connect(m_recordings, &QTableWidget::cellActivated, this, [this](int row, int) { openRecording(row); });
    connect(m_recordingsEnable, &QPushButton::clicked, this, [this] { enableRecordings(); });

    applyState();
}

void AnalyticsFeature::request(const QByteArray &method, const QString &path, const QJsonObject &body,
                               ReplyHandler handler)
{
    if (!m_request) {
        handler(false, {}, QStringLiteral("Analytics network access is unavailable."));
        return;
    }
    const QPointer<AnalyticsFeature> guard(this);
    m_request(method, path, body,
              [guard, handler = std::move(handler)](bool ok, const QJsonObject &payload,
                                                    const QString &error) mutable {
        if (guard)
            handler(ok, payload, error);
    });
}

QString AnalyticsFeature::organization() const
{
    return m_organization->currentData().toString();
}

QString AnalyticsFeature::organizationPath(const QString &suffix) const
{
    return QStringLiteral("/api/analytics/") + organization() + suffix;
}

QJsonObject AnalyticsFeature::organizationRecord() const
{
    const QString name = organization();
    for (const QJsonValue &value : m_organizations) {
        const QJsonObject item = value.toObject();
        if (text(item, "name") == name)
            return item;
    }
    return {};
}

QJsonObject AnalyticsFeature::projectRecord() const
{
    const QString id = m_project->currentData().toString();
    for (const QJsonValue &value : m_projects.value(QStringLiteral("projects")).toArray()) {
        const QJsonObject project = value.toObject();
        if (text(project, "id") == id)
            return project;
    }
    return {};
}

void AnalyticsFeature::refresh()
{
    refreshOrganizations();
}

bool AnalyticsFeature::exportData(forkmesh::market::NativePackageReplyHandler done)
{
    const QString path = forkmesh::market::nativePackageLifecyclePath(
        organization(), m_packageId, QStringLiteral("export"));
    if (path.isEmpty() || !organizationRecord().value(QStringLiteral("installed")).toBool())
        return false;
    request(QByteArrayLiteral("GET"), path, {}, std::move(done));
    return true;
}

QList<forkmesh::market::NativePackageSearchHit> AnalyticsFeature::searchHits(const QString &query, int limit) const
{
    const QString needle = query.trimmed();
    if (limit <= 0 || needle.size() < 2)
        return {};
    QList<forkmesh::market::NativePackageSearchHit> hits;
    for (const QJsonValue &value : m_projects.value(QStringLiteral("projects")).toArray()) {
        const QJsonObject project = value.toObject();
        const QString name = projectName(project);
        if (hits.size() < limit && name.contains(needle, Qt::CaseInsensitive))
            hits.append({QStringLiteral("project:") + text(project, "id"), name,
                         QStringLiteral("Analytics project in %1").arg(organization())});
    }
    const QJsonArray pages = m_overview.value(QStringLiteral("result")).toObject()
                                 .value(QStringLiteral("breakdowns")).toObject()
                                 .value(QStringLiteral("pages")).toArray();
    for (const QJsonValue &value : pages) {
        const QJsonObject page = value.toObject();
        if (hits.size() >= limit)
            break;
        if (text(page, "value").contains(needle, Qt::CaseInsensitive))
            hits.append({QStringLiteral("page:") + text(page, "value"), text(page, "value"),
                         QStringLiteral("%1 views · %2 visitors in the last %3")
                             .arg(countText(number(page, "count")), countText(number(page, "visitors")),
                                  m_rangeGroup->button(m_rangeHours)->text())});
    }
    return hits;
}

void AnalyticsFeature::activateSearchHit(const QString &id)
{
    if (id.startsWith(QLatin1String("project:"))) {
        const int index = m_project->findData(id.mid(8));
        if (index >= 0)
            m_project->setCurrentIndex(index);
        return;
    }
    if (!id.startsWith(QLatin1String("page:")))
        return;
    const QString path = id.mid(5);
    for (int row = 0; row < m_pages->rowCount(); ++row) {
        QTableWidgetItem *item = m_pages->item(row, 0);
        if (item && item->text() == path) {
            m_pages->scrollToItem(item);
            m_pages->setFocus();
            setStatus(QStringLiteral("%1: %2 views, %3 visitors in the last %4")
                          .arg(path, m_pages->item(row, 1)->text(), m_pages->item(row, 2)->text(),
                               m_rangeGroup->button(m_rangeHours)->text()));
            return;
        }
    }
}

void AnalyticsFeature::refreshOrganizations()
{
    if (m_loading)
        return;
    m_loading = true;
    const QString previous = organization();
    setStatus(QStringLiteral("Loading your organizations…"));
    request(QByteArrayLiteral("GET"), QStringLiteral("/api/analytics"), {},
            [this, previous](bool ok, const QJsonObject &payload, const QString &error) {
        m_loading = false;
        if (!ok)
            return setStatus(QStringLiteral("Analytics could not load: %1").arg(error), true);
        m_organizations = payload.value(QStringLiteral("organizations")).toArray();
        int preferred = -1;
        m_organization->blockSignals(true);
        m_organization->clear();
        for (const QJsonValue &value : std::as_const(m_organizations)) {
            const QJsonObject item = value.toObject();
            QString label = text(item, "name") + QStringLiteral(" · ") + text(item, "role");
            if (!item.value(QStringLiteral("installed")).toBool())
                label += QStringLiteral(" · not installed");
            else if (preferred < 0)
                preferred = m_organization->count();
            m_organization->addItem(label, text(item, "name"));
        }
        const int index = m_organization->findData(previous);
        m_organization->setCurrentIndex(index >= 0 ? index : std::max(0, preferred));
        m_organization->blockSignals(false);
        if (!m_organization->count()) {
            m_projects = QJsonObject();
            m_overview = QJsonObject();
            applyState();
            return setStatus(QStringLiteral("Join or create an organization first."), true);
        }
        loadOrganization();
    });
}

void AnalyticsFeature::loadOrganization()
{
    if (m_loading || organization().isEmpty())
        return;
    if (!organizationRecord().value(QStringLiteral("installed")).toBool()) {
        m_projects = QJsonObject();
        m_overview = QJsonObject();
        applyState();
        return setStatus(QStringLiteral("Analytics is not installed for %1.").arg(organization()));
    }
    m_loading = true;
    const QString previous = m_project->currentData().toString();
    setStatus(QStringLiteral("Loading projects…"));
    request(QByteArrayLiteral("GET"), organizationPath(QStringLiteral("/projects")), {},
            [this, previous](bool ok, const QJsonObject &payload, const QString &error) {
        m_loading = false;
        if (!ok) {
            m_projects = QJsonObject();
            applyState();
            return setStatus(QStringLiteral("Analytics could not load: %1").arg(error), true);
        }
        m_projects = payload;
        const QJsonArray projects = payload.value(QStringLiteral("projects")).toArray();
        int fallback = 0;
        m_project->blockSignals(true);
        m_project->clear();
        for (const QJsonValue &value : projects) {
            const QJsonObject project = value.toObject();
            if (project.value(QStringLiteral("siteDefault")).toBool())
                fallback = m_project->count();
            m_project->addItem(projectName(project), text(project, "id"));
        }
        const int index = m_project->findData(previous);
        m_project->setCurrentIndex(index >= 0 ? index : fallback);
        m_project->blockSignals(false);
        applyState();
        if (m_searchIndexChanged)
            m_searchIndexChanged();
        if (projects.isEmpty())
            return setStatus(QStringLiteral("%1 has no analytics project yet.").arg(organization()));
        reloadSection();
    });
}

void AnalyticsFeature::loadOverview()
{
    const QString id = m_project->currentData().toString();
    if (organization().isEmpty() || id.isEmpty())
        return;
    const int serial = ++m_serial;
    setStatus(QStringLiteral("Loading the last %1…").arg(m_rangeGroup->button(m_rangeHours)->text()));
    request(QByteArrayLiteral("POST"), organizationPath(QStringLiteral("/projects/") + id + QStringLiteral("/query")),
            overviewQuery(QDateTime::currentMSecsSinceEpoch(), m_rangeHours,
                          QDateTime::currentDateTime().offsetFromUtc() / 60),
            [this, serial](bool ok, const QJsonObject &payload, const QString &error) {
        if (serial != m_serial)
            return;
        if (!ok)
            return setStatus(QStringLiteral("The overview could not load: %1").arg(error), true);
        m_overview = payload;
        renderOverview();
        applyState();
        if (m_searchIndexChanged)
            m_searchIndexChanged();
    });
    loadCampaigns(serial);
    loadRecordings(serial);
}

void AnalyticsFeature::loadCampaigns(int serial)
{
    const QString id = m_project->currentData().toString();
    QString host;
    for (const QJsonValue &origin : projectRecord().value(QStringLiteral("settings")).toObject()
                                        .value(QStringLiteral("allowedOrigins")).toArray()) {
        host = QUrl(QString(origin.toString()).replace(QStringLiteral("://*."), QStringLiteral("://"))).host();
        if (!host.isEmpty())
            break;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const int offset = QDateTime::currentDateTime().offsetFromUtc() / 60;
    for (const QString &group : {QStringLiteral("campaign"), QStringLiteral("landing"), QStringLiteral("term")}) {
        request(QByteArrayLiteral("POST"), organizationPath(QStringLiteral("/projects/") + id + QStringLiteral("/query")),
                campaignQuery(now, m_rangeHours, offset, group, host),
                [this, serial, group](bool ok, const QJsonObject &payload, const QString &) {
            if (serial != m_serial)
                return;
            fillCampaigns(group, ok ? payload.value(QStringLiteral("result")).toObject() : QJsonObject());
        });
    }
}

void AnalyticsFeature::fillCampaigns(const QString &groupBy, const QJsonObject &result)
{
    QTableWidget *table = groupBy == QLatin1String("campaign") ? m_campaigns
                          : groupBy == QLatin1String("landing") ? m_landing
                                                                : m_terms;
    table->setRowCount(0);
    const auto cell = [table](int row, int column, const QString &value, bool right) {
        auto *item = new QTableWidgetItem(value);
        item->setToolTip(value);
        if (right)
            item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        table->setItem(row, column, item);
    };
    const auto bounce = [](const QJsonObject &row) {
        return QStringLiteral("%1%").arg(std::lround(number(row, "bounceRate") * 100));
    };
    for (const QJsonValue &value : result.value(QStringLiteral("rows")).toArray()) {
        const QJsonObject row = value.toObject();
        if (groupBy == QLatin1String("term") && text(row, "term").startsWith(QLatin1Char('(')))
            continue;
        const int index = table->rowCount();
        table->insertRow(index);
        if (groupBy == QLatin1String("campaign")) {
            const QString campaign = text(row, "campaign");
            const double sessions = number(row, "sessions");
            const double before = number(row, "previousSessions");
            const QJsonArray landing = row.value(QStringLiteral("topLanding")).toArray();
            cell(index, 0, campaign == QLatin1String("(none)") ? QStringLiteral("No campaign") : campaign, false);
            cell(index, 1, text(row, "source") + QStringLiteral(" / ") + text(row, "medium"), false);
            cell(index, 2, countText(sessions), true);
            cell(index, 3, before > 0 ? changeText(sessions, before) : QStringLiteral("new"), true);
            cell(index, 4, bounce(row), true);
            cell(index, 5, durationText(qint64(number(row, "averageSessionMs"))), true);
            cell(index, 6, landing.isEmpty() ? QString() : text(landing.first().toObject(), "value"), false);
        } else {
            cell(index, 0, text(row, groupBy == QLatin1String("landing") ? "landing" : "term"), false);
            cell(index, 1, countText(number(row, "sessions")), true);
            cell(index, 2, bounce(row), true);
            if (Theme::isNext())
                table->item(index, 1)->setData(Qt::UserRole, number(row, "sessions"));
        }
    }
    if (table->rowCount() == 0) {
        table->insertRow(0);
        auto *empty = new QTableWidgetItem(groupBy == QLatin1String("term")
            ? QStringLiteral("No search terms in this range")
            : QStringLiteral("No data in this range"));
        empty->setForeground(palette().color(QPalette::PlaceholderText));
        table->setItem(0, 0, empty);
    }
}

void AnalyticsFeature::installPlugin()
{
    const QString path = forkmesh::market::nativePackageLifecyclePath(
        organization(), m_packageId, QStringLiteral("install"));
    if (path.isEmpty())
        return setStatus(QStringLiteral("Select a valid organization before installing Analytics."), true);
    m_installButton->setEnabled(false);
    request(QByteArrayLiteral("POST"), path, {},
            [this](bool ok, const QJsonObject &, const QString &error) {
        m_installButton->setEnabled(true);
        if (!ok)
            return setStatus(QStringLiteral("Analytics was not installed: %1").arg(error), true);
        setStatus(QStringLiteral("Analytics installed for this organization."));
        refreshOrganizations();
    });
}

void AnalyticsFeature::createProject()
{
    if (organization().isEmpty() || !m_createButton->isVisible())
        return;
    const QString name = m_projectName->text().trimmed();
    m_createButton->setEnabled(false);
    request(QByteArrayLiteral("POST"), organizationPath(QStringLiteral("/projects")),
            QJsonObject{{QStringLiteral("name"), name.isEmpty() ? QStringLiteral("Website") : name},
                        {QStringLiteral("timezoneOffsetMinutes"),
                         QDateTime::currentDateTime().offsetFromUtc() / 60}},
            [this](bool ok, const QJsonObject &payload, const QString &error) {
        m_createButton->setEnabled(true);
        if (!ok)
            return setStatus(QStringLiteral("The project was not created: %1").arg(error), true);
        m_projectName->clear();
        m_project->blockSignals(true);
        m_project->addItem(projectName(payload.value(QStringLiteral("project")).toObject()),
                           text(payload.value(QStringLiteral("project")).toObject(), "id"));
        m_project->setCurrentIndex(m_project->count() - 1);
        m_project->blockSignals(false);
        loadOrganization();
    });
}

void AnalyticsFeature::copySnippet()
{
    const QString key = text(projectRecord(), "writeKey");
    if (key.isEmpty())
        return;
    QApplication::clipboard()->setText(snippetText(QString::fromLatin1(kAnalyticsOrigin), key));
    setStatus(QStringLiteral("Snippet copied. Paste it before </head> on every page you want counted."));
}

void AnalyticsFeature::openWebDashboard()
{
    QUrl url(QString::fromLatin1(kAnalyticsOrigin) + QStringLiteral("/dashboard/analytics"));
    if (!organization().isEmpty())
        url.setQuery(QStringLiteral("org=") + QString::fromUtf8(QUrl::toPercentEncoding(organization())));
    QDesktopServices::openUrl(url);
}

void AnalyticsFeature::applyState()
{
    const QJsonObject record = organizationRecord();
    const QString role = text(record, "role");
    const bool listed = !record.isEmpty();
    const bool installed = record.value(QStringLiteral("installed")).toBool();
    const bool canManage = role == QLatin1String("owner") || role == QLatin1String("admin");
    const bool projectsLoaded = m_projects.contains(QStringLiteral("projects"));
    const bool hasProjects = !m_projects.value(QStringLiteral("projects")).toArray().isEmpty();
    m_installPanel->setVisible(listed && !installed);
    m_installButton->setVisible(canManage);
    m_installNote->setText(canManage
        ? QStringLiteral("Analytics counts visitors, pageviews and events on your organization's sites, on this "
                         "ForkMesh instance only: no third-party service sees a visitor and no IP address is "
                         "stored. Install it for %1 to start.").arg(organization())
        : QStringLiteral("Ask an owner or admin of %1 to install Analytics.").arg(organization()));
    const bool manage = m_projects.value(QStringLiteral("canManage")).toBool(canManage);
    m_emptyPanel->setVisible(installed && projectsLoaded && !hasProjects);
    m_projectName->setVisible(manage);
    m_createButton->setVisible(manage);
    m_emptyNote->setText(manage
        ? QStringLiteral("Create a project for each site or app you want to measure. You will get a snippet to "
                         "paste into its pages.")
        : QStringLiteral("No analytics project yet. Ask an owner or admin of %1 to create one.").arg(organization()));
    m_project->setVisible(installed && m_project->count() > 1);
    for (QAbstractButton *button : m_rangeGroup->buttons())
        button->setVisible(installed && hasProjects);
    if (auto *segment = findChild<QFrame *>(QStringLiteral("analyticsRangeSegment")))
        segment->setVisible(installed && hasProjects);
    for (QAbstractButton *button : m_sectionGroup->buttons())
        button->setVisible(installed && hasProjects);
    m_content->setVisible(!m_overview.isEmpty());
    m_sections->setVisible(installed && hasProjects && (m_section != 0 || !m_overview.isEmpty()));
    m_playbook->setVisible(canManage && m_section == 1
                           && projectRecord().value(QStringLiteral("playbook")).toObject()
                                  .value(QStringLiteral("applied")).toArray().isEmpty());
    m_newExperiment->setEnabled(canManage);
    const QString key = text(projectRecord(), "writeKey");
    m_writeKey->setText(key.isEmpty() ? QString()
                                      : QStringLiteral("Write key %1 · send events to %2/api/analytics/_/capture")
                                            .arg(key, QString::fromLatin1(kAnalyticsOrigin)));
}

void AnalyticsFeature::fillTable(QTableWidget *table, const QJsonArray &rows, bool visitors)
{
    table->setRowCount(0);
    for (const QJsonValue &value : rows) {
        const QJsonObject row = value.toObject();
        const int index = table->rowCount();
        table->insertRow(index);
        const QString flag = table == m_countries ? MotionTrack::countryFlag(text(row, "value")) : QString();
        auto *label = new QTableWidgetItem(flag.isEmpty() ? text(row, "value") : flag + QLatin1Char(' ') + text(row, "value"));
        label->setToolTip(text(row, "value"));
        table->setItem(index, 0, label);
        auto *count = new QTableWidgetItem(countText(number(row, "count")));
        count->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        if (Theme::isNext())
            count->setData(Qt::UserRole, number(row, "count"));
        table->setItem(index, 1, count);
        if (visitors) {
            auto *people = new QTableWidgetItem(countText(number(row, "visitors")));
            people->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            table->setItem(index, 2, people);
        }
    }
    if (rows.isEmpty()) {
        table->insertRow(0);
        auto *empty = new QTableWidgetItem(QStringLiteral("No data in this range"));
        empty->setForeground(palette().color(QPalette::PlaceholderText));
        table->setItem(0, 0, empty);
    }
}

QString AnalyticsFeature::agoText(qint64 nowMs, qint64 atMs)
{
    const qint64 seconds = std::max<qint64>(0, (nowMs - atMs) / 1000);
    if (seconds < 60)
        return QStringLiteral("now");
    if (seconds < 3600)
        return QStringLiteral("%1m ago").arg(seconds / 60);
    if (seconds < 86400)
        return QStringLiteral("%1h ago").arg(seconds / 3600);
    if (seconds < 7 * 86400)
        return QStringLiteral("%1d ago").arg(seconds / 86400);
    return QLocale().toString(QDateTime::fromMSecsSinceEpoch(atMs).date(), QStringLiteral("MMM d"));
}

QString AnalyticsFeature::recordingTip(const QJsonObject &recording)
{
    QStringList trail;
    for (const QJsonValue &page : recording.value(QStringLiteral("pages")).toArray())
        trail.append(MotionTrack::pathOf(page.toString()));
    if (trail.isEmpty())
        trail.append(MotionTrack::pathOf(text(recording, "startUrl")));
    const QJsonObject device = recording.value(QStringLiteral("device")).toObject();
    QStringList lines{trail.join(QStringLiteral(" → ")),
                      QStringLiteral("%1 active · %2 clicks")
                          .arg(durationText(qint64(number(recording, "activeMs"))))
                          .arg(qint64(number(recording, "clicks")))};
    QStringList parts;
    for (const char *key : {"type", "browser", "os"}) {
        const QString part = text(device, key).trimmed();
        if (!part.isEmpty())
            parts.append(part);
    }
    if (!text(recording, "country").isEmpty())
        parts.append(text(recording, "country"));
    if (!parts.isEmpty())
        lines.append(parts.join(QStringLiteral(" · ")));
    if (number(recording, "errors") > 0)
        lines.append(QStringLiteral("%1 errors").arg(qint64(number(recording, "errors"))));
    return lines.join(QLatin1Char('\n'));
}

void AnalyticsFeature::loadRecordings(int serial)
{
    if (m_project->currentData().toString().isEmpty())
        return;
    request(QByteArrayLiteral("GET"), projectPath(QStringLiteral("/recordings")), {},
            [this, serial](bool ok, const QJsonObject &payload, const QString &error) {
        if (serial != m_serial)
            return;
        m_recordingList = ok ? payload.value(QStringLiteral("recordings")).toArray() : QJsonArray();
        renderRecordings();
        if (!ok) {
            m_recordingsNote->setText(QStringLiteral("Recordings could not load: %1").arg(error));
            m_recordingsNote->setVisible(true);
        }
    });
}

void AnalyticsFeature::renderRecordings()
{
    m_recordings->setRowCount(0);
    const bool enabled = projectRecord().value(QStringLiteral("settings")).toObject()
                             .value(QStringLiteral("replay")).toObject()
                             .value(QStringLiteral("enabled")).toBool();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QFont flagFont = m_recordings->font();
    flagFont.setFamilies({QStringLiteral("Noto Color Emoji"), QStringLiteral("Apple Color Emoji"),
                          QStringLiteral("Segoe UI Emoji")});
    for (const QJsonValue &value : std::as_const(m_recordingList)) {
        const QJsonObject recording = value.toObject();
        const QJsonArray pages = recording.value(QStringLiteral("pages")).toArray();
        const QString entry = pages.isEmpty() ? text(recording, "startUrl") : pages.first().toString();
        const QString tip = recordingTip(recording);
        const int row = m_recordings->rowCount();
        m_recordings->insertRow(row);
        const QString country = text(recording, "country");
        auto *flag = new QTableWidgetItem(MotionTrack::countryFlag(country));
        flag->setFont(flagFont);
        flag->setToolTip(country.isEmpty() ? QStringLiteral("Country unknown") : country);
        flag->setTextAlignment(Qt::AlignCenter);
        m_recordings->setItem(row, 0, flag);
        auto *page = new QTableWidgetItem(MotionTrack::pathOf(entry));
        page->setToolTip(tip);
        m_recordings->setItem(row, 1, page);
        const QStringList cells{countText(std::max<double>(1, pages.size())),
                                durationText(qint64(number(recording, "durationMs"))),
                                agoText(now, qint64(number(recording, "startedAt")))};
        for (int column = 0; column < cells.size(); ++column) {
            auto *item = new QTableWidgetItem(cells.at(column));
            item->setToolTip(column == 2 ? QLocale().toString(QDateTime::fromMSecsSinceEpoch(
                                                                  qint64(number(recording, "startedAt"))),
                                                              QLocale::ShortFormat)
                                         : tip);
            item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            m_recordings->setItem(row, column + 2, item);
        }
    }
    QString note;
    if (m_recordingList.isEmpty())
        note = enabled ? QStringLiteral("No recorded visits yet. New visits appear here within a few minutes.")
                       : QStringLiteral("Recordings are off for this project. When they are on, each visit is "
                                        "recorded so you can watch the mouse, clicks and scrolling.");
    m_recordingsNote->setText(note);
    m_recordingsNote->setVisible(!note.isEmpty());
    m_recordingsEnable->setVisible(!enabled && canManage());
    if (Theme::isNext())
        m_recordings->setVisible(!m_recordingList.isEmpty());
}

void AnalyticsFeature::openRecording(int row)
{
    if (row < 0 || row >= m_recordingList.size())
        return;
    const QJsonObject recording = m_recordingList.at(row).toObject();
    const QString session = text(recording, "sessionId");
    if (session.isEmpty())
        return;
    if (m_replayDialog && m_replayDialog->property("sessionId").toString() == session) {
        m_replayDialog->raise();
        m_replayDialog->activateWindow();
        return;
    }
    if (m_replayDialog)
        m_replayDialog->close();
    const QString path = projectPath(QStringLiteral("/recordings/") + session);
    const QPointer<AnalyticsFeature> guard(this);
    auto *dialog = new AnalyticsReplayDialog(
        recording,
        [guard, path](const QString &query, AnalyticsReplayDialog::ReplyHandler done) {
            if (!guard)
                return done(false, {}, QStringLiteral("Analytics was closed."));
            guard->request(QByteArrayLiteral("GET"), path + query, {}, std::move(done));
        },
        this);
    dialog->setProperty("sessionId", session);
    m_replayDialog = dialog;
    dialog->show();
}

void AnalyticsFeature::enableRecordings()
{
    if (!canManage())
        return;
    m_recordingsEnable->setEnabled(false);
    request(QByteArrayLiteral("PATCH"), projectPath(QString()),
            QJsonObject{{QStringLiteral("replay"), QJsonObject{{QStringLiteral("enabled"), true}}}},
            [this](bool ok, const QJsonObject &, const QString &error) {
        m_recordingsEnable->setEnabled(true);
        if (!ok)
            return setStatus(QStringLiteral("Recordings were not turned on: %1").arg(error), true);
        setStatus(QStringLiteral("Recordings are on. Sites start recording new visits within about five minutes."));
        loadOrganization();
    });
}

void AnalyticsFeature::renderOverview()
{
    const QJsonObject result = m_overview.value(QStringLiteral("result")).toObject();
    const QJsonObject totals = result.value(QStringLiteral("totals")).toObject();
    const QJsonObject previous = result.value(QStringLiteral("previous")).toObject();
    const QList<const char *> counted{"visitors", "pageviews", "sessions"};
    for (int index = 0; index < counted.size(); ++index) {
        const double current = number(totals, counted.at(index));
        const double before = number(previous, counted.at(index));
        m_kpiValues.at(index)->setText(countText(current));
        const QString change = changeText(current, before);
        if (Theme::isNext() && index < m_kpiCards.size()) {
            setNextDelta(m_kpiChanges.at(index), m_kpiCards.at(index), change);
            continue;
        }
        m_kpiChanges.at(index)->setText(change.isEmpty() ? QString() : change + QStringLiteral(" vs previous"));
        m_kpiChanges.at(index)->setStyleSheet(
            change.startsWith(QLatin1Char('-'))                       ? QStringLiteral("color: #d1453b;")
            : change.isEmpty() || change == QLatin1String("+0%") ? QString()
                                                                  : QStringLiteral("color: #1baf7a;"));
    }
    m_kpiValues.at(3)->setText(QStringLiteral("%1%").arg(std::lround(number(totals, "bounceRate") * 100)));
    m_kpiChanges.at(3)->clear();
    m_kpiValues.at(4)->setText(durationText(qint64(number(totals, "averageSessionMs"))));
    m_kpiChanges.at(4)->clear();
    for (int index = 3; index < m_kpiCards.size(); ++index)
        setNextDelta(m_kpiChanges.at(index), m_kpiCards.at(index), QString());
    m_chart->setSeries(result.value(QStringLiteral("timeseries")).toObject());
    renderChartHeading();
    const QJsonObject breakdowns = result.value(QStringLiteral("breakdowns")).toObject();
    fillTable(m_pages, breakdowns.value(QStringLiteral("pages")).toArray(), true);
    fillTable(m_referrers, breakdowns.value(QStringLiteral("referrers")).toArray(), false);
    fillTable(m_countries, breakdowns.value(QStringLiteral("countries")).toArray(), false);
    fillTable(m_devices, breakdowns.value(QStringLiteral("devices")).toArray(), false);
    fillTable(m_browsers, breakdowns.value(QStringLiteral("browsers")).toArray(), false);
    fillTable(m_events, breakdowns.value(QStringLiteral("events")).toArray(), false);
    m_truncated->setVisible(m_overview.value(QStringLiteral("truncated")).toBool());
    setStatus(QStringLiteral("%1 events in the last %2 · updated %3")
                  .arg(countText(number(totals, "events")),
                       Theme::isNext() ? rangePhrase(m_rangeHours) : m_rangeGroup->button(m_rangeHours)->text(),
                       QLocale().toString(QTime::currentTime(), QStringLiteral("h:mm AP"))));
}

void AnalyticsFeature::selectChartMetric(int metric)
{
    if (!m_chart || metric < 0 || metric > 1)
        return;
    m_chartMetric = metric;
    m_chart->setMetric(metric);
    for (int index = 0; index < m_kpiCards.size() && index < 2; ++index) {
        m_kpiCards.at(index)->setProperty("analyticsSelected", index == metric);
        repolishTree(m_kpiCards.at(index));
    }
    renderChartHeading();
}

void AnalyticsFeature::renderChartHeading()
{
    if (!m_chartTitle || !m_chartHint)
        return;
    const QString unit = m_chart->hourly() ? QStringLiteral("hour") : QStringLiteral("day");
    m_chartTitle->setText(QStringLiteral("%1 per %2")
                              .arg(m_chartMetric == 0 ? QStringLiteral("Visitors") : QStringLiteral("Pageviews"),
                                   unit));
    m_chartHint->setText(QStringLiteral("Last %1 · hover %2 for its value")
                             .arg(rangePhrase(m_rangeHours),
                                  m_chart->hourly() ? QStringLiteral("an hour") : QStringLiteral("a day")));
}

void AnalyticsFeature::setStatus(const QString &value, bool error)
{
    m_status->setText(value);
    m_status->setObjectName(error ? QStringLiteral("errorLabel") : QStringLiteral("mutedLabel"));
    m_status->style()->unpolish(m_status);
    m_status->style()->polish(m_status);
}

} // namespace forkmesh::analytics

namespace {
const forkmesh::market::NativePackageRegistration analyticsRegistration({
    QStringLiteral("builtin:analytics"),
    {QStringLiteral("extension.native")},
    QStringLiteral("Analytics"),
    QStringLiteral("pulse"),
    QStringLiteral("/api/analytics"),
    QStringLiteral("forkmesh-analytics-request-v1"),
    [](const forkmesh::market::NativePackageHost &host, QWidget *parent) {
        return new forkmesh::analytics::AnalyticsFeature(host, parent);
    },
});
} // namespace
