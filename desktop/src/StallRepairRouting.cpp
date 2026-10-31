#include "StallRepairRouting.h"

#include <QtGlobal>

namespace forkmesh::stallrepair {

bool looksLikeStallWork(const QString &title, const QString &prompt)
{
    if (title.contains(QLatin1String("UI stall repair"), Qt::CaseInsensitive))
        return true;
    const QString head = prompt.left(400);
    return head.contains(QLatin1String("Please fix these UI stalls")) ||
           head.contains(QLatin1String("ForkMesh's GUI thread stalled")) ||
           head.contains(QLatin1String("More UI stalls were recorded"));
}

bool canTakeMoreStalls(const Candidate &candidate)
{
    if (candidate.id <= 0 || candidate.foreign || candidate.associationOnly ||
        candidate.merged || !candidate.sameRepository) {
        return false;
    }
    return candidate.active || candidate.succeeded;
}

int pickSession(const QList<Candidate> &candidates, int rememberedId)
{
    if (rememberedId > 0) {
        for (const Candidate &candidate : candidates) {
            if (candidate.id == rememberedId && canTakeMoreStalls(candidate))
                return candidate.id;
        }
    }
    const Candidate *best = nullptr;
    for (const Candidate &candidate : candidates) {
        if (!candidate.active || !canTakeMoreStalls(candidate) ||
            !looksLikeStallWork(candidate.title, candidate.prompt)) {
            continue;
        }
        if (!best || candidate.createdAtMs > best->createdAtMs ||
            (candidate.createdAtMs == best->createdAtMs &&
             candidate.id > best->id)) {
            best = &candidate;
        }
    }
    return best ? best->id : 0;
}

qint64 followUpDelayMs(qint64 nowMs, qint64 lastDeliveryMs)
{
    if (lastDeliveryMs <= 0 || nowMs < lastDeliveryMs)
        return kFollowUpSettleMs;
    const qint64 sinceLast = nowMs - lastDeliveryMs;
    return qMax(kFollowUpSettleMs, kFollowUpMinIntervalMs - sinceLast);
}

QString followUpPrompt(const QStringList &newEntries, int totalStalls,
                       const QString &locations, int maxChars)
{
    QString head =
        QStringLiteral(
            "More UI stalls were recorded while you work on the stall repair: "
            "%1 new report(s), %2 this session in total. Add them to the work "
            "you already have under way in this session rather than starting "
            "over: for each one, find the blocking call in the backtrace and "
            "move the slow work off the GUI thread or skip it when nothing "
            "changed. Reports that match a fix you already made need no new "
            "change; say so in your summary.\n\n")
            .arg(newEntries.size())
            .arg(totalStalls);
    head += locations;
    head += QStringLiteral("\nNew stall reports (newest first):\n\n");

    QString body;
    const QString separator = QStringLiteral("\n\n---\n\n");
    for (int i = newEntries.size() - 1; i >= 0; --i) {
        const QString &entry = newEntries.at(i);
        if (!body.isEmpty() &&
            head.size() + body.size() + separator.size() + entry.size() >
                maxChars) {
            body += QStringLiteral(
                "\n\n(older reports omitted - the full history is in the "
                "stall log above)");
            break;
        }
        if (!body.isEmpty())
            body += separator;
        body += entry;
    }
    if (body.isEmpty())
        body = QStringLiteral("(nothing new recorded)");
    return (head + body).left(maxChars);
}

}
