
#include "MainWindow.h"
#include "CodeSymbols.h"
#include "CurrentPageStack.h"
#include "FileTreeSupport.h"
#include "GlobalSearchMatch.h"
#include "MainWindowInternal.h"
#include "MarkdownRender.h"
#include "NetworkFilesPanel.h"

#include <QCheckBox>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QShortcut>
#include <QSplitter>
#include <QTabBar>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTimer>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

using namespace forkmesh::ui;

namespace {
constexpr int kFileLoadedRole = Qt::UserRole + 2;

const char *kFilesRootSetting = "files/explorerRoot";
const char *kFilesHiddenSetting = "files/showHidden";
const char *kFilesShowSizeSetting = "files/showSize";

constexpr qint64 kPreviewByteLimit = 256 * 1024;

// The cap when a particular line was asked for. A log entry's origin link
// (adhoc #1587) names one, and the point of following it is to land on it —
// but MainWindowChat.cpp alone is ~1.2 MB, so the 256 KB cap above stopped
// around line 1,800 and clicking "MainWindowChat.cpp:8766" opened the file at
// the top and highlighted nothing at all (adhoc #1635). Still bounded: this
// ends up in a widget, and one enormous file must not take the window with it.
constexpr qint64 kPreviewLineByteLimit = 8 * 1024 * 1024;

// Lines kept past the one being parked on, so it lands with context under it
// rather than on the last row of the preview.
constexpr int kPreviewLineContext = 200;

// The preview opens with "<path>\n<size> · <modified>\n\n" before the file's
// own first line, so line N of the file is block kFilePreviewHeaderLines+N-1.
constexpr int kFilePreviewHeaderLines = 3;

// The head of `file`: the ordinary cap when no particular line is wanted, and
// otherwise as much as it takes to reach `line` plus kPreviewLineContext after
// it. Reading from the start (rather than seeking to the line) is what keeps
// line N at block kFilePreviewHeaderLines+N-1, which is the whole basis of the
// highlight; the early exit is what keeps that affordable on a large file.
QByteArray readPreviewHead(QFile &file, int line)
{
    if (line <= 0)
        return file.read(kPreviewByteLimit);
    constexpr qint64 kChunkBytes = 64 * 1024;
    const int wanted = line + kPreviewLineContext;
    QByteArray head;
    int newlines = 0;
    while (head.size() < kPreviewLineByteLimit && newlines < wanted) {
        const QByteArray chunk = file.read(
            qMin<qint64>(kChunkBytes, kPreviewLineByteLimit - head.size()));
        if (chunk.isEmpty())
            break;
        // Counted per chunk rather than over the whole buffer each time round:
        // re-scanning what has already been read turns this quadratic on the
        // very files the raised cap exists for.
        newlines += static_cast<int>(chunk.count('\n'));
        head += chunk;
    }
    return head;
}

QString modifiedLabel(const QFileInfo &info)
{
    const QDateTime when = info.lastModified();
    return when.isValid() ? when.toLocalTime().toString(
                                QStringLiteral("yyyy-MM-dd HH:mm"))
                          : QString();
}

// ---- Symbol tracing (adhoc #1607) --------------------------------------
// One `git grep` over the tree at the current ref answers "where else does this
// name appear"; the heuristics in CodeSymbols.h turn those lines into "which of
// them are definitions", "which are calls" and "which function is each call
// inside", which is what makes a trace step. Everything here runs on a worker
// thread (see MainWindow::runFilesSymbolQuery), so it touches nothing but its
// by-value arguments — no MainWindow state, no widgets.

constexpr int kSymbolGrepTimeoutMs = 8000;
constexpr int kSymbolShowTimeoutMs = 4000;
constexpr int kMaxSymbolRows = 300;
// Reading a file per call site is the expensive half of a caller trace, so the
// number of distinct files a single trace will open is capped.
constexpr int kMaxTraceFiles = 60;
// How deep a row can be expanded before the trace stops offering another step.
constexpr int kMaxTraceDepth = 8;

// One row of the trace panel. `symbol` is what expanding it traces next; an
// empty one is a leaf.
struct SymbolRow {
    QString label;
    QString detail;
    QString path;
    int line = 0;
    QString symbol;
    bool expandable = false;
};

// Run git in `dir` on a worker thread, returning stdout's non-empty lines. A
// hung git is killed at the timeout and reads as "nothing found" rather than
// stalling the trace.
// code-health: allow(blocking-wait) only reached via traceSymbolRows, which runs in runOffThread from runFilesSymbolQuery/expandFilesSymbolRow
QStringList symbolGitLines(const QString &dir, const QStringList &args,
                           int timeoutMs)
{
    if (dir.isEmpty())
        return {};
    QProcess git;
    git.start(QStringLiteral("git"),
              QStringList{QStringLiteral("-C"), dir} + args);
    if (!git.waitForFinished(timeoutMs)) {
        git.kill();
        git.waitForFinished(1000);
        return {};
    }
    return QString::fromUtf8(git.readAllStandardOutput())
        .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
}

// The file's lines at `ref`, so a trace works on a bare mirror too — there may
// be no working tree to read from.
// code-health: allow(blocking-wait) only reached via traceSymbolRows, which runs in runOffThread from runFilesSymbolQuery/expandFilesSymbolRow
QStringList symbolFileLines(const QString &dir, const QString &ref,
                            const QString &path)
{
    QProcess git;
    git.start(QStringLiteral("git"),
              {QStringLiteral("-C"), dir, QStringLiteral("show"),
               ref + QLatin1Char(':') + path});
    if (!git.waitForFinished(kSymbolShowTimeoutMs)) {
        git.kill();
        git.waitForFinished(1000);
        return {};
    }
    return QString::fromUtf8(git.readAllStandardOutput())
        .split(QLatin1Char('\n'));
}

// Every word-boundary match of `symbol` in the tree at `ref`. Issue metadata
// and the pulls worktree are excluded: they are signed records that happen to
// quote code, not code.
QVector<forkmesh::search::CodeRow> symbolMatches(const QString &dir,
                                                 const QString &ref,
                                                 const QString &symbol)
{
    QVector<forkmesh::search::CodeRow> rows;
    if (dir.isEmpty() || ref.isEmpty() || symbol.isEmpty())
        return rows;
    const QStringList raw = symbolGitLines(
        dir,
        {QStringLiteral("grep"), QStringLiteral("-n"), QStringLiteral("-I"),
         QStringLiteral("-w"), QStringLiteral("-F"), QStringLiteral("-e"), symbol,
         ref, QStringLiteral("--"), QStringLiteral(":!.forkmesh/issues"),
         QStringLiteral(":!pulls")},
        kSymbolGrepTimeoutMs);
    for (const QString &line : raw) {
        const forkmesh::search::CodeRow row =
            forkmesh::search::parseGrepRow(line, ref);
        if (row.valid)
            rows.append(row);
        if (rows.size() >= 4000)
            break; // a name like "i" would otherwise pull in the whole tree
    }
    return rows;
}

QString trimmedSnippet(const QString &text)
{
    const QString clean = text.trimmed();
    return clean.size() > 160
               ? clean.left(157) + QString::fromUtf8("\xE2\x80\xA6")
               : clean;
}

// The rows for one trace step. `mode` is MainWindow::FilesSymbolMode.
QVector<SymbolRow> traceSymbolRows(const QString &dir, const QString &ref,
                                   const QString &symbol, int mode)
{
    using namespace forkmesh::code;
    QVector<SymbolRow> rows;
    const QVector<forkmesh::search::CodeRow> matches =
        symbolMatches(dir, ref, symbol);

    if (mode == TraceDefinitions || mode == TraceReferences) {
        const bool definitionsOnly = mode == TraceDefinitions;
        for (const forkmesh::search::CodeRow &row : matches) {
            const bool defines = definesSymbol(row.text, symbol);
            if (definitionsOnly && !defines)
                continue;
            SymbolRow out;
            out.path = row.path;
            out.line = row.line;
            out.label = QStringLiteral("%1:%2").arg(row.path).arg(row.line);
            out.detail = trimmedSnippet(row.text);
            // A definition is worth pointing at even in the flat reference
            // list — it is usually the row the reader wants.
            if (defines && !definitionsOnly)
                out.detail = QString::fromUtf8("\xE2\x97\x86 ") + out.detail;
            rows.append(out);
            if (rows.size() >= kMaxSymbolRows)
                break;
        }
        return rows;
    }

    if (mode == TraceCallers) {
        // Backwards: every call site, named by the function it sits inside, so
        // the row can be expanded to find *that* function's callers in turn.
        QHash<QString, int> seen; // "caller\x1fpath" -> index into rows
        QHash<QString, QStringList> fileCache;
        QVector<int> extraSites;  // repeat calls from an already-listed caller
        for (const forkmesh::search::CodeRow &row : matches) {
            if (!callsSymbol(row.text, symbol))
                continue;
            if (!fileCache.contains(row.path)) {
                if (fileCache.size() >= kMaxTraceFiles)
                    break;
                fileCache.insert(row.path, symbolFileLines(dir, ref, row.path));
            }
            const Enclosing owner =
                enclosingDefinition(fileCache.value(row.path), row.line);
            const QString name = owner.name.isEmpty()
                                     ? QStringLiteral("(file scope)")
                                     : owner.name;
            const QString key = name + QChar(0x1f) + row.path;
            const int existing = seen.value(key, -1);
            if (existing >= 0) {
                // A function that calls this one several times is one caller,
                // not several: the row keeps the first call site and counts the
                // rest.
                ++extraSites[existing];
                continue;
            }
            SymbolRow out;
            out.label = name;
            out.detail = QStringLiteral("%1:%2  %3")
                             .arg(row.path)
                             .arg(row.line)
                             .arg(trimmedSnippet(row.text));
            out.path = row.path;
            out.line = row.line;
            out.symbol = owner.name; // empty at file scope: nothing to trace on
            out.expandable = !owner.name.isEmpty();
            seen.insert(key, rows.size());
            rows.append(out);
            extraSites.append(0);
            if (rows.size() >= kMaxSymbolRows)
                break;
        }
        for (int i = 0; i < rows.size(); ++i) {
            if (extraSites.value(i) > 0)
                rows[i].detail += QString::fromUtf8(" \xC2\xB7 +%1 more here")
                                      .arg(extraSites.at(i));
        }
        return rows;
    }

    // Forwards: find where this symbol is defined, then list what its body
    // calls. Each of those can be expanded to take the next step out.
    for (const forkmesh::search::CodeRow &row : matches) {
        if (!definesSymbol(row.text, symbol))
            continue;
        const QStringList lines = symbolFileLines(dir, ref, row.path);
        if (lines.isEmpty())
            continue;
        for (const CallSite &call : callsInBody(lines, row.line)) {
            SymbolRow out;
            out.label = call.name;
            out.detail = QStringLiteral("%1:%2  %3")
                             .arg(row.path)
                             .arg(call.line)
                             .arg(trimmedSnippet(lines.value(call.line - 1)));
            out.path = row.path;
            out.line = call.line;
            out.symbol = call.name;
            out.expandable = true;
            rows.append(out);
            if (rows.size() >= kMaxSymbolRows)
                return rows;
        }
        break; // the first definition is the one traced
    }
    return rows;
}

// Trace-panel row payloads.
constexpr int kSymbolPathRole = Qt::UserRole;
constexpr int kSymbolLineRole = Qt::UserRole + 1;
constexpr int kSymbolNameRole = Qt::UserRole + 2;
constexpr int kSymbolLoadedRole = Qt::UserRole + 3;
constexpr int kSymbolDepthRole = Qt::UserRole + 4;

const char *const kSymbolModeTitles[] = {"defines", "references", "callers of",
                                         "calls from"};

// Fill `parent` (or the tree's top level) with one step's rows.
void fillSymbolRows(QTreeWidget *tree, QTreeWidgetItem *parent,
                    const QVector<SymbolRow> &rows, int depth)
{
    for (const SymbolRow &row : rows) {
        auto *item = parent ? new QTreeWidgetItem(parent)
                            : new QTreeWidgetItem(tree);
        item->setText(0, row.detail.isEmpty()
                             ? row.label
                             : row.label + QStringLiteral("   ") + row.detail);
        item->setToolTip(0, row.path.isEmpty()
                                ? row.label
                                : QStringLiteral("%1:%2").arg(row.path).arg(row.line));
        item->setData(0, kSymbolPathRole, row.path);
        item->setData(0, kSymbolLineRole, row.line);
        item->setData(0, kSymbolNameRole, row.symbol);
        item->setData(0, kSymbolLoadedRole, false);
        item->setData(0, kSymbolDepthRole, depth);
        // A placeholder child is what draws the expand arrow before the next
        // step has been traced (the same trick the file explorer uses).
        if (row.expandable && depth < kMaxTraceDepth)
            item->addChild(new QTreeWidgetItem);
    }
}

constexpr int kFilesTabMaxWidth = 320;
constexpr int kFilesTreeRowHeight = 28;
constexpr int kFilesTreeCompactRowHeight = 26;
constexpr int kFilesTreeFontPx = 13;
constexpr int kFilesTreeIconSize = 16;
constexpr int kFilesTreeChevronSize = 12;
constexpr int kFilesTreePadX = 8;
constexpr int kFilesTreeIconInset = 2;
constexpr int kFilesTreeIconGap = 8;
constexpr int kFilesTreeRowInset = 1;
constexpr int kFilesTreeRowRadius = 6;
constexpr int kFilesTreeIndent = 16;
constexpr int kFilesTreeClassicIndent = 14;
constexpr int kFilesCommitMessageWidth = 200;
constexpr int kFilesCommitMessageClassicWidth = 240;
constexpr int kFilesCommitMessageMaxWidth = 360;
constexpr int kFilesToolbarSpacing = 4;
constexpr int kFilesRuleHeight = 16;
constexpr int kFilesRuleGap = 4;
constexpr int kFilesTabSpaces = 4;
constexpr int kFilesClassicPanelInset = 2;
constexpr int kFilesSymbolPanelTop = 6;
constexpr qreal kFilesPreviewMargin = 24.0;

class FilesTabBar : public QTabBar
{
public:
    using QTabBar::QTabBar;

protected:
    QSize tabSizeHint(int index) const override
    {
        QSize hint = QTabBar::tabSizeHint(index);
        hint.setWidth(qMin(hint.width(), kFilesTabMaxWidth));
        return hint;
    }

    QSize minimumTabSizeHint(int index) const override
    {
        return tabSizeHint(index);
    }
};

class FilesTabWidget : public QTabWidget
{
public:
    explicit FilesTabWidget(QWidget *parent = nullptr) : QTabWidget(parent)
    {
        auto *bar = new FilesTabBar(this);
        bar->setObjectName(QStringLiteral("qt_tabwidget_tabbar"));
        bar->setDrawBase(false);
        setTabBar(bar);
    }
};

QPixmap filesFolderPixmap(const QIcon &icon, bool open, const QColor &color, qreal dpr)
{
    static QHash<QString, QPixmap> cache;
    const QString key = QString::number(open ? 1 : 0) + QLatin1Char('|') +
                        QString::number(color.rgba(), 16) + QLatin1Char('|') +
                        QString::number(dpr);
    const auto cached = cache.constFind(key);
    if (cached != cache.constEnd())
        return cached.value();
    QPixmap pixmap =
        icon.pixmap(QSize(kFilesTreeIconSize, kFilesTreeIconSize), dpr);
    if (pixmap.isNull())
        return pixmap;
    QPainter painter(&pixmap);
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(QRect(QPoint(0, 0), pixmap.size()), color);
    painter.end();
    cache.insert(key, pixmap);
    return pixmap;
}

class FilesTreeDelegate : public HoverRowDelegate
{
public:
    FilesTreeDelegate(QAbstractItemView *view, bool folders)
        : HoverRowDelegate(view), m_folders(folders)
    {
    }

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override
    {
        if (!Theme::isNext())
            return HoverRowDelegate::sizeHint(option, index);
        int width = QFontMetrics(cellFont(option.font, index.column()))
                        .horizontalAdvance(displayText(index.data(Qt::DisplayRole),
                                                       option.locale)) +
                    2 * kFilesTreePadX + 2;
        if (index.column() == 0 && index.data(Qt::DecorationRole).isValid())
            width += kFilesTreeIconSize + kFilesTreeIconGap;
        return QSize(width, rowHeight(option.font));
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        if (!Theme::isNext()) {
            HoverRowDelegate::paint(painter, option, index);
            return;
        }
        const auto *view = qobject_cast<const QAbstractItemView *>(option.widget);
        const auto *tree = qobject_cast<const QTreeView *>(option.widget);
        const bool first = index.column() == 0;
        const bool enabled = index.flags().testFlag(Qt::ItemIsEnabled);
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        const bool hovered =
            enabled && option.state.testFlag(QStyle::State_MouseOver);
        const bool folder =
            m_folders && index.siblingAtColumn(0).data(kFileTreeIsDirRole).toBool();

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
        if (view && (selected || hovered)) {
            QRect clip = option.rect;
            if (first)
                clip.setLeft(0);
            painter->save();
            painter->setClipRect(clip, Qt::IntersectClip);
            painter->setPen(Qt::NoPen);
            painter->setBrush(Theme::color(selected ? Theme::Role::SurfaceActive
                                                    : Theme::Role::SurfaceRaised));
            painter->drawRoundedRect(
                QRectF(0, option.rect.top() + kFilesTreeRowInset,
                       view->viewport()->width(),
                       option.rect.height() - 2 * kFilesTreeRowInset),
                kFilesTreeRowRadius, kFilesTreeRowRadius);
            painter->restore();
        }

        const QString text =
            displayText(index.data(Qt::DisplayRole), option.locale);
        const QFont font = cellFont(option.font, index.column());
        Theme::Role ink = Theme::Role::FgMuted;
        if (!enabled)
            ink = Theme::Role::FgSubtle;
        else if (first)
            ink = selected || hovered || folder ? Theme::Role::Fg
                                                : Theme::Role::FgSecondary;
        else if (selected)
            ink = Theme::Role::FgSecondary;

        int left = option.rect.left() + kFilesTreePadX;
        if (first) {
            left = option.rect.left() + kFilesTreeIconInset;
            if (tree && index.model()->hasChildren(index)) {
                const int indent = tree->indentation();
                painter->drawPixmap(
                    glyphRect(option.rect,
                              option.rect.left() - indent +
                                  (indent - kFilesTreeChevronSize) / 2,
                              kFilesTreeChevronSize),
                    tintedOcticonPixmap(tree->isExpanded(index)
                                            ? QStringLiteral("chevron-down")
                                            : QStringLiteral("chevron-right"),
                                        Theme::color(hovered ? Theme::Role::Fg
                                                             : Theme::Role::FgMuted),
                                        kFilesTreeChevronSize));
            }
            const QIcon icon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
            if (!icon.isNull()) {
                const QRect iconRect =
                    glyphRect(option.rect, left, kFilesTreeIconSize);
                if (folder)
                    painter->drawPixmap(
                        iconRect,
                        filesFolderPixmap(icon, tree && tree->isExpanded(index),
                                          Theme::color(Theme::Role::FgMuted),
                                          painter->device()->devicePixelRatioF()));
                else
                    icon.paint(painter, iconRect);
                left += kFilesTreeIconSize + kFilesTreeIconGap;
            }
        }

        const QRect textRect(left, option.rect.top(),
                             qMax(0, option.rect.right() + 1 - kFilesTreePadX - left),
                             option.rect.height());
        const QVariant alignment = index.data(Qt::TextAlignmentRole);
        const int horizontal =
            alignment.isValid() ? (alignment.toInt() & Qt::AlignHorizontal_Mask)
                                : int(Qt::AlignLeft);
        painter->setFont(font);
        painter->setPen(Theme::color(ink));
        painter->drawText(textRect,
                          horizontal | Qt::AlignVCenter | Qt::TextSingleLine,
                          QFontMetrics(font).elidedText(text, Qt::ElideRight,
                                                        textRect.width()));
        painter->restore();
    }

private:
    static QFont cellFont(const QFont &base, int column)
    {
        QFont font = base;
        if (column > 0 && base.pixelSize() > 11)
            font.setPixelSize(base.pixelSize() - 1);
        return font;
    }

    static int rowHeight(const QFont &font)
    {
        return font.pixelSize() > 0 && font.pixelSize() < kFilesTreeFontPx
                   ? kFilesTreeCompactRowHeight
                   : kFilesTreeRowHeight;
    }

    static QRect glyphRect(const QRect &cell, int left, int size)
    {
        return QRect(left, cell.top() + (cell.height() - size) / 2, size, size);
    }

    bool m_folders = true;
};

void installFilesTreeDelegate(QAbstractItemView *view, bool folders)
{
    if (!view)
        return;
    QAbstractItemDelegate *previous = view->itemDelegate();
    view->setItemDelegate(new FilesTreeDelegate(view, folders));
    if (previous)
        previous->deleteLater();
}

class FilesEditorHook : public QObject
{
public:
    explicit FilesEditorHook(QPlainTextEdit *editor)
        : QObject(editor), m_editor(editor),
          m_classicTabStop(editor->tabStopDistance()),
          m_vertical(editor->verticalScrollBarPolicy()),
          m_horizontal(editor->horizontalScrollBarPolicy())
    {
        editor->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        const QEvent::Type type = event->type();
        if (watched == m_editor) {
            if (type == QEvent::ChildPolished)
                adoptPreview();
            else if (type == QEvent::FontChange || type == QEvent::Polish ||
                     type == QEvent::StyleChange)
                sync();
        } else if (watched == m_preview &&
                   (type == QEvent::Show || type == QEvent::Hide)) {
            sync();
        }
        return QObject::eventFilter(watched, event);
    }

private:
    void adoptPreview()
    {
        auto *preview = m_editor->findChild<QTextBrowser *>(
            QStringLiteral("markdownPreview"), Qt::FindDirectChildrenOnly);
        if (!preview || preview == m_preview)
            return;
        m_preview = preview;
        m_classicPreviewMargin = preview->document()->documentMargin();
        preview->installEventFilter(this);
        sync();
    }

    void sync()
    {
        const bool next = Theme::isNext();
        const qreal tabStop =
            next ? QFontMetricsF(m_editor->font())
                           .horizontalAdvance(QLatin1Char(' ')) *
                       kFilesTabSpaces
                 : m_classicTabStop;
        if (!qFuzzyCompare(m_editor->tabStopDistance() + 1.0, tabStop + 1.0))
            m_editor->setTabStopDistance(tabStop);
        if (m_preview) {
            const qreal margin = next ? kFilesPreviewMargin : m_classicPreviewMargin;
            if (!qFuzzyCompare(m_preview->document()->documentMargin() + 1.0,
                               margin + 1.0))
                m_preview->document()->setDocumentMargin(margin);
        }
        const bool covered = next && m_preview && !m_preview->isHidden();
        const Qt::ScrollBarPolicy vertical =
            covered ? Qt::ScrollBarAlwaysOff : m_vertical;
        const Qt::ScrollBarPolicy horizontal =
            covered ? Qt::ScrollBarAlwaysOff : m_horizontal;
        if (m_editor->verticalScrollBarPolicy() != vertical)
            m_editor->setVerticalScrollBarPolicy(vertical);
        if (m_editor->horizontalScrollBarPolicy() != horizontal)
            m_editor->setHorizontalScrollBarPolicy(horizontal);
    }

    QPlainTextEdit *m_editor = nullptr;
    QPointer<QTextBrowser> m_preview;
    qreal m_classicTabStop = 0.0;
    qreal m_classicPreviewMargin = 4.0;
    Qt::ScrollBarPolicy m_vertical = Qt::ScrollBarAsNeeded;
    Qt::ScrollBarPolicy m_horizontal = Qt::ScrollBarAsNeeded;
};

class FilesLookHook : public QObject
{
public:
    FilesLookHook(QWidget *page, std::function<void()> apply)
        : QObject(page), m_page(page), m_apply(std::move(apply))
    {
        m_settle.setSingleShot(true);
        m_settle.setInterval(0);
        connect(&m_settle, &QTimer::timeout, this, [this] { m_apply(); });
        page->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == m_page && (event->type() == QEvent::Polish ||
                                  event->type() == QEvent::StyleChange)) {
            m_apply();
            m_settle.start();
        }
        return QObject::eventFilter(watched, event);
    }

private:
    QWidget *m_page = nullptr;
    std::function<void()> m_apply;
    QTimer m_settle;
};

struct FilesHeaderParts {
    QBoxLayout *heading = nullptr;
    QBoxLayout *toolbar = nullptr;
    QWidget *title = nullptr;
    QWidget *status = nullptr;
    QWidget *hidden = nullptr;
    QWidget *size = nullptr;
    QWidget *up = nullptr;
    QWidget *home = nullptr;
    QWidget *repo = nullptr;
    QWidget *coves = nullptr;
    QWidget *network = nullptr;
    QWidget *choose = nullptr;
    QWidget *refresh = nullptr;
    QWidget *open = nullptr;
    QWidget *path = nullptr;
    QWidget *sourceRule = nullptr;
    QWidget *actionRule = nullptr;
};

void emptyFilesRow(QBoxLayout *row)
{
    while (QLayoutItem *item = row->takeAt(0))
        delete item;
}

void arrangeFilesHeader(const FilesHeaderParts &parts, bool next)
{
    emptyFilesRow(parts.heading);
    emptyFilesRow(parts.toolbar);
    parts.sourceRule->setVisible(next);
    parts.actionRule->setVisible(next);
    parts.status->setSizePolicy(next ? QSizePolicy::Ignored : QSizePolicy::Preferred,
                                QSizePolicy::Preferred);
    parts.heading->addWidget(parts.title);
    if (!next) {
        parts.heading->addWidget(parts.status);
        parts.heading->addStretch();
        for (QWidget *widget :
             {parts.hidden, parts.size, parts.up, parts.home, parts.repo,
              parts.coves, parts.network, parts.choose, parts.refresh, parts.open})
            parts.heading->addWidget(widget);
        parts.toolbar->addWidget(parts.path);
        return;
    }
    parts.heading->addWidget(parts.status, 1);
    parts.heading->addWidget(parts.hidden);
    parts.heading->addWidget(parts.size);
    const auto addRule = [&parts](QWidget *rule) {
        parts.toolbar->addSpacing(kFilesRuleGap);
        parts.toolbar->addWidget(rule);
        parts.toolbar->addSpacing(kFilesRuleGap);
    };
    parts.toolbar->addWidget(parts.repo);
    parts.toolbar->addWidget(parts.coves);
    parts.toolbar->addWidget(parts.network);
    addRule(parts.sourceRule);
    for (QWidget *widget : {parts.up, parts.home, parts.choose})
        parts.toolbar->addWidget(widget);
    parts.toolbar->addWidget(parts.path, 1);
    addRule(parts.actionRule);
    parts.toolbar->addWidget(parts.refresh);
    parts.toolbar->addWidget(parts.open);
}

QFrame *filesToolbarRule(QWidget *page)
{
    auto *rule = new QFrame(page);
    rule->setProperty("filesRule", true);
    rule->setFrameShape(QFrame::NoFrame);
    rule->setFixedSize(1, kFilesRuleHeight);
    rule->hide();
    return rule;
}

void tintFilesQuietButton(QPushButton *button)
{
    if (!button)
        return;
    if (!Theme::isNext()) {
        applyStoredOcticon(button);
        return;
    }
    const QString name = button->property("forkmeshOcticon").toString();
    if (name.isEmpty())
        return;
    const int size = button->property("forkmeshOcticonSize").toInt();
    button->setIcon(themedOcticon(name, Theme::color(Theme::Role::FgSecondary),
                                  size > 0 ? size : 16));
}

QString filesBreadcrumbMarkup(const QString &trail)
{
    const QString separator = QString::fromUtf8(" \xE2\x80\xBA ");
    const int cut = trail.lastIndexOf(separator);
    const QString head = cut < 0 ? QString() : trail.left(cut + separator.size());
    const QString leaf = cut < 0 ? trail : trail.mid(cut + separator.size());
    return QStringLiteral("<span style=\"white-space:pre; color:%1\">%2</span>"
                          "<span style=\"white-space:pre; color:%3\">%4</span>")
        .arg(Theme::hex(Theme::Role::FgMuted), head.toHtmlEscaped(),
             Theme::hex(Theme::Role::Fg), leaf.toHtmlEscaped());
}
} // namespace

QWidget *MainWindow::buildFilesSection()
{
    auto *page = new QWidget;
    page->setObjectName(QStringLiteral("filesSection"));
    auto *outer = new QVBoxLayout(page);
    outer->setContentsMargins(20, 18, 20, 20);
    outer->setSpacing(10);

    auto *heading = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("Files"));
    title->setObjectName(QStringLiteral("pageTitle"));
    heading->addWidget(title);
    m_fileExplorerStatus = new QLabel;
    m_fileExplorerStatus->setObjectName(QStringLiteral("mutedLabel"));
    m_fileExplorerStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    heading->addWidget(m_fileExplorerStatus);
    heading->addStretch();

    m_fileExplorerHidden = new QCheckBox(QStringLiteral("Show hidden"));
    m_fileExplorerHidden->setCursor(Qt::PointingHandCursor);
    m_fileExplorerHidden->setToolTip(
        QStringLiteral("List dotfiles and hidden directories (.git, .forkmesh, "
                       ".config …)"));
    m_fileExplorerHidden->setChecked(
        QSettings().value(QLatin1String(kFilesHiddenSetting), true).toBool());
    connect(m_fileExplorerHidden, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue(QLatin1String(kFilesHiddenSetting), on);
        setFileExplorerRoot(m_fileExplorerRoot);
    });
    heading->addWidget(m_fileExplorerHidden);

    // Sizes are off until asked for (adhoc #1634). The column is dead width for
    // most browsing, so it starts hidden on both trees and this toggle — like
    // "Show hidden" — is remembered across launches.
    m_filesSizeButton = new QPushButton(QStringLiteral("Size"));
    m_filesSizeButton->setObjectName(QStringLiteral("ghostButton"));
    m_filesSizeButton->setCheckable(true);
    m_filesSizeButton->setCursor(Qt::PointingHandCursor);
    m_filesSizeButton->setToolTip(
        QStringLiteral("Show each file's size beside its name"));
    setOcticon(m_filesSizeButton, "graph", 14);
    m_filesSizeButton->setChecked(
        QSettings().value(QLatin1String(kFilesShowSizeSetting), false).toBool());
    connect(m_filesSizeButton, &QPushButton::toggled, this, [this](bool on) {
        QSettings().setValue(QLatin1String(kFilesShowSizeSetting), on);
        setFilesSizeColumnVisible(on);
    });
    heading->addWidget(m_filesSizeButton);

    auto *upButton = new QPushButton(QStringLiteral("Up"));
    upButton->setObjectName(QStringLiteral("ghostButton"));
    upButton->setCursor(Qt::PointingHandCursor);
    upButton->setToolTip(QStringLiteral("Move the root to the parent directory"));
    setOcticon(upButton, "chevron-up", 14);
    connect(upButton, &QPushButton::clicked, this, [this] {
        QDir dir(m_fileExplorerRoot);
        if (dir.cdUp())
            setFileExplorerRoot(dir.absolutePath());
    });
    heading->addWidget(upButton);

    auto *homeButton = new QPushButton(QStringLiteral("Home"));
    homeButton->setObjectName(QStringLiteral("ghostButton"));
    homeButton->setCursor(Qt::PointingHandCursor);
    homeButton->setToolTip(QStringLiteral("Browse this project's folder"));
    setOcticon(homeButton, "home", 14);
    connect(homeButton, &QPushButton::clicked, this,
            [this] { setFileExplorerRoot(forkmesh::paths::projectDir()); });
    heading->addWidget(homeButton);

    m_filesRepoTreeButton = new QPushButton(QStringLiteral("Repo"));
    m_filesRepoTreeButton->setObjectName(QStringLiteral("ghostButton"));
    m_filesRepoTreeButton->setCheckable(true);
    m_filesRepoTreeButton->setCursor(Qt::PointingHandCursor);
    m_filesRepoTreeButton->setToolTip(
        QStringLiteral("Browse the open repository: the files it tracks and the "
                       "untracked and ignored ones beside them"));
    setOcticon(m_filesRepoTreeButton, "repo", 14);
    connect(m_filesRepoTreeButton, &QPushButton::clicked, this, [this] {
        showFilesRepoTree();
        if (m_openFileTabs.isEmpty())
            openRepoReadme();
    });
    heading->addWidget(m_filesRepoTreeButton);

    m_filesCoveTreeButton = new QPushButton(QStringLiteral("Coves"));
    m_filesCoveTreeButton->setObjectName(QStringLiteral("ghostButton"));
    m_filesCoveTreeButton->setCheckable(true);
    m_filesCoveTreeButton->setCursor(Qt::PointingHandCursor);
    m_filesCoveTreeButton->setToolTip(QStringLiteral(
        "Browse encrypted Cove documents in the same file editor"));
    setOcticon(m_filesCoveTreeButton, "shield-check", 14);
    connect(m_filesCoveTreeButton, &QPushButton::clicked, this,
            [this] { showFilesCoveTree(); });
    heading->addWidget(m_filesCoveTreeButton);

    m_filesNetworkTreeButton = new QPushButton(QStringLiteral("Network"));
    m_filesNetworkTreeButton->setObjectName(QStringLiteral("ghostButton"));
    m_filesNetworkTreeButton->setCheckable(true);
    m_filesNetworkTreeButton->setCursor(Qt::PointingHandCursor);
    m_filesNetworkTreeButton->setToolTip(QStringLiteral(
        "Browse folders shared on your network by a router, NAS or computer (SMB)"));
    setOcticon(m_filesNetworkTreeButton, "server", 14);
    connect(m_filesNetworkTreeButton, &QPushButton::clicked, this,
            [this] { showFilesNetworkTree(); });
    heading->addWidget(m_filesNetworkTreeButton);

    auto *chooseButton = new QPushButton(QStringLiteral("Choose"));
    chooseButton->setObjectName(QStringLiteral("ghostButton"));
    chooseButton->setCursor(Qt::PointingHandCursor);
    chooseButton->setToolTip(QStringLiteral("Pick a directory to browse"));
    setOcticon(chooseButton, "file-directory", 14);
    connect(chooseButton, &QPushButton::clicked, this, [this] {
        const QString picked = chooseExistingDirectory(
            this, QStringLiteral("Choose a directory"), m_fileExplorerRoot);
        if (!picked.isEmpty())
            setFileExplorerRoot(picked);
    });
    heading->addWidget(chooseButton);

    auto *refreshButton = new QPushButton(QStringLiteral("Refresh"));
    refreshButton->setObjectName(QStringLiteral("ghostButton"));
    refreshButton->setCursor(Qt::PointingHandCursor);
    refreshButton->setToolTip(QStringLiteral("Re-read whichever tree is showing"));
    setOcticon(refreshButton, "sync", 14);
    connect(refreshButton, &QPushButton::clicked, this, [this] {
        if (m_filesTreeStack && m_filesTreeStack->currentIndex() == 3 && m_networkFilesPanel)
            m_networkFilesPanel->reload();
        else if (m_filesTreeStack && m_filesTreeStack->currentIndex() == 2)
            loadCoveExplorer();
        else if (m_filesTreeStack && m_filesTreeStack->currentIndex() == 1)
            loadRepoFileTree();
        else
            setFileExplorerRoot(m_fileExplorerRoot);
    });
    heading->addWidget(refreshButton);

    auto *openButton = new QPushButton(QStringLiteral("Open"));
    openButton->setObjectName(QStringLiteral("primaryButton"));
    openButton->setCursor(Qt::PointingHandCursor);
    openButton->setToolTip(
        QStringLiteral("Open the current directory in the desktop file manager"));
    setOcticon(openButton, "link", 14);
    connect(openButton, &QPushButton::clicked, this, [this] {
        if (!m_fileExplorerRoot.isEmpty())
            QDesktopServices::openUrl(QUrl::fromLocalFile(m_fileExplorerRoot));
    });
    heading->addWidget(openButton);
    outer->addLayout(heading);

    m_fileExplorerPath = new QLineEdit;
    m_fileExplorerPath->setObjectName(QStringLiteral("filesPathBar"));
    m_fileExplorerPath->setPlaceholderText(
        QStringLiteral("Path to browse, e.g. %1").arg(forkmesh::paths::projectDir()));
    m_fileExplorerPath->setClearButtonEnabled(true);
    connect(m_fileExplorerPath, &QLineEdit::returnPressed, this,
            [this] { setFileExplorerRoot(m_fileExplorerPath->text()); });
    auto *toolbar = new QHBoxLayout;
    toolbar->setContentsMargins(0, 0, 0, 0);
    toolbar->setSpacing(kFilesToolbarSpacing);
    toolbar->addWidget(m_fileExplorerPath);
    outer->addLayout(toolbar);
    auto *editorBar = qobject_cast<QBoxLayout *>(buildFilesEditorBar());
    outer->addLayout(editorBar);

    auto *splitter = new QSplitter(Qt::Horizontal);
    splitter->setObjectName(QStringLiteral("filesExplorerSplitter"));
    splitter->setChildrenCollapsible(false);

    m_fileExplorerTree = new QTreeWidget;
    m_fileExplorerTree->setObjectName(QStringLiteral("fileTree"));
    enableHoverRowHighlight(m_fileExplorerTree);
    installFilesTreeDelegate(m_fileExplorerTree, true);
    m_fileExplorerTree->setColumnCount(3);
    m_fileExplorerTree->setHeaderLabels(
        {QStringLiteral("Name"), QStringLiteral("Size"),
         QStringLiteral("Modified")});
    m_fileExplorerTree->header()->setStretchLastSection(false);
    m_fileExplorerTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_fileExplorerTree->header()->setSectionResizeMode(
        1, QHeaderView::ResizeToContents);
    m_fileExplorerTree->header()->setSectionResizeMode(
        2, QHeaderView::ResizeToContents);
    m_fileExplorerTree->setMinimumWidth(320);
    m_fileExplorerTree->setIndentation(14);
    m_fileExplorerTree->setUniformRowHeights(true);
    m_fileExplorerTree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_fileExplorerTree, &QWidget::customContextMenuRequested, this,
            &MainWindow::showFileExplorerMenu);
    connectFileTreeFolderIcons(m_fileExplorerTree,
                               [this](bool opened) { return iconForDir(opened); });
    connect(m_fileExplorerTree, &QTreeWidget::itemExpanded, this,
            [this](QTreeWidgetItem *item) { populateFileExplorerItem(item); });
    connect(m_fileExplorerTree, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem *item, QTreeWidgetItem *) {
                previewFileExplorerFile(
                    item ? item->data(0, kFileTreePathRole).toString() : QString());
            });
    // A single click opens a file, matching the repository tree beside it.
    // Directories are left to the double-click below, which re-roots them.
    connect(m_fileExplorerTree, &QTreeWidget::itemClicked, this,
            [this](QTreeWidgetItem *item, int) {
                if (!item || fileTreeIsDir(item))
                    return;
                const QString path = item->data(0, kFileTreePathRole).toString();
                if (!path.isEmpty())
                    openLocalFileTab(path);
            });
    connect(m_fileExplorerTree, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem *item, int) {
                if (!item)
                    return;
                const QString path = item->data(0, kFileTreePathRole).toString();
                if (path.isEmpty())
                    return;
                if (fileTreeIsDir(item))
                    setFileExplorerRoot(path);
                else
                    openLocalFileTab(path);
            });

    m_repoFileTree = new QTreeWidget;
    m_repoFileTree->setObjectName(QStringLiteral("fileTree"));
    enableHoverRowHighlight(m_repoFileTree);
    installFilesTreeDelegate(m_repoFileTree, true);
    m_repoFileTree->setColumnCount(2);
    m_repoFileTree->setHeaderHidden(true);
    m_repoFileTree->header()->setStretchLastSection(false);
    m_repoFileTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_repoFileTree->header()->setSectionResizeMode(1,
                                                   QHeaderView::ResizeToContents);
    m_repoFileTree->setMinimumWidth(200);
    m_repoFileTree->setIndentation(14);
    m_repoFileTree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_repoFileTree, &QWidget::customContextMenuRequested, this,
            &MainWindow::showRepoFileTreeMenu);
    connect(m_repoFileTree, &QTreeWidget::itemClicked, this,
            [this](QTreeWidgetItem *item, int) {
                if (!item)
                    return;
                if (item->data(0, Qt::UserRole + 1).toBool())
                    item->setExpanded(!item->isExpanded());
                else
                    openRepoFile(item->data(0, Qt::UserRole).toString());
            });
    connect(m_repoFileTree, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem *item, int) {
                if (item && !item->data(0, Qt::UserRole + 1).toBool())
                    openRepoFile(item->data(0, Qt::UserRole).toString());
            });
    // A working-tree directory git reported as a single "other" row — an
    // ignored build directory, an untracked folder — lists its contents the
    // first time it is opened. Nothing in the checkout is unreachable from this
    // tree, and nothing outside the commit is listed until it is asked for.
    connect(m_repoFileTree, &QTreeWidget::itemExpanded, this,
            [this](QTreeWidgetItem *item) {
                const QString dir = repoGitDir();
                if (dir.isEmpty())
                    return;
                populateFileTreeDiskDir(
                    item, QDir(dir).filePath(fileTreePath(item)),
                    [this](QTreeWidgetItem *parent, const QString &rel,
                           const QFileInfo &entry) {
                        auto *child = new QTreeWidgetItem(parent);
                        child->setText(0, entry.fileName());
                        child->setData(0, kFileTreePathRole, rel);
                        child->setData(0, kFileTreeIsDirRole, entry.isDir());
                        child->setToolTip(0, entry.absoluteFilePath());
                        if (!entry.isDir()) {
                            child->setIcon(0, iconForFile(entry.fileName()));
                            setFileTreeSizeCell(child, 1,
                                                formatByteSize(entry.size()));
                            return;
                        }
                        child->setIcon(0, iconForDir(false));
                        // A symlinked directory gets no placeholder child, so
                        // one pointing at an ancestor cannot loop the tree.
                        if (!entry.isSymLink())
                            markFileTreeDiskDir(child);
                    });
            });
    connectFileTreeFolderIcons(m_repoFileTree,
                               [this](bool opened) { return iconForDir(opened); });

    m_filesTreeStack = new CurrentPageStack;
    m_filesTreeStack->addWidget(m_fileExplorerTree); // 0 filesystem
    m_filesTreeStack->addWidget(m_repoFileTree);     // 1 repository
    m_filesTreeStack->addWidget(buildFilesCovePanel()); // 2 encrypted Coves
    QTreeWidget *coveTree = m_filesTreeStack->widget(2)->findChild<QTreeWidget *>(
        QStringLiteral("fileTree"));
    installFilesTreeDelegate(coveTree, true);
    m_filesTreeStack->addWidget(buildFilesNetworkPanel());
    installFilesTreeDelegate(m_networkFilesPanel->tree(), true);
    splitter->addWidget(m_filesTreeStack);
    splitter->addWidget(buildFilesEditorColumn());

    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({420, 620});
    outer->addWidget(splitter, 1);

    // The IDE keys are scoped to this page. Ctrl+F is owned by MainWindow's
    // single window-wide dispatcher, which routes back to openFilesFindBar()
    // while this page is visible and opens page-find everywhere else.
    auto bind = [this, page](const QKeySequence &keys, auto handler) {
        auto *shortcut = new QShortcut(keys, page);
        shortcut->setContext(Qt::WidgetWithChildrenShortcut);
        connect(shortcut, &QShortcut::activated, this, handler);
    };
    bind(QKeySequence(Qt::CTRL | Qt::Key_H), [this] { openFilesFindBar(true); });
    bind(QKeySequence(Qt::Key_F3), [this] { stepFilesFind(1); });
    bind(QKeySequence(Qt::SHIFT | Qt::Key_F3), [this] { stepFilesFind(-1); });
    bind(QKeySequence(Qt::Key_Escape), [this] { closeFilesFindBar(); });
    bind(QKeySequence(Qt::CTRL | Qt::Key_G), [this] { goToLineInFilesEditor(); });
    bind(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O),
         [this] { showFilesSymbolOutline(); });
    bind(QKeySequence(Qt::Key_F12), [this] {
        traceFilesSymbol(filesEditorSymbol(), forkmesh::code::TraceDefinitions);
    });
    bind(QKeySequence(Qt::SHIFT | Qt::Key_F12), [this] {
        traceFilesSymbol(filesEditorSymbol(), forkmesh::code::TraceReferences);
    });
    bind(QKeySequence(Qt::ALT | Qt::SHIFT | Qt::Key_H), [this] {
        traceFilesSymbol(filesEditorSymbol(), forkmesh::code::TraceCallers);
    });

    // First root: whatever was browsed last, else the open repository's working
    // tree, else the project's folder (a new project has neither yet).
    QString root =
        QSettings().value(QLatin1String(kFilesRootSetting)).toString();
    if (root.isEmpty() || !QDir(root).exists())
        root = repoGitDir();
    if (root.isEmpty() || !QDir(root).exists())
        root = forkmesh::paths::projectDir();
    if (root.isEmpty() || !QDir(root).exists())
        root = QDir::homePath();
    // Both trees exist by now, so the remembered Size preference can land.
    setFilesSizeColumnVisible(m_filesSizeButton->isChecked());
    setFileExplorerRoot(root);
    updateRepoFileSaveActions();
    updateFilesCommitActions();

    openButton->setProperty("filesQuiet", true);
    m_repoFileSaveButton->setProperty("filesQuiet", true);
    FilesHeaderParts parts;
    parts.heading = heading;
    parts.toolbar = toolbar;
    parts.title = title;
    parts.status = m_fileExplorerStatus;
    parts.hidden = m_fileExplorerHidden;
    parts.size = m_filesSizeButton;
    parts.up = upButton;
    parts.home = homeButton;
    parts.repo = m_filesRepoTreeButton;
    parts.coves = m_filesCoveTreeButton;
    parts.network = m_filesNetworkTreeButton;
    parts.choose = chooseButton;
    parts.refresh = refreshButton;
    parts.open = openButton;
    parts.path = m_fileExplorerPath;
    parts.sourceRule = filesToolbarRule(page);
    parts.actionRule = filesToolbarRule(page);
    auto arranged = std::make_shared<int>(-1);
    const auto applyLook = [this, parts, arranged, editorBar, openButton, coveTree] {
        const bool next = Theme::isNext();
        if (*arranged != int(next)) {
            *arranged = int(next);
            arrangeFilesHeader(parts, next);
            for (QTreeWidget *tree : {m_fileExplorerTree, m_repoFileTree, coveTree,
                                      m_networkFilesPanel->tree()}) {
                if (!tree)
                    continue;
                tree->setIndentation(next ? kFilesTreeIndent
                                          : kFilesTreeClassicIndent);
                tree->doItemsLayout();
            }
            if (m_filesSymbolTree)
                m_filesSymbolTree->doItemsLayout();
            if (m_repoFileTabs)
                m_repoFileTabs->tabBar()->setDrawBase(!next);
            if (m_filesCommitMessage) {
                m_filesCommitMessage->setMinimumWidth(
                    next ? kFilesCommitMessageWidth
                         : kFilesCommitMessageClassicWidth);
                m_filesCommitMessage->setMaximumWidth(
                    next ? kFilesCommitMessageMaxWidth : QWIDGETSIZE_MAX);
                editorBar->setStretchFactor(m_filesCommitMessage, next ? 1 : 0);
            }
            const int inset = next ? 0 : kFilesClassicPanelInset;
            if (m_filesFindBar && m_filesFindBar->layout())
                m_filesFindBar->layout()->setContentsMargins(inset, 0, inset, 0);
            if (m_filesSymbolPanel && m_filesSymbolPanel->layout())
                m_filesSymbolPanel->layout()->setContentsMargins(
                    inset, kFilesSymbolPanelTop, inset, 0);
        }
        updateFilesEditorStatus();
        tintFilesQuietButton(openButton);
        tintFilesQuietButton(m_repoFileSaveButton);
    };
    applyLook();
    new FilesLookHook(page, applyLook);
    return page;
}

// The right-hand column of the Files splitter: a breadcrumb naming whichever
// file the open tab is showing, the find bar, the editor tabs themselves, and
// the symbol-trace panel under them. Everything but the tabs starts hidden.
QWidget *MainWindow::buildFilesEditorColumn()
{
    auto *column = new QWidget;
    column->setObjectName(QStringLiteral("filesEditorColumn"));
    auto *layout = new QVBoxLayout(column);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    // "owner/repo › src › MainWindow.cpp" on the left, "Ln 12, Col 4" on the
    // right: with several tabs open (and one of them the shared Preview) the
    // tab label alone doesn't say which file is on screen.
    auto *crumbRow = new QHBoxLayout;
    crumbRow->setContentsMargins(2, 0, 2, 0);
    crumbRow->setSpacing(10);
    m_filesBreadcrumb = new QLabel;
    m_filesBreadcrumb->setObjectName(QStringLiteral("mutedLabel"));
    m_filesBreadcrumb->setTextInteractionFlags(Qt::TextSelectableByMouse);
    // A deep path is long, and a label's natural width is its whole text — which
    // would push the window's minimum width out on a laptop screen (issue #369).
    // The text is elided to fit in updateFilesEditorStatus instead; the full
    // path stays in the tooltip.
    m_filesBreadcrumb->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_filesBreadcrumb->setProperty("filesRole", QStringLiteral("crumb"));
    crumbRow->addWidget(m_filesBreadcrumb, 1);
    m_filesCursorLabel = new QLabel;
    m_filesCursorLabel->setObjectName(QStringLiteral("mutedLabel"));
    m_filesCursorLabel->setProperty("filesRole", QStringLiteral("cursor"));
    crumbRow->addWidget(m_filesCursorLabel);
    layout->addLayout(crumbRow);

    layout->addWidget(buildFilesFindBar());

    m_repoFileTabs = new FilesTabWidget;
    m_repoFileTabs->setObjectName(QStringLiteral("fileTabs"));
    m_repoFileTabs->setDocumentMode(true);
    m_repoFileTabs->setMovable(true);
    m_repoFileTabs->setTabsClosable(true);
    // Many tabs scroll rather than shrink to nothing, and each keeps its full
    // path in the tooltip — the same "which file is this" problem the
    // breadcrumb above solves for the active one.
    m_repoFileTabs->setUsesScrollButtons(true);
    m_repoFileTabs->tabBar()->setElideMode(Qt::ElideMiddle);
    m_repoFileTabs->tabBar()->setExpanding(false);
    m_repoFileTabs->tabBar()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_repoFileTabs->tabBar(), &QWidget::customContextMenuRequested, this,
            &MainWindow::showFilesTabMenu);
    connect(m_repoFileTabs, &QTabWidget::tabCloseRequested, this,
            [this](int index) { closeFilesTab(index); });
    connect(m_repoFileTabs, &QTabWidget::currentChanged, this, [this] {
        updateRepoFileSaveActions();
        // The find bar's matches are cursors into one document, so a tab switch
        // has to re-run them against the document that just came up.
        if (m_filesFindBar && m_filesFindBar->isVisible())
            recomputeFilesFind();
        updateFilesEditorStatus();
        // Follow the tab in the tree only when the repository side is already
        // up: yanking someone off the filesystem tree they were browsing is
        // not what switching tabs asked for. The explicit "Reveal" actions
        // switch it deliberately.
        if (m_filesTreeStack && m_filesTreeStack->currentIndex() == 1)
            syncFilesTreeToOpenTab();
        else if (m_filesTreeStack && m_filesTreeStack->currentIndex() == 2)
            syncCoveTreeToOpenTab();
        // Each open file is a place on the Back/Forward trail, so opening one
        // from the file list (or switching tabs) is a step (adhoc #50).
        scheduleNavRecord();
    });
    auto *saveShortcut = new QShortcut(QKeySequence::Save, m_repoFileTabs);
    saveShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(saveShortcut, &QShortcut::activated, this, [this] {
        if (m_repoFileSaveButton && m_repoFileSaveButton->isEnabled())
            saveCurrentRepoFile(false);
        else if (m_repoFileCommitButton && m_repoFileCommitButton->isEnabled())
            saveCurrentRepoFile(false);
        else if (m_repoFilePullButton && m_repoFilePullButton->isEnabled())
            saveCurrentRepoFile(true);
    });

    m_fileExplorerPreview = new QPlainTextEdit;
    m_fileExplorerPreview->setObjectName(QStringLiteral("filesPreview"));
    m_fileExplorerPreview->setReadOnly(true);
    m_fileExplorerPreview->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_fileExplorerPreview->setPlaceholderText(
        QStringLiteral("Arrow-key through the tree to preview a file here, or "
                       "click one to open it in a tab."));
    wireFilesEditor(m_fileExplorerPreview);
    const int previewTab = m_repoFileTabs->addTab(m_fileExplorerPreview,
                                                  QStringLiteral("Preview"));
    m_repoFileTabs->setTabToolTip(
        previewTab, QStringLiteral("The selected file, read-only"));
    for (QTabBar::ButtonPosition side : {QTabBar::LeftSide, QTabBar::RightSide})
        m_repoFileTabs->tabBar()->setTabButton(previewTab, side, nullptr);

    m_filesEditorSplit = new QSplitter(Qt::Vertical);
    m_filesEditorSplit->setObjectName(QStringLiteral("filesEditorSplitter"));
    m_filesEditorSplit->setChildrenCollapsible(false);
    m_filesEditorSplit->addWidget(m_repoFileTabs);
    m_filesEditorSplit->addWidget(buildFilesSymbolPanel());
    // Adding a widget to a splitter can show it again, so the panel's "closed
    // until a trace opens it" state is set here, after it is in place.
    m_filesSymbolPanel->hide();
    m_filesEditorSplit->setStretchFactor(0, 1);
    m_filesEditorSplit->setStretchFactor(1, 0);
    layout->addWidget(m_filesEditorSplit, 1);
    return column;
}

// Ctrl+F / Ctrl+H over the open tab. Hidden until asked for; the replace row
// only appears for Ctrl+H, and only bites on an editable tab.
QWidget *MainWindow::buildFilesFindBar()
{
    m_filesFindBar = new QWidget;
    m_filesFindBar->setObjectName(QStringLiteral("filesFindBar"));
    auto *outer = new QVBoxLayout(m_filesFindBar);
    outer->setContentsMargins(2, 0, 2, 0);
    outer->setSpacing(4);

    auto *findRow = new QHBoxLayout;
    findRow->setContentsMargins(0, 0, 0, 0);
    findRow->setSpacing(6);
    m_filesFindInput = new QLineEdit;
    m_filesFindInput->setObjectName(QStringLiteral("issueSearch"));
    m_filesFindInput->setPlaceholderText(
        QString::fromUtf8("Find in this file\xE2\x80\xA6"));
    m_filesFindInput->setClearButtonEnabled(true);
    connect(m_filesFindInput, &QLineEdit::textChanged, this,
            [this] { recomputeFilesFind(); });
    connect(m_filesFindInput, &QLineEdit::returnPressed, this, [this] {
        stepFilesFind(QGuiApplication::keyboardModifiers() & Qt::ShiftModifier ? -1
                                                                              : 1);
    });
    findRow->addWidget(m_filesFindInput, 1);

    auto smallButton = [](QPushButton *button, const QString &tip) {
        button->setObjectName(QStringLiteral("ghostButton"));
        button->setProperty("buttonSize", "sm");
        button->setCursor(Qt::PointingHandCursor);
        button->setToolTip(tip);
    };
    m_filesFindCaseButton = new QPushButton(QStringLiteral("Aa"));
    m_filesFindCaseButton->setCheckable(true);
    smallButton(m_filesFindCaseButton, QStringLiteral("Match case"));
    connect(m_filesFindCaseButton, &QPushButton::toggled, this,
            [this] { recomputeFilesFind(); });
    m_filesFindWordButton = new QPushButton(QStringLiteral("Word"));
    m_filesFindWordButton->setCheckable(true);
    smallButton(m_filesFindWordButton, QStringLiteral("Whole word only"));
    connect(m_filesFindWordButton, &QPushButton::toggled, this,
            [this] { recomputeFilesFind(); });
    findRow->addWidget(m_filesFindCaseButton);
    findRow->addWidget(m_filesFindWordButton);

    m_filesFindCount = new QLabel;
    m_filesFindCount->setObjectName(QStringLiteral("hintLabel"));
    findRow->addWidget(m_filesFindCount);

    auto *prev = new QPushButton;
    setOcticon(prev, "chevron-up", 14);
    smallButton(prev, QStringLiteral("Previous match (Shift+F3)"));
    connect(prev, &QPushButton::clicked, this, [this] { stepFilesFind(-1); });
    auto *next = new QPushButton;
    setOcticon(next, "chevron-down", 14);
    smallButton(next, QStringLiteral("Next match (F3)"));
    connect(next, &QPushButton::clicked, this, [this] { stepFilesFind(1); });
    auto *close = new QPushButton;
    setOcticon(close, "x", 14);
    smallButton(close, QStringLiteral("Close the find bar (Esc)"));
    connect(close, &QPushButton::clicked, this, [this] { closeFilesFindBar(); });
    findRow->addWidget(prev);
    findRow->addWidget(next);
    findRow->addWidget(close);
    outer->addLayout(findRow);

    m_filesReplaceRow = new QWidget;
    auto *replaceRow = new QHBoxLayout(m_filesReplaceRow);
    replaceRow->setContentsMargins(0, 0, 0, 0);
    replaceRow->setSpacing(6);
    m_filesReplaceInput = new QLineEdit;
    m_filesReplaceInput->setObjectName(QStringLiteral("issueSearch"));
    m_filesReplaceInput->setPlaceholderText(
        QString::fromUtf8("Replace with\xE2\x80\xA6"));
    connect(m_filesReplaceInput, &QLineEdit::returnPressed, this,
            [this] { replaceInFilesEditor(false); });
    auto *replaceOne = new QPushButton(QStringLiteral("Replace"));
    smallButton(replaceOne, QStringLiteral("Replace this match"));
    connect(replaceOne, &QPushButton::clicked, this,
            [this] { replaceInFilesEditor(false); });
    auto *replaceAll = new QPushButton(QStringLiteral("Replace all"));
    smallButton(replaceAll, QStringLiteral("Replace every match in this file"));
    connect(replaceAll, &QPushButton::clicked, this,
            [this] { replaceInFilesEditor(true); });
    replaceRow->addWidget(m_filesReplaceInput, 1);
    replaceRow->addWidget(replaceOne);
    replaceRow->addWidget(replaceAll);
    m_filesReplaceRow->hide();
    outer->addWidget(m_filesReplaceRow);

    m_filesFindBar->hide();
    return m_filesFindBar;
}

// The trace panel under the editor: one symbol, one direction, rows that open
// the file they name and — for the two call directions — expand into the next
// step of the trace.
QWidget *MainWindow::buildFilesSymbolPanel()
{
    m_filesSymbolPanel = new QWidget;
    m_filesSymbolPanel->setObjectName(QStringLiteral("filesSymbolPanel"));
    auto *layout = new QVBoxLayout(m_filesSymbolPanel);
    layout->setContentsMargins(2, 6, 2, 0);
    layout->setSpacing(6);

    auto *header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(6);
    m_filesSymbolTitle = new QLabel;
    m_filesSymbolTitle->setObjectName(QStringLiteral("mutedLabel"));
    m_filesSymbolTitle->setTextInteractionFlags(Qt::TextSelectableByMouse);
    // Same reason as the breadcrumb: a long symbol name must not widen the
    // window's minimum (issue #369).
    m_filesSymbolTitle->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    header->addWidget(m_filesSymbolTitle, 1);

    // Backwards (who calls this) and forwards (what this calls) are the two
    // directions of the same trace, so they sit beside the flat lists rather
    // than in a menu.
    const struct {
        const char *label;
        const char *tip;
    } modes[] = {
        {"Definitions", "Where this symbol is defined"},
        {"References", "Every occurrence of this symbol in the repository"},
        {"Callers", "Trace backwards: the functions that call this one"},
        {"Calls", "Trace forwards: the functions this one calls"},
    };
    m_filesSymbolModeButtons.clear();
    for (int i = 0; i < int(sizeof(modes) / sizeof(modes[0])); ++i) {
        // The chip style is the app's segmented control: a checked one reads as
        // picked, which a ghost button does not (there is no ghost:checked rule).
        auto *button = new QPushButton(QString::fromLatin1(modes[i].label));
        button->setObjectName(QStringLiteral("logFilterChip"));
        button->setCheckable(true);
        button->setCursor(Qt::PointingHandCursor);
        button->setToolTip(QString::fromLatin1(modes[i].tip));
        connect(button, &QPushButton::clicked, this, [this, i] {
            if (!m_filesSymbolName.isEmpty())
                traceFilesSymbol(m_filesSymbolName, i);
        });
        m_filesSymbolModeButtons.append(button);
        header->addWidget(button);
    }

    auto *close = new QPushButton;
    close->setObjectName(QStringLiteral("ghostButton"));
    close->setProperty("buttonSize", "sm");
    close->setCursor(Qt::PointingHandCursor);
    close->setToolTip(QStringLiteral("Close the trace panel"));
    setOcticon(close, "x", 14);
    connect(close, &QPushButton::clicked, this,
            [this] { closeFilesSymbolPanel(); });
    header->addWidget(close);
    layout->addLayout(header);

    m_filesSymbolTree = new QTreeWidget;
    m_filesSymbolTree->setObjectName(QStringLiteral("searchResultsTree"));
    m_filesSymbolTree->setHeaderHidden(true);
    m_filesSymbolTree->setColumnCount(1);
    m_filesSymbolTree->setUniformRowHeights(true);
    m_filesSymbolTree->setExpandsOnDoubleClick(false);
    m_filesSymbolTree->setCursor(Qt::PointingHandCursor);
    enableHoverRowHighlight(m_filesSymbolTree);
    installFilesTreeDelegate(m_filesSymbolTree, false);
    connect(m_filesSymbolTree, &QTreeWidget::itemClicked, this,
            &MainWindow::activateFilesSymbolRow);
    connect(m_filesSymbolTree, &QTreeWidget::itemExpanded, this,
            &MainWindow::expandFilesSymbolRow);
    layout->addWidget(m_filesSymbolTree, 1);

    m_filesSymbolPanel->setMinimumHeight(140);
    m_filesSymbolPanel->hide();
    return m_filesSymbolPanel;
}

QLayout *MainWindow::buildFilesEditorBar()
{
    auto *row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(6);

    m_repoFilePreviewButton = new QPushButton(QStringLiteral("Preview"));
    m_repoFilePreviewButton->setObjectName(QStringLiteral("ghostButton"));
    m_repoFilePreviewButton->setCursor(Qt::PointingHandCursor);
    m_repoFilePreviewButton->setCheckable(true);
    m_repoFilePreviewButton->setToolTip(QStringLiteral("Preview rendered Markdown"));
    setOcticon(m_repoFilePreviewButton, "eye", 16);
    connect(m_repoFilePreviewButton, &QPushButton::clicked, this,
            &MainWindow::toggleRepoFileMarkdownPreview);

    m_repoFileHistoryButton = new QPushButton(QStringLiteral("Show history"));
    m_repoFileHistoryButton->setObjectName(QStringLiteral("ghostButton"));
    m_repoFileHistoryButton->setCursor(Qt::PointingHandCursor);
    m_repoFileHistoryButton->setToolTip(
        QStringLiteral("Show the commit history and changes for this file"));
    setOcticon(m_repoFileHistoryButton, "history", 16);
    connect(m_repoFileHistoryButton, &QPushButton::clicked, this, [this] {
        QWidget *w = m_repoFileTabs ? m_repoFileTabs->currentWidget() : nullptr;
        const QString path = w ? w->property("previewPath").toString() : QString();
        if (!path.isEmpty())
            showRepoFileHistory(path);
    });

    m_repoFileCommitButton = new QPushButton(QStringLiteral("Commit direct"));
    m_repoFileCommitButton->setObjectName(QStringLiteral("ghostButton"));
    m_repoFileCommitButton->setCursor(Qt::PointingHandCursor);
    m_repoFileCommitButton->setToolTip(QStringLiteral(
        "Save this file and commit it directly to the default branch"));
    setOcticon(m_repoFileCommitButton, "upload", 16);
    connect(m_repoFileCommitButton, &QPushButton::clicked, this,
            [this] { saveCurrentRepoFile(false); });

    m_repoFilePullButton = new QPushButton(QStringLiteral("Save as PR"));
    m_repoFilePullButton->setObjectName(QStringLiteral("ghostButton"));
    m_repoFilePullButton->setCursor(Qt::PointingHandCursor);
    m_repoFilePullButton->setToolTip(QStringLiteral(
        "Save this file on a new branch and open a pull request"));
    setOcticon(m_repoFilePullButton, "git-pull-request", 16);
    connect(m_repoFilePullButton, &QPushButton::clicked, this,
            [this] { saveCurrentRepoFile(true); });

    m_repoFileSaveButton = new QPushButton(QStringLiteral("Save"));
    m_repoFileSaveButton->setObjectName(QStringLiteral("primaryButton"));
    m_repoFileSaveButton->setCursor(Qt::PointingHandCursor);
    m_repoFileSaveButton->setToolTip(
        QStringLiteral("Write this file back to the working tree"));
    setOcticon(m_repoFileSaveButton, "check-circle", 16);
    m_repoFileSaveButton->hide();
    connect(m_repoFileSaveButton, &QPushButton::clicked, this,
            [this] { saveCurrentRepoFile(false); });

    row->addWidget(m_repoFilePreviewButton);
    row->addWidget(m_repoFileHistoryButton);
    row->addWidget(m_repoFileCommitButton);
    row->addWidget(m_repoFilePullButton);
    row->addWidget(m_repoFileSaveButton);
    row->addStretch(1);

    m_filesCommitMessage = new QLineEdit;
    m_filesCommitMessage->setObjectName(QStringLiteral("filesCommitMessage"));
    m_filesCommitMessage->setPlaceholderText(
        QString::fromUtf8("Commit message for all changes\xE2\x80\xA6"));
    m_filesCommitMessage->setClearButtonEnabled(true);
    m_filesCommitMessage->setMinimumWidth(240);
    connect(m_filesCommitMessage, &QLineEdit::returnPressed, this,
            &MainWindow::commitFilesTabChanges);
    row->addWidget(m_filesCommitMessage);

    m_filesCommitButton = new QPushButton(QStringLiteral("Commit"));
    m_filesCommitButton->setObjectName(QStringLiteral("ghostButton"));
    m_filesCommitButton->setCursor(Qt::PointingHandCursor);
    m_filesCommitButton->setToolTip(QStringLiteral(
        "Stage every change in the working tree and commit it with the message "
        "beside this button"));
    setOcticon(m_filesCommitButton, "git-commit", 16);
    connect(m_filesCommitButton, &QPushButton::clicked, this,
            &MainWindow::commitFilesTabChanges);
    row->addWidget(m_filesCommitButton);

    m_filesPushButton = new QPushButton(QStringLiteral("Push"));
    m_filesPushButton->setObjectName(QStringLiteral("primaryButton"));
    m_filesPushButton->setCursor(Qt::PointingHandCursor);
    m_filesPushButton->setToolTip(QStringLiteral(
        "Publish the commits on this branch to the network mirror (or push to "
        "the upstream branch)"));
    setOcticon(m_filesPushButton, "upload", 16);
    connect(m_filesPushButton, &QPushButton::clicked, this,
            &MainWindow::pushCurrentRepoUpstream);
    row->addWidget(m_filesPushButton);
    return row;
}

void MainWindow::commitFilesTabChanges()
{
    if (!m_filesCommitMessage)
        return;
    const QString message = m_filesCommitMessage->text();
    commitWorkingTree(message, [this, message](bool ok) {
        if (ok && m_filesCommitMessage && m_filesCommitMessage->text() == message)
            m_filesCommitMessage->clear();
        updateFilesCommitActions();
    });
}

void MainWindow::updateFilesCommitActions()
{
    const bool owned = !repoGitDir().isEmpty() && repoHasWorkingTree();
    if (m_filesCommitMessage)
        m_filesCommitMessage->setEnabled(owned);
    if (m_filesCommitButton)
        m_filesCommitButton->setEnabled(owned);
    if (m_filesPushButton)
        m_filesPushButton->setEnabled(owned);
}

void MainWindow::showFilesRepoTree()
{
    if (!m_filesTreeStack)
        return;
    if (m_treeLoadedForIndex != m_repoDetailIndex) {
        loadRepoFileTree();
        m_treeLoadedForIndex = m_repoDetailIndex;
    }
    m_filesTreeStack->setCurrentIndex(1);
    if (m_filesRepoTreeButton)
        m_filesRepoTreeButton->setChecked(true);
    if (m_filesCoveTreeButton)
        m_filesCoveTreeButton->setChecked(false);
    if (m_filesNetworkTreeButton)
        m_filesNetworkTreeButton->setChecked(false);
    if (m_fileExplorerStatus)
        m_fileExplorerStatus->setText(
            repoGitDir().isEmpty()
                ? QStringLiteral("No local copy of a repository is open.")
                : QStringLiteral("Files at %1, plus everything untracked")
                      .arg(currentRef()));
}

void MainWindow::showFilesCoveTree()
{
    if (!m_filesTreeStack || m_filesTreeStack->count() < 3)
        return;
    loadCoveExplorer();
    m_filesTreeStack->setCurrentIndex(2);
    if (m_filesRepoTreeButton)
        m_filesRepoTreeButton->setChecked(false);
    if (m_filesCoveTreeButton)
        m_filesCoveTreeButton->setChecked(true);
    if (m_filesNetworkTreeButton)
        m_filesNetworkTreeButton->setChecked(false);
    if (m_fileExplorerStatus) {
        QString repository;
        if (m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size()) {
            const RepositoryRecord &repo = m_repositories.at(m_repoDetailIndex);
            repository = repo.owner + QLatin1Char('/') + repo.name;
        }
        m_fileExplorerStatus->setText(
            repository.isEmpty()
                ? QStringLiteral("Open a repository to browse its Coves.")
                : QStringLiteral("Encrypted Cove files in %1").arg(repository));
    }
}

void MainWindow::showFilesDiskTree()
{
    if (m_filesTreeStack)
        m_filesTreeStack->setCurrentIndex(0);
    if (m_filesRepoTreeButton)
        m_filesRepoTreeButton->setChecked(false);
    if (m_filesCoveTreeButton)
        m_filesCoveTreeButton->setChecked(false);
    if (m_filesNetworkTreeButton)
        m_filesNetworkTreeButton->setChecked(false);
}

// The size column is column 1 on both trees (name is 0), so one toggle covers
// the filesystem explorer and the repository's tracked tree together. Column
// visibility survives clear() / re-listing, so this only has to run when the
// preference changes and once at build time.
void MainWindow::setFilesSizeColumnVisible(bool on)
{
    if (m_fileExplorerTree)
        m_fileExplorerTree->setColumnHidden(1, !on);
    if (m_repoFileTree)
        m_repoFileTree->setColumnHidden(1, !on);
}

void MainWindow::refreshFilesPage()
{
    if (!m_fileExplorerTree)
        return;
    updateFilesCommitActions();
    if (m_filesTreeStack && m_filesTreeStack->currentIndex() == 1)
        return;
    if (m_filesTreeStack && m_filesTreeStack->currentIndex() == 2) {
        loadCoveExplorer();
        return;
    }
    if (m_filesTreeStack && m_filesTreeStack->currentIndex() == 3)
        return;
    if (QDir(m_fileExplorerRoot).exists()) {
        if (m_fileExplorerTree->topLevelItemCount() == 0)
            setFileExplorerRoot(m_fileExplorerRoot);
        return;
    }
    QString fallback = repoGitDir();
    if (fallback.isEmpty() || !QDir(fallback).exists())
        fallback = forkmesh::paths::projectDir();
    if (fallback.isEmpty() || !QDir(fallback).exists())
        fallback = QDir::homePath();
    setFileExplorerRoot(fallback);
}

// Double-clicking a directory re-roots the explorer there, and that root is
// remembered across visits and launches — so after a session of drilling, the
// rail's Files button used to drop you three folders deep in whatever you were
// last poking at. Clicking it now means "take me back to the top of the open
// repository" (adhoc #1645). Typing a path, Up, Home and Choose still re-root
// wherever they like; this only resets on the way in.
void MainWindow::openFilesSectionAtRepoRoot()
{
    showSection(kFilesSectionIndex);
    if (!m_fileExplorerTree)
        return;
    const QString root = repoGitDir();
    if (root.isEmpty() || !QDir(root).exists())
        return; // no repository open: leave the browser where it was
    if (m_fileExplorerRoot == QDir(root).absolutePath())
        return;
    // setFileExplorerRoot() always shows the filesystem tree; if the tracked
    // tree was the one on screen, keep it there — it is already rooted at the
    // repository — and just re-root the explorer behind it.
    const bool repoTreeShowing =
        m_filesTreeStack && m_filesTreeStack->currentIndex() == 1;
    setFileExplorerRoot(root);
    if (repoTreeShowing)
        showFilesRepoTree();
}

QFileInfoList MainWindow::fileExplorerEntries(const QString &dir) const
{
    QDir::Filters filters = QDir::AllEntries | QDir::NoDotAndDotDot;
    if (!m_fileExplorerHidden || m_fileExplorerHidden->isChecked())
        filters |= QDir::Hidden | QDir::System;
    return QDir(dir).entryInfoList(filters,
                                   QDir::DirsFirst | QDir::Name |
                                       QDir::IgnoreCase | QDir::LocaleAware);
}

void MainWindow::setFileExplorerRoot(const QString &path)
{
    if (!m_fileExplorerTree)
        return;

    QString target = QDir::cleanPath(path.trimmed());
    if (target.startsWith(QLatin1Char('~')))
        target = QDir::homePath() + target.mid(1);
    if (target.isEmpty())
        target = forkmesh::paths::projectDir();
    if (target.isEmpty())
        target = QDir::homePath();
    const QFileInfo info(target);
    QString selectPath;
    if (info.isFile()) {
        selectPath = info.absoluteFilePath();
        target = info.absolutePath();
    }
    if (!QDir(target).exists()) {
        if (m_fileExplorerStatus)
            m_fileExplorerStatus->setText(
                QStringLiteral("No such directory: %1").arg(target));
        if (m_fileExplorerPath)
            m_fileExplorerPath->setText(m_fileExplorerRoot);
        return;
    }
    target = QDir(target).absolutePath();
    showFilesDiskTree();

    const QString previousRoot = m_fileExplorerRoot;
    m_fileExplorerRoot = target;
    QSettings().setValue(QLatin1String(kFilesRootSetting), target);
    if (m_fileExplorerPath && m_fileExplorerPath->text() != target)
        m_fileExplorerPath->setText(target);

    const bool sameRoot = (previousRoot == target);
    const QSet<QString> expanded =
        sameRoot ? fileTreeExpandedPaths(m_fileExplorerTree) : QSet<QString>();
    const int scrollOffset =
        sameRoot ? fileTreeScrollOffset(m_fileExplorerTree) : 0;

    m_fileExplorerTree->clear();
    const QFileInfoList entries = fileExplorerEntries(target);
    int hidden = 0;
    for (const QFileInfo &entry : entries) {
        if (entry.isHidden())
            ++hidden;
        addFileExplorerRow(nullptr, entry);
    }

    if (entries.isEmpty()) {
        auto *empty = new QTreeWidgetItem(m_fileExplorerTree,
                                          {QStringLiteral("(empty directory)")});
        empty->setFlags(Qt::NoItemFlags);
    }

    if (m_fileExplorerStatus) {
        const QString hiddenText =
            m_fileExplorerHidden && !m_fileExplorerHidden->isChecked()
                ? QStringLiteral("hidden not shown")
                : QStringLiteral("%1 hidden").arg(hidden);
        m_fileExplorerStatus->setText(
            QStringLiteral("%1 %2 · %3")
                .arg(entries.size())
                .arg(entries.size() == 1 ? QStringLiteral("entry")
                                         : QStringLiteral("entries"),
                     hiddenText));
    }
    previewFileExplorerFile(QString());
    restoreFileTreeExpandedPaths(
        m_fileExplorerTree, expanded, scrollOffset,
        [this](QTreeWidgetItem *item) { populateFileExplorerItem(item); });

    if (selectPath.isEmpty())
        return;
    for (int i = 0; i < m_fileExplorerTree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = m_fileExplorerTree->topLevelItem(i);
        if (item->data(0, kFileTreePathRole).toString() != selectPath)
            continue;
        m_fileExplorerTree->setCurrentItem(item);
        m_fileExplorerTree->scrollToItem(item);
        break;
    }
}

QTreeWidgetItem *MainWindow::addFileExplorerRow(QTreeWidgetItem *parent,
                                                const QFileInfo &entry)
{
    auto *item = parent ? new QTreeWidgetItem(parent)
                        : new QTreeWidgetItem(m_fileExplorerTree);
    const bool isDir = entry.isDir();
    item->setText(0, entry.fileName());
    item->setData(0, kFileTreePathRole, entry.absoluteFilePath());
    item->setData(0, kFileTreeIsDirRole, isDir);
    item->setIcon(0, isDir ? iconForDir(false) : iconForFile(entry.fileName()));
    if (!isDir)
        setFileTreeSizeCell(item, 1, formatByteSize(entry.size()));
    item->setText(2, modifiedLabel(entry));
    item->setToolTip(0, entry.absoluteFilePath());
    // The placeholder child is what draws the expand arrow before the directory
    // has been read. Symlinked directories never get one, so a link pointing at
    // an ancestor cannot loop the tree; an unreadable one does, and says so in
    // place when it is opened rather than silently collapsing again.
    if (isDir && !entry.isSymLink())
        item->addChild(new QTreeWidgetItem);
    return item;
}

void MainWindow::populateFileExplorerItem(QTreeWidgetItem *item)
{
    if (!fileTreeIsDir(item) || item->data(0, kFileLoadedRole).toBool())
        return;
    item->setData(0, kFileLoadedRole, true);
    qDeleteAll(item->takeChildren()); // drop the expand-arrow placeholder

    const QString dir = fileTreePath(item);
    const QFileInfoList entries = fileExplorerEntries(dir);
    if (entries.isEmpty()) {
        const bool readable = QFileInfo(dir).isReadable();
        auto *note = new QTreeWidgetItem(
            item, {readable ? QStringLiteral("(empty)")
                            : QStringLiteral("(not readable)")});
        note->setFlags(Qt::NoItemFlags);
        return;
    }
    for (const QFileInfo &entry : entries)
        addFileExplorerRow(item, entry);
}

void MainWindow::previewFileExplorerFile(const QString &path, int line)
{
    if (!m_fileExplorerPreview)
        return;
    m_fileExplorerPreview->setExtraSelections({});
    // Kept under its own name: "previewPath" is what the save / commit / history
    // actions key off, and the preview is none of those things — but the
    // breadcrumb still has to be able to say which file is on screen.
    m_fileExplorerPreview->setProperty("filesPreviewPath", path);
    if (path.isEmpty()) {
        m_fileExplorerPreview->clear();
        updateFilesEditorStatus();
        return;
    }
    if (m_repoFileTabs)
        m_repoFileTabs->setCurrentWidget(m_fileExplorerPreview);
    const QFileInfo info(path);
    const QString header =
        QStringLiteral("%1\n%2 · %3\n\n")
            .arg(info.absoluteFilePath(),
                 info.isDir() ? QStringLiteral("directory")
                              : formatByteSize(info.size()),
                 modifiedLabel(info));
    if (info.isDir()) {
        m_fileExplorerPreview->setPlainText(header +
                                            QStringLiteral("Expand to browse."));
        return;
    }
    if (!info.isFile()) {
        m_fileExplorerPreview->setPlainText(
            header + QStringLiteral("Not a regular file — no preview."));
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        m_fileExplorerPreview->setPlainText(
            header + QStringLiteral("Could not read this file: %1")
                         .arg(file.errorString()));
        return;
    }
    const QByteArray head = readPreviewHead(file, line);
    if (fileContentIsBinary(head)) {
        m_fileExplorerPreview->setPlainText(
            header + QStringLiteral("Binary file — no preview."));
        return;
    }
    QString text = QString::fromUtf8(head);
    if (info.size() > head.size()) {
        // What was actually read, not the cap: with a line to reach, the read
        // stops just past it, so naming the cap here would misreport how much
        // of the file is on screen.
        text += QStringLiteral("\n\n… truncated at %1 of %2.")
                    .arg(formatByteSize(head.size()),
                         formatByteSize(info.size()));
    }
    m_fileExplorerPreview->setPlainText(header + text);
    updateFilesEditorStatus();
    if (line > 0)
        highlightFileExplorerPreviewLine(line);
}

void MainWindow::highlightFileExplorerPreviewLine(int line)
{
    if (!m_fileExplorerPreview || line <= 0)
        return;
    QTextDocument *doc = m_fileExplorerPreview->document();
    const int block = kFilePreviewHeaderLines + line - 1;
    if (block >= doc->blockCount())
        return; // past the end (a truncated preview, or a stale line number)

    QTextCursor cursor(doc->findBlockByNumber(block));
    m_fileExplorerPreview->setTextCursor(cursor);
    m_fileExplorerPreview->centerCursor();

    QTextEdit::ExtraSelection selection;
    selection.cursor = cursor;
    selection.format.setBackground(
        Theme::nextOr(Theme::Role::AccentSubtle,
                      currentThemeIsDark() ? QColor(56, 47, 12) : QColor(255, 244, 191)));
    selection.format.setProperty(QTextFormat::FullWidthSelection, true);
    m_fileExplorerPreview->setExtraSelections({selection});
}

void MainWindow::openLocalFileTab(const QString &path)
{
    if (path.isEmpty())
        return;
    ensureSectionBuilt(kFilesSectionIndex);
    if (!m_repoFileTabs)
        return;
    const QString absolute = QFileInfo(path).absoluteFilePath();
    const QString dir = repoGitDir();
    if (!dir.isEmpty()) {
        const QString rel = QDir(dir).relativeFilePath(absolute);
        if (!rel.startsWith(QLatin1String("../")) &&
            !QDir::isAbsolutePath(rel) &&
            runGitCapture(dir, {"ls-files", "--error-unmatch", "--", rel}, nullptr,
                          nullptr)) {
            openRepoFile(rel);
            return;
        }
    }

    if (m_openFileTabs.contains(absolute)) {
        m_repoFileTabs->setCurrentWidget(m_openFileTabs.value(absolute));
        showSection(kFilesSectionIndex);
        return;
    }
    const QFileInfo info(absolute);
    QString content;
    bool editable = false;
    QFile file(absolute);
    if (!info.isFile()) {
        content = QStringLiteral("Not a regular file — nothing to open.");
    } else if (!file.open(QIODevice::ReadOnly)) {
        content = QStringLiteral("Could not read this file: %1")
                      .arg(file.errorString());
    } else {
        const QByteArray head = file.read(kEditableFileByteLimit);
        if (fileContentIsBinary(head)) {
            content = QStringLiteral("Binary file — not shown.");
        } else {
            content = QString::fromUtf8(head);
            if (info.size() > kEditableFileByteLimit)
                content += QStringLiteral("\n\n… truncated at %1 of %2.")
                               .arg(formatByteSize(kEditableFileByteLimit),
                                    formatByteSize(info.size()));
            else
                editable = info.isWritable();
        }
    }

    auto *editor = new CodePreviewEditor(absolute);
    // Files opened from the filesystem tree are on disk, including dotfiles
    // that are not tracked by the open repository.  Keep oversized and binary
    // files read-only, but let a writable text file use the same direct-save
    // path as a working-tree file.
    editor->setProperty("worktreeFile", editable ? absolute : QString());
    editor->setReadOnly(!editable);
    editor->setPlainText(content);
    editor->document()->setModified(false);
    new CodePreviewHighlighter(editor->document(), absolute);
    wireFilesEditor(editor);

    const QString name = info.fileName();
    const int index = m_repoFileTabs->addTab(editor, iconForFile(name), name);
    m_repoFileTabs->setTabToolTip(index, absolute);
    m_repoFileTabs->setCurrentIndex(index);
    m_openFileTabs.insert(absolute, editor);
    updateRepoFileSaveActions();
    showSection(kFilesSectionIndex);
}

void MainWindow::clearRepoFileTabs()
{
    if (!m_repoFileTabs)
        return;
    for (int i = m_repoFileTabs->count() - 1; i >= 0; --i) {
        QWidget *w = m_repoFileTabs->widget(i);
        if (w == m_fileExplorerPreview)
            continue;
        m_repoFileTabs->removeTab(i);
        w->deleteLater();
    }
    m_openFileTabs.clear();
    m_openCoveExplorerTabs.clear();
    // Both of these name things in the repository that is going away: the find
    // bar's matches are cursors into documents that just died, and the trace
    // panel's rows are paths in the old repo's tree.
    m_filesFindMatches.clear();
    m_filesFindIndex = -1;
    m_filesFindTab = nullptr;
    closeFilesSymbolPanel();
    updateRepoFileSaveActions();
    updateFilesEditorStatus();
}

void MainWindow::revealFileInExplorer(const QString &path, int line)
{
    const QFileInfo info(path);
    if (!info.isFile()) {
        flashMessage(QStringLiteral("No such file: %1").arg(path), true);
        return;
    }
    showSection(kFilesSectionIndex);
    if (m_filesNavButton)
        m_filesNavButton->setChecked(true);
    setFileExplorerRoot(info.absoluteFilePath());
    previewFileExplorerFile(info.absoluteFilePath(), line);
}

void MainWindow::revealLogSourceInExplorer(const QString &relativePath, int line)
{
    QStringList roots;
    const QString repo = repoGitDir();
    if (!repo.isEmpty())
        roots << repo;
    const QString resolved = forkmesh::resolveLogSourcePath(relativePath, roots);
    if (resolved.isEmpty()) {
        flashMessage(QStringLiteral("%1 isn't on this machine — the log entry "
                                    "was written by a build from another "
                                    "checkout.")
                         .arg(relativePath),
                     true);
        return;
    }
    revealFileInExplorer(resolved, line);
}

#ifdef FORKMESH_WINDOW_TESTS
bool MainWindow::testFilesNavButtonVisible() const
{
    return m_filesNavButton && !m_filesNavButton->isHidden();
}

bool MainWindow::testFilesRepoTreeShowing() const
{
    return m_filesTreeStack && m_filesTreeStack->currentIndex() == 1;
}

bool MainWindow::testFilesCoveTreeShowing() const
{
    return m_filesTreeStack && m_filesTreeStack->currentIndex() == 2;
}

int MainWindow::testCodeFilesPageCount() const
{
    return m_filesStack ? m_filesStack->count() : 0;
}

void MainWindow::testSetFileExplorerShowHidden(bool on)
{
    if (m_fileExplorerHidden)
        m_fileExplorerHidden->setChecked(on);
}

QStringList MainWindow::testFileExplorerNames() const
{
    QStringList names;
    if (!m_fileExplorerTree)
        return names;
    for (int i = 0; i < m_fileExplorerTree->topLevelItemCount(); ++i)
        names.append(m_fileExplorerTree->topLevelItem(i)->text(0));
    return names;
}

QStringList MainWindow::testRepoFileTreePaths() const
{
    QStringList paths;
    if (!m_repoFileTree)
        return paths;
    std::function<void(QTreeWidgetItem *)> walk = [&](QTreeWidgetItem *parent) {
        for (int i = 0; i < parent->childCount(); ++i) {
            QTreeWidgetItem *child = parent->child(i);
            const QString path = fileTreePath(child);
            if (!path.isEmpty())
                paths << path;
            walk(child);
        }
    };
    walk(m_repoFileTree->invisibleRootItem());
    return paths;
}

QStringList MainWindow::testExpandRepoFileTreeEntry(const QString &path)
{
    QStringList children;
    if (!m_repoFileTree)
        return children;
    std::function<QTreeWidgetItem *(QTreeWidgetItem *)> find =
        [&](QTreeWidgetItem *parent) -> QTreeWidgetItem * {
        for (int i = 0; i < parent->childCount(); ++i) {
            QTreeWidgetItem *child = parent->child(i);
            if (fileTreePath(child) == path)
                return child;
            if (QTreeWidgetItem *hit = find(child))
                return hit;
        }
        return nullptr;
    };
    QTreeWidgetItem *item = find(m_repoFileTree->invisibleRootItem());
    if (!item)
        return children;
    item->setExpanded(true);
    for (int i = 0; i < item->childCount(); ++i)
        children << fileTreePath(item->child(i));
    return children;
}

bool MainWindow::testFilesEditorEditable() const
{
    const QPlainTextEdit *editor = currentFilesEditor();
    return editor && !editor->isReadOnly();
}

QString MainWindow::testFileExplorerPreview() const
{
    return m_fileExplorerPreview ? m_fileExplorerPreview->toPlainText() : QString();
}

int MainWindow::testFileExplorerPreviewLine() const
{
    if (!m_fileExplorerPreview || m_fileExplorerPreview->extraSelections().isEmpty())
        return 0;
    const int block =
        m_fileExplorerPreview->extraSelections().first().cursor.blockNumber();
    return block < kFilePreviewHeaderLines
               ? 0
               : block - kFilePreviewHeaderLines + 1;
}

QStringList MainWindow::testExpandedFileExplorerPaths() const
{
    QStringList paths(fileTreeExpandedPaths(m_fileExplorerTree).values());
    paths.sort();
    return paths;
}

QString MainWindow::testFilesBreadcrumb() const
{
    return m_filesBreadcrumb ? m_filesBreadcrumb->text() : QString();
}

QString MainWindow::testFilesCursorLabel() const
{
    return m_filesCursorLabel ? m_filesCursorLabel->text() : QString();
}

void MainWindow::testOpenFilesFindBar(const QString &term)
{
    openFilesFindBar(false);
    if (m_filesFindInput)
        m_filesFindInput->setText(term);
}

QString MainWindow::testFilesFindCount() const
{
    return m_filesFindCount ? m_filesFindCount->text() : QString();
}

void MainWindow::testStepFilesFind(int delta)
{
    stepFilesFind(delta);
}

int MainWindow::testFilesFindLine() const
{
    const QPlainTextEdit *editor = currentFilesEditor();
    return editor ? editor->textCursor().blockNumber() + 1 : 0;
}

QString MainWindow::testFilesSymbolTitle() const
{
    return m_filesSymbolTitle ? m_filesSymbolTitle->text() : QString();
}

QStringList MainWindow::testFilesSymbolRows() const
{
    QStringList rows;
    if (!m_filesSymbolTree)
        return rows;
    for (int i = 0; i < m_filesSymbolTree->topLevelItemCount(); ++i)
        rows.append(m_filesSymbolTree->topLevelItem(i)->text(0));
    return rows;
}

bool MainWindow::testFilesSymbolPanelVisible() const
{
    return m_filesSymbolPanel && !m_filesSymbolPanel->isHidden();
}

QStringList MainWindow::testExpandFileExplorerEntry(const QString &name)
{
    QStringList names;
    if (!m_fileExplorerTree)
        return names;
    for (int i = 0; i < m_fileExplorerTree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = m_fileExplorerTree->topLevelItem(i);
        if (item->text(0) != name)
            continue;
        item->setExpanded(true);
        populateFileExplorerItem(item);
        for (int child = 0; child < item->childCount(); ++child)
            names.append(item->child(child)->text(0));
        break;
    }
    return names;
}

// Emitted, not called directly: the single-click open is a signal connection,
// so driving it this way is what proves the wiring rather than the handler.
void MainWindow::testClickFileExplorerEntry(const QString &name)
{
    if (!m_fileExplorerTree)
        return;
    for (int i = 0; i < m_fileExplorerTree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = m_fileExplorerTree->topLevelItem(i);
        if (item->text(0) != name)
            continue;
        emit m_fileExplorerTree->itemClicked(item, 0);
        break;
    }
}

QStringList MainWindow::testOpenFileTabTitles() const
{
    QStringList titles;
    if (!m_repoFileTabs)
        return titles;
    for (int i = 0; i < m_repoFileTabs->count(); ++i)
        titles.append(m_repoFileTabs->tabText(i));
    return titles;
}

void MainWindow::testSetFilesShowSize(bool on)
{
    if (m_filesSizeButton)
        m_filesSizeButton->setChecked(on);
}

bool MainWindow::testFilesSizeColumnVisible() const
{
    return m_fileExplorerTree && !m_fileExplorerTree->isColumnHidden(1) &&
           m_repoFileTree && !m_repoFileTree->isColumnHidden(1);
}
#endif

// ---- Files editor: tabs, status, and the symbol trace (adhoc #1607) -------

// Every editor tab (and the permanent preview) is wired the same way, so the
// right-click menu and the cursor readout work whichever route opened the file.
void MainWindow::wireFilesEditor(QPlainTextEdit *editor)
{
    if (!editor)
        return;
    new FilesEditorHook(editor);
    if (auto *code = dynamic_cast<CodePreviewEditor *>(editor)) {
        const QString path = code->property("previewPath").toString();
        if (previewSyntaxForPath(path) == PreviewSyntax::Markdown) {
            const QFileInfo file(path);
            const QString searchDir =
                file.isAbsolute() && file.exists() ? file.absolutePath() : QString();
            code->markdownRenderer = [searchDir](QTextBrowser *view, const QString &markdown) {
                view->setSearchPaths(searchDir.isEmpty() ? QStringList() : QStringList{searchDir});
                view->setMarkdown(MarkdownRender::forPreview(markdown));
                anchorReadmeHeadings(view->document());
                QTimer::singleShot(0, view, [view] {
                    fitReadmeImages(view, qMax(1, view->viewport()->width() - 8));
                    styleReadmeDocument(view);
                });
            };
        }
    }
    editor->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(editor, &QWidget::customContextMenuRequested, this,
            [this, editor](const QPoint &pos) { showFilesEditorMenu(editor, pos); });
    connect(editor, &QPlainTextEdit::cursorPositionChanged, this,
            [this, editor] {
                if (editor == currentFilesEditor())
                    updateFilesEditorStatus();
            });
}

QPlainTextEdit *MainWindow::currentFilesEditor() const
{
    QWidget *w = m_repoFileTabs ? m_repoFileTabs->currentWidget() : nullptr;
    return qobject_cast<QPlainTextEdit *>(w);
}

QString MainWindow::currentFilesEditorRepoPath() const
{
    QWidget *w = m_repoFileTabs ? m_repoFileTabs->currentWidget() : nullptr;
    const QString path = w ? w->property("previewPath").toString() : QString();
    // An absolute path is a file the repository does not track, and the preview
    // tab carries its own property — neither can be traced through git.
    return QDir::isAbsolutePath(path) ? QString() : path;
}

// The name to act on: the editor's selection when it is a single identifier,
// otherwise whatever the cursor is sitting in.
QString MainWindow::filesEditorSymbol() const
{
    QPlainTextEdit *editor = currentFilesEditor();
    if (!editor)
        return {};
    const QTextCursor cursor = editor->textCursor();
    if (cursor.hasSelection()) {
        const QString selected = cursor.selectedText().trimmed();
        if (!selected.isEmpty() &&
            forkmesh::code::identifierAt(selected, 0) == selected)
            return selected;
    }
    return forkmesh::code::identifierAt(cursor.block().text(),
                                        cursor.positionInBlock());
}

void MainWindow::showFilesEditorMenu(QPlainTextEdit *editor, const QPoint &pos)
{
    if (!editor)
        return;
    // Right-clicking a name acts on that name, not on wherever the caret
    // happened to be — so move the caret there first, the way an IDE does.
    const QTextCursor at = editor->cursorForPosition(pos);
    if (!editor->textCursor().hasSelection())
        editor->setTextCursor(at);
    const QString symbol =
        forkmesh::code::identifierAt(at.block().text(), at.positionInBlock());
    const QString repoPath = currentFilesEditorRepoPath();
    const bool traceable = !symbol.isEmpty() && !repoPath.isEmpty();

    QMenu menu(this);
    QAction *definition = nullptr;
    QAction *references = nullptr;
    QAction *callers = nullptr;
    QAction *calls = nullptr;
    QAction *highlight = nullptr;
    if (!symbol.isEmpty()) {
        if (traceable) {
            definition = menu.addAction(
                QStringLiteral("Go to definition of \"%1\"\tF12").arg(symbol));
            references = menu.addAction(
                QStringLiteral("Find all occurrences of \"%1\"\tShift+F12")
                    .arg(symbol));
            callers = menu.addAction(
                QString::fromUtf8("Trace back \xE2\x80\x94 what calls \"%1\"\tAlt+Shift+H")
                    .arg(symbol));
            calls = menu.addAction(
                QString::fromUtf8("Trace forward \xE2\x80\x94 what \"%1\" calls")
                    .arg(symbol));
        }
        highlight = menu.addAction(
            QStringLiteral("Highlight \"%1\" in this file").arg(symbol));
        menu.addSeparator();
    }

    QAction *find = menu.addAction(QString::fromUtf8("Find\xE2\x80\xA6\tCtrl+F"));
    QAction *replace = editor->isReadOnly()
                           ? nullptr
                           : menu.addAction(
                                 QString::fromUtf8("Replace\xE2\x80\xA6\tCtrl+H"));
    QAction *goToLine =
        menu.addAction(QString::fromUtf8("Go to line\xE2\x80\xA6\tCtrl+G"));
    QAction *outline = menu.addAction(
        QString::fromUtf8("Go to symbol in this file\xE2\x80\xA6\tCtrl+Shift+O"));
    menu.addSeparator();
    QAction *history = repoPath.isEmpty()
                           ? nullptr
                           : menu.addAction(QStringLiteral("Show file history"));
    QAction *copyPath = menu.addAction(QStringLiteral("Copy path"));
    QAction *reveal = menu.addAction(QStringLiteral("Reveal in the file tree"));

    // The editor's own Undo/Cut/Copy/Paste come last, so the code actions above
    // are what the menu opens on. QMenu::exec triggers them for us.
    std::unique_ptr<QMenu> standard(editor->createStandardContextMenu());
    if (standard) {
        menu.addSeparator();
        menu.addActions(standard->actions());
    }

    // exec() pumps the event loop, and a refresh that lands while the menu is
    // open can close this tab out from under us (clearRepoFileTabs) — so the
    // editor is only touched again through a pointer that goes null with it.
    QPointer<QPlainTextEdit> alive(editor);
    QAction *chosen = menu.exec(editor->viewport()->mapToGlobal(pos));
    if (!chosen || !alive)
        return;
    if (chosen == definition)
        traceFilesSymbol(symbol, forkmesh::code::TraceDefinitions);
    else if (chosen == references)
        traceFilesSymbol(symbol, forkmesh::code::TraceReferences);
    else if (chosen == callers)
        traceFilesSymbol(symbol, forkmesh::code::TraceCallers);
    else if (chosen == calls)
        traceFilesSymbol(symbol, forkmesh::code::TraceCalls);
    else if (chosen == highlight) {
        openFilesFindBar(false);
        if (m_filesFindWordButton)
            m_filesFindWordButton->setChecked(true);
        if (m_filesFindInput)
            m_filesFindInput->setText(symbol);
    } else if (chosen == find)
        openFilesFindBar(false);
    else if (chosen == replace)
        openFilesFindBar(true);
    else if (chosen == goToLine)
        goToLineInFilesEditor();
    else if (chosen == outline)
        showFilesSymbolOutline();
    else if (chosen == history)
        showRepoFileHistory(repoPath);
    else if (chosen == copyPath) {
        const QString own = editor->property("previewPath").toString();
        const QString covePath = editor->property("coveDocPath").toString();
        QGuiApplication::clipboard()->setText(
            !covePath.isEmpty()
                ? covePath
                : (own.isEmpty()
                       ? editor->property("filesPreviewPath").toString()
                       : own));
        flashMessage(QStringLiteral("Path copied."));
    } else if (chosen == reveal) {
        if (!editor->property("coveDocPath").toString().isEmpty()) {
            showFilesCoveTree();
            syncCoveTreeToOpenTab();
            return;
        }
        if (!repoPath.isEmpty()) {
            syncFilesTreeToOpenTab();
            return;
        }
        const QString own = editor->property("previewPath").toString();
        const QString absolute =
            own.isEmpty() ? editor->property("filesPreviewPath").toString() : own;
        if (absolute.isEmpty() || !m_repoFileTabs)
            return;
        // Re-rooting selects the row, and that selection previews the file —
        // which would leave the reader on the Preview tab instead of the one
        // they asked to reveal. Put their tab back.
        const int open = m_repoFileTabs->currentIndex();
        setFileExplorerRoot(absolute);
        m_repoFileTabs->setCurrentIndex(open);
    }
}

void MainWindow::showFilesTabMenu(const QPoint &pos)
{
    if (!m_repoFileTabs)
        return;
    QTabBar *bar = m_repoFileTabs->tabBar();
    const int index = bar->tabAt(pos);
    if (index < 0)
        return;
    QWidget *tab = m_repoFileTabs->widget(index);
    const bool permanent = tab == m_fileExplorerPreview;
    const QString repoPath =
        tab ? tab->property("previewPath").toString() : QString();
    const QString covePath =
        tab ? tab->property("coveDocPath").toString() : QString();
    const QString path = covePath.isEmpty() ? repoPath : covePath;

    QMenu menu(this);
    QAction *close = permanent ? nullptr : menu.addAction(QStringLiteral("Close"));
    QAction *closeOthers =
        m_repoFileTabs->count() > 2 ? menu.addAction(QStringLiteral("Close others"))
                                    : nullptr;
    QAction *closeAll = m_openFileTabs.isEmpty() && m_openCoveExplorerTabs.isEmpty()
                            ? nullptr
                            : menu.addAction(QStringLiteral("Close all"));
    QAction *copyPath = nullptr;
    QAction *revealTab = nullptr;
    QAction *history = nullptr;
    if (!path.isEmpty()) {
        menu.addSeparator();
        copyPath = menu.addAction(QStringLiteral("Copy path"));
        revealTab = menu.addAction(QStringLiteral("Reveal in the file tree"));
        if (covePath.isEmpty() && !QDir::isAbsolutePath(path))
            history = menu.addAction(QStringLiteral("Show file history"));
    }
    if (menu.isEmpty())
        return;

    // The menu pumps the event loop while it is open, so the tab it names can
    // be gone (or have moved — the bar is reorderable) by the time it closes.
    // Re-find it rather than trusting the index the click carried.
    QPointer<QWidget> alive(tab);
    QAction *chosen = menu.exec(bar->mapToGlobal(pos));
    if (!chosen || !alive)
        return;
    const int at = m_repoFileTabs->indexOf(tab);
    if (at < 0)
        return;
    if (chosen == close) {
        closeFilesTab(at);
    } else if (chosen == closeOthers) {
        for (int i = m_repoFileTabs->count() - 1; i >= 0; --i) {
            if (m_repoFileTabs->widget(i) != tab)
                closeFilesTab(i);
        }
    } else if (chosen == closeAll) {
        for (int i = m_repoFileTabs->count() - 1; i >= 0; --i)
            closeFilesTab(i);
    } else if (chosen == copyPath) {
        QGuiApplication::clipboard()->setText(path);
        flashMessage(QStringLiteral("Path copied."));
    } else if (chosen == revealTab) {
        m_repoFileTabs->setCurrentIndex(at);
        if (!covePath.isEmpty()) {
            showFilesCoveTree();
            syncCoveTreeToOpenTab();
        } else {
            syncFilesTreeToOpenTab();
        }
    } else if (chosen == history) {
        showRepoFileHistory(path);
    }
}

void MainWindow::closeFilesTab(int index)
{
    if (!m_repoFileTabs)
        return;
    QWidget *w = m_repoFileTabs->widget(index);
    if (!w || w == m_fileExplorerPreview)
        return; // the preview tab is permanent and carries no close button
    if (!w->property("coveId").toString().isEmpty() &&
        coveEditorIsModified(w)) {
        const QMessageBox::StandardButton choice = QMessageBox::question(
            this, QStringLiteral("Unsaved Cove file"),
            QStringLiteral("Save your changes before closing this file?"),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
            QMessageBox::Save);
        if (choice == QMessageBox::Cancel)
            return;
        if (choice == QMessageBox::Save) {
            m_repoFileTabs->setCurrentIndex(index);
            saveCurrentCoveExplorerDocument();
            if (coveEditorIsModified(w))
                return;
        }
    }
    // The find bar's matches are cursors into this document; they die with it.
    if (w == m_filesFindTab) {
        m_filesFindMatches.clear();
        m_filesFindIndex = -1;
        m_filesFindTab = nullptr;
    }
    m_openFileTabs.remove(m_openFileTabs.key(w));
    m_openCoveExplorerTabs.remove(m_openCoveExplorerTabs.key(w));
    m_repoFileTabs->removeTab(index);
    w->deleteLater();
    updateRepoFileSaveActions();
    updateFilesEditorStatus();
}

// The breadcrumb naming the file on screen, and the cursor readout beside it.
void MainWindow::updateFilesEditorStatus()
{
    if (!m_filesBreadcrumb || !m_repoFileTabs)
        return;
    QWidget *w = m_repoFileTabs->currentWidget();
    QString path = w ? w->property("previewPath").toString() : QString();
    const QString covePath =
        w ? w->property("coveDocPath").toString() : QString();
    const bool coveFile = !covePath.isEmpty();
    if (path.isEmpty() && w)
        path = w->property("filesPreviewPath").toString(); // the preview tab
    if (path.isEmpty() && coveFile)
        path = covePath;
    if (path.isEmpty()) {
        m_filesBreadcrumb->setText(
            QString::fromUtf8("No file open \xE2\x80\x94 pick one on the left."));
        m_filesBreadcrumb->setToolTip(QString());
        if (m_filesCursorLabel)
            m_filesCursorLabel->clear();
        return;
    }
    // "owner/repo › src › MainWindow.cpp" for a tracked file, the plain path for
    // anything else. The separators make the directory chain readable at a
    // glance without a second widget.
    QStringList crumbs = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (coveFile) {
        const QString coveName = w->property("coveName").toString().trimmed();
        crumbs.prepend(coveName.isEmpty()
                           ? QStringLiteral("Encrypted Cove")
                           : QStringLiteral("Cove: %1").arg(coveName));
    } else if (!QDir::isAbsolutePath(path) && m_repoDetailIndex >= 0 &&
        m_repoDetailIndex < m_repositories.size()) {
        const RepositoryRecord &repo = m_repositories.at(m_repoDetailIndex);
        crumbs.prepend(repo.owner + QLatin1Char('/') + repo.name);
    }
    const QString trail = crumbs.join(QString::fromUtf8(" \xE2\x80\xBA "));
    // Elided from the left: the file's own name is the part worth keeping when
    // the column is narrow.
    const int room = qMax(120, m_filesBreadcrumb->width() - 4);
    const QString shown =
        m_filesBreadcrumb->fontMetrics().elidedText(trail, Qt::ElideLeft, room);
    m_filesBreadcrumb->setText(Theme::isNext() ? filesBreadcrumbMarkup(shown)
                                               : shown);
    m_filesBreadcrumb->setToolTip(
        coveFile ? QStringLiteral("Encrypted Cove document\n%1").arg(path)
                 : path);

    auto *editor = qobject_cast<QPlainTextEdit *>(w);
    if (!m_filesCursorLabel)
        return;
    if (!editor) {
        m_filesCursorLabel->clear();
        return;
    }
    const QTextCursor cursor = editor->textCursor();
    QString text = QStringLiteral("Ln %1, Col %2")
                       .arg(cursor.blockNumber() + 1)
                       .arg(cursor.positionInBlock() + 1);
    if (cursor.hasSelection())
        text += QStringLiteral(" (%1 selected)")
                    .arg(cursor.selectedText().size());
    m_filesCursorLabel->setText(text);
}

// Follow the open tab in the repository tree, so the file being edited is
// always visible (and selected) on the left. Only the repository side is
// synced: selecting a row in the filesystem tree previews it, which would pull
// the editor off the tab that was just opened.
void MainWindow::syncFilesTreeToOpenTab()
{
    const QString path = currentFilesEditorRepoPath();
    if (path.isEmpty() || !m_repoFileTree)
        return;
    showFilesRepoTree();
    QTreeWidgetItem *parent = nullptr;
    QString walked;
    const QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        walked = walked.isEmpty() ? part : walked + QLatin1Char('/') + part;
        QTreeWidgetItem *found = nullptr;
        const int count = parent ? parent->childCount()
                                 : m_repoFileTree->topLevelItemCount();
        for (int i = 0; i < count && !found; ++i) {
            QTreeWidgetItem *child =
                parent ? parent->child(i) : m_repoFileTree->topLevelItem(i);
            if (child->data(0, Qt::UserRole).toString() == walked)
                found = child;
        }
        if (!found)
            return; // the tree has not been built for this ref
        found->setExpanded(true);
        parent = found;
    }
    if (!parent)
        return;
    m_repoFileTree->setCurrentItem(parent);
    m_repoFileTree->scrollToItem(parent);
}

void MainWindow::traceFilesSymbol(const QString &symbol, int mode)
{
    ensureSectionBuilt(kFilesSectionIndex);
    if (!m_filesSymbolPanel || !m_filesSymbolTree)
        return;
    const QString clean = symbol.trimmed();
    if (clean.isEmpty()) {
        flashMessage(
            QStringLiteral("Put the cursor on a name to trace it, then try again."),
            true);
        return;
    }
    if (repoGitDir().isEmpty()) {
        flashMessage(QStringLiteral("Open a repository to trace a symbol through "
                                    "its code."),
                     true);
        return;
    }
    m_filesSymbolName = clean;
    m_filesSymbolMode =
        qBound(0, mode, int(forkmesh::code::TraceModeCount) - 1);
    for (int i = 0; i < m_filesSymbolModeButtons.size(); ++i)
        m_filesSymbolModeButtons.at(i)->setChecked(i == m_filesSymbolMode);
    // Opening the panel takes a third of the column, once: after that the
    // splitter keeps wherever the reader dragged it.
    const bool firstOpen = m_filesSymbolPanel->isHidden();
    m_filesSymbolPanel->show();
    if (firstOpen && m_filesEditorSplit) {
        const int height = qMax(360, m_filesEditorSplit->height());
        m_filesEditorSplit->setSizes({height * 2 / 3, height / 3});
    }
    showSection(kFilesSectionIndex);
    runFilesSymbolQuery();
}

void MainWindow::runFilesSymbolQuery()
{
    if (!m_filesSymbolTree)
        return;
    // Bumping the generation is what makes a scan the user has moved past drop
    // its result, and what invalidates the row pointers a pending expansion
    // holds.
    const int gen = ++m_filesSymbolGen;
    m_filesSymbolTree->clear();

    const QString symbol = m_filesSymbolName;
    const int mode = m_filesSymbolMode;
    const QString dir = repoGitDir();
    const QString ref = currentRef();
    const QString what = QString::fromLatin1(
        kSymbolModeTitles[qBound(0, mode, int(forkmesh::code::TraceModeCount) - 1)]);
    if (m_filesSymbolTitle)
        m_filesSymbolTitle->setText(
            QString::fromUtf8("%1 \xE2\x80\x9C%2\xE2\x80\x9D\xE2\x80\xA6")
                .arg(what, symbol));
    if (dir.isEmpty())
        return;

    runOffThread<QVector<SymbolRow>>(
        [dir, ref, symbol, mode]() -> QVector<SymbolRow> {
            return traceSymbolRows(dir, ref, symbol, mode);
        },
        [this, gen, symbol, what](const QVector<SymbolRow> &rows) {
            if (gen != m_filesSymbolGen || !m_filesSymbolTree)
                return;
            fillSymbolRows(m_filesSymbolTree, nullptr, rows, 0);
            if (m_filesSymbolTitle)
                m_filesSymbolTitle->setText(
                    QString::fromUtf8("%1 \xE2\x80\x9C%2\xE2\x80\x9D \xC2\xB7 %3")
                        .arg(what, symbol,
                             rows.isEmpty()
                                 ? QStringLiteral("nothing found")
                                 : QStringLiteral("%1 result%2")
                                       .arg(rows.size())
                                       .arg(rows.size() == 1 ? QString()
                                                             : QStringLiteral("s"))));
            // One definition is an answer, not a list: jump straight to it, the
            // way "go to definition" is expected to behave.
            if (m_filesSymbolMode == forkmesh::code::TraceDefinitions &&
                rows.size() == 1 && !rows.first().path.isEmpty())
                openRepoFileAtLine(rows.first().path, rows.first().line);
        });
}

// Trace one more step out from a row: its callers' callers, or its callees'
// callees. The rows arrive lazily so a deep trace only costs what is opened.
void MainWindow::expandFilesSymbolRow(QTreeWidgetItem *item)
{
    if (!item || !m_filesSymbolTree)
        return;
    if (item->data(0, kSymbolLoadedRole).toBool())
        return;
    const QString symbol = item->data(0, kSymbolNameRole).toString();
    const int depth = item->data(0, kSymbolDepthRole).toInt();
    if (symbol.isEmpty() || depth >= kMaxTraceDepth)
        return;
    item->setData(0, kSymbolLoadedRole, true);
    qDeleteAll(item->takeChildren()); // drop the expand-arrow placeholder

    const int gen = m_filesSymbolGen;
    const int mode = m_filesSymbolMode;
    const QString dir = repoGitDir();
    const QString ref = currentRef();
    if (dir.isEmpty())
        return;
    runOffThread<QVector<SymbolRow>>(
        [dir, ref, symbol, mode]() -> QVector<SymbolRow> {
            return traceSymbolRows(dir, ref, symbol, mode);
        },
        [this, gen, item, depth](const QVector<SymbolRow> &rows) {
            // The generation guard also covers `item`: every path that clears
            // the tree (a new query, closing the panel) bumps it first, so a
            // surviving generation means the row is still there.
            if (gen != m_filesSymbolGen || !m_filesSymbolTree)
                return;
            if (rows.isEmpty()) {
                auto *note = new QTreeWidgetItem(
                    item, {QStringLiteral("(nothing further)")});
                note->setFlags(Qt::NoItemFlags);
                return;
            }
            fillSymbolRows(m_filesSymbolTree, item, rows, depth + 1);
        });
}

void MainWindow::activateFilesSymbolRow(QTreeWidgetItem *item, int)
{
    if (!item)
        return;
    const QString path = item->data(0, kSymbolPathRole).toString();
    const int line = item->data(0, kSymbolLineRole).toInt();
    if (path.isEmpty())
        return;
    // Opens the Files section on that file's tab and parks it on the line.
    openRepoFileAtLine(path, line);
}

void MainWindow::closeFilesSymbolPanel()
{
    ++m_filesSymbolGen; // drop anything still scanning
    m_filesSymbolName.clear();
    if (m_filesSymbolTree)
        m_filesSymbolTree->clear();
    if (m_filesSymbolPanel)
        m_filesSymbolPanel->hide();
}

// ---- Find / replace over the open tab -------------------------------------

void MainWindow::openFilesFindBar(bool replace)
{
    ensureSectionBuilt(kFilesSectionIndex);
    if (!m_filesFindBar || !m_filesFindInput)
        return;
    m_filesFindBar->show();
    if (m_filesReplaceRow) {
        QPlainTextEdit *editor = currentFilesEditor();
        m_filesReplaceRow->setVisible(replace);
        m_filesReplaceRow->setEnabled(editor && !editor->isReadOnly());
    }
    // Open on what the cursor is already on, the way every editor does.
    const QString seed = filesEditorSymbol();
    if (!seed.isEmpty())
        m_filesFindInput->setText(seed);
    m_filesFindInput->setFocus();
    m_filesFindInput->selectAll();
    recomputeFilesFind();
}

void MainWindow::closeFilesFindBar()
{
    if (!m_filesFindBar || !m_filesFindBar->isVisible())
        return;
    m_filesFindBar->hide();
    if (m_filesFindInput)
        m_filesFindInput->clear();
    m_filesFindMatches.clear();
    m_filesFindIndex = -1;
    m_filesFindTab = nullptr;
    if (m_filesFindCount)
        m_filesFindCount->clear();
    if (auto *code = dynamic_cast<CodePreviewEditor *>(currentFilesEditor()))
        code->clearPersistentSelections();
    else if (QPlainTextEdit *editor = currentFilesEditor())
        editor->setExtraSelections({});
}

void MainWindow::recomputeFilesFind()
{
    QPlainTextEdit *editor = currentFilesEditor();
    // Matches are cursors into one document: dropping the old list before
    // touching a different tab is what keeps them from dangling.
    if (m_filesFindTab && m_filesFindTab != editor) {
        if (auto *previous = dynamic_cast<CodePreviewEditor *>(m_filesFindTab))
            previous->clearPersistentSelections();
    }
    m_filesFindMatches.clear();
    m_filesFindIndex = -1;
    m_filesFindTab = editor;

    const QString term = m_filesFindInput ? m_filesFindInput->text() : QString();
    if (editor && !term.isEmpty()) {
        QTextDocument::FindFlags flags;
        if (m_filesFindCaseButton && m_filesFindCaseButton->isChecked())
            flags |= QTextDocument::FindCaseSensitively;
        if (m_filesFindWordButton && m_filesFindWordButton->isChecked())
            flags |= QTextDocument::FindWholeWords;
        QTextCursor cursor = editor->document()->find(term, 0, flags);
        while (!cursor.isNull()) {
            m_filesFindMatches.append(cursor);
            if (m_filesFindMatches.size() >= 5000)
                break; // safety cap on pathological match counts
            cursor = editor->document()->find(term, cursor, flags);
        }
        if (!m_filesFindMatches.isEmpty()) {
            // Start from where the cursor already is rather than the top of the
            // file, so Ctrl+F on a word finds that word first.
            const int from = editor->textCursor().selectionStart();
            m_filesFindIndex = 0;
            for (int i = 0; i < m_filesFindMatches.size(); ++i) {
                if (m_filesFindMatches.at(i).selectionStart() >= from) {
                    m_filesFindIndex = i;
                    break;
                }
            }
        }
    }
    stepFilesFind(0);
}

void MainWindow::stepFilesFind(int delta)
{
    QPlainTextEdit *editor = currentFilesEditor();
    if (!editor)
        return;
    // F3 with the bar closed re-runs the last term rather than doing nothing.
    if (m_filesFindMatches.isEmpty() && delta != 0 && m_filesFindInput &&
        !m_filesFindInput->text().isEmpty() && m_filesFindTab != editor) {
        recomputeFilesFind();
    }
    const int count = m_filesFindMatches.size();
    if (count > 0 && delta != 0)
        m_filesFindIndex = ((m_filesFindIndex + delta) % count + count) % count;

    const QString term = m_filesFindInput ? m_filesFindInput->text() : QString();
    const QList<QTextEdit::ExtraSelection> selections =
        findMatchSelections(m_filesFindMatches, m_filesFindIndex);
    if (auto *code = dynamic_cast<CodePreviewEditor *>(editor))
        code->setPersistentSelections(selections);
    else
        editor->setExtraSelections(selections);
    if (m_filesFindCount)
        m_filesFindCount->setText(
            findMatchCountText(m_filesFindIndex, count, term.isEmpty()));
    if (m_filesFindIndex < 0 || m_filesFindIndex >= count)
        return;
    editor->setTextCursor(m_filesFindMatches.at(m_filesFindIndex));
    editor->centerCursor();
}

void MainWindow::replaceInFilesEditor(bool all)
{
    QPlainTextEdit *editor = currentFilesEditor();
    if (!editor || editor->isReadOnly() || !m_filesReplaceInput)
        return;
    if (m_filesFindMatches.isEmpty())
        return;
    const QString with = m_filesReplaceInput->text();
    if (all) {
        // Back to front, so replacing one match cannot shift the positions of
        // the matches still to come.
        const int replaced = m_filesFindMatches.size();
        QTextCursor edit(editor->document());
        edit.beginEditBlock();
        for (int i = m_filesFindMatches.size() - 1; i >= 0; --i) {
            QTextCursor match = m_filesFindMatches.at(i);
            match.insertText(with);
        }
        edit.endEditBlock();
        recomputeFilesFind();
        showLocalNotice(m_filesFindCount,
                        QStringLiteral("Replaced %1 match%2.")
                            .arg(replaced)
                            .arg(replaced == 1 ? QString()
                                               : QStringLiteral("es")));
        return;
    }
    if (m_filesFindIndex < 0 || m_filesFindIndex >= m_filesFindMatches.size())
        return;
    QTextCursor match = m_filesFindMatches.at(m_filesFindIndex);
    match.insertText(with);
    recomputeFilesFind();
}

void MainWindow::goToLineInFilesEditor()
{
    QPlainTextEdit *editor = currentFilesEditor();
    if (!editor)
        return;
    const int lines = qMax(1, editor->document()->blockCount());
    bool ok = false;
    // The dialog pumps the event loop; the tab can be closed behind it.
    QPointer<QPlainTextEdit> alive(editor);
    const int line = QInputDialog::getInt(
        this, QStringLiteral("Go to line"),
        QStringLiteral("Line (1 - %1):").arg(lines),
        editor->textCursor().blockNumber() + 1, 1, lines, 1, &ok);
    if (!ok || !alive)
        return;
    QTextCursor cursor(editor->document()->findBlockByNumber(line - 1));
    editor->setTextCursor(cursor);
    editor->centerCursor();
    editor->setFocus();
}

// Ctrl+Shift+O: every definition in the open file, filtered as you type.
void MainWindow::showFilesSymbolOutline()
{
    QPlainTextEdit *editor = currentFilesEditor();
    if (!editor) {
        flashMessage(QStringLiteral("Open a file to list its symbols."), true);
        return;
    }
    struct OutlineEntry {
        QString name;
        int line = 0;
    };
    QVector<OutlineEntry> entries;
    const QStringList lines = editor->toPlainText().split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i) {
        const QString name = forkmesh::code::definitionName(lines.at(i));
        if (!name.isEmpty())
            entries.append({name, i + 1});
    }
    if (entries.isEmpty()) {
        flashMessage(QStringLiteral("No symbols found in this file."), true);
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Go to symbol"));
    dialog.resize(520, 460);
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);
    auto *filter = new QLineEdit;
    filter->setPlaceholderText(
        QString::fromUtf8("Filter %1 symbols\xE2\x80\xA6").arg(entries.size()));
    filter->setClearButtonEnabled(true);
    layout->addWidget(filter);
    auto *list = new QListWidget;
    list->setObjectName(QStringLiteral("overviewList"));
    layout->addWidget(list, 1);

    auto fill = [&entries, list](const QString &query) {
        list->clear();
        for (const OutlineEntry &entry : entries) {
            if (!query.isEmpty() &&
                !entry.name.contains(query, Qt::CaseInsensitive))
                continue;
            auto *item = new QListWidgetItem(
                QStringLiteral("%1    %2").arg(entry.name).arg(entry.line));
            item->setData(Qt::UserRole, entry.line);
            list->addItem(item);
        }
        if (list->count() > 0)
            list->setCurrentRow(0);
    };
    fill(QString());
    connect(filter, &QLineEdit::textChanged, &dialog, fill);
    // Enter in the box takes the highlighted row, so the whole dialog is
    // type-then-return without touching the mouse.
    connect(filter, &QLineEdit::returnPressed, &dialog, [&dialog, list] {
        if (list->currentItem())
            dialog.accept();
    });
    connect(list, &QListWidget::itemActivated, &dialog, &QDialog::accept);
    connect(list, &QListWidget::itemDoubleClicked, &dialog, &QDialog::accept);
    filter->setFocus();
    // exec() pumps: the tab this outline was built from can be closed while the
    // dialog is up.
    QPointer<QPlainTextEdit> alive(editor);
    if (dialog.exec() != QDialog::Accepted || !list->currentItem() || !alive)
        return;
    const int line = list->currentItem()->data(Qt::UserRole).toInt();
    QTextCursor cursor(editor->document()->findBlockByNumber(qMax(0, line - 1)));
    editor->setTextCursor(cursor);
    editor->centerCursor();
    editor->setFocus();
}

void MainWindow::showFileExplorerMenu(const QPoint &pos)
{
    if (!m_fileExplorerTree)
        return;
    QTreeWidgetItem *item = m_fileExplorerTree->itemAt(pos);
    QString path = fileTreePath(item);
    if (path.isEmpty())
        path = m_fileExplorerRoot;
    if (path.isEmpty())
        return;
    const QFileInfo info(path);
    const bool isDir = info.isDir();

    QMenu menu(this);
    QAction *open = menu.addAction(
        isDir ? QStringLiteral("Browse here") : QStringLiteral("Open in a tab"));
    QAction *desktop =
        isDir ? nullptr
              : menu.addAction(QStringLiteral("Open with the desktop app"));
    QAction *reveal = menu.addAction(
        QStringLiteral("Show in desktop file manager"));
    const bool onItem = item != nullptr;
    const QString folder = isDir ? path : info.absolutePath();
    menu.addSeparator();
    QAction *copy = onItem ? menu.addAction(QStringLiteral("Copy")) : nullptr;
    QAction *move = onItem ? menu.addAction(QStringLiteral("Move")) : nullptr;
    QAction *paste = menu.addAction(
        isDir && onItem ? QStringLiteral("Paste into %1").arg(info.fileName())
                        : QStringLiteral("Paste"));
    paste->setEnabled(!forkmesh::smb::fileClipboard().items.isEmpty());
    QAction *rename = onItem ? menu.addAction(QStringLiteral("Rename…")) : nullptr;
    QAction *trash = onItem ? menu.addAction(QStringLiteral("Move to trash…")) : nullptr;
    menu.addSeparator();
    QAction *newFolder = menu.addAction(QStringLiteral("New folder"));
    QAction *newFile = menu.addAction(QStringLiteral("New file"));
    QAction *copyIn = menu.addAction(QStringLiteral("Copy files here…"));
    menu.addSeparator();
    QAction *copyPath = menu.addAction(QStringLiteral("Copy path"));
    QAction *copyName = menu.addAction(QStringLiteral("Copy name"));
    QAction *details = menu.addAction(QStringLiteral("Info"));

    QAction *chosen = menu.exec(m_fileExplorerTree->viewport()->mapToGlobal(pos));
    if (!chosen)
        return;
    if (chosen == copy || chosen == move) {
        copyLocalFilesToClipboard({info.absoluteFilePath()}, chosen == move);
    } else if (chosen == paste) {
        pasteIntoLocalFolder(folder);
    } else if (chosen == rename) {
        renameLocalEntry(info.absoluteFilePath());
    } else if (chosen == trash) {
        trashLocalEntries({info.absoluteFilePath()});
    } else if (chosen == newFolder || chosen == newFile) {
        createLocalEntry(folder, chosen == newFolder);
    } else if (chosen == copyIn) {
        uploadIntoLocalFolder(folder);
    } else if (chosen == details) {
        showLocalEntryInfo(info.absoluteFilePath());
    } else if (chosen == open) {
        if (isDir)
            setFileExplorerRoot(path);
        else
            openLocalFileTab(path);
    } else if (chosen == desktop) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    } else if (chosen == reveal) {
        revealInDesktopFileManager(path);
    } else if (chosen == copyPath) {
        QGuiApplication::clipboard()->setText(info.absoluteFilePath());
        flashMessage(QStringLiteral("Path copied."));
    } else if (chosen == copyName) {
        QGuiApplication::clipboard()->setText(info.fileName());
        flashMessage(QStringLiteral("Name copied."));
    }
}
