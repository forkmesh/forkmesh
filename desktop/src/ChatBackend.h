#pragma once

#include "ChatMessageRetention.h"
#include "NodeDiagnostics.h"
#include "UserStatus.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

struct CommitIdentity {
    QString subject;           // first line of the commit message
    QString author;            // author name as recorded in the commit
    qint64 committedAtMs = 0;  // commit time; 0 = unknown/not advertised
};

constexpr int kMaxCommitSubjectChars = 120;
constexpr int kMaxCommitAuthorChars = 64;

struct MirrorAdvert {
    QString ownerName;    // this node's clone identity "<account>/name" (host routing)
    QString source;
    QString commit;       // full HEAD commit hash of the node's mirror (may be empty)
    QString branch;       // branch HEAD points to
    QString refsFingerprint;
    CommitIdentity commitIdentity;
    qint64 updatedMs = 0; // when the node last synced this repo from its source
    qint64 sizeBytes = 0; // on-disk size of this node's bare mirror (git objects)
    int issueCount = -1;  // issues this node's mirror holds; -1 = not advertised
    int commitCount = -1;     // commits on the served branch
    int mergeCount = -1;      // merge commits on the served branch
    QJsonArray activityWeeks; // commits per week, oldest first (52 buckets)
    QJsonArray mergeActivityWeeks; // merges per week, oldest first
    int branchCount = -1;     // local branches (refs/heads) the mirror holds
    int pullCount = -1;       // pull requests the mirror holds
    int discussionCount = -1; // discussions the mirror holds
    int worktreeCount = -1;   // git worktrees on this node's working copy
    int artifactCount = -1;
};

struct MemberInfo {
    QString id;
    QString name;      // chat/user display name
    QString nodeName;  // registered node account name, when known
    QString ownerUser; // user account that owns this node, when linked/known
    // "node" (default) or "user" — stamped from the sender's accountKind, so a
    // user-only profile (e.g. a bot's relayed chat, or a desktop client signed
    // in as a plain user account, not a linked node) can be told apart from a
    // real serving node. Empty for older peers that never advertised it, which
    // callers should treat the same as "node" for backward compatibility.
    QString accountKind;
    QString note;     // e.g. "(discovered)"
    qint64 createdAtMs = 0;
    bool self = false;
    bool online = false; // live link right now (green dot)
    QString solanaAddress;
    QString solanaBalance;  // reserved for a later balance indexer/API
    QString platform;    // linux | macos | windows | android | ios | web
    QString version;     // ForkMesh app version advertised by the node
    QStringList mirrors; // "owner/name" of repos this node mirrors
    QList<MirrorAdvert> mirrorDetails;
    qint64 memUsedBytes = 0;
    qint64 memTotalBytes = 0;
    qint64 diskUsedBytes = 0;
    qint64 diskTotalBytes = 0;
    double cpuPercent = -1.0; // whole-host CPU utilization, 0..100
    QList<NodeDiagnostics::Finding> diagnostics;
    qint64 diagnosticsMs = 0;
    // The member's away flag and custom status, as the peer's own relay
    // hello/presence frame or the account directory reported it. Only
    // meaningful when hasUserStatus is set: a frame from an older client
    // carries none, and must not read as "no status".
    forkmesh::userstatus::UserStatus userStatus;
    bool hasUserStatus = false;
};

struct ChatMessage {
    QString id;
    QString conversation;
    QString threadRootId;
    QString senderId;
    QString senderName;
    QString text;
    qint64 timestampMs = 0;
    QString fileName;   // non-empty => has a file attachment
    QString fileMime;
    QByteArray fileData;
    bool self = false;
    bool edited = false;
    bool deleted = false;
    bool historical = false; // backfill: render/cache without live notifications
    int recipientCount = -1;
    QStringList recipientNames;
    QJsonArray blocks;

    bool hasFile() const { return !fileName.isEmpty(); }
};

class ChatBackend : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;

    // Returns the locally-created message id when the backend can expose it.
    // The Team room uses that id as the thread root for mentioned teammates.
    virtual QString sendChat(const QString &channel, const QString &text) = 0;
    virtual void sendThreadReply(const QString &channel,
                                 const QString &rootMessageId,
                                 const QString &text) {
        Q_UNUSED(channel);
        Q_UNUSED(rootMessageId);
        Q_UNUSED(text);
    }
    virtual void sendDirect(const QString &targetId, const QString &text) = 0;
    virtual void sendFile(const QString &conversation, const QString &fileName,
                          const QString &mimeType, const QByteArray &data) = 0;
    virtual void sendReaction(const QString &conversation, const QString &messageId,
                              const QString &emoji) = 0;
    virtual void editMessage(const QString &conversation, const QString &messageId,
                             const QString &newText) = 0;
    virtual void deleteMessage(const QString &conversation, const QString &messageId) = 0;
    virtual void setAccountKind(const QString &kind) { Q_UNUSED(kind); }
    // Re-key room encryption once the server-issued shared passphrase arrives.
    // The passphrase fetch is async and often resolves after the backend is
    // already connected (it starts from the first heartbeat, which fires after
    // the initial connect), so a backend seeded with a fallback key must be
    // able to switch to the real one without a reconnect. Default no-op for
    // backends without room encryption.
    virtual void setRoomPassphrase(const QString &passphrase) { Q_UNUSED(passphrase); }
    // Public, non-secret id of this installation's automatic private-chat
    // encryption identity, or empty for backends without the ForkMesh room.
    virtual QString roomSealKeyId() const { return QString(); }
    virtual void sendBotChat(const QString &channel, const QString &text) {
        Q_UNUSED(channel);
        Q_UNUSED(text);
    }
    // ForkBot's answer belongs beside the message that invoked it. Backends
    // without thread support can leave this as a no-op.
    virtual void sendBotThreadReply(const QString &channel,
                                    const QString &rootMessageId,
                                    const QString &text) {
        Q_UNUSED(channel);
        Q_UNUSED(rootMessageId);
        Q_UNUSED(text);
    }
    // Best-effort room relay for a repository teammate. Implementations keep
    // ordinary private rooms closed; the built-in device-local #team room may
    // admit configured teammates. Other backends may leave this as a no-op.
    virtual void sendTeammateChat(const QString &channel,
                                  const QString &teammateId,
                                  const QString &teammateName,
                                  const QString &text) {
        Q_UNUSED(channel);
        Q_UNUSED(teammateId);
        Q_UNUSED(teammateName);
        Q_UNUSED(text);
    }
    // A local configured teammate may answer inside the private #team room.
    // The desktop keeps that room off the mesh transport; generic bot
    // integrations are not admitted.
    virtual void sendTeammateThreadReply(const QString &channel,
                                         const QString &rootMessageId,
                                         const QString &teammateId,
                                         const QString &teammateName,
                                         const QString &text) {
        Q_UNUSED(channel);
        Q_UNUSED(rootMessageId);
        Q_UNUSED(teammateId);
        Q_UNUSED(teammateName);
        Q_UNUSED(text);
    }
    // A device-local Team-room policy notice.  This is deliberately separate
    // from a teammate reply: the guard may speak before any configured agent
    // is allowed to receive the person's message.
    virtual void sendTeamNotice(const QString &channel,
                                const QString &rootMessageId,
                                const QString &text) {
        Q_UNUSED(channel);
        Q_UNUSED(rootMessageId);
        Q_UNUSED(text);
    }
    // Admin moderation: broadcast a signed request to delete any message (the
    // signature, made by the admin's identity key over a canonical string, lets
    // every peer authenticate it). Default no-op for backends without it.
    virtual void sendAdminDelete(const QString &conversation, const QString &messageId,
                                 qint64 ts, const QString &signature) {
        Q_UNUSED(conversation);
        Q_UNUSED(messageId);
        Q_UNUSED(ts);
        Q_UNUSED(signature);
    }
    // Apply an admin delete that the caller has already verified (good signature
    // from a confirmed admin). Removes the message and emits messageDeleted.
    virtual void applyAdminDelete(const QString &conversation,
                                  const QString &messageId) {
        Q_UNUSED(conversation);
        Q_UNUSED(messageId);
    }
    virtual void setAvatar(const QByteArray &pngData) = 0;
    virtual void setUserName(const QString &name) { Q_UNUSED(name); }
    virtual void setNodeIdentity(const QString &nodeName, const QString &ownerUser) {
        Q_UNUSED(nodeName);
        Q_UNUSED(ownerUser);
    }
    // This account's away flag and custom status (UserStatus::toJson). The
    // relay backend carries it on every hello/presence frame from now on and
    // sends a presence frame at once, so peers repaint without waiting for
    // the next beat. An empty object stops advertising one.
    virtual void setUserStatus(const QJsonObject &status) { Q_UNUSED(status); }
    virtual void forgetMember(const QString &peerId) { Q_UNUSED(peerId); }
    virtual void sendTyping(const QString &conversation, bool active) = 0;
    virtual void addChannel(const QString &channel) = 0;
    virtual void removeChannel(const QString &channel) { Q_UNUSED(channel); }
    virtual void createPrivateChannel(const QString &channel) { Q_UNUSED(channel); }
    // Announce that this node just opened an encrypted cove. The creator's node
    // (creatorKey) recognizes itself and raises a notification; everyone else
    // ignores it. The opener signs a canonical string so the creator can trust
    // who opened it. Ephemeral — not retained or replayed by the relay.
    virtual void notifyCoveOpened(const QString &creatorKey, const QString &coveId,
                                  const QString &coveName, const QString &openerKey,
                                  const QString &openerName, qint64 ts,
                                  const QString &signature)
    {
        Q_UNUSED(creatorKey);
        Q_UNUSED(coveId);
        Q_UNUSED(coveName);
        Q_UNUSED(openerKey);
        Q_UNUSED(openerName);
        Q_UNUSED(ts);
        Q_UNUSED(signature);
    }
    // Announce that `inviteeAccount` was just granted access to an account-scoped
    // cove. Every online node checks inviteeAccount against its own account and
    // raises a notification if it matches; everyone else ignores it. Advisory
    // only (it grants nothing by itself — the actual access grant already lives
    // in the cove's encrypted invitedAccounts list), so unlike notifyCoveOpened
    // it isn't signed.
    virtual void notifyCoveInvited(const QString &inviteeAccount, const QString &coveId,
                                   const QString &coveName, const QString &inviterName,
                                   const QString &shareUrl, qint64 ts)
    {
        Q_UNUSED(inviteeAccount);
        Q_UNUSED(coveId);
        Q_UNUSED(coveName);
        Q_UNUSED(inviterName);
        Q_UNUSED(shareUrl);
        Q_UNUSED(ts);
    }
    // Live WebSocket / Durable Object diagnostics for the Network tab. Backends
    // without a socket return an empty list.
    virtual QList<QJsonObject> networkDiagnostics() const { return {}; }
    virtual void setNetworkAvailable(bool available) { Q_UNUSED(available); }
    virtual void shutdown() = 0;
    virtual QString modeName() const = 0;

signals:
    void messageArrived(const ChatMessage &message);
    void reactionChanged(const QString &conversation, const QString &messageId,
                         const QString &emoji, const QString &reactorName, bool added);
    void messageEdited(const QString &conversation, const QString &messageId,
                       const QString &newText);
    void messageDeleted(const QString &conversation, const QString &messageId);
    // An admin moderation delete arrived from a peer. NOT applied yet — the UI
    // must verify the signature and the sender's admin status, then call
    // applyAdminDelete() to actually remove it.
    void adminDeleteRequested(const QString &conversation, const QString &messageId,
                              const QString &adminId, const QString &adminName,
                              qint64 ts, const QString &signature);
    void avatarChanged(const QString &peerId, const QByteArray &pngData);
    void typingChanged(const QString &conversation, const QString &peerId,
                       const QString &peerName, bool active);
    void systemMessage(const QString &text);
    void firewallBlocking(const QString &displayCommand,
                          const QString &privilegedCommand);
    void firewallHealthy();
    void channelsChanged(const QStringList &channels);
    void privateChannelJoined(const QString &channel);
    void rosterChanged(const QList<MemberInfo> &members);
    void mirrorUpdated(const QString &ownerName, const QString &peerName,
                       const QString &commit);
    void mirrorSynced(const QString &ownerName, const QString &peerName,
                      const QString &commit);
    // A peer opened an encrypted cove. The UI verifies the opener's signature and,
    // if this node created the cove (creatorKey), raises an "opened" notification.
    void coveOpened(const QString &creatorKey, const QString &coveId,
                    const QString &coveName, const QString &openerKey,
                    const QString &openerName, qint64 ts, const QString &signature);
    void coveInvited(const QString &inviteeAccount, const QString &coveId,
                     const QString &coveName, const QString &inviterName,
                     const QString &shareUrl, qint64 ts);
    // A brand-new ForkMesh account was created while this node was connected.
    // The relay signs and originates this frame; peers can only send encrypted
    // envelopes, so it cannot be forged from another client.
    void newMemberJoined(const QString &accountName);
    // A sealed frame was not addressed to this device, or an unsealed frame
    // was refused in a protected conversation. Carries only the public
    // sender-key id (empty for the latter), never private key material.
    void roomSealMismatch(const QString &keyId);
    void networkDiagnosticsChanged();
    // One text frame on the relay's websocket as it crosses: which way, its
    // type once opened ("chat", "presence", "typing", "e2ee-key"; "text" when
    // it could not be), the peer it came from or was sent to (empty for the
    // whole room or the relay itself), and its size on the wire. Only the
    // envelope's facts: never its contents.
    void socketFrame(bool outgoing, const QString &type, const QString &peerId,
                     qint64 bytes);
    void latencySampled(int ms);
    void statusChanged(const QString &status);
    void fatalError(const QString &message);
};
