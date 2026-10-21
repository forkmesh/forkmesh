#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

class ForkMeshIdentity;

// One entry in a pull request's append-only, signed conversation log. Stored as
// forkmesh/pulls/<N>/NNNN-<type>.md alongside pull.md, mirroring the issue event model.
// Signatures are raw Ed25519 over an explicit canonical string that the worker's
// verify_pull_comment_event reproduces byte-for-byte.
struct PullEvent {
    QString type;        // comment | review | line-comment
    QString id;
    QString author;      // signer pubkey (base64url)
    QString authorName;
    qint64 ts = 0;
    QString body;        // comment/review/line-comment body (markdown)
    QString state;       // review: approved | changes_requested | commented
    QString path;
    QString side;
    int line = 0;
    QString threadId;
    QString parentId;
    int lineStart = 0;
    int lineEnd = 0;
    QString suggestionPatch;
    QString targetPath;
    QString target;
    QString appliedCommit;
    // Patchset anchor for line-anchored events (line-comment, thread-comment).
    // `patchset` is the head commit the reviewer's diff was cut against (empty
    // when the client could not name one); `anchorLines` is the exact text of
    // the commented lines, diff prefix stripped and newline-joined. Both join
    // the signed content when present, so a thread carries verifiable proof of
    // which revision and which code it spoke about. Readers use them to keep
    // the thread on the same code after a re-push, or to mark it outdated when
    // that code is gone (see PullReviewModel.h).
    QString patchset;
    QString anchorLines;
    QString sig;

    bool hasAnchor() const { return !patchset.isEmpty() || !anchorLines.isEmpty(); }

    QJsonObject toJson() const; // wire format for the relay inbox
    static PullEvent fromJson(const QJsonObject &obj);
};

// What a reviewer's inline comment is pinned to beyond path/side/line: the
// patchset (head commit) their diff was cut against and the commented text.
struct PullAnchor {
    QString patchset;
    QString anchorLines;
    bool isEmpty() const { return patchset.isEmpty() && anchorLines.isEmpty(); }
};

// A pull request: a signed proposal to merge `head` into `base`, carrying a
// unified diff/patch. Lives in the repo's.forkmesh/pulls/ folder; submitted cross-node
// through the relay inbox (like issue submissions).
struct PullRequest {
    int number = 0;
    QString title;
    QString description;
    QString base;
    QString head;
    QString status = "open"; // open | merged | closed
    qint64 ts = 0;
    QString author;          // signer pubkey (base64url)
    QString authorName;
    QString sig;
    QString patch;           // unified diff (from changes.patch, or derived from refs)
    QString commits;         // from commits.mbox, or derived from refs
    // Branch-backed PRs keep their diff out of the repo entirely: the head branch
    // ref already carries every commit (full author history), so nothing but this
    // signed pull.md pointer is committed and `patch`/`commits` are reconstructed
    // from base..head on demand. Working-tree/imported/cross-node PRs are not
    // branch-backed and persist a portable patch/mbox alongside pull.md.
    bool branchBacked = false;
    // Immutable commits captured when a branch-backed pull is signed. Strict
    // readers reconstruct the signed patch and mbox from these OIDs instead of
    // following the mutable base/head branch names.
    QString creationBaseOid;
    QString creationHeadOid;
    int revision = 0;
    QString headBaseOid;
    QString headOid;
    QString mergeBase;
    QString mergeHead;
    qint64 mergedAt = 0;
    QString mergedBy;
    QString coAuthors;
    // Which tool opened this pull, and that tool's own signature over the
    // pull's identity. Written only by the ForkMesh MCP agent tool
    // (tools/forkmesh_mcp_server.py) and never interpreted here — the desktop
    // just carries both fields through every rewrite so merging a pull does
    // not erase the marker the microjob payout check reads.
    QString client;
    QString clientSig;
    QString aiModel;
    // Owner-authored merge reward estimate. It is intentionally outside the
    // contributor's immutable PR signature, like status and merge metadata.
    qint64 rewardEstimateLamports = 0;
    QString rewardRecipient;
    QString rewardId;
    QString rewardCondition; // merged
    QString rewardStatus;    // pending_merge | pending_wallet | ready | submitted | paid | cancelled
    QString rewardTxSignature;
    int filesChanged = 0;
    int additions = 0;
    int deletions = 0;
    QList<PullEvent> events; // conversation log (comments + reviews)
    // Transient: the head commit `patch` was cut against on this read (the
    // resolved head for a derived branch-backed pull, else the signed creation
    // head). It names the patchset new review threads anchor to and the one
    // existing threads are placed against. Never serialized or signed.
    QString patchHeadOid;
    // Transient UI provenance: this record came from the repository's code
    // branch rather than the current forkmesh/pulls ledger. Older ForkMesh
    // versions stored PRs there. It is never serialized or signed.
    bool archivedOnCodeBranch = false;

    QJsonObject toJson() const; // wire format for the relay inbox (includes patch)
    static PullRequest fromJson(const QJsonObject &obj);

    QString reviewSummary() const;
    int independentApprovalCount() const;
    // Reviewers other than this pull request's author whose latest review still
    // asks for changes, named the way the timeline names them. The merge report
    // lists them; hasIndependentChangesRequested() is the same answer as a bool.
    QStringList independentChangesRequestedBy() const;
    bool hasIndependentChangesRequested() const;
    bool independentReviewGateSatisfied() const;
};

// The status the desktop paints for a pull request. A code-branch-only record
// is historical until it is restored to the active pull ledger. Never paint
// that archived record as active work just because its old immutable metadata
// still says "open".
inline QString pullDisplayStatus(const PullRequest &pull)
{
    if (pull.archivedOnCodeBranch)
        return QStringLiteral("closed");
    if (pull.status == QLatin1String("closed") ||
        pull.status == QLatin1String("merged"))
        return pull.status;
    return QStringLiteral("open");
}

// The repository's peer-review setting is a default for contributors, not a
// lock on its owner. Keep the identity and gate calculation independent of the
// widgets so every desktop merge entry point makes the same decision.
namespace PullMergePolicy {

bool repositoryOwnedByAccount(const QString &repositoryOwner,
                              const QString &account,
                              const QStringList &linkedNodes = {});
// An organization name is the public owner of every repository it fronts.
// Its owner role carries the same review-waiver authority as a direct
// repository owner; admins and ordinary members deliberately do not.
bool repositoryOwnedByOrganizationOwner(const QString &repositoryOwner,
                                        const QJsonArray &organizations);
bool reviewGateBlocks(bool requirePeerReview, bool reviewReady,
                      bool repositoryOwner);
bool ownerOverrideRequired(bool requirePeerReview, bool reviewReady,
                           bool repositoryOwner);

} // namespace PullMergePolicy

struct PullReviewCheckout {
    QString path;             // detached linked worktree holding the change
    QString baseOid;          // commit the submission was replayed onto
    QString headOid;          // tip of the review copy
    int commitCount = 0;      // commits the submission adds over the base
    QString patch;            // base..head diff, for the in-app review view
    bool uncommitted = false;
    QStringList conflicts;    // paths that did not apply cleanly (if any)
};

// Who a repository already trusts to run code in it. Actions queued for a pull
// request execute the contributor's own shell commands on this node, so a
// first-time author's runs are held for a maintainer's approval — one approval
// per pull request — while a contributor who has already landed work here is
// treated like any other push.
namespace PullAuthorTrust {

// Stable per-author key: the signer's public key when the pull carries one,
// otherwise the (lowercased) display name. Empty when neither is known, which
// never counts as established.
QString identityOf(const PullRequest &pr);

// `trusted` holds identities that are established by definition — this node's
// own signing key and the repository owner's name. Everyone else has to have a
// merged pull request in `history` under the same identity.
bool isEstablished(const PullRequest &pr, const QList<PullRequest> &history,
                   const QStringList &trusted);

int pullOrdinal(const PullRequest &pr, const QList<PullRequest> &history);

QString ordinal(int n);

} // namespace PullAuthorTrust

// The owner-authored reward estimate a pull carries. The estimate itself is
// written by the repository owner (it sits outside the contributor's
// signature, like status and merge metadata); this namespace only reads it and
// records the one transition the pull itself decides — a merge-conditional
// estimate stops waiting once the pull lands.
namespace PullReward {

// "0.004 SOL" for a lamport estimate. Empty for a pull with no estimate.
QString amountText(qint64 lamports);

// Reader-facing state of the estimate: "pays on merge", "awaiting the
// recipient's wallet", "ready to pay", "payment submitted", "paid",
// "cancelled". Empty for a pull with no estimate.
QString statusText(const PullRequest &pr);

// Move a merge-conditional estimate out of pending_merge, which is the only
// thing that puts it in the payout queue the relay and the ledger read. Every
// path that records a merge calls this, so a merged pull never keeps
// advertising a reward that is still waiting for the merge. Returns true when
// `pr` changed.
bool advanceForMerge(PullRequest &pr);

} // namespace PullReward

class PullStore
{
public:
    PullStore(QString workTreePath, QString mirrorPath,
              const ForkMeshIdentity *identity, QString authorName = QString());

    bool canWrite() const; // has a working tree (can author/merge)

    QList<PullRequest> loadAll(QString *error = nullptr) const;
    // Include PR records left on the repository's code branch by older
    // ForkMesh versions. Current-ledger records win when the same number is in
    // both places; code-branch-only records are marked archivedOnCodeBranch.
    QList<PullRequest> loadAllIncludingArchived(QString *error = nullptr) const;
    // Copy one code-branch-only record into the current pull ledger so normal
    // review, close, merge, and delete actions can manage it.
    bool restoreArchivedPull(int number, QString *error = nullptr);
    QList<PullRequest> loadAllStrict(QString *error = nullptr) const;
    QList<PullRequest> loadAllStrictAtRef(const QString &ref,
                                          QString *error = nullptr) const;

    // Owner-side: create a PR locally from an already-computed diff. `commits` is
    // the optional format-patch mbox (base..head) used to preserve authorship on
    // merge; pass an empty string for working-tree/imported patches. Set
    // `branchBacked` when `head` is a real branch whose committed range (base..head)
    // *is* the change: the PR then stores only its signed pointer and reconstructs
    // the diff/commits from the synced ref, so no diff text is committed to the
    // repo (the passed patch/commits are recomputed from the refs before signing).
    int createPull(const QString &title, const QString &description,
                   const QString &base, const QString &head, const QString &patch,
                   const QString &commits, bool branchBacked = false,
                   QString *error = nullptr);
    bool setStatus(int number, const QString &status, QString *error = nullptr);
    // Attach the finalized public transaction that settled this pull's reward
    // estimate. The relay's own record has already moved to paid by the time
    // this is called; writing it here is what makes the pull itself say which
    // transfer paid it, instead of leaving that only in the relay's ledger.
    // Returns false with `error` set when the pull is unreadable, the receipt
    // does not belong to it, the estimate was cancelled, or a different
    // transaction is already recorded; returns true and leaves the tree alone
    // when the same signature is already there, so a retried reconciliation
    // is a no-op.
    bool recordRewardPayment(int number, const QString &rewardId,
                             const QString &chainSignature,
                             QString *error = nullptr);
    bool isBranchBehindBase(int number, bool *behind, QString *error = nullptr,
                            int *behindCount = nullptr) const;
    bool updateBranchFromBase(int number, QString *error = nullptr);
    // ownerReviewOverride may be true only after the caller has verified the
    // repository owner and obtained an explicit confirmation. When it actually
    // waives an unmet required-review gate, the ledger commit records that fact.
    bool mergePull(int number, QString *error = nullptr,
                   bool requirePeerReview = true,
                   bool ownerReviewOverride = false);
    // Ledger convergence: adopt pulls that another node (a mirror that won the
    // inbox lease while its ledger checkout was behind) committed to the
    // served forkmesh/pulls branch. Cherry-picks foreign commits that purely
    // add pull directories this ledger has never carried. Returns the adopted
    // pull numbers. The network fetch lives in prefetchForeignLedger so a
    // caller can run it off the GUI thread; pass fetchFirst=false after a
    // successful prefetch to adopt from the existing FETCH_HEAD.
    QList<int> adoptForeignPulls(const QString &remoteUrl,
                                 QString *error = nullptr,
                                 bool fetchFirst = true);
    // The only network step of adoptForeignPulls: fetch the served ledger tip
    // into the meta worktree's FETCH_HEAD. Touches nothing else, so it is
    // safe to run on a worker thread while the GUI keeps writing pulls.
    bool prefetchForeignLedger(const QString &remoteUrl,
                               QString *error = nullptr);
    // Record reality for open branch-backed pulls whose resolved head commit
    // is already an ancestor of the pull's base (or of main, for a stacked
    // pull whose base merged first): flip them to merged in the ledger
    // without touching the repository. Returns the flipped pull numbers.
    // `rewardsAdvanced` collects the pulls whose merge-conditional reward
    // estimate left pending_merge — the ones flipped here, plus already-merged
    // pulls whose estimate a desktop recorded the merge without updating.
    QList<int> reconcileMergedPulls(QString *error = nullptr,
                                    QList<int> *rewardsAdvanced = nullptr);
    bool checkMergeable(int number, bool *clean,
                        QStringList *conflictFiles = nullptr,
                        QString *error = nullptr,
                        bool keepGuiAlive = false) const;
    QString baseTip() const;

    bool startConflictMerge(int number, QStringList *conflicted,
                            bool *resolvedClean, QString *error = nullptr);
    bool startConflictAgentEdit(int number, QStringList *conflicted,
                                bool *resolvedClean,
                                QString *error = nullptr);
    bool finishConflictMerge(int number, QString *error = nullptr);
    void abortConflictMerge();
    bool conflictMergeInProgress() const;

    bool startPullFileEdit(int number, const QString &relPath, QString *content,
                           QString *error = nullptr);
    bool finishPullFileEdit(int number, const QString &relPath,
                            const QString &content, QString *error = nullptr);
    bool deletePullFile(int number, const QString &relPath, QString *error = nullptr);
    bool startPullAgentEdit(int number, QString *error = nullptr);
    bool finishPullAgentEdit(int number, const QString &commitMsg,
                             QString *error = nullptr);
    QString agentEditWorkTree() const;
    static QString syntheticMbox(const PullRequest &pr);

    // Merge a signed PR received from the relay inbox into.forkmesh/pulls/.
    bool applyRemotePull(const PullRequest &pr, QString *error = nullptr,
                         bool *opened = nullptr);
    bool applyRemoteRevision(int number, const PullRequest &revision,
                             const QString &revisionSig, QString *error = nullptr);

    bool checkoutForReview(const PullRequest &pr, const QString &dir,
                           PullReviewCheckout *out, QString *error = nullptr) const;
    void discardReviewCheckout(const QString &dir) const;
    bool deletePull(int number, bool rewriteHistory, QString *error = nullptr);
    // Bulk form of deletePull for "Delete closed and merged": every record
    // goes in one ledger commit (and, when asked, one history rewrite) instead
    // of a commit, ref teardown and rewrite per pull. Records already absent
    // get the same ref teardown deletePull gives them and are reported in
    // *missing rather than failing the batch; *deleted lists the numbers whose
    // record this call removed.
    bool deletePulls(const QList<int> &numbers, bool rewriteHistory,
                     QList<int> *deleted = nullptr, QList<int> *missing = nullptr,
                     QString *error = nullptr);
    // The message every "this node holds no such pull request" failure
    // reports, and the test for it. A caller tearing a PR down has already
    // reached its goal when it sees this one, so it treats it as success and
    // finishes the rest of the cleanup instead of stopping on it.
    static QString missingPullMessage(int number);
    static bool missingPullError(const QString &error, int number);

    // Conversation: append a signed comment or review event, then commit. The
    // review state is one of approved | changes_requested | commented.
    bool addComment(int number, const QString &body, QString *error = nullptr);
    bool addReview(int number, const QString &state, const QString &body,
                   QString *error = nullptr);
    // Append a signed comment anchored to a specific file + line of the diff.
    bool addLineComment(int number, const QString &path, const QString &side,
                        int line, const QString &body, QString *error = nullptr);
    bool addLineComment(int number, const QString &path, const QString &side,
                        int line, const QString &body, const PullAnchor &anchor,
                        QString *error = nullptr);
    bool addThreadComment(int number, const QString &path, const QString &side,
                          int lineStart, int lineEnd, const QString &body,
                          const QString &suggestionPatch = QString(),
                          QString *error = nullptr);
    bool addThreadComment(int number, const QString &path, const QString &side,
                          int lineStart, int lineEnd, const QString &body,
                          const QString &suggestionPatch, const PullAnchor &anchor,
                          QString *error = nullptr);
    bool addThreadReply(int number, const QString &threadId,
                        const QString &parentId, const QString &body,
                        QString *error = nullptr);
    bool setThreadState(int number, const QString &threadId, const QString &state,
                        const QString &body = QString(),
                        QString *error = nullptr);
    bool setSuggestionState(int number, const QString &threadId,
                            const QString &state,
                            const QString &appliedCommit = QString(),
                            const QString &body = QString(),
                            QString *error = nullptr);
    // Merge a signed event received from the relay inbox into.forkmesh/pulls/<N>/. The
    // event must already be signed and verified by the caller.
    bool applyRemoteEvent(int number, const PullEvent &ev, QString *error = nullptr);
    bool editComment(int number, const QString &target, const QString &body,
                     QString *error = nullptr);
    bool deleteComment(int number, const QString &target, QString *error = nullptr);
    PullEvent makeCommentChange(int number, const QString &target,
                                const QString &body, bool remove) const;
    static bool isCommentChangeEvent(const PullEvent &ev);
    static bool isEditableComment(const PullEvent &ev);
    static bool commentChangeApplies(const PullEvent &change, const PullEvent &target);
    static QList<PullEvent> foldCommentChanges(const QList<PullEvent> &events,
                                               QSet<QString> *edited = nullptr);
    bool authoredByThisNode(const PullEvent &ev) const;
    static bool isLifecycleEvent(const PullEvent &ev);
    bool applyRemoteLifecycleEvent(int number, const PullEvent &ev, QString *error = nullptr);

    static QString branchReviewKey(const QString &branch);
    QString branchReviewDir(const QString &branch) const;
    QList<PullEvent> readBranchReviewEvents(const QString &branch) const;
    PullEvent makeSignedBranchEvent(const QString &branch, PullEvent ev) const;
    static QByteArray branchCanonicalString(const QString &branch,
                                            const PullEvent &ev,
                                            const QString &repoKey = QString());
    bool addBranchThreadComment(const QString &branch, const QString &path,
                                const QString &side, int lineStart, int lineEnd,
                                const QString &body, const PullAnchor &anchor,
                                QString *error = nullptr);
    bool addBranchThreadReply(const QString &branch, const QString &threadId,
                              const QString &parentId, const QString &body,
                              QString *error = nullptr);
    bool setBranchThreadState(const QString &branch, const QString &threadId,
                              const QString &state, QString *error = nullptr);
    static bool validReviewBranchName(const QString &branch);
    bool applyRemoteBranchEvent(const QString &branch, const PullEvent &ev,
                                QString *error = nullptr);

    // Sign a conversation event with the node identity (fills id/author/
    // authorName/ts/sig). The signature binds the PR number — reviewers act on
    // the owner's synced mirror, which carries canonical numbers.
    PullEvent makeSignedEvent(int number, PullEvent ev) const;
    static QByteArray canonicalString(int number, const PullEvent &ev,
                                      const QString &repoKey = QString());
    void setSigningRepository(const QString &owner, const QString &name);
    static QString contentForSigning(const PullEvent &ev);

    // Sign a PR with the node identity (fills author/authorName/ts/sig). The
    // signature commits to title/base/head/patch (not the number, which the
    // owner assigns on merge).
    PullRequest makeSignedPull(PullRequest pr) const;
    static QByteArray canonicalString(const PullRequest &pr,
                                      const QString &repoKey = QString());
    static QByteArray legacyCanonicalString(const PullRequest &pr);

    static void computeStats(PullRequest &pr);

    // Give a pull request a commit to check out. Branch-backed pulls already
    // have one; a submission that arrived as a signed patch has nothing until
    // its commits are replayed onto the base, which is what this does — writing
    // refs/pr/<n>/head and the durable pr/<n> branch. Best effort and
    // idempotent: it returns true having changed nothing when the replay does
    // not apply. Expensive (a scratch worktree and a `git am`), so callers
    // reach for it only when no ref resolves.
    bool ensurePullHeadRef(const PullRequest &pr, QString *error = nullptr) const
    {
        return materializePullRef(pr, error);
    }

    enum class BranchSource { Local, Mirror, Pull, Missing };
    BranchSource ensureLocalBranch(const QString &branch, int *pullNumber = nullptr,
                                   QString *error = nullptr) const;

    QString metaWorkTree() const;

private:
    bool beginPullBranch(int number, QStringList *conflicted, bool *cleanApply,
                         QString *error);
    bool finalizeOnPullBranch(int number, const QString &commitMsg,
                              QString *error);
    bool beginPullEditWorkTree(int number, QString *error);
    bool beginPullConflictWorkTree(int number, QStringList *conflicted,
                                   bool *cleanApply, QString *error);
    void discardPullEditWorkTree();
    bool moveBranchToEditTip(QString *error);

    QString pullsDir() const;
    QString pullDir(int number) const;
    bool materializePullRef(const PullRequest &pr, QString *error) const;
    QString pullBaseTip(const PullRequest &pr) const;
    QString pullHeadCommit(const PullRequest &pr) const;
    // Materialize the PR's metadata on its independent
    // forkmesh/pull/<n> branch. The aggregate forkmesh/pulls ref remains only
    // as a compatibility index for older peers; refs/pr/<n>/metadata points at
    // the same isolated commit.
    bool materializePullMetadataRef(int number, QString *error) const;
    // Drop every ref materializePullRef()/materializePullMetadataRef() create
    // for this PR. loadAll() treats forkmesh/pull/<n> as authoritative over the
    // aggregate index, so a leftover metadata branch keeps a deleted PR in the
    // list; this runs even when the ledger record is already gone.
    void removeMaterializedPullRefs(int number, bool keepTombstone = true) const;
    bool purgePullPayloadHistory(int number, QString *error) const;
    int nextNumber() const;
    bool writePull(const PullRequest &pr, QString *error) const;
    bool readPull(int number, PullRequest &out) const;
    QList<PullEvent> readEvents(int number) const;
    int nextEventIndex(int number) const;
    bool writeEventFile(int number, int index, const PullEvent &ev,
                        QString *error) const;
    bool appendEvent(int number, const PullEvent &ev, const QString &commitMsg,
                     QString *error);
    bool appendBranchEvent(const QString &branch, const PullEvent &ev,
                           const QString &commitMsg, QString *error);
    bool commit(const QString &message, QString *error) const;
    QString mirrorRef() const;
    QByteArray showFromMirror(const QString &repoRelPath, bool *ok,
                              const QString &refOverride = QString()) const;
    QList<PullRequest> loadFromMirror(
        QString *error, bool strict = false,
        const QString &refOverride = QString()) const;

    QString m_workTree;
    QString m_mirror;
    const ForkMeshIdentity *m_identity;
    QString m_authorName;
    QString m_signingRepoKey;

    mutable QString m_metaWorkTreeCache;

    QString m_amBranch;
    QString m_amBase;
    QString m_amRestoreRef;
    QString m_editWorkTree;
};
