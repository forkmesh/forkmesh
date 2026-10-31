#pragma once

#include <QList>
#include <QString>
#include <QtGlobal>

namespace forkmesh::pingbatch {

inline constexpr int kSoloCards = 3;
inline constexpr int kFirstSummaryAt = 10;
inline constexpr int kSecondSummaryAt = 100;
inline constexpr int kLaterSummaryStep = 100;
inline constexpr qint64 kQuietMs = 30 * 1000;
inline constexpr qint64 kMaxHoldMs = 5 * 60 * 1000;
inline constexpr int kSummaryGroups = 4;
inline constexpr int kSummaryRefs = 5;

struct Item {
    QString title;
    QString body;
    bool warning = false;
    QString href;
    qint64 atMs = 0;
};

struct Card {
    QString title;
    QString body;
    bool warning = false;
    QString href;
    int count = 0;
    bool summary = false;
};

enum class Step { Show, Hold, Summarize };

int nextThreshold(int burstTotal);
QString groupTitle(const QString &title, QStringList *refs = nullptr);
Card summarize(const QList<Item> &items, int burstTotal);

class Batcher
{
public:
    Step offer(const Item &item, qint64 nowMs);
    bool due(qint64 nowMs) const;
    qint64 dueAtMs() const;
    Card take();
    int held() const { return int(m_held.size()); }
    int burstTotal() const { return m_total; }

private:
    QList<Item> m_held;
    int m_total = 0;
    int m_threshold = kFirstSummaryAt;
    qint64 m_lastAtMs = 0;
    qint64 m_firstHeldAtMs = 0;
};

} // namespace forkmesh::pingbatch
