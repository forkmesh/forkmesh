#include "AgentGuardrailBoard.h"

#include "AgentGuardrails.h"
#include "DesignTokens.h"

#include <QDateTime>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QResizeEvent>
#include <QToolTip>

#include <algorithm>

namespace {

constexpr int kMargin = 16;
constexpr int kGap = 10;
constexpr int kTileHeight = 78;
constexpr int kTileMinWidth = 150;
constexpr int kCardHeight = 104;
constexpr int kCardMinWidth = 240;
constexpr int kSectionHeight = 30;
constexpr int kRadius = 8;
constexpr int kPillHeight = 16;

QString counted(qint64 value, const char *singular, const char *pluralForm)
{
    return QStringLiteral("%1 %2").arg(QString::number(value),
                                       QLatin1String(value == 1 ? singular
                                                                : pluralForm));
}

QString whenText(qint64 ms, qint64 nowMs)
{
    if (ms <= 0)
        return QString();
    const QDateTime when = QDateTime::fromMSecsSinceEpoch(ms);
    return when.date() == QDateTime::fromMSecsSinceEpoch(nowMs).date()
               ? when.toString(QStringLiteral("HH:mm"))
               : when.toString(QStringLiteral("MMM d"));
}

} // namespace

AgentGuardrailBoard::AgentGuardrailBoard(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("agentGuardrailBoard"));
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    rebuildModel(QDateTime::currentMSecsSinceEpoch());
}

void AgentGuardrailBoard::setLedger(const AgentGuardrailLedger *ledger)
{
    m_ledger = ledger;
    refresh();
}

void AgentGuardrailBoard::setRepositories(
    const QVector<AgentGuardrailRepositoryPolicy> &policies)
{
    m_policies = policies;
}

void AgentGuardrailBoard::refresh(qint64 nowMs)
{
    rebuildModel(nowMs >= 0 ? nowMs : QDateTime::currentMSecsSinceEpoch());
    relayout(width() > 0 ? width() : sizeHint().width());
    update();
}

void AgentGuardrailBoard::rebuildModel(qint64 nowMs)
{
    m_totals = m_ledger ? m_ledger->totals(nowMs) : AgentGuardrailTotals{};
    m_cards.clear();
    int guarded = 0;
    int attention = 0;
    int allowedCount = 0;
    int wholeRepository = 0;
    for (const AgentGuardrailRepositoryPolicy &policy : std::as_const(m_policies)) {
        Card card;
        card.policy = policy;
        card.stats = m_ledger ? m_ledger->stats(policy.repository)
                              : AgentGuardrailRepoStats{};
        const AgentGuardrailRepoStats &stats = card.stats;
        if (policy.enabled) {
            ++guarded;
            allowedCount += policy.allowedDirectories.size();
            if (policy.allowedDirectories.contains(QStringLiteral(".")))
                ++wholeRepository;
            card.attention = policy.allowedDirectories.isEmpty() ||
                             !policy.missingDirectories.isEmpty();
            attention += card.attention ? 1 : 0;
            card.headline = stats.blocked > 0
                                ? counted(stats.blocked, "blocked call",
                                          "blocked calls")
                                : QStringLiteral("No blocked calls");
            if (policy.allowedDirectories.isEmpty())
                card.note = QStringLiteral("Nothing allowed: agents are refused");
            else if (!policy.missingDirectories.isEmpty())
                card.note = QStringLiteral("Missing %1: agents will not start")
                                .arg(policy.missingDirectories.join(
                                    QStringLiteral(", ")));
            else
                card.note = QStringLiteral("Allows %1").arg(
                    forkmesh::agentguard::policyDescription(
                        policy.allowedDirectories));
        } else {
            card.headline = QStringLiteral("Guardrails off");
            card.note = stats.blocked > 0
                            ? QStringLiteral("Agents keep their provider "
                                             "sandbox · %1 while on")
                                  .arg(counted(stats.blocked, "blocked call",
                                               "blocked calls"))
                            : QStringLiteral("Agents keep their provider sandbox");
        }
        QStringList facts;
        if (stats.approved > 0 || stats.denied > 0)
            facts << counted(stats.approved, "approved", "approved")
                  << counted(stats.denied, "denied", "denied");
        if (stats.agentsStopped > 0)
            facts << counted(stats.agentsStopped, "agent stopped",
                             "agents stopped");
        if (stats.lastEventMs > 0)
            facts << QStringLiteral("last %1").arg(
                whenText(stats.lastEventMs, nowMs));
        if (facts.isEmpty())
            facts << (policy.enabled
                          ? QStringLiteral("No approvals or denials yet")
                          : QStringLiteral("Click to turn guardrails on"));
        card.facts = facts.join(QStringLiteral(" · "));
        m_cards.append(card);
    }

    // Problems first, then the busiest boundary, then by name.
    std::stable_sort(m_cards.begin(), m_cards.end(),
                     [](const Card &a, const Card &b) {
                         if (a.attention != b.attention)
                             return a.attention;
                         if (a.stats.blocked != b.stats.blocked)
                             return a.stats.blocked > b.stats.blocked;
                         return a.policy.repository.compare(
                                    b.policy.repository, Qt::CaseInsensitive) < 0;
                     });

    const int total = m_policies.size();
    m_tiles.clear();
    Tile repositories;
    repositories.label = QStringLiteral("Guarded repositories");
    repositories.value = QStringLiteral("%1 / %2").arg(guarded).arg(total);
    repositories.attention = attention > 0;
    repositories.note =
        attention > 0 ? counted(attention, "needs attention", "need attention")
        : total == 0  ? QStringLiteral("no local repositories")
        : guarded < total
            ? counted(total - guarded, "unguarded", "unguarded")
            : QStringLiteral("every repository guarded");
    m_tiles.append(repositories);

    Tile blocked;
    blocked.label = QStringLiteral("Blocked tool calls");
    blocked.value = QString::number(m_totals.blocked);
    blocked.note = m_totals.blockedOutside > 0
                       ? QStringLiteral("%1 outside the repository")
                             .arg(m_totals.blockedOutside)
                       : QStringLiteral("stopped at a boundary");
    blocked.trend = m_totals.recentBlocked;
    m_tiles.append(blocked);

    Tile approved;
    approved.label = QStringLiteral("Approved");
    approved.value = QString::number(m_totals.approved);
    approved.note = counted(m_totals.denied, "denied", "denied");
    m_tiles.append(approved);

    Tile stopped;
    stopped.label = QStringLiteral("Agents stopped");
    stopped.value = QString::number(m_totals.agentsStopped);
    stopped.note = QStringLiteral("when a boundary tightened");
    m_tiles.append(stopped);

    Tile allowed;
    allowed.label = QStringLiteral("Allowed directories");
    allowed.value = QString::number(allowedCount);
    allowed.note = wholeRepository > 0
                       ? counted(wholeRepository, "whole repository",
                                 "whole repositories")
                       : QStringLiteral("across guarded repositories");
    m_tiles.append(allowed);

    Tile last;
    last.label = QStringLiteral("Last event");
    last.value = m_totals.lastEventMs > 0 ? whenText(m_totals.lastEventMs, nowMs)
                                          : QStringLiteral("—");
    last.note = m_totals.lastEventMs > 0
                    ? QDateTime::fromMSecsSinceEpoch(m_totals.lastEventMs)
                          .toString(QStringLiteral("yyyy-MM-dd"))
                    : QStringLiteral("nothing blocked yet");
    m_tiles.append(last);
}

void AgentGuardrailBoard::relayout(int width)
{
    const int usable = qMax(kCardMinWidth, width - 2 * kMargin);
    int y = kMargin;

    const int tileColumns =
        qBound(2, (usable + kGap) / (kTileMinWidth + kGap), int(m_tiles.size()));
    const int tileWidth = (usable - kGap * (tileColumns - 1)) / tileColumns;
    for (int i = 0; i < m_tiles.size(); ++i) {
        m_tiles[i].rect = QRect(kMargin + (i % tileColumns) * (tileWidth + kGap),
                                y + (i / tileColumns) * (kTileHeight + kGap),
                                tileWidth, kTileHeight);
    }
    const int tileRows = (m_tiles.size() + tileColumns - 1) / tileColumns;
    y += tileRows * (kTileHeight + kGap) + kGap;

    m_columns = qMax(1, (usable + kGap) / (kCardMinWidth + kGap));
    const int cardWidth = (usable - kGap * (m_columns - 1)) / m_columns;
    m_sections.clear();
    auto placeCards = [&](const QString &title, bool enabled) {
        int count = 0;
        for (const Card &card : std::as_const(m_cards))
            count += card.policy.enabled == enabled ? 1 : 0;
        if (count == 0)
            return;
        m_sections.append(Section{title, QRect(kMargin, y, usable, kSectionHeight)});
        y += kSectionHeight;
        int index = 0;
        for (Card &card : m_cards) {
            if (card.policy.enabled != enabled)
                continue;
            card.rect = QRect(kMargin + (index % m_columns) * (cardWidth + kGap),
                              y + (index / m_columns) * (kCardHeight + kGap),
                              cardWidth, kCardHeight);
            ++index;
        }
        y += ((count + m_columns - 1) / m_columns) * (kCardHeight + kGap) + kGap;
    };
    placeCards(QStringLiteral("Guarded"), true);
    placeCards(QStringLiteral("Not guarded"), false);

    m_contentHeight = y + kMargin - kGap;
    m_layoutWidth = width;
    if (minimumHeight() != m_contentHeight)
        setMinimumHeight(m_contentHeight);
    updateGeometry();
}

QSize AgentGuardrailBoard::sizeHint() const
{
    return QSize(760, qMax(240, m_contentHeight));
}

QSize AgentGuardrailBoard::minimumSizeHint() const
{
    return QSize(kCardMinWidth + 2 * kMargin, qMax(160, m_contentHeight));
}

const AgentGuardrailBoard::Card *
AgentGuardrailBoard::cardFor(const QString &repository) const
{
    for (const Card &card : m_cards)
        if (card.policy.repository == repository)
            return &card;
    return nullptr;
}

const AgentGuardrailBoard::Tile *
AgentGuardrailBoard::tileFor(const QString &label) const
{
    for (const Tile &tile : m_tiles)
        if (tile.label == label)
            return &tile;
    return nullptr;
}

QRect AgentGuardrailBoard::cardRect(const QString &repository) const
{
    const Card *card = cardFor(repository);
    return card ? card->rect : QRect();
}

QString AgentGuardrailBoard::repositoryAt(const QPoint &point) const
{
    for (const Card &card : m_cards)
        if (card.rect.contains(point))
            return card.policy.repository;
    return QString();
}

QString AgentGuardrailBoard::cardHeadline(const QString &repository) const
{
    const Card *card = cardFor(repository);
    return card ? card->headline : QString();
}

QString AgentGuardrailBoard::cardNote(const QString &repository) const
{
    const Card *card = cardFor(repository);
    return card ? card->note : QString();
}

QString AgentGuardrailBoard::tileValue(const QString &label) const
{
    const Tile *tile = tileFor(label);
    return tile ? tile->value : QString();
}

QString AgentGuardrailBoard::tileNote(const QString &label) const
{
    const Tile *tile = tileFor(label);
    return tile ? tile->note : QString();
}

void AgentGuardrailBoard::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (event->size().width() != m_layoutWidth)
        relayout(event->size().width());
}

bool AgentGuardrailBoard::darkSurface() const
{
    return palette().color(QPalette::Window).lightness() < 128;
}

QColor AgentGuardrailBoard::accentColor() const
{
    return Theme::nextOr(Theme::Role::Accent,
                         QColor(darkSurface() ? QStringLiteral("#3987e5")
                                              : QStringLiteral("#2a78d6")));
}

QColor AgentGuardrailBoard::dangerColor() const
{
    return QColor(darkSurface() ? QStringLiteral("#e66767")
                                : QStringLiteral("#e34948"));
}

void AgentGuardrailBoard::paintSparkline(QPainter &painter, const QRect &rect,
                                         const QVector<qint64> &values) const
{
    if (values.size() < 2 || rect.width() < 12 || rect.height() < 6)
        return;
    const qint64 maximum =
        qMax<qint64>(1, *std::max_element(values.cbegin(), values.cend()));
    QPainterPath path;
    QPointF newest;
    for (int i = 0; i < values.size(); ++i) {
        const QPointF point(
            rect.left() + rect.width() * i / qreal(values.size() - 1),
            rect.bottom() - rect.height() * values.at(i) / qreal(maximum));
        if (i == 0)
            path.moveTo(point);
        else
            path.lineTo(point);
        newest = point;
    }
    QColor muted = palette().color(QPalette::PlaceholderText);
    muted.setAlpha(170);
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(muted, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(path);
    painter.setPen(Qt::NoPen);
    painter.setBrush(accentColor());
    painter.drawEllipse(newest, 3.0, 3.0);
    painter.restore();
}

void AgentGuardrailBoard::paintTile(QPainter &painter, const Tile &tile) const
{
    const QPalette colors = palette();
    QColor border = colors.color(QPalette::Mid);
    border.setAlpha(140);
    painter.setPen(QPen(border, 1));
    painter.setBrush(colors.color(QPalette::Base));
    painter.drawRoundedRect(QRectF(tile.rect).adjusted(0.5, 0.5, -0.5, -0.5),
                            kRadius, kRadius);

    const QRect inner = tile.rect.adjusted(12, 8, -12, -8);
    QFont labelFont = font();
    labelFont.setPointSizeF(qMax(8.0, labelFont.pointSizeF() - 1.0));
    painter.setFont(labelFont);
    painter.setPen(colors.color(QPalette::PlaceholderText));
    painter.drawText(inner, Qt::AlignLeft | Qt::AlignTop, tile.label);

    const bool hasTrend = tile.trend.size() >= 2 &&
                          std::any_of(tile.trend.cbegin(), tile.trend.cend(),
                                      [](qint64 v) { return v > 0; });
    const int trendWidth = hasTrend ? qMin(72, inner.width() / 3) : 0;
    const int textWidth = inner.width() - trendWidth - (hasTrend ? 8 : 0);
    QFont valueFont = font();
    valueFont.setPointSizeF(valueFont.pointSizeF() + 5.0);
    valueFont.setWeight(QFont::DemiBold);
    painter.setFont(valueFont);
    painter.setPen(colors.color(QPalette::Text));
    const QRect valueRect(inner.left(), inner.top() + 16, textWidth, 28);
    painter.drawText(valueRect, Qt::AlignLeft | Qt::AlignVCenter,
                     QFontMetrics(valueFont).elidedText(
                         tile.value, Qt::ElideRight, valueRect.width()));

    painter.setFont(labelFont);
    painter.setPen(tile.attention ? dangerColor()
                                  : colors.color(QPalette::PlaceholderText));
    const QRect noteRect(inner.left(), inner.bottom() - 14, textWidth, 16);
    painter.drawText(noteRect, Qt::AlignLeft | Qt::AlignVCenter,
                     QFontMetrics(labelFont).elidedText(
                         tile.note, Qt::ElideRight, noteRect.width()));

    if (hasTrend)
        paintSparkline(painter,
                       QRect(inner.right() - trendWidth + 1, inner.top() + 18,
                             trendWidth - 4, inner.height() - 20),
                       tile.trend);
}

void AgentGuardrailBoard::paintCard(QPainter &painter, const Card &card,
                                    bool hovered) const
{
    const QPalette colors = palette();
    const QColor text = colors.color(QPalette::Text);
    const QColor muted = colors.color(QPalette::PlaceholderText);
    QColor border = hovered          ? accentColor()
                    : card.attention ? dangerColor()
                                     : colors.color(QPalette::Mid);
    border.setAlpha(hovered || card.attention ? 220 : 140);
    painter.setPen(QPen(border, 1));
    painter.setBrush(colors.color(QPalette::Base));
    painter.drawRoundedRect(QRectF(card.rect).adjusted(0.5, 0.5, -0.5, -0.5),
                            kRadius, kRadius);

    const QRect inner = card.rect.adjusted(12, 9, -12, -9);
    QFont smallFont = font();
    smallFont.setPointSizeF(qMax(8.0, smallFont.pointSizeF() - 1.5));
    const QFontMetrics smallMetrics(smallFont);
    QFont titleFont = font();
    titleFont.setWeight(QFont::DemiBold);

    // Status pill at the far right; its state is spelled out, not colour only.
    const QString status = card.policy.enabled ? QStringLiteral("guarded")
                                               : QStringLiteral("off");
    const int pillWidth = smallMetrics.horizontalAdvance(status) + 14;
    const QRect pill(inner.right() - pillWidth + 1, inner.top(), pillWidth,
                     kPillHeight);
    QColor pillFill = card.policy.enabled ? accentColor()
                                          : colors.color(QPalette::Mid);
    pillFill.setAlpha(card.policy.enabled ? 230 : 90);
    painter.setPen(Qt::NoPen);
    painter.setBrush(pillFill);
    painter.drawRoundedRect(pill, kPillHeight / 2.0, kPillHeight / 2.0);
    painter.setFont(smallFont);
    painter.setPen(card.policy.enabled ? colors.color(QPalette::Base) : muted);
    painter.drawText(pill, Qt::AlignCenter, status);

    painter.setFont(titleFont);
    painter.setPen(card.policy.enabled ? text : muted);
    const QRect titleRect(inner.left(), inner.top() - 1,
                          pill.left() - 8 - inner.left(), 18);
    painter.drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter,
                     QFontMetrics(titleFont).elidedText(
                         card.policy.repository, Qt::ElideMiddle,
                         titleRect.width()));

    QFont headlineFont = font();
    headlineFont.setPointSizeF(headlineFont.pointSizeF() + 1.5);
    headlineFont.setWeight(QFont::DemiBold);
    painter.setFont(headlineFont);
    painter.setPen(card.policy.enabled ? text : muted);
    const QRect headlineRect(inner.left(), inner.top() + 22, inner.width(), 22);
    painter.drawText(headlineRect, Qt::AlignLeft | Qt::AlignVCenter,
                     QFontMetrics(headlineFont).elidedText(
                         card.headline, Qt::ElideRight, headlineRect.width()));

    painter.setFont(smallFont);
    painter.setPen(card.attention ? dangerColor() : muted);
    const QRect noteRect(inner.left(), inner.top() + 46, inner.width(), 14);
    painter.drawText(noteRect, Qt::AlignLeft | Qt::AlignVCenter,
                     smallMetrics.elidedText(card.note, Qt::ElideRight,
                                             noteRect.width()));

    painter.setPen(muted);
    const QRect factsRect(inner.left(), inner.bottom() - 14, inner.width(), 14);
    painter.drawText(factsRect, Qt::AlignLeft | Qt::AlignVCenter,
                     smallMetrics.elidedText(card.facts, Qt::ElideRight,
                                             factsRect.width()));
}

void AgentGuardrailBoard::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    for (const Tile &tile : std::as_const(m_tiles))
        paintTile(painter, tile);

    QFont sectionFont = font();
    sectionFont.setWeight(QFont::DemiBold);
    painter.setFont(sectionFont);
    painter.setPen(palette().color(QPalette::Text));
    for (const Section &section : std::as_const(m_sections))
        painter.drawText(section.rect, Qt::AlignLeft | Qt::AlignVCenter,
                         section.title);
    if (m_cards.isEmpty()) {
        painter.setPen(palette().color(QPalette::PlaceholderText));
        painter.drawText(QRect(kMargin, m_contentHeight - kMargin, width(), 20),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         QStringLiteral("Add a local repository to set guardrails."));
    }
    for (int i = 0; i < m_cards.size(); ++i)
        paintCard(painter, m_cards.at(i), i == m_hoverCard);
}

QString AgentGuardrailBoard::cardTooltip(const Card &card) const
{
    const AgentGuardrailRepoStats &s = card.stats;
    QStringList lines;
    lines << QStringLiteral("<b>%1</b> — %2")
                 .arg(card.policy.repository.toHtmlEscaped(),
                      card.policy.enabled ? QStringLiteral("guardrails on")
                                          : QStringLiteral("guardrails off"));
    if (card.policy.enabled)
        lines << QStringLiteral("Allowed: %1")
                     .arg(forkmesh::agentguard::policyDescription(
                              card.policy.allowedDirectories)
                              .toHtmlEscaped());
    if (!card.policy.missingDirectories.isEmpty())
        lines << QStringLiteral("Missing: %1")
                     .arg(card.policy.missingDirectories
                              .join(QStringLiteral(", "))
                              .toHtmlEscaped());
    lines << QStringLiteral("Blocked: %1 (%2 outside the repository)")
                 .arg(s.blocked)
                 .arg(s.blockedOutside);
    lines << QStringLiteral("Approved: %1 · denied: %2 · agents stopped: %3")
                 .arg(s.approved)
                 .arg(s.denied)
                 .arg(s.agentsStopped);
    QStringList tools;
    for (auto it = s.tools.cbegin(); it != s.tools.cend(); ++it)
        tools << QStringLiteral("%1 ×%2").arg(it.key().toHtmlEscaped()).arg(it.value());
    std::sort(tools.begin(), tools.end());
    if (!tools.isEmpty())
        lines << QStringLiteral("Tools: %1").arg(tools.join(QStringLiteral(", ")));
    if (!s.recentPaths.isEmpty()) {
        QStringList paths;
        for (const QString &path : s.recentPaths)
            paths << path.toHtmlEscaped();
        lines << QStringLiteral("Recent paths:<br>%1")
                     .arg(paths.join(QStringLiteral("<br>")));
    }
    if (s.lastEventMs > 0)
        lines << QStringLiteral("Last event: %1")
                     .arg(QDateTime::fromMSecsSinceEpoch(s.lastEventMs)
                              .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
    lines << QStringLiteral("<i>Click to open this repository's guardrail "
                            "settings.</i>");
    return QStringLiteral("<p>%1</p>").arg(lines.join(QStringLiteral("<br>")));
}

void AgentGuardrailBoard::mouseMoveEvent(QMouseEvent *event)
{
    int hover = -1;
    for (int i = 0; i < m_cards.size(); ++i) {
        if (m_cards.at(i).rect.contains(event->pos())) {
            hover = i;
            break;
        }
    }
    if (hover != m_hoverCard) {
        m_hoverCard = hover;
        setCursor(hover >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
        if (hover >= 0)
            QToolTip::showText(event->globalPosition().toPoint(),
                               cardTooltip(m_cards.at(hover)), this,
                               m_cards.at(hover).rect);
        else
            QToolTip::hideText();
    }
    QWidget::mouseMoveEvent(event);
}

void AgentGuardrailBoard::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        const QString repository = repositoryAt(event->pos());
        if (!repository.isEmpty()) {
            if (repositoryActivated)
                repositoryActivated(repository);
            event->accept();
            return;
        }
    }
    QWidget::mousePressEvent(event);
}

void AgentGuardrailBoard::leaveEvent(QEvent *event)
{
    if (m_hoverCard >= 0) {
        m_hoverCard = -1;
        unsetCursor();
        update();
    }
    QWidget::leaveEvent(event);
}
