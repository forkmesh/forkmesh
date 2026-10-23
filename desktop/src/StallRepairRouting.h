#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace forkmesh::stallrepair {

inline constexpr qint64 kFollowUpMinIntervalMs = 10LL * 60 * 1000;
inline constexpr qint64 kFollowUpSettleMs = 60LL * 1000;
inline constexpr int kFollowUpMaxChars = 15500;

struct Candidate {
    int id = 0;
    bool active = false;
    bool succeeded = false;
    bool merged = false;
    bool foreign = false;
    bool associationOnly = false;
    bool sameRepository = false;
    QString title;
    QString prompt;
    qint64 createdAtMs = 0;
};

bool looksLikeStallWork(const QString &title, const QString &prompt);
bool canTakeMoreStalls(const Candidate &candidate);
int pickSession(const QList<Candidate> &candidates, int rememberedId);
qint64 followUpDelayMs(qint64 nowMs, qint64 lastDeliveryMs);
QString followUpPrompt(const QStringList &newEntries, int totalStalls,
                       const QString &locations,
                       int maxChars = kFollowUpMaxChars);

}
