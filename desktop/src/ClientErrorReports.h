#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QtGlobal>

namespace forkmesh {

inline const QString kReportUserVisibleErrorsSetting =
    QStringLiteral("diagnostics/reportErrors");

// Notice/ping kind for the failure of a local operation the operator asked this
// machine for and is watching: removing a worktree, deleting a branch. See
// ClientErrorReports::describesLocalOperation.
inline const QString kLocalOperationNoticeKind = QStringLiteral("local");

// Warning dialogs normally join the shared error log automatically from the
// application event filter. A dialog that only refuses an invalid local
// operation can set this dynamic property before it is shown to keep the same
// warning UI and local notification without filing a release-level failure.
inline constexpr char kLocalOperationDialogProperty[] =
    "forkmeshLocalOperation";

class ClientErrorReports
{
public:
    struct Report {
        QString kind;     // "dialog" (modal) or "notice" (inline error notice)
		QString surface;  // desktop surface; becomes the relay's error_log path
        QString title;    // the dialog's own name for the operation, may be empty
        QString message;  // the failure text; empty means "nothing to report"
        qint64 tsMs = 0;  // when the operator saw it
        int attempts = 0; // sends tried so far (a deferred report keeps counting)
        qint64 pingId = 0;
    };

    static constexpr int kMaxMessageChars = 700;
    static constexpr int kMaxTitleChars = 120;
    static constexpr qint64 kWindowMs = 60LL * 60 * 1000;
    static constexpr int kMaxPerWindow = 8;
    static constexpr int kMaxTrackedKeys = 200;
    static constexpr int kMaxDeferred = 8;
    static constexpr int kMaxAttempts = 3;
    static constexpr qint64 kMaxDeferralMs = 6LL * 60 * 60 * 1000;

    static QString redact(const QString &text, int maxChars);

    // True for an alert this machine only relayed: the relay's own error log
    // and status monitor push these into the account's ping inbox, and the
    // desktop just echoes them as warning cards. Reporting one back would
    // store an error *about* an error report, and because the relay groups on
    // the exact message text, the quoted copy never matches the group it came
    // from — so each round opens a fresh group, which pings again, forever
    // (adhoc #1654).
    static bool echoesRelayAlert(const QString &kind, const QString &text);

    // True for the failure of a local operation the operator just asked for on
    // this machine. The refusal is the answer to their click — git would not
    // remove a worktree, a branch would not delete — and they are looking at it
    // when it happens, so nobody learns anything from a copy in the relay's
    // error log. Worse, these carry a path, and redact() collapses every path
    // to "<path>": one group for the whole fleet, pinging every account on
    // every occurrence, for something no release could fix (adhoc #1659).
    // Classified by kind, never by message text — a substring rule would sweep
    // up real faults that merely mention a path.
    static bool describesLocalOperation(const QString &kind);

    static QString failureGroupKey(const QString &message);
    enum class DialogButton { Acknowledge, Choice, Decision, Aside };
    static bool dialogAsksQuestion(const QList<DialogButton> &buttons);

    static bool relayAccepts(const Report &report);
    static bool permanentRefusal(int httpStatus);

    static Report build(const QString &kind, const QString &surface,
                        const QString &title, const QString &message,
                        qint64 nowMs);

    static QJsonObject payload(const Report &report);

    bool accept(const Report &report);

    bool defer(const Report &report);

    QList<Report> takeDeferred(qint64 nowMs, QList<Report> *expiredOut = nullptr);
    int deferredCount() const { return m_deferred.size(); }

private:
    static QString dedupeKey(const Report &report);
    void prune(qint64 nowMs);

    QHash<QString, qint64> m_lastSentMs; // dedupe key -> when it last went out
    QList<qint64> m_acceptedMs;          // send stamps inside the current window
    QList<Report> m_deferred;
};

class ErrorPingGroups
{
public:
    static constexpr qint64 kQuietMs = 30LL * 60 * 1000;
    static constexpr int kMaxGroups = 200;

    struct Group {
        qint64 pingId = 0;
        QString title;
        int count = 0;
        qint64 firstMs = 0;
        qint64 lastMs = 0;
    };

    Group note(const QString &key, qint64 nowMs);
    void bind(const QString &key, qint64 pingId, const QString &title);
    void forget(const QString &key);
    void clear() { m_groups.clear(); }
    int size() const { return m_groups.size(); }

private:
    QHash<QString, Group> m_groups;
};

} // namespace forkmesh
