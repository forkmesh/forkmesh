#include "AgentToolEfficiencyBoard.h"

#include "AgentToolPolicy.h"
#include "DesignTokens.h"

#include <QDateTime>
#include <QFontMetrics>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QStringList>
#include <QToolTip>

#include <algorithm>
#include <cmath>

namespace {

constexpr int kMargin = 16;
constexpr int kGap = 10;
constexpr int kTileHeight = 78;
constexpr int kTileMinWidth = 150;
constexpr int kCardHeight = 112;
constexpr int kCardMinWidth = 224;
constexpr int kSectionHeight = 30;
constexpr int kRadius = 8;
constexpr int kToggleWidth = 34;
constexpr int kToggleHeight = 16;
constexpr int kImprovementRowHeight = 44;
constexpr int kImprovementRowGap = 6;
constexpr int kMaxImprovementRows = 5;

// The global stylesheet sizes widget fonts in pixels, which leaves
// pointSizeF() at -1: scale whichever unit the font actually carries, or a
// "+5pt" value font collapses to an unreadable 4pt.
QFont resizedFont(QFont font, qreal deltaPoints)
{
    if (font.pixelSize() > 0)
        font.setPixelSize(qMax(9, qRound(font.pixelSize() + deltaPoints * 4.0 / 3.0)));
    else if (font.pointSizeF() > 0)
        font.setPointSizeF(qMax(7.0, font.pointSizeF() + deltaPoints));
    return font;
}

QString plural(qint64 count, const char *singular, const char *pluralForm)
{
    return QStringLiteral("%1 %2")
        .arg(AgentToolEfficiencyBoard::compactNumber(count),
             QLatin1String(count == 1 ? singular : pluralForm));
}

QString latencyText(double ms)
{
    if (ms <= 0)
        return QString();
    if (ms < 1000)
        return QStringLiteral("%1 ms avg").arg(qRound(ms));
    return QStringLiteral("%1 s avg").arg(ms / 1000.0, 0, 'f', ms < 10000 ? 1 : 0);
}

QString availabilityLabel(const QString &availability)
{
    if (availability == AgentToolAvailability::Native)
        return QStringLiteral("native");
    if (availability == AgentToolAvailability::Package)
        return QStringLiteral("package");
    return QStringLiteral("planned");
}

} // namespace

AgentToolEfficiencyBoard::AgentToolEfficiencyBoard(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("agentEfficiencyBoard"));
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    rebuildModel(-1);
    relayout(width());
}

void AgentToolEfficiencyBoard::setLedger(const AgentToolEfficiencyLedger *ledger)
{
    m_ledger = ledger;
    refresh();
}

void AgentToolEfficiencyBoard::setImprovementLog(const AgentToolImprovementLog *log)
{
    // Takes effect on the next refresh(): the owner swaps the log and the
    // ledger together, and refreshing here could read a ledger it just freed.
    m_improvementLog = log;
}

void AgentToolEfficiencyBoard::setSelectedFamily(const QString &family)
{
    if (m_selectedFamily == family)
        return;
    m_selectedFamily = family;
    update();
}

QString AgentToolEfficiencyBoard::improvementRowTitle(int index) const
{
    return index >= 0 && index < m_improvementRows.size()
               ? m_improvementRows.at(index).title
               : QString();
}

QRect AgentToolEfficiencyBoard::improvementRowRect(int index) const
{
    return index >= 0 && index < m_improvementRows.size()
               ? m_improvementRows.at(index).rect
               : QRect();
}

void AgentToolEfficiencyBoard::refresh(qint64 nowMs)
{
    rebuildModel(nowMs);
    relayout(width());
    update();
}

void AgentToolEfficiencyBoard::setDisabledFamilies(const QSet<QString> &families)
{
    if (m_disabled == families)
        return;
    m_disabled = families;
    refresh();
}

bool AgentToolEfficiencyBoard::familyEnabled(const QString &family) const
{
    return !m_disabled.contains(family);
}

QRect AgentToolEfficiencyBoard::toggleRect(const QString &family) const
{
    for (const Card &card : m_cards)
        if (card.family.id == family)
            return card.toggle;
    return {};
}

QString AgentToolEfficiencyBoard::compactNumber(qint64 value)
{
    const qint64 magnitude = std::llabs(value);
    if (magnitude < 10'000)
        return QLocale::c().toString(value);
    if (magnitude < 1'000'000)
        return QStringLiteral("%1K").arg(value / 1000.0, 0, 'f',
                                          magnitude < 100'000 ? 1 : 0);
    return QStringLiteral("%1M").arg(value / 1'000'000.0, 0, 'f',
                                      magnitude < 10'000'000 ? 2 : 1);
}

QString AgentToolEfficiencyBoard::compactUsd(double value)
{
    if (value <= 0)
        return QStringLiteral("$0");
    if (value < 0.01)
        return QStringLiteral("$%1").arg(value, 0, 'f', 4);
    if (value < 100)
        return QStringLiteral("$%1").arg(value, 0, 'f', 2);
    return QStringLiteral("$%1").arg(QLocale::c().toString(qRound64(value)));
}

int AgentToolEfficiencyBoard::observedCardCount() const
{
    return int(std::count_if(m_cards.cbegin(), m_cards.cend(),
                             [](const Card &card) { return card.observed; }));
}

QRect AgentToolEfficiencyBoard::cardRect(const QString &family) const
{
    for (const Card &card : m_cards)
        if (card.family.id == family)
            return card.rect;
    return {};
}

QString AgentToolEfficiencyBoard::familyAt(const QPoint &point) const
{
    for (const Card &card : m_cards)
        if (card.rect.contains(point))
            return card.family.id;
    return {};
}

QString AgentToolEfficiencyBoard::cardHeadline(const QString &family) const
{
    for (const Card &card : m_cards)
        if (card.family.id == family)
            return card.headline;
    return {};
}

QString AgentToolEfficiencyBoard::tileValue(const QString &label) const
{
    for (const Tile &tile : m_tiles)
        if (tile.label == label)
            return tile.value;
    return {};
}

QSize AgentToolEfficiencyBoard::sizeHint() const
{
    return QSize(qMax(320, 2 * kMargin + 2 * kCardMinWidth + kGap),
                 m_contentHeight);
}

QSize AgentToolEfficiencyBoard::minimumSizeHint() const
{
    return QSize(2 * kMargin + kCardMinWidth, m_contentHeight);
}

void AgentToolEfficiencyBoard::rebuildModel(qint64 nowMs)
{
    m_totals = m_ledger ? m_ledger->totals(nowMs) : AgentToolLedgerTotals{};
    if (!m_ledger)
        m_totals.familiesTotal = agentToolFamilies().size();

    qint64 measuredCalls = 0;
    qint64 latencyTotal = 0;
    qint64 latencySamples = 0;
    m_maxConsumed = 0;
    m_cards.clear();
    for (const AgentToolFamily &family : agentToolFamilies()) {
        Card card;
        card.family = family;
        card.stats = m_ledger ? m_ledger->stats(family.id)
                              : AgentToolFamilyStats{};
        card.observed = card.stats.calls > 0;
        card.enabled = !m_disabled.contains(family.id);
        measuredCalls += card.stats.measuredCalls;
        latencyTotal += card.stats.latencyMsTotal;
        latencySamples += card.stats.latencySamples;
        m_maxConsumed = qMax(m_maxConsumed, card.stats.consumedTokens());

        if (card.stats.measuredCalls > 0) {
            card.headline =
                QStringLiteral("%1 tokens saved")
                    .arg(compactNumber(card.stats.savedTokens));
            card.headlineNote =
                QStringLiteral("≈ %1 · %2% of baseline")
                    .arg(compactUsd(card.stats.savedUsd))
                    .arg(qRound(card.stats.savingsRatio() * 100));
        } else if (card.observed) {
            card.headline =
                QStringLiteral("%1 tokens used")
                    .arg(compactNumber(card.stats.consumedTokens()));
            card.headlineNote =
                QStringLiteral("≈ %1 · no measurable baseline")
                    .arg(compactUsd(card.stats.consumedUsd));
        } else {
            card.headline = QStringLiteral("No calls yet");
            card.headlineNote =
                family.expectedSavings.isEmpty()
                    ? family.summary
                    : QStringLiteral("est. %1").arg(family.expectedSavings);
        }
        QStringList facts;
        if (card.observed) {
            facts << plural(card.stats.calls, "call", "calls");
            if (card.stats.errors > 0)
                facts << plural(card.stats.errors, "error", "errors");
            if (const QString latency =
                    latencyText(card.stats.averageLatencyMs());
                !latency.isEmpty())
                facts << latency;
            if (card.stats.inFlight > 0)
                facts << plural(card.stats.inFlight, "running", "running");
            if (card.stats.callsWhileOff > 0)
                facts << QStringLiteral("%1 while off")
                             .arg(plural(card.stats.callsWhileOff, "call", "calls"));
        } else {
            facts << family.summary;
        }
        card.facts = facts.join(QStringLiteral(" · "));
        m_cards.append(card);
    }

    // Observed families first, largest saving on top; the rest keep catalog
    // order so unobserved families stay grouped by category.
    std::stable_sort(m_cards.begin(), m_cards.end(),
                     [](const Card &a, const Card &b) {
                         if (a.observed != b.observed)
                             return a.observed;
                         if (!a.observed)
                             return false;
                         if (a.stats.savedTokens != b.stats.savedTokens)
                             return a.stats.savedTokens > b.stats.savedTokens;
                         return a.stats.calls > b.stats.calls;
                     });

    m_tiles.clear();
    Tile calls;
    calls.label = QStringLiteral("Tool calls");
    calls.value = compactNumber(m_totals.calls);
    calls.note = m_totals.inFlight > 0
                     ? plural(m_totals.inFlight, "running now", "running now")
                     : (m_totals.errors > 0
                            ? plural(m_totals.errors, "error", "errors")
                            : QStringLiteral("last 2 hours"));
    calls.trend = m_totals.recentCalls;
    m_tiles.append(calls);

    Tile saved;
    saved.label = QStringLiteral("Tokens saved");
    saved.value = compactNumber(m_totals.savedTokens);
    saved.note = measuredCalls > 0
                     ? QStringLiteral("measured on %1")
                           .arg(plural(measuredCalls, "call", "calls"))
                     : QStringLiteral("versus naive reads and rewrites");
    saved.trend = m_totals.recentSaved;
    m_tiles.append(saved);

    Tile usd;
    usd.label = QStringLiteral("Est. cost saved");
    usd.value = compactUsd(m_totals.savedUsd);
    usd.note = QStringLiteral("list-price estimate");
    m_tiles.append(usd);

    Tile consumed;
    consumed.label = QStringLiteral("Context injected");
    consumed.value = compactNumber(m_totals.consumedTokens);
    consumed.note = QStringLiteral("≈ %1 in tool traffic")
                        .arg(compactUsd(m_totals.consumedUsd));
    m_tiles.append(consumed);

    Tile latency;
    latency.label = QStringLiteral("Avg tool latency");
    const double avgMs =
        latencySamples > 0 ? double(latencyTotal) / latencySamples : 0.0;
    latency.value = avgMs <= 0 ? QStringLiteral("—")
                    : avgMs < 1000
                        ? QStringLiteral("%1 ms").arg(qRound(avgMs))
                        : QStringLiteral("%1 s").arg(avgMs / 1000.0, 0, 'f', 1);
    latency.note = QStringLiteral("tool_use to tool_result");
    m_tiles.append(latency);

    Tile coverage;
    coverage.label = QStringLiteral("Coverage");
    coverage.value = QStringLiteral("%1 / %2")
                         .arg(m_totals.familiesObserved)
                         .arg(m_totals.familiesTotal);
    coverage.note = m_disabled.isEmpty()
                        ? QStringLiteral("tool families observed")
                        : QStringLiteral("observed · %1 switched off")
                              .arg(m_disabled.size());
    m_tiles.append(coverage);

    m_improvementRows.clear();
    const QVector<AgentToolImprovement> signalList =
        m_improvementLog ? m_improvementLog->improvements()
                         : QVector<AgentToolImprovement>{};
    for (const AgentToolImprovement &signal : signalList) {
        if (m_improvementRows.size() >= kMaxImprovementRows)
            break;
        ImprovementRow row;
        row.signal = signal;
        const AgentToolFamily *family = agentToolFamily(signal.family);
        row.title = QStringLiteral("%1 · %2")
                        .arg(AgentToolImprovementLog::kindLabel(signal.kind),
                             family ? family->displayName : signal.family);
        if (!signal.topic.isEmpty())
            row.title += QStringLiteral(" · %1").arg(signal.topic);
        QStringList meta{QStringLiteral("×%1").arg(compactNumber(signal.occurrences))};
        if (signal.tokensAtStake > 0)
            meta << QStringLiteral("~%1 tok").arg(compactNumber(signal.tokensAtStake));
        if (signal.lastMs > 0)
            meta << QDateTime::fromMSecsSinceEpoch(signal.lastMs)
                        .toString(QStringLiteral("HH:mm:ss"));
        row.meta = meta.join(QStringLiteral(" · "));
        row.note = signal.detail.isEmpty()
                       ? signal.suggestion
                       : QStringLiteral("%1 → %2").arg(signal.detail, signal.suggestion);
        m_improvementRows.append(row);
    }
}

void AgentToolEfficiencyBoard::relayout(int width)
{
    const int usable = qMax(kCardMinWidth, width - 2 * kMargin);
    int y = kMargin;

    const int tileColumns =
        qBound(2, (usable + kGap) / (kTileMinWidth + kGap), m_tiles.size());
    const int tileWidth = (usable - kGap * (tileColumns - 1)) / tileColumns;
    for (int i = 0; i < m_tiles.size(); ++i) {
        const int row = i / tileColumns;
        const int column = i % tileColumns;
        m_tiles[i].rect = QRect(kMargin + column * (tileWidth + kGap),
                                y + row * (kTileHeight + kGap), tileWidth,
                                kTileHeight);
    }
    const int tileRows = (m_tiles.size() + tileColumns - 1) / tileColumns;
    y += tileRows * (kTileHeight + kGap) + kGap;

    m_sections.clear();
    {
        Section section;
        const int signalCount = m_improvementLog ? m_improvementLog->signalCount() : 0;
        section.title =
            signalCount == 0
                ? QStringLiteral("Potential improvements")
                : QStringLiteral("Potential improvements · %1 from %2")
                      .arg(plural(signalCount, "signal", "signals"),
                           plural(m_improvementLog->occurrenceCount(), "call", "calls"));
        section.rect = QRect(kMargin, y, usable, kSectionHeight);
        m_sections.append(section);
        y += kSectionHeight;
        m_improvementEmptyRect = QRect();
        if (m_improvementRows.isEmpty()) {
            m_improvementEmptyRect = QRect(kMargin, y, usable, kImprovementRowHeight);
            y += kImprovementRowHeight + kImprovementRowGap;
        }
        for (ImprovementRow &row : m_improvementRows) {
            row.rect = QRect(kMargin, y, usable, kImprovementRowHeight);
            y += kImprovementRowHeight + kImprovementRowGap;
        }
        y += kGap - kImprovementRowGap;
    }

    m_columns = qMax(1, (usable + kGap) / (kCardMinWidth + kGap));
    const int cardWidth = (usable - kGap * (m_columns - 1)) / m_columns;
    auto placeCards = [&](const QString &title, bool observed) {
        int count = 0;
        for (const Card &card : m_cards)
            count += card.observed == observed ? 1 : 0;
        if (count == 0)
            return;
        Section section;
        section.title = title;
        section.rect = QRect(kMargin, y, usable, kSectionHeight);
        m_sections.append(section);
        y += kSectionHeight;
        int index = 0;
        for (Card &card : m_cards) {
            if (card.observed != observed)
                continue;
            const int row = index / m_columns;
            const int column = index % m_columns;
            card.rect = QRect(kMargin + column * (cardWidth + kGap),
                              y + row * (kCardHeight + kGap), cardWidth,
                              kCardHeight);
            card.toggle = QRect(card.rect.right() - 12 - kToggleWidth + 1,
                                card.rect.top() + 9, kToggleWidth, kToggleHeight);
            ++index;
        }
        const int rows = (count + m_columns - 1) / m_columns;
        y += rows * (kCardHeight + kGap) + kGap;
    };
    placeCards(QStringLiteral("Observed tools"), true);
    placeCards(QStringLiteral("Not observed yet"), false);

    m_contentHeight = y + kMargin - kGap;
    m_layoutWidth = width;
    if (minimumHeight() != m_contentHeight)
        setMinimumHeight(m_contentHeight);
    updateGeometry();
}

void AgentToolEfficiencyBoard::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (event->size().width() != m_layoutWidth)
        relayout(event->size().width());
}

bool AgentToolEfficiencyBoard::darkSurface() const
{
    return palette().color(QPalette::Window).lightness() < 128;
}

QColor AgentToolEfficiencyBoard::accentColor() const
{
    // Dataviz reference palette, categorical slot 1, stepped per surface.
    return Theme::nextOr(Theme::Role::Accent,
                         QColor(darkSurface() ? QStringLiteral("#3987e5")
                                              : QStringLiteral("#2a78d6")));
}

QColor AgentToolEfficiencyBoard::dangerColor() const
{
    return QColor(darkSurface() ? QStringLiteral("#e66767")
                                : QStringLiteral("#e34948"));
}

void AgentToolEfficiencyBoard::paintSparkline(QPainter &painter,
                                              const QRect &rect,
                                              const QVector<qint64> &values) const
{
    if (values.size() < 2 || rect.width() < 12 || rect.height() < 6)
        return;
    const qint64 maximum =
        qMax<qint64>(1, *std::max_element(values.cbegin(), values.cend()));
    QVector<QPointF> points;
    points.reserve(values.size());
    for (int i = 0; i < values.size(); ++i) {
        const qreal x = rect.left() + rect.width() * i / qreal(values.size() - 1);
        const qreal y = rect.bottom() - rect.height() * values.at(i) / qreal(maximum);
        points.append(QPointF(x, y));
    }
    QColor muted = palette().color(QPalette::PlaceholderText);
    muted.setAlpha(170);
    QPainterPath path(points.first());
    for (int i = 1; i < points.size(); ++i)
        path.lineTo(points.at(i));
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(muted, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(path);
    // Current period in the accent so the newest sample reads instantly.
    painter.setPen(Qt::NoPen);
    painter.setBrush(accentColor());
    painter.drawEllipse(points.last(), 3.0, 3.0);
    painter.restore();
}

void AgentToolEfficiencyBoard::paintTile(QPainter &painter, const Tile &tile) const
{
    const QPalette colors = palette();
    QColor border = colors.color(QPalette::Mid);
    border.setAlpha(140);
    painter.setPen(QPen(border, 1));
    painter.setBrush(colors.color(QPalette::Base));
    painter.drawRoundedRect(QRectF(tile.rect).adjusted(0.5, 0.5, -0.5, -0.5),
                            kRadius, kRadius);

    const QRect inner = tile.rect.adjusted(12, 8, -12, -8);
    const QFont labelFont = resizedFont(font(), -1.0);
    painter.setFont(labelFont);
    painter.setPen(colors.color(QPalette::PlaceholderText));
    painter.drawText(inner, Qt::AlignLeft | Qt::AlignTop, tile.label);

    const bool hasTrend = tile.trend.size() >= 2 &&
                          std::any_of(tile.trend.cbegin(), tile.trend.cend(),
                                      [](qint64 v) { return v > 0; });
    const int trendWidth = hasTrend ? qMin(72, inner.width() / 3) : 0;
    QFont valueFont = resizedFont(font(), 5.0);
    valueFont.setWeight(QFont::DemiBold);
    painter.setFont(valueFont);
    painter.setPen(colors.color(QPalette::Text));
    const QRect valueRect(inner.left(), inner.top() + 16,
                          inner.width() - trendWidth - (hasTrend ? 8 : 0), 28);
    painter.drawText(valueRect, Qt::AlignLeft | Qt::AlignVCenter,
                     QFontMetrics(valueFont).elidedText(
                         tile.value, Qt::ElideRight, valueRect.width()));

    painter.setFont(labelFont);
    painter.setPen(colors.color(QPalette::PlaceholderText));
    const QRect noteRect(inner.left(), inner.bottom() - 14,
                         inner.width() - trendWidth - (hasTrend ? 8 : 0), 16);
    painter.drawText(noteRect, Qt::AlignLeft | Qt::AlignVCenter,
                     QFontMetrics(labelFont).elidedText(
                         tile.note, Qt::ElideRight, noteRect.width()));

    if (hasTrend)
        paintSparkline(painter,
                       QRect(inner.right() - trendWidth + 1, inner.top() + 18,
                             trendWidth - 4, inner.height() - 20),
                       tile.trend);
}

void AgentToolEfficiencyBoard::paintCard(QPainter &painter, const Card &card,
                                         bool hovered) const
{
    const QPalette colors = palette();
    const QColor text = colors.color(QPalette::Text);
    const QColor muted = colors.color(QPalette::PlaceholderText);
    const bool selected = card.family.id == m_selectedFamily;
    QColor border = hovered || selected ? accentColor() : colors.color(QPalette::Mid);
    border.setAlpha(selected ? 255 : hovered ? 220 : 140);
    painter.setPen(QPen(border, selected ? 2 : 1));
    painter.setBrush(colors.color(QPalette::Base));
    painter.drawRoundedRect(QRectF(card.rect).adjusted(0.5, 0.5, -0.5, -0.5),
                            kRadius, kRadius);

    const QRect inner = card.rect.adjusted(12, 9, -12, -9);
    const QFont smallFont = resizedFont(font(), -1.5);
    QFont titleFont = font();
    titleFont.setWeight(QFont::DemiBold);
    const QFontMetrics smallMetrics(smallFont);

    // Title row: name left, category + availability chip, then the on/off
    // toggle at the far right. The toggle carries its state as text too.
    const QRect toggle = card.toggle;
    {
        QColor track = card.enabled ? accentColor() : colors.color(QPalette::Mid);
        track.setAlpha(card.enabled ? 230 : 150);
        painter.setPen(Qt::NoPen);
        painter.setBrush(track);
        painter.drawRoundedRect(toggle, kToggleHeight / 2.0, kToggleHeight / 2.0);
        const int knob = kToggleHeight - 4;
        const QRect knobRect(card.enabled ? toggle.right() - knob - 1
                                          : toggle.left() + 2,
                             toggle.top() + 2, knob, knob);
        painter.setBrush(colors.color(QPalette::Base));
        painter.drawEllipse(knobRect);
        const QFont toggleFont = resizedFont(smallFont, -1.0);
        painter.setFont(toggleFont);
        painter.setPen(card.enabled ? colors.color(QPalette::Base) : muted);
        const QRect labelRect = card.enabled
                                    ? QRect(toggle.left() + 4, toggle.top(),
                                            toggle.width() - knob - 6, toggle.height())
                                    : QRect(toggle.left() + knob + 4, toggle.top(),
                                            toggle.width() - knob - 6, toggle.height());
        painter.drawText(labelRect, Qt::AlignCenter,
                         card.enabled ? QStringLiteral("on") : QStringLiteral("off"));
    }
    // The family name wins over the chip: a narrow card first drops the
    // availability, then the whole chip, before the name is elided.
    const int titleAdvance =
        QFontMetrics(titleFont).horizontalAdvance(card.family.displayName);
    const QStringList chipChoices{
        QStringLiteral("%1 · %2").arg(card.family.category.toLower(),
                                      availabilityLabel(card.family.availability)),
        card.family.category.toLower(), QString()};
    QString chip;
    QRect chipRect(toggle.left() - 6, inner.top(), 0, 16);
    for (const QString &choice : chipChoices) {
        const int chipWidth =
            choice.isEmpty() ? 0 : smallMetrics.horizontalAdvance(choice) + 12;
        chip = choice;
        chipRect = QRect(toggle.left() - 6 - chipWidth, inner.top(), chipWidth, 16);
        if (chipRect.left() - 8 - inner.left() >= titleAdvance)
            break;
    }
    if (!chip.isEmpty()) {
        QColor chipFill = colors.color(QPalette::Mid);
        chipFill.setAlpha(card.observed ? 90 : 50);
        painter.setPen(Qt::NoPen);
        painter.setBrush(chipFill);
        painter.drawRoundedRect(chipRect, 8, 8);
        painter.setFont(smallFont);
        painter.setPen(muted);
        painter.drawText(chipRect, Qt::AlignCenter, chip);
    }

    painter.setFont(titleFont);
    painter.setPen(card.observed && card.enabled ? text : muted);
    const QRect titleRect(inner.left(), inner.top() - 1,
                          chipRect.left() - 8 - inner.left(), 18);
    painter.drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter,
                     QFontMetrics(titleFont).elidedText(
                         card.family.displayName, Qt::ElideRight,
                         titleRect.width()));

    // Headline row with the sparkline of recent per-call savings at right.
    const bool hasTrend = card.stats.recentSaved.size() >= 2 &&
                          std::any_of(card.stats.recentSaved.cbegin(),
                                      card.stats.recentSaved.cend(),
                                      [](qint64 v) { return v > 0; });
    const int trendWidth = hasTrend ? qMin(64, inner.width() / 4) : 0;
    QFont headlineFont = resizedFont(font(), 1.5);
    headlineFont.setWeight(QFont::DemiBold);
    painter.setFont(headlineFont);
    painter.setPen(card.observed && card.enabled ? text : muted);
    const QRect headlineRect(inner.left(), inner.top() + 22,
                             inner.width() - trendWidth - (hasTrend ? 8 : 0), 22);
    painter.drawText(headlineRect, Qt::AlignLeft | Qt::AlignVCenter,
                     QFontMetrics(headlineFont).elidedText(
                         card.headline, Qt::ElideRight, headlineRect.width()));
    painter.setFont(smallFont);
    painter.setPen(muted);
    const QRect noteRect(inner.left(), inner.top() + 44, headlineRect.width(), 14);
    painter.drawText(noteRect, Qt::AlignLeft | Qt::AlignVCenter,
                     smallMetrics.elidedText(card.headlineNote, Qt::ElideRight,
                                             noteRect.width()));
    if (hasTrend)
        paintSparkline(painter,
                       QRect(inner.right() - trendWidth + 1, inner.top() + 22,
                             trendWidth - 4, 32),
                       card.stats.recentSaved);

    // Facts row. Errors are also spelled out in text, never colour alone.
    const QRect factsRect(inner.left(), inner.bottom() - 26, inner.width(), 14);
    painter.setPen(card.stats.errors > 0 && card.observed ? dangerColor()
                                                           : muted);
    painter.drawText(factsRect, Qt::AlignLeft | Qt::AlignVCenter,
                     smallMetrics.elidedText(card.facts, Qt::ElideRight,
                                             factsRect.width()));

    // Actual-versus-baseline bar: consumed (de-emphasis) + saved (accent)
    // add up to the naive baseline. Unmeasured families show their share of
    // the busiest family's context traffic in the de-emphasis hue instead.
    const QRect barRect(inner.left(), inner.bottom() - 6, inner.width(), 6);
    QColor track = colors.color(QPalette::Mid);
    track.setAlpha(70);
    painter.setPen(Qt::NoPen);
    painter.setBrush(track);
    painter.drawRoundedRect(barRect, 3, 3);
    if (!card.observed)
        return;
    QColor deEmphasis = muted;
    deEmphasis.setAlpha(190);
    if (card.stats.measuredCalls > 0 && card.stats.baselineTokens > 0) {
        const qint64 actual = card.stats.baselineTokens - card.stats.savedTokens;
        const qreal actualShare = qBound(0.0, actual / qreal(card.stats.baselineTokens), 1.0);
        const int actualWidth = qRound(barRect.width() * actualShare);
        const int gap = actualWidth > 0 && actualWidth < barRect.width() ? 2 : 0;
        if (actualWidth > 0) {
            painter.setBrush(deEmphasis);
            painter.drawRoundedRect(QRect(barRect.left(), barRect.top(),
                                          actualWidth, barRect.height()),
                                    3, 3);
        }
        if (actualWidth + gap < barRect.width()) {
            painter.setBrush(accentColor());
            painter.drawRoundedRect(QRect(barRect.left() + actualWidth + gap,
                                          barRect.top(),
                                          barRect.width() - actualWidth - gap,
                                          barRect.height()),
                                    3, 3);
        }
    } else if (m_maxConsumed > 0) {
        const qreal share = qBound(
            0.0, card.stats.consumedTokens() / qreal(m_maxConsumed), 1.0);
        const int width = qMax(4, qRound(barRect.width() * share));
        painter.setBrush(deEmphasis);
        painter.drawRoundedRect(QRect(barRect.left(), barRect.top(), width,
                                      barRect.height()),
                                3, 3);
    }
}

void AgentToolEfficiencyBoard::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    for (const Tile &tile : m_tiles)
        paintTile(painter, tile);

    QFont sectionFont = font();
    sectionFont.setWeight(QFont::DemiBold);
    for (const Section &section : m_sections) {
        painter.setFont(sectionFont);
        painter.setPen(palette().color(QPalette::Text));
        painter.drawText(section.rect, Qt::AlignLeft | Qt::AlignVCenter,
                         section.title);
    }
    for (const ImprovementRow &row : m_improvementRows)
        paintImprovementRow(painter, row);
    if (!m_improvementEmptyRect.isEmpty()) {
        painter.setFont(resizedFont(font(), -1.0));
        painter.setPen(palette().color(QPalette::PlaceholderText));
        painter.drawText(
            m_improvementEmptyRect.adjusted(12, 0, -12, 0),
            Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap,
            QStringLiteral("No signals yet. Whole-file reads, oversized output, "
                           "repeated calls, tool errors and policy gaps are logged "
                           "here the moment they happen."));
    }
    for (int i = 0; i < m_cards.size(); ++i)
        paintCard(painter, m_cards.at(i), i == m_hoverCard);
}

void AgentToolEfficiencyBoard::paintImprovementRow(QPainter &painter,
                                                   const ImprovementRow &row) const
{
    const QPalette colors = palette();
    QColor border = colors.color(QPalette::Mid);
    border.setAlpha(140);
    painter.setPen(QPen(border, 1));
    painter.setBrush(colors.color(QPalette::Base));
    painter.drawRoundedRect(QRectF(row.rect).adjusted(0.5, 0.5, -0.5, -0.5),
                            kRadius, kRadius);
    // Failures take the danger hue, waste the accent; the kind is always
    // spelled out in the title so colour is never the only cue.
    painter.setPen(Qt::NoPen);
    painter.setBrush(AgentToolImprovementLog::kindIsFailure(row.signal.kind)
                         ? dangerColor()
                         : accentColor());
    painter.drawRoundedRect(QRect(row.rect.left() + 6, row.rect.top() + 9, 3,
                                  row.rect.height() - 18),
                            1.5, 1.5);

    const QRect inner = row.rect.adjusted(16, 6, -12, -6);
    const QFont smallFont = resizedFont(font(), -1.5);
    QFont titleFont = font();
    titleFont.setWeight(QFont::DemiBold);
    const QFontMetrics smallMetrics(smallFont);
    const int metaWidth = qMin(inner.width() / 2,
                               smallMetrics.horizontalAdvance(row.meta) + 4);
    painter.setFont(smallFont);
    painter.setPen(colors.color(QPalette::PlaceholderText));
    painter.drawText(QRect(inner.right() - metaWidth + 1, inner.top(), metaWidth, 16),
                     Qt::AlignRight | Qt::AlignVCenter,
                     smallMetrics.elidedText(row.meta, Qt::ElideLeft, metaWidth));
    painter.setFont(titleFont);
    painter.setPen(colors.color(QPalette::Text));
    const QRect titleRect(inner.left(), inner.top(), inner.width() - metaWidth - 8, 16);
    painter.drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter,
                     QFontMetrics(titleFont).elidedText(row.title, Qt::ElideRight,
                                                        titleRect.width()));
    painter.setFont(smallFont);
    painter.setPen(colors.color(QPalette::PlaceholderText));
    const QRect noteRect(inner.left(), inner.bottom() - 14, inner.width(), 15);
    painter.drawText(noteRect, Qt::AlignLeft | Qt::AlignVCenter,
                     smallMetrics.elidedText(row.note, Qt::ElideRight,
                                             noteRect.width()));
}

QString AgentToolEfficiencyBoard::improvementTooltip(
    const AgentToolImprovement &signal) const
{
    const AgentToolFamily *family = agentToolFamily(signal.family);
    QStringList lines;
    lines << QStringLiteral("<b>%1</b> · %2")
                 .arg(AgentToolImprovementLog::kindLabel(signal.kind).toHtmlEscaped(),
                      (family ? family->displayName : signal.family).toHtmlEscaped());
    if (!signal.runtimeName.isEmpty())
        lines << QStringLiteral("Runtime tool: %1").arg(signal.runtimeName.toHtmlEscaped());
    if (!signal.topic.isEmpty())
        lines << QStringLiteral("Grouped as: %1").arg(signal.topic.toHtmlEscaped());
    lines << QStringLiteral("Seen %1 · %2 tokens at stake")
                 .arg(plural(signal.occurrences, "time", "times"),
                      compactNumber(signal.tokensAtStake));
    if (!signal.detail.isEmpty())
        lines << QStringLiteral("Latest: %1").arg(signal.detail.toHtmlEscaped());
    lines << QStringLiteral("Suggestion: %1").arg(signal.suggestion.toHtmlEscaped());
    const auto stamp = [](qint64 ms) {
        return QDateTime::fromMSecsSinceEpoch(ms).toString(
            QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    };
    if (signal.firstMs > 0)
        lines << QStringLiteral("First %1 · last %2")
                     .arg(stamp(signal.firstMs), stamp(signal.lastMs));
    return lines.join(QStringLiteral("<br>"));
}

QString AgentToolEfficiencyBoard::familyDetailsHtml(const QString &family) const
{
    for (const Card &card : m_cards) {
        if (card.family.id != family)
            continue;
        QString html = cardTooltip(card);
        html += QStringLiteral("<h4>Potential improvements</h4>");
        QStringList items;
        if (m_improvementLog) {
            for (const AgentToolImprovement &signal : m_improvementLog->improvements())
                if (signal.family == family)
                    items << QStringLiteral("<p>%1</p>").arg(improvementTooltip(signal));
        }
        html += items.isEmpty()
                    ? QStringLiteral("<p><i>No improvement signals for this family "
                                     "yet.</i></p>")
                    : items.join(QString());
        return html;
    }
    return QString();
}

QString AgentToolEfficiencyBoard::cardTooltip(const Card &card) const
{
    const AgentToolFamilyStats &s = card.stats;
    QStringList lines;
    lines << QStringLiteral("<b>%1</b> — %2")
                 .arg(card.family.displayName.toHtmlEscaped(),
                      card.family.summary.toHtmlEscaped());
    if (!card.family.availabilityNote.isEmpty())
        lines << QStringLiteral("Runs via: %1")
                     .arg(card.family.availabilityNote.toHtmlEscaped());
    lines << (card.enabled
                  ? QStringLiteral("<b>On.</b> Click the toggle to switch this "
                                   "family off when it performs worse.")
                  : QStringLiteral("<b>Off.</b> %1")
                        .arg(AgentToolPolicy::enforcementNote(card.family.id)
                                 .toHtmlEscaped()));
    if (!card.observed) {
        if (!card.family.expectedSavings.isEmpty())
            lines << QStringLiteral("Planning estimate: %1")
                         .arg(card.family.expectedSavings.toHtmlEscaped());
        return QStringLiteral("<p>%1</p>").arg(lines.join(QStringLiteral("<br>")));
    }
    lines << QStringLiteral("Calls: %1 · errors: %2 · running: %3")
                 .arg(s.calls)
                 .arg(s.errors)
                 .arg(s.inFlight);
    lines << QStringLiteral("Arguments: %1 tok · results: %2 tok · ≈ %3")
                 .arg(compactNumber(s.inputTokens), compactNumber(s.resultTokens),
                      compactUsd(s.consumedUsd));
    if (s.measuredCalls > 0) {
        QString method;
        if (card.family.baseline == AgentToolBaseline::PartialRead)
            method = QStringLiteral("whole file versus bytes returned");
        else if (card.family.baseline == AgentToolBaseline::AnchoredEdit)
            method = QStringLiteral("whole-file rewrite versus anchored patch");
        else if (card.family.baseline == AgentToolBaseline::MatchedFiles)
            method = QStringLiteral("reading every matched file versus matches");
        lines << QStringLiteral("Baseline (%1): %2 tok on %3 measured calls")
                     .arg(method, compactNumber(s.baselineTokens))
                     .arg(s.measuredCalls);
        lines << QStringLiteral("Saved: %1 tok ≈ %2")
                     .arg(compactNumber(s.savedTokens), compactUsd(s.savedUsd));
    } else {
        lines << QStringLiteral("No measurable baseline for this family; "
                                "consumption only.");
    }
    if (s.latencySamples > 0)
        lines << QStringLiteral("Latency: %1 ms avg · %2 ms max")
                     .arg(qRound(s.averageLatencyMs()))
                     .arg(s.latencyMsMax);
    QStringList names;
    for (auto it = s.runtimeNames.cbegin(); it != s.runtimeNames.cend(); ++it)
        names << QStringLiteral("%1 ×%2").arg(it.key().toHtmlEscaped()).arg(it.value());
    std::sort(names.begin(), names.end());
    if (!names.isEmpty())
        lines << QStringLiteral("Runtime tools: %1").arg(names.join(QStringLiteral(", ")));
    if (s.lastCallMs > 0)
        lines << QStringLiteral("Last call: %1")
                     .arg(QDateTime::fromMSecsSinceEpoch(s.lastCallMs)
                              .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
    lines << QStringLiteral("<i>Token counts are 4-bytes-per-token estimates; "
                            "USD uses list prices.</i>");
    return QStringLiteral("<p>%1</p>").arg(lines.join(QStringLiteral("<br>")));
}

void AgentToolEfficiencyBoard::mouseMoveEvent(QMouseEvent *event)
{
    int hover = -1;
    for (int i = 0; i < m_cards.size(); ++i) {
        if (m_cards.at(i).rect.contains(event->pos())) {
            hover = i;
            break;
        }
    }
    int hoverRow = -1;
    for (int i = 0; hover < 0 && i < m_improvementRows.size(); ++i) {
        if (m_improvementRows.at(i).rect.contains(event->pos())) {
            hoverRow = i;
            break;
        }
    }
    if (hover != m_hoverCard || hoverRow != m_hoverImprovement) {
        const bool cardChanged = hover != m_hoverCard;
        m_hoverCard = hover;
        m_hoverImprovement = hoverRow;
        if (cardChanged)
            update();
        if (hover >= 0)
            QToolTip::showText(event->globalPosition().toPoint(),
                               cardTooltip(m_cards.at(hover)), this,
                               m_cards.at(hover).rect);
        else if (hoverRow >= 0)
            QToolTip::showText(
                event->globalPosition().toPoint(),
                QStringLiteral("<p>%1</p>")
                    .arg(improvementTooltip(m_improvementRows.at(hoverRow).signal)),
                this, m_improvementRows.at(hoverRow).rect);
        else
            QToolTip::hideText();
    }
    QWidget::mouseMoveEvent(event);
}

void AgentToolEfficiencyBoard::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        for (const Card &card : m_cards) {
            if (card.toggle.contains(event->pos())) {
                if (familyToggled)
                    familyToggled(card.family.id, !card.enabled);
                event->accept();
                return;
            }
            if (!card.rect.contains(event->pos()))
                continue;
            const QString family = card.family.id;
            setSelectedFamily(family);
            if (familyActivated)
                familyActivated(family);
            event->accept();
            return;
        }
    }
    QWidget::mousePressEvent(event);
}

void AgentToolEfficiencyBoard::leaveEvent(QEvent *event)
{
    m_hoverImprovement = -1;
    if (m_hoverCard >= 0) {
        m_hoverCard = -1;
        update();
    }
    QWidget::leaveEvent(event);
}
