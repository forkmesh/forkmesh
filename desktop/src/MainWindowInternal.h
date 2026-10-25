#pragma once

// This header is the desktop's precompiled header: every change to it
// recompiles all ~75 MainWindow sources. Declare helpers here and put their
// bodies in MainWindowInternal1-4.cpp (any of them), so editing a body
// recompiles one file. Only templates, constexpr functions and small classes
// belong here in full. tools/mainwindow_split/split_internal_header.py moves
// inline bodies that come back.

#include "PingSettings.h"
#include "RailFolders.h"
#include "StartupTrace.h"
#include "AppPaths.h"
#include "MarkdownDocument.h"


#include "MainWindow.h"
#include "TerminalWidget.h"

#include "ActionFile.h"
#include "ActionRunner.h"
#include "AgentGuardrails.h"
#include "BackgroundActivity.h"
#include "GitIndexQueue.h"
#include "BackoffNetworkAccessManager.h"
#include "FileTreeSupport.h"
#include "GuiPump.h"
#include "ClaudeAgentScript.h"
#include "CloudflareAgentScript.h"
#include "ClaudeIdeBridge.h"
#include "ClaudeStreamSession.h"
#include "ClaudeTranscriptView.h"
#include "CommitGraph.h"
#include "ScrollJumpButtons.h"
#include "DirectorySizeScan.h"
#include "StallWatchdog.h"
#include "IssueBurnup.h"
#include "QrCode.h"
#include "ReferenceLinks.h"

#include "MarkdownEditor.h"
#include "MessageRow.h"
#include "MainnodeRoom.h"
#include "NodeDiagnostics.h"
#include "PullReviewModel.h"
#include "RepoSecurity.h"
#include "ServerNode.h"
#include "SingleInstance.h"
#include "SystemStats.h"
#include "AgentStore.h"
#include "Theme.h"
#include "DesignTokens.h"
#include "NextShellHooks.h"
#include "UiKit.h"
#include "VirtualMachineRuntime.h"

#include <QAbstractButton>
#include <QAbstractAnimation>
#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QCursor>
#include <QCloseEvent>
#include <QCryptographicHash>
#include <QComboBox>
#include <QCompleter>
#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QLocale>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEasingCurve>
#include <QFileInfo>
#include <QMimeData>
#include <QMutex>
#include <QMutexLocker>
#include <QFontDatabase>
#include <QFormLayout>
#include <QFrame>
#include <QFutureWatcher>
#include <QGraphicsOpacityEffect>
#include <QGridLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QWidgetAction>
#include <QtConcurrent/QtConcurrentRun>
#include <QEnterEvent>
#include <QMessageBox>
#include <QContextMenuEvent>
#include <QMimeDatabase>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslSocket>
#include <QSslError>
#include <QImage>
#include <QImageReader>
#include <QKeyEvent>
#include <QHelpEvent>
#include <QLineF>
#include <QToolTip>
#include <QConicalGradient>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QRadialGradient>
#include <QRadioButton>
#include <QShortcut>
#include <QTreeWidgetItem>
#include <QtMath>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPropertyAnimation>
#include <QRandomGenerator>
#include <QEventLoop>
#include <QFileSystemWatcher>
#include <QProcess>
#include <QProcessEnvironment>
#include <QProgressBar>
#include <QTextCursor>
#include <QTimeZone>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSaveFile>
#include <QScreen>
#include <QScopedValueRollback>
#include <QScrollArea>
#include <QSet>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QSize>
#include <QSysInfo>
#include <QSplitter>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStringList>
#include <QStringListModel>
#include <QStyle>
#include <QStylePainter>
#include <QStyledItemDelegate>
#include <QStyleHints>
#include <QStyleOptionComboBox>
#include <QSyntaxHighlighter>
#include <QAbstractItemView>
#include <QHeaderView>
#include <QTableWidget>
#include <QTabBar>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextEdit>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QToolButton>
#include <QTreeWidget>
#include <QSystemTrayIcon>
#include <QSvgRenderer>
#include <QScopeGuard>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>
#include <QVector>
#include <QWindow>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <utility>

#ifndef Q_OS_WIN
#include <csignal>
#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#ifndef FORKMESH_SOURCE_DIR
#define FORKMESH_SOURCE_DIR ""
#endif

namespace forkmesh::localcontrol::agentops {
struct UsageWindow;
}

namespace forkmesh::ui {

class WindowChromeBar : public QWidget
{
public:
    explicit WindowChromeBar(QWidget *parent = nullptr) : QWidget(parent)
    {
        setObjectName(QStringLiteral("windowChromeBar"));
        setFixedHeight(42);
    }

    std::function<void(const QPoint &)> onContextMenu;

protected:
    void contextMenuEvent(QContextMenuEvent *event) override
    {
        if (onContextMenu) {
            onContextMenu(event->globalPos());
            event->accept();
            return;
        }
        QWidget::contextMenuEvent(event);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            if (QWindow *handle = window()->windowHandle())
                handle->startSystemMove();
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            QWidget *top = window();
            if (top->isMaximized())
                top->showNormal();
            else
                top->showMaximized();
            event->accept();
            return;
        }
        QWidget::mouseDoubleClickEvent(event);
    }
};

QString openAiAuthHeader(const QString &apiKey);
QNetworkRequest openAiRequest(const QUrl &url, const QString &apiKey);
QString apiErrorSummary(QNetworkReply *reply, const QByteArray &body);
QString openAiResponseText(const QJsonObject &obj);
double openAiAskCostUsd(const QJsonObject &response, qint64 *inTokens = nullptr,
                        qint64 *outTokens = nullptr);
QString mirrorHeadBranch(const QString &mirrorPath);
QString mirrorBranchCommit(const QString &mirrorPath, const QString &branch);
QString cachedRemoteFetchUrl(const QString &repoPath, const QString &remote);
// True when the checkout's upstream remote is the relay (host relayHost) or
// the repository's own mirror: Sync Changes then publishes through the mirror
// and never advances the @{upstream} tracking ref, so commits "ahead of
// upstream" are not waiting to sync. Runs git; callable from workers.
bool upstreamPublishesThroughMirror(const QString &localPath,
                                    const QString &mirrorPath,
                                    const QString &relayHost);
// Host of the configured relay server (the default relay when unset).
QString configuredRelayHost();
void forgetConfiguredRelayHost();
struct MirrorBranchTip {
    QString branch;
    QString commit;
};
MirrorBranchTip mirrorPrimaryBranchTip(const QString &mirrorPath,
                                       const QString &workTree);
QString actionStatusText(const QString &status);
QColor actionStatusColor(const QString &status);
void logStartup(const QString &phase);
void beginRestartLog();
void logRestart(const QString &phase);


// The composer takes video attachments beside images, so every surface that
// renders or forwards one has to know which it is holding: a video has no
// thumbnail to draw, and no model takes it as image content.
bool isVideoAttachmentPath(const QString &path);
// The filter the composer's attach dialog offers.
QString promptAttachmentFileFilter();
// The line a queued attachment contributes to the prompt. Videos are named as
// videos so a CLI that only understands images leaves them as a path.
QString promptAttachmentLine(const QString &path);
// The two halves of a sent prompt: the attachment paths it names, and the prose
// with those lines taken out. Both kinds of attachment count.
QStringList promptAttachmentPaths(const QString &prompt);
QString promptTextWithoutAttachments(const QString &prompt);

struct DiffFileEntry {
    QString path;
    QString anchor;
    int adds = 0;
    int dels = 0;
    QString status = QStringLiteral("modified"); // added/deleted/modified/renamed
    bool binary = false;
    // Set on a markdown file: the registerDiffMarkdownSource() key its Preview
    // pill opens the file formatted from.
    QString markdownPreviewKey;
    QString editDir;
    bool deletable = false;
    QString repoDir;
    QString branch;
};
struct DiffRenderAppearance {
    bool dark = true;
    qreal dpr = 1.0;
    QMap<QString, QString> colorRemap;
    QString treeIconDir;
    bool changeArrows = false;
    int changeArrowsStagedFiles = 0;
};
DiffRenderAppearance currentDiffRenderAppearance();
// One compact score in the branch review's merge-health strip. Scores are
// intentionally backed by `detail`: a coloured number without the evidence
// behind it would look more authoritative than a lightweight diff heuristic is.
struct DiffHealthMetric {
    QString key;
    QString label;
    int score = 100; // 85-100 green, 60-84 orange, 0-59 red
    QString detail;
};
struct DiffHealthReport {
    QList<DiffHealthMetric> metrics; // Merge first, then its component signals
    QList<RepoSecurityFinding> secrets;
};
// Grades only the patch on screen plus Git's clean-merge/divergence result. It
// performs no network traffic and executes none of the changed code.
DiffHealthReport diffHealthReport(const QString &patch,
                                  const QList<DiffFileEntry> &files,
                                  int behind, int ahead, bool mergeConflict,
                                  bool worktreeConflict,
                                  bool conflictCheckPending);
// How wide a fingerprint diffFileFingerprints() writes, and how wide the review
// state stores one.
inline constexpr int kDiffFingerprintChars = 16;
// What each file of a patch looked like, as path -> a digest of the lines that
// file's section holds. Ticking Viewed records the file's fingerprint, so a
// later patch that fingerprints differently says the file changed under the
// reviewer and the mark no longer describes what is on screen. A path that
// sections twice — the source-control pane concatenates the staged and unstaged
// patches — folds into one digest, so staging alone counts as a change.
QHash<QString, QString> diffFileFingerprints(const QString &patch);
struct DiffViewedPrune {
    QHash<QString, QString> stamps;
    QStringList dropped;
    bool changed = false;
};
DiffViewedPrune pruneDiffViewedStamps(QHash<QString, QString> stamps,
                                      const QHash<QString, QString> &current);
struct DiffRenderSink {
    std::function<bool(const QString &chunk, int filesDone)> deliver;
    bool aborted = false;
};
struct DiffCachedFile {
    QString block;
    QString openBlock;
    qsizetype openHeaderChars = 0;
};
struct DiffBlockCache {
    QList<DiffFileEntry> files;
    QHash<QString, DiffCachedFile> byAnchor;
    bool comments = false;
};
struct BranchDiffRender {
    QString html;
    QList<DiffFileEntry> files;
    QSet<QString> viewed;
    QSet<QString> expanded;
    QHash<QString, QString> fingerprints;
    DiffViewedPrune prune;
    bool hasHealth = false;
    QString healthKey;
    DiffHealthReport health;
    DiffBlockCache blocks;
    bool aborted = false;
};
QString diffStyleSheet(int fontPt = 12);
// `expandedFiles` reopens files that are in `viewedFiles`: a reviewer can look
// at a folded file again without giving up the review credit for it.
// Line-note key for review threads shown at the top of a file's diff table
// rather than on a line (see fileLevelNoteRow in MainWindowShared.cpp).
QString fileLevelNoteKey(const QString &path);
QString renderDiffHtml(const QString &patch, QList<DiffFileEntry> &files,
                       const QString &dir, const QString &base, const QString &head,
                       const QString &anchorFile = QString(),
                       const QHash<QString, QString> &lineNotes = {},
                       const QSet<QString> &viewedFiles = {},
                       const QSet<QString> &expandedFiles = {});
QString renderDiffHtmlSplit(bool split, const QString &patch,
                            QList<DiffFileEntry> &files, const QString &dir,
                            const QString &base, const QString &head,
                            const QString &anchorFile = QString(),
                            const QHash<QString, QString> &lineNotes = {},
                            const QSet<QString> &viewedFiles = {},
                            const QSet<QString> &expandedFiles = {},
                            std::optional<DiffRenderAppearance> appearanceOverride =
                                std::nullopt,
                            DiffRenderSink *sink = nullptr);
bool diffSplitPref();
void setDiffSplitPref(bool split);
QString diffFileLabelHtml(const DiffFileEntry &f, bool viewed);
QString diffRowControlsHtml(const QString &path, double progress, bool viewed,
                            bool comments, const QString &leadCells = QString(),
                            bool expanded = false);
// "expand:<percent-encoded path>" — the anchor the fold toggle carries.
QString diffExpandHref(const QString &path);
QString diffEditHref(const QString &path, const QString &dir);
QString diffEditableDir(const QString &dir, const QString &head);
bool diffEditTarget(const QUrl &url, QString *dir, QString *path);
QString diffDeleteHref(const QString &path, const QString &dir);
bool diffDeleteTarget(const QUrl &url, QString *dir, QString *path);
struct DiffFileMenuTarget {
    QString path;
    QString dir;
    bool deletable = false;
    QString repoDir;
    QString branch;
};
QString diffFileMenuHref(const DiffFileEntry &f);
bool diffFileMenuTarget(const QUrl &url, DiffFileMenuTarget *target);
QString diffFileActionPillsHtml(const DiffFileEntry &f);
// A markdown file's two sides as a diff render saw them, kept so the file's
// Preview pill can open it formatted. `newText`/`oldText` are what the patch
// itself spells out: the whole file when it was added or deleted, only the
// changed sections otherwise.
struct DiffMarkdownSource {
    QString dir;
    QString base;
    QString head; // empty: the working tree
    QString path;
    QString status;
    QString newText;
    QString oldText;
};
// "mdpreview:<percent-encoded path>?k=<key>" — the Preview pill's anchor.
QString diffMarkdownPreviewHref(const QString &path, const QString &key);
QString registerDiffMarkdownSource(const DiffMarkdownSource &source);
bool diffMarkdownSourceForKey(const QString &key, DiffMarkdownSource *out);
struct DiffMarkdownPreviewSide {
    bool exists = false;   // false: the file has no such side (added/deleted)
    bool complete = false; // false: only the sections the patch carries
    QString text;
    QString note;
};
// Reads through git when the source names a repository, which pumps the
// event loop.
DiffMarkdownPreviewSide diffMarkdownPreviewSide(const DiffMarkdownSource &source,
                                                bool after);
// Opens the formatted preview a Preview pill's anchor asks for; null when
// `url` is not one.
QDialog *showDiffMarkdownPreview(QWidget *parent, const QUrl &url);
void openDiffMarkdownPreview(QWidget *parent, const QUrl &url);
QString diffReviewSummaryHtml(const QList<DiffFileEntry> &files,
                              const QSet<QString> &viewed, bool current);
QString diffViewedCountHtml(int seen, int total, bool current);
// What ticking Viewed on one file costs. `noteDiffFileBlocks` records the
// blocks a full render put in the document; `swapDiffFileViewedState` then
// folds or reopens exactly one of them in place, leaving every other file's
// layout — and the reviewer's scroll position — untouched. A file sliced across
// page edges folds the same way, on and off the loaded window; when the
// document itself cannot be edited (the window is still landing, the body to
// put back is many slices) the loaded window is redrawn at that file from the
// edited plan. `applied` is false only when there is nothing to work from (no
// plan yet, or this document never drew the file open), and the caller
// re-renders in full instead.
struct DiffFileBlockSwap {
    bool applied = false;
    QString oldHtml; // the block the document held, for the caller's render key
    QString newHtml; // what stands in its place
};
QStringList splitDiffFileBlocks(const QString &html);
DiffBlockCache buildDiffBlockCache(const QStringList &blocks,
                                   const QList<DiffFileEntry> &files,
                                   const QSet<QString> &viewedFiles,
                                   const QSet<QString> &expandedFiles, bool comments,
                                   const DiffRenderAppearance &appearance);
void noteDiffFileBlocks(QTextEdit *view, DiffBlockCache cache);
void noteDiffFileBlocks(QTextEdit *view, const QString &html,
                        const QList<DiffFileEntry> &files,
                        const QSet<QString> &viewedFiles,
                        const QSet<QString> &expandedFiles, bool comments);
QList<DiffFileEntry> diffRenderedFiles(QTextEdit *view);
DiffFileBlockSwap swapDiffFileViewedState(QTextEdit *view, const QString &anchor,
                                          bool viewed, bool expanded);
QString diffSectionBreakHtml();
void renderDiffStreamed(QTextEdit *view, const QString &html,
                        const QString &styleSheet, bool endCap = true);
bool restyleDiffStreamed(QTextEdit *view, const QString &styleSheet);
void beginDiffStreamIncremental(QTextEdit *view, const QString &styleSheet,
                                const QString &placeholder, bool endCap = true);
void appendDiffStreamChunk(QTextEdit *view, const QString &chunk);
QList<QStringList> paginateDiffStreamChunk(const QString &chunk);
void appendDiffStreamPages(QTextEdit *view, const QList<QStringList> &pages);
void finishDiffStreamIncremental(QTextEdit *view);
void scrollDiffToAnchor(QTextEdit *view, const QString &anchor);
void flushDiffStream(QTextEdit *view);
void addDiffStreamFinishedHook(QTextEdit *view, std::function<void()> hook);
bool diffStreamInProgress(QTextEdit *view);
bool diffIsPaginated(QTextEdit *view);
bool showDiffPageContainingText(QTextEdit *view, const QString &text,
                                int direction, bool wrap);
#ifdef FORKMESH_WINDOW_TESTS
QList<qsizetype> diffPlannedUiApplySizesForTest(const QString &html);
qsizetype diffMaxUiApplyCharsForTest();
QStringList diffFileBlocksForTest(const QString &html);
QList<QStringList> diffPlannedPagesForTest(const QString &html);
int diffPageCountForTest(QTextEdit *view);
void showDiffPageForTest(QTextEdit *view, int page);
int diffWindowFirstPageForTest(QTextEdit *view);
int diffLoadedPageViewportYForTest(QTextEdit *view, int page);
bool diffScrollBarGeometryForTest(QTextEdit *view, int *value, int *maximum);
#endif
bool autoMarkViewedOnScrollPref();
void setAutoMarkViewedOnScrollPref(bool on);
void applyDiffSearchHighlights(QTextBrowser *diff,
                               const QList<QTextCursor> &matches, int activeIndex,
                               QLabel *countLabel, bool termEmpty);
// The two halves of the above, for find bars over a widget the overload above
// can't take — the code editor's is a QPlainTextEdit (adhoc #1607).
QList<QTextEdit::ExtraSelection> findMatchSelections(const QList<QTextCursor> &matches,
                                                     int activeIndex);
QString findMatchCountText(int activeIndex, int count, bool termEmpty);
// Parent for a widget pinned over a diff — a sticky file header, the outline
// around the active file. QTextEdit scrolls by calling QWidget::scroll() on its
// viewport, and that drags the viewport's own children along with the text: an
// overlay parented there is displaced by every scroll step and only put back
// afterwards, which is the flicker. Riding the view instead leaves it still.
QWidget *diffOverlayParent(QTextEdit *view);
// Positions such an overlay in the viewport's coordinates regardless.
void placeDiffOverlay(QWidget *overlay, QTextEdit *view,
                      const QRect &viewportRect);
// Y for a sticky file header of `stickyHeight`, given the scroll position and
// where the *next* file starts in document space: 0 while the pinned file still
// has rows under the bar, then negative as the next file's header pushes it
// out, so the two never draw on top of each other.
int diffStickyTopOffset(int stickyHeight, int viewTop, int fileBottom);
// Document-space rectangle of the file header row the given document position
// sits in — the row's own edges, which is what an outline drawn around a file
// has to line up with. Null when the position is not inside a header row.
QRectF diffFileRowRect(const QTextEdit *view, int position);
// One side of a file's "GIT binary patch" block, inflated back into the bytes
// it encodes: the post-image, or the pre-image with `preImage`. Empty for a
// delta block, a truncated one, or a patch written without --binary.
QByteArray diffBinaryPatchImage(const QString &payload, bool preImage);
void prefetchDiffImageBlobs(const QString &patch, const QString &dir,
                            const QString &base, const QString &head);
QString diffStickyStyleSheet(int fontPt);
QString diffStickyPathHtml(const QString &fullPath);
QString diffStickyPathText(const QString &path);
void configureDiffStickyPathLabel(QLabel *label);
QString agentCostText(double usd);
QString agentStatusText(const QString &status);
QColor agentStatusColor(const QString &status);
bool agentSessionActive(const AgentSession *s);
QIcon agentStatusOcticon(const AgentSession &s, int px = 13);
QColor agentStatusIconColor(const AgentSession &s);
QString linkifyIssueRefs(const QString &escaped);
// Markup to place before a linked reference: kind is "issue", "pull" or "ref"
// (a bare #N). An empty return leaves the link undecorated.
using ReferenceStatusGlyph = std::function<QString(const QString &kind, int number)>;
QString linkifyReferenceLine(const QString &line,
                             const ReferenceStatusGlyph &statusGlyph = {});
class DiffFileNavigator : public QObject
{
public:
    DiffFileNavigator(QTextBrowser *diff, QListWidget *list, int anchorRole,
                      QObject *parent)
        : QObject(parent), m_diff(diff), m_list(list), m_anchorRole(anchorRole)
    {
        m_list->setFocusPolicy(Qt::StrongFocus);
        m_list->setSelectionMode(QAbstractItemView::SingleSelection);
        m_list->installEventFilter(this);
        m_sticky = new QLabel(diffOverlayParent(m_diff));
        m_sticky->setObjectName(QStringLiteral("diffStickyHeader"));
        m_sticky->setTextFormat(Qt::RichText);
        configureDiffStickyPathLabel(m_sticky);
        m_sticky->hide();
        QObject::connect(m_diff->verticalScrollBar(), &QScrollBar::valueChanged,
                         this, [this] {
                             if (!m_ignoreScroll)
                                 refresh(/*syncSelection=*/true);
                         });
        QObject::connect(m_list, &QListWidget::currentItemChanged, this,
                         [this](QListWidgetItem *it, QListWidgetItem *) {
                             if (!it)
                                 return;
                             const QString anchor = it->data(m_anchorRole).toString();
                             if (anchor.isEmpty())
                                 return;
                             m_ignoreScroll = true;
                             scrollDiffToAnchor(m_diff, anchor);
                             m_ignoreScroll = false;
                             refresh(/*syncSelection=*/false);
                         });
        addDiffStreamFinishedHook(m_diff, [this] { rebuildSpans(); });
    }

    void rebuild(const QList<DiffFileEntry> &files, int fontPt)
    {
        m_files = files;
        m_filesByAnchor.clear();
        for (const DiffFileEntry &file : m_files)
            m_filesByAnchor.insert(file.anchor, file);
        m_sticky->setStyleSheet(diffStickyStyleSheet(fontPt));
        rebuildSpans();
    }

private:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == m_list && event->type() == QEvent::KeyPress) {
            const auto *key = static_cast<QKeyEvent *>(event);
            if ((key->key() == Qt::Key_Up || key->key() == Qt::Key_Down) &&
                !(key->modifiers() &
                  (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) {
                const int delta = key->key() == Qt::Key_Up ? -1 : 1;
                int row = m_list->currentRow();
                if (row < 0)
                    row = delta > 0 ? -1 : m_list->count();
                do {
                    row += delta;
                } while (row >= 0 && row < m_list->count() &&
                         m_list->item(row)->isHidden());
                if (row >= 0 && row < m_list->count()) {
                    m_list->setCurrentRow(row);
                    m_list->scrollToItem(m_list->item(row));
                }
                return true;
            }
        }
        return QObject::eventFilter(watched, event);
    }

    struct Span {
        int pos;
        QString path;
        QString anchor;
    };

    void rebuildSpans()
    {
        m_spans.clear();
        QTextDocument *doc = m_diff->document();
        // One pass over the document, not one per file: a paged diff can list
        // hundreds of files while the document holds only the current page.
        QHash<QString, int> anchorPositions;
        for (QTextBlock block = doc->begin(); block.isValid();
             block = block.next()) {
            for (auto it = block.begin(); !it.atEnd(); ++it) {
                const QTextFragment fragment = it.fragment();
                if (!fragment.isValid() || !fragment.charFormat().isAnchor())
                    continue;
                const QStringList names = fragment.charFormat().anchorNames();
                for (const QString &name : names) {
                    if (!anchorPositions.contains(name))
                        anchorPositions.insert(name, fragment.position());
                }
            }
        }
        // Path-text matching stays as the fallback for content without real
        // anchor characters, but never on a paged diff: there a missing file
        // legitimately lives on another page, and its path can occur verbatim
        // inside unrelated hunks.
        const bool pathFallback = !diffIsPaginated(m_diff);
        for (const DiffFileEntry &file : std::as_const(m_files)) {
            int position = anchorPositions.value(file.anchor, -1);
            if (position < 0 && pathFallback) {
                const QTextCursor found = doc->find(file.path);
                if (!found.isNull())
                    position = found.selectionStart();
            }
            if (position >= 0)
                m_spans.append({position, file.path, file.anchor});
        }
        std::sort(m_spans.begin(), m_spans.end(),
                  [](const Span &left, const Span &right) {
                      return left.pos < right.pos;
                  });
        refresh(/*syncSelection=*/false);
    }

    void refresh(bool syncSelection)
    {
        if (m_spans.isEmpty() || m_diff->verticalScrollBar()->value() <= 0) {
            m_sticky->hide();
            return;
        }
        const int top = m_diff->cursorForPosition(QPoint(2, 2)).position();
        const Span *cur = nullptr;
        for (const Span &s : m_spans) {
            if (s.pos <= top)
                cur = &s;
            else
                break;
        }
        if (!cur) {
            m_sticky->hide();
            return;
        }
        if (syncSelection)
            selectByAnchor(cur->anchor);
        const auto file = m_filesByAnchor.constFind(cur->anchor);
        m_sticky->setText(file == m_filesByAnchor.constEnd()
                              ? diffStickyPathHtml(cur->path)
                              : diffFileLabelHtml(file.value(), false));
        const bool wasHidden = m_sticky->isHidden();
        placeDiffOverlay(m_sticky, m_diff,
                         QRect(0, 0, m_diff->viewport()->width(),
                               m_sticky->sizeHint().height()));
        if (wasHidden) {
            // Only when it comes back: raise() dirties the widget, and doing
            // that on every scroll tick is what makes the bar flicker.
            m_sticky->show();
            m_sticky->raise();
        }
    }

    void selectByAnchor(const QString &anchor)
    {
        for (int i = 0; i < m_list->count(); ++i) {
            if (m_list->item(i)->data(m_anchorRole).toString() != anchor)
                continue;
            if (m_list->currentRow() != i) {
                QSignalBlocker block(m_list);
                m_list->setCurrentRow(i);
            }
            return;
        }
    }

    QTextBrowser *m_diff = nullptr;
    QListWidget *m_list = nullptr;
    QLabel *m_sticky = nullptr;
    int m_anchorRole = Qt::UserRole;
    bool m_ignoreScroll = false;
    QList<DiffFileEntry> m_files;
    QHash<QString, DiffFileEntry> m_filesByAnchor;
    QList<Span> m_spans;
};
struct ScmAiModel {
    const char *provider; // "claude" | "openai"
    const char *id;
    const char *label;
    double inPerM;
    double outPerM;
    bool estimated; // pricing is approximate (OpenAI)
};
const ScmAiModel kScmAiModels[] = {
    {"claude", "claude-opus-5", "Claude Opus 5", 5.0, 25.0, false},
    {"claude", "claude-sonnet-5", "Claude Sonnet 5", 3.0, 15.0, false},
    {"claude", "claude-haiku-4-5", "Claude Haiku 4.5", 1.0, 5.0, false},
    {"openai", "gpt-4.1", "OpenAI GPT-4.1", 2.0, 8.0, true},
    {"openai", "gpt-4.1-mini", "OpenAI GPT-4.1 mini", 0.40, 1.60, true},
    {"openai", "gpt-4.1-nano", "OpenAI GPT-4.1 nano", 0.10, 0.40, true},
};
const int kScmAiModelCount = int(sizeof(kScmAiModels) / sizeof(kScmAiModels[0]));

// What the OS notifier is handed: desktop shells scale the file down themselves,
// so give them room rather than a 40px face blown up.
constexpr int kAgentNotificationIconPx = 128;

constexpr int kTableSortRole = Qt::UserRole + 10;
constexpr int kProgressBarRole = Qt::UserRole + 11;
constexpr int kPacmanAnchorRole = Qt::UserRole + 12;
constexpr int kNodeLightRole = Qt::UserRole + 13;
// Repos page: bool, this stacked cell opens on a title line (drawn bold).
constexpr int kRepoLeadBoldRole = Qt::UserRole + 14;
// Repos page: bool, this is the cell the mirror-count request writes into.
constexpr int kRepoMirrorCellRole = Qt::UserRole + 15;
// Actual jittered timer interval for a behind-row countdown. It differs from
// the operator's disconnected cadence while the event socket is healthy.
constexpr int kPacmanIntervalRole = Qt::UserRole + 16;
// One-minute legacy/disconnected baseline, also used to throttle repeated
// source-convergence attempts. WebSocket commit events are the primary update
// path and use the separate sparse safety interval below.
constexpr qint64 kMirrorSyncIntervalMs = 60LL * 1000;
// While the authenticated repository event socket is healthy, this is the
// maximum safety interval. A shorter operator-configured interval still wins,
// because collaboration refs can advance without a code-commit event.
constexpr qint64 kEventDrivenMirrorSafetyIntervalMs = 10LL * 60 * 1000;
constexpr int kMirrorSyncJitterPercent = 15;
constexpr int kMirrorSyncIntervalMinMinutes = 1;
constexpr int kMirrorSyncIntervalMaxMinutes = 24 * 60;
constexpr int kMirrorSyncIntervalDefaultMinutes = 1;
const QString kMirrorSyncIntervalSetting =
    QStringLiteral("repos/mirrorSyncIntervalMinutes");

int mirrorSyncIntervalMinutes();

qint64 mirrorSyncIntervalMs();

constexpr auto kActionNodeLabelsSetting = "actions/nodeLabels";
bool currentThemeIsDark();
QIcon themedOcticon(const QString &name, const QColor &color, int size);

constexpr int kCommitSummaryCol = 6;
constexpr int kCommitHashCol = 2;
constexpr int kCommitActionCol = 7;
constexpr int kCommitGraphCol = 8;
constexpr int kGraphLanesRole = Qt::UserRole + 20;    // QVariantList<int> lanes at the row's top edge
constexpr int kGraphNodeLaneRole = Qt::UserRole + 21; // int lane of this commit's dot
constexpr int kGraphBottomLanesRole =
    Qt::UserRole + 22; // QVariantList<int> lanes at the row's bottom edge
constexpr int kCommitRowKindRole = Qt::UserRole + 23; // 0 = commit, 1 = file child row
constexpr int kCommitExpandedRole = Qt::UserRole + 24; // bool: commit row is expanded
constexpr int kCommitAuthorRole = Qt::UserRole + 25;   // author drawn right of the summary
constexpr int kCommitUnsyncedRole = Qt::UserRole + 26; // bool: not yet on the mirror
constexpr int kCommitFileAddsRole = Qt::UserRole + 27; // file row: added lines
constexpr int kCommitFileDelsRole = Qt::UserRole + 28; // file row: deleted lines
constexpr int kCommitFilePathRole = Qt::UserRole + 29; // file row: repo-relative path
constexpr int kCommitRefsRole = Qt::UserRole + 30;     // branch/tag badges (QStringList)
constexpr int kCommitBodyRole = Qt::UserRole + 31;     // full message body (fed to the hover box)
constexpr int kGraphIsMergeRole = Qt::UserRole + 32;   // graph cell: commit has >1 parent
constexpr int kCommitFilesRole =
    Qt::UserRole + 33; // QStringList "path\tadds\tdels" of the files the commit
constexpr int kCommitRefKindsRole =
    Qt::UserRole + 34; // QStringList aligned with kCommitRefsRole: local/remote/tag
constexpr int kCommitOutgoingRole = Qt::UserRole + 35;

const QLatin1String kBranchLinkScheme("forkmesh-branch:");

const QLatin1String kPullLinkScheme("forkmesh-pull:");

const QLatin1String kIssueLinkScheme("forkmesh-issue:");

const QLatin1String kTaskLinkScheme("forkmesh-task:");

const QLatin1String kUserLinkScheme("forkmesh-user:");

QString userProfileAccount(const QString &name);
QString userProfileLinkHtml(const QString &account, const QString &shown = QString());

QString branchLinkHtml(const QString &branch);

const QLatin1String kRepoInfoPath(".forkmesh/info.json");

const QLatin1String kAgentLinkScheme("forkmesh-agent:");

// Sidebar values are painted in #statusLine's muted grey, which Qt folds into
// the widget palette when it polishes the rule. An anchor placed inside one has
// to reuse that colour rather than the palette's link blue, or making a value
// clickable would repaint the whole row and break the column's rhythm.
QString issueSidebarValueColor(const QWidget *value);



void paintRowSelectionBorder(QPainter *painter,
                                    const QStyleOptionViewItem &option,
                                    const QModelIndex &index);


class CommitSummaryDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        opt.text.clear();
        opt.icon = QIcon();
        opt.features &= ~QStyleOptionViewItem::HasDecoration;
        opt.state &= ~(QStyle::State_MouseOver | QStyle::State_Selected);
        const QWidget *w = option.widget;
        QStyle *style = w ? w->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, w);
        paintRowSelectionBorder(painter, option, index);

        const bool fileRow = index.data(kCommitRowKindRole).toInt() == 1;

        const QModelIndex graphIdx =
            index.model()->index(index.row(), kCommitGraphCol);
        const QVariantList topLanes = graphIdx.data(kGraphLanesRole).toList();
        const QVariantList botLanes =
            graphIdx.data(kGraphBottomLanesRole).toList();
        const QVariant nodeLaneVar = graphIdx.data(kGraphNodeLaneRole);
        const int nodeLane = nodeLaneVar.isValid() ? nodeLaneVar.toInt() : -1;
        int rowMaxLane = std::max(nodeLane, 0);
        for (const QVariant &v : topLanes)
            rowMaxLane = std::max(rowMaxLane, v.toInt());
        for (const QVariant &v : botLanes)
            rowMaxLane = std::max(rowMaxLane, v.toInt());
        if (fileRow) // nested one step under its commit's lane
            rowMaxLane =
                std::max(rowMaxLane, index.data(kGraphNodeLaneRole).toInt());
        const int laneCount =
            std::max(rowMaxLane + 1,
                     w ? w->property(kGraphLaneCountProperty).toInt() : 0);
        const CommitGraphMetrics metrics = commitGraphMetrics(laneCount);
        paintCommitGraphGutter(painter, option.rect, topLanes, botLanes,
                               nodeLane,
                               graphIdx.data(kGraphIsMergeRole).toBool(),
                               index.data(kCommitOutgoingRole).toBool(),
                               metrics);
        const int indent = commitGraphTextIndent(rowMaxLane, metrics);

        const QFontMetrics fm(option.font);
        QRect r = option.rect.adjusted(indent, 0, -8, 0);
        const QColor dim = Theme::remapColor(QColor("#8b949e"));

        painter->save();
        if (fileRow) {
            r.adjust(18, 0, 0, 0); // nest files under their commit
            const int adds = index.data(kCommitFileAddsRole).toInt();
            const int dels = index.data(kCommitFileDelsRole).toInt();
            const QString addsTxt = QStringLiteral("+%1").arg(adds);
            const QString delsTxt =
                QString::fromUtf8("\xE2\x88\x92%1").arg(dels);
            const int delsW = fm.horizontalAdvance(delsTxt);
            const int addsW = fm.horizontalAdvance(addsTxt);
            painter->setPen(QColor("#f85149"));
            painter->drawText(QRect(r.right() - delsW, r.top(), delsW, r.height()),
                              Qt::AlignVCenter | Qt::AlignRight, delsTxt);
            painter->setPen(QColor("#2ea043"));
            painter->drawText(
                QRect(r.right() - delsW - 6 - addsW, r.top(), addsW, r.height()),
                Qt::AlignVCenter | Qt::AlignRight, addsTxt);
            int fx = r.left();
            const QIcon fic = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
            if (!fic.isNull()) {
                fic.paint(painter, QRect(fx, r.center().y() - 7, 14, 14));
                fx += 18;
            }
            const int textW = r.right() - fx - delsW - addsW - 20;
            painter->setPen(dim);
            painter->drawText(
                QRect(fx, r.top(), std::max(0, textW), r.height()),
                Qt::AlignVCenter | Qt::AlignLeft,
                fm.elidedText(index.data(Qt::DisplayRole).toString(),
                              Qt::ElideMiddle, std::max(0, textW)));
            painter->restore();
            return;
        }

        int x = r.left();
        const QIcon icon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
        if (!icon.isNull()) {
            const QRect ir(x, r.center().y() - 7, 14, 14);
            icon.paint(painter, ir);
            x += 18;
        }
        int rightEdge = r.right();
        const QStringList refs = index.data(kCommitRefsRole).toStringList();
        const QStringList refKinds =
            index.data(kCommitRefKindsRole).toStringList();
        if (index.data(kCommitUnsyncedRole).toBool()) {
            const QString mark = QString::fromUtf8("\xE2\x96\xB2");
            const int mw = fm.horizontalAdvance(mark);
            painter->setPen(QColor("#d29922"));
            painter->drawText(QRect(rightEdge - mw, r.top(), mw, r.height()),
                              Qt::AlignVCenter | Qt::AlignRight, mark);
            rightEdge -= mw + 8;
        }
        if (!refs.isEmpty()) {
            painter->setRenderHint(QPainter::Antialiasing, true);
            for (int refIndex = refs.size() - 1; refIndex >= 0; --refIndex) {
                const QString ref = refs.at(refIndex);
                const QString kind = refKinds.value(refIndex);
                const bool remote = kind == QLatin1String("remote");
                const bool tag = kind == QLatin1String("tag");
                const QString iconName = remote ? QStringLiteral("cloud")
                                                : tag ? QStringLiteral("tag")
                                                      : QStringLiteral("git-commit");
                const QColor badgeColor =
                    remote ? QColor("#8957e5")
                           : tag ? QColor("#2da44e") : QColor("#1f6feb");
                constexpr int iconSize = 12;
                constexpr int iconGap = 4;
                const int rw = fm.horizontalAdvance(ref) + 12 + iconSize + iconGap;
                if (x + rw > rightEdge - 80)
                    break; // keep room for the summary itself
                rightEdge -= rw;
                const QRect br(rightEdge, r.center().y() - fm.height() / 2 - 1, rw,
                               fm.height() + 2);
                painter->setPen(Qt::NoPen);
                painter->setBrush(badgeColor);
                painter->drawRoundedRect(br, br.height() / 2.0,
                                         br.height() / 2.0);
                const QRect iconRect(br.left() + 6,
                                     br.center().y() - iconSize / 2,
                                     iconSize, iconSize);
                themedOcticon(iconName, Qt::white, iconSize)
                    .paint(painter, iconRect);
                painter->setPen(Qt::white);
                painter->drawText(
                    br.adjusted(6 + iconSize + iconGap, 0, -6, 0),
                    Qt::AlignVCenter | Qt::AlignLeft, ref);
                rightEdge -= 5;
            }
            painter->setBrush(Qt::NoBrush);
        }
        const QVariant fgVar = index.data(Qt::ForegroundRole);
        const QColor fg = fgVar.isValid()
                              ? qvariant_cast<QBrush>(fgVar).color()
                              : option.palette.color(QPalette::Text);
        const int textW = std::max(0, rightEdge - x);
        const QString subject = index.data(Qt::DisplayRole).toString();
        const QString author = index.data(kCommitAuthorRole).toString();
        const QString suffix =
            author.isEmpty() ? QString()
                             : QString::fromUtf8("  \xC2\xB7  ") + author;
        const QString elidedSubject =
            fm.elidedText(subject, Qt::ElideRight, textW);
        const int subjectW = fm.horizontalAdvance(elidedSubject);
        painter->setPen(fg);
        painter->drawText(QRect(x, r.top(), subjectW, r.height()),
                          Qt::AlignVCenter | Qt::AlignLeft, elidedSubject);
        if (!suffix.isEmpty()) {
            const int rem = std::max(0, textW - subjectW);
            painter->setPen(dim);
            painter->drawText(
                QRect(x + subjectW, r.top(), rem, r.height()),
                Qt::AlignVCenter | Qt::AlignLeft,
                fm.elidedText(suffix, Qt::ElideRight, rem));
        }
        painter->restore();
    }
};

class SortTableWidgetItem : public QTableWidgetItem
{
public:
    using QTableWidgetItem::QTableWidgetItem;

    bool operator<(const QTableWidgetItem &other) const override
    {
        const QVariant left = data(kTableSortRole);
        const QVariant right = other.data(kTableSortRole);
        if (left.isValid() && right.isValid()) {
            bool leftOk = false;
            bool rightOk = false;
            const double leftNumber = left.toDouble(&leftOk);
            const double rightNumber = right.toDouble(&rightOk);
            if (leftOk && rightOk)
                return leftNumber < rightNumber;
            return left.toString().compare(right.toString(), Qt::CaseInsensitive) < 0;
        }
        return QTableWidgetItem::operator<(other);
    }
};

QColor insightContributorColor(const QString &name);

class CommitBarChart : public QWidget
{
public:
    CommitBarChart(QVector<int> counts, int sharedMax, const QColor &color,
                   QWidget *parent = nullptr)
        : QWidget(parent), m_counts(std::move(counts)),
          m_max(qMax(1, sharedMax)), m_color(color)
    {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setMinimumHeight(30);
    }

protected:
    QSize sizeHint() const override { return QSize(240, 30); }

    void paintEvent(QPaintEvent *) override
    {
        if (m_counts.isEmpty())
            return;
        QPainter p(this);
        const int n = m_counts.size();
        const qreal slot = qreal(width()) / n;
        const int barW = qMax(1, int(slot) - 2);
        const int h = height();
        p.fillRect(0, h - 1, width(), 1, QColor(255, 255, 255, 28));
        for (int i = 0; i < n; ++i) {
            const int c = m_counts.at(i);
            if (c <= 0)
                continue;
            const int bh = qMax(2, qRound(qreal(c) / m_max * (h - 2)));
            p.fillRect(QRectF(i * slot, h - bh, barW, bh), m_color);
        }
    }

private:
    QVector<int> m_counts;
    int m_max;
    QColor m_color;
};

class TokenUsageMiniChart : public QWidget
{
public:
    enum Window { FiveHour = 0, Weekly = 1, Fable = 2, WindowCount = 3 };

    explicit TokenUsageMiniChart(const QString &title =
                                     QStringLiteral("Claude Code usage"),
                                 bool remainingMode = false,
                                 int windows = 2,
                                 QWidget *parent = nullptr)
        : QWidget(parent), m_title(title), m_remainingMode(remainingMode)
    {
        const int count = qMax(1, windows);
        m_pct.fill(-1, count);
        m_note.resize(count);
        for (int i = 0; i < count; ++i)
            m_labels.append(defaultLabel(i));
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        setCursor(Qt::PointingHandCursor);
        resizeForWindowCount();
        refreshTooltip();
    }

    QVector<int> percentages() const { return m_pct; }
    int filledPercent(int index) const
    {
        const int percentage = m_pct.value(index, -1);
        if (percentage < 0)
            return -1;
        return m_remainingMode ? 100 - percentage : percentage;
    }

    // Codex can return several independently metered buckets (the general
    // weekly balance plus model-specific windows). Keep the compact chart
    // data-driven while preserving the named slots used by Claude callers.
    void setUsageRows(const QStringList &labels, const QVector<int> &percentages,
                      const QStringList &notes)
    {
        const int count = qMax(
            1, qMax(int(labels.size()),
                    qMax(int(percentages.size()), int(notes.size()))));
        QStringList nextLabels = labels;
        QVector<int> nextPercentages(count, -1);
        QStringList nextNotes = notes;
        while (nextLabels.size() < count)
            nextLabels.append(defaultLabel(nextLabels.size()));
        for (int i = 0; i < qMin(count, int(percentages.size())); ++i)
            nextPercentages[i] = percentages.at(i);
        nextNotes.resize(count);
        for (int &percentage : nextPercentages)
            percentage = percentage < 0 ? -1 : qBound(0, percentage, 100);
        if (m_labels == nextLabels && m_pct == nextPercentages &&
            m_note == nextNotes)
            return;
        m_labels = nextLabels;
        m_pct = nextPercentages;
        m_note = nextNotes;
        resizeForWindowCount();
        refreshTooltip();
        update();
    }

    void setUsage(Window window, int percent)
    {
        ensureWindow(static_cast<int>(window));
        int &slot = m_pct[static_cast<int>(window)];
        const int clamped = percent < 0 ? -1 : qBound(0, percent, 100);
        if (slot == clamped)
            return;
        slot = clamped;
        refreshTooltip();
        update();
    }
    void setUsage(bool weekly, int percent)
    {
        setUsage(weekly ? Weekly : FiveHour, percent);
    }

    void setReset(Window window, const QString &remaining)
    {
        setWindowNote(window,
                      remaining.isEmpty()
                          ? QString()
                          : QStringLiteral("resets in %1").arg(remaining));
    }
    void setReset(bool weekly, const QString &remaining)
    {
        setReset(weekly ? Weekly : FiveHour, remaining);
    }

    void setWindowNote(Window window, const QString &note)
    {
        ensureWindow(static_cast<int>(window));
        QString &slot = m_note[static_cast<int>(window)];
        if (slot == note)
            return;
        slot = note;
        refreshTooltip();
    }
    void setWindowNote(bool weekly, const QString &note)
    {
        setWindowNote(weekly ? Weekly : FiveHour, note);
    }

    void setRemaining(bool weekly, int percent, const QString &note)
    {
        setUsage(weekly, percent);
        setWindowNote(weekly, note);
    }

    void flashRefresh(bool ok)
    {
        m_flash = ok ? 1 : -1;
        update();
        const int token = ++m_flashToken;
        QTimer::singleShot(kFlashMs, this, [this, token] {
            if (token != m_flashToken) // a newer flash owns the box now
                return;
            m_flash = 0;
            update();
        });
    }

    QString stats() const { return m_stats; }
    void setStats(const QString &stats)
    {
        if (m_stats == stats)
            return;
        m_stats = stats;
        refreshTooltip();
    }

    std::function<void()> onHover;
    std::function<void(const QPoint &)> onClick;
    std::function<void(const QPoint &)> onContextMenu;
    std::function<void()> onChanged;

    QColor barColor(int pct) const
    {
        if (m_remainingMode) {
            if (pct <= 10)
                return QColor("#f85149"); // red: nearly out of window
            if (pct <= 30)
                return QColor("#d29922"); // amber: low remaining time
            return QColor("#3fb950");     // green: plenty remaining
        }
        if (pct >= 90)
            return QColor("#f85149"); // red: near the cap
        if (pct >= 70)
            return QColor("#d29922"); // amber: getting close
        return QColor("#3fb950");     // green: plenty left
    }

protected:
    void enterEvent(QEnterEvent *) override
    {
        if (onHover)
            onHover();
    }
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        const int windows = m_pct.size();
        const qreal totalW = windows * kBarW + (windows - 1) * kGap;
        qreal x = (width() - totalW) / 2.0;
        const qreal top = 3.0;
        const qreal trackH = height() - 6.0;
        for (int i = 0; i < windows; ++i) {
            const QRectF track(x, top, kBarW, trackH);
            p.setPen(Qt::NoPen);
            p.setBrush(textColor(38));
            p.drawRoundedRect(track, kBarW / 2.0, kBarW / 2.0);
            if (m_pct[i] >= 0) {
                const int percentage = qBound(0, m_pct[i], 100);
                // All gauges visualize quota consumed. Codex supplies the
                // inverse value (quota remaining), so flip only its painted
                // height while preserving the remaining value in the tooltip
                // and for the low-balance warning colour.
                const int fillPercentage =
                    m_remainingMode ? 100 - percentage : percentage;
                if (fillPercentage > 0) {
                    const qreal fillH = trackH * fillPercentage / 100.0;
                    const QRectF fill(x, top + trackH - fillH, kBarW, fillH);
                    p.setBrush(barColor(percentage));
                    p.drawRoundedRect(fill, kBarW / 2.0, kBarW / 2.0);
                }
            }
            x += kBarW + kGap;
        }

        if (m_flash != 0) {
            QPen pen(m_flash > 0 ? QColor("#3fb950") : QColor("#f85149"));
            pen.setWidthF(1.5);
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(QRectF(rect()).adjusted(0.75, 0.75, -0.75, -0.75),
                              4.0, 4.0);
        }
    }
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && onClick) {
            onClick(mapToGlobal(event->position().toPoint()));
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }
    void contextMenuEvent(QContextMenuEvent *event) override
    {
        if (onContextMenu) {
            onContextMenu(event->globalPos());
            event->accept();
            return;
        }
        QWidget::contextMenuEvent(event);
    }

private:
    static QString defaultLabel(int index)
    {
        static const char *labels[WindowCount] = {"5-hour", "Weekly", "Fable"};
        return index < WindowCount
                   ? QString::fromLatin1(labels[index])
                   : QStringLiteral("Limit %1").arg(index + 1);
    }
    void ensureWindow(int index)
    {
        if (index < m_pct.size())
            return;
        const int oldSize = m_pct.size();
        m_pct.resize(index + 1);
        m_note.resize(index + 1);
        for (int i = oldSize; i <= index; ++i) {
            m_pct[i] = -1;
            m_labels.append(defaultLabel(i));
        }
        resizeForWindowCount();
    }
    void resizeForWindowCount()
    {
        const int windows = qMax(1, m_pct.size());
        setFixedSize(qRound(windows * kBarW + (windows - 1) * kGap) + 6, 24);
    }
    QColor textColor(int alpha) const
    {
        QColor c = palette().color(QPalette::WindowText);
        c.setAlpha(alpha);
        return c;
    }
    void refreshTooltip()
    {
        auto line = [this](const QString &label, int v, const QString &note) {
            const QString value =
                v < 0 ? QString::fromUtf8("\xE2\x80\x94") // em dash
                      : QStringLiteral("%1%").arg(v);
            QString s = m_remainingMode
                            ? QStringLiteral("%1 remaining: %2").arg(label, value)
                            : QStringLiteral("%1: %2").arg(label, value);
            if (!note.isEmpty())
                s += QString::fromUtf8(" \xC2\xB7 ") + note; // ·
            return s;
        };
        QString tip = m_title;
        for (int i = 0; i < m_pct.size(); ++i)
            tip += QLatin1Char('\n')
                   + line(m_labels.value(i, defaultLabel(i)), m_pct[i],
                          m_note.value(i));
        if (!m_stats.isEmpty())
            tip += QStringLiteral("\n\n") + m_stats;
        tip += QStringLiteral("\n\nClick to open the Usage page. Right-click to "
                              "manage accounts.");
        setToolTip(tip);
        if (onChanged)
            onChanged();
    }

    static constexpr qreal kBarW = 4.0;
    static constexpr qreal kGap = 3.0;
    static constexpr int kFlashMs = 900; // how long the hover box stays up

    QString m_title;
    bool m_remainingMode = false;
    QStringList m_labels;
    QVector<int> m_pct;
    QStringList m_note; // extra tooltip text per displayed window
    QString m_stats; // per-session token/cost line, shown under the gauges
    int m_flash = 0;     // 0 = none, 1 = refreshed (green), -1 = failed (red)
    int m_flashToken = 0; // guards against an older flash clearing a newer one
};

class TokenUsageMenuRow : public QWidget
{
public:
    TokenUsageMenuRow(const QString &label, const QString &value,
                      const QString &resetNote, int percent,
                      QWidget *parent = nullptr, qint64 resetAtMs = 0,
                      qint64 windowMs = 0)
        : QWidget(parent), m_label(label), m_value(value),
          m_resetNote(resetNote), m_percent(percent), m_resetAtMs(resetAtMs),
          m_windowMs(windowMs)
    {
        // Keep the availability/reset line clear of the next menu action.
        // A 34px row leaves the two text baselines only a few pixels from the
        // neighbouring action at the default menu font, which makes adjacent
        // usage entries visually run together.
        setFixedHeight(40);
        // The second line carries both availability and the reset countdown.
        // Reserve enough room that values such as "100% available" remain
        // visible instead of leaving only the trailing word "remaining".
        setMinimumWidth(380);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setAccessibleName(QStringLiteral("%1 usage").arg(label));
        setToolTip(QStringLiteral("%1: %2%3")
                       .arg(label, value,
                            resetNote.isEmpty()
                                ? QString()
                                : QStringLiteral(" · %1").arg(resetNote)));
    }

    void setUsageData(const QString &label, const QString &value,
                      const QString &resetNote, int percent,
                      qint64 resetAtMs = 0, qint64 windowMs = 0)
    {
        m_label = label;
        m_value = value;
        m_resetNote = resetNote;
        m_percent = percent;
        m_resetAtMs = resetAtMs;
        m_windowMs = windowMs;
        refreshActionButton();
        setAccessibleName(QStringLiteral("%1 usage").arg(label));
        setToolTip(QStringLiteral("%1: %2%3")
                       .arg(label, value,
                            resetNote.isEmpty()
                                ? QString()
                                : QStringLiteral(" · %1").arg(resetNote)));
        update();
    }

    // When the window resets, in epoch milliseconds; 0 when it has no
    // upcoming reset (credits, an unread window, one that is already ready).
    qint64 resetAtMs() const { return m_resetAtMs; }

    // A small button at the right edge of the row, shown only while the
    // window has a reset still ahead of it. The row owns its placement.
    void setActionButton(QAbstractButton *button)
    {
        m_actionButton = button;
        if (!button)
            return;
        button->setParent(this);
        button->setFixedSize(kActionButtonSide, kActionButtonSide);
        setMinimumWidth(380 + kActionButtonSide + 4);
        placeActionButton();
        refreshActionButton();
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        placeActionButton();
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const QColor text = palette().color(QPalette::WindowText);
        const QColor muted(text.red(), text.green(), text.blue(), 150);
        const QColor track(text.red(), text.green(), text.blue(), 38);
        // The value stays right-aligned against the button's column even
        // while the button is hidden, so rows in one menu line up.
        const int trailing = m_actionButton ? kActionButtonSide + 4 : 0;
        painter.setPen(text);
        painter.drawText(QRectF(4, 3, 90, 17), Qt::AlignLeft | Qt::AlignVCenter,
                         m_label);

        const QRectF bar(96, 8, 132, 8);
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const bool paced = m_windowMs > 0 && m_resetAtMs > now;
        QPainterPath shape;
        shape.addRoundedRect(bar, 4, 4);
        if (paced) {
            const qint64 step =
                m_windowMs > kDayMs ? kDayMs : qint64(60 * 60 * 1000);
            for (qint64 t = step; t < m_windowMs; t += step) {
                const qreal x = std::round(
                    bar.left() + bar.width() * t / double(m_windowMs));
                QPainterPath gap;
                gap.addRect(QRectF(x - 1, bar.top() - 1, 2, bar.height() + 2));
                shape = shape.subtracted(gap);
            }
        }
        painter.save();
        painter.setClipPath(shape);
        painter.setPen(Qt::NoPen);
        painter.setBrush(track);
        painter.drawRect(bar);
        if (m_percent >= 0) {
            const qreal width = bar.width() * qBound(0, m_percent, 100) / 100.0;
            if (width > 0) {
                QColor fill = m_percent >= 90 ? QColor("#f85149")
                                              : m_percent >= 70
                                                    ? QColor("#d29922")
                                                    : QColor("#3fb950");
                painter.setBrush(fill);
                painter.drawRect(QRectF(bar.left(), bar.top(), width,
                                        bar.height()));
            }
        }
        painter.restore();
        if (paced) {
            const qreal elapsed = qBound(
                0.0, 1.0 - (m_resetAtMs - now) / double(m_windowMs), 1.0);
            const qreal x = bar.left() + bar.width() * elapsed;
            painter.setPen(QPen(text, 1.5));
            painter.drawLine(QPointF(x, bar.top() - 4),
                             QPointF(x, bar.top() - 1));
            painter.drawLine(QPointF(x, bar.bottom() + 1),
                             QPointF(x, bar.bottom() + 4));
        }

        painter.setPen(text);
        painter.drawText(QRectF(234, 3, width() - 238 - trailing, 17),
                         Qt::AlignRight | Qt::AlignVCenter, m_value);
        if (!m_resetNote.isEmpty()) {
            painter.setPen(muted);
            painter.drawText(QRectF(96, 22, width() - 100, 15),
                             Qt::AlignLeft | Qt::AlignVCenter, m_resetNote);
        }
    }

private:
    static constexpr int kActionButtonSide = 22;
    static constexpr qint64 kDayMs = 24LL * 60 * 60 * 1000;

    void placeActionButton()
    {
        if (m_actionButton)
            m_actionButton->move(width() - kActionButtonSide - 2,
                                 (height() - kActionButtonSide) / 2);
    }

    void refreshActionButton()
    {
        if (m_actionButton)
            m_actionButton->setVisible(
                m_resetAtMs > QDateTime::currentMSecsSinceEpoch());
    }

    QString m_label;
    QString m_value;
    QString m_resetNote;
    int m_percent = -1;
    qint64 m_resetAtMs = 0;
    qint64 m_windowMs = 0;
    QPointer<QAbstractButton> m_actionButton;
};

struct SparklineHoverContent {
    QString title;
    QString value;
    QString unit;
    QVector<double> history;
    double max = 100.0;
    int maxPoints = 60;
    QColor color;
    QString detail;
};

class SparklineHoverCard : public QWidget
{
public:
    using Source = std::function<SparklineHoverContent()>;

    static void showFor(QWidget *anchor, Source source)
    {
        if (!anchor || !anchor->window())
            return;
        SparklineHoverCard *card = cardFor(anchor->window());
        card->m_anchor = anchor;
        card->m_source = std::move(source);
        card->refresh();
        card->m_timer.start();
    }

    static void hideFor(QWidget *anchor)
    {
        if (!anchor || !anchor->window())
            return;
        auto *card = existingCard(anchor->window());
        if (card && card->m_anchor == anchor)
            card->dismiss();
    }

    static bool isShownInWindow(const QWidget *anchor)
    {
        if (!anchor || !anchor->window())
            return false;
        auto *card = existingCard(anchor->window());
        return card && card->isVisible();
    }

    static bool isShownFor(const QWidget *anchor)
    {
        if (!anchor || !anchor->window())
            return false;
        auto *card = existingCard(anchor->window());
        return card && card->isVisible() && card->m_anchor == anchor;
    }

    static QString detailShownFor(const QWidget *anchor)
    {
        if (!isShownFor(anchor))
            return QString();
        return existingCard(anchor->window())->m_content.detail;
    }

    static QString formatNumber(double value)
    {
        const bool fractional = qAbs(value) < 10.0 &&
                                !qFuzzyCompare(value + 1.0, std::round(value) + 1.0);
        return QString::number(value, 'f', fractional ? 1 : 0);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const bool dark = currentThemeIsDark();
        const QColor surface =
            Theme::remapColor(QColor(dark ? "#161b22" : "#ffffff"));
        const QColor border =
            Theme::remapColor(QColor(dark ? "#30363d" : "#d0d7de"));
        const QColor ink = Theme::remapColor(QColor(dark ? "#e6edf3" : "#1f2328"));
        const QColor muted =
            Theme::remapColor(QColor(dark ? "#8b949e" : "#656d76"));
        p.fillRect(rect(), surface);
        p.setPen(QPen(border, 1));
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5));

        const Layout layout = measure();
        QFont titleFont = font();
        titleFont.setPixelSize(12);
        titleFont.setBold(true);
        p.setFont(titleFont);
        p.setPen(ink);
        p.drawText(layout.title, Qt::AlignLeft | Qt::AlignVCenter,
                   QFontMetrics(titleFont).elidedText(
                       m_content.title, Qt::ElideRight,
                       layout.title.width() - layout.valueWidth - 8));
        p.drawText(layout.title, Qt::AlignRight | Qt::AlignVCenter,
                   m_content.value);

        QFont smallFont = font();
        smallFont.setPixelSize(10);
        p.setFont(smallFont);
        if (!layout.chart.isEmpty())
            paintChart(p, layout.chart, border, muted, smallFont);

        if (!layout.stats.isEmpty()) {
            p.setPen(muted);
            p.drawText(layout.stats, Qt::AlignLeft | Qt::AlignVCenter,
                       statsLine());
        }
        if (!layout.detail.isEmpty()) {
            QFont detailFont = font();
            detailFont.setPixelSize(11);
            p.setFont(detailFont);
            p.setPen(ink);
            p.drawText(layout.detail, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                       m_content.detail);
        }
    }

private:
    static constexpr int kWidth = 280;
    static constexpr int kPadding = 10;
    static constexpr int kChartHeight = 72;

    struct Layout {
        QRect title;
        QRect chart;
        QRect stats;
        QRect detail;
        int valueWidth = 0;
        int height = 0;
    };

    explicit SparklineHoverCard(QWidget *window)
        : QWidget(window, Qt::ToolTip | Qt::FramelessWindowHint)
    {
        setObjectName(QStringLiteral("sparklineHoverCard"));
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        m_timer.setInterval(250);
        QObject::connect(&m_timer, &QTimer::timeout, this, [this] {
            if (!anchorHovered()) {
                dismiss();
                return;
            }
            refresh();
        });
    }

    static SparklineHoverCard *cardFor(QWidget *window)
    {
        SparklineHoverCard *card = existingCard(window);
        return card ? card : new SparklineHoverCard(window);
    }

    static SparklineHoverCard *existingCard(const QWidget *window)
    {
        return dynamic_cast<SparklineHoverCard *>(window->findChild<QWidget *>(
            QStringLiteral("sparklineHoverCard"), Qt::FindDirectChildrenOnly));
    }

    bool anchorHovered() const
    {
        if (!m_anchor || !m_anchor->isVisible())
            return false;
        const QRect global(m_anchor->mapToGlobal(QPoint(0, 0)), m_anchor->size());
        return global.adjusted(-2, -2, 2, 2).contains(QCursor::pos());
    }

    void dismiss()
    {
        m_timer.stop();
        m_anchor = nullptr;
        m_source = nullptr;
        hide();
    }

    void refresh()
    {
        if (!m_source || !m_anchor)
            return;
        m_content = m_source();
        const Layout layout = measure();
        resize(kWidth, layout.height);
        place();
        if (!isVisible())
            show();
        raise();
        update();
    }

    void place()
    {
        const QPoint anchorTop = m_anchor->mapToGlobal(QPoint(0, 0));
        QScreen *screen = m_anchor->screen();
        const QRect bounds = screen ? screen->availableGeometry()
                                    : QRect(anchorTop - QPoint(2000, 2000),
                                            QSize(4000, 4000));
        int x = anchorTop.x() + m_anchor->width() / 2 - width() / 2;
        int y = anchorTop.y() - height() - 6;
        if (y < bounds.top())
            y = anchorTop.y() + m_anchor->height() + 6;
        x = qBound(bounds.left() + 4, x, qMax(bounds.left() + 4,
                                              bounds.right() - width() - 4));
        y = qMin(y, qMax(bounds.top(), bounds.bottom() - height() - 4));
        move(x, y);
    }

    Layout measure() const
    {
        Layout layout;
        const int inner = kWidth - kPadding * 2;
        int y = kPadding;
        QFont titleFont = font();
        titleFont.setPixelSize(12);
        titleFont.setBold(true);
        const QFontMetrics titleMetrics(titleFont);
        layout.valueWidth = titleMetrics.horizontalAdvance(m_content.value);
        layout.title = QRect(kPadding, y, inner, titleMetrics.height());
        y += layout.title.height() + 6;
        QFont smallFont = font();
        smallFont.setPixelSize(10);
        const int smallHeight = QFontMetrics(smallFont).height();
        if (m_content.history.size() >= 2) {
            layout.chart = QRect(kPadding, y, inner, kChartHeight + smallHeight + 2);
            y += layout.chart.height() + 4;
            layout.stats = QRect(kPadding, y, inner, smallHeight);
            y += smallHeight + 6;
        }
        if (!m_content.detail.isEmpty()) {
            QFont detailFont = font();
            detailFont.setPixelSize(11);
            const QRect needed = QFontMetrics(detailFont).boundingRect(
                QRect(0, 0, inner, 2000), Qt::AlignLeft | Qt::TextWordWrap,
                m_content.detail);
            layout.detail = QRect(kPadding, y, inner, needed.height());
            y += needed.height() + 4;
        }
        layout.height = y + kPadding - 4;
        return layout;
    }

    QString statsLine() const
    {
        const QVector<double> &h = m_content.history;
        double low = h.first();
        double high = h.first();
        double sum = 0.0;
        for (double v : h) {
            low = qMin(low, v);
            high = qMax(high, v);
            sum += v;
        }
        const QString unit = m_content.unit;
        return QString::fromUtf8("now %1%5 \xC2\xB7 avg %2%5 \xC2\xB7 min %3%5 "
                                 "\xC2\xB7 peak %4%5")
            .arg(formatNumber(h.last()), formatNumber(sum / h.size()),
                 formatNumber(low), formatNumber(high), unit);
    }

    void paintChart(QPainter &p, const QRect &frame, const QColor &grid,
                    const QColor &muted, const QFont &axisFont) const
    {
        const int axisHeight = QFontMetrics(axisFont).height();
        const QRectF area(frame.left(), frame.top(), frame.width(),
                          frame.height() - axisHeight - 2);
        QPen gridPen(grid, 1);
        p.setPen(gridPen);
        p.drawLine(QPointF(area.left(), area.bottom()),
                   QPointF(area.right(), area.bottom()));
        gridPen.setStyle(Qt::DotLine);
        p.setPen(gridPen);
        p.drawLine(QPointF(area.left(), area.top() + area.height() / 2),
                   QPointF(area.right(), area.top() + area.height() / 2));
        p.drawLine(QPointF(area.left(), area.top()),
                   QPointF(area.right(), area.top()));

        const QVector<double> &h = m_content.history;
        const double maxValue = m_content.max > 0 ? m_content.max : 1.0;
        const int points = qMax(2, m_content.maxPoints);
        const double step = area.width() / double(points - 1);
        const int n = h.size();
        QPolygonF curve;
        for (int i = 0; i < n; ++i) {
            const double x = area.right() - (n - 1 - i) * step;
            const double norm = qBound(0.0, h.at(i) / maxValue, 1.0);
            curve << QPointF(x, area.bottom() - norm * (area.height() - 2));
        }
        QColor line = m_content.color.isValid() ? m_content.color : QColor("#58a6ff");
        QPolygonF fill = curve;
        fill << QPointF(curve.last().x(), area.bottom())
             << QPointF(curve.first().x(), area.bottom());
        QColor under = line;
        under.setAlpha(60);
        p.setPen(Qt::NoPen);
        p.setBrush(under);
        p.drawPolygon(fill);
        QPen pen(line, 2);
        pen.setJoinStyle(Qt::RoundJoin);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawPolyline(curve);
        p.setPen(Qt::NoPen);
        p.setBrush(line);
        p.drawEllipse(curve.last(), 3, 3);

        p.setFont(axisFont);
        p.setPen(muted);
        p.drawText(QRectF(area.left() + 2, area.top() + 1, area.width() - 4,
                          axisHeight),
                   Qt::AlignLeft | Qt::AlignTop,
                   formatNumber(maxValue) + m_content.unit);
        const QRectF axis(area.left(), area.bottom() + 2, area.width(), axisHeight);
        p.drawText(axis, Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("%1 s ago").arg(points - 1));
        p.drawText(axis, Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("now"));
    }

    QPointer<QWidget> m_anchor;
    Source m_source;
    SparklineHoverContent m_content;
    QTimer m_timer;
};

constexpr int kResourceSparklineSide = 34;
constexpr int kResourceSparklineCompactSide = 19;

class ResourceSparkline : public QWidget
{
public:
    explicit ResourceSparkline(const QString &label, QWidget *parent = nullptr,
                               int side = kResourceSparklineSide,
                               int maxPoints = 60)
        : QWidget(parent), m_label(label), m_maxPoints(qMax(2, maxPoints))
    {
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        setFixedSize(side, side); // a little button-sized square
        setCursor(Qt::PointingHandCursor);
    }

    void addSample(double value, double maxValue, const QString &valueText)
    {
        m_max = maxValue > 0 ? maxValue : 100.0;
        m_value = valueText;
        m_history.append(value);
        while (m_history.size() > m_maxPoints)
            m_history.removeFirst();
        update();
    }

    void setSamples(const QVector<double> &values, double maxValue,
                    const QString &valueText)
    {
        m_loading = false;
        m_max = maxValue > 0 ? maxValue : 1.0;
        m_value = valueText;
        m_history = values.mid(qMax(0, values.size() - m_maxPoints));
        update();
    }

    void setLoading(bool loading)
    {
        m_loading = loading;
        if (loading) {
            m_value.clear();
            m_history.clear();
        }
        update();
    }

    void setHealthColorsEnabled(bool enabled)
    {
        m_healthColors = enabled;
        update();
    }

    void setUnit(const QString &unit) { m_unit = unit; }

    void setHoverCardEnabled(bool enabled) { m_hoverCard = enabled; }

    bool isCompact() const { return width() < 28; }

    SparklineHoverContent hoverContent() const
    {
        SparklineHoverContent content;
        content.title = accessibleName().isEmpty() ? m_label : accessibleName();
        content.value = m_loading ? QStringLiteral("…") : m_value;
        content.unit = m_unit;
        content.history = m_history;
        content.max = m_max;
        content.maxPoints = m_maxPoints;
        content.color = seriesColor(
            m_history.isEmpty() ? 0.0 : m_history.last() / m_max * 100.0);
        content.detail = toolTip();
        return content;
    }

    std::function<void()> onClicked; // invoked on a left-click

protected:
    bool event(QEvent *e) override
    {
        if (m_hoverCard && e->type() == QEvent::ToolTip) {
            SparklineHoverCard::showFor(this, [this] { return hoverContent(); });
            return true;
        }
        return QWidget::event(e);
    }

    void enterEvent(QEnterEvent *e) override
    {
        if (m_hoverCard && SparklineHoverCard::isShownInWindow(this))
            SparklineHoverCard::showFor(this, [this] { return hoverContent(); });
        QWidget::enterEvent(e);
    }

    void hideEvent(QHideEvent *e) override
    {
        SparklineHoverCard::hideFor(this);
        QWidget::hideEvent(e);
    }

    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton && onClicked)
            onClicked();
        QWidget::mousePressEvent(e);
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        const bool compact = isCompact();
        const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        QPainterPath cardPath;
        cardPath.addRoundedRect(box, compact ? 2 : 4, compact ? 2 : 4);
        QColor card = palette().color(QPalette::WindowText);
        card.setAlpha(28);
        p.setPen(Qt::NoPen);
        p.setBrush(card);
        p.drawPath(cardPath);

        if (m_loading) {
            QColor placeholder = palette().color(QPalette::WindowText);
            placeholder.setAlpha(42);
            p.setPen(Qt::NoPen);
            p.setBrush(placeholder);
            if (compact) {
                p.drawRoundedRect(box.adjusted(4, 7, -4, -7), 1, 1);
                return;
            }
            p.drawRoundedRect(box.adjusted(7, 9, -7, -16), 2, 2);
            p.drawRoundedRect(box.adjusted(10, 21, -10, -7), 2, 2);
            return;
        }

        const qreal inset = compact ? 1.0 : 1.5;
        const QRectF area = box.adjusted(inset, inset, -inset, -inset);
        const bool charted = area.height() >= 2 && m_history.size() >= 2;
        if (charted) {
            p.save();
            p.setClipPath(cardPath);
            const QColor line = seriesColor(m_history.last() / m_max * 100.0);
            const double step = area.width() / double(m_maxPoints - 1);
            const int n = m_history.size();
            QPolygonF curve;
            for (int i = 0; i < n; ++i) {
                const double x = area.right() - (n - 1 - i) * step;
                const double norm = qBound(0.0, m_history.at(i) / m_max, 1.0);
                curve << QPointF(x, area.bottom() - norm * area.height());
            }
            QPolygonF fill = curve;
            fill << QPointF(curve.last().x(), area.bottom())
                 << QPointF(curve.first().x(), area.bottom());
            QColor under = line;
            under.setAlpha(70);
            p.setBrush(under);
            p.setPen(Qt::NoPen);
            p.drawPolygon(fill);
            QPen pen(line);
            pen.setWidthF(compact ? 1.0 : 1.2);
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            p.drawPolyline(curve);
            p.restore();
        }

        if (compact) {
            if (charted)
                return;
            QFont tiny = font();
            tiny.setPixelSize(6);
            tiny.setBold(true);
            p.setFont(tiny);
            QColor lab = palette().color(QPalette::WindowText);
            lab.setAlpha(170);
            p.setPen(lab);
            p.drawText(box, Qt::AlignCenter, m_label);
            return;
        }

        QFont f = font();
        double pt = f.pointSizeF() > 0 ? qMin(8.0, f.pointSizeF()) : 7.0;
        const double avail = width() - 6;
        for (; pt > 5.5; pt -= 0.5) {
            f.setPointSizeF(pt);
            const QFontMetrics fm(f);
            if (fm.horizontalAdvance(m_label) <= avail &&
                fm.horizontalAdvance(m_value) <= avail)
                break;
        }
        f.setPointSizeF(pt);
        p.setFont(f);
        const QFontMetrics fm(f);
        const int lineH = fm.height();

        const double topY = (height() - lineH * 2) / 2.0;
        QColor lab = palette().color(QPalette::WindowText);
        lab.setAlpha(170);
        p.setPen(lab);
        p.drawText(QRectF(3, topY, width() - 6, lineH),
                   Qt::AlignVCenter | Qt::AlignHCenter, m_label);
        const double lastPct =
            m_history.isEmpty() ? 0.0 : m_history.last() / m_max * 100.0;
        QColor val = seriesColor(lastPct);
        val.setAlpha(210);
        p.setPen(val);
        p.drawText(QRectF(3, topY + lineH, width() - 6, lineH),
                   Qt::AlignVCenter | Qt::AlignHCenter, m_value);
    }

private:
    QColor seriesColor(double pct) const
    {
        if (m_healthColors)
            return gaugeColor(pct);
        QColor color = palette().color(QPalette::Highlight);
        if (!color.isValid())
            color = QColor("#58a6ff");
        return color;
    }

    static QColor gaugeColor(double pct)
    {
        if (pct >= 90)
            return QColor("#f85149"); // red: pegged
        if (pct >= 70)
            return QColor("#d29922"); // amber: getting busy
        return QColor("#3fb950");     // green: light load
    }

    QString m_label;
    QString m_value;
    QString m_unit;
    bool m_hoverCard = false;
    double m_max = 100.0;
    QVector<double> m_history;
    int m_maxPoints = 60;
    bool m_loading = false;
    bool m_healthColors = true;
};

class ResourceQuadrantSparkline : public QWidget
{
public:
    enum Resource {
        Cpu = 0,
        Memory,
        Swap,
        Disk,
        ResourceCount
    };

    explicit ResourceQuadrantSparkline(QWidget *parent = nullptr,
                                       int side = kResourceSparklineSide,
                                       int maxPoints = 60)
        : QWidget(parent), m_maxPoints(qMax(2, maxPoints))
    {
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        setFixedSize(side, side);
        setCursor(Qt::PointingHandCursor);
        setMouseTracking(true);
    }

    void addSample(Resource resource, double value, double maxValue)
    {
        Sample &sample = m_samples[resource];
        sample.max = maxValue > 0 ? maxValue : 100.0;
        sample.history.append(value);
        while (sample.history.size() > m_maxPoints)
            sample.history.removeFirst();
        update();
    }

    void setResourceToolTip(Resource resource, const QString &toolTip)
    {
        m_samples[resource].toolTip = toolTip;
    }

    void setClickHandler(Resource resource, std::function<void()> handler)
    {
        m_samples[resource].onClicked = std::move(handler);
    }

    SparklineHoverContent hoverContent(int resource) const
    {
        static const char *const titles[ResourceCount] = {
            "CPU used by this app", "Host memory", "Swap", "Drive space"};
        SparklineHoverContent content;
        if (resource < 0 || resource >= ResourceCount)
            return content;
        const Sample &sample = m_samples[resource];
        content.title = QString::fromLatin1(titles[resource]);
        content.unit = QStringLiteral("%");
        content.history = sample.history;
        content.max = sample.max;
        content.maxPoints = m_maxPoints;
        const double pct = sample.history.isEmpty()
                               ? 0.0
                               : sample.history.last() / sample.max * 100.0;
        content.value = sample.history.isEmpty()
                            ? QStringLiteral("…")
                            : SparklineHoverCard::formatNumber(pct) +
                                  QStringLiteral("%");
        content.color = gaugeColor(pct);
        content.detail = sample.toolTip;
        if (sample.onClicked)
            content.detail += QStringLiteral("\nClick for details");
        return content;
    }

protected:
    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::ToolTip) {
            const auto *helpEvent = static_cast<QHelpEvent *>(event);
            showHoverCard(resourceAt(helpEvent->pos()));
            return true;
        }
        return QWidget::event(event);
    }

    void enterEvent(QEnterEvent *event) override
    {
        if (SparklineHoverCard::isShownInWindow(this))
            showHoverCard(resourceAt(event->position().toPoint()));
        QWidget::enterEvent(event);
    }

    void hideEvent(QHideEvent *event) override
    {
        SparklineHoverCard::hideFor(this);
        QWidget::hideEvent(event);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        const int resource = resourceAt(event->pos());
        if (event->button() == Qt::LeftButton && resource >= 0 &&
            m_samples[resource].onClicked) {
            m_samples[resource].onClicked();
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        const int resource = resourceAt(event->pos());
        if (resource != m_hoveredResource && resource >= 0 &&
            SparklineHoverCard::isShownFor(this))
            showHoverCard(resource);
        m_hoveredResource = resource;
        QWidget::mouseMoveEvent(event);
    }

    void leaveEvent(QEvent *event) override
    {
        m_hoveredResource = -1;
        QWidget::leaveEvent(event);
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        for (int resource = 0; resource < ResourceCount; ++resource)
            paintQuadrant(p, resource, quadrantRect(resource));
    }

private:
    struct Sample {
        QVector<double> history;
        double max = 100.0;
        QString toolTip;
        std::function<void()> onClicked;
    };

    void showHoverCard(int resource)
    {
        if (resource < 0)
            return;
        m_hoveredResource = resource;
        SparklineHoverCard::showFor(
            this, [this, resource] { return hoverContent(resource); });
    }

    static QColor gaugeColor(double pct)
    {
        if (pct >= 90)
            return QColor("#f85149"); // red: pegged
        if (pct >= 70)
            return QColor("#d29922"); // amber: getting busy
        return QColor("#3fb950");     // green: light load
    }

    QRectF quadrantRect(int resource) const
    {
        constexpr qreal gap = 1.0;
        const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        const qreal cellWidth = (box.width() - gap) / 2.0;
        const qreal cellHeight = (box.height() - gap) / 2.0;
        const int row = resource / 2;
        const int column = resource % 2;
        return QRectF(box.left() + column * (cellWidth + gap),
                      box.top() + row * (cellHeight + gap), cellWidth, cellHeight);
    }

    int resourceAt(const QPoint &position) const
    {
        for (int resource = 0; resource < ResourceCount; ++resource) {
            if (quadrantRect(resource).contains(position))
                return resource;
        }
        return -1;
    }

    void paintQuadrant(QPainter &p, int resource, const QRectF &box) const
    {
        QPainterPath cardPath;
        cardPath.addRoundedRect(box, 2, 2);
        QColor card = palette().color(QPalette::WindowText);
        card.setAlpha(28);
        p.setPen(Qt::NoPen);
        p.setBrush(card);
        p.drawPath(cardPath);

        const Sample &sample = m_samples[resource];
        const QRectF area = box.adjusted(1.0, 1.0, -1.0, -1.0);
        if (area.height() < 2 || sample.history.size() < 2)
            return;

        p.save();
        p.setClipPath(cardPath);
        const QColor line = gaugeColor(sample.history.last() / sample.max * 100.0);
        const double step = area.width() / double(m_maxPoints - 1);
        const int n = sample.history.size();
        QPolygonF curve;
        for (int i = 0; i < n; ++i) {
            const double x = area.right() - (n - 1 - i) * step;
            const double norm = qBound(0.0, sample.history.at(i) / sample.max, 1.0);
            curve << QPointF(x, area.bottom() - norm * area.height());
        }
        QPolygonF fill = curve;
        fill << QPointF(curve.last().x(), area.bottom())
             << QPointF(curve.first().x(), area.bottom());
        QColor under = line;
        under.setAlpha(70);
        p.setBrush(under);
        p.setPen(Qt::NoPen);
        p.drawPolygon(fill);
        QPen pen(line);
        pen.setWidthF(1.0);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawPolyline(curve);
        p.restore();
    }

    Sample m_samples[ResourceCount];
    int m_maxPoints = 60;
    int m_hoveredResource = -1;
};

class ProcessMemorySparkline : public QWidget
{
public:
    static constexpr int kMaxPoints = 24; // ~2 minutes at the panel's 5s refresh

    explicit ProcessMemorySparkline(QWidget *parent = nullptr) : QWidget(parent)
    {
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        setFixedSize(kSide, kSide);
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }

    void setHistory(const QVector<double> &history, double maxValue)
    {
        m_history = history;
        m_max = maxValue > 0 ? maxValue : 1.0;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        QPainterPath cardPath;
        cardPath.addRoundedRect(box, 3, 3);
        QColor card = palette().color(QPalette::WindowText);
        card.setAlpha(28);
        p.setPen(Qt::NoPen);
        p.setBrush(card);
        p.drawPath(cardPath);
        const QRectF area = box.adjusted(1.5, 1.5, -1.5, -1.5);
        if (m_history.isEmpty() || area.height() < 2 || area.width() < 2)
            return;

        p.save();
        p.setClipPath(cardPath);
        const double norm = qBound(0.0, m_history.last() / m_max, 1.0);
        const QColor line = norm >= 0.66   ? QColor("#f85149")
                            : norm >= 0.33 ? QColor("#d29922")
                                           : QColor("#3fb950");
        const int n = m_history.size();
        const double step = area.width() / double(kMaxPoints - 1);
        QPolygonF curve;
        for (int i = 0; i < n; ++i) {
            const double x =
                n > 1 ? area.right() - (n - 1 - i) * step : area.left();
            const double v = qBound(0.0, m_history.at(i) / m_max, 1.0);
            curve << QPointF(x, area.bottom() - v * area.height());
        }
        if (n == 1)
            curve << QPointF(area.right(), curve.first().y());
        QPolygonF fill = curve;
        fill << QPointF(curve.last().x(), area.bottom())
             << QPointF(curve.first().x(), area.bottom());
        QColor under = line;
        under.setAlpha(70);
        p.setBrush(under);
        p.setPen(Qt::NoPen);
        p.drawPolygon(fill);
        QPen pen(line);
        pen.setWidthF(1.2);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawPolyline(curve);
        p.restore();
    }

private:
    static constexpr int kSide = 34; // fits a table row without growing it
    QVector<double> m_history;
    double m_max = 1.0;
};

class RelaySpeedDot : public QWidget
{
public:
    explicit RelaySpeedDot(QWidget *parent = nullptr) : QWidget(parent)
    {
        setFixedSize(kWidth, kHeight);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        if (parent)
            parent->installEventFilter(this);
        reposition();
    }

    void setLatency(int ms)
    {
        ms = qMax(0, ms);
        if (!m_unreachable && m_latencyMs == ms)
            return;
        m_latencyMs = ms;
        m_unreachable = false;
        update();
    }

    void setUnreachable()
    {
        if (m_unreachable)
            return;
        m_unreachable = true;
        update();
    }

    int latencyMs() const { return m_latencyMs; }
    bool unreachable() const { return m_unreachable; }

    static QColor speedColor(int ms, bool unreachable)
    {
        if (unreachable)
            return QColor("#f85149"); // red: not answering
        if (ms < 0)
            return Theme::remapColor(QColor("#8b949e")); // grey: still measuring
        if (ms >= 1000)
            return QColor("#f85149"); // red
        if (ms >= 300)
            return QColor("#d29922"); // amber
        return QColor("#3fb950");     // green
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == parentWidget() && (event->type() == QEvent::Resize ||
                                          event->type() == QEvent::Show))
            reposition();
        return QWidget::eventFilter(watched, event);
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const QColor colour = speedColor(m_latencyMs, m_unreachable);

        const QRectF plate(rect());
        const qreal radius = plate.height() / 2.0;
        QColor wash = colour;
        wash.setAlpha(52);
        p.setPen(Qt::NoPen);
        p.setBrush(palette().color(QPalette::Window));
        p.drawRoundedRect(plate, radius, radius);
        p.setBrush(wash);
        p.drawRoundedRect(plate, radius, radius);

        const QPointF centre = plate.center();
        QPolygonF caret;
        caret << QPointF(centre.x() - 3.0, centre.y() - 1.6)
              << QPointF(centre.x() + 3.0, centre.y() - 1.6)
              << QPointF(centre.x(), centre.y() + 2.4);
        p.setBrush(colour);
        p.drawPolygon(caret);
    }

private:
    void reposition()
    {
        if (QWidget *owner = parentWidget())
            move(qMax(0, owner->width() - width()),
                 qMax(0, owner->height() - height()));
    }

    static constexpr int kWidth = 14;
    static constexpr int kHeight = 10;
    int m_latencyMs = -1;       // last measured round-trip; -1 = unknown/probing
    bool m_unreachable = false; // relay failed to answer the last probe
};

namespace ChromeDotGrid {

constexpr int kRows = 3;
constexpr int kPitch = 7;
constexpr int kGridHeight = kRows * kPitch;

QFont captionFont();

int captionHeight();

int captionWidth(const QString &text);

int totalHeight();

void paintCaption(QPainter &p, const QWidget *widget,
                         const QString &text);

int widthFor(int columns, const QString &caption);

constexpr int kAnimationIntervalMs = 60;

QTimer *animationClock();

void holdAnimationClock(bool *held, bool want);

} // namespace ChromeDotGrid

QColor agentRunningFireballColor();

class AgentDotMatrix : public QWidget
{
public:
    struct Dot {
        int sessionId = 0;
        QColor color;
        bool running = false;
        double intensity = 0.0; // 0..1 live-output (bytes) meter
        double throughput = 0.0;

        bool operator==(const Dot &other) const
        {
            return sessionId == other.sessionId && color == other.color &&
                   running == other.running &&
                   qFuzzyCompare(intensity + 1.0, other.intensity + 1.0) &&
                   qFuzzyCompare(throughput + 1.0, other.throughput + 1.0);
        }
        bool operator!=(const Dot &other) const { return !(*this == other); }
    };

    static constexpr const char *kCaption = "Agents";

    explicit AgentDotMatrix(QWidget *parent = nullptr) : QWidget(parent)
    {
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        setFixedHeight(ChromeDotGrid::totalHeight());
        setFixedWidth(0); // nothing to show until the first setDots()
        setCursor(Qt::PointingHandCursor);
        hide();
        connect(ChromeDotGrid::animationClock(), &QTimer::timeout, this, [this] {
            if (!m_ticking)
                return;
            m_phase += 0.045;
            if (m_phase >= 1.0)
                m_phase -= 1.0;
            update();
        });
    }

    ~AgentDotMatrix() override
    {
        ChromeDotGrid::holdAnimationClock(&m_ticking, false);
    }

    void setDots(const QVector<Dot> &dots)
    {
        // adhoc #2104: the scanner tick calls this on every beat whether or not
        // a square moved, and setFixedWidth() invalidates the whole top chrome's
        // layout each time — 582 ms of blocked GUI thread in one sampled stall.
        // An unchanged row is already on screen: leave it alone.
        if (dots == m_dots)
            return;
        if (m_ticking && sameSquares(dots, m_dots)) {
            m_dots = dots;
            return;
        }
        m_dots = dots;
        const int columns = (m_dots.size() + kRows - 1) / kRows;
        const int width =
            ChromeDotGrid::widthFor(columns, QLatin1String(kCaption));
        if (width != maximumWidth() || width != minimumWidth())
            setFixedWidth(width);
        bool anyRunning = false;
        for (const Dot &d : std::as_const(m_dots))
            anyRunning = anyRunning || d.running;
        ChromeDotGrid::holdAnimationClock(&m_ticking, anyRunning);
        update();
    }

    int shownCount() const { return m_dots.size(); }

    bool sweeping() const { return m_ticking; }

    std::function<void(int)> onDotClicked;

protected:
    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton && onDotClicked) {
            const int index = dotAt(e->position().toPoint());
            onDotClicked(index >= 0 ? m_dots.at(index).sessionId : 0);
            e->accept();
            return;
        }
        QWidget::mousePressEvent(e);
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        for (int i = 0; i < m_dots.size(); ++i) {
            const Dot &dot = m_dots.at(i);
            QColor color = dot.running ? agentRunningFireballColor() : dot.color;
            double scale = 1.0;
            if (dot.running) {
                const double meter = qMax(qBound(0.0, dot.intensity, 1.0),
                                          qBound(0.0, dot.throughput, 1.0));
                const double speed = 0.6 + 1.9 * meter;
                double t = m_phase * speed + i * kSweepStep;
                t -= std::floor(t);
                const double tri = 1.0 - std::abs(2.0 * t - 1.0);
                const double depth = 0.45 + 0.35 * meter;
                const double glow = (1.0 - depth) + depth * tri;
                color.setAlphaF(qBound(0.18, glow, 1.0));
                scale = 0.82 + 0.18 * tri; // the crest swells a touch
            } else {
                color.setAlpha(205);
            }
            const QPointF center = cellCenter(i);
            const double side = kSide * scale;
            p.setBrush(color);
            p.drawRoundedRect(
                QRectF(center.x() - side / 2.0, center.y() - side / 2.0, side,
                       side),
                1.2, 1.2);
        }
        ChromeDotGrid::paintCaption(p, this, QLatin1String(kCaption));
    }

private:
    static bool sameSquares(const QVector<Dot> &a, const QVector<Dot> &b)
    {
        if (a.size() != b.size())
            return false;
        for (qsizetype i = 0; i < a.size(); ++i)
            if (a.at(i).sessionId != b.at(i).sessionId ||
                a.at(i).color != b.at(i).color ||
                a.at(i).running != b.at(i).running)
                return false;
        return true;
    }

    QPointF cellCenter(int index) const
    {
        const int column = index / kRows;
        const int row = index % kRows;
        return QPointF(column * kPitch + kPitch / 2.0,
                       row * kPitch + kPitch / 2.0);
    }

    int dotAt(const QPoint &pos) const
    {
        const int column = pos.x() / kPitch;
        const int row = pos.y() / kPitch;
        if (column < 0 || row < 0 || row >= kRows)
            return -1;
        const int index = column * kRows + row;
        return index < m_dots.size() ? index : -1;
    }

    static constexpr int kRows = ChromeDotGrid::kRows;   // squares per column
    static constexpr int kPitch = ChromeDotGrid::kPitch; // cell, including gap
    static constexpr double kSide = 4.5;   // painted square
    static constexpr double kSweepStep = 0.06; // per-square offset of the sweep

    QVector<Dot> m_dots;
    double m_phase = 0.0;      // 0..1 Larson sweep parameter
    bool m_ticking = false;
};

class NodeDotMatrix : public QWidget
{
public:
    struct Dot {
        QString name;
        QColor color;
        bool self = false; // this machine, ringed so it's findable

        bool operator==(const Dot &other) const
        {
            return name == other.name && color == other.color &&
                   self == other.self;
        }
        bool operator!=(const Dot &other) const { return !(*this == other); }
    };

    static constexpr const char *kCaption = "Nodes";

    explicit NodeDotMatrix(QWidget *parent = nullptr) : QWidget(parent)
    {
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        setFixedHeight(ChromeDotGrid::totalHeight());
        setFixedWidth(0); // nothing to show until the first setDots()
        setCursor(Qt::PointingHandCursor);
        hide();
    }

    void setDots(const QVector<Dot> &dots)
    {
        const QVector<Dot> shown = dots.mid(0, kRows * kMaxColumns);
        if (shown == m_dots)
            return; // same row as the one painted; skip the chrome relayout
        m_dots = shown;
        const int columns = (m_dots.size() + kRows - 1) / kRows;
        const int width =
            ChromeDotGrid::widthFor(columns, QLatin1String(kCaption));
        if (width != maximumWidth() || width != minimumWidth())
            setFixedWidth(width);
        update();
    }

    int shownCount() const { return m_dots.size(); }

    std::function<void(const QString &)> onDotClicked;

protected:
    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton && onDotClicked) {
            const int index = dotAt(e->position().toPoint());
            onDotClicked(index >= 0 ? m_dots.at(index).name : QString());
            e->accept();
            return;
        }
        QWidget::mousePressEvent(e);
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        for (int i = 0; i < m_dots.size(); ++i) {
            const Dot &dot = m_dots.at(i);
            QColor color = dot.color;
            color.setAlpha(205); // same weight as an idle agent square
            const QPointF center = cellCenter(i);
            p.setBrush(color);
            p.setPen(Qt::NoPen);
            p.drawEllipse(center, kRadius, kRadius);
            if (dot.self) {
                QColor ring = palette().color(QPalette::WindowText);
                ring.setAlpha(190);
                p.setBrush(Qt::NoBrush);
                p.setPen(QPen(ring, 0.9));
                p.drawEllipse(center, kRadius + 1.0, kRadius + 1.0);
            }
        }
        ChromeDotGrid::paintCaption(p, this, QLatin1String(kCaption));
    }

private:
    QPointF cellCenter(int index) const
    {
        const int column = index / kRows;
        const int row = index % kRows;
        return QPointF(column * kPitch + kPitch / 2.0,
                       row * kPitch + kPitch / 2.0);
    }

    int dotAt(const QPoint &pos) const
    {
        const int column = pos.x() / kPitch;
        const int row = pos.y() / kPitch;
        if (column < 0 || row < 0 || row >= kRows)
            return -1;
        const int index = column * kRows + row;
        return index < m_dots.size() ? index : -1;
    }

    static constexpr int kRows = ChromeDotGrid::kRows;   // dots per column
    static constexpr int kPitch = ChromeDotGrid::kPitch; // cell, including gap
    static constexpr double kRadius = 2.3; // painted dot
    static constexpr int kMaxColumns = 12; // ~36 nodes before the tooltip takes over

    QVector<Dot> m_dots;
};

class ActionRunStrip : public QWidget
{
public:
    struct Cell {
        int runId = 0;
        QColor color;
        bool running = false;
    };

    struct Badge {
        int runId = 0;
        QString iconName;
        QColor color;

        bool operator==(const Badge &other) const
        {
            return runId == other.runId && iconName == other.iconName &&
                   color == other.color;
        }
        bool operator!=(const Badge &other) const { return !(*this == other); }
    };

    static constexpr int kRows = ChromeDotGrid::kRows;
    static constexpr int kMaxColumns = 6;
    static constexpr int kMaxCells = kRows * kMaxColumns;
    static constexpr int kMaxBadges = 6;
    static constexpr int kBadgeSide = ChromeDotGrid::kGridHeight;
    static constexpr int kBadgeGap = 4;
    static constexpr const char *kCaption = "Actions";

    explicit ActionRunStrip(QWidget *parent = nullptr) : QWidget(parent)
    {
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        setFixedHeight(ChromeDotGrid::totalHeight());
        setFixedWidth(0);
        setCursor(Qt::PointingHandCursor);
        hide();
        connect(ChromeDotGrid::animationClock(), &QTimer::timeout, this, [this] {
            if (!m_ticking)
                return;
            m_phase += m_badges.isEmpty() ? kCellPulsePhaseStep : kBreathStep;
            if (m_phase >= 1.0)
                m_phase -= 1.0;
            update();
        });
    }

    ~ActionRunStrip() override
    {
        ChromeDotGrid::holdAnimationClock(&m_ticking, false);
    }

    void setCells(const QVector<Cell> &cells,
                  const QVector<Badge> &badges = {})
    {
        m_cells = cells.mid(0, kMaxCells);
        const QVector<Badge> shown = badges.mid(0, kMaxBadges);
        if (shown != m_badges) {
            m_badges = shown;
            m_glyphs.clear();
        }
        int width = 0;
        if (!m_badges.isEmpty()) {
            width = qMax(badgeRowWidth(m_badges.size()),
                         ChromeDotGrid::captionWidth(
                             QLatin1String(kCaption)));
        } else {
            const int columns = (m_cells.size() + kRows - 1) / kRows;
            width = ChromeDotGrid::widthFor(columns, QLatin1String(kCaption));
        }
        if (width != maximumWidth() || width != minimumWidth())
            setFixedWidth(width);
        bool animate = !m_badges.isEmpty();
        for (const Cell &c : std::as_const(m_cells))
            animate = animate || c.running;
        ChromeDotGrid::holdAnimationClock(&m_ticking, animate);
        update();
    }

    int shownCount() const
    {
        return m_badges.isEmpty() ? m_cells.size() : m_badges.size();
    }

    bool showingIcons() const { return !m_badges.isEmpty(); }

    const QVector<Badge> &badges() const { return m_badges; }

    std::function<void(int)> onCellClicked;

protected:
    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton && onCellClicked) {
            const QPoint pos = e->position().toPoint();
            int runId = 0;
            if (!m_badges.isEmpty()) {
                const int index = badgeAt(pos);
                runId = m_badges.at(index >= 0 ? index : 0).runId;
            } else {
                const int index = cellAt(pos);
                runId = index >= 0 ? m_cells.at(index).runId : 0;
            }
            onCellClicked(runId);
            e->accept();
            return;
        }
        QWidget::mousePressEvent(e);
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        p.setPen(Qt::NoPen);
        if (!m_badges.isEmpty())
            paintBadges(p);
        else
            paintCells(p);
        ChromeDotGrid::paintCaption(p, this, QLatin1String(kCaption));
    }

private:
    void paintCells(QPainter &p)
    {
        for (int i = 0; i < m_cells.size(); ++i) {
            const Cell &cell = m_cells.at(i);
            QColor color = cell.color;
            if (cell.running) {
                double t = m_phase + i * kPulseStep;
                t -= std::floor(t);
                const double tri = 1.0 - std::abs(2.0 * t - 1.0);
                color.setAlphaF(qBound(0.35, 0.55 + 0.45 * tri, 1.0));
            } else {
                color.setAlpha(205);
            }
            const QPointF center = cellCenter(i);
            p.setBrush(color);
            p.drawRoundedRect(QRectF(center.x() - kSide / 2.0,
                                     center.y() - kSide / 2.0, kSide, kSide),
                              1.2, 1.2);
        }
    }

    void paintBadges(QPainter &p)
    {
        const qreal dpr = devicePixelRatioF();
        if (m_glyphs.size() != m_badges.size() || m_glyphDpr != dpr) {
            m_glyphs.clear();
            m_glyphDpr = dpr;
            for (const Badge &badge : std::as_const(m_badges))
                m_glyphs.append(
                    themedOcticon(badge.iconName, badge.color, kGlyphPx)
                        .pixmap(QSize(kGlyphPx, kGlyphPx), dpr));
        }
        for (int i = 0; i < m_badges.size(); ++i) {
            const Badge &badge = m_badges.at(i);
            double t = m_phase + i * kBreathOffset;
            t -= std::floor(t);
            const double breath = 0.5 - 0.5 * std::cos(2.0 * M_PI * t);
            const QRectF tile = badgeRect(i);
            QRadialGradient glow(tile.center(), kBadgeSide / 2.0);
            QColor halo = badge.color;
            halo.setAlphaF(0.05 + 0.35 * breath);
            glow.setColorAt(0.0, halo);
            halo.setAlphaF(0.0);
            glow.setColorAt(1.0, halo);
            p.setBrush(glow);
            p.drawEllipse(tile);
            const double side = kGlyphPx * (0.86 + 0.14 * breath);
            p.save();
            p.setOpacity(0.55 + 0.45 * breath);
            p.drawPixmap(QRectF(tile.center().x() - side / 2.0,
                                tile.center().y() - side / 2.0, side, side),
                         m_glyphs.at(i), QRectF(m_glyphs.at(i).rect()));
            p.restore();
        }
    }

    static int badgeRowWidth(int count)
    {
        return count <= 0 ? 0 : count * kBadgeSide + (count - 1) * kBadgeGap;
    }

    QRectF badgeRect(int index) const
    {
        return QRectF(index * (kBadgeSide + kBadgeGap), 0, kBadgeSide,
                      kBadgeSide);
    }

    int badgeAt(const QPoint &pos) const
    {
        if (pos.y() < 0 || pos.y() >= kBadgeSide)
            return -1;
        const int index = pos.x() / (kBadgeSide + kBadgeGap);
        return index >= 0 && index < m_badges.size() ? index : -1;
    }

    QPointF cellCenter(int index) const
    {
        const int column = index / kRows;
        const int row = index % kRows;
        return QPointF(column * kPitch + kPitch / 2.0,
                       row * kPitch + kPitch / 2.0);
    }

    int cellAt(const QPoint &pos) const
    {
        const int column = pos.x() / kPitch;
        const int row = pos.y() / kPitch;
        if (column < 0 || row < 0 || row >= kRows)
            return -1;
        const int index = column * kRows + row;
        return index < m_cells.size() ? index : -1;
    }

    static constexpr int kPitch = ChromeDotGrid::kPitch;
    static constexpr double kSide = 4.5;
    static constexpr double kPulseStep = 0.09;
    static constexpr int kGlyphPx = kBadgeSide;
    static constexpr double kBreathStep = 0.025;
    static constexpr double kCellPulsePhaseStep = 1.0 / 30;
    static constexpr double kBreathOffset = 0.17;

    QVector<Cell> m_cells;
    QVector<Badge> m_badges;
    QVector<QPixmap> m_glyphs;
    qreal m_glyphDpr = 0.0;
    double m_phase = 0.0;
    bool m_ticking = false;
};

// A workflow row is deliberately a widget instead of a wrapped text item: the
// same compact graph can show every declared step and keep the active step
// animated while the Actions runner updates its persisted record.
class ActionWorkflowArea final : public QWidget
{
public:
    struct Step {
        QString name;
        QString status;
    };

    explicit ActionWorkflowArea(QWidget *parent = nullptr) : QWidget(parent)
    {
        setAttribute(Qt::WA_TranslucentBackground);
        setCursor(Qt::PointingHandCursor);
        m_pulse = new QTimer(this);
        m_pulse->setInterval(80);
        connect(m_pulse, &QTimer::timeout, this, [this] {
            m_phase += 0.06;
            if (m_phase >= 1.0)
                m_phase -= 1.0;
            update();
        });
    }

    void setData(const QString &name, const QString &path,
                 const QStringList &details, const QVector<Step> &steps,
                 bool enabled, bool selected, bool runnableHere)
    {
        m_name = name;
        m_path = path;
        m_details = details;
        m_steps = steps;
        m_enabled = enabled;
        m_selected = selected;
        m_runnableHere = runnableHere;
        bool running = false;
        for (const Step &step : std::as_const(m_steps))
            running = running || step.status == ActionStepStatus::Running;
        if (running && !m_pulse->isActive())
            m_pulse->start();
        else if (!running && m_pulse->isActive())
            m_pulse->stop();
        setMinimumHeight(sizeHint().height());
        updateGeometry();
        update();
    }

    std::function<void()> onClicked;
    std::function<void(bool)> onEnabledChanged;

    QSize sizeHint() const override
    {
        if (!m_selected)
            return QSize(300, kRowHeight);
        return QSize(300, kFirstStepY + qMax(1, m_steps.size()) * 26 - 4);
    }

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) {
            QWidget::mousePressEvent(event);
            return;
        }
        if (QRect(2, 2, 32, kRowHeight - 4)
                .contains(event->position().toPoint())) {
            m_enabled = !m_enabled;
            if (onEnabledChanged)
                onEnabledChanged(m_enabled);
            update();
        } else if (onClicked) {
            onClicked();
        }
        event->accept();
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QColor base = palette().color(QPalette::Base);
        const QColor highlight = palette().color(QPalette::Highlight);
        painter.setPen(QPen(m_selected ? highlight
                                       : palette().color(QPalette::Mid),
                            m_selected ? 1.0 : 0.5));
        painter.setBrush(m_selected ? highlight.lighter(145) : base);
        painter.drawRoundedRect(rect().adjusted(2, 2, -2, -2), 5, 5);

        const QRect check(11, (kRowHeight - 16) / 2, 16, 16);
        painter.setPen(QPen(m_enabled ? QColor("#58a6ff")
                                     : palette().color(QPalette::Mid),
                        1.2));
        painter.setBrush(m_enabled ? QColor("#1f6feb") : base);
        painter.drawRoundedRect(check, 3, 3);
        if (m_enabled) {
            painter.setPen(QPen(Qt::white, 1.6, Qt::SolidLine,
                                Qt::RoundCap, Qt::RoundJoin));
            QPainterPath tick;
            tick.moveTo(check.left() + 3, check.top() + 8);
            tick.lineTo(check.left() + 7, check.top() + 11);
            tick.lineTo(check.left() + 13, check.top() + 4);
            painter.drawPath(tick);
        }

        bool running = false;
        for (const Step &step : std::as_const(m_steps))
            running = running || step.status == ActionStepStatus::Running;
        const int spinnerWidth = running && !m_selected ? 22 : 0;
        if (spinnerWidth > 0) {
            const QColor color = stepColor(ActionStepStatus::Running);
            const QPointF centre(width() - 16, kRowHeight / 2.0);
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(color, 2.0));
            painter.drawArc(QRectF(centre.x() - 6, centre.y() - 6, 12, 12),
                            int(-360 * 16 * m_phase), 245 * 16);
        }

        QFont titleFont = painter.font();
        titleFont.setBold(true);
        painter.setFont(titleFont);
        painter.setPen(m_runnableHere ? palette().color(QPalette::Text)
                                      : palette().color(QPalette::Disabled,
                                                        QPalette::Text));
        const QRect titleRect(34, 0, width() - 42 - spinnerWidth, kRowHeight);
        painter.drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter,
                         painter.fontMetrics().elidedText(
                             m_name, Qt::ElideRight, titleRect.width()));
        if (!m_selected)
            return;

        painter.setFont(font());
        painter.setPen(palette().color(QPalette::PlaceholderText));
        const QString details = m_details.join(QStringLiteral("  ·  "));
        const QRect detailsRect(34, kRowHeight - 6, width() - 44, 20);
        painter.drawText(detailsRect, Qt::AlignLeft | Qt::AlignVCenter,
                         painter.fontMetrics().elidedText(
                             details.isEmpty() ? m_path : details,
                             Qt::ElideRight, detailsRect.width()));

        const int firstY = kFirstStepY;
        const int stepX = 18;
        if (m_steps.size() > 1) {
            painter.setPen(QPen(palette().color(QPalette::Mid), 1.2));
            painter.drawLine(stepX, firstY + 7, stepX,
                             firstY + (m_steps.size() - 1) * 26 + 7);
        }
        for (int index = 0; index < m_steps.size(); ++index) {
            const Step &step = m_steps.at(index);
            const int y = firstY + index * 26;
            const QColor color = stepColor(step.status);
            painter.setPen(QPen(color, 1.5));
            painter.setBrush(color);
            if (step.status == ActionStepStatus::Success) {
                painter.drawEllipse(QPointF(stepX, y + 7), 6, 6);
                painter.setPen(QPen(Qt::white, 1.3, Qt::SolidLine,
                                    Qt::RoundCap, Qt::RoundJoin));
                QPainterPath tick;
                tick.moveTo(stepX - 3, y + 7);
                tick.lineTo(stepX - 1, y + 9);
                tick.lineTo(stepX + 3, y + 4);
                painter.drawPath(tick);
            } else if (step.status == ActionStepStatus::Running) {
                painter.setBrush(Qt::NoBrush);
                painter.drawEllipse(QPointF(stepX, y + 7), 6, 6);
                painter.setPen(QPen(color, 2.0));
                painter.drawArc(QRectF(stepX - 6, y + 1, 12, 12),
                                int(-360 * 16 * m_phase), 245 * 16);
            } else if (step.status == ActionStepStatus::Failed) {
                painter.drawEllipse(QPointF(stepX, y + 7), 6, 6);
                painter.setPen(QPen(Qt::white, 1.2));
                painter.drawLine(stepX - 3, y + 4, stepX + 3, y + 10);
                painter.drawLine(stepX + 3, y + 4, stepX - 3, y + 10);
            } else {
                painter.setBrush(step.status == ActionStepStatus::Queued
                                     ? QBrush(color)
                                     : QBrush(Qt::NoBrush));
                painter.drawEllipse(QPointF(stepX, y + 7), 5, 5);
            }
            painter.setPen(step.status == ActionStepStatus::Running
                               ? color
                               : palette().color(QPalette::Text));
            const QString label = QStringLiteral("%1. %2")
                                      .arg(index + 1)
                                      .arg(step.name.isEmpty()
                                               ? QStringLiteral("shell step")
                                               : step.name);
            painter.drawText(34, y + 12,
                             painter.fontMetrics().elidedText(
                                 label, Qt::ElideRight, width() - 46));
        }
    }

private:
    static constexpr int kRowHeight = 34;
    static constexpr int kFirstStepY = 56;

    static QColor stepColor(const QString &status)
    {
        if (status == ActionStepStatus::Success)
            return QColor("#3fb950");
        if (status == ActionStepStatus::Running)
            return QColor("#58a6ff");
        if (status == ActionStepStatus::Failed)
            return QColor("#f85149");
        if (status == ActionStepStatus::Queued)
            return QColor("#d29922");
        return Theme::remapColor(QColor("#8b949e"));
    }

    QString m_name;
    QString m_path;
    QStringList m_details;
    QVector<Step> m_steps;
    bool m_enabled = true;
    bool m_selected = false;
    bool m_runnableHere = true;
    QTimer *m_pulse = nullptr;
    qreal m_phase = 0.0;
};

class ToggleSwitch : public QAbstractButton
{
public:
    explicit ToggleSwitch(QWidget *parent = nullptr) : QAbstractButton(parent)
    {
        setCheckable(true);
        setCursor(Qt::PointingHandCursor);
        setFixedSize(m_switchSize);
    }

    // The status bar is pinned to a single text line, so the switch that lives
    // down there has to be shorter than the 46x24 the settings pages use. The
    // painting below is all derived from width()/height(), so a smaller box is
    // the same switch drawn smaller.
    void setSwitchSize(QSize size)
    {
        if (size.isEmpty() || size == m_switchSize)
            return;
        m_switchSize = size;
        setFixedSize(size);
        updateGeometry();
    }

    QSize sizeHint() const override { return m_switchSize; }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        const QColor track = toggleSwitchTrackHook(
            isChecked(), isChecked() ? QColor("#2ea043") : Theme::remapColor(QColor("#30363d")));
        const QRectF trackRect(0.5, 0.5, width() - 1.0, height() - 1.0);
        const qreal r = trackRect.height() / 2.0;
        p.setPen(QPen(track.darker(130), 1));
        p.setBrush(track);
        p.drawRoundedRect(trackRect, r, r);

        const qreal knobD = trackRect.height() - 4.0;
        const qreal x = isChecked() ? trackRect.right() - knobD - 2.0
                                    : trackRect.left() + 2.0;
        const QRectF knobRect(x, trackRect.top() + 2.0, knobD, knobD);
        p.setPen(Qt::NoPen);
        p.setBrush(Qt::white);
        p.drawEllipse(knobRect);
    }

private:
    QSize m_switchSize{46, 24};
};

struct MirrorNodeDot
{
    QString id;
    QString name;
    bool online = false;
    bool self = false;
    bool behind = false;
    bool integrityFailing = false;
};

class HoverRowDelegate : public QStyledItemDelegate
{
public:
    explicit HoverRowDelegate(QAbstractItemView *view)
        : QStyledItemDelegate(view), m_view(view)
    {
        m_view->setMouseTracking(true);
        m_view->viewport()->setMouseTracking(true);
        m_view->viewport()->installEventFilter(this);
        connect(m_view, &QAbstractItemView::entered, this,
                [this](const QModelIndex &index) { setHovered(index); });
        connect(m_view, &QAbstractItemView::viewportEntered, this,
                [this] { setHovered(QModelIndex()); });
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt(option);
        opt.state &= ~QStyle::State_MouseOver;
        const bool sameRow = m_hovered.isValid() &&
                             index.row() == m_hovered.row() &&
                             index.parent() == m_hovered.parent();
        if (m_hoverFill && sameRow && !(option.state & QStyle::State_Selected))
            painter->fillRect(option.rect,
                              nextHoverFill(QColor(46, 160, 67, 55)));
        opt.state &= ~QStyle::State_Selected;
        QStyledItemDelegate::paint(painter, opt, index);
        paintRowSelectionBorder(painter, option, index);
    }

    QString displayText(const QVariant &value, const QLocale &locale) const override
    {
        QString text = QStyledItemDelegate::displayText(value, locale);
        if (text.size() > kMaxCellDisplayChars) {
            text.truncate(kMaxCellDisplayChars);
            text += QChar(0x2026); // horizontal ellipsis
        }
        return text;
    }

protected:
    static constexpr int kMaxCellDisplayChars = 512;

    bool eventFilter(QObject *obj, QEvent *event) override
    {
        if (event->type() == QEvent::Leave)
            setHovered(QModelIndex());
        return QStyledItemDelegate::eventFilter(obj, event);
    }

    // The row under the pointer, or -1. QTableView only flags the single cell
    // it is over with State_MouseOver, so a delegate that reveals a control
    // whenever the *agent* is hovered — any column of the row — has to ask for
    // the row here. Subclasses that paint from it must set m_hoverRepaint.
    int hoveredRow() const { return m_hovered.isValid() ? m_hovered.row() : -1; }

    bool m_hoverFill = false;
    // Repaint the row the pointer left and the row it entered on every hover
    // change, for delegates whose painting reads hoveredRow().
    bool m_hoverRepaint = false;

private:
    // The full-width band one row occupies in the viewport. visualRect() of
    // any cell in the row gives its vertical extent; the band spans every
    // column so the fill and the delegates of the whole row repaint together.
    QRect rowBand(const QModelIndex &index) const
    {
        if (!m_view || !index.isValid())
            return QRect();
        const QRect cell = m_view->visualRect(index);
        if (cell.isNull())
            return QRect();
        return QRect(0, cell.top(), m_view->viewport()->width(), cell.height());
    }

    void setHovered(const QModelIndex &index)
    {
        if (QModelIndex(m_hovered) == index)
            return;
        const QRect before = rowBand(m_hovered);
        m_hovered = index;
        // Only the hover fill and hover-revealed controls read m_hovered when
        // painting, and only the row the pointer left and the row it entered
        // change. Repainting the whole viewport here made every pointer
        // crossing re-run every visible delegate (the commit graph gutter,
        // badges, elided text), so dragging the mouse down a table cost
        // rows x columns paints per row boundary.
        if (!m_view || (!m_hoverFill && !m_hoverRepaint))
            return;
        QRegion dirty;
        if (!before.isNull())
            dirty += before;
        const QRect after = rowBand(m_hovered);
        if (!after.isNull())
            dirty += after;
        if (!dirty.isEmpty())
            m_view->viewport()->update(dirty);
    }

    QAbstractItemView *m_view = nullptr;
    QPersistentModelIndex m_hovered;
};

void blankSelectionBand(QAbstractItemView *view);

void enableHoverRowHighlight(QAbstractItemView *view);

void paintRowBorder(QPainter *painter,
                           const QStyleOptionViewItem &option,
                           const QModelIndex &index, const QColor &color);

void paintRowSelectionBorder(QPainter *painter,
                                    const QStyleOptionViewItem &option,
                                    const QModelIndex &index);

class SelectionBorderRowDelegate : public HoverRowDelegate
{
public:
    explicit SelectionBorderRowDelegate(QAbstractItemView *view)
        : HoverRowDelegate(view)
    {
        m_hoverFill = false;
    }
};

class ActionFailureBorderDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    static constexpr int ActionFailedRole = Qt::UserRole + 1;
    static constexpr int ActionRunDetailRole = Qt::UserRole + 2;

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override
    {
        QSize size = QStyledItemDelegate::sizeHint(option, index);
        if (!index.data(ActionRunDetailRole).toString().isEmpty())
            size.rheight() += option.fontMetrics.height();
        return size;
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        const QString detail = index.data(ActionRunDetailRole).toString();
        if (detail.isEmpty()) {
            QStyledItemDelegate::paint(painter, option, index);
        } else {
            QStyleOptionViewItem opt(option);
            initStyleOption(&opt, index);
            const QWidget *widget = opt.widget;
            QStyle *style = widget ? widget->style() : QApplication::style();
            const QRect textRect = style->subElementRect(
                QStyle::SE_ItemViewItemText, &opt, widget);
            const QString name = opt.text;
            opt.text.clear();
            style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);
            const bool selected = opt.state & QStyle::State_Selected;
            const int line = opt.fontMetrics.height();
            const int top = textRect.center().y() - line + 1;
            const QRect nameRect(textRect.left() + 2, top,
                                 textRect.width() - 4, line);
            const QRect detailRect = nameRect.translated(0, line);
            painter->save();
            painter->setFont(opt.font);
            painter->setPen(opt.palette.color(
                selected ? QPalette::HighlightedText : QPalette::Text));
            painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter,
                              opt.fontMetrics.elidedText(
                                  name, Qt::ElideRight, nameRect.width()));
            QColor dim = opt.palette.color(
                selected ? QPalette::HighlightedText : QPalette::PlaceholderText);
            if (selected)
                dim.setAlpha(190);
            painter->setPen(dim);
            painter->drawText(detailRect, Qt::AlignLeft | Qt::AlignVCenter,
                              opt.fontMetrics.elidedText(
                                  detail, Qt::ElideRight, detailRect.width()));
            painter->restore();
        }
        if (index.sibling(index.row(), 0).data(ActionFailedRole).toBool())
            paintRowBorder(painter, option, index, QColor(0xf8, 0x51, 0x49)); // red
    }
};

class TableRepaintGuard
{
public:
    explicit TableRepaintGuard(QWidget *w) : m_w(w)
    {
        if (m_w) {
            m_was = m_w->updatesEnabled();
            m_w->setUpdatesEnabled(false);
        }
    }
    ~TableRepaintGuard()
    {
        if (m_w)
            m_w->setUpdatesEnabled(m_was);
    }
    TableRepaintGuard(const TableRepaintGuard &) = delete;
    TableRepaintGuard &operator=(const TableRepaintGuard &) = delete;

private:
    QWidget *m_w = nullptr;
    bool m_was = true;
};

// Freeze the auto-sized columns at their content width once the first rows
// land, so they can be dragged afterwards. Columns named in
// `keepFlexibleColumns` stay Stretch: a column holding free text of unbounded
// length has to keep dividing the leftover width, or its content decides how
// wide the grid is and pushes the rest of it off the window.
void makeColumnsResizable(QTableWidget *table,
                                 const QSet<int> &keepFlexibleColumns);

void makeColumnsResizable(QTableWidget *table, int keepFlexibleColumn = -1);

// makeColumnsResizable() freezes a grid at whatever width its *first* batch of
// rows needed, and that batch is usually the least informative one: a fleet
// roster starts as the local row alone, and the health, tunnel and
// reachability probes answer seconds later. Everything they fill in is wider
// than the placeholder it replaces, so those columns elide it forever - a cell
// reading "Out of date" paints "Out ...". Call this after a rebuild to
// re-measure and widen the columns that no longer fit.
//
// It only ever grows, so a column that has already made room for a long value
// keeps it and the grid doesn't twitch as rows come and go. `maxSectionWidth`
// bounds the growth: free text (a commit subject) stays elided against its
// tooltip rather than letting one row decide how wide the whole grid is. A
// column the user has dragged is left exactly where they put it.
void growColumnsToContents(QTableWidget *table, int maxSectionWidth = 200);


int progressPctForX(const QRect &cellRect, int x, const QFontMetrics &fm);

void paintProgressBar(QPainter *painter, const QRect &cellRect, int pct,
                             const QFontMetrics &fm);

class ProgressBarDelegate : public HoverRowDelegate
{
public:
    using HoverRowDelegate::HoverRowDelegate;

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override
    {
        QSize base = HoverRowDelegate::sizeHint(option, index);
        return QSize(qMax(base.width(), 96), qMax(base.height(), 18));
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        HoverRowDelegate::paint(painter, option, index);
        const QVariant value = index.data(kProgressBarRole);
        if (!value.isValid())
            return;
        paintProgressBar(painter, option.rect, value.toInt(), option.fontMetrics);
    }
};

void paintResourceBar(QPainter *painter, const QRect &cellRect, int pct,
                             const QFontMetrics &fm);

class ResourceBarDelegate : public HoverRowDelegate
{
public:
    using HoverRowDelegate::HoverRowDelegate;

    bool mutedSelectionBand = false;

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override
    {
        QSize base = HoverRowDelegate::sizeHint(option, index);
        return QSize(qMax(base.width(), 84), qMax(base.height(), 18));
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt(option);
        if (mutedSelectionBand && (option.state & QStyle::State_Selected)) {
            painter->fillRect(option.rect, currentThemeIsDark()
                                               ? Theme::remapColor(QColor("#21262d"))
                                               : Theme::remapColor(QColor("#eaeef2")));
            opt.state &= ~QStyle::State_Selected;
        }
        HoverRowDelegate::paint(painter, opt, index);
        const QVariant value = index.data(kProgressBarRole);
        paintResourceBar(painter, option.rect,
                         value.isValid() ? value.toInt() : -1,
                         option.fontMetrics);
    }
};

// A draggable version of the progress bar for the issue detail panel: click or
// drag anywhere along the track to set the percentage. Pure QWidget (no moc) —
// the owner wires the result through the onCommitted callback, fired once the
// drag/click finishes so the store is written only on release.
class ProgressSlider : public QWidget
{
public:
    explicit ProgressSlider(QWidget *parent = nullptr) : QWidget(parent)
    {
        setCursor(Qt::PointingHandCursor);
        setMinimumHeight(18);
        setToolTip(QStringLiteral("Drag to set progress"));
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    int value() const { return m_value; }
    void setValue(int pct)
    {
        pct = qBound(0, pct, 100);
        if (pct == m_value)
            return;
        m_value = pct;
        update();
    }

    std::function<void(int)> onCommitted;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        paintProgressBar(&painter, rect(), m_value, fontMetrics());
    }
    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton)
            return;
        m_dragging = true;
        setValue(progressPctForX(rect(), int(e->position().x()), fontMetrics()));
    }
    void mouseMoveEvent(QMouseEvent *e) override
    {
        if (m_dragging)
            setValue(progressPctForX(rect(), int(e->position().x()), fontMetrics()));
    }
    void mouseReleaseEvent(QMouseEvent *e) override
    {
        if (!m_dragging || e->button() != Qt::LeftButton)
            return;
        m_dragging = false;
        if (onCommitted)
            onCommitted(m_value);
    }

private:
    int m_value = 0;
    bool m_dragging = false;
};

// One column of the Kanban issue board: a QListWidget that accepts cards dragged
// from any sibling column. On a cross-column drop it doesn't move the item itself
// (the board is rebuilt from the store after the issue's status label changes);
// instead it reads the dragged card's issue number and invokes onDrop with this
// column's name. A plain callback avoids needing Q_OBJECT/moc in this.cpp.
class BoardColumnList : public QListWidget
{
public:
    explicit BoardColumnList(QString column, QWidget *parent = nullptr)
        : QListWidget(parent), m_column(std::move(column))
    {
        setObjectName("issueBoardList");
        setDragEnabled(true);
        setAcceptDrops(true);
        setDragDropMode(QAbstractItemView::DragDrop);
        setDefaultDropAction(Qt::MoveAction);
        setSelectionMode(QAbstractItemView::SingleSelection);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setWordWrap(true);
        setUniformItemSizes(false);
    }

    std::function<void(int number, const QString &column)> onDrop;

protected:
    void dropEvent(QDropEvent *event) override
    {
        auto *src = qobject_cast<QListWidget *>(event->source());
        QListWidgetItem *item = src ? src->currentItem() : nullptr;
        if (src && src != this && item && onDrop) {
            const int number = item->data(Qt::UserRole).toInt();
            event->setDropAction(Qt::IgnoreAction);
            event->accept();
            onDrop(number, m_column);
            return;
        }
        event->ignore();
    }

private:
    QString m_column;
};

// The Chat sidebar's channel list. Top-level rows are ungrouped channels and
// group headers; grouped channels hang under their header so the group can be
// collapsed. Dragging a channel onto a header, between two channels, or onto
// empty space only reports where it landed — the host rewrites its group state
// and rebuilds the tree (the same rebuild-from-store approach as
// BoardColumnList), so Qt's InternalMove never mutates the items itself. A
// plain callback avoids needing Q_OBJECT/moc in this header.
class ChannelGroupTree : public QTreeWidget
{
public:
    // Set on group headers only; its value is the group name. Channel rows
    // carry the conversation key in Qt::UserRole, exactly as the DM list does.
    static constexpr int kGroupRole = Qt::UserRole + 1;
    static constexpr int kSettingsColumn = 1;
    static constexpr int kSettingsColumnWidth = 26;
    static constexpr int kUnreadCountRole = Qt::UserRole + 2;
    static constexpr int kPresenceRole = Qt::UserRole + 3;
    static constexpr int kPreviewRole = Qt::UserRole + 4;
    static constexpr int kPreviewTimeRole = Qt::UserRole + 5;
    static constexpr int kAvatarRole = Qt::UserRole + 6;

    explicit ChannelGroupTree(QWidget *parent = nullptr) : QTreeWidget(parent)
    {
        setObjectName("chatChannelTree");
        setColumnCount(2);
        setHeaderHidden(true);
        header()->setStretchLastSection(false);
        header()->setSectionResizeMode(0, QHeaderView::Stretch);
        header()->setSectionResizeMode(1, QHeaderView::Fixed);
        header()->resizeSection(1, kSettingsColumnWidth);
        setRootIsDecorated(false);
        setExpandsOnDoubleClick(false);
        setIndentation(14);
        setUniformRowHeights(false);
        setSelectionMode(QAbstractItemView::SingleSelection);
        setDragEnabled(true);
        setAcceptDrops(true);
        setDropIndicatorShown(true);
        setDragDropMode(QAbstractItemView::InternalMove);
        setDefaultDropAction(Qt::MoveAction);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        invisibleRootItem()->setFlags(Qt::ItemIsEnabled | Qt::ItemIsDropEnabled);
    }

    static bool isGroupItem(const QTreeWidgetItem *item)
    {
        return item && item->data(0, kGroupRole).isValid();
    }
    static QString groupNameOf(const QTreeWidgetItem *item)
    {
        return item ? item->data(0, kGroupRole).toString() : QString();
    }
    static QString channelOf(const QTreeWidgetItem *item)
    {
        return isGroupItem(item) || !item
                   ? QString()
                   : item->data(0, Qt::UserRole).toString();
    }

    // (channel, destination group — empty for the top level, index among the
    // destination's channels as currently rendered, or -1 to append).
    std::function<void(const QString &channel, const QString &group, int index)>
        onMoveChannel;

protected:
    void dropEvent(QDropEvent *event) override
    {
        event->setDropAction(Qt::IgnoreAction);
        const QString channel = channelOf(currentItem());
        if (event->source() != this || channel.isEmpty() || !onMoveChannel) {
            event->ignore();
            return;
        }
        QTreeWidgetItem *target = itemAt(event->position().toPoint());
        const DropIndicatorPosition where = dropIndicatorPosition();
        QString group;
        int index = -1;
        if (target && where != OnViewport) {
            if (isGroupItem(target)) {
                if (where == BelowItem) {
                    group = groupNameOf(target);
                    index = 0;
                } else if (where == AboveItem) {
                    // The gap above a header belongs to whatever precedes it:
                    // the previous group's tail, or the ungrouped region.
                    const int row = indexOfTopLevelItem(target);
                    group = row > 0 ? groupNameOf(topLevelItem(row - 1))
                                    : QString();
                } else {
                    group = groupNameOf(target);
                }
            } else {
                QTreeWidgetItem *parent = target->parent();
                group = groupNameOf(parent);
                if (parent) {
                    index = parent->indexOfChild(target) +
                            (where == BelowItem ? 1 : 0);
                }
            }
        }
        event->accept();
        onMoveChannel(channel, group, index);
    }
};

class MirrorSyncDelegate : public HoverRowDelegate
{
public:
    using HoverRowDelegate::HoverRowDelegate;

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override
    {
        QSize base = HoverRowDelegate::sizeHint(option, index);
        if (index.data(kPacmanAnchorRole).isValid())
            base.setWidth(base.width() + kDiameter + 12); // room for the pie
        return base;
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        HoverRowDelegate::paint(painter, option, index);
        const QVariant anchor = index.data(kPacmanAnchorRole);
        if (!anchor.isValid())
            return;
        const qint64 intervalMs = qMax(
            1LL, index.data(kPacmanIntervalRole).isValid()
                     ? index.data(kPacmanIntervalRole).toLongLong()
                     : mirrorSyncIntervalMs());
        qint64 elapsed =
            (QDateTime::currentMSecsSinceEpoch() - anchor.toLongLong()) %
            intervalMs;
        if (elapsed < 0)
            elapsed += intervalMs;
        const double frac =
            qBound(0.0, double(elapsed) / double(intervalMs), 1.0);

        const bool dark = currentThemeIsDark();
        QRectF box(option.rect.right() - kDiameter - 6,
                   option.rect.center().y() - kDiameter / 2.0,
                   kDiameter, kDiameter);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setPen(QPen(Theme::remapColor(QColor(dark ? "#30363d" : "#d0d7de")), 1.2));
        painter->setBrush(Qt::NoBrush);
        painter->drawEllipse(box);
        if (frac > 0.004) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor(dark ? "#d29922" : "#9a6700"));
            painter->drawPie(box, 90 * 16, -int(frac * 360.0 * 16));
        }
        painter->restore();
    }

private:
    static constexpr int kDiameter = 12;
};

QString formatByteSize(qint64 bytes);

SortTableWidgetItem *makeResourceBarCell(int pct, const QString &tooltip);

SortTableWidgetItem *makeByteUsageCell(const QString &label, qint64 used, qint64 total);

QString nodeHealthColor(int severity);

SortTableWidgetItem *makeNodeHealthCell(
    const QList<NodeDiagnostics::Finding> &findings, qint64 diagnosticsMs,
    qint64 nowMs);

SortTableWidgetItem *makeCpuUsageCell(double cpuPercent);

QString formatCount(qint64 n);

class ClickableIssueBody : public QWidget
{
public:
    explicit ClickableIssueBody(QWidget *parent = nullptr) : QWidget(parent) {}

    std::function<void()> onClicked;

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && onClicked) {
            auto callback = onClicked;
            callback();
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }
};

class ClickableIssueImage : public QLabel
{
public:
    explicit ClickableIssueImage(QWidget *parent = nullptr) : QLabel(parent) {}

    std::function<void()> onClicked;

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && onClicked) {
            auto callback = onClicked;
            callback();
            event->accept();
            return;
        }
        QLabel::mousePressEvent(event);
    }
};

const QString kRepoUrl = QStringLiteral("https://github.com/forkmesh/forkmesh.git");
const QString kDisplayNameSetting = QStringLiteral("profile/displayName");
const QString kHandleSetting = QStringLiteral("profile/handle");
const QString kAccountNameSetting = QStringLiteral("account/nodeName");
// The fun node name a true first run handed out (see randomFunNodeName). While
// the account name is still exactly this — nobody typed a username, signed up,
// or logged in — the person at the desktop is a guest, not a user, and chat
// speaks as "Guest ####" like the website does for anonymous visitors
// Renaming (setup screen or Settings) or claiming a user account makes
// account/nodeName diverge from this and thereby exits guest mode.
const QString kGeneratedNodeNameSetting =
    QStringLiteral("account/generatedNodeName");
const QString kMachineNodeNameSetting = QStringLiteral("node/machineName");
const QString kShareTipAddressSetting = QStringLiteral("chat/shareTipAddress");
const QString kHostsSetting = QStringLiteral("hosts/list");
// Hosts deleted from that list, stamped with when. Survives a relaunch: the
// relay lease that keeps a destroyed mirror "online" outlives most sessions.
const QString kDecommissionedNodesSetting =
    QStringLiteral("hosts/decommissionedNodes");
// Relay-confirmed deletions from the Nodes directory. Kept separate from the
// host ledger: "Remove saved host" intentionally leaves that node running.
const QString kDeletedMeshNodesSetting =
    QStringLiteral("nodes/deletedNodes");
const QString kSolanaSetting = QStringLiteral("profile/solana");
const QString kServerUrlSetting = QStringLiteral("server/url");
// The mainnode is ForkMesh's canonical coordination point: a well-known
// owner/repo/room triple that every node's shared rooms and inbox routes
// converge on. The *host* is fully configurable (the first-run Relay server
// field; self-hosting one is a first-class target — see /docs#self-hosting), but
// this path shape is a network-wide protocol constant, so it lives in one place
// instead of being spelled out at each call site.
const QString kMainnodeDefaultHost = forkmesh::mainnode::kDefaultHost;
const QString kMainnodeRoomPath = forkmesh::mainnode::kRoomPath;
const QString kLocalServerUrl = forkmesh::mainnode::kLocalServerUrl;
const QString kDefaultServerUrl = forkmesh::mainnode::kDefaultServerUrl;
const QString kRoomNameSetting = QStringLiteral("server/room");
const QString kWorldDevUrlSetting = QStringLiteral("world/localDevUrl");
// Last account this node key authenticated as; lets the app start offline once a
// registered account has been confirmed at least once on this machine.
const QString kAuthedAccountSetting = QStringLiteral("account/authedName");
const QString kRecentUserAccountsSetting =
    QStringLiteral("account/recentUsers");
const QString kRecentUserInstancesSetting =
    QStringLiteral("account/recentUserInstances");
// V2 stores one row per exact (instance, username) pair. The legacy settings
// above remain migration inputs only; unlike their account-keyed object this
// array can represent @alice on two independent relays without collision.
const QString kRecentUserIdentitiesSetting =
    QStringLiteral("account/recentIdentitiesV2");
const QString kDesktopCapableAccountSetting =
    QStringLiteral("account/desktopCapableName");
const QString kDesktopCapablePublicKeySetting =
    QStringLiteral("account/desktopCapablePublicKey");
const QString kEmailVerifiedSettingPrefix =
    QStringLiteral("account/emailVerified/");
const QString kServersArray = QStringLiteral("servers/items");
const QString kActiveServerSetting = QStringLiteral("servers/active");
const QString kDefaultRoomName = forkmesh::mainnode::kDefaultRoomName;
const QString kRepositoriesArray = QStringLiteral("repositories/items");
// JSON object: relay origin -> [owner/name] that relay last listed.
const QString kRelayRepositoryListsSetting =
    QStringLiteral("repositories/relayLists");
const QString kMirrorRootSetting = QStringLiteral("repositories/mirrorRoot");
const QString kLastRepositorySetting = QStringLiteral("repositories/lastOpen");
const QString kLastRepoDetailTabSetting =
    QStringLiteral("repositories/lastOpenDetailTab");
const QString kLooperActiveSetting = QStringLiteral("looper/active");
const QString kLooperProviderSetting = QStringLiteral("looper/provider");
const QString kLooperRepoSetting = QStringLiteral("looper/repo"); // "owner/name"

QString savedSolanaAddress();

void saveSolanaAddress(const QString &address);

QString emailVerifiedSettingKey(const QString &accountName);
const QString kPreviewCacheRootSetting = QStringLiteral("repositories/previewCacheRoot");
const QString kConnectionTotalSetting = QStringLiteral("stats/connectionTotalMs");
const QString kNodeOfflineSetting = QStringLiteral("stats/nodeOffline");
// Offline mode: the whole app stops using the network, rather than the node
// stopping serving repos (that is kNodeOfflineSetting, a different switch).
// The companion key records whether the connectivity watch turned it on, which
// is what decides whether it may turn itself back off again.
const QString kOfflineModeSetting = QStringLiteral("network/offlineMode");
const QString kOfflineModeAutoSetting =
    QStringLiteral("network/offlineModeAutomatic");
const QString kThemeSetting = QStringLiteral("app/theme");
const QString kThemeBaseDarkSetting = QStringLiteral("app/themeBaseDark");
const QString kMarketplaceThemePrefix = QStringLiteral("market:");
const QString kHalloweenTheme = QStringLiteral("halloween");
const QString kPushAlertSetting = QStringLiteral("actions/pushAlert");
const QString kPhoneReleasesWithoutPromptSetting =
    QStringLiteral("desktopCommands/releasesWithoutPrompt");
const QString kActionAlertSetting = QStringLiteral("actions/runAlert");
const QString kActionAlertStartedSetting =
    QStringLiteral("actions/runAlertStarted");
const QString kActionAlertModeSetting = QStringLiteral("actions/runAlertMode");
const QString kAutoDeleteCancelledRunsSetting =
    QStringLiteral("actions/autoDeleteCancelledRuns");

QString actionAlertMode();

// Per-event Action pings (adhoc #384). Each run event has two switches: whether
// it files a Ping at all (the Pings row and its sound), and whether it
// also raises a system pop-up. Starts and successes are routine, so they file
// nothing by default; failures, stops and approval requests still do. A
// pop-up's default comes from the older run-alert combo and started checkbox,
// so an existing choice carries over.
enum class ActionPingEvent { Started, Succeeded, Failed, Stopped, Approval };
QString actionPingEventLabel(ActionPingEvent event);
bool actionPingEnabled(ActionPingEvent event);
void setActionPingEnabled(ActionPingEvent event, bool enabled);
bool actionPopupEnabled(ActionPingEvent event);
void setActionPopupEnabled(ActionPingEvent event, bool enabled);
const QString kNodeConnectAlertSetting = QStringLiteral("notifications/nodeConnect");
const QString kDisbursementAlertSetting = QStringLiteral("notifications/disbursement");
const QString kChatMessageAlertSetting = QStringLiteral("notifications/chatMessages");
const QString kMentionAlertSetting = QStringLiteral("notifications/mentions");
const QString kIssueAlertSetting = QStringLiteral("notifications/issues");
const QString kPullAlertSetting = QStringLiteral("notifications/pullRequests");
const QString kCommentAlertSetting = QStringLiteral("notifications/comments");
const QString kMirrorUpdateAlertSetting = QStringLiteral("notifications/mirrorUpdated");
const QString kCoveOpenAlertSetting = QStringLiteral("notifications/coveOpened");
const QString kNewUserAlertSetting = QStringLiteral("notifications/newUser");
const QString kErrorLogAlertSetting = QStringLiteral("notifications/errorLogFlash");
// Log categories the user has switched off. A muted category is not recorded
// at all - not to the buffer, the live file, the chart digest or the activity
// lights - so this is a logging switch, not the chip row's view filter.
const QString kMutedLogCategoriesSetting =
    QStringLiteral("logs/mutedCategories");
const QString kLoggingOffSetting = QStringLiteral("logs/allOff");
const QString kSystemAlertSetting = QStringLiteral("notifications/systemAlert");
// Native notification-centre cards and taskbar attention are a second,
// outside-the-app interruption rail. Keep that rail opt-in even when a
// particular event category is enabled; every ping is still filed locally.
const QString kNativeSystemNotificationsSetting =
    QStringLiteral("notifications/nativeSystemNotifications");

bool nativeSystemNotificationsEnabled();

// Operational alerts are the relay's own health transitions. They remain in
// the Pings inbox by default, but an untouched install does not sound an alert.
bool forkMeshSystemAlertsEnabled();
const QString kEmailNotifyMentionSetting = QStringLiteral("notifications/email/mention");
const QString kEmailNotifySubscribedSetting = QStringLiteral("notifications/email/subscribed");
const QString kEmailNotifyPullSubmittedSetting = QStringLiteral("notifications/email/pullSubmitted");
const QString kEmailNotifyIssueAssignedSetting = QStringLiteral("notifications/email/issueAssigned");
const QString kEmailNotifyRepoSharedSetting = QStringLiteral("notifications/email/repoShared");
const QString kEmailNotifyBountyFundedSetting = QStringLiteral("notifications/email/bountyFunded");
const QString kEmailNotifyBountyPaidSetting = QStringLiteral("notifications/email/bountyPaid");
const QString kEmailNotifyReleasePublishedSetting = QStringLiteral("notifications/email/releasePublished");
const QString kEmailNotifyPendingInboxSetting = QStringLiteral("notifications/email/pendingInbox");
const QString kEmailNotifyCreditsRefilledSetting = QStringLiteral("notifications/email/creditsRefilled");
const QString kEmailNotifyGeneralChatSetting = QStringLiteral("notifications/email/generalChat");
const QString kEmailNotifyHostOnlineSetting = QStringLiteral("notifications/email/hostOnline");
const QString kEmailNotifyHostOfflineSetting = QStringLiteral("notifications/email/hostOffline");
// Pacing for the emails the keys above opt into: "digest" rolls them up,
// "instant" sends each ping on its own the moment the relay sees it.
const QString kEmailNotifyDeliverySetting = QStringLiteral("notifications/email/delivery");
const QString kWelcomeChannel = QStringLiteral("#office/welcome");
const qint64 kWelcomePingFreshMs = 5 * 60 * 1000;
const QString kLegacyWelcomeAnnouncedSettingPrefix =
    QStringLiteral("chat/welcomeAnnounced/");

bool notifyEnabled(const QString &key);
// Pause notifications: a deadline in ms since the epoch, not a mode, so it
// lifts on its own and the same instant reads identically here and on the relay
// (the signed heartbeat carries it both ways). While it is in the future Pings
// keep arriving and being filed, but none may raise an OS card, tray balloon,
// taskbar attention flash, or alert sound; the relay mails none of them.
const QString kPingPauseUntilSetting =
    QStringLiteral("notifications/pauseUntilMs");
// A local pause can be chosen while the relay is offline or a prior heartbeat
// is in flight. Persist the unsent intent separately from the deadline so an
// app restart cannot silently discard it.
const QString kPingPausePushPendingSetting =
    QStringLiteral("notifications/pausePushPending");
// When the Pings section was last opened; local pings filed after it count on
// the rail badge. Local pings have no per-row read state, unlike website ones.
const QString kLocalPingsSeenAtSetting =
    QStringLiteral("notifications/localSeenAtMs");
constexpr qint64 kPingPauseHourMs = 60 * 60 * 1000;
// Mirrors PING_PAUSE_MAX_MS on the relay, which clamps anything longer.
constexpr qint64 kPingPauseMaxMs = 30 * 24 * kPingPauseHourMs;
constexpr int kPingPauseMaxHours = 30 * 24;

qint64 pingPauseUntilMs();

// True inside a pause or the daily quiet hours (#347): both mute every
// interruption rail while pings keep being filed.
bool pingsPaused();

// How a live pause reads in a tooltip or a status line, e.g. "Notifications
// paused until 2:30 PM" today, "Notifications paused until Fri 22 Aug, 9:00
// AM" beyond it. The clock format follows the system locale, as the website's
// does.
QString pingPauseLabel(qint64 untilMs);
const QString kAutoPrBountyEnabledSetting =
    QStringLiteral("bounty/autoPrEnabled");
const QString kAutoPrBountyAmountSetting =
    QStringLiteral("bounty/autoPrAmountUsd");
const QString kAutoPrBountyModeSetting = QStringLiteral("bounty/autoPrMode");
const QString kSolanaLastBalanceSettingPrefix =
    QStringLiteral("profile/solanaLastBalance/");
const QString kWindowGeometrySetting = QStringLiteral("ui/windowGeometry");
const QString kWindowOpacitySetting = QStringLiteral("ui/windowOpacity");
constexpr int kMinimumWindowOpacityPercent = 30;
constexpr int kDefaultWindowOpacityPercent = 100;

int windowOpacityPercent();

// Background veil over the main window (WindowBackdrop.h): "none", "color" or
// "image", with a strength in percent of the veil's opacity.
const QString kWindowBackdropModeSetting = QStringLiteral("ui/backdropMode");
const QString kWindowBackdropColorSetting = QStringLiteral("ui/backdropColor");
const QString kWindowBackdropImageSetting = QStringLiteral("ui/backdropImage");
const QString kWindowBackdropStrengthSetting =
    QStringLiteral("ui/backdropStrength");
constexpr int kMaximumWindowBackdropStrengthPercent = 80;
constexpr int kDefaultWindowBackdropStrengthPercent = 20;
const QString kDefaultWindowBackdropColor = QStringLiteral("#1f6feb");

int windowBackdropStrengthPercent();

// Chat sidebar: the divider is a splitter handle, and the chosen width keeps
// across launches. The bounds keep both panes usable on a narrow window.
const QString kChatSidebarWidthSetting = QStringLiteral("ui/chatSidebarWidth");
constexpr int kChatSidebarDefaultWidth = 240;
constexpr int kChatSidebarMinWidth = 200;
constexpr int kChatSidebarMaxWidth = 720;
constexpr int kChatMainColumnMinWidth = 320;
constexpr int kChatThreadPaneMinWidth = 360;
constexpr int kChatThreadPaneDefaultWidth = 440;
constexpr int kChatBarHeight = 48;
constexpr int kChatUserPanelHeight = 52;
constexpr int kChatUserAvatarSide = 32;
constexpr int kChatMembersPanelWidth = 240;
constexpr int kChatMembersPanelMinBodyWidth = 720;
constexpr int kChatMemberAvatarSide = 32;
constexpr int kChatListAvatarSide = 28;
const QString kChatMembersPanelSetting = QStringLiteral("chat/memberListVisible");
// Sidebar channel groups, one compact JSON document (ChannelGroups.h).
const QString kChannelGroupsSetting = QStringLiteral("chat/channelGroups");
// Which layout preset the "Change view" button in the top-right chrome last
// selected (forkmesh::ui::appViewModeKey). Stored as the mode's name, not its
// enum value, so reordering the list cannot repoint a saved preference.
const QString kAppViewModeSetting = QStringLiteral("ui/viewMode");
const QString kShowRebuildButtonSetting = QStringLiteral("ui/showRebuildButton");
// Drop the caption under each left-rail icon, leaving an icon-only rail that is
// both narrower and shorter per item (forkmesh::ui::railCaptionsHidden()).
const QString kHideRailCaptionsSetting = QStringLiteral("ui/hideRailCaptions");
const QString kHiddenTopBarIndicatorsSetting =
    QStringLiteral("ui/hiddenTopBarIndicators");
const QString kShowTeammateRailSetting = QStringLiteral("ui/showTeammateRail");
const QString kShowPackageRailSetting = QStringLiteral("ui/showPackageRail");
const QString kPackageRailReminderSetting = QStringLiteral("ui/packageRailReminder");
const QString kTeammateCreatorSessionsSetting =
    QStringLiteral("teammates/creatorSessions");
const QString kTeammateDoneUnseenSetting =
    QStringLiteral("teammates/doneUnseenSessions");
const QString kTeammateWorktreesSetting =
    QStringLiteral("teammates/ownWorktrees");
const QString kAgentSeenFinishedAtSetting =
    QStringLiteral("agents/seenFinishedAt");
const QString kAgentReadBaselineSetting =
    QStringLiteral("agents/readBaselineMs");
// Order the left rail by how often each item is clicked, most clicked first
// (MainWindow::applyRailOrder). The counts live under kRailClickCountsSetting,
// keyed by a stable rail key rather than the caption, and are kept whether or
// not the sort is on so switching it on has history to sort by.
const QString kSortRailByClicksSetting = QStringLiteral("ui/sortRailByClicks");
const QString kRailClickCountsSetting = QStringLiteral("ui/railClickCounts");
const QString kRailFoldersSetting = QStringLiteral("ui/railFolders");
const QString kRailSortModeSetting = QStringLiteral("ui/railSortMode");
const QString kRailGroupedSetting = QStringLiteral("ui/railGrouped");
const QString kRailGroupsSetting = QStringLiteral("ui/railGroups");
const QString kRailOrderSetting = QStringLiteral("ui/railOrder");
const QString kRailHiddenSetting = QStringLiteral("ui/railHidden");
const QString kRailItemMimeType =
    QStringLiteral("application/x-forkmesh-rail-item");
const QString kPoppedOutPageMimeType =
    QStringLiteral("application/x-forkmesh-popped-out-page");
const QString kButtonPlacementSetting = QStringLiteral("ui/buttonPlacement");
const QString kButtonPanelRail = QStringLiteral("rail");
const QString kButtonPanelRepoTabs = QStringLiteral("repoTabs");
const QString kButtonPanelRepoCode = QStringLiteral("repoCode");
// Switch Listen on for a run as it starts, rather than waiting to be asked
// (AgentSonifier.h). With a fleet going this turns the app into something you
// can leave behind you and still know the shape of: each new agent takes a
// seat in the mix the moment it begins.
const QString kListenToNewAgentsSetting =
    QStringLiteral("agents/listenToNewRuns");
const QString kShowDebugBarOnStartupSetting =
    QStringLiteral("ui/showDebugBarOnStartup");
const QString kDiagnosticsReportBaselineSetting =
    QStringLiteral("diagnostics/selfFixReportBaseline");
const QString kCloudLogMonitorSetting =
    QStringLiteral("diagnostics/cloudLogMonitor");
const QString kCloudLogMonitorMissingWorkerSetting =
    QStringLiteral("diagnostics/cloudLogMonitorMissingWorker");
constexpr int kCloudLogMonitorStartupDelayMs = 5000;
const QString kCoreLogFeedSetting =
    QStringLiteral("diagnostics/coreNodeLogFeed");
const QString kVerboseNetworkLogSetting =
    QStringLiteral("diagnostics/verboseNetworkLog");
const QString kRequestFirewallEnabledSetting =
    QStringLiteral("firewall/whitelistOnly");
const QString kRequestFirewallWhitelistSetting =
    QStringLiteral("firewall/whitelist");
const QString kRequestFirewallDenylistSetting =
    QStringLiteral("firewall/denylist");
const QString kRequestFirewallDefaultSeededSetting =
    QStringLiteral("firewall/defaultWhitelistSeeded");
const QString kRequestFirewallBuiltinApprovalsRevokedSetting =
    QStringLiteral("firewall/builtinApprovalsRevoked");
constexpr int kRequestFirewallHistoryLimit = 100;

// ForkMesh's own services. These used to be seeded as allow rules on every
// install, so the app reached them without anyone deciding to. Every external
// destination now needs an explicit Allow or Deny; only a run with no screen
// to ask on keeps these pre-approved.
QStringList defaultRequestFirewallWhitelist();

// An offscreen run (a headless node, a scripted measurement) has nobody to
// answer the prompt, so an unlisted destination there is simply blocked.
bool requestFirewallCanPrompt();

const QString kRequestFirewallSeededDefaultsSetting =
    QStringLiteral("firewall/seededDefaultRules");
const QString kDefaultApprovedFirewallRulesResource =
    QStringLiteral(":/firewall/default-approved.json");

// The URLs ForkMesh cannot work without (sign-in, sync, updates, the relay
// sockets), shipped with every download from
// desktop/resources/firewall/default-approved.json. URL patterns on the
// default relay, not whole domains.
QList<BackoffNetworkAccessManager::FirewallRuleEntry>
defaultApprovedFirewallRules();

// Seeds each shipped rule once. A rule the person removed or denied stays
// that way; a rule a later release adds to the list is seeded on upgrade.
void seedDefaultApprovedFirewallRules(QSettings &settings, QStringList *rules);

QStringList requestFirewallWhitelist();

QStringList requestFirewallDenylist();

const QString kNetworkEndpointCatalogResource =
    QStringLiteral(":/network/endpoint-catalog.json");

// What every network call is for, how important it is, a modest rate for it
// and how likely it really has to be a request rather than a push, an event
// or a refresh. Shipped in desktop/resources/network/endpoint-catalog.json,
// read once.
const QList<BackoffNetworkAccessManager::EndpointProfile> &
networkEndpointCatalog();
const QString kAutoSyncIssuesSetting = QStringLiteral("repos/autoSyncIssues");
const QString kAutoSyncOnMergeSetting = QStringLiteral("repos/autoSyncOnMerge");
const QString kAutoUpdateSetting = QStringLiteral("update/autoUpdate");
// How long a deleted agent stays recoverable in the recycle bin (issue #578).
// Hours because that is the unit the Settings control offers, and the default
// is the 24 hours the request asked for.
constexpr int kAgentRecycleRetentionMinHours =
    AgentStore::kMinRecycleRetentionHours;
constexpr int kAgentRecycleRetentionMaxHours =
    AgentStore::kMaxRecycleRetentionHours;
constexpr int kAgentRecycleRetentionDefaultHours =
    AgentStore::kDefaultRecycleRetentionHours;
const QString kAgentRecycleRetentionSetting =
    QStringLiteral("agents/recycleRetentionHours");

int agentRecycleRetentionHours();

qint64 agentRecycleRetentionMs();
const QString kAutoUpdateRestartSetting = QStringLiteral("update/autoUpdateRestart");
const QString kReopenAfterCloseSetting = QStringLiteral("startup/reopenAfterClose");
const QString kAutoBackupEnabledSetting = QStringLiteral("backup/hourlyEnabled");
const QString kAutoBackupKeepSetting = QStringLiteral("backup/keepCount");
const QString kAutoAgentOnStallSetting =
    QStringLiteral("diagnostics/autoAgentOnStall");
const QString kStallRepairSessionSetting =
    QStringLiteral("diagnostics/stallRepairSession");
const QString kHighMemoryAutoKillSetting =
    QStringLiteral("diagnostics/highMemoryAutoKill");
const QString kAutoAgentOnErrorLogSetting =
    QStringLiteral("diagnostics/autoAgentOnErrorLog");
const QString kCrashLogSeenOffsetSetting =
    QStringLiteral("diagnostics/crashLogSeenOffset");
// Newest crash-record epoch (seconds) already handed to a repair teammate.
// Whether that repair *succeeded* is the monitor runtime's checkpoint; this
// only keeps one crash from being queued again on every later launch.
const QString kCrashRepairObservedEpochSetting =
    QStringLiteral("diagnostics/crashRepairObservedEpoch");
const QString kVotesSpentSetting = QStringLiteral("votes/spent");
const QString kVotedSetting = QStringLiteral("votes/voted");
// Personal access tokens used only to authenticate clones when importing a repo
// from GitHub/GitLab, which lifts the unauthenticated clone rate limits. Stored
// locally; never published or sent anywhere but the provider's git endpoint.
const QString kGithubTokenSetting = QStringLiteral("import/githubToken");
const QString kGitlabTokenSetting = QStringLiteral("import/gitlabToken");
const QString kCodexApiKeySetting = QStringLiteral("agents/codexApiKey");
const QString kOpenAiAdminKeySetting = QStringLiteral("agents/openAiAdminKey");
const QString kWhisperDirSetting = QStringLiteral("voice/whisperDir");
const QString kWhisperModelSetting = QStringLiteral("voice/whisperModel");
// Public World origin allowed to control local speech-to-text. This is not a
// capability; one-use/session secrets are memory-only inside WorldSpeechBridge.
const QString kWorldSpeechOriginSetting =
    QStringLiteral("voice/worldExactOrigin");
const QString kVoiceEngineSetting = QStringLiteral("voice/engine");
const QString kParakeetModelSetting = QStringLiteral("voice/parakeetModel");
const QString kVoiceInputDeviceSetting = QStringLiteral("voice/inputDevice");
const QString kVoiceInputEnabledSetting = QStringLiteral("voice/inputEnabled");
const QString kCodexModelSetting = QStringLiteral("agents/codexModel");
const QString kIssueAskAiModel = QStringLiteral("gpt-4.1-nano");
// How much of an issue thread a model prompt carries, and the answer budget an
// @model mention is given. Both bound what one comment can spend.
constexpr int kIssueAiContextChars = 12000;
// And how much of the repository itself rides along with an @model mention:
// commit history, the pull requests, and any file the thread points at. A
// mention that asks "was this fixed?" is answered from here, not the thread,
// so it gets its own budget rather than eating the conversation's.
constexpr int kIssueRepoContextChars = 16000;
constexpr int kIssueModelMentionMaxTokens = 900;
// A reasoning model bills its thinking against that same output budget and
// writes the prose afterwards, so a budget this small can be spent before a
// single word comes back -- an answer that arrives empty with nothing wrong.
// One retry with room for both is what turns that into a reply.
constexpr int kIssueModelMentionRetryMaxTokens = 4000;
const QString kQuickAddHistorySetting = QStringLiteral("issues/quickAddHistory");
// How many times each Shortcuts-page entry has actually been launched, so the
// merged shortcuts+skills list can lead with what this device really uses.
// Keys are "<scope>:<owner/name>:<file>" for repository entries and
// "<scope>:<file>" for the device-wide app/organization skill scopes.
const QString kShortcutUsageSetting = QStringLiteral("shortcuts/usageCounts");
// Outcome history behind each card's last-run, success-rate and duration line,
// keyed like kShortcutUsageSetting. Each value is a map of lastRunAtMs,
// finished, succeeded, timed, totalMs and lastMs.
const QString kShortcutRunStatsSetting = QStringLiteral("shortcuts/runStats");
// Dedicated agents a shortcut or skill started and that have not finished yet:
// agent session id -> usage key, so the outcome lands on the right card even
// after a restart.
const QString kShortcutAgentRunsSetting = QStringLiteral("shortcuts/agentRuns");
const QString kShortcutViewSetting = QStringLiteral("shortcuts/view");
const QString kQuickAddCreateIssueSetting =
    QStringLiteral("issues/quickAddCreateIssue");
// Genie: the website's remote-MCP setup, as generated by
// Organization Admin → "Remote ForkMesh MCP". The token is the revocable
// task-only bearer credential; the org names whose shared task list the agent
// works; the workflow picks the finishing sequence ("pr" or "deploy"). The URL
// defaults to the active relay's /mcp endpoint when left blank.
const QString kGenieTokenSetting = QStringLiteral("genie/token");
const QString kGenieOrgSetting = QStringLiteral("genie/org");
const QString kGenieWorkflowSetting = QStringLiteral("genie/workflow");
const QString kGenieUrlSetting = QStringLiteral("genie/mcpUrl");
const QString kClaudeApiKeySetting = QStringLiteral("agents/claudeApiKey");
const QString kClaudeAdminKeySetting = QStringLiteral("agents/claudeAdminKey");
const QString kCodexCommandSetting = QStringLiteral("agents/codexCommand");
const QString kClaudeCommandSetting = QStringLiteral("agents/claudeCommand");
const QString kAgentContextSetting = QStringLiteral("agents/contextWindow");
const QString kAgentMaxOutputSetting = QStringLiteral("agents/maxOutputTokens");
const QString kAgentPromptPreambleSetting =
    QStringLiteral("agents/promptPreamble");
const QString kPrioritizePromptSetting =
    QStringLiteral("agents/prioritizePrompt");
const QString kOpenAiSpendTextSetting = QStringLiteral("agents/openAiSpendText");
const QString kOpenAiSpendTsSetting = QStringLiteral("agents/openAiSpendTs");
const QString kClaudeSpendTextSetting = QStringLiteral("agents/claudeSpendText");
const QString kClaudeSpendTsSetting = QStringLiteral("agents/claudeSpendTs");
const QString kOpenAiCreditTextSetting = QStringLiteral("agents/openAiCreditText");
const QString kOpenAiCreditTsSetting = QStringLiteral("agents/openAiCreditTs");
const QString kClaudeCreditTextSetting = QStringLiteral("agents/claudeCreditText");
const QString kClaudeCreditTsSetting = QStringLiteral("agents/claudeCreditTs");
const QString kCodexLimit5hStartSetting = QStringLiteral("agents/codexLimit5hStart");
const QString kCodexLimitWeekStartSetting = QStringLiteral("agents/codexLimitWeekStart");
const QString kCodexUsage5hPctSetting = QStringLiteral("agents/codexUsage5hPct");
const QString kCodexUsageWeekPctSetting = QStringLiteral("agents/codexUsageWeekPct");
const QString kCodexUsage5hResetSetting = QStringLiteral("agents/codexUsage5hReset");
const QString kCodexUsageWeekResetSetting = QStringLiteral("agents/codexUsageWeekReset");
// Ordered, normalized rows from account/rateLimits/read. Unlike the legacy
// four scalar settings above, this preserves model-specific buckets such as
// Codex Spark's independent 5-hour and weekly windows.
const QString kCodexUsageWindowsSetting =
    QStringLiteral("agents/codexUsageWindowsV2");

QJsonArray codexUsageWindows(const QVariant &stored);

QByteArray codexUsageWindowsJson(const QJsonArray &windows);
const QString kClaudeLimit5hStartSetting = QStringLiteral("agents/claudeLimit5hStart");
const QString kClaudeLimitWeekStartSetting = QStringLiteral("agents/claudeLimitWeekStart");
const QString kClaudeUsage5hPctSetting = QStringLiteral("agents/claudeUsage5hPct");
const QString kClaudeUsageWeekPctSetting = QStringLiteral("agents/claudeUsageWeekPct");
const QString kClaudeUsage5hResetSetting = QStringLiteral("agents/claudeUsage5hReset");
const QString kClaudeUsageWeekResetSetting = QStringLiteral("agents/claudeUsageWeekReset");
const QString kClaudeUsageFablePctSetting = QStringLiteral("agents/claudeUsageFablePct");
const QString kClaudeUsageFableResetSetting = QStringLiteral("agents/claudeUsageFableReset");
// Extra usage — "usage credits" in Claude's own wording — rides along in the
// same usage payload as the rate-limit windows. Money arrives in minor units
// of the account currency (cents for USD), so it is stored that way and only
// formatted for display.
const QString kClaudeCreditsEnabledSetting = QStringLiteral("agents/claudeCreditsEnabled");
const QString kClaudeCreditsUsedSetting = QStringLiteral("agents/claudeCreditsUsed");
const QString kClaudeCreditsLimitSetting = QStringLiteral("agents/claudeCreditsLimit");
const QString kClaudeCreditsPctSetting = QStringLiteral("agents/claudeCreditsPct");
const QString kClaudeCreditsCurrencySetting =
    QStringLiteral("agents/claudeCreditsCurrency");

QString formatUsageCredits(double minorUnits, const QString &currency);
struct AgentAccountProfile {
    QString id;
    QString label;
    QString configDir;
    bool builtIn = false;
};

QString agentAccountProviderKey(const QString &provider);

QString agentAccountDefaultConfigDir(const QString &provider);

QString agentAccountCredentialPath(const QString &provider,
                                          const QString &configDir);

QString agentAccountProfilesGroup(const QString &provider);

QString agentActiveAccountSetting(const QString &provider);

QString agentAccountLabelSetting(const QString &provider,
                                        const QString &accountId);

QString agentAccountIdentitySetting(const QString &provider,
                                    const QString &accountId);

QString claudeAccountStateFile(const AgentAccountProfile &profile);

QString claudeAccountLoginRoot();

// Reading the account profiles builds a QSettings, and the GUI thread blocks on
// the global settings lock whenever a worker is mid-sync() — a 1.1 s stall was
// sampled inside that constructor here, reached from a usage response that asks
// for the active account. The three functions that write these keys
// (addAgentAccount, renameAgentAccount, selectAgentAccount) drop the cache.
QMutex &agentAccountProfileCacheMutex();

QHash<QString, QList<AgentAccountProfile>> &agentAccountProfileCache();

// The selected account id per provider, read from the same settings file.
// activeAgentAccount() is on the per-transcript-event path (credential
// redaction, usage/rate-limit updates, the conversation-id lookup), and each
// call built a QSettings just for this one key — the profile cache above was
// added one level too shallow. Cleared together with the profiles; the one
// writer (selectAgentAccount) invalidates after it stores the new id.
QHash<QString, QString> &activeAgentAccountIdCache();

void invalidateAgentAccountProfileCache();

QList<AgentAccountProfile> agentAccountProfilesUncached(
    const QString &provider);

QList<AgentAccountProfile> agentAccountProfiles(const QString &provider);

AgentAccountProfile activeAgentAccount(const QString &provider);

bool agentAccountSignedIn(const QString &provider, const QString &configDir);

QString agentAccountIdentity(const QString &provider, const QString &configDir);

QList<forkmesh::localcontrol::agentops::UsageWindow>
agentAccountUsageWindows(const QString &provider, const QString &accountId);

struct AgentUsageMenuData {
    QString label;
    QString value;
    QString resetNote;
    QString detail;
    int usedPercent = -1;
    qint64 resetAtMs = 0;
    qint64 windowMs = 0;
};

QList<AgentUsageMenuData> agentAccountUsageMenuData(const QString &provider,
                                                    const QString &accountId);

QString agentAccountUsageSetting(const QString &provider,
                                        const QString &accountId,
                                        const QString &field);

QString agentAccountUsageField(const QString &globalSetting);

QStringList activeAgentAccountEnv(const QString &provider);
constexpr qint64 kAgentLimit5hMs = 5LL * 60 * 60 * 1000;
constexpr qint64 kAgentLimitWeekMs = 7LL * 24 * 60 * 60 * 1000;
const QString kClaudeUsage5hExhaustedSetting = QStringLiteral("agents/claudeUsage5hExhausted");
const QString kClaudeUsageWeekExhaustedSetting = QStringLiteral("agents/claudeUsageWeekExhausted");
const QString kClaudeUsageFableExhaustedSetting = QStringLiteral("agents/claudeUsageFableExhausted");
const QString kCodexUsage5hExhaustedSetting = QStringLiteral("agents/codexUsage5hExhausted");
const QString kCodexUsageWeekExhaustedSetting = QStringLiteral("agents/codexUsageWeekExhausted");
const QString kEmailOnCreditsRefillSetting = QStringLiteral("agents/emailOnCreditsRefill");
const QString kUsageLimitCalendarReminderSetting =
    QStringLiteral("agents/usageLimitCalendarReminder");
const QString kUsageLimitReminderScheduledPrefix =
    QStringLiteral("agents/usageLimitReminderScheduled/");
// The reset a calendar file was written for, tracked apart from the armed
// timer above so switching the calendar option on still exports the window
// that is already counting down.
const QString kUsageLimitCalendarWrittenPrefix =
    QStringLiteral("agents/usageLimitCalendarWritten/");
const QString kUsageLimitReminderNotifiedPrefix =
    QStringLiteral("agents/usageLimitReminderNotified/");
const QString kUsageLimitAutoContinueSetting =
    QStringLiteral("agents/usageLimitAutoContinue");
const QString kUsageLimitAgentHoldGroup =
    QStringLiteral("agents/usageLimitAgentHold");

QString humanizeRemaining(qint64 ms);

const QString kDefaultCodexCommand =
    QStringLiteral("codex -a never {modelArg} exec --sandbox danger-full-access - < {promptFile}");
const QString kPreviousCodexCommand =
    QStringLiteral("codex -a never exec --sandbox workspace-write - < {promptFile}");
const QString kOlderCodexCommand =
    QStringLiteral("codex exec --sandbox workspace-write - < {promptFile}");
const QString kLegacyCodexCommand =
    QStringLiteral("codex exec --sandbox workspace-write --ask-for-approval never \"$(cat {promptFile})\"");
const QString kLegacyClaudeCommand =
    QStringLiteral("claude -p \"$(cat {promptFile})\" --dangerously-skip-permissions");
const QString kClaudeCodeCommandSetting = QStringLiteral("agents/claudeCodeCommand");
const QString kClaudeCodeModelSetting = QStringLiteral("agents/claudeCodeModel");
const QString kReleaseNotesClaudeModelSetting =
    QStringLiteral("agents/releaseNotesClaudeModel");
const QString kReleaseNotesGptModelSetting =
    QStringLiteral("agents/releaseNotesGptModel");
const QString kClaudeModelsCacheSetting = QStringLiteral("agents/claudeModelsCache");
const QString kCodexModelsCacheSetting = QStringLiteral("agents/codexModelsCache");
const QString kCloudflareAiModelsCacheSetting =
    QStringLiteral("agents/cloudflareAiModelsCache");
const QString kCloudflareAiModelSetting =
    QStringLiteral("agents/cloudflareAiModel");
// Empty keeps the existing local title slugger. A Workers AI model id opts in
// to sending new ad-hoc prompts to the relay for semantic workspace names.
const QString kAgentWorkspaceNameModelSetting =
    QStringLiteral("agents/workspaceNameCloudflareModel");
const QString kAgentTitleFromAgentSetting =
    QStringLiteral("agents/titleFromAgent");
const QString kLocalLlamaModelSetting =
    QStringLiteral("agents/localLlamaModel");
const QString kLocalLlamaEndpointSetting =
    QStringLiteral("agents/localLlamaEndpoint");
// Whether a thinking-capable local model (Gemma 4, gpt-oss…) reasons before
// each step. Off by default: on CPU-bound boxes thinking multiplies the
// per-turn latency and the structured tools carry the reasoning instead.
const QString kLocalLlamaThinkingSetting =
    QStringLiteral("agents/localLlamaThinking");
const QString kMarketplaceLocalModelsCacheSetting =
    QStringLiteral("agents/marketplaceLocalModels");
const QString kDefaultLocalLlamaEndpoint =
    QStringLiteral("http://127.0.0.1:11434/v1/chat/completions");
// Composer "Auto mode" toggle: true => run Claude Code unattended (skip the
// permission prompts). Read when a transcript session launches.
const QString kClaudeAutoModeSetting = QStringLiteral("agents/claudeAutoMode");
const QString kAgentModeSetting = QStringLiteral("agents/cliPermissionMode");
// The composer mode-selector labels. One word each so the whole
// composer row stays compact — "Auto mode" was the only one carrying the word
// "mode" and the dropdown itself already says what it is. Codex maps each label
// to an approval policy/sandbox by substring ("ask", "edit", "plan", "auto"),
// so these names are what it keys off too; Claude Code takes the ids
// agentPermissionModeId() maps them to.
const QString kClaudeAutoModeLabel = QStringLiteral("Auto");
const QString kAgentAskModeLabel = QStringLiteral("Ask");

// Does a session's stored permission-mode label (AgentSession::mode) run the
// agent unattended? An empty label means the session predates per-session mode
// capture, so callers fall back to the global kClaudeAutoModeSetting. Sessions
// (and the saved kAgentModeSetting) written before the labels were shortened
// still say "Auto mode", and must keep running unattended.
bool agentModeSkipsPermissions(const QString &modeLabel);

// Claude Code's own name for a mode label — what `--permission-mode` takes at
// launch and what a set_permission_mode control request switches a running
// session to (ClaudeTranscriptView maps them back for display). Auto is the
// only one that needs --dangerously-skip-permissions instead of the flag, and
// it is the one switch a running CLI refuses: bypassing every check is offered
// only to a process that was launched that way. An empty label yields an empty
// id, which callers read as "no mode recorded, use the global default".
QString agentPermissionModeId(const QString &modeLabel);
const QString kClaudeEffortSetting = QStringLiteral("agents/claudeEffort");
const QString kClaudeEffortLevelsCacheSetting =
    QStringLiteral("agents/claudeEffortLevels");
QStringList defaultAgentEffortLevels();
QString agentEffortLabel(const QString &level);
const QString kClaudeThinkingSetting = QStringLiteral("agents/claudeThinking");
const QString kClaudeFallbackModelSetting =
    QStringLiteral("agents/claudeFallbackModel");
const QString kAutoSwitchToAgentSetting = QStringLiteral("agents/autoSwitchToAgent");
const QString kTerminalTranscriptOutputSetting =
    QStringLiteral("agents/terminalTranscriptOutput");
// Issues: let an "@opus ..." / "@fable fix this" mention in an issue thread
// reach the model it names — the host answers in the thread, or starts a coding
// agent when the mention asks for work (see forkmesh::issuemention). Only the
// node that can write the issue store acts, so one mention gets one reply.
// Handled mentions and the per-repo seed marker are remembered so a rescan
// never answers the same comment twice and adding a repo full of history never
// fires a burst of replies.
const QString kIssueModelMentionsSetting =
    QStringLiteral("issues/modelMentions");
const QString kIssueModelMentionsHandledSetting =
    QStringLiteral("issues/modelMentionsHandled");
// Per repository, "<owner>/<name> <epoch ms>": mentions older than the stamp
// have already been considered and are never looked at again. The stamp is
// what keeps the handled-key list below small — a key is only ever written for
// a mention that was actually answered — and what makes a first scan of a
// repository full of history cost one write instead of a burst of replies.
const QString kIssueModelMentionsFloorSetting =
    QStringLiteral("issues/modelMentionsConsideredBefore");
// At most this many mentions are acted on per scan (the rest wait for the next
// one), and this many handled keys are kept: every setValue() rewrites the
// whole ini, so the list is not allowed to grow without bound. Evicting the
// oldest key is safe because its mention is far below that repository's stamp
// by then, so it is skipped before the key is ever consulted.
constexpr int kIssueModelMentionsPerScan = 3;
constexpr int kIssueModelMentionsHandledMax = 4000;
// A mention this recent is still answered on the first scan of a repository:
// without the window, the first mention someone types in a repository this
// node has not scanned before would be filed as history and go unanswered.
constexpr qint64 kIssueModelMentionsSeedGraceMs = 10 * 60 * 1000;
// When on, agent sessions started here are owner-encrypted before their opaque
// snapshots reach the relay. The browser deliberately has no recipient private
// key; inspection and steering remain on the owner desktop. Off by default so
// no agent metadata leaves this machine unless the user opts in.
const QString kPublishAgentsToWebSetting =
    QStringLiteral("agents/publishToWeb");
const QString kPublishedAgentReposSetting =
    QStringLiteral("agents/publishedRepos");
// The streamed provider sign-in a browser can ask this node for, gated on the
// same consent as the setting above. The replay buffer repaints a reconnecting
// tab and is held in memory only; the lifetime cap ends a session whose tab
// disappeared without closing it.
constexpr int kNodeTerminalReplayBytes = 48 * 1024;
constexpr int kNodeTerminalMaxLifetimeMs = 30 * 60 * 1000;
const QString kAutoFixFailuresSetting =
    QStringLiteral("agents/autoFixFailures");
const QString kExcludeExternalClaudeSetting =
    QStringLiteral("agents/excludeExternalClaude");
const QString kAgentJailSetting = QStringLiteral("agents/jailEnabled");
const QString kAgentJailMemoryMbSetting = QStringLiteral("agents/jailMemoryMb");
constexpr int kDefaultAgentJailMemoryMb = 4096;
constexpr int kMinAgentJailMemoryMb = 256;

int agentJailMemoryMb();
const QString kMaxRunningAgentsSetting = QStringLiteral("agents/maxRunning");
constexpr int kDefaultMaxRunningAgents = 5;
constexpr int kMinMaxRunningAgents = 1;
// How far the agents tool bar's queue drop-down counts up. A limit typed into
// Settings above this still stands; the menu just grows to include it.
constexpr int kQueueMenuMaxRunningAgents = 20;

int maxRunningAgents();
const QString kVoiceAutoSubmitSetting = QStringLiteral("agents/voiceAutoSubmit");
const QString kOrganizationTaskOpenCountSetting =
    QStringLiteral("tasks/openCount");
// Canonical prefixes this desktop signs with its account key to open, update,
// and close an organization task when it has no account session token to present (the
// authenticateSilently path holds keys, not sessions). Must stay byte-identical
// to ORG_TASK_OPEN_PROOF / ORG_TASK_COMPLETE_PROOF in the worker's entry.py.
const QString kOrgTaskOpenProof = QStringLiteral("forkmesh-org-task-open-v1");
const QString kOrgTaskCompleteProof =
    QStringLiteral("forkmesh-org-task-complete-v1");
const QString kOrgTaskUndoProof =
    QStringLiteral("forkmesh-org-task-undo-v1");
const QString kOrgTaskAgentStatusProof =
    QStringLiteral("forkmesh-org-task-agent-status-v1");
// Must match ORG_TASK_AGENT_STATUS_BATCH_PROOF in entry.py byte-for-byte.
const QString kOrgTaskAgentStatusBatchProof =
    QStringLiteral("forkmesh-org-task-agent-status-batch-v1");
constexpr int kOrgTaskAgentStatusAckTimeoutMs = 30000;
constexpr int kNodeHeartbeatAckTimeoutMs = 30000;
const QString kOrgTaskListProof = QStringLiteral("forkmesh-org-task-list-v1");
const QString kOrgTaskAttachmentProof =
    QStringLiteral("forkmesh-org-task-attachment-v1");
// Must match ORG_TASK_DELETE_PROOF in entry.py byte-for-byte.
const QString kOrgTaskDeleteProof =
    QStringLiteral("forkmesh-org-task-delete-v1");
const QString kOrgTaskUpdateProof =
    QStringLiteral("forkmesh-org-task-update-v1");
const QString kOrgTaskRemindProof =
    QStringLiteral("forkmesh-org-task-remind-v1");
const QString kOrgTaskCheckinProof =
    QStringLiteral("forkmesh-org-task-checkin-v1");
const QString kOrgTaskReplyProof =
    QStringLiteral("forkmesh-org-task-reply-v1");
const QString kOrgTaskHistoryProof =
    QStringLiteral("forkmesh-org-task-history-v1");
const QString kTaskWorkItemCacheSetting =
    QStringLiteral("tasks/workItemsCache");
// Calendar requests use distinct, short-lived account-key proofs when the
// normal desktop launch authenticated this device without retaining a password
// session token. Must match the CALENDAR_*_PROOF constants in entry.py.
const QString kCalendarListProof =
    QStringLiteral("forkmesh-calendar-list-v1");
const QString kCalendarCreateProof =
    QStringLiteral("forkmesh-calendar-create-v1");
const QString kCalendarReadProof =
    QStringLiteral("forkmesh-calendar-read-v1");
const QString kCalendarUpdateProof =
    QStringLiteral("forkmesh-calendar-update-v1");
const QString kCalendarDeleteProof =
    QStringLiteral("forkmesh-calendar-delete-v1");
const QString kCalendarRsvpProof =
    QStringLiteral("forkmesh-calendar-rsvp-v1");
// Market-plugin install/uninstall/export lifecycle requests. Must match
// PLUGIN_REQUEST_PROOF in the Worker's entry.py byte-for-byte.
const QString kPluginRequestProof =
    QStringLiteral("forkmesh-plugin-request-v1");
// Must match GENIE_CREDENTIAL_PROOF in entry.py byte-for-byte.
const QString kGenieCredentialProof =
    QStringLiteral("forkmesh-genie-credential-v1");
// Must match the ACCOUNT_ALERT_*_PROOF constants in entry.py byte-for-byte.
const QString kAccountAlertListProof =
    QStringLiteral("forkmesh-account-alert-list-v1");
const QString kAccountAlertReadProof =
    QStringLiteral("forkmesh-account-alert-read-v1");
const QString kAccountAlertDeleteProof =
    QStringLiteral("forkmesh-account-alert-delete-v1");
// Clearing all website pings is broader than deleting one row, so it requires
// its own signed proof. Must stay byte-identical to ACCOUNT_ALERT_CLEAR_PROOF
// in entry.py.
const QString kAccountAlertClearProof =
    QStringLiteral("forkmesh-account-alert-clear-v1");
// A desktop timer's expiry raised as an account ping. Must stay
// byte-identical to TIMER_PING_PROOF in app/src/timer_pings.py.
const QString kTimerPingProof =
    QStringLiteral("forkmesh-account-timer-ping-v1");
// Canonical prefixes the Settings -> Organizations panel signs with this
// machine's account key when it holds no session token. One prefix per
// operation, and the canonical additionally binds the request path plus every
// body field that decides what the write does, so a captured proof can never
// be replayed as a different edit. Must stay byte-identical to the
// ORG_ADMIN_*_PROOF constants in entry.py.
const QString kOrgAdminListProof = QStringLiteral("forkmesh-org-admin-list-v1");
const QString kOrgAdminCreateProof =
    QStringLiteral("forkmesh-org-admin-create-v1");
const QString kOrgAdminReadProof = QStringLiteral("forkmesh-org-admin-read-v1");
const QString kOrgAdminProfileProof =
    QStringLiteral("forkmesh-org-admin-profile-v1");
const QString kOrgAdminDeleteProof =
    QStringLiteral("forkmesh-org-admin-delete-v1");
const QString kOrgAdminMemberSetProof =
    QStringLiteral("forkmesh-org-admin-member-set-v1");
const QString kOrgAdminMemberRemoveProof =
    QStringLiteral("forkmesh-org-admin-member-remove-v1");
const QString kOrgAdminTeamSetProof =
    QStringLiteral("forkmesh-org-admin-team-set-v1");
const QString kOrgAdminTeamRemoveProof =
    QStringLiteral("forkmesh-org-admin-team-remove-v1");
const QString kOrgAdminTeamMemberAddProof =
    QStringLiteral("forkmesh-org-admin-team-member-add-v1");
const QString kOrgAdminTeamMemberRemoveProof =
    QStringLiteral("forkmesh-org-admin-team-member-remove-v1");
const QString kOrgAdminRepoLinkProof =
    QStringLiteral("forkmesh-org-admin-repo-link-v1");
const QString kOrgAdminRepoUnlinkProof =
    QStringLiteral("forkmesh-org-admin-repo-unlink-v1");
// Must match ORG_ADMIN_FIELD_MAX in entry.py.
constexpr int kOrgAdminFieldMax = 500;
// Admin directory reads carry signup/referral evidence that the public roster
// omits. Must match ACCOUNT_USERS_ADMIN_PROOF in entry.py byte-for-byte.
const QString kAccountUsersAdminProof =
    QStringLiteral("forkmesh-account-users-admin-v1");
const QString kAccountUserQuarantineProof =
    QStringLiteral("forkmesh-account-user-quarantine-v1");
// Sweeping the whole email-confirmation queue is its own action, so it
// carries its own proof. Must match ACCOUNT_PENDING_QUARANTINE_PROOF in
// entry.py, and its resource is the literal "pending".
const QString kAccountPendingQuarantineProof =
    QStringLiteral("forkmesh-account-pending-quarantine-v1");
const QString kAccountPendingQuarantineResource =
    QStringLiteral("pending");
// Restoring preserved account and node rows is a distinct administrator
// action. Must match ACCOUNT_USER_UNQUARANTINE_PROOF in entry.py.
const QString kAccountUserUnquarantineProof =
    QStringLiteral("forkmesh-account-user-unquarantine-v1");
// Deleting a member is irreversible and wider than quarantining one, so it
// carries its own proof. Must match ACCOUNT_USER_DELETE_PROOF in entry.py.
const QString kAccountUserDeleteProof =
    QStringLiteral("forkmesh-account-user-delete-v1");
constexpr qint64 kWebAlertPushFloorMs = 5000;
const QString kClaudeDiffSplitSetting = QStringLiteral("agents/claudeDiffSplit");
const QString kDiffFontPtSetting = QStringLiteral("ui/diffFontPt");
const QString kDefaultClaudeCodeCommand =
    QStringLiteral("claude -p \"$(cat {promptFile})\" --dangerously-skip-permissions");
const QString kClaudeCodeTerminalCommandSetting =
    QStringLiteral("agents/claudeCodeTerminalCommand");
const QString kDefaultClaudeCodeTerminalCommand =
    QStringLiteral("claude \"$(cat {promptFile})\" --dangerously-skip-permissions");
// Prior interactive default that prompted for every permission. Migrated to the
// full-accept default above so existing sessions stop stalling on prompts.
const QString kLegacyClaudeCodeTerminalCommand =
    QStringLiteral("claude \"$(cat {promptFile})\"");
constexpr int kNetworkLogLimit = 20000;
// Rich log rows carry icons, links and coloured spans. Keep both the initial
// tail and each older-page request small enough to lay out without monopolising
// the GUI thread; rebuildNetworkLogView further divides this into event slices.
constexpr int kNetworkLogSegmentSize = 96;
constexpr int kFooterLogSeedLines = 300;
constexpr int kDebugLogTailLines = 5;

const QString kCodexProvider = QStringLiteral("codex");
const QString kClaudeCodeProvider = QStringLiteral("claude-code");

const QString kAgentDonePingKind = QStringLiteral("agent-done");
constexpr int kAgentDoneSummaryChars = 400;
const QString kAgentDoneAlertSetting = QStringLiteral("notifications/agentDone");
const QString kClaudeDesktopSessionsSetting = QStringLiteral("agents/claudeDesktopSessions");
const QString kAgentStartPingSetting = QStringLiteral("notifications/agentStart");

bool agentIsClaudeProvider(const QString &provider);

bool agentIsCodexProvider(const QString &provider);

QString cliProviderLabel(const QString &provider);

bool agentUsesOpenAiKey(const QString &provider);

const QString kCloudflareAiProvider = QStringLiteral("cloudflare-ai");
const QString kLocalLlamaProvider = QStringLiteral("local-llama");

bool agentIsCloudflareAiProvider(const QString &provider);

bool agentIsLocalLlamaProvider(const QString &provider);

// Providers whose combined-picker row names a model rather than the agent
// itself; the headless API agents have no model of their own to pick.
bool agentSelectorTakesModel(const QString &provider);

const QString kDefaultAgentProviderSetting =
    QStringLiteral("agents/defaultProvider");
const QString kQuickAddAgentProviderSetting =
    QStringLiteral("agents/quickAddProvider");
const QString kFallbackAgentProvider = QStringLiteral("openai");
// The prompt picker also offers a couple of destinations which do not start
// an agent. Keep them out of the normal provider catalog (they cannot become
// a default coding agent), but preserve a deliberate prompt selection.
const QString kActivityPubPromptProvider = QStringLiteral("activitypub");
const QString kNotePromptProvider = QStringLiteral("note");
const QString kTaskPromptProvider = QStringLiteral("task");
const QString kEditComposerModelsRow = QStringLiteral("edit-models");
const QString kQuickAddTaskAssigneeSetting =
    QStringLiteral("tasks/composerAssignee");

bool quickAddProviderIsPromptAction(const QString &provider);

bool agentProviderIsKnown(const QString &provider);

QString defaultAgentProvider();

QString quickAddAgentProvider();

void selectDefaultAgentProvider(QComboBox *combo);

void selectQuickAddAgentProvider(QComboBox *combo);

constexpr int kAgentChoiceDescriptionRole = Qt::UserRole + 7;

constexpr int kAgentChoiceStatusRole = Qt::UserRole + 8;
inline const QString kAgentChoiceStatusOk = QStringLiteral("ok");
inline const QString kAgentChoiceStatusFailed = QStringLiteral("failed");

QString agentChoiceStatusGlyph(const QString &state);

QColor agentChoiceStatusColour(const QString &state);

class FullPopupComboBox : public QComboBox {
public:
    explicit FullPopupComboBox(QWidget *parent = nullptr) : QComboBox(parent)
    {
        setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    }

    QSize sizeHint() const override { return currentTextSizeHint(); }
    QSize minimumSizeHint() const override { return currentTextSizeHint(); }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        if (m_hintedText != currentText()) {
            m_hintedText = currentText();
            updateGeometry();
        }
        QComboBox::paintEvent(event);
    }

    QWidget *popupContainer() const
    {
        QAbstractItemView *v = view();
        if (!v)
            return nullptr;
        for (QWidget *w = v; w; w = w->parentWidget()) {
            if (w->windowFlags().testFlag(Qt::Popup))
                return w;
        }
        QWidget *top = v->window();
        if (top && top->windowFlags().testFlag(Qt::Popup))
            return top;
        return v;
    }

    QRect availableScreenRect(const QWidget *popup) const
    {
        QScreen *screen = popup ? popup->screen() : nullptr;
        if (!screen && windowHandle())
            screen = windowHandle()->screen();
        if (!screen)
            screen = QGuiApplication::primaryScreen();
        return screen ? screen->availableGeometry()
                      : QRect(QPoint(0, 0), QSize(10000, 10000));
    }

    void showPopup() override
    {
        setMaxVisibleItems(qMax(maxVisibleItems(), count()));
        QComboBox::showPopup();
        QAbstractItemView *v = view();
        if (!v || count() == 0)
            return;
        QWidget *popup = popupContainer();
        fitPopupWidth(v, popup);
        int rowsH = 0;
        for (int row = 0; row < count(); ++row) {
            int rowH = v->sizeHintForRow(row);
            if (rowH <= 0)
                rowH = fontMetrics().height() + 8;
            rowsH += rowH;
        }
        const int fullHeight = 2 * v->frameWidth() + rowsH;
        QRect geo = popup->geometry();
        const QRect avail = availableScreenRect(popup);
        const int height = qMin(fullHeight, avail.height());
        v->setVerticalScrollBarPolicy(fullHeight <= avail.height()
                                          ? Qt::ScrollBarAlwaysOff
                                          : Qt::ScrollBarAsNeeded);
        v->setMinimumHeight(height);
        popup->setMinimumHeight(height);
        if (height <= geo.height() && geo.height() >= fullHeight)
            return; // already tall enough
        geo.setHeight(height);
        if (geo.bottom() > avail.bottom())
            geo.moveBottom(avail.bottom());
        if (geo.top() < avail.top())
            geo.moveTop(avail.top());
        popup->setGeometry(geo);
    }

    void fitPopupWidth(QAbstractItemView *v, QWidget *popup)
    {
        if (!v || !popup || count() == 0)
            return;
        const QFontMetrics fm = v->fontMetrics();
        int content = 0;
        for (int row = 0; row < count(); ++row) {
            int rowW = fm.horizontalAdvance(itemText(row));
            const QString description =
                itemData(row, kAgentChoiceDescriptionRole).toString();
            if (!description.isEmpty())
                rowW += 12 + fm.horizontalAdvance(description);
            content = qMax(content, rowW);
        }
        content += v->iconSize().width() + 28;
        int width = qMax(content, v->sizeHintForColumn(0)) + 2 * v->frameWidth();
        QRect geo = popup->geometry();
        if (geo.width() >= width)
            return;
        const QRect avail = availableScreenRect(popup);
        width = qMin(width, avail.width());
        v->setMinimumWidth(width - 2 * v->frameWidth());
        popup->setMinimumWidth(width);
        geo.setWidth(width);
        if (geo.right() > avail.right())
            geo.moveRight(avail.right());
        if (geo.left() < avail.left())
            geo.moveLeft(avail.left());
        popup->setGeometry(geo);
    }

private:
    QSize currentTextSizeHint() const
    {
        const QFontMetrics fm = fontMetrics();
        const QString text = currentText();
        QSize contents(fm.horizontalAdvance(text.isEmpty() ? QStringLiteral("XX") : text),
                       qMax(fm.height(), 14) + 2);
        const QIcon icon = currentIndex() >= 0 ? itemIcon(currentIndex()) : QIcon();
        if (!icon.isNull()) {
            contents.setWidth(contents.width() + iconSize().width() + 4);
            contents.setHeight(qMax(contents.height(), iconSize().height()));
        }
        QStyleOptionComboBox opt;
        initStyleOption(&opt);
        return style()->sizeFromContents(QStyle::CT_ComboBox, &opt, contents, this);
    }

    QString m_hintedText;
};

class AgentChoiceDescriptionDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override
    {
        QSize hint = QStyledItemDelegate::sizeHint(option, index);
        const QString description =
            index.data(kAgentChoiceDescriptionRole).toString();
        if (!description.isEmpty())
            hint.setWidth(hint.width() + kGap +
                          option.fontMetrics.horizontalAdvance(description));
        return hint;
    }

protected:
    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        paintWithStatusGlyph(painter, option, index);
        const QString description =
            index.data(kAgentChoiceDescriptionRole).toString();
        if (description.isEmpty())
            return;
        QStyleOptionViewItem styled(option);
        initStyleOption(&styled, index);
        const QWidget *widget = styled.widget;
        QStyle *style = widget ? widget->style() : QApplication::style();
        QRect textRect =
            style->subElementRect(QStyle::SE_ItemViewItemText, &styled, widget);
        textRect.setLeft(textRect.left() +
                         styled.fontMetrics.horizontalAdvance(styled.text) + kGap);
        if (textRect.width() <= 0)
            return;
        QColor colour = styled.palette.color(QPalette::Text);
        colour.setAlpha(160);
        painter->save();
        painter->setPen(colour);
        painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter,
                          styled.fontMetrics.elidedText(
                              description, Qt::ElideRight, textRect.width()));
        painter->restore();
    }

private:
    void paintWithStatusGlyph(QPainter *painter,
                              const QStyleOptionViewItem &option,
                              const QModelIndex &index) const
    {
        const QString state = index.data(kAgentChoiceStatusRole).toString();
        const QString glyph = agentChoiceStatusGlyph(state);
        QStyleOptionViewItem styled(option);
        initStyleOption(&styled, index);
        if (glyph.isEmpty() || !styled.text.endsWith(glyph)) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }
        const QWidget *widget = styled.widget;
        QStyle *style = widget ? widget->style() : QApplication::style();
        QStyleOptionViewItem plain(styled);
        plain.text.chop(glyph.size()); // the separating space stays put
        style->drawControl(QStyle::CE_ItemViewItem, &plain, painter, widget);
        QRect glyphRect =
            style->subElementRect(QStyle::SE_ItemViewItemText, &styled, widget);
        glyphRect.setLeft(glyphRect.left() +
                          styled.fontMetrics.horizontalAdvance(plain.text));
        if (glyphRect.width() <= 0)
            return;
        painter->save();
        painter->setPen(agentChoiceStatusColour(state));
        painter->drawText(glyphRect, Qt::AlignLeft | Qt::AlignVCenter, glyph);
        painter->restore();
    }

    static constexpr int kGap = 12;
};

// Compact composer controls whose closed state is just the selected icon. The
// popup still uses the normal combo model, so opening it reveals the full icon
// + label rows (the effort ladder). This keeps the prompt chrome quiet without
// making the choices cryptic once clicked.
class IconOnlyFullPopupComboBox : public FullPopupComboBox {
public:
    explicit IconOnlyFullPopupComboBox(QWidget *parent = nullptr)
        : FullPopupComboBox(parent)
    {
        setIconSize(QSize(22, 22));
        setFixedWidth(30);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        setItemDelegate(new AgentChoiceDescriptionDelegate(this));
    }

    QSize sizeHint() const override
    {
        return QSize(30, qMax(26, FullPopupComboBox::sizeHint().height()));
    }

    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QStylePainter painter(this);
        QStyleOptionComboBox option;
        initStyleOption(&option);
        option.currentText.clear();
        option.iconSize = iconSize();
        painter.drawComplexControl(QStyle::CC_ComboBox, option);
        painter.drawControl(QStyle::CE_ComboBoxLabel, option);
    }
};

// The same compact composer control, but the closed state keeps the selected
// option's one word beside the icon. What a permission mode allows is not
// something a glyph can say on its own, so Auto / Ask / Plan / Edit stays
// readable at a glance in small type; the effort ladder next to it is a
// speedometer whose needle already reads as the level, so it stays icon-only.
class IconWordFullPopupComboBox : public FullPopupComboBox {
public:
    explicit IconWordFullPopupComboBox(QWidget *parent = nullptr)
        : FullPopupComboBox(parent)
    {
        setIconSize(QSize(20, 20));
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        setItemDelegate(new AgentChoiceDescriptionDelegate(this));
        // Theme.h's global QWidget{font-size:14px} beats setFont(), so the small
        // word has to come from this widget's own sheet. The selector matches
        // the combo box alone, so the open popup keeps the normal row type.
        setStyleSheet(QStringLiteral("QComboBox { font-size: 11px; }"));
    }

    QSize sizeHint() const override
    {
        QSize hint = FullPopupComboBox::sizeHint();
        hint.setHeight(qMax(26, hint.height()));
        return hint;
    }

    QSize minimumSizeHint() const override { return sizeHint(); }
};

QIcon agentControlIcon(int index);

const QString kClaudeAutoModelId = QStringLiteral("auto");

struct ClaudeAutoRung {
    QString alias;
    QString id;
    QString label;
};

// The rungs "auto" climbs, and the models an "@opus"/"@fable" issue mention
// resolves to (forkmesh::issuemention::modelForAlias keeps an offline copy for
// its unit tests; this ladder is what wins at runtime).
const QList<ClaudeAutoRung> &claudeAutoLadder();

bool agentModelIsClaudeStyle(const QString &model);

bool agentModelIsCloudflareStyle(const QString &model);

bool agentModelMatchesProvider(const QString &provider, const QString &model);

int agentModelPortraitIconIndex(const QString &model);

int agentModelFaceIconIndex(const QString &provider, const QString &model);

QString marketplaceAgentIconKey(const QString &provider,
                                       const QString &model);

QHash<QString, QIcon> &marketplaceAgentIcons();

void clearMarketplaceAgentIcons();

void registerMarketplaceAgentIcon(const QString &provider,
                                         const QString &model,
                                         const QString &path);

QIcon agentModelIcon(const QString &provider, const QString &model);

// The face a session shows everywhere it is drawn: its model's, or the Auto
// route's own.
QIcon agentStatusModelIcon(const AgentSession &session);

struct ClaudeAutoRoute {
    QString model;  // empty = not confident, fall through to LLM triage
    QString reason; // human-readable, shown in the transcript
};

ClaudeAutoRoute claudeAutoHeuristicRoute(const QString &task);

void populateClaudeModelCombo(QComboBox *combo);

QString codexChatGptModelId(const QString &model);

QString codexModelDisplayLabel(const QString &model,
                                      const QString &fallback = QString());

void populateCodexModelCombo(QComboBox *combo);

QVector<QPair<QString, QString>> cloudflareAiFallbackModels();

QString cloudflareAiModelLabel(const QString &model);

void populateCloudflareAiModelCombo(QComboBox *combo);

void mergeLiveCodexModels(QComboBox *combo, const QJsonArray &models);

QString selectedModelComboValue(QComboBox *combo);

void selectModelComboValue(QComboBox *combo, const QString &model);

QString compactModelName(const QString &name);

double agentModelVersionNumber(const QString &text);

int agentModelPowerRank(const QString &model, const QString &label);

const QString kComposerHiddenModelsSetting =
    QStringLiteral("agents/composerHiddenModels");

// Which agent+model the pull request "Fix" button starts, as a
// composerModelKey(). Remembered so the next conflicted PR opens on the same
// choice rather than back at the top of the list.
const QString kPullFixAgentModelSetting =
    QStringLiteral("pulls/fixAgentModel");

// How many agent+model rows the "Fix" combo offers: the ten most-run pairs on
// this node, which is a glance rather than a scroll.
constexpr int kPullFixModelChoices = 10;

QString composerModelKey(const QString &provider, const QString &model);

QSet<QString> hiddenComposerModels();

void saveHiddenComposerModels(const QSet<QString> &hidden);

QString agentModelLabel(const QString &model);

QString agentModelShortLabel(const QString &model);

// Fill an agent-provider model combo for one of the three agent providers
// (; shared by the branch "Fix with agent" bar and the Actions "Fix
// with agent" bar). Item data is the model id passed straight to the caller's
// start function. claude-code combos start with this static fallback so they
// are never blank, then refreshClaudeModelCombo / mergeLiveClaudeModels swaps
// in the live line-up once a claude.ai OAuth token is available (users signed
// in with an API key, or offline, keep the fallback); openai/claude-api combos
// keep static lists too (no live fetch for those).
void fillAgentFixModelCombo(QComboBox *combo, const QString &provider);

void mergeLiveClaudeModels(QComboBox *combo, const QJsonArray &models);

constexpr int kRepoLandingTab = 0; // Code

constexpr int kRepoAgentsTab = 3;

// Live claude.ai OAuth access token the Claude Code CLI stores in
// ~/.claude/.credentials.json. Empty when the user logged in with an API key
// (or isn't signed in). Read fresh each call so a token the CLI has rotated is
// picked up automatically.
QString claudeCodeOAuthToken(const QString &configDir = QString());

// True while Claude Code holds a refresh token it can still redeem. The CLI
// refreshes a lapsed access token itself on its next run, so work that runs
// the CLI (not a direct API call) is still signed in on an idle node.
bool claudeCodeHasRefreshableLogin(const QString &configDir = QString());

// #team runs local CLI teammates.  Do not let a generic desktop login or an
// unconfigured provider profile turn it into a shared-agent inbox: the person
// asking a teammate must have signed in to at least one of their own CLI
// accounts on this device.
bool hasLinkedTeamChatAgentAccount();

// System identity Anthropic requires on /v1/messages when authenticating with a
// claude.ai subscription OAuth token (as the Claude Code CLI does) instead of an
// API key.
const QString kClaudeCodeOAuthSystem =
    QStringLiteral("You are Claude Code, Anthropic's official CLI for Claude.");

QString claudeAgentScriptPath(const QString &directory = QString());

QString defaultClaudeCommand();

QString cloudflareAgentScriptPath(const QString &directory = QString());

QString defaultCloudflareAiCommand();

QString claudeCommandSetting();

QString claudeCodeCommandSetting();

QString agentPromptPreamble();

QString defaultPrioritizePrompt();

QString prioritizePromptSetting();

QString completenessPrompt();

QString codexCommandSetting();

QString runningClientDirUnderHome(const QString &home);

QString runningClientDir();

QString workingClientDir();

// code-health: allow(blocking-wait) GUI-thread read used only by the self-update/rebuild flows (maybeAutoUpdate, the rebuild pass, quickUpdatePullArguments) for local rev-parse/branch/for-each-ref/merge-base on the app's own checkout; capped at 3 s, then killed and reaped for at most 1 s
bool gitOutput(const QString &clientDir, const QStringList &arguments, QString *out);

QStringList quickUpdatePullArguments(const QString &clientDir);

QString invokingNonRootUser();

QString homeForUser(const QString &user);

QString runningClientDirUnderHome(const QString &home);

QString runningClientExecutableUnderHome(const QString &home);

QString runningClientExecutable();

QString shellSingleQuote(const QString &value);

QString builtExecutablePath(const QString &buildDir);

#ifdef Q_OS_MACOS
// code-health: allow(blocking-wait) macOS only; called solely by cmakeConfigureArgs when a self-rebuild or pull preview queues its cmake configure step (twice: qt, openssl@3); each `brew --prefix` is capped at 3 s
inline QString brewPrefix(const QString &formula)
{
    const QString brew = QStandardPaths::findExecutable("brew");
    if (brew.isEmpty())
        return {};

    QProcess process;
    process.start(brew, {"--prefix", formula});
    if (!process.waitForFinished(3000) || process.exitCode() != 0)
        return {};
    return QString::fromUtf8(process.readAllStandardOutput()).trimmed();
}
#endif

// The most heavy compiles a self-rebuild ever runs at once, whatever the host
// offers. Twelve keeps the 22 giant MainWindow units moving on a large box
// without turning every core into a 1.6 GiB compiler.
constexpr int kMaxRebuildJobs = 12;

int buildJobsForMemory(qint64 totalRam, qint64 availableRam,
                              int idealThreads);

int ramCappedBuildJobs();

QString buildScratchDir(const QString &buildDir);

qint64 freeBytesForPath(const QString &path);

qint64 rebuildFreeBytesRequired(const QString &buildDir);

QString updateFailureSummary(const QString &output);

// One value out of a CMakeCache.txt ("KEY:TYPE=value"), or an empty string.
QString cmakeCacheValue(const QString &cacheFile, const QString &key);

// The generator a self-rebuild configures with: Ninja when it is installed,
// otherwise CMake's platform default. Ninja honours the heavy-compile job pool
// (Make ignores pools) and schedules the giant units first, so a high -j is
// safe and the critical path starts immediately.
QString cmakeGeneratorForRebuild();

// Whether an existing build tree can be built as-is: same source directory,
// build type and generator. A matching tree needs no explicit configure step
// (cmake --build re-runs the generator itself when a CMakeLists.txt changed),
// which saves the two-to-five-second reconfigure on every restart.
bool cmakeCacheMatches(const QString &clientDir, const QString &buildDir,
                              const QString &buildType, const QString &generator);

// CMake refuses to switch an existing tree to another generator. The entries a
// generator owns are removed instead of the whole directory: the objects stay
// for ccache and, more importantly, the built binary the running desktop (and
// its systemd unit) point at is left in place until the new link replaces it.
QStringList cmakeGeneratorEntriesToDrop(const QString &buildDir,
                                               const QString &generator);

QStringList cmakeConfigureArgs(const QString &clientDir, const QString &buildDir,
                               const QString &buildType);

struct PullPreviewStep {
    QString program;
    QStringList args;
    QString dir;
    QString status;
};

QList<PullPreviewStep> pullPreviewSteps(const QString &gitDir,
                                        const QString &previewDir,
                                        const QString &clientDir,
                                        const QString &buildDir,
                                        const QString &commit, bool haveWorktree,
                                        int jobs);

const QString kDmPrefix = QStringLiteral("@");

bool isDirectConversation(const QString &conversation);

QString dmKey(const QString &peerId);

QString dmPeerId(const QString &conversation);

QString repoSegment(QString value, const QString &fallback);

QString repoNameFromUrl(QString url);

// Username (and legacy node name): a single DNS-like label — lowercase letters,
// digits and hyphens, starting with a letter and ending with a letter or digit,
// max 63 chars. Users and nodes are being split apart — a user signs up with a
// username and can attach many nodes — but both identifiers share this shape,
// and the wire protocol still calls the account field "nodeName". Mirrors
// valid_node_name in the worker and NAME_RE on the website.
bool isValidNodeName(const QString &value);

QString randomFunNodeName();

QString accountNameFromInput(QString value, const QString &fallback = QStringLiteral("node"));

bool textMentionsNodeName(const QString &text, const QString &nodeName);

QString savedProfileName();

QString formatRepoDate(qint64 timestampMs);

// "How long ago" is a difference of two instants, so it needs no calendar and no
// time zone — but going through QDateTime made every call resolve the local zone,
// and glibc's tzset() stat()s /etc/localtime each time. Table rebuilds call these
// once per row: stalls were sampled inside tzset()/mktime() under both
// formatShortRelativeTime() and refreshActionsTable(). Subtract epochs instead.
// Issue records written by early web clients stored Unix seconds in fields that
// newer clients store as milliseconds.  Keep the signed value untouched and
// normalize only at the display boundary so legacy event signatures continue
// to verify.
qint64 issueDisplayTimestampMs(qint64 timestamp);

QString formatIssueRelativeTime(qint64 timestampMs);

QString formatShortRelativeTime(qint64 timestampSecs);

QString formatInsightBytes(qint64 bytes);

QString insightMetricCell(const QString &label, const QString &value,
                          const QString &detail = QString());

QString insightMetricsTable(const QStringList &cells);

bool resolveThemeIsDark();

int &themeIsDarkCacheSlot();

// MainWindow::applyTheme() is the one funnel for a theme change (the settings
// combo, and the OS colour-scheme signal wired up in main()), so it re-arms the
// cache below.
void invalidateThemeIsDarkCache();

// Every themed paintEvent() asked this, and each answer built a QSettings —
// re-reading and re-parsing the INI. A 502 ms stall was sampled directly inside
// QSettings' constructor under ActivityRailButton::paintEvent. The answer only
// moves when the theme does, so resolve it once per change.
bool currentThemeIsDark();

// The href "(clear search)" carries, so every list's empty-state notice lands
// on the same handler.
inline const QString kClearSearchLink = QStringLiteral("forkmesh:clear-search");

// What a list says once the search box filtered it down to nothing. Repeating
// the term back is what turns "this list is broken" into "this list is
// filtered", and the link is the one click out of the dead end — an emptied
// list leaves nothing else to aim at.
QString searchEmptyNoticeHtml(const QString &query);

QLabel *makeSearchEmptyNotice();

class IssueBurnupChart final : public QWidget
{
public:
    explicit IssueBurnupChart(QWidget *parent = nullptr) : QWidget(parent)
    {
        setMinimumHeight(340);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }

    void setSeries(QList<IssueBurnupPoint> series)
    {
        m_series = std::move(series);
        update();
    }

    QSize sizeHint() const override { return QSize(760, 420); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const bool dark = currentThemeIsDark();
        const QColor text = Theme::remapColor(QColor(dark ? "#e6edf3" : "#1f2328"));
        const QColor muted = Theme::remapColor(QColor(dark ? "#8b949e" : "#656d76"));
        const QColor grid = Theme::remapColor(QColor(dark ? "#30363d" : "#d8dee4"));
        const QColor openColor(dark ? "#58a6ff" : "#0969da");
        const QColor closedColor(dark ? "#3fb950" : "#1a7f37");
        const QRectF plot = QRectF(rect()).adjusted(54, 18, -20, -46);

        if (plot.width() <= 0 || plot.height() <= 0)
            return;
        if (m_series.isEmpty()) {
            painter.setPen(muted);
            painter.drawText(plot, Qt::AlignCenter,
                             QStringLiteral("No issue history in this range"));
            return;
        }

        int maximum = 1;
        for (const IssueBurnupPoint &point : m_series)
            maximum = qMax(maximum, qMax(point.openCount, point.closedCount));
        const int roundedMaximum = qMax(4, ((maximum + 3) / 4) * 4);

        painter.setFont(font());
        for (int i = 0; i <= 4; ++i) {
            const qreal y = plot.bottom() - plot.height() * i / 4.0;
            painter.setPen(QPen(grid, 1));
            painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
            painter.setPen(muted);
            painter.drawText(QRectF(0, y - 10, plot.left() - 8, 20),
                             Qt::AlignRight | Qt::AlignVCenter,
                             QString::number(roundedMaximum * i / 4));
        }

        const qint64 firstTs = m_series.first().timestampMs;
        const qint64 lastTs = m_series.last().timestampMs;
        const qint64 duration = qMax<qint64>(1, lastTs - firstTs);
        auto position = [&](int index, int count) {
            const IssueBurnupPoint &point = m_series.at(index);
            const qreal x = plot.left() +
                            plot.width() * (point.timestampMs - firstTs) / duration;
            const qreal y = plot.bottom() -
                            plot.height() * count / roundedMaximum;
            return QPointF(x, y);
        };

        const QString dateFormat =
            duration <= 2 * 24 * 60 * 60 * 1000LL
                ? QStringLiteral("h AP")
                : (duration <= 14 * 24 * 60 * 60 * 1000LL
                       ? QStringLiteral("ddd")
                       : QStringLiteral("MMM d"));
        for (int tick = 0; tick <= 4; ++tick) {
            const int index = (m_series.size() - 1) * tick / 4;
            const qreal x = position(index, 0).x();
            painter.setPen(muted);
            painter.drawText(
                QRectF(x - 46, plot.bottom() + 10, 92, 24),
                Qt::AlignHCenter | Qt::AlignTop,
                QDateTime::fromMSecsSinceEpoch(m_series.at(index).timestampMs)
                    .toString(dateFormat));
        }

        auto drawSeries = [&](const QColor &color, auto countFor) {
            QPainterPath path;
            for (int i = 0; i < m_series.size(); ++i) {
                const QPointF point = position(i, countFor(m_series.at(i)));
                if (i == 0)
                    path.moveTo(point);
                else
                    path.lineTo(point);
            }
            painter.setPen(QPen(color, 3, Qt::SolidLine, Qt::RoundCap,
                                Qt::RoundJoin));
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(path);
            const QPointF last =
                position(m_series.size() - 1, countFor(m_series.last()));
            painter.setBrush(color);
            painter.setPen(QPen(dark ? Theme::remapColor(QColor("#0d1117")) : QColor("#ffffff"), 2));
            painter.drawEllipse(last, 5, 5);
        };

        drawSeries(openColor,
                   [](const IssueBurnupPoint &point) { return point.openCount; });
        drawSeries(closedColor,
                   [](const IssueBurnupPoint &point) { return point.closedCount; });

        painter.setPen(text);
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(plot);
    }

private:
    QList<IssueBurnupPoint> m_series;
};

using forkmesh::SunburstNode;

class RepoSunburstChart final : public QWidget
{
public:
    explicit RepoSunburstChart(QWidget *parent = nullptr) : QWidget(parent)
    {
        setMinimumSize(360, 400);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setMouseTracking(true);
    }

    void setRoot(SunburstNode root)
    {
        m_root = std::move(root);
        m_trail.clear();
        m_hover = -1;
        update();
    }

    void clear()
    {
        m_basePath.clear();
        setRoot(SunburstNode());
    }

    void setBasePath(const QString &path) { m_basePath = path; }

    QSize sizeHint() const override { return QSize(640, 640); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const bool dark = currentThemeIsDark();
        const QColor surface = Theme::remapColor(QColor(dark ? "#0d1117" : "#ffffff"));
        const QColor text = Theme::remapColor(QColor(dark ? "#e6edf3" : "#1f2328"));
        const QColor muted = Theme::remapColor(QColor(dark ? "#8b949e" : "#656d76"));
        m_segments.clear();

        const SunburstNode *focus = focusNode();
        if (!focus || focus->size <= 0) {
            painter.setPen(muted);
            painter.drawText(rect(), Qt::AlignCenter,
                             QStringLiteral("No files to chart yet"));
            return;
        }

        const Geometry g = geometry_();
        layoutRing(*focus, 1, 90.0, -360.0, m_trail);

        for (int i = 0; i < m_segments.size(); ++i) {
            const Segment &seg = m_segments.at(i);
            const qreal r0 = g.hole + (seg.depth - 1) * g.ringWidth;
            const qreal r1 = r0 + g.ringWidth;
            QPainterPath path;
            const QRectF outer(g.center.x() - r1, g.center.y() - r1, 2 * r1, 2 * r1);
            const QRectF inner(g.center.x() - r0, g.center.y() - r0, 2 * r0, 2 * r0);
            path.arcMoveTo(outer, seg.startDeg);
            path.arcTo(outer, seg.startDeg, seg.spanDeg);
            path.arcTo(inner, seg.startDeg + seg.spanDeg, -seg.spanDeg);
            path.closeSubpath();
            QColor fill = seg.color;
            if (i == m_hover)
                fill = dark ? fill.lighter(125) : fill.darker(112);
            painter.setPen(QPen(surface, 2));
            painter.setBrush(fill);
            painter.drawPath(path);
        }

        painter.setFont(font());
        for (const Segment &seg : m_segments) {
            if (qAbs(seg.spanDeg) < 18.0)
                continue;
            const qreal midDeg = seg.startDeg + seg.spanDeg / 2;
            const qreal midRad = midDeg * M_PI / 180.0;
            const qreal r = g.hole + (seg.depth - 1) * g.ringWidth + g.ringWidth / 2;
            const QPointF at(g.center.x() + r * std::cos(midRad),
                             g.center.y() - r * std::sin(midRad));
            const QString name = painter.fontMetrics().elidedText(
                seg.name, Qt::ElideRight, int(g.ringWidth * 1.8));
            const QRectF box(at.x() - 70, at.y() - 15, 140, 30);
            painter.setPen(QColor(0, 0, 0, 150));
            painter.drawText(box.translated(1, 1), Qt::AlignCenter,
                             name + "\n" + QLocale().formattedDataSize(seg.size));
            painter.setPen(Qt::white);
            painter.drawText(box, Qt::AlignCenter,
                             name + "\n" + QLocale().formattedDataSize(seg.size));
        }

        painter.setPen(text);
        QFont hubFont = font();
        hubFont.setBold(true);
        painter.setFont(hubFont);
        const QRectF hub(g.center.x() - g.hole, g.center.y() - g.hole,
                         2 * g.hole, 2 * g.hole);
        const QString hubName = m_trail.isEmpty()
                                    ? (m_root.name.isEmpty()
                                           ? QStringLiteral("repository")
                                           : m_root.name)
                                    : focus->name;
        painter.drawText(hub.adjusted(6, 0, -6, -14), Qt::AlignCenter,
                         painter.fontMetrics().elidedText(
                             hubName, Qt::ElideMiddle, int(g.hole * 1.7)));
        painter.setFont(font());
        painter.setPen(muted);
        painter.drawText(hub.adjusted(6, 22, -6, 0), Qt::AlignCenter,
                         QLocale().formattedDataSize(focus->size) +
                             (m_trail.isEmpty() ? QString()
                                                : QStringLiteral("\n⌃ up")));
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        const int hit = segmentAt(event->pos());
        if (hit != m_hover) {
            m_hover = hit;
            update();
        }
        if (hit >= 0) {
            const Segment &seg = m_segments.at(hit);
            setCursor(seg.hasChildren ? Qt::PointingHandCursor : Qt::ArrowCursor);
            const qreal pct = m_root.size > 0
                                  ? 100.0 * double(seg.size) / double(m_root.size)
                                  : 0.0;
            QToolTip::showText(
                event->globalPosition().toPoint(),
                QStringLiteral("%1 — %2 · %3% of repo")
                    .arg(seg.path, QLocale().formattedDataSize(seg.size),
                         QString::number(pct, 'f', 1)),
                this);
        } else {
            QToolTip::hideText();
            setCursor(!m_trail.isEmpty() && inHub(event->pos())
                          ? Qt::PointingHandCursor
                          : Qt::ArrowCursor);
        }
        QWidget::mouseMoveEvent(event);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            if (inHub(event->pos())) {
                if (!m_trail.isEmpty()) {
                    m_trail.removeLast();
                    m_hover = -1;
                    update();
                }
                return;
            }
            const int hit = segmentAt(event->pos());
            if (hit >= 0 && m_segments.at(hit).hasChildren) {
                m_trail = m_segments.at(hit).trail;
                m_hover = -1;
                update();
                return;
            }
        }
        QWidget::mousePressEvent(event);
    }

    void leaveEvent(QEvent *event) override
    {
        m_hover = -1;
        update();
        QWidget::leaveEvent(event);
    }

    void contextMenuEvent(QContextMenuEvent *event) override
    {
        if (m_basePath.isEmpty()) {
            QWidget::contextMenuEvent(event);
            return;
        }
        const int hit = segmentAt(event->pos());
        QString rel;
        QString label;
        if (hit >= 0) {
            rel = m_segments.at(hit).path;
            label = m_segments.at(hit).name;
        } else if (inHub(event->pos())) {
            rel = focusRelativePath();
            const SunburstNode *focus = focusNode();
            label = focus && !focus->name.isEmpty() ? focus->name
                                                    : QStringLiteral("repository");
        } else {
            QWidget::contextMenuEvent(event);
            return;
        }
        const QString target =
            rel.isEmpty() ? m_basePath : QDir(m_basePath).filePath(rel);
        const QFileInfo targetInfo(target);
        const QString dir = targetInfo.isDir()
                                ? target
                                : (targetInfo.isFile() ? targetInfo.absolutePath()
                                                       : QString());
        if (dir.isEmpty() || !QDir(dir).exists()) {
            QWidget::contextMenuEvent(event);
            return;
        }
        QMenu menu(this);
        QAction *open = menu.addAction(
            QStringLiteral("Open \"%1\" in file explorer").arg(label));
        connect(open, &QAction::triggered, this,
                [target] { revealInDesktopFileManager(target); });
        menu.exec(event->globalPos());
    }

private:
    static constexpr int kRings = 4;

    struct Segment {
        qreal startDeg = 0; // Qt convention: 0° at 3 o'clock, CCW positive
        qreal spanDeg = 0;  // negative = clockwise on screen
        int depth = 1;      // 1-based ring index from the focus
        qint64 size = 0;
        QString name;
        QString path; // repo-relative, for the tooltip
        QColor color;
        bool hasChildren = false;
        QList<int> trail; // child-index chain from the root to this node
    };

    struct Geometry {
        QPointF center;
        qreal hole = 0;
        qreal ringWidth = 0;
    };

    Geometry geometry_() const
    {
        Geometry g;
        g.center = QPointF(width() / 2.0, height() / 2.0);
        const qreal radius = qMax<qreal>(60.0, qMin(width(), height()) / 2.0 - 8);
        g.hole = qMax<qreal>(34.0, radius * 0.24);
        g.ringWidth = (radius - g.hole) / kRings;
        return g;
    }

    const SunburstNode *focusNode() const
    {
        const SunburstNode *node = &m_root;
        for (int index : m_trail) {
            if (index < 0 || index >= node->children.size())
                return &m_root;
            node = &node->children.at(index);
        }
        return node;
    }

    QString focusRelativePath() const
    {
        QStringList names;
        const SunburstNode *node = &m_root;
        for (int index : m_trail) {
            if (index < 0 || index >= node->children.size())
                return QString();
            node = &node->children.at(index);
            names.append(node->name);
        }
        return names.join(QLatin1Char('/'));
    }

    QColor slotColor(int index, bool dark) const
    {
        static const char *kDark[] = {"#3987e5", "#199e70", "#c98500",
                                      "#008300", "#9085e9", "#e66767",
                                      "#d55181", "#d95926"};
        static const char *kLight[] = {"#2a78d6", "#1baf7a", "#eda100",
                                       "#008300", "#4a3aa7", "#e34948",
                                       "#e87ba4", "#eb6834"};
        if (index < 0 || index >= 8)
            return QColor("#8a8a8a");
        return QColor(dark ? kDark[index] : kLight[index]);
    }

    static QColor shaded(const QColor &base, int depth, bool dark, int index)
    {
        const qreal f = qMin(0.55, (depth - 1) * 0.16 +
                                       (depth > 1 ? (index % 2) * 0.06 : 0.0));
        const int toward = dark ? 255 : 0;
        return QColor(int(base.red() + (toward - base.red()) * f),
                      int(base.green() + (toward - base.green()) * f),
                      int(base.blue() + (toward - base.blue()) * f));
    }

    QColor colorForTrail(const QList<int> &trail, bool dark) const
    {
        if (trail.isEmpty())
            return QColor("#8a8a8a");
        return shaded(slotColor(trail.first(), dark), trail.size(), dark,
                      trail.last());
    }

    void layoutRing(const SunburstNode &node, int depth, qreal startDeg,
                    qreal spanDeg, const QList<int> &trail)
    {
        if (depth > kRings || node.size <= 0)
            return;
        const bool dark = currentThemeIsDark();
        qreal at = startDeg;
        for (int i = 0; i < node.children.size(); ++i) {
            const SunburstNode &child = node.children.at(i);
            if (child.size <= 0)
                continue;
            const qreal childSpan =
                spanDeg * qMin<qreal>(1.0, double(child.size) / double(node.size));
            if (qAbs(childSpan) >= 0.4) {
                QList<int> childTrail = trail;
                childTrail.append(i);
                QStringList names;
                const SunburstNode *walk = &m_root;
                for (int index : childTrail) {
                    walk = &walk->children.at(index);
                    names.append(walk->name);
                }
                Segment seg;
                seg.startDeg = at;
                seg.spanDeg = childSpan;
                seg.depth = depth;
                seg.size = child.size;
                seg.name = child.name;
                seg.path = names.join(QLatin1Char('/'));
                seg.color = colorForTrail(childTrail, dark);
                seg.hasChildren = !child.children.isEmpty();
                seg.trail = childTrail;
                m_segments.append(seg);
                layoutRing(child, depth + 1, at, childSpan, childTrail);
            }
            at += childSpan;
        }
    }

    bool inHub(const QPoint &pos) const
    {
        const Geometry g = geometry_();
        return QLineF(g.center, pos).length() <= g.hole;
    }

    int segmentAt(const QPoint &pos) const
    {
        const Geometry g = geometry_();
        const qreal r = QLineF(g.center, pos).length();
        if (r <= g.hole)
            return -1;
        qreal angle = std::atan2(g.center.y() - pos.y(), pos.x() - g.center.x())
                      * 180.0 / M_PI;
        for (int i = 0; i < m_segments.size(); ++i) {
            const Segment &seg = m_segments.at(i);
            const qreal r0 = g.hole + (seg.depth - 1) * g.ringWidth;
            const qreal r1 = r0 + g.ringWidth;
            if (r < r0 || r > r1)
                continue;
            qreal delta = std::fmod(seg.startDeg - angle, 360.0);
            if (delta < 0)
                delta += 360.0;
            if (delta <= qAbs(seg.spanDeg))
                return i;
        }
        return -1;
    }

    SunburstNode m_root;
    QString m_basePath; // absolute working-tree path, for "open in file explorer"
    QList<int> m_trail; // child-index chain from the root to the focus
    int m_hover = -1;
    mutable QVector<Segment> m_segments; // rebuilt each paint (geometry-dependent)
};

// One mounted filesystem beside the big size map: a small
// used/free donut plus the mount point, already drawn so the whole set reads
// as a column of tiny maps. Clicking one re-roots the full scan on that mount,
// which is how "/" (or any other filesystem) gets expanded without the folder
// picker. Header-only with a plain callback, like the other Internal.h mini
// widgets — no Q_OBJECT, so no moc entry is needed.
class StorageMiniMap final : public QWidget
{
public:
    explicit StorageMiniMap(const QStorageInfo &volume, QWidget *parent = nullptr)
        : QWidget(parent), m_mountPoint(volume.rootPath()),
          m_device(QString::fromUtf8(volume.device())),
          m_type(QString::fromUtf8(volume.fileSystemType())),
          m_total(qMax<qint64>(0, volume.bytesTotal())),
          m_used(qMax<qint64>(0, volume.bytesTotal() - volume.bytesAvailable()))
    {
        setCursor(Qt::PointingHandCursor);
        setMouseTracking(true);
        setMinimumHeight(56);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setToolTip(QStringLiteral("%1\n%2 · %3\n%4 used of %5")
                       .arg(QDir::toNativeSeparators(m_mountPoint), m_device,
                            m_type, QLocale().formattedDataSize(m_used),
                            QLocale().formattedDataSize(m_total)));
    }

    QString mountPoint() const { return m_mountPoint; }

    void setOnClicked(std::function<void()> callback)
    {
        m_onClicked = std::move(callback);
    }

    void setSelected(bool selected)
    {
        if (m_selected == selected)
            return;
        m_selected = selected;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const bool dark = currentThemeIsDark();
        const QColor text = Theme::remapColor(QColor(dark ? "#e6edf3" : "#1f2328"));
        const QColor muted = Theme::remapColor(QColor(dark ? "#8b949e" : "#656d76"));
        const QColor track = Theme::remapColor(QColor(dark ? "#30363d" : "#d0d7de"));
        const QColor used(dark ? "#3987e5" : "#2a78d6");
        const QColor accent(dark ? "#58a6ff" : "#0969da");

        if (m_selected || m_hover) {
            painter.setPen(m_selected ? QPen(accent, 1) : Qt::NoPen);
            painter.setBrush(Theme::remapColor(QColor(dark ? "#161b22" : "#f6f8fa")));
            painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5),
                                    6, 6);
        }

        const qreal ring = 6.0;
        const qreal diameter = qMin<qreal>(38.0, height() - 12);
        const QRectF donut(8 + ring / 2, (height() - diameter) / 2.0 + ring / 2,
                           diameter - ring, diameter - ring);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(track, ring, Qt::SolidLine, Qt::FlatCap));
        painter.drawEllipse(donut);
        const qreal fraction =
            m_total > 0 ? qBound<qreal>(0.0, double(m_used) / double(m_total), 1.0)
                        : 0.0;
        if (fraction > 0) {
            painter.setPen(QPen(used, ring, Qt::SolidLine, Qt::FlatCap));
            painter.drawArc(donut, 90 * 16, int(-fraction * 360 * 16));
        }

        QFont pct = font();
        pct.setPointSizeF(qMax<qreal>(7.0, font().pointSizeF() - 2.5));
        painter.setFont(pct);
        painter.setPen(muted);
        painter.drawText(donut.adjusted(-ring, -ring, ring, ring), Qt::AlignCenter,
                         QStringLiteral("%1%").arg(qRound(fraction * 100)));

        const int textLeft = int(8 + diameter + 10);
        const QRect textArea(textLeft, 6, width() - textLeft - 8, height() - 12);
        QFont title = font();
        title.setBold(true);
        painter.setFont(title);
        painter.setPen(m_selected ? accent : text);
        const QRect titleRect(textArea.left(), textArea.top(), textArea.width(),
                              textArea.height() / 2);
        painter.drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter,
                         painter.fontMetrics().elidedText(
                             QDir::toNativeSeparators(m_mountPoint),
                             Qt::ElideMiddle, titleRect.width()));
        painter.setFont(font());
        painter.setPen(muted);
        const QRect subRect(textArea.left(), textArea.center().y(),
                            textArea.width(), textArea.height() / 2);
        painter.drawText(subRect, Qt::AlignLeft | Qt::AlignVCenter,
                         painter.fontMetrics().elidedText(
                             QStringLiteral("%1 of %2 · %3")
                                 .arg(QLocale().formattedDataSize(m_used),
                                      QLocale().formattedDataSize(m_total), m_type),
                             Qt::ElideRight, subRect.width()));
    }

    void enterEvent(QEnterEvent *event) override
    {
        m_hover = true;
        update();
        QWidget::enterEvent(event);
    }

    void leaveEvent(QEvent *event) override
    {
        m_hover = false;
        update();
        QWidget::leaveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton || !rect().contains(event->pos()) ||
            !m_onClicked) {
            QWidget::mouseReleaseEvent(event);
            return;
        }
        const std::function<void()> callback = m_onClicked;
        event->accept();
        callback();
    }

private:
    QString m_mountPoint;
    QString m_device;
    QString m_type;
    qint64 m_total = 0;
    qint64 m_used = 0;
    bool m_hover = false;
    bool m_selected = false;
    std::function<void()> m_onClicked;
};

QString formatDuration(qint64 ms);

QString compactAddress(QString address);

QStringList splitIssueFieldList(const QString &text);

qreal iconDevicePixelRatio();

QPixmap crispIconPixmap(int w, int h, qreal dpr);

QPixmap crispIconPixmap(int size, qreal dpr);

QPixmap refreshPixmap(const QColor &color, double angleDeg, int size);

QPixmap hourglassPixmap(const QColor &color, double angleDeg, int size);

QPixmap restartProgressPixmap(const QColor &color, double angleDeg, int size,
                                     int percent, bool hourglass);

QByteArray forkMeshAvatarPng(const QString &seed);

QByteArray forkMeshNodeAvatarPng(const QString &seed);

QPixmap roundedAvatar(const QPixmap &src, int side,
                             qreal radiusRatio = 0.28);

QPixmap roundedAvatar(const QByteArray &png, int side,
                             qreal radiusRatio = 0.28);

// A teammate's round portrait. Named avatars have their own artwork; the rest
// fall back to the face of the model the teammate runs on. Shared by the
// Teammates panel and by the agent list, which badges every row a teammate
// started with the same portrait.
QPixmap teammateAvatarPixmap(const QString &avatarKey,
                                    const QString &provider,
                                    const QString &model, int side);

QPixmap teammateFacePixmap(const QString &avatarKey, const QString &provider,
                           const QString &model, int side);

QPixmap nodeMachineFavicon(const QString &seed, int side = 36);

QIcon osBadgeIcon(const QString &platform, bool online, int size);

QString defaultDisplayName(const ForkMeshIdentity &identity);

void saveProfileName(const QString &name);

QIcon statusDotIcon(bool online);

enum class PreviewSyntax {
    Plain,
    Markdown,
    Json,
    Cpp,
    JavaScript,
    Python,
    Yaml,
    Diff
};

PreviewSyntax previewSyntaxForPath(const QString &path);

QTextCharFormat previewFormat(const QColor &color, int weight = QFont::Normal,
                              bool italic = false);

struct HighlightRule {
    QRegularExpression pattern;
    QTextCharFormat format;
};

class CodePreviewHighlighter : public QSyntaxHighlighter
{
public:
    CodePreviewHighlighter(QTextDocument *document, const QString &path)
        : QSyntaxHighlighter(document), m_syntax(previewSyntaxForPath(path))
    {
        configureRules();
    }

protected:
    void highlightBlock(const QString &text) override
    {
        if (m_syntax == PreviewSyntax::Diff) {
            if (text.startsWith("+++ ") || text.startsWith("--- "))
                setFormat(0, text.length(), m_keywordFormat);
            else if (text.startsWith("+"))
                setFormat(0, text.length(), m_addedFormat);
            else if (text.startsWith("-"))
                setFormat(0, text.length(), m_removedFormat);
            else if (text.startsWith("@@"))
                setFormat(0, text.length(), m_headingFormat);
        }

        if (m_syntax == PreviewSyntax::Markdown) {
            if (text.startsWith("#")) {
                const auto match =
                    QRegularExpression(QStringLiteral("^#{1,6}\\s+.*$")).match(text);
                if (match.hasMatch())
                    setFormat(match.capturedStart(), match.capturedLength(),
                              m_headingFormat);
            }
            const auto quoteMatch =
                QRegularExpression(QStringLiteral("^\\s*>.*$")).match(text);
            if (quoteMatch.hasMatch())
                setFormat(quoteMatch.capturedStart(), quoteMatch.capturedLength(),
                          m_commentFormat);
        }

        for (const HighlightRule &rule : std::as_const(m_rules)) {
            auto matches = rule.pattern.globalMatch(text);
            while (matches.hasNext()) {
                const auto match = matches.next();
                setFormat(match.capturedStart(), match.capturedLength(), rule.format);
            }
        }
    }

private:
    void addKeywords(const QStringList &keywords, const QTextCharFormat &format)
    {
        m_rules.push_back({
            QRegularExpression(QStringLiteral("\\b(%1)\\b").arg(keywords.join('|'))),
            format});
    }

    void configureRules()
    {
        const bool dark = currentThemeIsDark();
        m_headingFormat =
            previewFormat(QColor(dark ? "#7ee787" : "#1a7f37"), QFont::Bold);
        m_keywordFormat =
            previewFormat(QColor(dark ? "#ff7b72" : "#cf222e"), QFont::Bold);
        m_stringFormat = previewFormat(QColor(dark ? "#a5d6ff" : "#0a3069"));
        m_numberFormat = previewFormat(QColor(dark ? "#79c0ff" : "#0550ae"));
        m_commentFormat =
            previewFormat(Theme::remapColor(QColor(dark ? "#8b949e" : "#6e7781")), QFont::Normal, true);
        m_keyFormat =
            previewFormat(QColor(dark ? "#d2a8ff" : "#8250df"), QFont::Bold);
        m_addedFormat = previewFormat(QColor(dark ? "#7ee787" : "#1a7f37"));
        m_removedFormat = previewFormat(QColor(dark ? "#ffa198" : "#cf222e"));
        const QTextCharFormat punctuationFormat =
            previewFormat(Theme::remapColor(QColor(dark ? "#8b949e" : "#6e7781")));

        if (m_syntax == PreviewSyntax::Markdown) {
            m_rules.push_back({QRegularExpression(QStringLiteral("`[^`]+`")),
                               m_stringFormat});
            m_rules.push_back({QRegularExpression(QStringLiteral("\\*\\*[^*]+\\*\\*")),
                               m_keywordFormat});
            m_rules.push_back({QRegularExpression(QStringLiteral("\\[[^\\]]+\\]\\([^\\)]+\\)")),
                               m_keyFormat});
            m_rules.push_back({QRegularExpression(QStringLiteral("^\\s*[-*+]\\s+")),
                               m_keywordFormat});
            m_rules.push_back({QRegularExpression(QStringLiteral("^\\s*```.*$")),
                               m_commentFormat});
            return;
        }

        if (m_syntax == PreviewSyntax::Json) {
            m_rules.push_back({QRegularExpression(QStringLiteral("\"([^\"\\\\]|\\\\.)*\"")),
                               m_stringFormat});
            m_rules.push_back({QRegularExpression(QStringLiteral("\"([^\"\\\\]|\\\\.)+\"(?=\\s*:)")),
                               m_keyFormat});
            m_rules.push_back({QRegularExpression(QStringLiteral("\\b-?(0|[1-9]\\d*)(\\.\\d+)?([eE][+-]?\\d+)?\\b")),
                               m_numberFormat});
            addKeywords({"true", "false", "null"}, m_keywordFormat);
            m_rules.push_back({QRegularExpression(QStringLiteral("[{}\\[\\],:]")),
                               punctuationFormat});
            return;
        }

        if (m_syntax == PreviewSyntax::Yaml) {
            m_rules.push_back({QRegularExpression(QStringLiteral("#[^\\n]*")),
                               m_commentFormat});
            m_rules.push_back({QRegularExpression(QStringLiteral("\"([^\"\\\\]|\\\\.)*\"|'[^']*'")),
                               m_stringFormat});
            m_rules.push_back({QRegularExpression(QStringLiteral("^\\s*-?\\s*[A-Za-z0-9_.-]+(?=:)")),
                               m_keyFormat});
            m_rules.push_back({QRegularExpression(QStringLiteral("\\b[0-9]+(\\.[0-9]+)?\\b")),
                               m_numberFormat});
            addKeywords({"true", "false", "yes", "no", "null"}, m_keywordFormat);
            return;
        }

        if (m_syntax == PreviewSyntax::Python) {
            addKeywords({"and", "as", "assert", "async", "await", "break", "class",
                         "continue", "def", "elif", "else", "except", "False", "finally",
                         "for", "from", "if", "import", "in", "is", "lambda", "None",
                         "not", "or", "pass", "raise", "return", "True", "try", "while",
                         "with", "yield"},
                        m_keywordFormat);
            m_rules.push_back({QRegularExpression(QStringLiteral("#[^\\n]*")),
                               m_commentFormat});
        } else if (m_syntax == PreviewSyntax::JavaScript) {
            addKeywords({"async", "await", "break", "case", "catch", "class", "const",
                         "continue", "default", "else", "export", "extends", "false",
                         "for", "from", "function", "if", "import", "let", "new", "null",
                         "return", "switch", "this", "throw", "true", "try", "typeof",
                         "undefined", "var", "while"},
                        m_keywordFormat);
            m_rules.push_back({QRegularExpression(QStringLiteral("//[^\\n]*")),
                               m_commentFormat});
        } else if (m_syntax == PreviewSyntax::Cpp) {
            addKeywords({"auto", "bool", "break", "case", "class", "const", "constexpr",
                         "continue", "else", "enum", "false", "for", "if", "namespace",
                         "nullptr", "private", "protected", "public", "return", "static",
                         "struct", "switch", "template", "true", "typename", "using",
                         "void", "while"},
                        m_keywordFormat);
            m_rules.push_back({QRegularExpression(QStringLiteral("^\\s*#[^\\n]*")),
                               m_commentFormat});
            m_rules.push_back({QRegularExpression(QStringLiteral("//[^\\n]*")),
                               m_commentFormat});
        }

        if (m_syntax == PreviewSyntax::Cpp || m_syntax == PreviewSyntax::JavaScript ||
            m_syntax == PreviewSyntax::Python) {
            m_rules.push_back({QRegularExpression(QStringLiteral("\"([^\"\\\\]|\\\\.)*\"|'([^'\\\\]|\\\\.)*'")),
                               m_stringFormat});
            m_rules.push_back({QRegularExpression(QStringLiteral("\\b[0-9]+(\\.[0-9]+)?\\b")),
                               m_numberFormat});
        }
    }

    PreviewSyntax m_syntax = PreviewSyntax::Plain;
    QVector<HighlightRule> m_rules;
    QTextCharFormat m_headingFormat;
    QTextCharFormat m_keywordFormat;
    QTextCharFormat m_stringFormat;
    QTextCharFormat m_numberFormat;
    QTextCharFormat m_commentFormat;
    QTextCharFormat m_keyFormat;
    QTextCharFormat m_addedFormat;
    QTextCharFormat m_removedFormat;
};

void applyLogFont(QPlainTextEdit *view);

QString displaySafePlainLog(QString text);

class AgentLogHighlighter : public QSyntaxHighlighter
{
public:
    explicit AgentLogHighlighter(QTextDocument *document)
        : QSyntaxHighlighter(document)
    {
        const bool dark = currentThemeIsDark();
        auto fmt = [](const QColor &c, bool bold = false, bool italic = false) {
            QTextCharFormat f;
            f.setForeground(c);
            if (bold)
                f.setFontWeight(QFont::Bold);
            f.setFontItalic(italic);
            return f;
        };
        m_net = fmt(QColor(dark ? "#d2a8ff" : "#8250df"), true);     // network traffic
        m_system = fmt(QColor(dark ? "#58a6ff" : "#0969da"), true);  // ==> markers
        m_success = fmt(QColor(dark ? "#3fb950" : "#1a7f37"), true); // success
        m_error = fmt(QColor(dark ? "#ff7b72" : "#cf222e"), true);   // !! errors
        m_command = fmt(QColor(dark ? "#79c0ff" : "#0550ae"), true); // $ shell command
        m_muted = fmt(Theme::remapColor(QColor(dark ? "#8b949e" : "#6e7781")), false, true); // tool output
        m_tool = fmt(QColor(dark ? "#e3b341" : "#9a6700"), true);    // tool-use headers
    }

protected:
    void highlightBlock(const QString &text) override
    {
        if (text.size() > 8192)
            return;
        const QString trimmed = text.trimmed();
        const int len = text.length();
        if (trimmed.startsWith(QLatin1String("==> [net]")) ||
            trimmed.startsWith(QLatin1String("[net]"))) {
            setFormat(0, len, m_net);
        } else if (trimmed.startsWith(QLatin1String("==>"))) {
            if (trimmed.contains(QLatin1String("SUCCESS")) ||
                trimmed.contains(QLatin1String("Agent finished")) ||
                trimmed.contains(QLatin1String("Created pull request")))
                setFormat(0, len, m_success);
            else if (trimmed.contains(QLatin1String("FAILED")))
                setFormat(0, len, m_error);
            else
                setFormat(0, len, m_system);
        } else if (trimmed.startsWith(QLatin1String("!!")) ||
                   trimmed.contains(QLatin1String("Traceback"))) {
            setFormat(0, len, m_error);
        } else if (trimmed.startsWith(QLatin1String("$ "))) {
            setFormat(0, len, m_command);
        } else if (trimmed.startsWith(QLatin1String("(exit code")) ||
                   trimmed.startsWith(QLatin1String("...[output"))) {
            setFormat(0, len, m_muted);
        } else if (trimmed.startsWith(QString::fromUtf8("\xE2\x97\x8F ")) ||  // ●
                   trimmed.startsWith(QString::fromUtf8("\xE2\x8F\xBA"))) {   // ⏺
            setFormat(0, len, m_tool);
        }
    }

private:
    QTextCharFormat m_net, m_system, m_success, m_error, m_command, m_muted, m_tool;
};

QPixmap tintedOcticonPixmap(const QString &name, const QColor &color,
                                   int size);

class LogActivityLights : public QWidget
{
public:
    enum Presentation {
        Compact,
        Header,
        Debug,
    };

    struct WebsiteStatus {
        QString id;
        QString label;
        QString status;
        QString reason;
        qint64 minuteTs = 0;
        bool local = false;
        QString localStatus;
        QString localReason;
        qint64 localCheckedTs = 0;
        bool checking = false;
    };

    explicit LogActivityLights(Presentation presentation = Compact,
                               QWidget *parent = nullptr)
        : QWidget(parent), m_presentation(presentation)
    {
        setObjectName(presentation == Compact
                          ? QStringLiteral("logActivityLights")
                          : presentation == Header
                                ? QStringLiteral("logActivityHeader")
                                : QStringLiteral("debugActivityLights"));
        if (presentation == Compact) {
            updateCompactSize();
        } else if (presentation == Header) {
            setFixedHeight(kHeaderHeight);
            setMinimumWidth(0);
            setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        } else {
            updateDebugSize();
        }
        setCursor(Qt::PointingHandCursor);
        setMouseTracking(true);
        setAccessibleName(
            presentation == Compact
                ? QStringLiteral("Live log activity by category")
                : presentation == Header
                      ? QStringLiteral("Live log category counts and website status")
                      : QStringLiteral("Debug activity counts and website status"));
        updateSummaryToolTip();
    }

    void pulse(const QString &badge)
    {
        const int lane = categoryIndex(badge);
        if (lane < 0)
            return;
        const QString key = categories().at(lane).badge;
        const quint64 previous = m_counts[key]++;
        const int generation = ++m_generation[key];
        m_blinking[key] = previous > 0;
        m_blinkVisible[key] = previous == 0;
        update();
        if (previous > 0) {
            for (int phase = 1; phase <= 4; ++phase) {
                QTimer::singleShot(phase * 110, this,
                                   [this, key, generation, phase] {
                    if (m_generation.value(key) != generation)
                        return;
                    m_blinkVisible[key] = (phase % 2) != 0;
                    if (phase == 4) {
                        m_blinkVisible[key] = true;
                        m_blinking[key] = false;
                    }
                    update();
                });
            }
        }
        updateSummaryToolTip();
    }

    void setExpanded(bool expanded)
    {
        m_expanded = expanded;
        updateSummaryToolTip();
    }

    void reset()
    {
        m_counts.clear();
        m_blinking.clear();
        m_blinkVisible.clear();
        for (const Category &category : categories())
            ++m_generation[category.badge];
        updateSummaryToolTip();
        update();
    }

    void resetCategory(const QString &badge)
    {
        const int lane = categoryIndex(badge);
        if (lane < 0)
            return;
        const QString key = categories().at(lane).badge;
        m_counts.remove(key);
        m_blinking.remove(key);
        m_blinkVisible.remove(key);
        ++m_generation[key];
        updateSummaryToolTip();
        update();
    }

    void setWebsiteStatuses(const QList<WebsiteStatus> &statuses)
    {
        m_websiteStatuses = statuses;
        noteWebsiteCheckCycles();
        if (m_presentation == Compact)
            updateCompactSize();
        else if (m_presentation == Debug)
            updateDebugSize();
        updateSummaryToolTip();
        updateWebsiteAnimation();
        update();
    }

    void setStallToolTip(const QString &toolTip)
    {
        m_stallToolTip = toolTip;
    }

    int lightCount() const { return categoryCount(); }
    int visibleLightCount() const { return displayLanes().size(); }

    void setHiddenCategories(const QSet<QString> &hidden)
    {
        if (m_presentation != Debug || hidden == m_hiddenCategories)
            return;
        m_hiddenCategories = hidden;
        updateDebugSize();
        updateGeometry();
        update();
    }

    static bool registerCategory(const QString &badge, const QString &accent,
                                 const QString &icon)
    {
        const QString normalized = badge.trimmed().toUpper();
        if (normalized.isEmpty() || accent.trimmed().isEmpty() ||
            icon.trimmed().isEmpty())
            return false;
        for (const Category &category : mutableCategories())
            if (category.badge == normalized)
                return false;
        mutableCategories().append({normalized, accent, icon});
        categoryLanes() = buildCategoryLanes();
        return true;
    }

    static void unregisterCategory(const QString &badge)
    {
        const QString normalized = badge.trimmed().toUpper();
        auto &values = mutableCategories();
        for (int i = builtinCategoryCount(); i < values.size(); ++i) {
            if (values.at(i).badge == normalized) {
                values.removeAt(i);
                categoryLanes() = buildCategoryLanes();
                return;
            }
        }
    }

    void categoryRegistryChanged()
    {
        if (m_presentation == Compact)
            updateCompactSize();
        else if (m_presentation == Debug)
            updateDebugSize();
        updateSummaryToolTip();
        updateGeometry();
        update();
    }

    static QStringList badges()
    {
        QStringList names;
        names.reserve(categoryCount());
        for (int i = 0; i < categoryCount(); ++i)
            names << categories().at(i).badge;
        return names;
    }

    static QString iconForBadge(const QString &badge)
    {
        const int lane = laneForBadge(badge);
        return lane >= 0 ? categories().at(lane).icon : QStringLiteral("info");
    }

    // The one place a category's colour is written down: the log's badge
    // column, its filter chips and these lights all read it from here.
    static QString accentForBadge(const QString &badge)
    {
        const int lane = laneForBadge(badge);
        return lane >= 0 ? categories().at(lane).accent
                         : QStringLiteral("#8b949e");
    }

    quint64 countFor(const QString &badge) const
    {
        const int lane = categoryIndex(badge);
        return lane >= 0 ? m_counts.value(categories().at(lane).badge) : 0;
    }
    bool isActive(const QString &badge) const { return countFor(badge) > 0; }
    bool isBlinking(const QString &badge) const
    {
        const int lane = categoryIndex(badge);
        return lane >= 0 && m_blinking.value(categories().at(lane).badge);
    }
    int websiteStatusCount() const { return m_websiteStatuses.size(); }
    QString websiteStatusFor(const QString &id) const
    {
        for (const WebsiteStatus &status : m_websiteStatuses) {
            if (status.id == id)
                return status.status;
        }
        return QString();
    }
    QString websiteLocalStatusFor(const QString &id) const
    {
        for (const WebsiteStatus &status : m_websiteStatuses) {
            if (status.id == id)
                return status.localStatus;
        }
        return QString();
    }
    bool isHeader() const { return m_presentation == Header; }
    bool isDebug() const { return m_presentation == Debug; }

    std::function<void(const QString &category)> onCategoryClicked;
    std::function<void(const QString &statusId)> onWebsiteClicked;
    std::function<void()> onStallClicked;
    std::function<void()> onStallContextMenu;
    std::function<void()> onClicked;

protected:
    void paintEvent(QPaintEvent *) override
    {
        const QVector<int> lanes = displayLanes();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const bool dark = currentThemeIsDark();
        if (m_presentation == Compact) {
            painter.setPen(QPen(Theme::remapColor(QColor(dark ? "#30363d" : "#d0d7de")), 1));
            painter.setBrush(Theme::remapColor(QColor(dark ? "#161b22" : "#ffffff")));
            painter.drawRoundedRect(rect().adjusted(0, 0, -1, -1), 8, 8);
        } else if (m_presentation == Header) {
            painter.fillRect(rect(), Theme::remapColor(QColor(dark ? "#0d1117" : "#f6f8fa")));
            painter.setPen(Theme::remapColor(QColor(dark ? "#30363d" : "#d0d7de")));
            painter.drawLine(0, height() - 1, width(), height() - 1);
        }

        const QColor unseen = Theme::remapColor(QColor(dark ? "#6e7681" : "#8c959f"));
        const QColor blinkOff = Theme::remapColor(QColor(dark ? "#30363d" : "#d0d7de"));
        for (int i = 0; i < lanes.size(); ++i) {
            const int lane = lanes.at(i);
            const Category &category = categories().at(lane);
            const quint64 laneCount = m_counts.value(category.badge);
            QColor color = unseen;
            if (laneCount > 0)
                color = m_blinkVisible.value(category.badge)
                            ? QColor(category.accent)
                            : blinkOff;
            const QRect iconRect = categoryIconRect(i);
            const int iconSize = qMax(7, qMin(iconRect.width(), iconRect.height()));
            const QPixmap pixmap = tintedOcticonPixmap(
                category.icon, color, iconSize);
            painter.drawPixmap(iconRect.center().x() - iconSize / 2,
                               iconRect.center().y() - iconSize / 2, pixmap);

            if (m_presentation == Header) {
                QFont countFont = painter.font();
                countFont.setPixelSize(qMax(6, qMin(9, categorySlotWidth() - 2)));
                countFont.setBold(laneCount > 0);
                painter.setFont(countFont);
                painter.setPen(laneCount > 0 ? color : unseen);
                painter.drawText(categoryCountRect(i), Qt::AlignHCenter | Qt::AlignTop,
                                 QString::number(laneCount));
            } else if (m_presentation == Debug) {
                QFont countFont = painter.font();
                countFont.setPixelSize(6);
                countFont.setBold(laneCount > 0);
                painter.setFont(countFont);
                painter.setPen(laneCount > 0 ? color : unseen);
                const QString count = laneCount > 999
                                          ? QStringLiteral("999+")
                                          : QString::number(laneCount);
                painter.drawText(categoryCountRect(i),
                                 Qt::AlignLeft | Qt::AlignTop, count);

                QFont labelFont = painter.font();
                labelFont.setPixelSize(6);
                labelFont.setBold(false);
                painter.setFont(labelFont);
                painter.setPen(unseen);
                painter.drawText(categoryLabelRect(i),
                                 Qt::AlignHCenter | Qt::AlignVCenter,
                                 category.badge);
            }
        }

        if (!m_websiteStatuses.isEmpty()) {
            const int separatorX = websiteSeparatorX();
            painter.setPen(QPen(Theme::remapColor(QColor(dark ? "#484f58" : "#afb8c1")), 1));
            painter.drawLine(separatorX, 5, separatorX, height() - 6);
            painter.setPen(Qt::NoPen);
            for (int i = 0; i < m_websiteStatuses.size(); ++i) {
                const QRect dotRect = websiteStatusRect(i);
                const QColor verdict = websiteStatusColor(
                    m_websiteStatuses.at(i).status, dark);
                const bool checking = websiteChecking(i);
                const QColor color =
                    checking && !m_websiteBlinkOn ? blinkOff : verdict;
                painter.setBrush(color);
                if (m_presentation == Debug &&
                    i + 1 < m_websiteStatuses.size()) {
                    QPainterPath dot;
                    dot.addEllipse(QRectF(dotRect));
                    QPainterPath cover;
                    cover.addEllipse(QRectF(websiteStatusRect(i + 1))
                                         .adjusted(-kDebugWebsiteRing,
                                                   -kDebugWebsiteRing,
                                                   kDebugWebsiteRing,
                                                   kDebugWebsiteRing));
                    painter.drawPath(dot.subtracted(cover));
                } else {
                    painter.drawEllipse(dotRect);
                }
            }
        }
    }

    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::ToolTip) {
            auto *help = static_cast<QHelpEvent *>(event);
            const int lane = categoryAt(help->pos());
            if (lane >= 0) {
                const quint64 count =
                    m_counts.value(categories().at(lane).badge);
                QString tip = QStringLiteral("%1 — %2 occurrence%3")
                                  .arg(categories().at(lane).badge)
                                  .arg(count)
                                  .arg(count == 1 ? QString() : QStringLiteral("s"));
                if (lane == stallCategoryIndex() && !m_stallToolTip.isEmpty())
                    tip += QLatin1Char('\n') + m_stallToolTip;
                else if (m_presentation != Header)
                    tip += QStringLiteral("\nClick for full Log page filtered to this "
                                          "category");
                QToolTip::showText(help->globalPos(), tip, this);
                return true;
            }
            const int website = websiteStatusAt(help->pos());
            if (website >= 0) {
                const WebsiteStatus &status = m_websiteStatuses.at(website);
                QString tip = QStringLiteral("%1 — %2")
                                  .arg(status.label, status.status);
                if (status.minuteTs > 0) {
                    tip += QStringLiteral("\n%1: %2")
                               .arg(status.local
                                        ? QStringLiteral("Checked from this desktop")
                                        : QStringLiteral("Last completed minute"),
                                    QDateTime::fromMSecsSinceEpoch(status.minuteTs)
                                        .toLocalTime()
                                        .toString(QStringLiteral("yyyy-MM-dd HH:mm")));
                }
                if (!status.reason.isEmpty())
                    tip += QLatin1Char('\n') + status.reason;
                if (!status.localStatus.isEmpty()) {
                    tip += QStringLiteral("\nFrom this desktop: %1")
                               .arg(status.localStatus);
                    if (status.localCheckedTs > 0)
                        tip += QStringLiteral(" (checked %1)")
                                   .arg(QDateTime::fromMSecsSinceEpoch(
                                            status.localCheckedTs)
                                            .toLocalTime()
                                            .toString(QStringLiteral("HH:mm")));
                    if (!status.localReason.isEmpty())
                        tip += QLatin1Char('\n') + status.localReason;
                }
                if (websiteChecking(website))
                    tip += QStringLiteral("\nChecking now…");
                else if (m_presentation == Debug &&
                         websiteCountdownFraction(website) > 0.0)
                    tip += QStringLiteral("\nNext check in %1 s")
                               .arg(qCeil(websiteCountdownFraction(website) *
                                          kWebsiteCheckIntervalMs / 1000.0));
                if (onWebsiteClicked)
                    tip += QStringLiteral(
                        "\nClick to check again now and open the related "
                        "website page.");
                QToolTip::showText(help->globalPos(), tip, this);
                return true;
            }
        }
        return QWidget::event(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && rect().contains(event->pos())) {
            const int lane = categoryAt(event->pos());
            if (lane == stallCategoryIndex() && onStallClicked) {
                onStallClicked();
            } else if (lane < 0) {
                const int website = websiteStatusAt(event->pos());
                if (website >= 0 && onWebsiteClicked) {
                    onWebsiteClicked(m_websiteStatuses.at(website).id);
                } else if (onClicked)
                    onClicked();
            } else if (m_presentation != Header) {
                if (onCategoryClicked)
                    onCategoryClicked(categories().at(lane).badge);
                else if (onClicked)
                    onClicked();
            }
        }
        QWidget::mouseReleaseEvent(event);
    }

    void contextMenuEvent(QContextMenuEvent *event) override
    {
        if (categoryAt(event->pos()) == stallCategoryIndex() &&
            onStallContextMenu) {
            onStallContextMenu();
            event->accept();
            return;
        }
        QWidget::contextMenuEvent(event);
    }

    void showEvent(QShowEvent *event) override
    {
        QWidget::showEvent(event);
        updateWebsiteAnimation();
    }

    void hideEvent(QHideEvent *event) override
    {
        if (m_websiteAnimation)
            m_websiteAnimation->stop();
        QWidget::hideEvent(event);
    }

private:
    struct Category {
        QString badge;
        QString accent;
        QString icon;
    };

    // Every badge here is a light, a filter chip and a colour in the log's
    // badge column, and networkLogStyleFor() (MainWindowSettings.cpp) is what
    // fills them. Keep the two in step so every category can be reached.
    static QList<Category> &mutableCategories()
    {
        static QList<Category> values = {
            {"SESSION", "#f2cc60", "history"},
            {"STATUS", "#56d364", "check-circle"},
            {"PEER", "#3fb950", "people"},
            {"NODE", "#3fb950", "server"},
            {"FORKED", "#3fb950", "repo-forked"},
            {"MIRROR", "#39c5cf", "sync"},
            {"SYNC", "#39c5cf", "sync"},
            {"ACCOUNT", "#8b949e", "person"},
            {"ACTIONS", "#f0883e", "workflow"},
            {"PIN", "#79c0ff", "shield-check"},
            {"GIT", "#58a6ff", "git-commit"},
            {"BGTASK", "#8b949e", "gear"},
            {"BGBLOCK", "#f0883e", "stop"},
            {"BGNOTE", "#6e7681", "note"},
            {"PUBLISH", "#58a6ff", "upload"},
            {"PULL", "#3fb950", "git-pull-request"},
            {"MERGE", "#a371f7", "git-merge"},
            {"ISSUE", "#bc8cff", "issue-opened"},
            {"PROMPT", "#bc8cff", "comment"},
            {"BOUNTY", "#d29922", "star"},
            {"CRYPTO", "#79c0ff", "lock"},
            {"IDENTITY", "#79c0ff", "key"},
            {"ADMIN", "#db6d28", "crown"},
            {"NETWORK", "#f2cc60", "broadcast"},
            {"CLOUD", "#f6821f", "cloud"},
            {"DB", "#f778ba", "database"},
            {"APP", "#6cb6ff", "code"},
            {"CRON", "#dcbdfb", "clock"},
            {"WEB", "#7ee787", "link"},
            {"DAEMON", "#76e3ea", "repo"},
            {"IDS", "#ff9492", "eye"},
            {"CERT", "#e3b341", "shield-check"},
            {"OPS", "#b083f0", "terminal"},
            {"JOB", "#ffa657", "play"},
            {"MAIL", "#b4c9ff", "paper-airplane"},
            {"DEPLOY", "#f9826c", "rocket"},
            {"SYSTEM", "#909dab", "server"},
            {"STALL", "#d29922", "alert"},
            {"ERROR", "#f85149", "x"},
            {"INFO", "#6e7681", "info"},
        };
        return values;
    }

    static const QList<Category> &categories() { return mutableCategories(); }
    static constexpr int builtinCategoryCount() { return 40; }
    static int categoryCount() { return categories().size(); }
    static constexpr int kCompactRows = 3;
    static int compactColumns()
    {
        return (categoryCount() + kCompactRows - 1) / kCompactRows;
    }
    static constexpr int kCompactCell = 16;
    static constexpr int kCompactPadding = 7;
    static constexpr int kHeaderHeight = 32;
    static constexpr int kDebugHeight = 40;
    static constexpr int kDebugPadding = 4;
    static constexpr int kDebugCategorySlot = 25;
    static constexpr int kDebugWebsiteDot = 12;
    static constexpr int kDebugWebsiteStep = 8;
    static constexpr int kDebugWebsiteRing = 2;
    static constexpr int kWebsiteCheckIntervalMs = 60000;
    static constexpr int kWebsiteCheckBlinkTailMs = 900;

    static int categoryIndex(const QString &badge)
    {
        if (const int lane = laneForBadge(badge); lane >= 0)
            return lane;
        return builtinCategoryCount() - 1; // unknown categories pulse INFO
    }

    static QHash<QString, int> buildCategoryLanes()
    {
        QHash<QString, int> lanes;
        const QList<Category> &values = categories();
        lanes.reserve(values.size());
        for (int i = 0; i < values.size(); ++i) {
            if (!lanes.contains(values.at(i).badge))
                lanes.insert(values.at(i).badge, i);
        }
        return lanes;
    }

    static QHash<QString, int> &categoryLanes()
    {
        static QHash<QString, int> lanes = buildCategoryLanes();
        return lanes;
    }

    static int laneForBadge(const QString &badge)
    {
        const QHash<QString, int> &lanes = categoryLanes();
        const auto found = lanes.constFind(badge);
        return found == lanes.cend() ? -1 : found.value();
    }

    static int stallCategoryIndex()
    {
        static const int index = categoryIndex(QStringLiteral("STALL"));
        return index;
    }

    int compactCategoryWidth() const
    {
        return kCompactPadding * 2 + compactColumns() * kCompactCell;
    }

    int compactWebsiteWidth() const
    {
        if (m_websiteStatuses.isEmpty())
            return 0;
        return 12 + ((m_websiteStatuses.size() + kCompactRows - 1) /
                     kCompactRows) * 9 + kCompactPadding;
    }

    void updateCompactSize()
    {
        setFixedSize(compactCategoryWidth() + compactWebsiteWidth(),
                     kCompactPadding * 2 + kCompactRows * kCompactCell);
    }

    int debugCategoryWidth() const
    {
        return kDebugPadding * 2 + displayLanes().size() * kDebugCategorySlot;
    }

    int debugWebsiteWidth() const
    {
        return m_websiteStatuses.isEmpty()
                   ? 0
                   : 8 + (m_websiteStatuses.size() - 1) * kDebugWebsiteStep +
                         kDebugWebsiteDot + 4;
    }

    void updateDebugSize()
    {
        setFixedSize(debugCategoryWidth() + debugWebsiteWidth(), kDebugHeight);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    }

    int headerWebsiteWidth() const
    {
        if (m_websiteStatuses.isEmpty())
            return 0;
        const int wanted = 16 +
            ((m_websiteStatuses.size() + 1) / 2) * 7;
        return qBound(72, wanted, qMax(72, width() / 3));
    }

    int websiteSeparatorX() const
    {
        return m_presentation == Compact
                   ? compactCategoryWidth() + 3
                   : m_presentation == Debug
                         ? debugCategoryWidth() + 3
                   : qMax(1, width() - headerWebsiteWidth());
    }

    int categoryAreaWidth() const
    {
        if (m_presentation == Compact)
            return compactCategoryWidth();
        if (m_presentation == Debug)
            return debugCategoryWidth();
        return m_websiteStatuses.isEmpty() ? width()
                                           : websiteSeparatorX() - 3;
    }

    int categorySlotWidth() const
    {
        if (m_presentation == Debug)
            return kDebugCategorySlot;
        return qMax(1, categoryAreaWidth() / categoryCount());
    }

    QRect categoryIconRect(int index) const
    {
        if (m_presentation == Compact) {
            const int col = index % compactColumns();
            const int row = index / compactColumns();
            return QRect(kCompactPadding + col * kCompactCell,
                         kCompactPadding + row * kCompactCell,
                         kCompactCell, kCompactCell);
        }
        if (m_presentation == Debug) {
            const int x = kDebugPadding + index * kDebugCategorySlot;
            // A count occupies the top of the slot in debug mode. Keeping the
            // icon below it makes both legible, especially for 2- and 3-digit
            // totals that previously painted directly over the icon.
            return QRect(x + (kDebugCategorySlot - 14) / 2, 10, 14, 14);
        }
        const int slot = categorySlotWidth();
        return QRect(index * slot, 2, slot, 15);
    }

    QRect categoryCountRect(int index) const
    {
        if (m_presentation == Debug) {
            const int x = kDebugPadding + index * kDebugCategorySlot;
            return QRect(x, 1, kDebugCategorySlot, 8);
        }
        const int slot = categorySlotWidth();
        return QRect(index * slot, 17, slot, 12);
    }

    QRect categoryLabelRect(int index) const
    {
        const int x = kDebugPadding + index * kDebugCategorySlot;
        return QRect(x, 26, kDebugCategorySlot, 12);
    }

    int categoryAt(const QPoint &point) const
    {
        const int displayIndex = categoryDisplayIndexAt(point);
        if (displayIndex < 0)
            return -1;
        const QVector<int> lanes = displayLanes();
        return lanes.value(displayIndex, -1);
    }

    QVector<int> displayLanes() const
    {
        QVector<int> lanes = sortedCategoryLanes();
        if (m_presentation != Debug || m_hiddenCategories.isEmpty())
            return lanes;
        lanes.erase(std::remove_if(lanes.begin(), lanes.end(),
                                   [this](int lane) {
                                       return m_hiddenCategories.contains(
                                           categories().at(lane).badge);
                                   }),
                    lanes.end());
        return lanes;
    }

    int categoryDisplayIndexAt(const QPoint &point) const
    {
        if (m_presentation == Compact) {
            if (point.x() < kCompactPadding ||
                point.x() >= compactCategoryWidth() - kCompactPadding ||
                point.y() < kCompactPadding)
                return -1;
            const int col = (point.x() - kCompactPadding) / kCompactCell;
            const int row = (point.y() - kCompactPadding) / kCompactCell;
            const int index = row * compactColumns() + col;
            return index >= 0 && index < categoryCount() ? index : -1;
        }
        if (m_presentation == Debug) {
            if (point.x() < kDebugPadding || point.x() >= debugCategoryWidth())
                return -1;
            const int index = (point.x() - kDebugPadding) / kDebugCategorySlot;
            return index >= 0 && index < displayLanes().size() ? index : -1;
        }
        if (point.x() < 0 || point.x() >= categoryAreaWidth())
            return -1;
        return qBound(0, point.x() / categorySlotWidth(), categoryCount() - 1);
    }

    QVector<int> sortedCategoryLanes() const
    {
        QVector<int> lanes;
        lanes.reserve(categoryCount());
        for (int i = 0; i < categoryCount(); ++i)
            lanes << i;
        std::stable_sort(lanes.begin(), lanes.end(),
                         [this](int left, int right) {
                             const QString leftBadge = categories().at(left).badge;
                             const QString rightBadge = categories().at(right).badge;
                             if (m_counts.value(leftBadge) ==
                                 m_counts.value(rightBadge))
                                 return left < right;
                             return m_counts.value(leftBadge) >
                                    m_counts.value(rightBadge);
                         });
        return lanes;
    }

    QRect websiteStatusRect(int index) const
    {
        if (m_presentation == Compact) {
            const int col = index / kCompactRows;
            const int row = index % kCompactRows;
            const int x = websiteSeparatorX() + 7 + col * 9;
            const int y = kCompactPadding + row * kCompactCell + 5;
            return QRect(x, y, 6, 6);
        }
        if (m_presentation == Debug) {
            const int x = websiteSeparatorX() + 6 + index * kDebugWebsiteStep;
            return QRect(x, (kDebugHeight - kDebugWebsiteDot) / 2,
                         kDebugWebsiteDot, kDebugWebsiteDot);
        }
        const int col = index / 2;
        const int row = index % 2;
        const int x = websiteSeparatorX() + 8 + col * 7;
        const int y = 8 + row * 10;
        return QRect(x, y, 5, 5);
    }

    qreal websiteCountdownFraction(int index) const
    {
        const qint64 checked =
            m_websiteCheckedAt.value(m_websiteStatuses.at(index).id, 0);
        if (checked <= 0)
            return 0.0;
        const qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - checked;
        if (elapsed < 0 || elapsed >= kWebsiteCheckIntervalMs)
            return 0.0;
        return 1.0 - qreal(elapsed) / qreal(kWebsiteCheckIntervalMs);
    }

    bool websiteChecking(int index) const
    {
        if (index < 0 || index >= m_websiteStatuses.size())
            return false;
        const WebsiteStatus &status = m_websiteStatuses.at(index);
        if (status.checking)
            return true;
        return m_websiteBlinkUntil.value(status.id, 0) >
               QDateTime::currentMSecsSinceEpoch();
    }

    bool anyWebsiteChecking() const
    {
        for (int i = 0; i < m_websiteStatuses.size(); ++i) {
            if (websiteChecking(i))
                return true;
        }
        return false;
    }

    void noteWebsiteCheckCycles()
    {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        QSet<QString> present;
        QSet<QString> checking;
        for (const WebsiteStatus &status : m_websiteStatuses) {
            present.insert(status.id);
            if (status.checking) {
                checking.insert(status.id);
                m_websiteBlinkUntil[status.id] = now + kWebsiteCheckBlinkTailMs;
            } else if (m_websiteChecking.contains(status.id) ||
                       !m_websiteCheckedAt.contains(status.id)) {
                m_websiteCheckedAt[status.id] = now;
            }
        }
        m_websiteChecking = checking;
        for (auto it = m_websiteCheckedAt.begin();
             it != m_websiteCheckedAt.end();) {
            if (present.contains(it.key()))
                ++it;
            else
                it = m_websiteCheckedAt.erase(it);
        }
        for (auto it = m_websiteBlinkUntil.begin();
             it != m_websiteBlinkUntil.end();) {
            if (present.contains(it.key()))
                ++it;
            else
                it = m_websiteBlinkUntil.erase(it);
        }
    }

    void updateWebsiteAnimation()
    {
        const bool blinking = anyWebsiteChecking();
        if (m_websiteStatuses.isEmpty() || !isVisible() || !blinking) {
            if (m_websiteAnimation)
                m_websiteAnimation->stop();
            return;
        }
        if (!m_websiteAnimation) {
            m_websiteAnimation = new QTimer(this);
            connect(m_websiteAnimation, &QTimer::timeout, this, [this] {
                m_websiteBlinkOn = !m_websiteBlinkOn;
                updateWebsiteAnimation();
                update();
            });
        }
        const int interval = blinking ? 200 : 1000;
        if (m_websiteAnimation->interval() != interval)
            m_websiteAnimation->setInterval(interval);
        if (!m_websiteAnimation->isActive())
            m_websiteAnimation->start();
    }

    int websiteStatusAt(const QPoint &point) const
    {
        for (int i = m_websiteStatuses.size() - 1; i >= 0; --i) {
            const int grow = m_presentation == Debug ? 4 : 2;
            if (websiteStatusRect(i).adjusted(-grow, -grow, grow, grow)
                    .contains(point))
                return i;
        }
        return -1;
    }

    static QColor websiteStatusColor(const QString &status, bool dark)
    {
        if (status == QLatin1String("operational"))
            return QColor(dark ? "#3fb950" : "#1a7f37");
        if (status == QLatin1String("degraded"))
            return QColor(dark ? "#d29922" : "#9a6700");
        if (status == QLatin1String("down"))
            return QColor(dark ? "#f85149" : "#cf222e");
        return Theme::remapColor(QColor(dark ? "#6e7681" : "#8c959f"));
    }

    void updateSummaryToolTip()
    {
        if (m_presentation == Header) {
            setToolTip(QStringLiteral(
                "Log categories and session counts; website /status results "
                "for the newest completed minute are separated on the right."));
            return;
        }
        setToolTip(QStringLiteral(
            "%1 live-log categories%2 — hover an icon for its count; click for full "
            "Log page filtered to this category")
                       .arg(categoryCount())
                       .arg(m_websiteStatuses.isEmpty()
                                ? QString()
                                : QStringLiteral(" and %1 website status results")
                                      .arg(m_websiteStatuses.size())));
    }

    Presentation m_presentation = Compact;
    QHash<QString, quint64> m_counts;
    QHash<QString, bool> m_blinkVisible;
    QHash<QString, bool> m_blinking;
    QHash<QString, int> m_generation;
    bool m_expanded = false;
    QList<WebsiteStatus> m_websiteStatuses;
    QString m_stallToolTip;
    QHash<QString, qint64> m_websiteCheckedAt;
    QHash<QString, qint64> m_websiteBlinkUntil;
    QSet<QString> m_websiteChecking;
    QSet<QString> m_hiddenCategories;
    QTimer *m_websiteAnimation = nullptr;
    bool m_websiteBlinkOn = true;
};

struct ConflictRegion {
    int startLine = -1; // the "<<<<<<<" line
    int sepLine = -1;   // the "=======" line
    int endLine = -1;   // the ">>>>>>>" line
};

QList<ConflictRegion> findConflicts(const QStringList &lines);

class ConflictHighlighter : public QSyntaxHighlighter
{
public:
    explicit ConflictHighlighter(QTextDocument *document)
        : QSyntaxHighlighter(document)
    {
        const bool dark = currentThemeIsDark();
        auto bg = [](const QColor &c) {
            QTextCharFormat f;
            f.setBackground(c);
            return f;
        };
        m_marker.setForeground(Theme::remapColor(QColor(dark ? "#8b949e" : "#6e7781")));
        m_marker.setFontWeight(QFont::Bold);
        m_ours = bg(QColor(dark ? "#0b2a4a" : "#ddf4ff"));    // HEAD / base side
        m_theirs = bg(QColor(dark ? "#0b3a1e" : "#e6ffec"));  // PR side
    }

protected:
    void highlightBlock(const QString &text) override
    {
        int prev = previousBlockState();
        int state = prev == 1 || prev == 2 ? prev : 0;
        if (text.startsWith(QLatin1String("<<<<<<< "))) {
            setFormat(0, text.length(), m_marker);
            state = 1;
        } else if (text.startsWith(QLatin1String("=======")) &&
                   text.trimmed().length() == 7 && state == 1) {
            setFormat(0, text.length(), m_marker);
            state = 2;
        } else if (text.startsWith(QLatin1String(">>>>>>> ")) && state == 2) {
            setFormat(0, text.length(), m_marker);
            state = 0;
        } else if (state == 1) {
            setFormat(0, text.length(), m_ours);
        } else if (state == 2) {
            setFormat(0, text.length(), m_theirs);
        }
        setCurrentBlockState(state);
    }

private:
    QTextCharFormat m_marker, m_ours, m_theirs;
};

class BusySpinner : public QWidget
{
public:
    explicit BusySpinner(QWidget *parent = nullptr, int size = 16)
        : QWidget(parent), m_size(size)
    {
        setFixedSize(size, size);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        m_timer = new QTimer(this);
        m_timer->setInterval(60);
        connect(m_timer, &QTimer::timeout, this, [this] {
            m_angle = (m_angle + 30) % 360;
            update();
        });
    }

protected:
    void showEvent(QShowEvent *e) override
    {
        m_timer->start();
        QWidget::showEvent(e);
    }
    void hideEvent(QHideEvent *e) override
    {
        m_timer->stop();
        QWidget::hideEvent(e);
    }
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.drawPixmap(0, 0,
                     refreshPixmap(QColor(Theme::kRunning), m_angle, m_size));
    }

private:
    QTimer *m_timer = nullptr;
    int m_size;
    int m_angle = 0;
};

class DoneCheckMark : public QWidget
{
public:
    explicit DoneCheckMark(QWidget *parent = nullptr, int size = 18,
                           const QColor &color = QColor("#3fb950"))
        : QWidget(parent), m_size(size), m_color(color)
    {
        setFixedSize(size, size);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        m_timer = new QTimer(this);
        m_timer->setInterval(kTickMs);
        connect(m_timer, &QTimer::timeout, this, [this] {
            m_elapsedMs += kTickMs;
            if (m_elapsedMs >= kDrawMs + kPulseMs * kPulses)
                m_timer->stop();
            update();
        });
    }

protected:
    void showEvent(QShowEvent *e) override
    {
        if (m_elapsedMs < kDrawMs + kPulseMs * kPulses)
            m_timer->start();
        QWidget::showEvent(e);
    }
    void hideEvent(QHideEvent *e) override
    {
        m_timer->stop();
        QWidget::hideEvent(e);
    }
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const double t = qBound(0.0, double(m_elapsedMs) / kDrawMs, 1.0);
        double pulse = 0.0;
        if (m_elapsedMs > kDrawMs) {
            const double phase =
                double((m_elapsedMs - kDrawMs) % kPulseMs) / double(kPulseMs);
            pulse = std::sin(phase * M_PI);
        }

        const double penWidth = 1.6;
        const double inset = penWidth / 2.0 + 1.0;
        const QRectF box(inset, inset, m_size - 2 * inset, m_size - 2 * inset);
        QColor ringColor = m_color;
        ringColor.setAlpha(int(110 + 145 * pulse));
        QPen ring(ringColor);
        ring.setWidthF(penWidth);
        p.setPen(ring);
        p.setBrush(Qt::NoBrush);
        p.drawArc(box, 90 * 16, -int(t * 360) * 16);

        const QPointF a(m_size * 0.28, m_size * 0.52);
        const QPointF b(m_size * 0.43, m_size * 0.68);
        const QPointF c(m_size * 0.73, m_size * 0.34);
        const double len1 = QLineF(a, b).length();
        const double len2 = QLineF(b, c).length();
        const double drawn = t * (len1 + len2);
        QPen stroke(m_color);
        stroke.setWidthF(2.0);
        stroke.setCapStyle(Qt::RoundCap);
        stroke.setJoinStyle(Qt::RoundJoin);
        p.setPen(stroke);
        if (drawn <= len1) {
            p.drawLine(QLineF(a, a + (b - a) * (len1 > 0 ? drawn / len1 : 1.0)));
        } else {
            p.drawLine(QLineF(a, b));
            const double rest = qMin(drawn - len1, len2);
            p.drawLine(QLineF(b, b + (c - b) * (len2 > 0 ? rest / len2 : 1.0)));
        }
    }

private:
    static constexpr int kTickMs = 30;
    static constexpr int kDrawMs = 420;  // ring + check stroke in
    static constexpr int kPulseMs = 900; // one breath of the ring
    static constexpr int kPulses = 2;
    QTimer *m_timer = nullptr;
    int m_size;
    QColor m_color;
    int m_elapsedMs = 0;
};

// Confetti and fireworks across the whole window for a few seconds. Raised
// when the relay says a brand-new member just joined ForkMesh, so the same
// moment the World and the website celebrate is visible here too. The overlay
// is click-through, never takes focus, and deletes itself when the show ends.
class CelebrationOverlay final : public QWidget
{
public:
    explicit CelebrationOverlay(QWidget *parent, int durationMs = 12000,
                                const QString &headline = QString(),
                                const QString &detail = QString(),
                                int intensity = 1)
        : QWidget(parent), m_durationMs(durationMs), m_headline(headline),
          m_detail(detail)
    {
        const int flakes = kFlakes * qBound(1, intensity, 4);
        const int bursts = kBursts * qBound(1, intensity, 4);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setAttribute(Qt::WA_TranslucentBackground);
        setFocusPolicy(Qt::NoFocus);
        setGeometry(parent ? parent->rect() : QRect(0, 0, 640, 480));

        auto *random = QRandomGenerator::global();
        const int wide = qMax(width(), 320);
        m_flakes.reserve(flakes);
        for (int i = 0; i < flakes; ++i) {
            Flake flake;
            flake.x = random->bounded(wide);
            // Start the field mid-fall so the first frame is already a party.
            flake.y = -random->bounded(height() + 200);
            flake.fall = 1.4 + random->bounded(220) / 100.0;
            flake.drift = (random->bounded(120) - 60) / 100.0;
            flake.spin = (random->bounded(160) - 80) / 10.0;
            flake.angle = random->bounded(360);
            flake.hue = random->bounded(360);
            flake.width = 5 + random->bounded(5);
            flake.height = 9 + random->bounded(7);
            m_flakes.append(flake);
        }
        for (int i = 0; i < bursts; ++i)
            m_bursts.append(newBurst(random, -random->bounded(kBurstMs)));

        m_timer = new QTimer(this);
        m_timer->setInterval(kTickMs);
        connect(m_timer, &QTimer::timeout, this, [this] {
            m_elapsedMs += kTickMs;
            if (m_elapsedMs >= m_durationMs) {
                m_timer->stop();
                deleteLater();
                return;
            }
            advance();
            placeCloseButton();
            update();
        });
        m_timer->start();
        raise();
        show();
        if (parent) {
            m_closeButton = new QToolButton(parent);
            m_closeButton->setText(QString::fromUtf8("\xE2\x9C\x95"));
            m_closeButton->setToolTip(QStringLiteral("Close"));
            m_closeButton->setAccessibleName(QStringLiteral("Close celebration"));
            m_closeButton->setCursor(Qt::PointingHandCursor);
            m_closeButton->setFocusPolicy(Qt::NoFocus);
            m_closeButton->setFixedSize(kCloseSize, kCloseSize);
            m_closeButton->setStyleSheet(
                QStringLiteral("QToolButton { color: white; background: rgba(15, 23, 42, 230);"
                               " border: 1px solid rgba(255, 255, 255, 60);"
                               " border-radius: %1px; font-size: 13px; padding: 0; }"
                               " QToolButton:hover { background: rgba(51, 65, 85, 240); }")
                    .arg(kCloseSize / 2));
            connect(m_closeButton, &QToolButton::clicked, this, [this] {
                m_timer->stop();
                deleteLater();
            });
            m_closeButton->hide();
        }
    }

    ~CelebrationOverlay() override { delete m_closeButton; }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        // The last second fades everything out instead of cutting to nothing.
        const int remaining = m_durationMs - m_elapsedMs;
        const qreal fade =
            remaining >= kFadeMs ? 1.0 : qMax(0.0, qreal(remaining) / kFadeMs);

        p.setPen(Qt::NoPen);
        for (const Burst &burst : std::as_const(m_bursts)) {
            if (burst.ageMs < 0)
                continue;
            const qreal life = qreal(burst.ageMs) / kBurstMs;
            if (life > 1.0)
                continue;
            const qreal radius = burst.radius * (0.15 + 0.85 * life);
            const qreal alpha = fade * (1.0 - life) * (life < 0.12
                                                           ? life / 0.12
                                                           : 1.0);
            for (int spoke = 0; spoke < kBurstSpokes; ++spoke) {
                const qreal theta =
                    (2.0 * M_PI * spoke) / kBurstSpokes + burst.rotation;
                QColor color =
                    QColor::fromHsv((burst.hue + spoke * 14) % 360, 190, 255);
                color.setAlphaF(qBound(0.0, alpha, 1.0));
                p.setBrush(color);
                p.drawEllipse(QPointF(burst.center.x() + std::cos(theta) * radius,
                                      burst.center.y() + std::sin(theta) * radius),
                              3.0, 3.0);
            }
        }

        for (const Flake &flake : std::as_const(m_flakes)) {
            QColor color = QColor::fromHsv(flake.hue, 190, 245);
            color.setAlphaF(qBound(0.0, fade, 1.0));
            p.save();
            p.translate(flake.x, flake.y);
            p.rotate(flake.angle);
            p.setBrush(color);
            p.drawRoundedRect(QRectF(-flake.width / 2.0, -flake.height / 2.0,
                                     flake.width, flake.height),
                              1.5, 1.5);
            p.restore();
        }
        if (!m_headline.isEmpty())
            paintHeadline(p, fade);
    }

private:
    void paintHeadline(QPainter &p, qreal fade)
    {
        const qreal pop = m_elapsedMs < kPopMs
                              ? 0.4 + 0.6 * qreal(m_elapsedMs) / kPopMs
                              : 1.0;
        QFont big = font();
        big.setPointSizeF(qMax(20.0, width() / 36.0));
        big.setBold(true);
        QFont small = font();
        small.setPointSizeF(qMax(11.0, big.pointSizeF() / 2.4));
        const int room = qMax(200, qMin(width() - 48, 720));
        const QFontMetrics bigMetrics(big);
        const QFontMetrics smallMetrics(small);
        const int wrapFlags = Qt::AlignHCenter | Qt::TextWordWrap;
        const QRect wrapBox(0, 0, room - 48, 100000);
        const QRect headBox =
            bigMetrics.boundingRect(wrapBox, wrapFlags, m_headline);
        const QRect detailBox =
            m_detail.isEmpty()
                ? QRect()
                : smallMetrics.boundingRect(wrapBox, wrapFlags, m_detail);
        const int textWide = qMax(headBox.width(), detailBox.width());
        const int cardWide = textWide + 56;
        const int cardHigh = headBox.height() + 36 +
                             (m_detail.isEmpty() ? 0 : detailBox.height() + 8);
        m_card = QRectF(width() / 2.0 - cardWide / 2.0,
                        height() * 0.38 - cardHigh / 2.0, cardWide, cardHigh);
        p.save();
        p.setOpacity(qBound(0.0, fade * qMin(1.0, pop * 1.4), 1.0));
        p.translate(width() / 2.0, height() * 0.38);
        p.scale(pop, pop);
        const QRectF card(-cardWide / 2.0, -cardHigh / 2.0, cardWide, cardHigh);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(15, 23, 42, 210));
        p.drawRoundedRect(card, 22, 22);
        p.setPen(Qt::white);
        p.setFont(big);
        const QRectF headRect(-wrapBox.width() / 2.0, card.top() + 18,
                              wrapBox.width(), headBox.height());
        p.drawText(headRect, wrapFlags, m_headline);
        if (!m_detail.isEmpty()) {
            QColor soft(Qt::white);
            soft.setAlphaF(0.85);
            p.setPen(soft);
            p.setFont(small);
            p.drawText(QRectF(headRect.left(), headRect.bottom() + 8,
                              headRect.width(), detailBox.height()),
                       wrapFlags, m_detail);
        }
        p.restore();
    }

    struct Flake {
        qreal x = 0;
        qreal y = 0;
        qreal fall = 2;
        qreal drift = 0;
        qreal spin = 0;
        qreal angle = 0;
        qreal width = 7;
        qreal height = 12;
        int hue = 0;
    };
    struct Burst {
        QPointF center;
        qreal radius = 60;
        qreal rotation = 0;
        int hue = 0;
        int ageMs = 0;
    };

    Burst newBurst(QRandomGenerator *random, int ageMs) const
    {
        Burst burst;
        burst.center = QPointF(40 + random->bounded(qMax(1, width() - 80)),
                               40 + random->bounded(qMax(1, height() / 2)));
        burst.radius = 46 + random->bounded(46);
        burst.rotation = random->bounded(100) / 100.0;
        burst.hue = random->bounded(360);
        burst.ageMs = ageMs;
        return burst;
    }

    void placeCloseButton()
    {
        if (!m_closeButton)
            return;
        const bool onCard = !m_headline.isEmpty();
        const bool shown = m_elapsedMs >= kPopMs &&
                           m_durationMs - m_elapsedMs > kFadeMs &&
                           (!onCard || !m_card.isEmpty());
        if (!shown) {
            m_closeButton->hide();
            return;
        }
        const QPoint corner =
            onCard ? QPoint(qRound(m_card.right()) - kCloseSize + 6,
                            qRound(m_card.top()) - 6)
                   : QPoint(width() - kCloseSize - 16, 16);
        m_closeButton->move(mapToParent(corner));
        if (m_closeButton->isHidden()) {
            m_closeButton->show();
            m_closeButton->raise();
        }
    }

    void advance()
    {
        // The window can be resized mid-show; follow it rather than leaving a
        // stale rectangle of confetti over part of the UI.
        if (parentWidget() && geometry() != parentWidget()->rect())
            setGeometry(parentWidget()->rect());
        auto *random = QRandomGenerator::global();
        const int wide = qMax(width(), 320);
        for (Flake &flake : m_flakes) {
            flake.y += flake.fall * (kTickMs / 16.0);
            flake.x += flake.drift * (kTickMs / 16.0);
            flake.angle += flake.spin * (kTickMs / 16.0);
            if (flake.y > height() + 20) {
                flake.y = -20 - random->bounded(120);
                flake.x = random->bounded(wide);
            }
            if (flake.x < -20)
                flake.x = wide + 20;
            else if (flake.x > wide + 20)
                flake.x = -20;
        }
        for (Burst &burst : m_bursts) {
            burst.ageMs += kTickMs;
            if (burst.ageMs > kBurstMs)
                burst = newBurst(random, -random->bounded(kBurstMs / 2));
        }
    }

    static constexpr int kTickMs = 33;
    static constexpr int kFadeMs = 900;
    static constexpr int kBurstMs = 1400;
    static constexpr int kBurstSpokes = 12;
    static constexpr int kFlakes = 90;
    static constexpr int kBursts = 5;
    static constexpr int kPopMs = 260;
    static constexpr int kCloseSize = 26;
    QTimer *m_timer = nullptr;
    QPointer<QToolButton> m_closeButton;
    QRectF m_card;
    int m_durationMs;
    int m_elapsedMs = 0;
    QString m_headline;
    QString m_detail;
    QList<Flake> m_flakes;
    QList<Burst> m_bursts;
};

class RingSpinner : public QWidget
{
public:
    explicit RingSpinner(QWidget *parent = nullptr,
                         const QColor &color = QColor(Theme::kRunning))
        : QWidget(parent), m_color(color)
    {
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        m_timer = new QTimer(this);
        m_timer->setInterval(40);
        connect(m_timer, &QTimer::timeout, this, [this] {
            m_angle = (m_angle + 8) % 360;
            update();
        });
    }

protected:
    void showEvent(QShowEvent *e) override
    {
        m_timer->start();
        QWidget::showEvent(e);
    }
    void hideEvent(QHideEvent *e) override
    {
        m_timer->stop();
        QWidget::hideEvent(e);
    }
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const double pen = 2.0;
        const double inset = pen / 2.0 + 1.0;
        const QRectF box(inset, inset, width() - 2 * inset, height() - 2 * inset);
        QPen track(QColor(m_color.red(), m_color.green(), m_color.blue(), 60));
        track.setWidthF(pen);
        p.setPen(track);
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(box);
        QPen arc(m_color);
        arc.setWidthF(pen);
        arc.setCapStyle(Qt::RoundCap);
        p.setPen(arc);
        p.drawArc(box, -m_angle * 16, 100 * 16);
    }

private:
    QTimer *m_timer = nullptr;
    QColor m_color;
    int m_angle = 0;
};

// The issue looper reads as one toolbar glyph with an on/off menu rather than a
// switch: click the icon, pick On or Off. It keeps the travelling neon loop while
// running, and the "#N" badge stays a shortcut into the running agent session.
class LooperToggle : public QWidget
{
public:
    explicit LooperToggle(QWidget *parent = nullptr) : QWidget(parent)
    {
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_TranslucentBackground);
        setToolTip(QString::fromUtf8(
            "Issue looper \xE2\x80\x94 run the default agent on every open issue in "
            "turn. Click for the on/off menu."));
        m_loopTimer = new QTimer(this);
        m_loopTimer->setInterval(40); // smooth travel; lap speed set by the step
        connect(m_loopTimer, &QTimer::timeout, this, [this] {
            m_loopPos += 0.0035; // ~one slow lap every ~11s
            if (m_loopPos >= 1.0)
                m_loopPos -= 1.0;
            update();
        });
    }

    void setActive(bool on)
    {
        if (m_active == on)
            return;
        m_active = on;
        if (m_active)
            m_loopTimer->start();
        else
            m_loopTimer->stop();
        update();
    }
    bool isActive() const { return m_active; }

    void setIssueNumber(int n)
    {
        if (m_issue == n)
            return;
        m_issue = n;
        updateGeometry(); // width depends on the "#N" badge
        update();
    }
    void setOnClick(std::function<void()> cb) { m_onClick = std::move(cb); }
    void setOnNumberClick(std::function<void()> cb)
    {
        m_onNumberClick = std::move(cb);
    }

    // Matches VerticalIconButton: the looper sits in the issue toolbar between
    // "Reprioritize" and the view tabs, so it is measured and painted as one of
    // those glyph-over-caption tiles rather than as a chip of its own.
    QSize sizeHint() const override
    {
        QFont caption = font();
        caption.setPixelSize(10);
        caption.setWeight(QFont::DemiBold);
        const int captionW =
            QFontMetrics(caption).horizontalAdvance(QStringLiteral("Looper"));
        return QSize(qMax(44, qMax(kIconPx, captionW) + 16), kHeight);
    }
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton) {
            if (m_onNumberClick && m_issue > 0
                && m_labelRect.contains(e->position())) {
                m_onNumberClick();
                return;
            }
            showStateMenu();
            return;
        }
        QWidget::mousePressEvent(e);
    }
    void enterEvent(QEnterEvent *) override
    {
        m_hover = true;
        update();
    }
    void leaveEvent(QEvent *) override
    {
        m_hover = false;
        update();
    }
    void paintEvent(QPaintEvent *) override
    {
        QPainter g(this);
        g.setRenderHint(QPainter::Antialiasing);
        const bool dark = currentThemeIsDark();
        const QColor neon(57, 255, 110); // neon green

        if (m_hover) {
            g.setPen(Qt::NoPen);
            g.setBrush(Theme::remapColor(QColor(dark ? "#21262d" : "#eaeef2")));
            g.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 6, 6);
        }

        const QRect iconRect((width() - kIconPx) / 2, 6, kIconPx, kIconPx);
        // Running state keeps the travelling light, now a track around the
        // glyph instead of the dark chip the toggle used to carry.
        if (m_active) {
            QPainterPath track;
            track.addRoundedRect(
                QRectF(iconRect).adjusted(-4.0, -3.5, 4.0, 3.5), 7.0, 7.0);
            QPen base(QColor(dark ? "#1f3d29" : "#aceebb"));
            base.setWidthF(1.4);
            g.strokePath(track, base);
            drawTravellingLoop(g, track, neon);
        }

        const QColor glyphColor =
            m_active ? neon
                     : QColor(dark ? (m_hover ? "#e6edf3" : "#8b949e")
                                   : (m_hover ? "#1f2328" : "#656d76"));
        g.drawPixmap(iconRect.topLeft(),
                     tintedOcticonPixmap(QStringLiteral("loop"), glyphColor,
                                         kIconPx));

        QFont caption = font();
        caption.setPixelSize(10);
        caption.setWeight(QFont::DemiBold);
        g.setFont(caption);
        g.setPen(m_active ? neon : glyphColor);
        g.drawText(QRect(2, iconRect.bottom() + 2, width() - 4, 14),
                   Qt::AlignHCenter | Qt::AlignTop, QStringLiteral("Looper"));

        const QString label = labelText();
        if (label.isEmpty()) {
            m_labelRect = QRectF();
            return;
        }
        // "#N" badge, in the same top-right corner slot the icon tiles use for
        // their counts, so a running looper reads as "Looper ¹²³".
        QFont bf = font();
        bf.setPixelSize(9);
        bf.setBold(true);
        g.setFont(bf);
        const qreal bw =
            qMax(qreal(kBadgeH), QFontMetrics(bf).horizontalAdvance(label) + 8.0);
        const QRectF badge(qMin(qreal(width() - bw),
                                iconRect.right() + kBadgeH / 2.0 + 2 - bw),
                           qMax(0.0, double(iconRect.top() - 5)), bw, kBadgeH);
        g.setBrush(QColor(m_active ? "#1f3d29" : (dark ? "#30363d" : "#d0d7de")));
        g.setPen(Qt::NoPen);
        g.drawRoundedRect(badge, kBadgeH / 2.0, kBadgeH / 2.0);
        g.setPen(m_active ? neon : Theme::remapColor(QColor(dark ? "#e6edf3" : "#1f2328")));
        g.drawText(badge, Qt::AlignCenter, label);
        m_labelRect = badge;
    }

private:
    void showStateMenu()
    {
        QMenu menu(this);
        menu.setObjectName(QStringLiteral("issueLooperMenu"));
        QAction *on = menu.addAction(QStringLiteral("On"));
        QAction *off = menu.addAction(QStringLiteral("Off"));
        on->setCheckable(true);
        off->setCheckable(true);
        on->setChecked(m_active);
        off->setChecked(!m_active);
        on->setToolTip(QStringLiteral(
            "Run the default agent on every open issue in turn"));
        off->setToolTip(QStringLiteral(
            "Stop after the current agent finishes"));
        QAction *picked = menu.exec(mapToGlobal(QPoint(0, height() + 2)));
        if (!picked || !m_onClick)
            return;
        if ((picked == on) != m_active) // only act on a real state change
            m_onClick();
    }
    QString labelText() const
    {
        return m_issue > 0 ? QStringLiteral("#%1").arg(m_issue) : QString();
    }
    void drawTravellingLoop(QPainter &g, const QPainterPath &chip,
                            const QColor &neon) const
    {
        const qreal seg = 0.30; // fraction of the loop lit at once
        QPainterPath lit;
        const int steps = 40;
        for (int i = 0; i <= steps; ++i) {
            qreal t = m_loopPos + seg * i / steps;
            if (t >= 1.0)
                t -= 1.0; // wrap into [0,1); m_loopPos<1 and seg<1, so one wrap
            const QPointF p = chip.pointAtPercent(t);
            if (i == 0)
                lit.moveTo(p);
            else
                lit.lineTo(p);
        }
        QPen glow(QColor(neon.red(), neon.green(), neon.blue(), 70));
        glow.setWidthF(5.0);
        glow.setCapStyle(Qt::RoundCap);
        g.strokePath(lit, glow);
        QPen core(neon);
        core.setWidthF(2.0);
        core.setCapStyle(Qt::RoundCap);
        g.strokePath(lit, core);
    }

    static constexpr int kIconPx = 16;  // VerticalIconButton's glyph box
    static constexpr int kHeight = 44;  // ... and its tile height
    static constexpr int kBadgeH = 14;

    bool m_active = false;
    bool m_hover = false;
    int m_issue = 0;
    qreal m_loopPos = 0.0;
    QTimer *m_loopTimer = nullptr;
    QRectF m_labelRect; // hit rect of the "#N" badge, set in paintEvent
    std::function<void()> m_onClick;
    std::function<void()> m_onNumberClick;
};

class CodePreviewEditor;

class CodeLineNumberArea : public QWidget
{
public:
    explicit CodeLineNumberArea(CodePreviewEditor *editor);

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    CodePreviewEditor *m_editor = nullptr;
};

class CodePreviewEditor : public QPlainTextEdit
{
public:
    explicit CodePreviewEditor(const QString &path, QWidget *parent = nullptr)
        : QPlainTextEdit(parent), m_lineNumberArea(new CodeLineNumberArea(this))
    {
        setObjectName("codeEditor");
        setProperty("previewPath", path);
        setReadOnly(true);
        setLineWrapMode(QPlainTextEdit::NoWrap);
        setFrameShape(QFrame::NoFrame);

        QFont mono = font();
        mono.setFamily(QStringLiteral("Menlo"));
        mono.setStyleHint(QFont::Monospace);
        mono.setPointSize(12);
        setFont(mono);
        setTabStopDistance(fontMetrics().horizontalAdvance(QLatin1Char(' ')) * 4);

        connect(this, &QPlainTextEdit::blockCountChanged, this,
                [this] { updateLineNumberAreaWidth(); });
        connect(this, &QPlainTextEdit::updateRequest, this,
                [this](const QRect &rect, int dy) { updateLineNumberArea(rect, dy); });
        connect(this, &QPlainTextEdit::cursorPositionChanged, this,
                [this] { highlightCurrentLine(); });

        updateLineNumberAreaWidth();
        highlightCurrentLine();
    }

    int lineNumberAreaWidth() const
    {
        int digits = 1;
        int max = qMax(1, blockCount());
        while (max >= 10) {
            max /= 10;
            ++digits;
        }
        return qMax(42, 14 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits);
    }

    // Set by MainWindow for repository files: renders the preview the way the
    // overview README is rendered (raw HTML flattened, relative images, heading
    // anchors) and routes its links back into the repository.
    std::function<void(QTextBrowser *, const QString &)> markdownRenderer;
    std::function<void(QTextBrowser *, const QUrl &)> markdownLinkHandler;

    // Hidden-flag based rather than isVisible(), so a preview switched on for a
    // tab that is not on screen yet still reports as on.
    bool markdownPreviewVisible() const
    {
        return m_markdownPreview && !m_markdownPreview->isHidden();
    }

    QTextBrowser *markdownPreview() const { return m_markdownPreview; }

    void setMarkdownPreviewVisible(bool on)
    {
        if (!on) {
            if (m_markdownPreview)
                m_markdownPreview->hide();
            return;
        }
        if (!m_markdownPreview) {
            m_markdownPreview = new QTextBrowser(this);
            m_markdownPreview->setObjectName("markdownPreview");
            // A relative README link would otherwise navigate the browser
            // itself to a path it cannot load and blank the preview.
            m_markdownPreview->setOpenLinks(false);
            m_markdownPreview->setFrameShape(QFrame::NoFrame);
            connect(m_markdownPreview, &QTextBrowser::anchorClicked, this,
                    [this](const QUrl &url) {
                        if (markdownLinkHandler) {
                            markdownLinkHandler(m_markdownPreview, url);
                            return;
                        }
                        const QString scheme = url.scheme().toLower();
                        if (scheme.isEmpty() && url.path().isEmpty() &&
                            url.hasFragment())
                            m_markdownPreview->scrollToAnchor(url.fragment());
                        else if (scheme == QLatin1String("http") ||
                                 scheme == QLatin1String("https") ||
                                 scheme == QLatin1String("mailto"))
                            QDesktopServices::openUrl(url);
                    });
        }
        m_markdownPreview->setGeometry(contentsRect());
        m_markdownPreview->show();
        m_markdownPreview->raise();
        if (markdownRenderer)
            markdownRenderer(m_markdownPreview, toPlainText());
        else
            m_markdownPreview->setMarkdown(toPlainText());
    }

    // Highlights that outlive a keystroke: the find bar's matches and the
    // "highlight every occurrence" marks (adhoc #1607). They have to be held
    // rather than pushed straight into setExtraSelections(), because moving the
    // cursor re-runs highlightCurrentLine() and that rebuilds the whole list.
    void setPersistentSelections(const QList<QTextEdit::ExtraSelection> &selections)
    {
        m_persistent = selections;
        highlightCurrentLine();
    }

    void clearPersistentSelections()
    {
        if (m_persistent.isEmpty())
            return;
        m_persistent.clear();
        highlightCurrentLine();
    }

    bool hasPersistentSelections() const { return !m_persistent.isEmpty(); }

    void lineNumberAreaPaintEvent(QPaintEvent *event)
    {
        const bool dark = currentThemeIsDark();
        QPainter painter(m_lineNumberArea);
        painter.fillRect(event->rect(),
                         Theme::nextOr(Theme::Role::Bg,
                                       Theme::remapColor(
                                           QColor(dark ? "#0d1117" : "#f6f8fa"))));
        painter.setPen(Theme::remapColor(QColor(dark ? "#6e7681" : "#8c959f")));

        QTextBlock block = firstVisibleBlock();
        int blockNumber = block.blockNumber();
        int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
        int bottom = top + qRound(blockBoundingRect(block).height());
        const int rightPadding = 10;

        while (block.isValid() && top <= event->rect().bottom()) {
            if (block.isVisible() && bottom >= event->rect().top()) {
                const QString number = QString::number(blockNumber + 1);
                painter.drawText(0, top, m_lineNumberArea->width() - rightPadding,
                                 fontMetrics().height(), Qt::AlignRight, number);
            }
            block = block.next();
            top = bottom;
            bottom = top + qRound(blockBoundingRect(block).height());
            ++blockNumber;
        }
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QPlainTextEdit::resizeEvent(event);
        const QRect cr = contentsRect();
        m_lineNumberArea->setGeometry(
            QRect(cr.left(), cr.top(), lineNumberAreaWidth(), cr.height()));
        if (m_markdownPreview && m_markdownPreview->isVisible())
            m_markdownPreview->setGeometry(cr);
    }

    void changeEvent(QEvent *event) override
    {
        QPlainTextEdit::changeEvent(event);
        if (event->type() == QEvent::PaletteChange ||
            event->type() == QEvent::ApplicationPaletteChange ||
            event->type() == QEvent::StyleChange) {
            highlightCurrentLine();
            m_lineNumberArea->update();
        }
    }

private:
    void updateLineNumberAreaWidth()
    {
        setViewportMargins(lineNumberAreaWidth(), 0, 0, 0);
    }

    void updateLineNumberArea(const QRect &rect, int dy)
    {
        if (dy)
            m_lineNumberArea->scroll(0, dy);
        else
            m_lineNumberArea->update(0, rect.y(), m_lineNumberArea->width(),
                                     rect.height());
        if (rect.contains(viewport()->rect()))
            updateLineNumberAreaWidth();
    }

    void highlightCurrentLine()
    {
        QList<QTextEdit::ExtraSelection> selections;
        QTextEdit::ExtraSelection selection;
        selection.format.setBackground(Theme::nextOr(
            Theme::Role::SurfaceRaised,
            QColor(currentThemeIsDark() ? "#161b22" : "#f6f8fa")));
        selection.format.setProperty(QTextFormat::FullWidthSelection, true);
        selection.cursor = textCursor();
        selection.cursor.clearSelection();
        selections.append(selection);
        // The current-line wash goes underneath, so a match sitting on the
        // cursor's line still reads as a match.
        selections += m_persistent;
        setExtraSelections(selections);
    }

    CodeLineNumberArea *m_lineNumberArea = nullptr;
    QTextBrowser *m_markdownPreview = nullptr; // lazy rendered-markdown overlay
    QList<QTextEdit::ExtraSelection> m_persistent;
};

inline CodeLineNumberArea::CodeLineNumberArea(CodePreviewEditor *editor)
    : QWidget(editor), m_editor(editor)
{
    setObjectName("codeLineNumberArea");
}

inline QSize CodeLineNumberArea::sizeHint() const
{
    return QSize(m_editor ? m_editor->lineNumberAreaWidth() : 0, 0);
}

inline void CodeLineNumberArea::paintEvent(QPaintEvent *event)
{
    if (m_editor)
        m_editor->lineNumberAreaPaintEvent(event);
}


QPixmap tintedOcticonPixmap(const QString &name, const QColor &color, int size);

QIcon themedOcticon(const QString &name, const QColor &color, int size);

inline constexpr int kAgentSpinTickMs = 60;
// While the animated table is not on screen the spin timers fall back to this
// cadence: cheap enough to leave armed, frequent enough that the animation is
// back within a blink of the table being shown again.
inline constexpr int kAgentSpinHiddenTickMs = 500;

// Runs at the top of an animation tick: returns true when the view is visible
// and the timer is at animation rate; otherwise idles the timer down (or back
// up) and tells the caller to skip this frame's per-row work.
bool spinTimerTickAllowed(QTimer *timer, const QWidget *view);
// The status glyph still animates at 60 ms, but the detail header's process
// snapshot is deliberately much slower.  Besides avoiding needless rich-text
// relayout, this keeps Linux /proc traversal out of an animation-rate path.
inline constexpr int kAgentProcessMetaCacheMs = 2000;
inline constexpr int kAgentDetailMetaRefreshTicks =
    (kAgentProcessMetaCacheMs + kAgentSpinTickMs - 1) / kAgentSpinTickMs;
inline constexpr bool agentDetailMetaRefreshDue(int spinTick)
{
    return spinTick > 0 && spinTick % kAgentDetailMetaRefreshTicks == 0;
}

// Animated glyphs are asked for a frame every kAgentSpinTickMs per running
// row. Snapping the angle to whole degrees bounds the distinct frames to 360
// per (glyph, size) so the rotated pixmap is painted once and then served
// from memory — at 60 ms a tick a 1-degree step is well under what the eye can
// tell apart from the continuous angle.
int quantizedSpinAngle(qreal angleDeg);

QPixmap rotatedTintedOcticonPixmap(const QString &name, const QColor &color,
                                   int size, qreal angleDeg);

// A hot violet head circles clockwise while progressively smaller embers mark
// where it was.  Callers keep supplying their existing angle/timer cadence;
// this helper is only the shared artwork for the row and detail-header spinners.
QPixmap agentRunningFireballPixmap(int size, qreal angleDeg);

QPixmap nodeStatusLightPixmap(const QColor &color, int size, qreal angleDeg,
                                     bool spinning);

QString octiconForBackgroundTaskWord(const QString &word);

QString octiconForBackgroundTaskWord(const QString &word);

class BackgroundTaskChip : public QWidget
{
public:
    explicit BackgroundTaskChip(const QString &word, QWidget *parent = nullptr)
        : QWidget(parent), m_word(word)
    {
        setFixedHeight(kIconSize);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        setFocusPolicy(Qt::NoFocus);
        setProperty("processKind", m_word);
        setProperty("processCount", m_count);
        setAccessibleName(QStringLiteral("%1 background work").arg(m_word));
        updateAccessibleDescription();
    }

    QString word() const { return m_word; }

    void setCount(int count)
    {
        count = qMax(1, count);
        if (m_count == count)
            return;
        m_count = count;
        setProperty("processCount", m_count);
        updateAccessibleDescription();
        updateGeometry();
        update();
    }

    void setAngle(qreal degrees)
    {
        m_angle = degrees;
        update();
    }

    static QString octiconForWord(const QString &word)
    {
        return octiconForBackgroundTaskWord(word);
    }

    QSize sizeHint() const override
    {
        return QSize(kIconSize + kTextGap +
                         fontMetrics().horizontalAdvance(displayText()),
                     kIconSize);
    }

    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const QColor accent(QString::fromLatin1(Theme::kRunning));

        const int segments = qBound(1, m_count, kMaxSegments);
        const qreal inset = kPenWidth / 2.0 + 0.5;
        const QRectF ring(inset, inset, kIconSize - inset * 2.0,
                          kIconSize - inset * 2.0);
        const int span = 5760 / segments;
        const int gap = segments == 1 ? 1440 : qMax(320, span / 3);
        QPen pen(accent, kPenWidth);
        pen.setCapStyle(Qt::FlatCap);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        for (int i = 0; i < segments; ++i)
            p.drawArc(ring, int(-m_angle * 16) + i * span, span - gap);

        const QPixmap glyph = tintedOcticonPixmap(
            octiconForWord(m_word),
            currentThemeIsDark() ? Theme::remapColor(QColor("#c9d1d9")) : Theme::remapColor(QColor("#57606a")),
            kGlyphSize);
        p.drawPixmap(QPointF((kIconSize - kGlyphSize) / 2.0,
                             (height() - kGlyphSize) / 2.0),
                     glyph);
        p.setPen(currentThemeIsDark() ? Theme::remapColor(QColor("#8b949e"))
                                      : Theme::remapColor(QColor("#656d76")));
        p.drawText(QRect(kIconSize + kTextGap, 0,
                         width() - kIconSize - kTextGap, height()),
                   Qt::AlignLeft | Qt::AlignVCenter, displayText());
    }

private:
    QString displayText() const
    {
        if (m_count == 1)
            return m_word;
        return QStringLiteral("%1 %2%3")
            .arg(m_word)
            .arg(QChar(0x00D7))
            .arg(m_count);
    }

    void updateAccessibleDescription()
    {
        setProperty("processText", displayText());
        setAccessibleDescription(
            QStringLiteral("%1 %2 in progress: %3")
                .arg(m_count)
                .arg(m_count == 1 ? QStringLiteral("process")
                                  : QStringLiteral("processes"),
                     displayText()));
    }

    static constexpr int kIconSize = 18;
    static constexpr int kGlyphSize = 10;
    static constexpr int kTextGap = 4;
    static constexpr qreal kPenWidth = 1.3;
    static constexpr int kMaxSegments = 8;
    QString m_word;
    int m_count = 1;
    qreal m_angle = 0.0;
};

void applyStoredOcticon(QPushButton *button);

void setOcticon(QPushButton *button, const QString &name, int size = 16,
                       qreal rotationDeg = 0.0);

void tintComposerActionButton(QPushButton *button);

void applyStoredLabelOcticon(QLabel *label);

void setLabelOcticon(QLabel *label, const QString &name, int size = 12);

void setTabOcticon(QTabWidget *tabs, int index, const QString &name,
                          int size = 14);

void refreshTabOcticons(QTabWidget *tabs);

class ColumnFlowWidget : public QWidget
{
public:
    explicit ColumnFlowWidget(QWidget *parent = nullptr) : QWidget(parent)
    {
        m_row = new QHBoxLayout(this);
        m_row->setContentsMargins(0, 0, 0, 0);
        m_row->setSpacing(kColumnGap);
    }

    void setMinimumColumnWidth(int px)
    {
        m_minColumnWidth = qMax(120, px);
        reflow(true);
    }
    void setMaximumColumns(int columns)
    {
        m_maxColumns = qMax(1, columns);
        reflow(true);
    }

    QVBoxLayout *addSection()
    {
        auto *card = new QWidget(this);
        auto *body = new QVBoxLayout(card);
        body->setContentsMargins(0, 0, 0, 0);
        body->setSpacing(8);
        sectionCardHook(card);
        m_sections.append(card);
        m_pending = true;
        return body;
    }

    QSize minimumSizeHint() const override
    {
        QSize hint = QWidget::minimumSizeHint();
        hint.setWidth(qMin(hint.width(), m_minColumnWidth));
        return hint;
    }

    int columnCount() const { return m_columnCount; }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        reflow(false);
    }
    void showEvent(QShowEvent *event) override
    {
        QWidget::showEvent(event);
        reflow(false);
    }

private:
    static constexpr int kColumnGap = 22;

    int columnsForWidth() const
    {
        int best = 1;
        for (int n = 2; n <= qMin(m_maxColumns, int(m_sections.size())); ++n) {
            if (width() >= n * m_minColumnWidth + (n - 1) * kColumnGap)
                best = n;
        }
        return best;
    }

    void reflow(bool force)
    {
        const int columns = columnsForWidth();
        if (!force && !m_pending && columns == m_columnCount)
            return;
        m_columnCount = columns;
        m_pending = false;

        for (QVBoxLayout *column : std::as_const(m_columns)) {
            while (QLayoutItem *item = column->takeAt(0))
                delete item;
        }
        qDeleteAll(m_columns);
        m_columns.clear();
        while (QLayoutItem *item = m_row->takeAt(0))
            delete item;

        for (int i = 0; i < columns; ++i) {
            auto *column = new QVBoxLayout;
            column->setContentsMargins(0, 0, 0, 0);
            column->setSpacing(14);
            m_columns.append(column);
            m_row->addLayout(column, 1);
        }

        QList<int> filled(columns, 0);
        for (QWidget *section : std::as_const(m_sections)) {
            int target = 0;
            for (int i = 1; i < columns; ++i) {
                if (filled.at(i) < filled.at(target))
                    target = i;
            }
            m_columns.at(target)->addWidget(section);
            filled[target] += qMax(1, section->sizeHint().height());
        }
        for (QVBoxLayout *column : std::as_const(m_columns))
            column->addStretch(1);
    }

    QHBoxLayout *m_row = nullptr;
    QList<QWidget *> m_sections;
    QList<QVBoxLayout *> m_columns;
    int m_columnCount = 0;
    int m_minColumnWidth = 300;
    int m_maxColumns = 3;
    bool m_pending = false;
};

class ElidingPushButton : public QPushButton
{
public:
    using QPushButton::QPushButton;

    void setFullText(const QString &text)
    {
        m_fullText = text;
        applyElide();
    }
    QString fullText() const { return m_fullText; }

    QSize sizeHint() const override
    {
        QSize hint = QPushButton::sizeHint();
        hint.setWidth(qBound(kMinWidth,
                             fontMetrics().horizontalAdvance(m_fullText) +
                                 chromeWidth(),
                             kMaxWidth));
        return hint;
    }
    QSize minimumSizeHint() const override
    {
        QSize hint = QPushButton::minimumSizeHint();
        hint.setWidth(qMin(hint.width(), kMinWidth));
        return hint;
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QPushButton::resizeEvent(event);
        applyElide();
    }

private:
    static constexpr int kMinWidth = 56;  // still shows a few characters
    static constexpr int kMaxWidth = 240; // long refs stop growing the row here

    int chromeWidth() const
    {
        return 28 + (icon().isNull() ? 0 : iconSize().width() + 6) +
               (menu() ? 14 : 0);
    }

    void applyElide()
    {
        const QString elided = fontMetrics().elidedText(
            m_fullText, Qt::ElideMiddle, qMax(0, width() - chromeWidth()));
        if (elided != text())
            QPushButton::setText(elided);
    }

    QString m_fullText;
};

class StackedIconButton : public QPushButton
{
public:
    using QPushButton::QPushButton;

    void setGlyph(const QString &glyph)
    {
        m_glyph = glyph;
        updateGeometry();
        update();
    }

    QSize sizeHint() const override
    {
        const QSize slot = iconSlotSize();
        const QFontMetrics fm(captionFont());
        const bool hasCaption = !text().isEmpty();
        const int captionW = hasCaption ? fm.horizontalAdvance(text()) + 2 : 0;
        const int captionH = hasCaption ? fm.height() + kGap : 0;
        return QSize(qMax(slot.width(), captionW) + 2 * kPadH,
                     slot.height() + captionH + 2 * kPadV);
    }
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    bool event(QEvent *e) override
    {
        switch (e->type()) {
        case QEvent::FontChange:
        case QEvent::StyleChange:
        case QEvent::ApplicationFontChange:
            updateGeometry();
            break;
        default:
            break;
        }
        return QPushButton::event(e);
    }

    void paintEvent(QPaintEvent *) override
    {
        QStylePainter painter(this);
        QStyleOptionButton opt;
        initStyleOption(&opt);
        const QIcon glyphIcon = opt.icon;
        opt.text.clear();
        opt.icon = QIcon();
        opt.iconSize = QSize();
        painter.drawControl(QStyle::CE_PushButton, opt);

        const QSize slot = iconSlotSize();
        const QFont caption = captionFont();
        const QFontMetrics fm(caption);
        const bool hasCaption = !text().isEmpty();
        const int captionH = hasCaption ? fm.height() : 0;
        const int blockH = slot.height() + (hasCaption ? kGap + captionH : 0);
        int top = rect().top() + (height() - blockH) / 2;

        const QRect slotRect((width() - slot.width()) / 2, top, slot.width(),
                             slot.height());
        if (!glyphIcon.isNull()) {
            glyphIcon.paint(&painter, slotRect, Qt::AlignCenter,
                            isEnabled() ? QIcon::Normal : QIcon::Disabled,
                            QIcon::Off);
        } else if (!m_glyph.isEmpty()) {
            QFont glyphFont = font();
            glyphFont.setBold(true);
            if (glyphFont.pixelSize() > 0)
                glyphFont.setPixelSize(slot.height());
            else
                glyphFont.setPointSize(qMax(9, slot.height() - 3));
            painter.setFont(glyphFont);
            painter.setPen(labelColor(opt));
            painter.drawText(slotRect, Qt::AlignCenter, m_glyph);
        }

        if (!hasCaption)
            return;
        const QRect captionRect(kPadH, top + slot.height() + kGap,
                                qMax(0, width() - 2 * kPadH), captionH);
        painter.setFont(caption);
        painter.setPen(labelColor(opt));
        painter.drawText(captionRect, Qt::AlignHCenter | Qt::AlignVCenter,
                         fm.elidedText(text(), Qt::ElideRight, captionRect.width()));
    }

private:
    static constexpr int kPadH = 8;
    static constexpr int kPadV = 4;
    static constexpr int kGap = 3; // between the glyph and its caption

    QSize iconSlotSize() const
    {
        if (!icon().isNull() && iconSize().isValid())
            return iconSize();
        return QSize(16, 16);
    }

    QColor labelColor(const QStyleOptionButton &opt) const
    {
        return opt.palette.color(isEnabled() ? QPalette::Active
                                             : QPalette::Disabled,
                                 QPalette::ButtonText);
    }

    QFont captionFont() const
    {
        QFont f = font();
        if (f.pixelSize() > 0)
            f.setPixelSize(qMax(9, f.pixelSize() - 4));
        else
            f.setPointSize(qMax(7, f.pointSize() - 3));
        f.setWeight(QFont::DemiBold);
        return f;
    }

    QString m_glyph;
};

class ElidingStatusLabel : public QLabel
{
public:
    explicit ElidingStatusLabel(QWidget *parent = nullptr) : QLabel(parent)
    {
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        setTextInteractionFlags(Qt::NoTextInteraction);
    }

    void setFullText(const QString &text)
    {
        const QString line = text.simplified();
        if (line == m_fullText)
            return;
        m_fullText = line;
        setToolTip(line);
        applyElide();
    }
    QString fullText() const { return m_fullText; }

    // A file path is worth eliding from the middle: the trailing file name
    // identifies it, and the leading root says which store it came from.
    void setElideMode(Qt::TextElideMode mode)
    {
        if (mode == m_elideMode)
            return;
        m_elideMode = mode;
        applyElide();
    }

    QSize minimumSizeHint() const override
    {
        QSize hint = QLabel::minimumSizeHint();
        hint.setWidth(0);
        return hint;
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QLabel::resizeEvent(event);
        applyElide();
    }

private:
    void applyElide()
    {
        const QString elided = fontMetrics().elidedText(
            m_fullText, m_elideMode, qMax(0, width()));
        if (elided != text())
            QLabel::setText(elided);
    }

    QString m_fullText;
    Qt::TextElideMode m_elideMode = Qt::ElideRight;
};

class VerticalIconButton : public QPushButton
{
public:
    enum Form { Action, Tab, Bare };
    explicit VerticalIconButton(const QString &text, Form form,
                                QWidget *parent = nullptr)
        : QPushButton(text, parent), m_form(form)
    {
        setCursor(Qt::PointingHandCursor);
        setFlat(true);
        setAttribute(Qt::WA_Hover, true);
    }

    QSize sizeHint() const override
    {
        const QSize hooked = tabButtonSizeHook(this, paintState());
        if (hooked.isValid())
            return hooked;
        QFont f = font();
        f.setPixelSize(10);
        f.setWeight(QFont::DemiBold);
        const int textW = QFontMetrics(f).horizontalAdvance(text());
        const int badgesW = badgeStripWidth() + kIconPx / 2;
        return QSize(qMax(44, qMax(qMax(kIconPx, textW), badgesW) + 16),
                     tileHeight());
    }
    QSize minimumSizeHint() const override { return sizeHint(); }

    // Lift the count clear of the glyph. A five-digit tally painted over the
    // icon (the log's category chips reach 20,000) hides the very thing that
    // says which category it counts, so those tiles give the badge its own
    // band above the icon instead of overlapping it.
    void setBadgeAboveIcon(bool above)
    {
        if (m_badgeAbove == above)
            return;
        m_badgeAbove = above;
        updateGeometry();
        update();
    }

    void setBadgeCount(qint64 count)
    {
        if (m_badge == count)
            return;
        m_badge = count;
        updateGeometry(); // a longer count can need a wider tile
        update();
    }
    qint64 badgeCount() const { return m_badge; }

    void setAlertBadgeCount(qint64 count)
    {
        count = qMax<qint64>(0, count);
        if (m_alertBadge == count)
            return;
        m_alertBadge = count;
        updateGeometry();
        update();
    }
    qint64 alertBadgeCount() const { return m_alertBadge; }

    // A small live-work light for tabs whose contents need attention. Unlike
    // an alert badge this is not an error or a count; it simply pulses while
    // the underlying work is active.
    void setAttentionLight(bool active)
    {
        if (m_attentionLight == active)
            return;
        m_attentionLight = active;
        m_attentionLightOn = active;
        if (active) {
            if (!m_attentionTimer) {
                m_attentionTimer = new QTimer(this);
                m_attentionTimer->setInterval(650);
                QObject::connect(m_attentionTimer, &QTimer::timeout, this,
                                 [this] {
                                     m_attentionLightOn = !m_attentionLightOn;
                                     update();
                                 });
            }
            m_attentionTimer->start();
        } else if (m_attentionTimer) {
            m_attentionTimer->stop();
        }
        update();
    }
    bool hasAttentionLight() const { return m_attentionLight; }

    void setOcticonName(const QString &name)
    {
        if (m_iconName == name)
            return;
        m_iconName = name;
        update();
    }

    void setAccentColor(const QColor &accent)
    {
        if (m_accent == accent)
            return;
        m_accent = accent;
        update();
    }
    QColor accentColor() const { return m_accent; }

    void setInRail(bool inRail)
    {
        if (m_inRail == inRail)
            return;
        m_inRail = inRail;
        update();
    }
    bool inRail() const { return m_inRail; }

    void setFooterWidget(QWidget *footer)
    {
        if (m_footer == footer)
            return;
        if (m_footer)
            m_footer->deleteLater();
        m_footer = footer;
        if (footer) {
            footer->setParent(this);
            footer->show();
        }
        updateGeometry();
        placeFooter();
    }
    QWidget *footerWidget() const { return m_footer; }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QPushButton::resizeEvent(event);
        placeFooter();
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        if (paintTabButtonHook(this, p, paintState()))
            return;
        const bool dark = currentThemeIsDark();
        const bool hovered = isEnabled() && underMouse();
        QColor fg;
        if (m_form == Action)
            fg = dark ? Theme::remapColor(QColor("#e6edf3")) : Theme::remapColor(QColor("#1f2328"));
        else
            fg = dark ? QColor(isChecked() || hovered ? "#e6edf3" : "#8b949e")
                      : QColor(isChecked() || hovered ? "#1f2328" : "#656d76");
        if (!isEnabled())
            fg = Theme::remapColor(QColor("#6e7681"));
        const bool accented = m_accent.isValid() && isEnabled();
        const QColor glyphColor = accented ? m_accent : fg;
        const QColor captionColor =
            accented && (isChecked() || hovered) ? m_accent : fg;

        if (m_form == Action) {
            if (hovered) {
                p.setPen(Qt::NoPen);
                p.setBrush(Theme::remapColor(QColor(dark ? "#21262d" : "#eaeef2")));
                p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5),
                                  6, 6);
            }
        } else if (m_form == Tab && isChecked()) {
            const QColor bar = accented ? m_accent
                                        : QColor(dark ? "#2ea043" : "#1f883d");
            if (m_inRail)
                p.fillRect(QRectF(0, 4, 2, height() - 8), bar);
            else
                p.fillRect(QRect(0, height() - 2, width(), 2), bar);
        }

        const QRect iconRect((width() - kIconPx) / 2, iconTop(), kIconPx,
                             kIconPx);
        if (m_iconName.isEmpty())
            icon().paint(&p, iconRect, Qt::AlignCenter,
                         isEnabled() ? QIcon::Normal : QIcon::Disabled);
        else
            p.drawPixmap(iconRect.topLeft(),
                         tintedOcticonPixmap(m_iconName, glyphColor, kIconPx));

        if (m_attentionLight) {
            QColor light(QStringLiteral("#58a6ff"));
            light.setAlpha(m_attentionLightOn ? 255 : 85);
            p.setPen(Qt::NoPen);
            p.setBrush(light);
            p.drawEllipse(QRectF(iconRect.right() - 2, iconRect.top() - 2,
                                 6, 6));
        }

        QFont f = font();
        f.setPixelSize(10);
        f.setWeight(QFont::DemiBold);
        p.setFont(f);
        p.setPen(captionColor);
        p.drawText(QRect(2, iconRect.bottom() + 2, width() - 4, 14),
                   Qt::AlignHCenter | Qt::AlignTop,
                   QFontMetrics(f).elidedText(text(), Qt::ElideRight,
                                              width() - 4));

        // Hanging off the icon's right shoulder normally; centred over the
        // tile once the badges have a band of their own, so a wide count reads
        // as this tile's number rather than drifting towards the next one.
        double rightEdge = m_badgeAbove
                               ? (width() + badgeStripWidth()) / 2.0
                               : iconRect.right() + kBadgeH / 2.0 + 2;
        const auto paintBadge = [&](qint64 count, const QColor &fill) {
            if (count <= 0)
                return;
            const QString badgeText = formatCount(count);
            QFont bf = font();
            bf.setPixelSize(9);
            bf.setBold(true);
            p.setFont(bf);
            const int h = kBadgeH;
            const int w = badgeWidth(count);
            const QRectF badge(qMax(0.0, rightEdge - w),
                               m_badgeAbove
                                   ? 0.0
                                   : qMax(0.0, double(iconRect.top() - 5)),
                               w, h);
            p.setPen(Qt::NoPen);
            p.setBrush(fill);
            p.drawRoundedRect(badge, h / 2.0, h / 2.0);
            p.setPen(fill.lightness() > 155 ? Theme::remapColor(QColor("#0d1117"))
                                            : QColor("#ffffff"));
            p.drawText(badge, Qt::AlignCenter, badgeText);
            rightEdge = badge.left() - 2;
        };
        paintBadge(m_badge, accented ? m_accent : QColor("#1f6feb"));
        paintBadge(m_alertBadge, QColor(dark ? "#da3633" : "#cf222e"));
    }

private:
    TabButtonPaintState paintState() const
    {
        TabButtonPaintState state;
        state.form = int(m_form);
        state.badge = m_badge;
        state.alertBadge = m_alertBadge;
        state.badgeAbove = m_badgeAbove;
        state.attentionLight = m_attentionLight;
        state.attentionLightOn = m_attentionLightOn;
        state.inRail = m_inRail;
        state.iconName = m_iconName;
        state.accent = m_accent;
        return state;
    }

    int badgeWidth(qint64 count) const
    {
        if (count <= 0)
            return 0;
        QFont bf = font();
        bf.setPixelSize(9);
        bf.setBold(true);
        return qMax(kBadgeH,
                    QFontMetrics(bf).horizontalAdvance(formatCount(count)) + 8);
    }
    int badgeStripWidth() const
    {
        const int blue = badgeWidth(m_badge);
        const int red = badgeWidth(m_alertBadge);
        return blue + red + (blue > 0 && red > 0 ? 2 : 0);
    }
    // The badge band pushes the glyph down by its own height, and the tile
    // grows by the same amount so the caption keeps its room underneath.
    int iconTop() const { return m_badgeAbove ? kBadgeH + 1 : 6; }
    int bodyHeight() const { return m_badgeAbove ? kHeight + kBadgeH - 8 : kHeight; }
    int footerHeight() const
    {
        return m_footer ? m_footer->sizeHint().height() : 0;
    }
    int tileHeight() const { return bodyHeight() + footerHeight(); }
    void placeFooter()
    {
        if (!m_footer)
            return;
        const QSize size = m_footer->sizeHint();
        m_footer->setGeometry((width() - size.width()) / 2, bodyHeight() - 2,
                              size.width(), size.height());
    }

    static constexpr int kIconPx = 16;
    static constexpr int kHeight = 44;
    static constexpr int kBadgeH = 14; // badge height and rounded-end radius
    Form m_form;
    qint64 m_badge = 0;
    qint64 m_alertBadge = 0;
    bool m_badgeAbove = false; // badges in their own band, clear of the glyph
    bool m_attentionLight = false;
    bool m_attentionLightOn = false;
    bool m_inRail = false;
    QTimer *m_attentionTimer = nullptr;
    QPointer<QWidget> m_footer;
    QString m_iconName; // empty: paint the QIcon set by setOcticon instead
    QColor m_accent;    // invalid: the rail's neutral grey/hover colours
};

class RatchetToggleButton : public QToolButton
{
public:
    explicit RatchetToggleButton(QWidget *parent = nullptr)
        : QToolButton(parent)
    {
        setText(QStringLiteral("Ratchet"));
        setCheckable(true);
        setCursor(Qt::PointingHandCursor);
    }

    QSize sizeHint() const override
    {
        QFont f = font();
        f.setPixelSize(10);
        f.setWeight(QFont::DemiBold);
        const int textW = QFontMetrics(f).horizontalAdvance(text());
        return QSize(qMax(48, qMax(kIconPx, textW) + 16), kHeight);
    }
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const bool dark = currentThemeIsDark();
        const bool hovered = isEnabled() && underMouse();
        const bool on = isChecked();

        const QColor accent =
            nextAccentTint(QColor(dark ? "#2ea043" : "#1f883d"));
        QRectF pill = QRectF(rect()).adjusted(0.5, 3.5, -0.5, -3.5);
        if (on) {
            QColor fill = accent;
            if (hovered)
                fill = fill.lighter(112);
            p.setPen(QPen(accent.darker(115), 1));
            p.setBrush(fill);
        } else {
            p.setPen(QPen(Theme::remapColor(QColor(dark ? "#30363d" : "#d0d7de")), 1));
            p.setBrush(hovered ? Theme::remapColor(QColor(dark ? "#21262d" : "#eaeef2"))
                               : QColor(Qt::transparent));
        }
        p.drawRoundedRect(pill, 6, 6);

        const QColor fg =
            !isEnabled() ? Theme::remapColor(QColor("#6e7681"))
            : on         ? QColor("#ffffff")
                         : (dark ? Theme::remapColor(QColor(hovered ? "#e6edf3" : "#8b949e"))
                                 : Theme::remapColor(QColor(hovered ? "#1f2328" : "#656d76")));
        const QRect iconRect((width() - kIconPx) / 2, int(pill.top()) + 5,
                             kIconPx, kIconPx);
        p.drawPixmap(iconRect.topLeft(),
                     tintedOcticonPixmap(on ? QStringLiteral("lock")
                                            : QStringLiteral("unlock"),
                                         fg, kIconPx));

        QFont f = font();
        f.setPixelSize(10);
        f.setWeight(QFont::DemiBold);
        p.setFont(f);
        p.setPen(fg);
        p.drawText(QRect(2, iconRect.bottom() + 2, width() - 4, 12),
                   Qt::AlignHCenter | Qt::AlignTop,
                   QFontMetrics(f).elidedText(text(), Qt::ElideRight,
                                              width() - 4));
    }
    void enterEvent(QEnterEvent *e) override
    {
        update();
        QToolButton::enterEvent(e);
    }
    void leaveEvent(QEvent *e) override
    {
        update();
        QToolButton::leaveEvent(e);
    }

private:
    static constexpr int kIconPx = 16;
    static constexpr int kHeight = 44;
};

constexpr int kRailItemWidth = 42;
constexpr int kRailItemHeight = 44; // 16px icon + 10px caption + breathing room
constexpr int kRailItemHeightNoCaption = 30; // icon only, no caption line
constexpr int kRailIconPx = 16;
constexpr int kRepoTabRowTopInset = 4;

// Settings → Appearance can switch the rail captions off. Every rail metric
// reads this one flag, which is seeded from QSettings on first use and then
// held in memory so a live toggle takes effect without a restart.
bool &railCaptionsHiddenState();
bool railCaptionsHidden();
void setRailCaptionsHidden(bool hidden);

// The width the rail needs when it does paint captions: the widest caption at
// the caption font, measured once. Kept separate from railItemWidth() so the
// measurement survives toggling captions off and back on.
int railCaptionItemWidth();

int railItemWidth();
int railWidth(); // + slim scrollbar

QFont navBalanceFont();

int navBalanceLineHeight();

class ActivityRailButton : public QPushButton
{
public:
    explicit ActivityRailButton(const QString &iconName, const QString &label,
                                QWidget *parent = nullptr)
        : QPushButton(label, parent), m_iconName(iconName), m_label(label)
    {
        setCheckable(true);
        setCursor(Qt::PointingHandCursor);
        setFlat(true);
        setAccessibleName(m_label);
        setFixedSize(railButtonWidth(), railButtonHeight());
        m_spinTimer = new QTimer(this);
        m_spinTimer->setInterval(60);
        connect(m_spinTimer, &QTimer::timeout, this, [this] {
            m_spinAngle = (m_spinAngle + 30) % 360;
            update();
        });
        m_blinkTimer = new QTimer(this);
        m_blinkTimer->setInterval(550);
        connect(m_blinkTimer, &QTimer::timeout, this, [this] {
            m_blinkOn = !m_blinkOn;
            update();
        });
        m_desktopOfflineTimer = new QTimer(this);
        m_desktopOfflineTimer->setInterval(80);
        connect(m_desktopOfflineTimer, &QTimer::timeout, this, [this] {
            m_desktopOfflineStep = (m_desktopOfflineStep + 1) % kDesktopOfflineSteps;
            update();
        });
    }

    void setCompact(bool compact)
    {
        if (m_compact == compact)
            return;
        m_compact = compact;
        setFixedSize(railButtonWidth(), railButtonHeight());
        update();
    }

    bool captionVisible() const
    {
        return !m_compact && !m_label.isEmpty() &&
               (m_captionAlwaysVisible || !railCaptionsHidden());
    }

    // Mark this button as one of the left rail's own and re-fit it to the
    // current caption preference. Buttons elsewhere — the agent detail's action
    // row, the debug bar, the top nav — are sized from their own caption and
    // keep it whatever the rail preference says, so they never call this.
    void followRailCaptionPreference()
    {
        m_captionAlwaysVisible = false;
        const QSize wanted(railButtonWidth(), railButtonHeight());
        if (size() == wanted)
            return;
        setFixedSize(wanted);
        update();
    }

    void setGlyph(const QString &iconName, const QString &label)
    {
        if (m_iconName == iconName && m_label == label)
            return;
        m_iconName = iconName;
        m_label = label;
        setText(label);
        setAccessibleName(label);
        update();
    }

    void setBadgeCount(int count)
    {
        if (m_badge == count)
            return;
        m_badge = count;
        update();
        if (m_badgeObserver)
            m_badgeObserver();
        if (m_badgeOrderObserver)
            m_badgeOrderObserver();
    }
    int badgeCount() const { return m_badge; }
    bool badgeUrgent() const { return m_badgeUrgent; }
    QString iconName() const { return m_iconName; }
    QString label() const { return m_label; }
    bool syncing() const { return m_syncing; }
    bool alertTint() const { return m_alert; }
    bool dangerTint() const { return m_danger; }
    void setBadgeObserver(std::function<void()> observer)
    {
        m_badgeObserver = std::move(observer);
    }
    void setBadgeOrderObserver(std::function<void()> observer)
    {
        m_badgeOrderObserver = std::move(observer);
    }

    void setPendingSyncCount(int count)
    {
        count = qMax(0, count);
        if (m_pendingSync == count)
            return;
        m_pendingSync = count;
        update();
    }
    int pendingSyncCount() const { return m_pendingSync; }

    // A tiny glyph riding the top of the button, for controls whose state is
    // a mode rather than on/off: the caller names an octicon and its tint, and
    // clears the marker by passing an empty name.
    void setStateGlyph(const QString &iconName, const QColor &darkColor,
                       const QColor &lightColor)
    {
        if (m_stateGlyph == iconName && m_stateGlyphDark == darkColor &&
            m_stateGlyphLight == lightColor)
            return;
        m_stateGlyph = iconName;
        m_stateGlyphDark = darkColor;
        m_stateGlyphLight = lightColor;
        update();
    }
    QString stateGlyph() const { return m_stateGlyph; }

    void setIconPainter(
        std::function<void(QPainter &, const QRect &, const QColor &)> painter)
    {
        m_iconPainter = std::move(painter);
        update();
    }

    void setBadgeUrgent(bool urgent)
    {
        if (m_badgeUrgent == urgent)
            return;
        m_badgeUrgent = urgent;
        update();
        if (m_badgeObserver)
            m_badgeObserver();
    }

    void setAlertTint(bool alert)
    {
        if (m_alert == alert)
            return;
        m_alert = alert;
        update();
    }

    void setDangerTint(bool danger)
    {
        if (m_danger == danger)
            return;
        m_danger = danger;
        update();
    }

    void setAccentTint(bool accent)
    {
        if (m_accent == accent)
            return;
        m_accent = accent;
        update();
    }

    void setSyncing(bool on)
    {
        if (m_syncing == on)
            return;
        m_syncing = on;
        if (m_syncing && isVisible())
            m_spinTimer->start();
        else
            m_spinTimer->stop();
        update();
    }

    void setActivityBlink(bool on)
    {
        if (m_blink == on)
            return;
        m_blink = on;
        m_blinkOn = true;
        if (m_blink && isVisible())
            m_blinkTimer->start();
        else
            m_blinkTimer->stop();
        update();
    }
    bool activityBlink() const { return m_blink; }

    void setDesktopOffline(bool on)
    {
        if (m_desktopOffline == on)
            return;
        m_desktopOffline = on;
        m_desktopOfflineStep = 0;
        if (m_desktopOffline && isVisible())
            m_desktopOfflineTimer->start();
        else
            m_desktopOfflineTimer->stop();
        update();
    }
    bool desktopOffline() const { return m_desktopOffline; }
    double desktopOfflineLevel() const
    {
        return 0.2 + 0.8 * qAbs(1.0 - 2.0 * m_desktopOfflineStep /
                                          kDesktopOfflineSteps);
    }

protected:
    void showEvent(QShowEvent *e) override
    {
        if (m_syncing)
            m_spinTimer->start();
        if (m_blink)
            m_blinkTimer->start();
        if (m_desktopOffline)
            m_desktopOfflineTimer->start();
        QPushButton::showEvent(e);
    }
    void hideEvent(QHideEvent *e) override
    {
        m_spinTimer->stop();
        m_blinkTimer->stop();
        m_desktopOfflineTimer->stop();
        QPushButton::hideEvent(e);
    }
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        if (paintRailButtonHook(this, p, paintState()))
            return;
        const bool dark = currentThemeIsDark();
        const bool lit = isEnabled() && (isChecked() || underMouse());
        const bool accentAction = isEnabled() && m_accent;
        const QColor fg =
            !isEnabled() ? Theme::remapColor(QColor(dark ? "#484f58" : "#b6bdc4"))
                : accentAction ? QColor("#ffffff")
                : m_danger ? QColor(dark ? (lit ? "#ff7b72" : "#f85149")
                                          : (lit ? "#a40e26" : "#cf222e"))
                : m_alert ? QColor(dark ? (lit ? "#f0b72f" : "#d29922")
                                         : (lit ? "#7d4e00" : "#9a6700"))
                          : (dark ? Theme::remapColor(QColor(lit ? "#e6edf3" : "#8b949e"))
                                  : Theme::remapColor(QColor(lit ? "#1f2328" : "#656d76")));
        const bool showLabel = captionVisible();

        if (accentAction) {
            const QColor fill = dark
                                    ? QColor(lit ? "#2ea043" : "#238636")
                                    : QColor(lit ? "#2ea043" : "#1f883d");
            p.setPen(Qt::NoPen);
            p.setBrush(fill);
            p.drawRoundedRect(QRectF(rect()).adjusted(1, 2, -1, -2), 6, 6);
        } else if (isEnabled() && m_danger) {
            QColor fill(dark ? "#f85149" : "#cf222e");
            fill.setAlpha(lit ? 56 : 32);
            p.setPen(Qt::NoPen);
            p.setBrush(fill);
            p.drawRoundedRect(QRectF(rect()).adjusted(1, 2, -1, -2), 6, 6);
        }

        if (isChecked() && !accentAction)
            p.fillRect(QRectF(0, 4, 2, height() - 8), QColor("#2ea043"));

        const int iconPx = kRailIconPx;
        const QRect iconRect((width() - iconPx) / 2,
                             showLabel ? 6 : (height() - iconPx) / 2,
                             iconPx, iconPx);
        if (m_iconPainter)
            m_iconPainter(p, iconRect, fg);
        else if (property("fmSpinning").toBool() && !icon().isNull())
            icon().paint(&p, iconRect, Qt::AlignCenter, QIcon::Normal, QIcon::Off);
        else
            p.drawPixmap(iconRect.topLeft(),
                         tintedOcticonPixmap(m_iconName, fg, iconPx));

        // Commits waiting to sync: an amber "N↑" pill on the icon's left
        // shoulder, kept clear of the blue changed-files badge (or the sync
        // spinner) on the right; the arrow goes first when the rail is narrow.
        if (m_pendingSync > 0) {
            QFont f = font();
            f.setPixelSize(9);
            f.setBold(true);
            p.setFont(f);
            const QFontMetrics fm(f);
            const int h = 14;
            const auto pillWidth = [&](const QString &text) {
                return qMax(h, fm.horizontalAdvance(text) + 8);
            };
            double rightLimit = width() - 1;
            if (m_syncing)
                rightLimit = iconRect.right() - 14 / 2 + 2;
            else if (m_badge > 0)
                rightLimit = iconRect.right() + h / 2.0 + 1 -
                             pillWidth(QString::number(m_badge));
            const QString count = m_pendingSync > 999
                                      ? QStringLiteral("999+")
                                      : QString::number(m_pendingSync);
            QString text = count + QChar(0x2191);
            const double preferredLeft = iconRect.left() - 9;
            if (qMin(preferredLeft, rightLimit - pillWidth(text)) < 1)
                text = count;
            const int w = pillWidth(text);
            const double left = qMax(1.0, qMin(preferredLeft, rightLimit - w));
            const QRectF pill(left, qMax(0.0, double(iconRect.top() - 5)), w, h);
            p.setPen(QPen(Theme::remapColor(QColor(dark ? "#0d1117" : "#ffffff")), 1));
            p.setBrush(QColor(dark ? "#bb8009" : "#9a6700"));
            p.drawRoundedRect(pill, h / 2.0, h / 2.0);
            p.setPen(QColor("#ffffff"));
            p.drawText(pill, Qt::AlignCenter, text);
        }

        if (showLabel) {
            QFont f = font();
            f.setPixelSize(10);
            f.setWeight(QFont::DemiBold);
            p.setFont(f);
            p.setPen(fg);
            p.drawText(QRect(1, iconRect.bottom() + 2, width() - 2, 14),
                       Qt::AlignHCenter | Qt::AlignTop,
                       QFontMetrics(f).elidedText(m_label, Qt::ElideRight,
                                                  width() - 2));
        }

        if (m_syncing) {
            const int s = 14;
            const QPoint at(iconRect.right() - s / 2 + 4,
                            qMax(0, iconRect.top() - 4));
            p.setPen(Qt::NoPen);
            p.setBrush(Theme::remapColor(QColor(dark ? "#0d1117" : "#ffffff")));
            p.drawEllipse(QRect(at, QSize(s, s)).adjusted(-1, -1, 1, 1));
            p.drawPixmap(at, refreshPixmap(QColor(Theme::kRunning), m_spinAngle, s));
        } else if (m_badge > 0) {
            const QString text = QString::number(m_badge);
            QFont f = font();
            f.setPixelSize(9);
            f.setBold(true);
            p.setFont(f);
            const int h = 14;
            const int w = qMax(h, QFontMetrics(f).horizontalAdvance(text) + 8);
            const QRectF badge(
                iconRect.right() - w + h / 2.0 + 2,
                qMax(0.0, double(iconRect.top() - 5)),
                w, h);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(m_badgeUrgent ? (dark ? "#da3633" : "#cf222e")
                                            : "#1f6feb"));
            p.drawRoundedRect(badge, h / 2.0, h / 2.0);
            p.setPen(QColor("#ffffff"));
            p.drawText(badge, Qt::AlignCenter, text);
        }

        if (!m_stateGlyph.isEmpty()) {
            const int s = 11;
            const QPoint at(qMin(width() - s - 2, iconRect.right() - 3),
                            qMax(0, iconRect.top() - 5));
            p.setPen(Qt::NoPen);
            p.setBrush(Theme::remapColor(QColor(dark ? "#0d1117" : "#ffffff")));
            p.drawEllipse(QRect(at, QSize(s, s)).adjusted(-2, -2, 2, 2));
            const QColor glyphColor = dark ? m_stateGlyphDark : m_stateGlyphLight;
            p.drawPixmap(at,
                         tintedOcticonPixmap(
                             m_stateGlyph,
                             isEnabled() && glyphColor.isValid() ? glyphColor : fg,
                             s));
        }

        if (m_blink && m_blinkOn) {
            const int d = 8;
            const QRect lamp(iconRect.right() - 2, iconRect.bottom() - d + 1,
                             d, d);
            p.setPen(Qt::NoPen);
            p.setBrush(Theme::remapColor(QColor(dark ? "#0d1117" : "#ffffff")));
            p.drawEllipse(lamp.adjusted(-2, -2, 2, 2));
            p.setBrush(QColor("#3fb950"));
            p.drawEllipse(lamp);
        }

        if (m_desktopOffline) {
            const int d = 8;
            const QRect dot(iconRect.left() - d + 2, iconRect.bottom() - d + 1,
                            d, d);
            p.save();
            p.setOpacity(desktopOfflineLevel());
            p.setPen(Qt::NoPen);
            p.setBrush(Theme::remapColor(QColor(dark ? "#0d1117" : "#ffffff")));
            p.drawEllipse(dot.adjusted(-2, -2, 2, 2));
            p.setBrush(QColor(dark ? "#d29922" : "#9a6700"));
            p.drawEllipse(dot);
            p.restore();
        }
    }
    void enterEvent(QEnterEvent *e) override
    {
        update();
        QPushButton::enterEvent(e);
    }
    void leaveEvent(QEvent *e) override
    {
        update();
        QPushButton::leaveEvent(e);
    }

private:
    // Only a rail member ever narrows: everything else keeps the caption-sized
    // width it has always had, whatever the rail preference is.
    RailButtonPaintState paintState() const
    {
        RailButtonPaintState state;
        state.iconName = m_iconName;
        state.label = m_label;
        state.stateGlyph = m_stateGlyph;
        state.stateGlyphDark = m_stateGlyphDark;
        state.stateGlyphLight = m_stateGlyphLight;
        state.badge = m_badge;
        state.pendingSync = m_pendingSync;
        state.spinAngle = m_spinAngle;
        state.badgeUrgent = m_badgeUrgent;
        state.alert = m_alert;
        state.danger = m_danger;
        state.accent = m_accent;
        state.syncing = m_syncing;
        state.compact = m_compact;
        state.captionAlwaysVisible = m_captionAlwaysVisible;
        state.captionVisible = captionVisible();
        state.blink = m_blink;
        state.blinkOn = m_blinkOn;
        state.desktopOffline = m_desktopOffline;
        state.desktopOfflineLevel = desktopOfflineLevel();
        state.iconPainter = m_iconPainter;
        return state;
    }

    int railButtonWidth() const
    {
        const QSize hooked = railButtonSizeHook(this, paintState());
        if (hooked.isValid())
            return hooked.width();
        return (!m_captionAlwaysVisible && railCaptionsHidden())
                   ? kRailItemWidth
                   : railCaptionItemWidth();
    }

    int railButtonHeight() const
    {
        const QSize hooked = railButtonSizeHook(this, paintState());
        if (hooked.isValid())
            return hooked.height();
        if (m_compact)
            return kRailItemHeightNoCaption;
        if (m_label.isEmpty())
            return 40;
        return captionVisible() ? kRailItemHeight : kRailItemHeightNoCaption;
    }

    QString m_iconName;
    QString m_label;
    QString m_stateGlyph;
    QColor m_stateGlyphDark;
    QColor m_stateGlyphLight;
    int m_badge = 0;
    int m_pendingSync = 0;
    bool m_badgeUrgent = false;
    bool m_alert = false;
    bool m_danger = false;
    bool m_accent = false;
    bool m_syncing = false;
    bool m_compact = false;
    bool m_captionAlwaysVisible = true;
    bool m_blink = false;
    bool m_blinkOn = true;
    bool m_desktopOffline = false;
    static constexpr int kDesktopOfflineSteps = 30;
    int m_desktopOfflineStep = 0;
    QTimer *m_spinTimer = nullptr;
    QTimer *m_blinkTimer = nullptr;
    QTimer *m_desktopOfflineTimer = nullptr;
    int m_spinAngle = 0;
    std::function<void()> m_badgeObserver;
    std::function<void()> m_badgeOrderObserver;
    std::function<void(QPainter &, const QRect &, const QColor &)> m_iconPainter;
};

class RailEventHook : public QObject
{
public:
    using Handler = std::function<bool(QObject *, QEvent *)>;
    RailEventHook(Handler handler, QObject *parent)
        : QObject(parent), m_handler(std::move(handler))
    {
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        return m_handler && m_handler(watched, event);
    }

private:
    Handler m_handler;
};

class RailFolderPopout : public QWidget
{
public:
    using QWidget::QWidget;

    void setHighlighted(bool on)
    {
        if (m_highlighted == on)
            return;
        m_highlighted = on;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        if (paintRailFolderPopoutHook(this, p, m_highlighted))
            return;
        const bool dark = currentThemeIsDark();
        const QRectF frame = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        p.setPen(Theme::remapColor(QColor(dark ? "#30363d" : "#d0d7de")));
        p.setBrush(Theme::remapColor(QColor(dark ? "#161b22" : "#ffffff")));
        p.drawRoundedRect(frame, 6, 6);
        if (m_highlighted) {
            p.setPen(Qt::NoPen);
            p.setBrush(dark ? QColor(46, 160, 67, 60) : QColor(31, 136, 61, 40));
            p.drawRoundedRect(frame, 6, 6);
        }
    }

private:
    bool m_highlighted = false;
};

class RailFolderWidget : public QWidget
{
public:
    RailFolderWidget(const QString &id, const QString &name,
                     QWidget *parent = nullptr)
        : QWidget(parent), m_id(id)
    {
        setObjectName(QStringLiteral("railFolder"));
        setAcceptDrops(true);
        setProperty("forkmeshRailKey", RailFolders::railKey(id));
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        m_header = new ActivityRailButton(QStringLiteral("file-directory"), name);
        m_header->setObjectName(QStringLiteral("railFolderHeader"));
        m_header->setProperty("forkmeshRailFolderId", id);
        layout->addWidget(m_header, 0, Qt::AlignLeft);
        m_body = new RailFolderPopout(this);
        m_body->setObjectName(QStringLiteral("railFolderBody"));
        m_body->setAcceptDrops(true);
        m_body->installEventFilter(this);
        m_bodyLayout = new QHBoxLayout(m_body);
        m_bodyLayout->setContentsMargins(4, 3, 4, 3);
        m_bodyLayout->setSpacing(1);
        m_empty = new QLabel(QStringLiteral("Drop\nhere"));
        m_empty->setObjectName(QStringLiteral("railFolderEmpty"));
        m_empty->setAlignment(Qt::AlignCenter);
        m_empty->setToolTip(QStringLiteral(
            "Drag a rail icon here, or right-click it and choose Move to folder"));
        QFont emptyFont = m_empty->font();
        emptyFont.setPixelSize(9);
        m_empty->setFont(emptyFont);
        m_empty->setFixedWidth(railCompactItemWidth());
        m_bodyLayout->addWidget(m_empty, 0, Qt::AlignVCenter);
        m_body->hide();
        if (parent)
            parent->installEventFilter(this);

        m_expandTimer = new QTimer(this);
        m_expandTimer->setSingleShot(true);
        m_expandTimer->setInterval(90);
        connect(m_expandTimer, &QTimer::timeout, this,
                [this] { setExpanded(true); });
        m_revealAnim = new QVariantAnimation(this);
        connect(m_revealAnim, &QVariantAnimation::valueChanged, this,
                [this](const QVariant &value) { applyReveal(value.toReal()); });
        m_collapseTimer = new QTimer(this);
        m_collapseTimer->setSingleShot(true);
        m_collapseTimer->setInterval(300);
        connect(m_collapseTimer, &QTimer::timeout, this, [this] {
            if (m_pinned || m_dragInside || containsCursor())
                return;
            if (QApplication::activePopupWidget()) {
                m_collapseTimer->start();
                return;
            }
            setExpanded(false);
        });
        connect(m_header, &QPushButton::clicked, this, [this] {
            m_pinned = !(m_pinned && m_expanded);
            setExpanded(m_pinned);
            refresh();
        });
        setName(name);
    }

    ~RailFolderWidget() override
    {
        if (m_body && m_body->parentWidget() != this)
            delete m_body;
    }

    QString id() const { return m_id; }
    QString name() const { return m_name; }
    ActivityRailButton *header() const { return m_header; }
    bool isExpanded() const { return m_expanded; }
    bool isPinned() const { return m_pinned; }

    void setName(const QString &name)
    {
        m_name = name;
        m_header->setGlyph(QStringLiteral("file-directory"), name);
        m_header->setAccessibleName(QStringLiteral("%1 folder").arg(name));
        m_header->setToolTip(QStringLiteral(
            "%1\nHover to open. Click to keep it open. Right-click to rename.")
                                 .arg(name));
    }

    QList<QWidget *> members() const
    {
        QList<QWidget *> widgets;
        if (!m_body)
            return widgets;
        for (const QPointer<QWidget> &member : m_members)
            if (member && member->parentWidget() == m_body)
                widgets.append(member);
        return widgets;
    }

    void setMembers(const QList<QWidget *> &widgets)
    {
        if (!m_body)
            return;
        for (QWidget *widget : widgets) {
            if (auto *button = dynamic_cast<ActivityRailButton *>(widget)) {
                QPointer<RailFolderWidget> self(this);
                button->setBadgeObserver([self] {
                    if (self)
                        self->refresh();
                });
            }
        }
        for (const QPointer<QWidget> &old : std::as_const(m_members))
            if (auto *button = qobject_cast<QAbstractButton *>(old.data()))
                disconnect(button, &QAbstractButton::clicked, this, nullptr);
        for (QWidget *widget : widgets) {
            if (auto *button = qobject_cast<QAbstractButton *>(widget)) {
                connect(button, &QAbstractButton::clicked, this, [this, button] {
                    if (m_body && button->parentWidget() == m_body)
                        setExpanded(false);
                });
            }
        }
        while (m_bodyLayout->count() > 0)
            delete m_bodyLayout->takeAt(0);
        m_members.clear();
        for (QWidget *widget : widgets) {
            m_bodyLayout->addWidget(widget, 0, Qt::AlignVCenter);
            m_members.append(widget);
        }
        m_bodyLayout->addWidget(m_empty, 0, Qt::AlignVCenter);
        refresh();
    }

    void followRailCaptionPreference()
    {
        m_header->followRailCaptionPreference();
        if (!m_body)
            return;
        m_empty->setFixedWidth(railCompactItemWidth());
        for (QWidget *member : members())
            if (auto *button = dynamic_cast<ActivityRailButton *>(member))
                button->followRailCaptionPreference();
        placeBody();
    }

    void refresh()
    {
        if (!m_body)
            return;
        bool active = false;
        bool urgent = false;
        int badge = 0;
        int shown = 0;
        QStringList labels;
        for (QWidget *member : members()) {
            if (member->isHidden())
                continue;
            ++shown;
            if (auto *button = qobject_cast<QAbstractButton *>(member)) {
                active = active || button->isChecked();
                if (!button->text().isEmpty())
                    labels.append(button->text());
            }
            if (auto *rail = dynamic_cast<ActivityRailButton *>(member)) {
                badge += rail->badgeCount();
                urgent = urgent || (rail->badgeCount() > 0 && rail->badgeUrgent());
            }
        }
        m_header->setChecked(active);
        m_header->setBadgeCount(m_expanded ? 0 : badge);
        m_header->setBadgeUrgent(urgent);
        m_empty->setVisible(shown == 0);
        m_header->setToolTip(
            QStringLiteral("%1%2\nHover to open. Click to keep it open. "
                           "Right-click to rename.")
                .arg(m_name,
                     labels.isEmpty()
                         ? QString()
                         : QStringLiteral(": ") + labels.join(QStringLiteral(", "))));
        placeBody();
    }

    void setExpanded(bool expanded, bool animated = true)
    {
        if (!expanded)
            m_pinned = false;
        if (m_expanded == expanded || !m_body)
            return;
        m_expanded = expanded;
        if (expanded) {
            QWidget *host = window();
            if (host != this && m_body->parentWidget() != host) {
                m_body->setParent(host);
                host->installEventFilter(this);
            }
            if (m_body->isHidden())
                m_reveal = 0.0;
            placeBody();
            m_body->show();
            m_body->raise();
        }
        const qreal target = expanded ? 1.0 : 0.0;
        m_revealAnim->stop();
        if (animated && isVisible() && m_reveal != target) {
            m_revealAnim->setDuration(qMax(
                40, qRound((expanded ? 200 : 170) * qAbs(target - m_reveal))));
            m_revealAnim->setEasingCurve(expanded ? QEasingCurve::OutCubic
                                                  : QEasingCurve::InCubic);
            m_revealAnim->setStartValue(m_reveal);
            m_revealAnim->setEndValue(target);
            m_revealAnim->start();
        } else {
            applyReveal(target);
        }
        refresh();
        update();
    }

    void setDropHighlight(bool on)
    {
        if (m_dragInside == on)
            return;
        m_dragInside = on;
        if (m_body)
            m_body->setHighlighted(on);
        update();
    }

    std::function<void(const QString &key)> onDropKey;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (m_body && watched == m_body.data()) {
            switch (event->type()) {
            case QEvent::Enter:
                m_collapseTimer->stop();
                break;
            case QEvent::Leave:
                if (m_expanded && !m_pinned)
                    m_collapseTimer->start();
                break;
            case QEvent::DragEnter:
                dragEnterEvent(static_cast<QDragEnterEvent *>(event));
                return true;
            case QEvent::DragMove:
                dragMoveEvent(static_cast<QDragMoveEvent *>(event));
                return true;
            case QEvent::DragLeave:
                dragLeaveEvent(static_cast<QDragLeaveEvent *>(event));
                return true;
            case QEvent::Drop:
                dropEvent(static_cast<QDropEvent *>(event));
                return true;
            default:
                break;
            }
        } else if (event->type() == QEvent::Move ||
                   event->type() == QEvent::Resize) {
            placeBody();
        }
        return QWidget::eventFilter(watched, event);
    }

    void moveEvent(QMoveEvent *event) override
    {
        placeBody();
        QWidget::moveEvent(event);
    }

    void resizeEvent(QResizeEvent *event) override
    {
        placeBody();
        QWidget::resizeEvent(event);
    }

    void hideEvent(QHideEvent *event) override
    {
        if (!event->spontaneous()) {
            setExpanded(false, false);
            m_revealAnim->stop();
            applyReveal(0.0);
        }
        QWidget::hideEvent(event);
    }

    void enterEvent(QEnterEvent *event) override
    {
        m_collapseTimer->stop();
        if (!m_expanded)
            m_expandTimer->start();
        QWidget::enterEvent(event);
    }

    void leaveEvent(QEvent *event) override
    {
        m_expandTimer->stop();
        if (m_expanded && !m_pinned)
            m_collapseTimer->start();
        QWidget::leaveEvent(event);
    }

    void dragEnterEvent(QDragEnterEvent *event) override
    {
        if (!event->mimeData()->hasFormat(kRailItemMimeType))
            return;
        event->acceptProposedAction();
        m_collapseTimer->stop();
        setDropHighlight(true);
        setExpanded(true);
    }

    void dragMoveEvent(QDragMoveEvent *event) override
    {
        if (event->mimeData()->hasFormat(kRailItemMimeType))
            event->acceptProposedAction();
    }

    void dragLeaveEvent(QDragLeaveEvent *event) override
    {
        setDropHighlight(false);
        if (!m_pinned)
            m_collapseTimer->start();
        QWidget::dragLeaveEvent(event);
    }

    void dropEvent(QDropEvent *event) override
    {
        setDropHighlight(false);
        const QString key = QString::fromUtf8(
            event->mimeData()->data(kRailItemMimeType));
        if (key.isEmpty())
            return;
        event->acceptProposedAction();
        if (onDropKey)
            onDropKey(key);
    }

    void paintEvent(QPaintEvent *) override
    {
        if (!m_expanded && !m_dragInside)
            return;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const bool dark = currentThemeIsDark();
        QColor fill = dark ? QColor(255, 255, 255, 16) : QColor(0, 0, 0, 12);
        if (m_dragInside)
            fill = nextAccentTint(dark ? QColor(46, 160, 67, 60)
                                       : QColor(31, 136, 61, 40));
        p.setPen(Qt::NoPen);
        p.setBrush(fill);
        p.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 6, 6);
    }

private:
    bool containsCursor() const
    {
        if (rect().contains(mapFromGlobal(QCursor::pos())))
            return true;
        return m_body && m_body->isVisible() &&
               m_body->rect().contains(m_body->mapFromGlobal(QCursor::pos()));
    }

    void applyReveal(qreal reveal)
    {
        m_reveal = qBound(0.0, reveal, 1.0);
        if (!m_body)
            return;
        if (!m_expanded && m_reveal <= 0.0) {
            m_body->hide();
            m_body->clearMask();
            return;
        }
        placeBody();
    }

    void placeBody()
    {
        if (!m_body || (!m_expanded && m_reveal <= 0.0))
            return;
        QWidget *host = m_body->parentWidget();
        if (!host || host == this)
            return;
        m_body->adjustSize();
        QWidget *rail = parentWidget() ? parentWidget() : this;
        const int left = rail->mapTo(host, QPoint(rail->width(), 0)).x();
        const int top = mapTo(host, QPoint(0, 0)).y() +
                        (m_header->height() - m_body->height()) / 2;
        const int width = m_body->width();
        const int shown = qBound(1, qRound(width * m_reveal), width);
        m_body->move(
            qMax(0, qMin(left, host->width() - width)) - (width - shown),
            qMax(0, qMin(top, host->height() - m_body->height())));
        if (shown >= width)
            m_body->clearMask();
        else
            m_body->setMask(QRegion(width - shown, 0, shown, m_body->height()));
    }

    QString m_id;
    QString m_name;
    ActivityRailButton *m_header = nullptr;
    QPointer<RailFolderPopout> m_body;
    QHBoxLayout *m_bodyLayout = nullptr;
    QLabel *m_empty = nullptr;
    QTimer *m_expandTimer = nullptr;
    QTimer *m_collapseTimer = nullptr;
    QVariantAnimation *m_revealAnim = nullptr;
    qreal m_reveal = 0.0;
    QList<QPointer<QWidget>> m_members;
    bool m_expanded = false;
    bool m_pinned = false;
    bool m_dragInside = false;
};

class RailGroupSeparator : public QWidget
{
public:
    RailGroupSeparator(const QString &id, const QString &name, bool custom,
                       QWidget *parent = nullptr)
        : QWidget(parent), m_id(id), m_custom(custom)
    {
        setObjectName(QStringLiteral("railGroupSeparator"));
        setProperty("forkmeshRailGroupId", id);
        setAcceptDrops(custom);
        setName(name);
        followRailCaptionPreference();
    }

    QString id() const { return m_id; }
    QString name() const { return m_name; }
    bool isCustom() const { return m_custom; }
    std::function<void(const QString &)> onDropKey;

    void setName(const QString &name)
    {
        m_name = name;
        setAccessibleName(QStringLiteral("%1 group").arg(name));
        setToolTip(m_custom
                       ? QStringLiteral("%1\nDrop a rail icon here to add it. "
                                        "Right-click to rename, move or remove.")
                             .arg(name)
                       : QStringLiteral("%1\nRight-click to move this group or "
                                        "add one of your own.")
                             .arg(name));
        update();
    }

    void followRailCaptionPreference()
    {
        const QSize wanted(railExpanded() ? railExpandedItemWidth()
                                          : railCompactItemWidth(),
                           captionShown() ? 14 : 7);
        if (size() != wanted)
            setFixedSize(wanted);
        update();
    }

protected:
    void dragEnterEvent(QDragEnterEvent *event) override
    {
        if (!m_custom || !event->mimeData()->hasFormat(kRailItemMimeType))
            return;
        m_dropping = true;
        update();
        event->acceptProposedAction();
    }

    void dragLeaveEvent(QDragLeaveEvent *) override
    {
        m_dropping = false;
        update();
    }

    void dropEvent(QDropEvent *event) override
    {
        m_dropping = false;
        update();
        const QString key =
            QString::fromUtf8(event->mimeData()->data(kRailItemMimeType));
        if (key.isEmpty() || !onDropKey)
            return;
        event->acceptProposedAction();
        const auto drop = onDropKey;
        QTimer::singleShot(0, this, [drop, key] { drop(key); });
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        const bool dark = currentThemeIsDark();
        const QColor line = m_dropping
                                ? QColor("#2ea043")
                                : Theme::remapColor(QColor(dark ? "#30363d" : "#d0d7de"));
        const QColor text = Theme::remapColor(QColor(dark ? "#8b949e" : "#656d76"));
        const int midY = height() / 2;
        if (!captionShown()) {
            p.fillRect(QRect(6, midY, width() - 12, 1), line);
            return;
        }
        QFont f = font();
        f.setPixelSize(8);
        f.setBold(true);
        f.setCapitalization(QFont::AllUppercase);
        p.setFont(f);
        const QFontMetrics fm(f);
        const QString caption =
            fm.elidedText(m_name, Qt::ElideRight, qMax(0, width() - 8));
        const int textWidth = fm.horizontalAdvance(caption);
        const int left = 4;
        p.setPen(text);
        p.drawText(QRect(left, 0, textWidth + 1, height()),
                   Qt::AlignLeft | Qt::AlignVCenter, caption);
        const int lineLeft = left + textWidth + 3;
        if (lineLeft < width() - 4)
            p.fillRect(QRect(lineLeft, midY, width() - 4 - lineLeft, 1), line);
    }

private:
    bool captionShown() const
    {
        return !railCaptionsHidden() || railExpanded();
    }

    QString m_id;
    QString m_name;
    bool m_custom = false;
    bool m_dropping = false;
};

QString whisperDir();

QString whisperModelName();

QString whisperModelPath();

QString whisperBinaryPath();

bool whisperInstalled();

QString voiceEngine();

QString parakeetDir();

QString parakeetPython();

QString parakeetScriptPath();

QString parakeetModelName();

bool parakeetInstalled();

bool voiceInputReady();

bool voiceInputEnabled();

QString voiceModelLabel();

struct AudioRecorderCommand {
    QString program;
    QStringList args;
};

QString preferredAudioRecorder();

// code-health: allow(blocking-wait) runs one device-listing command (pactl / arecord -l / ffmpeg -list_devices) capped at 3 s, on the GUI thread only when buildSettingsSection fills the microphone combo, or on Windows when audioRecorderFor starts a recording with no saved device
QList<QPair<QString, QString>> voiceInputDevices();

AudioRecorderCommand audioRecorderFor(const QString &outWav);

double wavPeakAmplitude(const QString &path);

inline constexpr double kVoiceSpokeThreshold = 0.02;

double wavLevelSince(const QString &path, qint64 *pos);

bool isWhisperSilenceHallucination(const QString &textIn);

void fitFileListToWidestEntry(QListWidget *list, int minW = 180, int maxW = 520);

void fitTreeToWidestEntry(QTreeWidget *tree, int minW = 240, int maxW = 620);

QString octiconMarkup(const QString &name, int size,
                      const QColor &color = Theme::remapColor(QColor("#8b949e")));

const QString kLogPromptAnchorPrefix = QStringLiteral("fmlogprompt:");
const QString kLogPromptIconResource = QStringLiteral("logprompt://add");

QString logPromptAnchorHref(const QString &storedLine);

QString logPromptAnchorLine(const QString &href);

const QString kLogSourceAnchorPrefix = QStringLiteral("fmlogsrc:");

QString logSourceAnchorHref(const QString &relativePath, int line);

bool logSourceAnchorTarget(const QString &href, QString *relativePath,
                                  int *line);

QString logSourceAnchorHtml(const QString &relativePath, int line,
                                   bool dark);

QString octiconForBackgroundTaskWord(const QString &word);

QString backgroundTaskWordFromLogMessage(const QString &storedLine);

QString logIconSpacerTag(QTextEdit *view, int size);

QString logBgtaskIconTag(QTextEdit *view, const QString &storedLine);

QString logPromptIconTag(QTextEdit *view, const QString &storedLine);

QString serverHost(const QString &serverUrl);

QString canonicalServerUrl(const QString &input);

QString normalizedRepoWebsite(QString input, QString *error = nullptr);

QString serverHostDisplay(const QString &fullUrl);

QString fileTypeIconName(const QString &fileNameLower);

// Where the file/folder tree icon set lives: baked into the binary as a Qt
// resource, with the source checkout and a directory beside the executable as
// fallbacks for from-source and unpackaged builds (see desktop/CMakeLists.txt).
QString treeIconsDir();

// The same glyph the Changes tree draws beside a path, inlined for rich text.
// Rendered once per (icon, size, DPR) and kept as finished markup: a wide diff
// asks for one per file row, and each miss re-parses an SVG and re-encodes a
// PNG.
QString fileTypeIconMarkup(const QString &fileName, int size);

QString languageForFile(const QString &name);

QString languageColor(const QString &lang);

// Depth of GitKeepAlive scopes. waitForGit now pumps on every GUI-thread wait
// regardless (see its comment), so the counter no longer gates anything; the
// scopes remain because they document interactive multi-read loads and their
// re-entrancy guards (openRepoDetail's m_repoDetailLoading, the ScopedFlag
// pattern below). The pump includes user input: excluding it made a Git read
// look backgrounded while window moves and every other interaction remained
// queued until the read completed.
// inline so the counter is a single shared instance across every TU that
// includes this header (the per-feature MainWindow*.cpp files all use GitKeepAlive).
inline int g_gitKeepAliveDepth = 0;

QElapsedTimer &keepAliveClock();
inline qint64 g_lastKeepAlivePumpMs = 0;

// code-health: allow(blocking-wait) this IS the keep-alive pump every cooperative GUI-thread wait calls; one processEvents pass capped at 12 ms, and its callers (waitForGit and friends) are where the wait is measured
void pumpKeepAlive();

QString gitPathspecCrumbName(const QString &pathspec);

// Leading "-c key=value" overrides come first on our fetch/clone lines, and the
// value is a secret: viewAuthGitArgs/importAuthGitArgs pass the signed view
// token as "-c http.extraHeader=Authorization: Basic <base64>". Anything built
// from a command line reaches the log file and actions.jsonl, so keep the key
// (an authed read is worth seeing) and drop the credential. Splits the
// remaining "-C <dir>" off as the repo name too, since both renderings put it
// at the end rather than in front of the subcommand.
QStringList gitRedactedConfigArgs(QStringList *args, QString *repo);

// The whole command line, redacted but never shortened: no 80-character cap on
// the command, no "+N more" on the pathspecs. This is what the footer's
// background-outcome line shows, so a ticket that keeps reappearing can be
// pasted into a shell as-is. Use gitArgsCrumb() instead where the text has to
// stay one glanceable phrase.
QString gitArgsLine(const QString &program, const QStringList &argsIn);

QString gitArgsCrumb(const QString &program, const QStringList &argsIn);

QString gitBlockingCrumb(const QProcess &process);

QString gitProcessCommandLine(const QProcess &process);

// code-health: allow(blocking-wait) only reached after a git wait already timed out (waitForGit, the branch-diff reader); stopProcessGracefully waits at most its grace period so git can drop index.lock before it is killed
QString gitTimeoutError(QProcess &process, int waitedMs);

inline constexpr int kGitReadTimeoutMs = 8000;
inline constexpr int kGitWriteTimeoutMs = 60000;

int gitWaitTimeoutMs(const QStringList &gitArguments);

bool isTransientGitError(const QString &err);

bool isGitLockContentionError(const QString &err);

QString worktreeTabKey(const QString &absPath);

QString gitignoreRuleForPath(const QString &relPath);

QString branchDiffErrorHtml(const QString &branch, const QString &err,
                                   int attempts);

QString inlinePixmapMarkup(const QPixmap &pixmap, int size);

QString inlinePixmapMarkup(const QPixmap &pixmap, int width, int height);

QString roundedCornerMarkup(int radius, int corner, const QColor &fill,
                            const QColor &border);

QString roundedPanelHtml(const QString &inner, const QString &width,
                         const QColor &fill, const QColor &border, int radius,
                         int padding, const QString &innerStyle = QString());

QString certificateFontStyle();

QString certificateHeadingHtml(const QString &text);

QString certificateColumnHtml(const QString &inner);

QString mergedBranchFromMergeSubject(const QString &subject);

struct MergeCelebrationRow {
    QString branch;      // empty when the merge subject named no branch
    QString mergeCommit; // full sha — what the row links to once the branch is gone
    bool branchStillExists = false;
    qint64 whenSecs = 0;
    int files = 0;
    int insertions = 0;
    int deletions = 0;
    QString actor;   // "Opus 5" for an agent, the account/commit name for a person
    QString detail;
    QPixmap avatar;  // model portrait or identicon, already sized
    bool byAgent = false;
    bool current = false; // this branch's own landing — highlighted in the day
    bool justNow = false; // ...and it just happened, so it is being celebrated
};

// The certificate always shows these rolling windows, independently of the
// history window selected for the rows below it. The series are one bar per
// minute, hour, rolling 24-hour slice (for both the 7- and 30-day windows), and
// rolling 7-day slice. All run oldest to newest so the right edge means "now"
// in the pane and shared image. The 52-week series is pane-only: the shared
// image and its share links keep the four shorter windows.
struct MergeVelocity {
    QVector<int> minutely = QVector<int>(60, 0);
    QVector<int> hourly = QVector<int>(24, 0);
    QVector<int> daily = QVector<int>(7, 0);
    QVector<int> monthly = QVector<int>(30, 0);
    QVector<int> weekly = QVector<int>(52, 0);
    int lastHour = 0;
    int last24Hours = 0;
    int last7Days = 0;
    int last30Days = 0;
    int last52Weeks = 0;
    // Per card (1h, 24h, 7d, 30d, 52w): the busiest rolling window of that
    // length in the whole merge history, and the mean merges per window of
    // that length since the oldest merge. Empty when only the bucket series
    // are known (a decoded share link); the cards then fall back to the
    // current total.
    QVector<int> periodTop;
    QVector<double> periodAvg;
};

void mergeVelocityPeriodStats(QList<qint64> timestamps, qint64 nowSecs,
                                     MergeVelocity *velocity);

MergeVelocity mergeVelocityForTimestamps(const QList<qint64> &timestamps,
                                                 qint64 nowSecs);

struct MergeCertificateData {
    QString branch;
    QString base;
    QString actor;
    QString detail;
    QString mergeCommit;
    qint64 whenSecs = 0;
    int commits = -1;
    int files = 0;
    int insertions = 0;
    int deletions = 0;
    MergeVelocity velocity;
};

MergeCertificateData mergeCertificateData(
    const MergeCelebrationRow &landed, const QString &base, int commits,
    const MergeVelocity &velocity);

QString mergeCelebrationRowHref(const MergeCelebrationRow &row);

struct MergeActorTally {
    QString actor;
    QPixmap avatar;
    int merges = 0;
    int files = 0;
    int insertions = 0;
    int deletions = 0;
};

// minMerges = 2 for the celebration: a single landing is already its own row in
// the day below it, so a tally of one would only restate it. A pane that shows
// the tallies without that list (the empty-range stats) passes 1.
QList<MergeActorTally> mergeCelebrationTallies(
    const QList<MergeCelebrationRow> &today, int minMerges = 2);

QString mergeChurnHtml(int files, int insertions, int deletions);

QString mergeRowChurnHtml(const MergeCelebrationRow &row);

QPixmap mergeVelocityChartPixmap(const QVector<int> &source,
                                        const QString &oldestLabel,
                                        int width = 306, int height = 88);

QString mergeVelocityHtml(const MergeVelocity &velocity,
                          const QString &heading = QStringLiteral("MERGE VELOCITY"),
                          bool compact = false);

QString userActivityHeatHtml(const QVector<int> &counts, int nowSlot);

QJsonArray mergeVelocityJson(const QVector<int> &values);

QVector<int> mergeVelocityFromJson(const QJsonValue &value, int size);

QUrl mergeCertificateShareUrl(const QString &platform,
                                     const MergeCertificateData &data);

bool mergeCertificateDataFromUrl(const QUrl &url,
                                        MergeCertificateData *data);

QString mergeCertificateShortName(const QString &value, int limit);

QString mergeCertificateShareTitle(const MergeCertificateData &data);

QString mergeCertificateShareText(const MergeCertificateData &data);

QUrl mergeCertificateComposerUrl(const QString &platform,
                                        const MergeCertificateData &data);

QImage mergeCertificateImage(const MergeCertificateData &data);

QString mergeCertificateShareButtonsHtml(
    const MergeCertificateData &data);

QString mergeSectionHeadingHtml(const QString &text);

// adhoc #32: the list under a landing used to be "today", a window that resets
// at midnight — a branch merged at 23:50 fell out of it ten minutes later, and
// nothing on the page could look further back. It is a window the reader picks
// instead, and it opens on the last 24 hours.
struct MergeRangeSpec {
    const char *key;     // href token: fmmergerange:<key>
    const char *button;  // what the pill says
    const char *heading; // "PAST 24 HOURS", for a section heading
    const char *phrase;  // "in the past 24 hours", mid-sentence
    int days;            // how far back the window reaches
};

inline constexpr int kMergeRangeCount = 4;

const MergeRangeSpec *mergeRangeSpecs();

int clampedMergeRange(int index);

const MergeRangeSpec &mergeRangeSpec(int index);

// An unknown token — a stale page's href — reads as the default window.
int mergeRangeIndexForKey(const QString &key);

QString mergeRangeHeading(int index);

QString mergeRangePhrase(int index);

// The window picker. Table cells, not inline spans: Qt's rich text pads the
// former and ignores padding on the latter, and an unpadded pill is a word.
QString mergeRangePickerHtml(int selected);

// A section heading with the window picker parked at its right edge. An empty
// heading leaves the picker alone on the line — what a window with nothing in
// it shows, so that page can still be widened.
QString mergeRangeHeaderHtml(const QString &text, int selected);

QString certificateListHtml(const QStringList &rows, int highlighted);

// "Who landed how much in the window" — the per-actor half of a merge summary.
// Shown both under a fresh landing and on a range that has nothing left to
// show. A wide window can name more mergers than a pane can carry, so the
// leaders are listed and the tail is counted rather than dropped in silence.
QString mergeActorTalliesHtml(const QList<MergeActorTally> &allTallies,
                                     const QString &heading);

// "What landed in the window" — one row per merge, newest first.
QString mergeDayRowsHtml(const QList<MergeCelebrationRow> &recent,
                                const QString &base, int truncated, int range = 0);

QString certificateCardHtml(const QString &title, const QString &eyebrow,
                            const QPixmap &avatar, const QString &headline,
                            const QString &statement, const QString &credit,
                            const QString &below, const QString &width);

// The fresh-merge pane and the later no-changes pane are two entrances to the
// same artifact. Keep the certificate itself here so branding, velocity, and
// sharing cannot drift between the moment a branch lands and a later visit.
QStringList mergeCertificateHtml(
    const MergeCelebrationRow &landed, const QString &base, int commits,
    const MergeVelocity &velocity, const QList<MergeActorTally> &tallies,
    const QString &talliesHeading, bool alreadyLanded);

struct PullCertificateData {
    int number = 0;
    QString title;
    QString head;
    QString base;
    QString author;
    QPixmap avatar;
    qint64 openedSecs = 0;
    int files = 0;
    int additions = 0;
    int deletions = 0;
    MergeVelocity velocity;
};

QString pullCertificateHtml(const PullCertificateData &data);

QString mergeCelebrationHtml(const MergeCelebrationRow &landed,
                                    const QString &base,
                                    const QList<MergeCelebrationRow> &recent,
                                    int commits, int truncated,
                                    const QList<MergeActorTally> &tallies = {},
                                    int range = 0,
                                    const MergeVelocity &velocity = {},
                                    const QVector<int> &activity = {},
                                    int activityNowSlot = -1,
                                    bool lookBack = false);

// The range pane of a branch that has nothing left to show against the base —
// most often an agent branch whose work already landed. One grey line used to
// be the whole page; the window's merges and who landed them are the stats that
// line was hiding (adhoc #1651). `landed` names this branch's own merge when
// the window still holds it, and is left empty otherwise.
QString branchNoChangesHtml(const QString &message,
                                   const MergeCelebrationRow &landed,
                                   const QString &base,
                                   const QList<MergeCelebrationRow> &recent,
                                   int truncated,
                                   const QList<MergeActorTally> &tallies,
                                   int range = 0,
                                   const MergeVelocity &velocity = {});

// A subprocess whose complete lifetime belongs to a worker thread.  Calling
// QProcess::start() is normally quick, but under memory or process pressure
// Qt's launcher handshake can itself take seconds.  Keeping construction,
// launch, pipe draining, deadline handling and reaping together here means an
// asynchronous caller never has to put any part of that handshake on the GUI
// thread.
struct WorkerProcessRequest {
    QString program;
    QStringList arguments;
    QString workingDirectory;
    QProcessEnvironment environment;
    QProcess::ProcessChannelMode channelMode = QProcess::SeparateChannels;
    int timeoutMs = 8000;
    qsizetype maxOutputBytes = -1;
    QString activityKind;
    QString activityDetail;
    // Attribution for the footer's outcome line and the action journal (adhoc
    // #186): the routine that asked for this process, read off
    // BackgroundRoutine on the requesting thread before the hop to the worker,
    // and how long the request waited in a queue before it was started.
    QString activityRoutine;
    qint64 activityQueuedMs = -1;
};

struct WorkerProcessResult {
    bool started = false;
    bool ok = false;
    bool timedOut = false;
    bool cancelled = false;
    int exitCode = -1;
    QProcess::ExitStatus exitStatus = QProcess::CrashExit;
    QByteArray output;
};

void appendWorkerProcessOutput(QByteArray *output,
                                      const QByteArray &chunk,
                                      qsizetype maxBytes);

// The outcome a worker process retires its ticket with. Names the exit code
// so a failing `git status` reads as "failed (exit 128)" on the footer line
// instead of the same checkmark as a clean run.
QString workerProcessOutcome(const WorkerProcessResult &result);

WorkerProcessResult runWorkerProcess(
    const WorkerProcessRequest &request,
    const std::shared_ptr<std::atomic_bool> &cancelled = {},
    const forkmesh::LogSource &source = forkmesh::logSourceHere());

inline constexpr int kAgentPullGitTimeoutMs = 120000;

struct AgentPullCapture {
    QString patch;
    QString commits;
    QString failure;
};

AgentPullCapture captureAgentPull(const QString &workdir, const QString &baseRef,
                                  const QString &baseBranch);

// The checkout a git process works in: its working directory, or the `-C
// <dir>` the capture helpers pass instead of setting one.
QString gitProcessWorkingDirectory(const QProcess &process);

// How a finished process ended, in the words the footer's outcome line and
// the action journal use: "succeeded", "failed (exit 128)", "crashed",
// "failed to start".
QString processExitOutcome(const QProcess &process);

// code-health: allow(blocking-wait, pump-in-loop) this IS the app's git wait primitive: off the GUI thread a plain waitForFinished capped by gitWaitTimeoutMs; on it, 40 ms waitForFinished slices each followed by a 12 ms-capped pumpKeepAlive, with the process stopped at the same cap; callers own the re-entrancy guards
bool waitForGit(QProcess &process, QString *err,
                       const forkmesh::LogSource &source =
                           forkmesh::logSourceHere());

void trackProcessActivity(QProcess *process, const QString &kind,
                                 const QString &detail = QString(),
                                 const forkmesh::LogSource &source =
                                     forkmesh::logSourceHere());

// RAII: marks the run of synchronous git reads in an interactive load (a node
// switch or opening a repo). Nestable. waitForGit pumps on the GUI thread with
// or without this scope now; the marker is kept for the depth counter and as
// documentation that the enclosing flow expects pumped re-entrancy.
struct GitKeepAlive {
    GitKeepAlive() { ++g_gitKeepAliveDepth; }
    ~GitKeepAlive() { --g_gitKeepAliveDepth; }
    GitKeepAlive(const GitKeepAlive &) = delete;
    GitKeepAlive &operator=(const GitKeepAlive &) = delete;
};

// RAII: hold a bool true for the scope's lifetime. Used as a re-entrancy guard so
// a heavy slot serviced by GitKeepAlive's event-loop pump can't re-enter and stack
// its synchronous git work mid-flight.
struct ScopedFlag {
    bool &flag;
    explicit ScopedFlag(bool &f) : flag(f) { flag = true; }
    ~ScopedFlag() { flag = false; }
    ScopedFlag(const ScopedFlag &) = delete;
    ScopedFlag &operator=(const ScopedFlag &) = delete;
};

QString resolveGitDirPath(const QString &dir);

QString headBranchFromFile(const QString &dir);

// Resolves a full ref name ("refs/heads/main") to its object name by reading
// the repository's files the way git does — the loose ref wins, else the
// packed-refs entry — without spawning a process. Empty when neither exists
// (or the repository uses a ref backend other than files). Used to key memos
// of git reads that are pure functions of a branch tip, so the periodic
// catalog and mirror-panel refreshes stop re-running `rev-list`/`log` walks
// for a tip that has not moved.
QString refTipFromFiles(const QString &gitDir, const QString &fullRef);

// A cheap "did any ref move?" stamp for a repository: the newest mtime and the
// entry count across HEAD, packed-refs and everything under refs/ (walked
// recursively — git renames a lock file into the branch's own directory, so a
// nested branch like agent/x only bumps refs/heads/agent/, never refs/heads).
// A few dozen stats, microseconds; compare it before spending a git process.
QString gitRefsDirSignature(const QString &gitDir);

bool forEachRefHeadsAndTagsFromFiles(const QString &gitDir, QByteArray *output);

// Memo for a git read that is a pure function of one branch tip. When the tip
// can be read from the ref files the value is cached per (repo, branch,
// what) until the tip moves; otherwise compute() runs every time. Thread-safe:
// the catalog advert pass and the mirror-panel snapshot both run on workers.
template <typename T>
inline T mirrorTipMemoized(const QString &mirrorPath, const QString &branch,
                           const QString &what, const std::function<T()> &compute,
                           const QString &extraKey = QString())
{
    struct Entry {
        QString tip;
        T value;
    };
    static QHash<QString, Entry> cache;
    static QMutex mutex;
    const QString tip =
        refTipFromFiles(mirrorPath, QStringLiteral("refs/heads/") + branch);
    if (tip.isEmpty())
        return compute();
    const QString key = mirrorPath + QLatin1Char('\n') + branch +
                        QLatin1Char('\n') + what + QLatin1Char('\n') + extraKey;
    {
        QMutexLocker lock(&mutex);
        const auto cached = cache.constFind(key);
        if (cached != cache.constEnd() && cached->tip == tip)
            return cached->value;
    }
    T value = compute();
    QMutexLocker lock(&mutex);
    if (cache.size() > 1024)
        cache.clear();
    cache.insert(key, Entry{tip, value});
    return value;
}

// A few stats that change whenever `git worktree list --porcelain` would
// print something different about the linked worktrees: the worktrees
// directory itself (an add or remove) and each linked worktree's HEAD (a
// branch switch or detach). Empty when the path is not a git checkout.
QString worktreeListStamp(const QString &repoPath);

struct GitCaptureResult {
    bool ok = false;
    QByteArray output;
    QString error;
    QString failureOutput;
};

QString gitFailureDetail(const GitCaptureResult &result, const QString &command,
                         int maxChars = 240);

// code-health: allow(blocking-wait, git-in-loop) runs on workers only: runGitCaptureBackgrounded hands GUI-thread calls to QtConcurrent, and the direct callers are QtConcurrent/runOffThread bodies; runGitIndexSerialized re-runs git only on index/ref lock contention (bounded by GitLockRetryPolicy), each capped by waitForGit (8 s for reads, 60 s for index writes, 120 s for merges)
GitCaptureResult runGitCaptureDirect(
    const QString &dir, const QStringList &args,
    const QByteArray *input = nullptr,
    const forkmesh::LogSource &source = forkmesh::logSourceHere());

GitCaptureResult runGitCaptureBackgrounded(
    const QString &dir, const QStringList &args,
    const QByteArray *input = nullptr,
    const forkmesh::LogSource &source = forkmesh::logSourceHere());

bool runGitCapture(const QString &dir, const QStringList &args,
                          QByteArray *out, QString *err,
                          const forkmesh::LogSource &source =
                              forkmesh::logSourceHere());

bool runGitCaptureInput(const QString &dir, const QStringList &args,
                               const QByteArray &input, QByteArray *out,
                               QString *err,
                               const forkmesh::LogSource &source =
                                   forkmesh::logSourceHere());

QString jsonBrokenByMergeResolution(const QString &dir);
bool commitResolvedMerge(const QString &dir, QString *error);

// `git worktree list --porcelain` for a checkout, served from a process-wide
// memo while worktreeListStamp() is unchanged. The branches panel, the
// worktrees panel, the catalog pass and every per-branch/per-session worktree
// lookup all want this list, and between them they spawned it thousands of
// times a day for an answer that moves only when a worktree is added, removed
// or switches branch. Thread-safe (workers call it too).
bool cachedWorktreeListPorcelain(const QString &repoPath, QByteArray *out);

// The repository's common directory — where refs/, packed-refs and config
// live. For a linked worktree that is the main repository's gitdir, named by
// the worktree's own `commondir` file; for anything else it is just the gitdir.
QString gitCommonDirPath(const QString &repoPath);

// Stamp for repository-local `config` reads. Everything we ask `git config
// --local` for lives in that one file, so its mtime+size answers "could this
// have changed" without spawning anything.
QString gitConfigStamp(const QString &repoPath);

// `git config --local --get <key>`, served from a process-wide memo while the
// repository's config file is unchanged. The mirror panels re-ask for the same
// identity marker once per repository per rebuild, and on a loaded box a single
// one of those spawns has been measured blocking the GUI for 25 s (stalls.log
// 2026-09-09). A config write moves the file's mtime, so the memo invalidates
// itself and needs no explicit eviction. Thread-safe (workers call it too).
bool cachedGitConfigLocal(const QString &repoPath, const QString &key,
                                 QByteArray *out);

// `git remote -v`, memoised on the same config stamp. Remotes are declared in
// the config file, so nothing else can move this answer. This was the single
// most expensive blocking call in stalls.log over 2026-09-01..10 — 33 UI
// stalls totalling 85 s — because the repo panels re-read it per rebuild.
bool cachedRepoRemotesRaw(const QString &repoPath, QByteArray *out);

QString worktreeHeadBranch(const QString &workTree);

QString worktreeHeadCommit(const QString &workTree);

QString worktreeBranchCommit(const QString &workTree, const QString &branch);

// True when git can resolve `ref` to a commit in `dir` — a branch, a tag or a
// raw object name. The ref a screen is browsing can vanish under it (merging a
// branch deletes it, so do "Delete merged branches", removing an agent's
// worktree and deleting a pull request), and every git pass after that fails
// with "fatal: Not a valid object name <ref>".
bool gitRefResolves(const QString &dir, const QString &ref);

bool worktreeTrackedClean(const QString &workTree);

bool releaseWorktreeHoldingBranch(const QString &dir, const QString &branch);

bool checkoutReleasingWorktree(const QString &dir, const QString &branch,
                               QString *err);

// code-health: allow(blocking-wait) sync git primitive; the only wait is waitForGit (pumped 40 ms slices on the GUI thread, a plain wait on workers, 8 s cap either way), and the heavy diff callers already run it in runOffThread/worker reads
QByteArray gitCaptureStdout(const QString &dir, const QStringList &args,
                                   const forkmesh::LogSource &source =
                                       forkmesh::logSourceHere());

// code-health: allow(blocking-wait) only used by buildWorkingTreeDiff, reached once from the New pull request dialog's OK path; the wait is waitForGit's pumped 40 ms slices, capped at 8 s per git call
bool runGitCaptureWithEnv(const QString &dir, const QStringList &args,
                          const QProcessEnvironment &env, QByteArray *out,
                          QString *err,
                          const forkmesh::LogSource &source =
                              forkmesh::logSourceHere());

// code-health: allow(blocking-wait) only caller is the QThread::create worker that tidies the mirror after syncRepository's fetch (MainWindowRepoFetch.cpp); the update-ref wait is capped at 8 s
void pruneNonStableMirrorRefs(const QString &mirrorPath);

qint64 parseCountObjectsSizeBytes(const QByteArray &countObjectsOutput);

qint64 mirrorRepoSizeBytes(const QString &mirrorPath);

// The release artifact store the gateway streams from, and the sharded CAS
// inside it. Both the mirror gateway (`releaseStore` in its config) and the
// publisher address blobs as <store>/sha256/<aa>/<hash>/data, so these two must
// stay in step with tools/mirror_gateway.py and forkmesh-release-publish.sh.
QString mirrorReleaseStoreRoot(const QString &mirrorPath);
QString mirrorReleaseCasRoot(const QString &mirrorPath);
QString mirrorReleaseBlobPath(const QString &mirrorPath, const QString &hash);

// code-health: allow(dir-walk-in-loop) the loop is the single walk of the sharded sha256/<aa>/<hash>/data layout: one entryList per shard directory (at most 256), never a re-walk of the same tree, plus one stat per stored blob
int releaseCasBlobCount(const QDir &casDir);
int mirrorArtifactCount(const QString &mirrorPath);
int checkoutArtifactCount(const QString &localPath);

struct MirrorReleaseBlob {
    QString hash; // the blob's sha256 (its own directory name / identity)
    QString path; // absolute path to the "data" file on disk
    qint64 size = 0; // byte size of the stored blob
};

// code-health: allow(dir-walk-in-loop) the loop is the single walk of the sharded sha256/<aa>/<hash>/data layout: one entryList per shard directory (at most 256), never a re-walk of the same tree, plus one stat per stored blob
QList<MirrorReleaseBlob> mirrorReleaseBlobs(const QString &mirrorPath);

// Carry a node's release artifact store from a retiring mirror directory into
// the one that will serve the repository next. An encrypted repository is
// served out of a temporary materialization that every sealing pass replaces,
// and release binaries live beside the git data rather than in it,
// so without this every artifact this node hosts is dropped the moment the old
// materialization is released — install.sh then 404s on a release it just
// published. Blobs are content-addressed and immutable, so a hard link is
// enough (and costs nothing); copying is only the cross-device fallback.
// Returns the number of blobs carried over.
int carryMirrorReleaseCas(const QString &fromMirror,
                                 const QString &toMirror);

int mirrorNumberedDirCountUncached(const QString &mirrorPath,
                                          const QString &branch,
                                          const QString &subdir);

int mirrorNumberedDirCount(const QString &mirrorPath, const QString &branch,
                                  const QString &subdir);

int mirrorNumberedDirMax(const QString &mirrorPath, const QString &branch,
                                const QString &subdir);

QString nodeListIdentityKey(const MemberInfo &m);

// The stats side of the Nodes table (owner / version / platform / mirrors /
// CPU / RAM / disk / node id) is backfilled from the catalog sweep, because a
// headless mirror never joins the chat room and so has a blank roster record.
// That sweep polls one /mirrors endpoint per repository, and any of them can be
// skipped by the poll backoff or answer with ok:false — a Cloudflare 429 puts
// the whole relay host in cooldown, which suppresses every request in the
// round. Its result is therefore a partial view, not a census, and assigning it
// wholesale blanked the entire fleet's stats whenever a round came back short.
// Deleting a node forced exactly that: it refetches immediately, in the middle
// of the burst the deletion itself just sent at the relay.
//
// Merge instead. A node this round actually saw wins; a node it did not see is
// dropped only when every repository it was mirroring was successfully re-read,
// which is the only evidence that it is genuinely gone rather than unobserved.
QHash<QString, QJsonObject> mergeNodesCatalogSweep(
    const QHash<QString, QJsonObject> &previous,
    const QHash<QString, QJsonObject> &sweep,
    const QSet<QString> &coveredSources);

bool isTemporaryChatGuest(const MemberInfo &m);

int mirrorOpenIssueCount(const QString &mirrorPath, const QString &branch);

int mirrorIssueCount(const QString &mirrorPath, const QString &branch);
int mirrorIssueMaxNumber(const QString &mirrorPath, const QString &branch);
// Whether the mirror has the given branch, answered from the ref files when
// it uses the files backend (the case for every mirror this app creates) and
// by git only when the layout is unrecognised.
bool mirrorHasBranch(const QString &mirrorPath, const QString &branch);

// mirrorHasBranch() for a path that may be a work tree or a linked worktree
// rather than a bare mirror: resolve to the common directory first, because
// that is where refs/ and packed-refs actually live. Branch-existence probes
// like these were the second-largest source of blocking git spawns in
// stalls.log over 2026-09-01..10 (the `rev-parse --verify` cluster: 29 UI
// stalls, 36 s), and none of them needs a process.
bool repoHasLocalBranch(const QString &repoPath, const QString &branch);
QString localPullAgentBranch(const QString &gitDir, const PullRequest &pull);

// PullStore::loadAll() skips a forkmesh/pull/<n> branch whose snapshot is no
// newer than the ledger commit that deleted its record, because mirrors keep
// those branches long after the pull is gone. The mirror counts have to skip
// the same leftovers, or the Pulls tab badge counts deleted pulls the list
// never shows (#175 and #182 kept a badge of 2 over an empty Open list).
// `independentRefs` is for-each-ref output in the
// "%(refname) %(objectname) %(committerdate:unix)" format.
QSet<int> mirrorDeletedPullRefNumbers(const QString &mirrorPath,
                                             const QByteArray &independentRefs,
                                             const QSet<int> &ledgerNumbers);

int mirrorPullCount(const QString &mirrorPath, const QString &branch);
// The status a mirrored pull record declares: the `status:` line of a
// pull.md front matter, or the "status" field of a legacy JSON record.
// Empty when the record does not say, which every reader treats as open.
QString mirrorPullRecordStatus(const QByteArray &blob);

// Pull requests still waiting for review, which is what the repository tab
// badge reports. mirrorPullCount() above counts every record the mirror
// holds — on a long-lived repository that is the whole merged history, two
// orders of magnitude more than the work actually open. Records that live
// only on the code branch are the pre-ledger archive and never count: the
// list paints them as closed however their old metadata reads.
int mirrorOpenPullCount(const QString &mirrorPath, const QString &branch);

int mirrorDiscussionCount(const QString &mirrorPath, const QString &branch);

int mirrorCommitCount(const QString &mirrorPath, const QString &branch);

int mirrorMergeCommitCount(const QString &mirrorPath,
                                  const QString &branch);

CommitIdentity gitCommitIdentity(const QString &gitPath,
                                        const QString &commit);

CommitIdentity mirrorCommitIdentity(const QString &mirrorPath,
                                           const QString &workTreePath,
                                           const QString &commit);

QJsonArray mirrorGitActivityWeeks(const QString &mirrorPath,
                                         const QString &branch,
                                         bool mergesOnly);

// The 52-week histograms walk a year of history (the two slowest reads of
// the catalog pass, ~200 ms each on this repository). Per tip they only move
// as the calendar does, so remember them per (tip, day).
QJsonArray mirrorCommitActivityWeeks(const QString &mirrorPath,
                                            const QString &branch);

QJsonArray mirrorMergeActivityWeeks(const QString &mirrorPath,
                                           const QString &branch);

int mirrorBranchCount(const QString &mirrorPath);

int mirrorWorktreeCount(const QString &localPath);

// The live index of a checkout root, found without spawning git: a plain
// checkout keeps it at .git/index, and a linked worktree's .git is a one-line
// file naming the private gitdir that holds its index.
QString liveGitIndexPath(const QString &dir);

// Seeds the temporary snapshot index with a copy of the checkout's live index.
// An index freshly built by `read-tree` carries no stat cache, so the
// `git add -A` that follows re-reads and re-hashes every file in the tree: a
// few seconds on a large checkout, every time the diff is opened. A copy of
// the live index carries the size and mtime of each tracked file as git last
// saw it, so the same add only hashes files whose stat data changed and
// finishes in tens of milliseconds. The snapshot's content is the same either
// way, since `add -A` replaces every entry with what is on disk; git rewrites
// the index atomically (index.lock then rename), so the copy is never of a
// half-written file. Returns false when there is no live index to copy, in
// which case the caller loads the base tree instead.
bool seedSnapshotIndexFromWorktree(const QString &dir, const QString &indexPath,
                                          QString *livePath = nullptr);

bool buildWorkingTreeDiff(const QString &dir, const QString &base, QByteArray *out,
                          QString *err);

// A directory below a worktree is usually fine for ordinary Git commands, but
// not for our temporary-index snapshot. `git add -A -- .` then names that
// directory as a pathspec relative to the real worktree root; if it is an
// ignored container such as .worktrees, Git rejects the snapshot outright.
// Only use a live-worktree snapshot when the supplied path is the worktree
// root. Callers can safely fall back to a committed range otherwise.
bool isGitWorkingTreeRoot(const QString &dir);

bool ensureGitCommitGraph(const QString &dir);

QString pillTextColor(const QString &backgroundHex);

// The two paths a favicon probe can take: the browser-conventional root and
// the static asset ForkMesh relays actually publish.  The network log knows a
// probe by the mark on its request (markFaviconProbe), not by these paths.
QString faviconRootPath();

QString faviconAssetPath();

// A favicon is decoration: every host that reaches these helpers is one the
// app merely saw in a log line or a server entry, not one it must reach.  A
// name under a reserved special-use TLD can never answer on the public
// internet -- .internal in particular is this codebase's own synthetic host
// for Worker cache keys and tests, so it shows up in tailed CLOUD log lines --
// and probing it only buys a guaranteed DNS failure.  Draw the generated
// letter mark instead of sending a request that cannot succeed.
bool hostCannotServePublicFavicon(const QString &host);

QUrl faviconUrl(const QString &serverUrl);

QUrl faviconAssetUrl(const QUrl &url);

QString faviconCacheDir();

QString faviconCachePath(const QString &host);

QPixmap roundedRectPixmap(const QPixmap &src, int side, qreal radius);

QPixmap withPresenceDot(const QPixmap &face, const QColor &dot,
                        const QColor &ring);

QPixmap roundedBannerPixmap(const QPixmap &src, int w, int h, qreal radius);

QPixmap languageBarPixmap(const QList<QPair<QString, qint64>> &langs,
                          qint64 total, int shown, int w, int h);

QPixmap letterFavicon(const QString &host, int side = 36);

QString builtinFaviconKey(const QString &host);

bool hasBuiltinFavicon(const QString &host);

QPixmap builtinFavicon(const QString &host, int side = 36);

#if defined(Q_OS_WIN)
const QString kWinRunKey =
    QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run");
#endif

QString autostartDesktopPath();

#if defined(Q_OS_MACOS)
inline QString launchAgentPath()
{
    return QDir::homePath() + "/Library/LaunchAgents/com.forkmesh.app.plist";
}
#endif

bool isAutostartEnabled();

QString autostartMechanismName();

QString autostartLocation();

// A desktop started directly by systemd --user inherits INVOCATION_ID and has
// its unit in the unified cgroup path. A close is an explicit user request to
// stop that desktop, not a failure that its Restart=always policy should undo.
// Keep this parser side-effect free so callers can safely decline to involve
// systemd when either signal is absent.
QString systemdUserServiceUnitFromCgroup(const QByteArray &contents);

QString supervisingSystemdUserServiceUnit();

QStringList systemdUserServiceCloseArguments(const QString &unit, bool restart);

inline constexpr char kSourceGuardUnit[] = "forkmesh-source.service";
inline constexpr qint64 kDesktopCloseHoldSeconds = 100LL * 365 * 24 * 3600;

QString sourceGuardHoldPath();

bool closeHoldsSourceGuard(const QString &unit, bool restart,
                           bool reopenAfterClose);

bool writeDesktopCloseHold(const QString &path, qint64 nowSeconds);

bool releaseDesktopCloseHold(const QString &path);

bool requestSupervisingUserServiceRestart(const QString &relaunchPath);

bool supervisingUserServiceRestartRequested();

bool setAutostartEnabled(bool enabled);


} // namespace forkmesh::ui
