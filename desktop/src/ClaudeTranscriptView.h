#pragma once

#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QPixmap>
#include <QPointer>
#include <QScrollArea>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include "ClaudeTranscriptTools.h"
#include "TranscriptCoverage.h"

#include <functional>

class QToolButton;
class QVBoxLayout;
class QWidget;
class QLabel;
class QPropertyAnimation;
class QResizeEvent;
class CacheLabel;
class Collapsible;
class ScrollJumpButtons;
class PromptDeckBar;

class ClaudeTranscriptView : public QScrollArea
{
    Q_OBJECT
public:
    explicit ClaudeTranscriptView(QWidget *parent = nullptr);

    void handleEvent(const QJsonObject &ev, bool countStats = true);
    // `at` stamps the speaker's caption; an invalid time leaves it name-only.
    void addUserTurn(const QString &text, const QDateTime &at = QDateTime());
    void clear();

    // When a transcript event was said: the host's own receive stamp ("_ts",
    // ms since epoch) or a Claude session file's ISO "timestamp". Invalid when
    // the event carries neither (transcripts stored before stamping).
    static QDateTime eventTime(const QJsonObject &ev);

    struct Palette {
        QString canvas, surface, border, text, muted, accent, add, del, addBg,
            delBg, userBg, aiBg;
        // Chat bubbles and the terminal panel are their own tints: a bubble has
        // to lift off the canvas without becoming a box, and a command panel has
        // to read as a screen rather than as one more card.
        QString aiEdge, userEdge, termBg, termEdge, chipBg;
        // Bash tokens use Primer-like colors with separately tuned light/dark
        // values; paths double as clickable file references.
        QString shellCommand, shellKeyword, shellString, shellVariable,
            shellOperator, shellComment, shellPath;
    };

    // Whether a shell command may run without the user: worked out by the host
    // from the run's permission mode and the project's agent access, since the
    // view knows neither. Drawn as a chip on the command's prompt line.
    struct CommandApproval {
        enum State { Unknown, Allowed, Asks, Blocked, Denied };
        State state = Unknown;
        QString label;  // "Allowed", "Asks first", "Blocked", "Denied"
        QString reason; // one sentence naming the rule that decided it
    };
    using CommandApprovalResolver = std::function<CommandApproval(
        const QString &command, const QString &cwd)>;
    // Every command the transcript shows, oldest first, with its chip's state.
    struct CommandApprovalRow {
        QString toolUseId;
        QString command;
        QString cwd;
        CommandApproval approval;
        bool finished = false;
        bool failed = false;
    };

    enum RowGlyph { GlyphDot, GlyphChevron, GlyphStar, GlyphNone };

    // How one transcript row decorates its gutter and its content column. A
    // plain row just marks the gutter with a glyph; a speaker's row wears a
    // round portrait there, set on its plate colour.
    struct RowStyle {
        QString nodeColor; // glyph colour
        RowGlyph glyph = GlyphDot;
        QString plate;
        QPixmap avatar;    // gutter portrait; replaces the glyph
        QString avatarRing;// ring drawn around the portrait
        QString monogram;  // fallback when there is no portrait
    };

signals:
    void statsChanged(qint64 tokens, double costUsd);
    // The user answered an AskUserQuestion clarifying-question card. toolUseId is
    // the pending tool_use to satisfy; answer is the assembled reply text. The
    // host sends it back to the CLI, while sensitive keeps secrets out of the
    // persisted transcript.
    void questionAnswered(const QString &toolUseId, const QString &answer,
                          bool sensitive);
    void inlineChoiceAnswered(const QString &answer);
    void loadEarlierRequested();
    // The command stays verbatim here even though its card is rendered on one
    // line. The host opens a separate native terminal in the recorded cwd.
    void commandRunRequested(const QString &command, const QString &cwd);
    // The approval chip on a command was clicked: show the run's commands and
    // the permissions that apply to them.
    void commandApprovalActivated(const QString &toolUseId);
    void rawLogRequested(const QJsonObject &event);

protected:
    bool event(QEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

public:
    void scrollToTop();
    void scrollToBottom();
    void jumpToBottom();
    void setSplitDiffs(bool on);

    static QString failureReason(const QJsonObject &ev);
    static bool resultIsError(const QJsonObject &ev);

    void setCodexStyle(bool on);
    // The transcript has inline colours on its cards, so it needs the active
    // application theme explicitly when the user changes appearance while a
    // run is open.
    void setTheme(bool dark);
    // Keep command output in a terminal-like viewport that follows streamed
    // output and never grows beyond five visible rows. Turning this off keeps
    // the older expandable transcript presentation.
    void setTerminalOutputMode(bool on) { m_terminalOutputMode = on; }

    // Without a resolver commands carry no approval chip. Set it before the
    // session's events are replayed; refreshCommandApprovals() re-asks it for
    // every command already drawn (the project's policy changed).
    void setCommandApprovalResolver(CommandApprovalResolver resolver);
    void refreshCommandApprovals();
    QVector<CommandApprovalRow> commandApprovals() const;
    // A tool result that is the permission layer refusing the call, not the
    // command failing: the gate's or Claude Code's own denial text.
    static bool isPermissionDenial(const QString &resultText);

    // Host-supplied context for the "session started" divider: the
    // worktree branch the run works on, the permission mode it was launched
    // under ("Auto" / "Plan" / …), its reasoning strength ("high" / "low") and
    // the name of the provider account it is logged in as — the
    // CLI never says which of the configured logins is spending the tokens.
    // Only the CLI's own model and cwd ride in the init event, so the host sets
    // this from the AgentSession before the session's events are replayed or
    // streamed. Empty parts are omitted; an empty mode falls back to the
    // permissionMode the event itself reports (surfaced external sessions).
    void setSessionContext(const QString &branch, const QString &mode,
                           const QString &strength,
                           const QString &account = QString());

    // The portraits worn by the two speakers' chat bubbles: the running model's
    // own face, and the signed-in user's picture. The view can work out neither
    // — the host owns the provider/model → portrait mapping and the account
    // avatar — so until this is called each bubble falls back to a lettered
    // chip. Names ride along because they caption the bubble ("Opus 5" / "You").
    void setSpeakerAvatars(const QPixmap &agent, const QString &agentName,
                           const QPixmap &user, const QString &userName);

    // The CLI closes a turn with a green "✓ Done" row the moment it stops
    // talking — even when the build or test the agent left running in the
    // background is still going. The host feeds that state in here, and while a
    // note is set the row reads "⟳ Finishing …" with what is still running
    // appended. An empty note restores the plain "Done".
    void setBackgroundWorkNote(const QString &note);

    // A whole batch of rows is about to land (a stored session replayed, a page
    // of earlier events prepended) rather than one row streaming in. Besides
    // skipping the per-row fade-in, this holds the column's layout and keeps it
    // off screen until the batch is built — see beginBatchBuild().
    void setBulkPopulate(bool on);
    void setBatchHoldsFrame(bool hold, bool rowsLandAbove = false);
    void accumulateStatsOnly(const QJsonObject &ev);
    void addSkippedNotice(int count);
    void prependEarlierEvents(const QList<QJsonObject> &events, int stillSkipped);

    void noteRawLog(const QString &raw);
    void noteRawLine(const QString &line);
    const QVector<forkmesh::transcriptcoverage::Gap> &coverageGaps() const
    {
        return m_coverage.gaps();
    }

    int search(const QString &query);
    void searchNext();
    void searchPrev();
    void clearSearch();

    static bool parseInlineChoices(const QString &markdown, QStringList &options);

    static QString linkifyReferences(const QString &markdown);

#ifdef FORKMESH_WINDOW_TESTS
    int pendingLabelRelayoutsForTest() const;
    bool followsBottomForTest() const { return m_stickBottom; }
    void leaveBottomForTest() { m_stickBottom = false; }
    CommandApproval resolveCommandApprovalForTest(const QString &command,
                                                  const QString &cwd) const
    {
        return m_approvalResolver ? m_approvalResolver(command, cwd) : CommandApproval{};
    }
#endif

signals:
    void searchResultsChanged(int current, int total);
    void referenceActivated(const QString &href);
    void coverageGapsActivated();

private:
    friend class CacheLabel;
    struct ToolCard; // defined among the members below
    void queueLabelRelayout(CacheLabel *label);
    void processLabelRelayouts();

    void applyScheme(bool dark);
    void layoutPromptDeck();
    void noteCoverage(const QJsonObject &ev);
    void refreshCoverageButton();
    void layoutCoverageButton();
    QString coverageToolTip() const;
    QWidget *addRow(QWidget *card, const RowStyle &style);
    QWidget *addRow(QWidget *card, const QString &nodeColor = QString(),
                    RowGlyph glyph = GlyphDot);
    // The speaker rows: the agent's own prose and the user's turns. Both are the
    // parts of a transcript worth finding at a glance, so they are the only rows
    // that get a portrait and a bubble.
    QWidget *addAgentRow(QWidget *card);
    QWidget *addUserRow(QWidget *card);
    RowStyle speakerStyle(bool user) const;
    void ensureActivity();
    void clearActivity();
    void cycleActivityWord();
    void smoothScrollTo(int value);
    void beginBatchBuild();
    void endBatchBuild();
    void retireRow(QWidget *row);
    void deleteRetiredRows();
    void releaseHeldFrame();
    void fadeIn(QWidget *card);
    QString accentFor(const QString &toolName) const;
    void enableReferenceLinks(QLabel *label);
    QString referenceHtml(const QString &plainText) const;

    Collapsible *makeCollapsible(const QString &header, QWidget *body,
                                 bool expanded);
    void addAssistantBlocks(const QJsonObject &message);
    bool addAssistantText(const QString &markdown);
    // The speaker's portrait and small "Opus 5" / "You" name above their text.
    // A valid `at` adds the full date and 12-hour time beside the name.
    QWidget *speakerCaption(bool user, const QDateTime &at);
    void stampSpeakerCaption(QWidget *row, const QDateTime &at);
    QString fileEditSubtitleHtml(const QStringList &paths,
                                 const forkmesh::transcripttools::EditStats &stats) const;
    QWidget *dotHeader(const QString &name, const QString &subtitle,
                       bool bareSubtitle = false, bool html = false);
    QWidget *connectorRow(QWidget *content, const QString &glyph = QString());
    QWidget *makeMono(const QString &text, bool collapsedIfLong);

    void ensureLiveThinking();
    void setThinkingTokens(int tokens);
    void appendThinkingDelta(const QString &text);
    void finalizeThinking(const QString &fullText);

    void addToolUse(const QString &id, const QString &name,
                    const QJsonObject &input);
    void addToolResult(const QString &id, const QString &text, bool isError);
    void appendToolOutput(const QString &id, const QString &text);
    void appendAgentText(const QString &id, const QString &text);
    void completeAgentText(const QString &id, const QString &text);
    void addResult(const QJsonObject &ev);
    void applyBackgroundWorkNote();
    void addNotice(const QString &text, bool error = false);
    void handleClaudeStreamEvent(const QJsonObject &event);
    bool completeStreamedText(const QString &messageId, const QString &text);
    void dropLiveAgentText(const QString &key);
    void noteSubagentActivity(const QString &parentId, const QJsonObject &message);
    void showAgentReport(ToolCard &tc, const QString &report, const QString &status);
    bool addAgentTaskNotification(const QString &toolUseId, const QString &status,
                                  const QString &report);
    void addTaskNotificationText(const QString &text);
    bool addWindowResult(ToolCard &tc, const QString &text);

    void addAskUserQuestion(const QString &id, const QJsonObject &input);
    void markAskAnswered(const QString &id, const QString &answer);

    QWidget *addInlineChoices(const QStringList &options);
    void lockInlineChoices(QWidget *box);

    // A shell command and everything it printed, drawn as one terminal panel:
    // the ❯ prompt line on top, a hairline, then the output. Both CLI styles go
    // through it, so a Bash turn looks the same whoever ran it.
    QWidget *commandPanel(const QString &command, const QString &cwd,
                          ToolCard &tc);
    QVBoxLayout *commandOutputColumn(ToolCard &tc);
    void addCommandOutput(ToolCard &tc, const QString &text, bool isError);
    bool addCodexExplore(const QString &id, const QString &name,
                         const QJsonObject &input);
    void addCommand(const QString &id, const QString &command,
                    const QString &description, const QString &cwd);
    QString commandHtml(const QString &line) const;
    CommandApproval commandApprovalFor(const QString &toolUseId) const;
    void paintCommandApproval(QWidget *header,
                              const CommandApproval &approval) const;
    void settleCommandApproval(const QString &id, const QString &resultText,
                               bool isError);

    QString toolSubtitle(const QString &name, const QJsonObject &input) const;
    QWidget *makeMarkdown(const QString &markdown);
    QWidget *toolBody(const QString &name, const QJsonObject &input);
    QWidget *makeDiff(const QString &oldText, const QString &newText);
    QWidget *makeCode(const QString &text, bool collapsedIfLong = false);

    Palette m_p;
    QWidget *m_container = nullptr;
    QVBoxLayout *m_col = nullptr;
    QWidget *m_bottomSpacer = nullptr; // small tail so the last row isn't flush
    bool m_stickBottom = true;
    ScrollJumpButtons *m_jumpButtons = nullptr; // floating ▲/▼ jump corner
    PromptDeckBar *m_promptDeck = nullptr;
    QToolButton *m_coverageButton = nullptr;
    forkmesh::transcriptcoverage::Tracker m_coverage;
    bool m_splitDiffs = false;            // side-by-side vs unified diffs
    QWidget *m_activity = nullptr;        // live "what it's doing" ticker row
    QLabel *m_activityLabel = nullptr;
    QTimer *m_activityTimer = nullptr;
    qint64 m_activityStartMs = 0;         // Codex ticks elapsed, not gerunds

    bool m_codexStyle = false;
    bool m_terminalOutputMode = true;
    QPointer<QWidget> m_exploreBlock;
    QVBoxLayout *m_exploreLines = nullptr;
    bool m_keepExplore = false; // addRow() guard: this row IS the block/ticker

    int m_prependAt = -1;
    QPointer<QWidget> m_skippedNotice; // the "Load N earlier events" row, if any
    int m_skippedCount = 0;
    bool m_loadEarlierPending = false; // a request is in flight; don't re-emit

    struct ToolCard {
        QVBoxLayout *io = nullptr;   // column: header, body, then result rows
        bool hasResult = false;      // a result row was appended
        bool isCommand = false;      // a shell command with a terminal panel
        QPointer<QLabel> exploreLine;
        bool summarizeResult = false;
        bool suppressResult = false;
        bool quietSuccess = false;
        QDateTime startedAt;
        QPointer<QLabel> liveOutput; // incrementally streamed command output
        QPointer<QScrollArea> terminalOutput; // bounded, auto-following output
        QString liveOutputText;
        QVBoxLayout *panelOutput = nullptr; // see commandOutputColumn()
        QPointer<QWidget> panelBox;         // shown once output exists
        QPointer<QWidget> commandHeader;
        QString toolUseId; // set when the command has an approval row
        QString toolName;
        bool windowTool = false;
        int subagentTools = 0;
        QPointer<QLabel> subagentLine;
        QString agentTitle;
        QPointer<QWidget> agentState;
        QPointer<QWidget> agentSummary;
    };
    QHash<QString, ToolCard> m_toolCards;
    CommandApprovalResolver m_approvalResolver;
    QVector<CommandApprovalRow> m_commandApprovals;
    int m_approvalPrependAt = -1; // insert index while prepending earlier events
    QHash<QString, QPointer<QLabel>> m_liveAgentText;
    QHash<QString, QString> m_liveAgentTextValue;
    QHash<QString, QPointer<QWidget>> m_liveAgentRows;
    QString m_streamMessageId;
    QDateTime m_streamMessageAt;
    QHash<int, QString> m_streamTextKeys;
    QList<QPair<QString, QString>> m_pendingStreamText;
    forkmesh::transcripttools::OutputTokenCounter m_outputTokens;

    struct AskCard {
        QPointer<QWidget> buttons; // disabled on answer
        QPointer<QLabel> status;   // "✓ You answered: …"
        bool sensitive = false;    // never reveal or persist secret input
    };
    QHash<QString, AskCard> m_askCards;
    QPointer<QWidget> m_openInlineChoices;

    Collapsible *m_liveThinking = nullptr;
    QLabel *m_thinkingBody = nullptr;
    QString m_thinkingText;
    QString m_lastFinalizedThinkingText;
    int m_thinkingTokens = 0;
    qint64 m_thinkingStartMs = 0; // wall-clock start, for "Thought for Ns"

    QPropertyAnimation *m_scrollAnim = nullptr; // smooth scrolling
    qint64 m_totalTokens = 0;
    double m_totalCost = 0.0;
    bool m_bulkPopulate = false; // see setBulkPopulate()
    bool m_batchBuildFrozen = false; // see beginBatchBuild()
    bool m_batchThawing = false;
    QList<QPointer<QWidget>> m_retiredRows;
    bool m_retiredRowsScheduled = false;
    bool m_holdFrame = false;
    bool m_holdRowsAbove = false;
    QLabel *m_heldFrame = nullptr;
    int m_heldMax = 0;
    int m_heldValue = 0;
    QPointer<QLabel> m_resultLabel; // the newest clean `result` footer row
    QString m_resultLabelText;      // its text without the background-work note
    QString m_backgroundWorkNote;   // see setBackgroundWorkNote()
    QString m_ctxBranch, m_ctxMode, m_ctxStrength, m_ctxAccount;
    QPixmap m_agentAvatar, m_userAvatar; // see setSpeakerAvatars()
    QString m_agentName, m_userName;
    QDateTime m_eventTime; // eventTime() of the event handleEvent() is drawing
    QJsonObject m_rowSource;
    qint64 m_lastFadeMs = 0; // rate limits row fade-ins; see fadeIn()

    // A streamed turn arrives as hundreds of small deltas, and every one of them
    // used to reshape a whole rich-text label on the GUI thread. Mark what has
    // grown and repaint the lot on one short timer instead, flushing eagerly
    // whenever something depends on the text being current.
    void markTextDirty();
    void flushStreamingText();
    QSet<QString> m_dirtyAgentText;   // m_liveAgentText ids
    QSet<QString> m_dirtyToolOutput;  // m_toolCards ids with live output
    bool m_dirtyThinking = false;
    QTimer *m_textFlushTimer = nullptr;

    struct LabelHit {
        QPointer<QLabel> label;
        Qt::TextFormat fmt;
        QString orig;
        int count; // matches in this label
    };
    void rebuildSearchMatches();   // recompute m_searchLabels from m_searchQuery
    void renderSearchHighlights(); // (re)apply highlights, marking the current one
    void restoreSearchOriginals(); // put every modified label back as it was
    void scrollToCurrentMatch();
    void stepMatch(int delta);
    QLabel *currentMatchLabel() const;
    QString highlightedTextFor(const QString &orig, Qt::TextFormat fmt,
                               int currentLocalOcc) const;
    QString m_searchQuery;
    QVector<LabelHit> m_searchLabels;
    int m_searchTotal = 0;    // sum of all per-label counts
    int m_searchCurrent = -1; // global match index, -1 = none selected

    // A width change used to synchronously reshape every rich-text label from
    // inside QScrollArea::resizeEvent. Keep cache misses coalesced by label and
    // warm their exact heights in short event-loop slices instead.
    QVector<QPointer<QLabel>> m_labelRelayoutQueue;
    bool m_labelRelayoutScheduled = false;
};
