#include "PingBatch.h"

#include <QHash>
#include <QRegularExpression>

#include <algorithm>

namespace forkmesh::pingbatch {

namespace {

QString clipped(const QString &text, int limit)
{
    const QString flat = text.simplified();
    if (flat.size() <= limit)
        return flat;
    return flat.left(limit - 1).trimmed() + QChar(0x2026);
}

struct Group {
    QString title;
    QString firstTitle;
    QStringList refs;
    QString latestBody;
    int count = 0;
    int warnings = 0;
    int latestIndex = 0;
};

}

int nextThreshold(int burstTotal)
{
    if (burstTotal < kFirstSummaryAt)
        return kFirstSummaryAt;
    if (burstTotal < kSecondSummaryAt)
        return kSecondSummaryAt;
    return (burstTotal / kLaterSummaryStep + 1) * kLaterSummaryStep;
}

QString groupTitle(const QString &title, QStringList *refs)
{
    static const QRegularExpression ref(QStringLiteral("#\\d+"));
    QString grouped = title;
    if (refs) {
        auto it = ref.globalMatch(title);
        while (it.hasNext())
            refs->append(it.next().captured(0));
    }
    grouped.remove(ref);
    grouped = grouped.simplified();
    static const QRegularExpression danglingSpace(QStringLiteral("\\s+([!?.,:;])"));
    grouped.replace(danglingSpace, QStringLiteral("\\1"));
    return grouped.isEmpty() ? title.simplified() : grouped;
}

Card summarize(const QList<Item> &items, int burstTotal)
{
    Card card;
    card.summary = true;
    card.count = int(items.size());
    QList<Group> groups;
    QHash<QString, int> byKey;
    for (int i = 0; i < items.size(); ++i) {
        const Item &item = items.at(i);
        QStringList refs;
        const QString title = groupTitle(
            item.title.simplified().isEmpty() ? item.body : item.title, &refs);
        const QString key = title.toCaseFolded();
        int at = byKey.value(key, -1);
        if (at < 0) {
            at = int(groups.size());
            byKey.insert(key, at);
            groups.append(Group{title, item.title.simplified().isEmpty()
                                           ? item.body.simplified()
                                           : item.title.simplified(),
                                {}, {}, 0, 0, 0});
        }
        Group &group = groups[at];
        ++group.count;
        if (item.warning)
            ++group.warnings;
        for (const QString &r : refs)
            if (!group.refs.contains(r))
                group.refs.append(r);
        group.latestIndex = i;
        group.latestBody = item.title.simplified().isEmpty() ? QString() : item.body;
        if (item.warning)
            card.warning = true;
    }
    std::stable_sort(groups.begin(), groups.end(), [](const Group &a, const Group &b) {
        if ((a.warnings > 0) != (b.warnings > 0))
            return a.warnings > 0;
        if (a.count != b.count)
            return a.count > b.count;
        return a.latestIndex > b.latestIndex;
    });
    int warnings = 0;
    for (const Item &item : items)
        if (item.warning)
            ++warnings;

    card.title = warnings > 0
                     ? QStringLiteral("%1 more pings, %2 with a problem")
                           .arg(card.count)
                           .arg(warnings)
                     : QStringLiteral("%1 more pings").arg(card.count);

    QStringList lines;
    const int shown = std::min<int>(kSummaryGroups, int(groups.size()));
    for (int i = 0; i < shown; ++i) {
        const Group &group = groups.at(i);
        QString line;
        if (group.count == 1) {
            line = clipped(group.firstTitle, 60);
            if (!group.latestBody.simplified().isEmpty())
                line += QString::fromUtf8(" \xE2\x80\x94 ") + clipped(group.latestBody, 60);
        } else {
            line = QStringLiteral("%1%2 %3").arg(QString::number(group.count),
                                                 QString(QChar(0x00D7)),
                                                 clipped(group.title, 50));
            if (!group.refs.isEmpty()) {
                QStringList refs = group.refs.mid(
                    std::max<qsizetype>(0, group.refs.size() - kSummaryRefs));
                line += QString::fromUtf8(" \xE2\x80\x94 ") + refs.join(QStringLiteral(", "));
                if (group.refs.size() > kSummaryRefs)
                    line += QStringLiteral(" +%1").arg(group.refs.size() - kSummaryRefs);
            } else if (!group.latestBody.simplified().isEmpty()) {
                line += QStringLiteral(" (latest: %1)").arg(clipped(group.latestBody, 50));
            }
        }
        lines.append(line);
    }
    if (groups.size() > shown) {
        int rest = 0;
        for (int i = shown; i < groups.size(); ++i)
            rest += groups.at(i).count;
        lines.append(QStringLiteral("+ %1 more of %2 other kinds")
                         .arg(rest)
                         .arg(groups.size() - shown));
    }
    lines.append(QStringLiteral("%1 pings in this burst. Pings lists each one.")
                     .arg(burstTotal));
    card.body = lines.join(QLatin1Char('\n'));
    return card;
}

Step Batcher::offer(const Item &item, qint64 nowMs)
{
    if (m_held.isEmpty() && m_lastAtMs > 0 && nowMs - m_lastAtMs >= kQuietMs) {
        m_total = 0;
        m_threshold = kFirstSummaryAt;
    }
    m_lastAtMs = nowMs;
    ++m_total;
    if (m_total <= kSoloCards)
        return Step::Show;
    if (m_held.isEmpty())
        m_firstHeldAtMs = nowMs;
    Item held = item;
    if (held.atMs <= 0)
        held.atMs = nowMs;
    m_held.append(held);
    if (m_total >= m_threshold) {
        m_threshold = nextThreshold(m_total);
        return Step::Summarize;
    }
    return Step::Hold;
}

bool Batcher::due(qint64 nowMs) const
{
    return !m_held.isEmpty() && nowMs >= dueAtMs();
}

qint64 Batcher::dueAtMs() const
{
    if (m_held.isEmpty())
        return 0;
    return std::min(m_lastAtMs + kQuietMs, m_firstHeldAtMs + kMaxHoldMs);
}

Card Batcher::take()
{
    Card card;
    if (m_held.size() == 1) {
        const Item &item = m_held.first();
        card.title = item.title;
        card.body = item.body;
        card.warning = item.warning;
        card.href = item.href;
        card.count = 1;
    } else if (!m_held.isEmpty()) {
        card = summarize(m_held, m_total);
    }
    m_held.clear();
    m_firstHeldAtMs = 0;
    return card;
}

} // namespace forkmesh::pingbatch
