#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

// Host management from the owner's phone, over the local control channel
// (LocalControl.h), on the LAN or through the relay. This file is the policy
// half, kept free of widgets so the tests can drive it: who may manage hosts,
// the fresh confirmation every mutating operation needs, how a saved host
// becomes a phone row, and the bounded, acknowledged stream of log lines.
// The operations themselves are the Hosts page's own code paths
// (MainWindowLocalControlHosts.cpp); nothing here reaches a host, and the
// Vultr key and SSH keys never leave the desktop.
namespace forkmesh::localcontrol::hosts {

// Operations the phone can ask for. Every one but "list"/"detail"/"options"
// and the log stream changes something, so each needs a confirmation token
// from "hosts.prepare"; "destroy" also needs the host's name typed back.
QStringList mutatingActions();
bool isMutatingAction(const QString &action);
bool isDestructiveAction(const QString &action);
// The phone shows this before asking for the confirmation.
QString actionSummary(const QString &action, const QString &host);

// Only the account that owns this desktop manages its hosts from a phone: the
// device must be approved at this desktop (the channel checks that) and name
// the same account the desktop is signed in as.
bool ownerMayManageHosts(const QString &deviceUser, const QString &desktopAccount);

// A typed-name confirmation: the host's own name, exactly, ignoring only
// surrounding whitespace.
bool typedNameMatches(const QString &typed, const QString &host);

// A host name the phone may name: the saved-host alphabet, bounded.
bool validHostName(const QString &name);

// One-use confirmation tokens, bound to the device, the action and the host,
// that expire after a minute.
class ConfirmationStore
{
public:
    enum class Result { Ok, Missing, Expired, Mismatch };

    static constexpr qint64 kLifetimeMs = 60 * 1000;
    static constexpr int kMaxOutstanding = 32;

    QString issue(const QString &deviceKey, const QString &action,
                  const QString &host, qint64 nowMs);
    // Spends the token whatever the outcome, so a wrong guess cannot be
    // retried against the same token.
    Result consume(const QString &token, const QString &deviceKey,
                   const QString &action, const QString &host, qint64 nowMs);
    int outstanding() const { return m_tokens.size(); }

private:
    struct Entry {
        QString deviceKey;
        QString action;
        QString host;
        qint64 expiresAtMs = 0;
    };
    void prune(qint64 nowMs);
    QHash<QString, Entry> m_tokens;
};

QString confirmationError(ConfirmationStore::Result result);

// "online", "attention", "offline", "provisioning" or "unknown", from the
// Hosts page's own probe label ("● Online · Mirror node", ...).
QString healthFromProbeLabel(const QString &label);

// A saved host (the Hosts page record) as the phone lists it. `probe` is the
// live probe label, empty when none has run; `version`/`online` come from the
// home roster when the node is in it.
QJsonObject hostRow(const QJsonObject &saved, const QString &probe,
                    const QString &version, bool rosterOnline);

// The bounded stream of one host's log lines to one phone. Lines are batched;
// at most kWindow batches travel unacknowledged, and while the phone lags the
// buffer keeps only the newest kMaxBufferedLines and counts what it dropped.
class LogStream
{
public:
    static constexpr int kWindow = 4;
    static constexpr int kMaxBufferedLines = 2000;
    static constexpr int kMaxBatchLines = 200;
    static constexpr int kMaxLineChars = 2000;

    void push(qint64 atMs, const QString &source, const QString &text);
    bool hasPending() const { return !m_buffer.isEmpty() || m_dropped > 0; }
    bool canSend() const { return m_inFlight < kWindow; }
    // The next batch: {seq, lines:[{t, src, text}], dropped}; empty when
    // nothing is pending or the window is full.
    QJsonObject takeBatch();
    // The phone has shown every batch up to `seq`.
    void ack(qint64 seq);
    qint64 droppedTotal() const { return m_droppedTotal; }
    int buffered() const { return m_buffer.size(); }
    int inFlight() const { return m_inFlight; }

private:
    struct Line {
        qint64 atMs = 0;
        QString source;
        QString text;
    };
    QList<Line> m_buffer;
    qint64 m_nextSeq = 1;
    qint64 m_ackedSeq = 0;
    int m_inFlight = 0;
    qint64 m_dropped = 0;
    qint64 m_droppedTotal = 0;
};

} // namespace forkmesh::localcontrol::hosts
