#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

class ForkMeshIdentity;

// One entry in an issue's append-only, signed history. Different event types use
// different fields. Signatures are raw Ed25519 over an
// explicit canonical string, matching the worker's ed25519_verify.
struct IssueEvent {
    QString type;          // open | comment | edit | status | labels | milestone | dates | priority | progress | assignees | reporter | agent | bounty | reaction | delete
    QString id;
    QString author;        // signer pubkey (base64url)
    QString authorName;
    qint64 ts = 0;
    QString title;         // open/title
    QString body;          // open/comment/edit body stored in issue JSON
    QStringList attachments; // open/comment/edit (issue-folder-relative paths)
    QString target;        // edit/delete/reaction (event id, or "self" for delete-issue)
    QString status;        // status: open|closed
    QStringList labels;    // labels
    QString milestone;     // milestone (empty = none)
    qint64 startDate = 0;  // dates: planned start (epoch ms, 0 = unset)
    qint64 endDate = 0;    // dates: planned end (epoch ms, 0 = unset)
    int priority = 0;      // priority: 1 (highest) through 8999 (lowest), 0 = unset
    int progress = 0;      // progress: 0..100 percent complete
    QStringList assignees; // assignees
    // claim: one account adding or dropping ITSELF. Unlike "assignees", which
    // replaces the whole list and so needs repository authority, a claim moves
    // exactly the named account, which is why a public contributor is allowed
    // to sign one for themselves (issue #570).
    QString assignee;      // claim: the account claiming or releasing
    QString claimState;    // claim: assigned|released
    QString reporter;      // reporter: who the issue is credited to
    QString reaction;      // reaction: vocabulary slug ("eyes"), see ReactionCatalog.h
    QString agentProvider; // agent: codex|openai|claude-api|claude-code
    int agentSessionId = 0; // agent
    QString agentStatus;   // agent
    bool agentCreatePr = false; // agent
    double bountyUsd = 0.0;     // legacy bounty audit metadata, in USD
    QString bountyAddress;      // legacy escrow address; migration-only/read-only
    QString bountyStatus;       // historical open | funded | paid state
    QString sig;

    QJsonObject toJson() const;
    static IssueEvent fromJson(const QJsonObject &obj);
};

// A single issue: derived metadata plus the full signed event log.
struct Issue {
    int number = 0;
    QString title;
    QString status = "open";
    QStringList labels;
    QString milestone;
    qint64 startDate = 0; // planned start (epoch ms, 0 = unset)
    qint64 endDate = 0;   // planned end (epoch ms, 0 = unset)
    int priority = 0; // 1 (highest) through 8999 (lowest), 0 = unset
    int progress = 0; // 0..100 percent complete (latest signed progress event)
    QStringList assignees;
    qint64 createdAt = 0;
    QString author;
    QString authorName;
    int votes = 0; // distinct voters (uptime-credit votes), derived from events
    // Bounty state folded from the latest signed "bounty" event (0 = none).
    double bountyUsd = 0.0;
    QString bountyAddress;
    QString bountyStatus;
    // Owner-authored contributor reward estimate. This is deliberately
    // separate from the frozen legacy USD bounty/escrow fields above: it is
    // paid directly from an organization owner's local Qt wallet.
    qint64 rewardEstimateLamports = 0;
    QString rewardRecipient;
    QString rewardId;
    QString rewardCondition; // accepted_report
    QString rewardStatus;    // pending_wallet | ready | submitted | paid | cancelled
    QString rewardTxSignature;
    QList<IssueEvent> events;

    QJsonObject toJson() const;
    static Issue fromJson(const QJsonObject &obj);
    bool isDeleted() const;
    bool hasUnauthorizedDeleteAttempt() const;
};

// Issue-level metadata that rides alongside a remote "open" submission. These
// fields aren't part of the open event's signature (which only covers
// title/body/attachments), so the owner applies them on merge as vouched data.
struct RemoteIssueMeta {
    QStringList labels;
    QString milestone;
    int priority = 0;
    QStringList assignees;
    // Human credited for the report when a bot transcribed the signed event.
    QString reporter;
    // A narrow, trusted intake may attach a pending issue-report estimate.
    qint64 rewardEstimateLamports = 0;
    QString rewardRecipient;
    QString rewardId;
    QString rewardCondition;
    QString rewardStatus;
    bool wantsAgent = false;
    QString wantsAgentModel;
    QString wantsAgentProvider;
};

// Raw bytes for one image a no-write-access node attached to a remote "open"
// or "comment" submission. Such a node has no working tree to copy the file
// into (see IssueStore::copyAttachments), so it hashes/names the file itself
// via readAttachmentsForRemoteSubmit() and ships the bytes alongside the
// signed event; applyRemoteEvent() writes them into the issue folder on
// merge, keyed by `name` (which already matches an entry in the event's
// signed IssueEvent::attachments list).
struct RemoteAttachment {
    QString name;
    QByteArray data;
};

struct IssueLabel {
    QString name;
    QString color;
};

struct IssueMilestone {
    QString title;
    qint64 due = 0;
    QString status = "open";
    QString description;
};

// One slug's worth of reactions on a single issue event, folded from the
// signed reaction events in the thread (and the delete events that take them
// back). `value` is a ReactionCatalog.h slug, so the desktop draws it rather
// than trusting a host emoji font.
struct IssueReaction {
    QString value;
    QStringList names; // display names that reacted, in the order they did
    bool mine = false; // the author/name pair asked about is one of them
};

// Source-backed task metadata for interactive issue descriptions. `line` is
// intentionally the Markdown line rather than a transient UI index: desktop
// and web can both turn a click or drop into one ordinary signed body edit.
struct IssueChecklistTask {
    int line = -1;
    int indent = 0;
    bool checked = false;
    QString label; // visible label, without the portable sub-issue suffix
    int subIssueNumber = 0;
};

// Another issue referencing this one by number, in its description or in a
// comment. Derived from signed text at read time rather than stored, so every
// issue that already mentions another shows up without rewriting history.
struct IssueMention {
    int sourceNumber = 0;
    QString sourceTitle;
    QString sourceStatus;
    QString author;
    QString authorName;
    qint64 ts = 0;
};

class IssueStore
{
public:
    IssueStore(QString workTreePath, QString mirrorPath,
               const ForkMeshIdentity *identity, QString authorName = QString());

    bool canWrite() const;

    QList<Issue> loadAll(QString *error = nullptr,
                         const std::function<void()> &tick = {}) const;
    QList<Issue> loadAllStrict(QString *error = nullptr) const;
    QList<Issue> loadAllStrictAtRef(const QString &ref,
                                    QString *error = nullptr) const;
    QList<IssueLabel> loadLabels() const;
    QList<IssueMilestone> loadMilestones() const;

    // A cheap content signature of the issue metadata subtree: its git tree oid
    // (plus the source path, so a different repo can't collide). Two calls return
    // an equal, non-empty value iff loadAll()/loadLabels()/loadMilestones() would
    // return the same data, so the UI can skip re-reading and rebuilding the issue
    // list when nothing changed. Returns empty when it can't be computed (no
    // source) — treat empty as "unknown" and don't skip. One fast `git rev-parse`,
    // no blob reads.
    QString contentSignature() const;

    int createIssue(const QString &title, const QString &body,
                    const QStringList &labels, const QString &milestone,
                    int priority,
                    const QStringList &assignees,
                    const QStringList &attachmentSrcPaths, QString *error = nullptr,
                    Issue *createdOut = nullptr);
    int createIssue(const QString &title, const QString &body,
                    const QStringList &labels, const QString &milestone,
                    int priority,
                    const QStringList &assignees,
                    const QStringList &attachmentSrcPaths,
                    const QStringList &attachmentPlaceholders,
                    QString *error = nullptr, Issue *createdOut = nullptr);
    // Turn every unlinked, unchecked Markdown task in the parent's effective
    // description into an issue. Child descriptions link back with
    // "Parent issue: #N" and the parent tasks gain "(sub-issue #N)", keeping
    // the relationship portable in the signed text rather than a local index.
    // Checked tasks and tasks already carrying that suffix are ignored.
    bool createSubIssues(int parentNumber, QList<int> *createdNumbers = nullptr,
                         QString *error = nullptr);
    // Convert exactly one task row (checked or unchecked) and keep its current
    // check state. This powers the row-level hover action without converting
    // every other unchecked task in the description.
    bool createSubIssue(int parentNumber, int taskLine,
                        int *createdNumber = nullptr,
                        QString *error = nullptr);
    static QStringList subIssueTitles(const QString &body);
    static QList<IssueChecklistTask> checklistTasks(const QString &body);
    // "Parent issue: #N" / "Depends on: #A, #B" lines in an opening
    // description, in first-seen order (capped at 25).
    static QList<int> dependencyNumbers(const QString &body);
    // The same body with those relationship lines blanked. Line count is kept
    // so checklist task line numbers still index the original description.
    static QString bodyWithoutDependencyLines(const QString &body);
    // The description after its latest signed edit.
    static QString effectiveBody(const Issue &issue);
    // Every bare #N in Markdown prose, first-seen order. Fenced and inline
    // code are skipped, as the reference autolinker skips them.
    static QList<int> referencedIssueNumbers(const QString &body);
    // Issues whose description names `parentNumber` on a relationship line,
    // plus rows the parent's own checklist links with "(sub-issue #N)".
    // Ascending by number; deleted issues are left out.
    static QList<int> subIssueNumbers(const QList<Issue> &issues,
                                      int parentNumber);
    // Other issues that reference `number` in a description or live comment,
    // one entry per source issue and author, oldest first.
    static QList<IssueMention> mentionsOf(const QList<Issue> &issues,
                                          int number);
    static QString setChecklistTaskChecked(const QString &body, int line,
                                           bool checked,
                                           bool *changed = nullptr);
    static QString reorderChecklistTask(const QString &body, int fromLine,
                                        int toLine, bool placeAfter,
                                        bool *changed = nullptr);
    bool addComment(int number, const QString &body,
                    const QStringList &attachmentSrcPaths, QString *error = nullptr);
    bool addComment(int number, const QString &body,
                    const QStringList &attachmentSrcPaths,
                    const QStringList &attachmentPlaceholders,
                    QString *error = nullptr);
    bool addVote(int number, QString *error = nullptr);
    // React to one event in the thread — a comment, or the open event for the
    // description. A reaction is an ordinary signed event, so it syncs and
    // folds like every other action; taking one back is a delete event naming
    // it, which is what keeps the log append-only. Setting a reaction that is
    // already in the wanted state writes nothing and returns true.
    bool setReaction(int number, const QString &targetEventId,
                     const QString &reaction, bool on, QString *error = nullptr);
    bool toggleReaction(int number, const QString &targetEventId,
                        const QString &reaction, QString *error = nullptr);
    // The live reactions on one event, in the order the slugs first appeared.
    // The display name is part of the identity here, not just the signing key:
    // one node signs for every model bot it speaks for, so "Sonnet 5 [bot]"
    // reacting must not toggle off what "Opus 5 [bot]" left on the same
    // comment.
    static QList<IssueReaction> reactionsForEvent(const Issue &issue,
                                                  const QString &targetEventId,
                                                  const QString &author,
                                                  const QString &authorName);
    bool editEvent(int number, const QString &eventId, const QString &newBody,
                   const QStringList &keepAttachments = {},
                   const QStringList &newAttachmentSrcPaths = {},
                   QString *error = nullptr);
    bool editEvent(int number, const QString &eventId, const QString &newBody,
                   const QStringList &keepAttachments,
                   const QStringList &newAttachmentSrcPaths,
                   const QStringList &newAttachmentPlaceholders,
                   QString *error = nullptr);
    // Rename an issue via a signed "title" event (folds into Issue::title).
    bool setTitle(int number, const QString &newTitle, QString *error = nullptr);
    bool setStatus(int number, const QString &status, QString *error = nullptr);
    bool setLabels(int number, const QStringList &labels, QString *error = nullptr);
    bool setMilestone(int number, const QString &milestone, QString *error = nullptr);
    bool setDates(int number, qint64 startDate, qint64 endDate,
                  QString *error = nullptr);
    bool setPriority(int number, int priority, QString *error = nullptr);
    bool setProgress(int number, int progress, QString *error = nullptr);
    bool setBounty(int number, double amountUsd, const QString &address,
                   const QString &status, QString *error = nullptr);
    // Record a settled contributor reward payment on the issue it paid for.
    // The reward estimate is owner-authored metadata rather than a signed
    // contributor event, so this writes the top-level fields directly — the
    // same shape the relay's ledger projects. Returns false with `error` set
    // when the issue is unreadable or the receipt belongs to another reward;
    // returns true without writing when the same signature is already there,
    // so a retried reconciliation is a no-op.
    //
    // `estimate` is the relay's own record of the reward (recipient, amount,
    // condition). It is only consulted when the issue carries no reward at
    // all: mirrors built before 2026-09-14 filed Discord reports without the
    // reward fields, so the paid estimate is adopted onto the issue — provided
    // it credits the person the issue is filed under.
    bool recordRewardPayment(int number, const QString &rewardId,
                             const QString &chainSignature,
                             QString *error = nullptr,
                             const RemoteIssueMeta *estimate = nullptr);
    bool setAssignees(int number, const QStringList &assignees, QString *error = nullptr);
    // Re-credit an issue to another person. The opening event keeps the key
    // that signed it; only the name the report is filed under moves, which is
    // what a proxied report (ForkBot intake, a Discord bug, an agent filing on
    // someone's behalf) gets wrong often enough to need fixing by hand.
    bool setReporter(int number, const QString &reporter, QString *error = nullptr);
    bool assignAgent(int number, const QString &provider, int sessionId,
                     bool createPr, const QString &status, QString *error = nullptr);
    bool deleteEvent(int number, const QString &eventId, QString *error = nullptr);
    // Append a creator-signed delete/self tombstone event. This is used for
    // portable signed deletion events; repository owners use removeIssue below.
    bool tombstoneIssue(int number, QString *error = nullptr);
    // Remove an issue from the current tree while leaving its prior commits
    // recoverable. This is the owning desktop's regular delete path; unlike a
    // creator tombstone, it remains authoritative when the issue was opened by
    // a browser or an older signing key.
    bool removeIssue(int number, QString *error = nullptr);
    bool deleteIssue(int number, QString *error = nullptr);

    bool saveLabels(const QList<IssueLabel> &labels, QString *error = nullptr);
    bool saveMilestones(const QList<IssueMilestone> &milestones, QString *error = nullptr);

    // Build a signed event with the node identity (also used by the relay-sync
    // path). Fills id/author/authorName/ts/sig.
    IssueEvent makeSignedEvent(int number, IssueEvent ev) const;

    // Merge a signature-bearing event received from the relay inbox into the
    // local issue store (used by cross-user sync). The event must already be
    // signed and verified by the caller. `attachments` carries the raw bytes
    // for any names listed in ev.attachments (a remote submitter has no
    // working tree to have copied them into already); each is written into
    // the issue folder before the event is committed.
    bool applyRemoteEvent(int number, const IssueEvent &ev, const QString &titleIfNew,
                          QString *error = nullptr,
                          const RemoteIssueMeta &meta = {},
                          const QList<RemoteAttachment> &attachments = {});

    // For a node with no working tree (relay/inbox submission path): read and
    // content-address each source file the same way copyAttachments() would
    // (sha256-prefix + extension), without touching disk. The returned names
    // (RemoteAttachment::name) are what to put in IssueEvent::attachments
    // before signing; the bytes ride along in the submission so the owner's
    // applyRemoteEvent() can materialize them on merge.
    static QList<RemoteAttachment> readAttachmentsForRemoteSubmit(
        const QStringList &srcPaths);

    static QString substituteAttachmentPlaceholders(
        QString body, const QStringList &srcPaths, const QStringList &placeholders,
        const QStringList &attachmentNames);

    // The exact bytes that an event's signature commits to. Public + static so
    // it can be unit-tested and kept byte-identical to the worker's verifier.
    static QByteArray canonicalString(int number, const IssueEvent &ev,
                                      const QString &repoKey = QString());
    void setSigningRepository(const QString &owner, const QString &name);
    static QString contentForSigning(const IssueEvent &ev);

    static QString issueDirPath(const QString &workTree, int number);

    // Who this store signs and shows as, for reactionsForEvent()'s `mine`.
    QString authorKey() const;
    QString authorName() const { return m_authorName; }

private:
    QString issuesDir() const;                 // <workTree>/.forkmesh/issues
    QString issueDir(int number) const;        // <workTree>/.forkmesh/issues/{open,closed}/<n>
    QString issueFilePath(int number) const;   // <issueDir>/issue-<n>.json
    void migrateLegacyLayout() const;
    bool readIssueFile(int number, Issue &out) const;
    bool writeIssueFile(const Issue &issue, QString *error) const;
    void recomputeMetadata(Issue &issue) const; // fold events into top-level fields
    QStringList copyAttachments(int number, const QStringList &srcPaths) const;
    // Write the raw bytes a remote submitter shipped alongside a signed event
    // into the issue folder, one file per name in `names` that has a matching
    // entry in `attachments` (materializing what copyAttachments() would have
    // written had the submitter had a working tree to copy from).
    void writeRemoteAttachmentFiles(int number, const QStringList &names,
                                    const QList<RemoteAttachment> &attachments) const;
    int nextNumber() const;
    bool commit(const QString &message, QString *error) const;

    QList<Issue> loadFromMirror(QString *error, bool strict = false,
                                const QString &refOverride = QString()) const;
    QByteArray showFromMirror(const QString &repoRelPath, bool *ok,
                              const QString &refOverride = QString()) const;
    QString mirrorRef() const;

    QString m_workTree;
    QString m_mirror;
    const ForkMeshIdentity *m_identity;
    QString m_authorName;
    QString m_signingRepoKey;

    // mirrorRef() is hit once per blob read from the mirror; cache it for the
    // store's lifetime so we resolve the ref via git at most once.
    mutable bool m_mirrorRefResolved = false;
    mutable QString m_cachedMirrorRef;
};
