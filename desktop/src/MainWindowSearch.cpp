
#include "MainWindow.h"
#include "HiddenTerms.h"
#include "MainWindowInternal.h"
#include "SettingsSectionsView.h"

#include "ForkMeshPackageRegistry.h"
#include "GlobalSearchMatch.h"
#include "SearchHitDelegate.h"
#include "TimersFeature.h"

#include <QFocusFrame>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonObject>
#include <QListView>
#include <QPersistentModelIndex>
#include <QPushButton>
#include <QRegularExpression>
#include <QTableView>
#include <QTimer>
#include <QTreeView>

using namespace forkmesh::ui;
using namespace forkmesh::search;

namespace {

constexpr int kMaxPerCategory = 40;
constexpr int kMaxCodeHits = 80; // file contents: the one deliberately longer list
constexpr int kMaxCommits = 200; // rows `git log` is allowed to walk back over
constexpr int kGrepTimeoutMs = 8000;
constexpr int kGitTimeoutMs = 5000;
constexpr int kProjectGrepTimeoutMs = 20000;
constexpr int kMaxPageFindMatches = 2000;
constexpr int kMaxPageFindModelIndexes = 100000;

struct RepoBrief {
    int index = 0;
    QString owner;
    QString name;
    QString description;
};

struct HitBag {
    QVector<GlobalSearchHit> hits;
    QHash<QString, int> counts;

    bool room(const char *kind, int cap = kMaxPerCategory) const
    {
        return counts.value(QLatin1String(kind)) < cap;
    }
    void add(const char *kind, GlobalSearchHit hit)
    {
        hit.kind = QLatin1String(kind);
        counts[hit.kind] += 1;
        hits.append(std::move(hit));
    }
};

// code-health: allow(blocking-wait) sync git primitive; every caller (runGlobalSearch waves, scanGitCatalogHits/scanWorktreeCodeHits/scanProjectFileHits/scanSavedSearchGitHits) runs in a runOffThread work lambda
QStringList gitLines(const QString &dir, const QStringList &args, int timeoutMs)
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

QString pageFindLabelText(const QLabel *label)
{
    if (!label)
        return {};
    const QString text = label->text();
    if (label->textFormat() == Qt::PlainText ||
        (label->textFormat() == Qt::AutoText && !Qt::mightBeRichText(text)))
        return text;
    QTextDocument document;
    if (label->textFormat() == Qt::MarkdownText)
        document.setMarkdown(text);
    else
        document.setHtml(text);
    return document.toPlainText();
}

QString pageFindPreview(const QString &text, int matchStart, int matchLength)
{
    constexpr int kContextChars = 72;
    const int start = qMax(0, matchStart - kContextChars / 2);
    const int end = qMin(text.size(), matchStart + matchLength +
                                         kContextChars / 2);
    QString preview = text.mid(start, end - start).simplified();
    if (start > 0)
        preview.prepend(QChar(0x2026));
    if (end < text.size())
        preview.append(QChar(0x2026));
    return preview;
}

bool pageFindWidgetVisible(QWidget *widget, QWidget *root)
{
    if (!widget || !root)
        return false;
    if (widget != root && !root->isAncestorOf(widget))
        return false;
    return widget->isVisibleTo(root);
}

QString selectedPageText(QWidget *root)
{
    QWidget *focus = QApplication::focusWidget();
    if (!focus || !root ||
        (focus != root && !root->isAncestorOf(focus)))
        return {};

    QString selected;
    if (auto *plain = qobject_cast<QPlainTextEdit *>(focus))
        selected = plain->textCursor().selectedText();
    else if (auto *rich = qobject_cast<QTextEdit *>(focus))
        selected = rich->textCursor().selectedText();
    else if (auto *line = qobject_cast<QLineEdit *>(focus)) {
        if (line->echoMode() == QLineEdit::Normal)
            selected = line->selectedText();
    } else if (auto *label = qobject_cast<QLabel *>(focus)) {
        selected = label->selectedText();
    }
    selected.replace(QChar(0x2029), QLatin1Char(' '));
    selected = selected.simplified();
    return selected.size() <= 120 ? selected : QString();
}

struct PageFindMatch {
    QPointer<QWidget> target;
    QPersistentModelIndex modelIndex;
    QString kind;
    QString preview;
    int start = -1;
    int length = 0;
};

class PageFindController final : public QObject
{
public:
    PageFindController(MainWindow *window, QWidget *root, const QString &seed)
        : m_window(window), m_root(root)
    {
        m_dialog = new QDialog(window, Qt::Tool | Qt::WindowTitleHint |
                                           Qt::WindowCloseButtonHint);
        setParent(m_dialog); // the model and its decorations die with the popup
        m_dialog->setAttribute(Qt::WA_DeleteOnClose);
        m_dialog->setObjectName(QStringLiteral("pageFindDialog"));
        m_dialog->setWindowTitle(QStringLiteral("Find on this page"));
        m_dialog->setModal(false);
        m_dialog->resize(500, 300);

        auto *layout = new QVBoxLayout(m_dialog);
        layout->setContentsMargins(12, 12, 12, 12);
        layout->setSpacing(8);

        auto *searchRow = new QHBoxLayout;
        searchRow->setContentsMargins(0, 0, 0, 0);
        searchRow->setSpacing(6);

        m_input = new QLineEdit;
        m_input->setObjectName(QStringLiteral("pageFindInput"));
        m_input->setClearButtonEnabled(true);
        m_input->setPlaceholderText(QStringLiteral("Find on this page\u2026"));
        m_input->setAccessibleName(QStringLiteral("Find on this page"));
        m_input->installEventFilter(this);
        searchRow->addWidget(m_input, 1);

        m_count = new QLabel(QStringLiteral("Type to find"));
        m_count->setObjectName(QStringLiteral("pageFindCount"));
        m_count->setMinimumWidth(72);
        m_count->setAlignment(Qt::AlignCenter);
        searchRow->addWidget(m_count);

        m_previous = new QPushButton(QString::fromUtf8("\xE2\x86\x91"));
        m_previous->setObjectName(QStringLiteral("pageFindPrevious"));
        m_previous->setToolTip(QStringLiteral("Previous match (Shift+Enter)"));
        m_previous->setAccessibleName(QStringLiteral("Previous match"));
        m_previous->setProperty("buttonSize", "sm");
        m_previous->setEnabled(false);
        searchRow->addWidget(m_previous);

        m_next = new QPushButton(QString::fromUtf8("\xE2\x86\x93"));
        m_next->setObjectName(QStringLiteral("pageFindNext"));
        m_next->setToolTip(QStringLiteral("Next match (Enter)"));
        m_next->setAccessibleName(QStringLiteral("Next match"));
        m_next->setProperty("buttonSize", "sm");
        m_next->setEnabled(false);
        searchRow->addWidget(m_next);

        auto *close = new QPushButton(QString::fromUtf8("\xC3\x97"));
        close->setObjectName(QStringLiteral("pageFindClose"));
        close->setToolTip(QStringLiteral("Close (Esc)"));
        close->setAccessibleName(QStringLiteral("Close find"));
        close->setProperty("buttonSize", "sm");
        searchRow->addWidget(close);
        layout->addLayout(searchRow);

        m_hint = new QLabel(
            QStringLiteral("Searches the text, fields, and rows on the page "
                           "currently in view."));
        m_hint->setObjectName(QStringLiteral("statusLine"));
        m_hint->setWordWrap(true);
        layout->addWidget(m_hint);

        m_results = new QListWidget;
        m_results->setObjectName(QStringLiteral("pageFindResults"));
        m_results->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_results->setSelectionMode(QAbstractItemView::SingleSelection);
        m_results->setAccessibleName(QStringLiteral("Find results on this page"));
        layout->addWidget(m_results, 1);

        connect(m_input, &QLineEdit::textChanged, this,
                [this] { rebuild(); });
        connect(m_previous, &QPushButton::clicked, this,
                [this] { step(-1); });
        connect(m_next, &QPushButton::clicked, this, [this] { step(1); });
        connect(close, &QPushButton::clicked, m_dialog, &QDialog::close);
        connect(m_results, &QListWidget::currentRowChanged, this,
                [this](int row) { navigateTo(row); });
        connect(m_dialog, &QDialog::finished, this, [this] {
            m_closed = true;
            clearActiveDecoration();
        });
        connect(root, &QObject::destroyed, m_dialog, &QDialog::close);

        auto *findAgain = new QShortcut(QKeySequence::Find, m_dialog);
        connect(findAgain, &QShortcut::activated, this, [this] {
            m_input->setFocus();
            m_input->selectAll();
        });
        auto *nextShortcut = new QShortcut(QKeySequence(Qt::Key_F3), m_dialog);
        connect(nextShortcut, &QShortcut::activated, this, [this] { step(1); });
        auto *previousShortcut =
            new QShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F3), m_dialog);
        connect(previousShortcut, &QShortcut::activated, this,
                [this] { step(-1); });

        if (!seed.isEmpty())
            m_input->setText(seed);
        else
            rebuild();
    }

    ~PageFindController() override
    {
        clearActiveDecoration();
        if (m_focusFrame)
            m_focusFrame->deleteLater();
    }

    QWidget *root() const { return m_root.data(); }
    bool isClosed() const { return m_closed; }

    void showAndFocus()
    {
        if (!m_dialog || m_closed)
            return;
        if (m_dialog->isVisible())
            rebuild(); // the active tab or live page contents may have changed
        m_dialog->show();
        positionDialog();
        m_dialog->raise();
        m_dialog->activateWindow();
        m_input->setFocus();
        m_input->selectAll();
    }

    void close() const
    {
        if (m_dialog)
            m_dialog->close();
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == m_input && event->type() == QEvent::KeyPress) {
            auto *key = static_cast<QKeyEvent *>(event);
            if (key->key() == Qt::Key_Escape) {
                m_dialog->close();
                return true;
            }
            if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
                step(key->modifiers() & Qt::ShiftModifier ? -1 : 1);
                return true;
            }
        }
        return QObject::eventFilter(watched, event);
    }

private:
    void positionDialog()
    {
        if (!m_dialog || !m_root)
            return;
        const QPoint pageTopRight =
            m_root->mapToGlobal(QPoint(m_root->width(), 0));
        QRect bounds;
        if (QScreen *screen = QGuiApplication::screenAt(pageTopRight))
            bounds = screen->availableGeometry();
        if (!bounds.isValid() && m_window && m_window->screen())
            bounds = m_window->screen()->availableGeometry();

        QPoint wanted(pageTopRight.x() - m_dialog->width() - 16,
                      pageTopRight.y() + 12);
        if (bounds.isValid()) {
            wanted.setX(qBound(bounds.left() + 8, wanted.x(),
                               bounds.right() - m_dialog->width() - 8));
            wanted.setY(qBound(bounds.top() + 8, wanted.y(),
                               bounds.bottom() - m_dialog->height() - 8));
        }
        m_dialog->move(wanted);
    }

    void clearActiveDecoration()
    {
        if (m_focusFrame)
            m_focusFrame->hide();
        QWidget *target = m_activeTarget.data();
        if (!target)
            return;
        if (auto *plain = qobject_cast<QPlainTextEdit *>(target)) {
            QTextCursor cursor = plain->textCursor();
            cursor.clearSelection();
            plain->setTextCursor(cursor);
        } else if (auto *rich = qobject_cast<QTextEdit *>(target)) {
            QTextCursor cursor = rich->textCursor();
            cursor.clearSelection();
            rich->setTextCursor(cursor);
        } else if (auto *line = qobject_cast<QLineEdit *>(target)) {
            line->deselect();
        } else if (auto *label = qobject_cast<QLabel *>(target)) {
            if (label->hasSelectedText())
                label->setSelection(-1, 0);
        }
        m_activeTarget.clear();
    }

    void addTextMatches(QWidget *target, const QString &rawText,
                        const QString &kind,
                        const QPersistentModelIndex &modelIndex = {})
    {
        if (m_query.isEmpty() || !target || rawText.isEmpty())
            return;
        int from = 0;
        while (from <= rawText.size() - m_query.size()) {
            const int at = rawText.indexOf(m_query, from, Qt::CaseInsensitive);
            if (at < 0)
                break;
            if (m_matches.size() >= kMaxPageFindMatches) {
                m_truncated = true;
                return;
            }
            PageFindMatch match;
            match.target = target;
            match.modelIndex = modelIndex;
            match.kind = kind;
            match.preview = pageFindPreview(rawText, at, m_query.size());
            match.start = at;
            match.length = m_query.size();
            m_matches.append(std::move(match));
            from = at + qMax(1, m_query.size());
        }
    }

    void addDocumentMatches(QWidget *target, QTextDocument *document,
                            const QString &kind)
    {
        if (!document || m_query.isEmpty())
            return;
        const QString plain = document->toPlainText();
        QTextCursor cursor = document->find(m_query, 0);
        while (!cursor.isNull()) {
            if (m_matches.size() >= kMaxPageFindMatches) {
                m_truncated = true;
                return;
            }
            PageFindMatch match;
            match.target = target;
            match.kind = kind;
            match.start = cursor.selectionStart();
            match.length = cursor.selectionEnd() - cursor.selectionStart();
            match.preview = pageFindPreview(plain, match.start, match.length);
            m_matches.append(std::move(match));
            cursor = document->find(m_query, cursor);
        }
    }

    bool modelRowHidden(QAbstractItemView *view, int row,
                        const QModelIndex &parent) const
    {
        if (auto *tree = qobject_cast<QTreeView *>(view))
            return tree->isRowHidden(row, parent);
        if (!parent.isValid()) {
            if (auto *table = qobject_cast<QTableView *>(view))
                return table->isRowHidden(row);
            if (auto *list = qobject_cast<QListView *>(view))
                return list->isRowHidden(row);
        }
        return false;
    }

    bool modelColumnHidden(QAbstractItemView *view, int column) const
    {
        if (auto *tree = qobject_cast<QTreeView *>(view))
            return tree->isColumnHidden(column);
        if (auto *table = qobject_cast<QTableView *>(view))
            return table->isColumnHidden(column);
        return false;
    }

    void addModelMatches(QAbstractItemView *view, const QModelIndex &parent = {})
    {
        QAbstractItemModel *model = view ? view->model() : nullptr;
        if (!model || m_truncated)
            return;
        const int rows = model->rowCount(parent);
        const int columns = model->columnCount(parent);
        const QString kind = qobject_cast<QTreeView *>(view)
                                 ? QStringLiteral("Tree item")
                                 : qobject_cast<QTableView *>(view)
                                       ? QStringLiteral("Table cell")
                                       : QStringLiteral("List item");
        for (int row = 0; row < rows && !m_truncated; ++row) {
            if (modelRowHidden(view, row, parent))
                continue;
            for (int column = 0; column < columns; ++column) {
                if (++m_modelIndexesVisited > kMaxPageFindModelIndexes) {
                    m_truncated = true;
                    return;
                }
                if (modelColumnHidden(view, column))
                    continue;
                const QModelIndex index = model->index(row, column, parent);
                addTextMatches(view, index.data(Qt::DisplayRole).toString(),
                               kind, QPersistentModelIndex(index));
            }

            const QModelIndex branch = model->index(row, 0, parent);
            if (!branch.isValid() || !model->hasChildren(branch))
                continue;
            auto *tree = qobject_cast<QTreeView *>(view);
            if (!tree || !tree->isExpanded(branch))
                continue; // only expanded tree descendants are on this page
            addModelMatches(view, branch);
        }
    }

    void collectMatches()
    {
        if (!m_root)
            return;
        QList<QWidget *> widgets = m_root->findChildren<QWidget *>();
        widgets.prepend(m_root);
        for (QWidget *widget : std::as_const(widgets)) {
            if (m_truncated || !pageFindWidgetVisible(widget, m_root))
                continue;
            if (auto *plain = qobject_cast<QPlainTextEdit *>(widget)) {
                addDocumentMatches(plain, plain->document(),
                                   QStringLiteral("Text"));
            } else if (auto *rich = qobject_cast<QTextEdit *>(widget)) {
                addDocumentMatches(rich, rich->document(),
                                   QStringLiteral("Text"));
            } else if (auto *view = qobject_cast<QAbstractItemView *>(widget)) {
                if (!qobject_cast<QHeaderView *>(view))
                    addModelMatches(view);
            } else if (auto *line = qobject_cast<QLineEdit *>(widget)) {
                if (line->echoMode() == QLineEdit::Normal)
                    addTextMatches(line, line->text(), QStringLiteral("Field"));
            } else if (auto *label = qobject_cast<QLabel *>(widget)) {
                addTextMatches(label, pageFindLabelText(label),
                               QStringLiteral("Text"));
            } else if (auto *button = qobject_cast<QAbstractButton *>(widget)) {
                addTextMatches(button, button->text(),
                               QStringLiteral("Button"));
            } else if (auto *tabs = qobject_cast<QTabBar *>(widget)) {
                for (int i = 0; i < tabs->count(); ++i)
                    if (tabs->isTabVisible(i))
                        addTextMatches(tabs, tabs->tabText(i),
                                       QStringLiteral("Tab"));
            } else if (auto *combo = qobject_cast<QComboBox *>(widget)) {
                addTextMatches(combo, combo->currentText(),
                               QStringLiteral("Selection"));
            } else if (auto *group = qobject_cast<QGroupBox *>(widget)) {
                addTextMatches(group, group->title(),
                               QStringLiteral("Section"));
            }
        }
    }

    void rebuild()
    {
        clearActiveDecoration();
        m_matches.clear();
        m_results->clear();
        m_activeIndex = -1;
        m_modelIndexesVisited = 0;
        m_truncated = false;
        m_query = m_input->text();

        if (m_query.isEmpty()) {
            m_count->setText(QStringLiteral("Type to find"));
            m_hint->setText(
                QStringLiteral("Searches the text, fields, and rows on the page "
                               "currently in view."));
            m_previous->setEnabled(false);
            m_next->setEnabled(false);
            return;
        }

        collectMatches();
        for (const PageFindMatch &match : std::as_const(m_matches)) {
            auto *item = new QListWidgetItem(
                QStringLiteral("%1  \u00b7  %2").arg(match.kind, match.preview));
            item->setToolTip(match.preview);
            m_results->addItem(item);
        }

        const bool found = !m_matches.isEmpty();
        m_previous->setEnabled(found);
        m_next->setEnabled(found);
        if (!found) {
            m_count->setText(QStringLiteral("No matches"));
            m_hint->setText(QStringLiteral("No matches on this page."));
            return;
        }
        m_hint->setText(m_truncated
                            ? QStringLiteral("Showing the first 2,000 matches.")
                            : QStringLiteral("Select a result or use Enter and "
                                             "Shift+Enter to move through them."));
        m_results->setCurrentRow(0);
    }

    void step(int delta)
    {
        const int count = m_matches.size();
        if (count <= 0)
            return;
        const int current = m_activeIndex >= 0 ? m_activeIndex : 0;
        const int next = ((current + delta) % count + count) % count;
        m_results->setCurrentRow(next);
        m_results->scrollToItem(m_results->item(next));
    }

    void revealWidget(QWidget *target)
    {
        if (!target)
            return;
        for (QWidget *ancestor = target->parentWidget(); ancestor;
             ancestor = ancestor->parentWidget()) {
            if (auto *scroll = qobject_cast<QScrollArea *>(ancestor))
                scroll->ensureWidgetVisible(target, 24, 24);
            if (ancestor == m_root)
                break;
        }
    }

    void navigateTo(int index)
    {
        if (index < 0 || index >= m_matches.size())
            return;
        clearActiveDecoration();
        m_activeIndex = index;
        const PageFindMatch &match = m_matches.at(index);
        QWidget *target = match.target.data();
        if (!target)
            return;
        m_activeTarget = target;

        revealWidget(target);
        if (match.modelIndex.isValid()) {
            if (auto *view = qobject_cast<QAbstractItemView *>(target)) {
                view->setCurrentIndex(match.modelIndex);
                view->scrollTo(match.modelIndex,
                               QAbstractItemView::PositionAtCenter);
            }
        } else if (auto *plain = qobject_cast<QPlainTextEdit *>(target)) {
            QTextCursor cursor(plain->document());
            cursor.setPosition(match.start);
            cursor.setPosition(match.start + match.length,
                               QTextCursor::KeepAnchor);
            plain->setTextCursor(cursor);
            plain->centerCursor();
        } else if (auto *rich = qobject_cast<QTextEdit *>(target)) {
            QTextCursor cursor(rich->document());
            cursor.setPosition(match.start);
            cursor.setPosition(match.start + match.length,
                               QTextCursor::KeepAnchor);
            rich->setTextCursor(cursor);
            rich->ensureCursorVisible();
        } else if (auto *line = qobject_cast<QLineEdit *>(target)) {
            line->setSelection(match.start, match.length);
        } else if (auto *label = qobject_cast<QLabel *>(target)) {
            const bool plain = label->textFormat() == Qt::PlainText ||
                               (label->textFormat() == Qt::AutoText &&
                                !Qt::mightBeRichText(label->text()));
            if (plain)
                label->setSelection(match.start, match.length);
        }

        if (!m_focusFrame) {
            m_focusFrame = new QFocusFrame(m_window);
            m_focusFrame->setObjectName(QStringLiteral("pageFindFocusFrame"));
        }
        m_focusFrame->setWidget(target);
        m_focusFrame->show();
        m_focusFrame->raise();
        m_count->setText(QStringLiteral("%1 of %2%3")
                             .arg(index + 1)
                             .arg(m_matches.size())
                             .arg(m_truncated ? QStringLiteral("+") : QString()));
    }

    QPointer<MainWindow> m_window;
    QPointer<QWidget> m_root;
    QPointer<QDialog> m_dialog;
    QLineEdit *m_input = nullptr;
    QLabel *m_count = nullptr;
    QLabel *m_hint = nullptr;
    QListWidget *m_results = nullptr;
    QPushButton *m_previous = nullptr;
    QPushButton *m_next = nullptr;
    QPointer<QWidget> m_activeTarget;
    QPointer<QFocusFrame> m_focusFrame;
    QVector<PageFindMatch> m_matches;
    QString m_query;
    int m_activeIndex = -1;
    int m_modelIndexesVisited = 0;
    bool m_truncated = false;
    bool m_closed = false;
};

} // namespace

QWidget *MainWindow::pageFindRoot() const
{
    if (m_stack && m_stack->currentIndex() == 0)
        return m_stack->currentWidget();
    if (m_sectionStack && m_sectionStack->currentWidget())
        return m_sectionStack->currentWidget();
    if (m_stack && m_stack->currentWidget())
        return m_stack->currentWidget();
    return centralWidget();
}

void MainWindow::openPageFind()
{
    auto closePageDialog = [this] {
        if (auto *controller =
                static_cast<PageFindController *>(m_pageFindController.data()))
            controller->close();
    };

    // These surfaces already provide document-aware search, including all-hit
    // highlighting and paged-diff traversal. Keep those richer behaviors while
    // making the same standard shortcut work on every other page too.
    if (m_pullDiff && m_pullDiff->isVisibleTo(this)) {
        closePageDialog();
        togglePullDiffSearch(true);
        return;
    }
    if (m_branchDiffView && m_branchDiffView->isVisibleTo(this)) {
        closePageDialog();
        toggleBranchDiffSearch(true);
        return;
    }
    // Settings has its own search, which narrows the page to the matching
    // sections instead of stepping through its text.
    if (m_settingsSections && m_settingsSections->isVisibleTo(this)) {
        closePageDialog();
        m_settingsSections->focusSearch();
        return;
    }
    if (m_sectionStack &&
        m_sectionStack->currentIndex() == kFilesSectionIndex &&
        currentFilesEditor()) {
        closePageDialog();
        openFilesFindBar(false);
        return;
    }

    QWidget *root = pageFindRoot();
    if (!root)
        return;
    if (auto *controller =
            static_cast<PageFindController *>(m_pageFindController.data())) {
        if (!controller->isClosed() && controller->root() == root) {
            controller->showAndFocus();
            return;
        }
        if (!controller->isClosed())
            controller->close();
    }

    auto *controller =
        new PageFindController(this, root, selectedPageText(root));
    m_pageFindController = controller;
    controller->showAndFocus();
}

void MainWindow::openGlobalSearch()
{
    QDialog dialog(this);
    m_searchDialog = &dialog;
    dialog.setWindowTitle("Search everything");
    dialog.setObjectName("searchDialog");
    dialog.resize(760, 560);
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(8);

    m_searchInput = new QLineEdit;
    m_searchInput->setObjectName("searchInput");
    m_searchInput->setClearButtonEnabled(true);
    m_searchInput->setPlaceholderText(
        "Search repositories, issues, pull requests, discussions, agents, "
        "logs, branches, worktrees, commits and code…");
    layout->addWidget(m_searchInput);

    m_searchStatus = new QLabel(
        "Type at least 2 characters to search everything on this node.");
    m_searchStatus->setObjectName("statusLine");
    layout->addWidget(m_searchStatus);

    m_searchList = new QListWidget;
    m_searchList->setObjectName("searchResults");
    m_searchList->setUniformItemSizes(false);
    SearchHitDelegate::install(m_searchList);
    layout->addWidget(m_searchList, 1);

    auto *debounce = new QTimer(&dialog);
    debounce->setSingleShot(true);
    debounce->setInterval(180);
    connect(m_searchInput, &QLineEdit::textChanged, debounce,
            qOverload<>(&QTimer::start));
    connect(debounce, &QTimer::timeout, this,
            [this] { runGlobalSearch(m_searchInput->text()); });

    auto activate = [this, &dialog](QListWidgetItem *item) {
        if (!item)
            return;
        GlobalSearchHit hit;
        hit.kind = item->data(Qt::UserRole).toString();
        if (hit.kind.isEmpty())
            return;
        hit.number = item->data(Qt::UserRole + 1).toInt();
        hit.path = item->data(Qt::UserRole + 2).toString();
        hit.line = item->data(Qt::UserRole + 3).toInt();
        hit.repoIndex = item->data(Qt::UserRole + 4).toInt();
        dialog.accept();
        activateGlobalSearchHit(hit);
    };
    connect(m_searchList, &QListWidget::itemActivated, this, activate);
    connect(m_searchList, &QListWidget::itemClicked, this, activate);

    m_searchInput->setFocus();
    dialog.exec();

    ++m_searchGen;
    m_searchWavesPending = 0;
    m_searchHits.clear();
    m_searchDialog = nullptr;
    m_searchInput = nullptr;
    m_searchList = nullptr;
    m_searchStatus = nullptr;
}

QVector<GlobalSearchHit> MainWindow::scanGitCatalogHits(const QString &gitDir,
                                                        const QString &ref,
                                                        const QString &query,
                                                        int capPerKind,
                                                        int repoIndex,
                                                        const QString &repoLabel)
{
    QVector<GlobalSearchHit> hits;
    if (gitDir.isEmpty() || query.isEmpty() || capPerKind <= 0)
        return hits;

    const auto labelled = [&repoLabel](const QString &detail) {
        if (repoLabel.isEmpty())
            return detail;
        return detail.isEmpty() ? repoLabel
                                : repoLabel + QStringLiteral(" · ") + detail;
    };
    const auto append = [&](const char *kind, const QString &path,
                            const QString &primary, const QString &detail) {
        GlobalSearchHit hit;
        hit.kind = QLatin1String(kind);
        hit.path = path;
        hit.primary = primary;
        hit.detail = labelled(detail);
        hit.repoIndex = repoIndex;
        hits.append(hit);
    };

    int branches = 0;
    for (const QString &row :
         gitLines(gitDir,
                  {QStringLiteral("for-each-ref"),
                   QStringLiteral("--sort=-committerdate"),
                   QStringLiteral("--format=%(refname:short)%1f%(objectname:"
                                  "short)%1f%(contents:subject)"),
                   QStringLiteral("refs/heads"),
                   QStringLiteral("refs/remotes")},
                  kGitTimeoutMs)) {
        if (branches >= capPerKind)
            break;
        const QStringList f = row.split(QChar(0x1f));
        if (f.isEmpty())
            continue;
        const QString name = f.value(0);
        if (isLedgerBranch(name))
            continue;
        const QString subject = f.value(2);
        QString where;
        if (containsFold(name, query))
            where = f.value(1);
        else if (containsFold(subject, query))
            where = f.value(1) + QStringLiteral("  ") + subject;
        else
            continue;
        append("branch", name, name, where);
        ++branches;
    }

    int worktrees = 0;
    for (const WorktreeRecord &wt : parseWorktreePorcelain(
             gitLines(gitDir,
                      {QStringLiteral("worktree"), QStringLiteral("list"),
                       QStringLiteral("--porcelain")},
                      kGitTimeoutMs))) {
        if (worktrees >= capPerKind)
            break;
        if (isLedgerBranch(wt.branch))
            continue;
        if (!containsFold(wt.path, query) && !containsFold(wt.branch, query) &&
            !(query.size() >= 4 && wt.head.startsWith(query, Qt::CaseInsensitive)))
            continue;
        // switchToWorktree() navigates by branch.
        append("worktree", wt.branch,
               wt.detached ? QStringLiteral("(detached %1)").arg(wt.head.left(8))
                           : (wt.branch.isEmpty() ? QStringLiteral("(no branch)")
                                                  : wt.branch),
               wt.path);
        ++worktrees;
    }

    int tags = 0;
    for (const QString &row :
         gitLines(gitDir,
                  {QStringLiteral("for-each-ref"),
                   QStringLiteral("--sort=-creatordate"),
                   QStringLiteral("--format=%(refname:short)%1f%(objectname:"
                                  "short)%1f%(contents:subject)"),
                   QStringLiteral("refs/tags")},
                  kGitTimeoutMs)) {
        if (tags >= capPerKind)
            break;
        const QStringList f = row.split(QChar(0x1f));
        if (f.isEmpty())
            continue;
        const QString name = f.value(0);
        if (!containsFold(name, query) && !containsFold(f.value(2), query))
            continue;
        append("tag", name, name, f.value(2));
        ++tags;
    }

    if (!ref.isEmpty()) {
        int paths = 0;
        for (const QString &path :
             gitLines(gitDir,
                      {QStringLiteral("ls-tree"), QStringLiteral("-r"),
                       QStringLiteral("--name-only"), ref},
                      kGitTimeoutMs)) {
            if (paths >= capPerKind)
                break;
            if (!containsFold(path, query))
                continue;
            append("file", path, path, QString());
            hits.last().line = 1;
            ++paths;
        }
    }

    return hits;
}

QVector<GlobalSearchGitRepo> MainWindow::globalSearchGitRepos() const
{
    QVector<GlobalSearchGitRepo> repos;
    repos.reserve(m_repositories.size());
    for (int i = 0; i < m_repositories.size(); ++i) {
        const RepositoryRecord &record = m_repositories.at(i);
        if (record.localPath.isEmpty() && record.mirrorPath.isEmpty())
            continue;
        GlobalSearchGitRepo repo;
        repo.index = i;
        repo.label = record.owner.isEmpty()
                         ? record.name
                         : QStringLiteral("%1/%2").arg(record.owner, record.name);
        if (forkmesh::hiddenterms::hides(repo.label + QLatin1Char('\n') +
                                         record.localPath + QLatin1Char('\n') +
                                         record.mirrorPath))
            continue;
        repo.localPath = record.localPath;
        repo.mirrorPath = record.mirrorPath;
        // The open repository leads, so its hits survive every per-kind cap.
        if (i == m_repoDetailIndex)
            repos.prepend(repo);
        else
            repos.append(repo);
    }
    return repos;
}

QVector<GlobalSearchHit> MainWindow::scanAllGitCatalogHits(
    const QVector<GlobalSearchGitRepo> &repos, int openIndex,
    const QString &openRef, const QString &query, int capPerKind)
{
    QVector<GlobalSearchHit> hits;
    QHash<QString, int> counts;
    QSet<QString> seenDirs;
    for (const GlobalSearchGitRepo &repo : repos) {
        // Same rule as repoGitDir(): the working clone, else the mirror.
        QString dir;
        if (!repo.localPath.isEmpty() && QDir(repo.localPath).exists())
            dir = repo.localPath;
        else if (!repo.mirrorPath.isEmpty() && QDir(repo.mirrorPath).exists())
            dir = repo.mirrorPath;
        if (dir.isEmpty())
            continue;
        const QString canonical = QDir(dir).canonicalPath();
        if (seenDirs.contains(canonical))
            continue; // a preview row can share its source's clone
        seenDirs.insert(canonical);

        const bool open = repo.index == openIndex;
        for (GlobalSearchHit &hit : scanGitCatalogHits(
                 dir, open ? openRef : QString(), query, capPerKind,
                 repo.index, open ? QString() : repo.label)) {
            int &count = counts[hit.kind];
            if (count >= capPerKind)
                continue;
            ++count;
            hits.append(std::move(hit));
        }
    }
    return hits;
}

QVector<GlobalSearchHit> MainWindow::scanWorktreeCodeHits(const QString &gitDir,
                                                          const QString &ref,
                                                          const QString &query,
                                                          int cap)
{
    constexpr int kMaxWorktrees = 32;
    constexpr int kMaxChangedPaths = 2000;
    QVector<GlobalSearchHit> hits;
    if (gitDir.isEmpty() || query.isEmpty() || cap <= 0)
        return hits;

    int scanned = 0;
    for (const WorktreeRecord &wt : parseWorktreePorcelain(
             gitLines(gitDir,
                      {QStringLiteral("worktree"), QStringLiteral("list"),
                       QStringLiteral("--porcelain")},
                      kGitTimeoutMs))) {
        if (hits.size() >= cap || scanned >= kMaxWorktrees)
            break;
        if (isLedgerBranch(wt.branch) || !QDir(wt.path).exists())
            continue;
        ++scanned;

        // Only what this worktree changed or added against the browsed ref:
        // every other file is identical to the tree the Code group already
        // searched, and repeating it once per agent worktree is noise.
        QStringList paths =
            gitLines(wt.path,
                     {QStringLiteral("diff"), QStringLiteral("--name-only"),
                      QStringLiteral("--diff-filter=d"),
                      ref.isEmpty() ? QStringLiteral("HEAD") : ref,
                      QStringLiteral("--")},
                     kGitTimeoutMs);
        paths += gitLines(wt.path,
                          {QStringLiteral("ls-files"), QStringLiteral("--others"),
                           QStringLiteral("--exclude-standard")},
                          kGitTimeoutMs);
        paths.removeDuplicates();
        if (paths.isEmpty())
            continue;
        if (paths.size() > kMaxChangedPaths)
            paths = paths.mid(0, kMaxChangedPaths);

        QStringList args{QStringLiteral("grep"), QStringLiteral("--untracked"),
                         QStringLiteral("-n"), QStringLiteral("-I"),
                         QStringLiteral("-i"), QStringLiteral("-F"),
                         QStringLiteral("-e"), query, QStringLiteral("--")};
        args += paths;
        const QString branch =
            wt.branch.isEmpty() ? QStringLiteral("detached %1").arg(wt.head.left(8))
                                : wt.branch;
        // Name the worktree (its folder) as well as its branch, so hits from
        // agent worktrees on look-alike branches can be told apart.
        const QString worktreeName = QFileInfo(QDir::cleanPath(wt.path)).fileName();
        const QString where =
            worktreeName.isEmpty() || worktreeName == branch
                ? branch
                : QStringLiteral("%1 (%2)").arg(worktreeName, branch);
        for (const QString &raw : gitLines(wt.path, args, kGrepTimeoutMs)) {
            if (hits.size() >= cap)
                break;
            const CodeRow row = parseGrepRow(raw, QString());
            if (!row.valid)
                continue;
            GlobalSearchHit hit;
            hit.kind = QStringLiteral("worktree-code");
            hit.path = QDir(wt.path).filePath(row.path); // opened from disk
            hit.line = row.line;
            hit.primary = QStringLiteral("%1:%2").arg(row.path).arg(row.line);
            hit.detail = where + QStringLiteral(" · ") + row.text.left(160);
            hits.append(hit);
        }
    }
    return hits;
}

QVector<GlobalSearchHit> MainWindow::scanProjectFileHits(const QString &workDir,
                                                         const QString &query,
                                                         int cap)
{
    QVector<GlobalSearchHit> hits;
    if (workDir.isEmpty() || query.isEmpty() || cap <= 0 ||
        !QFileInfo::exists(QDir(workDir).filePath(QStringLiteral(".git"))))
        return hits;
    for (const QString &raw :
         gitLines(workDir,
                  {QStringLiteral("grep"), QStringLiteral("--untracked"),
                   QStringLiteral("-n"), QStringLiteral("-I"), QStringLiteral("-i"),
                   QStringLiteral("-F"), QStringLiteral("-e"), query},
                  kProjectGrepTimeoutMs)) {
        if (hits.size() >= cap)
            break;
        const CodeRow row = parseGrepRow(raw, QString());
        if (!row.valid)
            continue;
        GlobalSearchHit hit;
        hit.kind = QStringLiteral("project-file");
        hit.path = QDir(workDir).filePath(row.path);
        hit.line = row.line;
        hit.primary = QStringLiteral("%1:%2").arg(row.path).arg(row.line);
        hit.detail = row.text.left(160);
        hits.append(hit);
    }
    return hits;
}

QVector<GlobalSearchHit> MainWindow::scanSavedSearchGitHits(const QString &gitDir,
                                                            const QString &ref,
                                                            const QString &query,
                                                            int capPerKind)
{
    QVector<GlobalSearchHit> hits;
    if (gitDir.isEmpty() || query.isEmpty() || capPerKind <= 0)
        return hits;
    const QString rev = ref.isEmpty() ? QStringLiteral("HEAD") : ref;

    // The same three Git searches the results page streams, with the same
    // arguments, so a background run and a watched run agree on what exists.
    int code = 0;
    for (const QString &raw :
         gitLines(gitDir,
                  {QStringLiteral("grep"), QStringLiteral("-n"), QStringLiteral("-I"),
                   QStringLiteral("-i"), QStringLiteral("-F"), QStringLiteral("-e"),
                   query, rev},
                  kGrepTimeoutMs)) {
        if (code >= capPerKind)
            break;
        const CodeRow row = parseGrepRow(raw, rev);
        if (!row.valid)
            continue;
        GlobalSearchHit hit;
        hit.kind = QStringLiteral("file-content");
        hit.path = row.path;
        hit.line = row.line;
        hit.primary = QStringLiteral("%1:%2").arg(row.path).arg(row.line);
        hit.detail = row.text;
        hits.append(hit);
        ++code;
    }

    const auto commits = [&](const QString &kind, const QStringList &args) {
        int count = 0;
        for (const QString &line : gitLines(gitDir, args, kGrepTimeoutMs)) {
            if (count >= capPerKind)
                break;
            const QStringList fields = line.split(QChar(0x1f));
            if (fields.size() < 4 || fields.at(0).isEmpty())
                continue;
            GlobalSearchHit hit;
            hit.kind = kind;
            hit.path = fields.at(0);
            hit.primary = fields.at(3);
            hit.detail = fields.at(2);
            hits.append(hit);
            ++count;
        }
    };
    const QString maxCount = QStringLiteral("--max-count=%1").arg(capPerKind);
    commits(QStringLiteral("commit-message"),
            QStringList{QStringLiteral("log"), QStringLiteral("-i"),
                        QStringLiteral("--fixed-strings"), maxCount,
                        QStringLiteral("--grep=") + query,
                        QStringLiteral("--format=%H%x1f%h%x1f%ar%x1f%s")} +
                allBranchRevisionArgs(rev));
    commits(QStringLiteral("history"),
            {QStringLiteral("log"), QStringLiteral("-i"), maxCount,
             QStringLiteral("-G") + QRegularExpression::escape(query),
             QStringLiteral("--format=%H%x1f%h%x1f%ar%x1f%s"), rev});
    return hits;
}

// code-health: allow(blocking-wait) the git grep wait is inside a runOffThread work lambda; nothing blocks the GUI thread here
void MainWindow::runGlobalSearch(const QString &rawQuery)
{
    if (!m_searchList || !m_searchStatus)
        return;
    const int gen = ++m_searchGen;
    m_searchHits.clear();
    m_searchWavesPending = 0;
    m_searchList->clear();

    const QString query = rawQuery.trimmed();
    if (query.size() < 2) {
        m_searchStatus->setText(
            "Type at least 2 characters to search everything on this node.");
        return;
    }
    m_searchStatus->setText(QStringLiteral("Searching for “%1”…").arg(query));

    // Capture everything the workers need by value — nothing GUI-owned is
    // touched inside a work lambda. The stores are read-only here (no signing),
    // so the identity pointer is only ever read.
    QVector<RepoBrief> repos;
    repos.reserve(m_repositories.size());
    for (int i = 0; i < m_repositories.size(); ++i) {
        const RepositoryRecord &record = m_repositories.at(i);
        if (forkmesh::hiddenterms::hides(
                QStringLiteral("%1/%2\n%3\n%4\n%5")
                    .arg(record.owner, record.name, record.description,
                         record.localPath, record.mirrorPath)))
            continue;
        repos.append({i, record.owner, record.name, record.description});
    }
    const int openIndex = m_repoDetailIndex;
    const bool repoOpen = openIndex >= 0 && openIndex < m_repositories.size();
    QString localPath, mirrorPath;
    if (repoOpen) {
        const RepositoryRecord &repo =
            writableRecordFor(m_repositories.at(openIndex));
        localPath = repo.localPath;
        mirrorPath = repo.mirrorPath;
    }
    const ForkMeshIdentity *identity = &m_profileIdentity;
    const QString userName = m_userName;
    const QString gitDir = repoOpen ? repoGitDir() : QString();
    const QString ref = repoOpen ? currentRef() : QString();
    const QList<AgentSession> sessions = m_agentSessions;
    const QHash<int, AgentDiffStat> diffStats = m_agentDiffStats;
    const QList<ActionRun> runs = m_actionRuns;
    const QStringList networkLog = m_networkLog;
    const QVector<GlobalSearchGitRepo> gitRepos = globalSearchGitRepos();

    auto deliver = [this, gen, query](const QVector<GlobalSearchHit> &hits) {
        if (gen != m_searchGen || !m_searchList || !m_searchStatus)
            return;
        static const QHash<QString, QString> kindPackages = {
            {QStringLiteral("issue"), QStringLiteral("issues")},
            {QStringLiteral("milestone"), QStringLiteral("issues")},
            {QStringLiteral("pull"), QStringLiteral("pull-requests")},
            {QStringLiteral("discussion"), QStringLiteral("discussions")},
            {QStringLiteral("agent"), QStringLiteral("agents")},
            {QStringLiteral("action"), QStringLiteral("actions")},
            {QStringLiteral("commit"), QStringLiteral("commits")},
            {QStringLiteral("branch"), QStringLiteral("branches")},
            {QStringLiteral("code"), QStringLiteral("code")},
        };
        for (const GlobalSearchHit &hit : hits) {
            const QString package = kindPackages.value(hit.kind);
            if (package.isEmpty() || packageSurfaceShown(package))
                m_searchHits.append(hit);
        }
        if (m_searchWavesPending > 0)
            --m_searchWavesPending;
        rebuildGlobalSearchList(query);
    };

    m_searchWavesPending = 4;

    runOffThread<QVector<GlobalSearchHit>>(
        [=]() -> QVector<GlobalSearchHit> {
            HitBag bag;

            for (const RepoBrief &repo : repos) {
                if (!bag.room("repo"))
                    break;
                const QString slug =
                    QStringLiteral("%1/%2").arg(repo.owner, repo.name);
                QString where;
                if (containsFold(slug, query))
                    where = QStringLiteral("repository");
                else if (containsFold(repo.description, query))
                    where = snippetAround(repo.description, query);
                else
                    continue;
                GlobalSearchHit hit;
                hit.number = repo.index;
                hit.repoIndex = repo.index;
                hit.primary = slug;
                hit.detail = repo.index == openIndex
                                 ? where + QStringLiteral(" · open")
                                 : where;
                bag.add("repo", hit);
            }

            if (!localPath.isEmpty() || !mirrorPath.isEmpty()) {
                const IssueStore issueStore(localPath, mirrorPath, identity,
                                            userName);
                const PullStore pullStore(localPath, mirrorPath, identity,
                                          userName);
                const DiscussionStore discussionStore(
                    localPath, mirrorPath, identity, userName);
                const ProjectStore projectStore(localPath, mirrorPath, identity,
                                                userName);
                const QVector<RecordHit> records = matchRepositoryRecords(
                    query, issueStore.loadAll(), issueStore.loadMilestones(),
                    pullStore.loadAll(), discussionStore.loadAll(),
                    projectStore.loadAll(), kMaxPerCategory);
                for (const RecordHit &record : records) {
                    const QByteArray kind = record.kind.toLatin1();
                    if (!bag.room(kind.constData()))
                        continue;
                    GlobalSearchHit hit;
                    hit.number = record.number;
                    hit.path = record.path;
                    hit.repoIndex = openIndex;
                    hit.primary = record.primary;
                    hit.detail = record.detail;
                    hit.status = record.status;
                    bag.add(kind.constData(), hit);
                }
            }

            for (const AgentSession &session : sessions) {
                if (!bag.room("agent"))
                    break;
                const QString worktree = diffStats.value(session.id).worktree;
                QString where;
                if (containsFold(session.issueTitle, query))
                    where = QStringLiteral("task");
                else if (containsFold(session.prompt, query))
                    where = snippetAround(session.prompt, query);
                else if (containsFold(session.branchName, query))
                    where = QStringLiteral("branch ") + session.branchName;
                else if (containsFold(worktree, query))
                    where = QStringLiteral("worktree ") + worktree;
                else
                    continue;
                const QString task = session.issueTitle.isEmpty()
                                         ? session.prompt.simplified().left(90)
                                         : session.issueTitle;
                GlobalSearchHit hit;
                hit.number = session.id;
                hit.primary = task.isEmpty()
                                  ? QStringLiteral("Session %1").arg(session.id)
                                  : task;
                hit.detail = QStringLiteral("%1/%2 · %3 · %4")
                                 .arg(session.owner, session.name,
                                      session.status, where);
                for (const RepoBrief &repo : repos) {
                    if (repo.owner == session.owner && repo.name == session.name)
                        hit.repoIndex = repo.index;
                }
                bag.add("agent", hit);
            }

            for (const ActionRun &run : runs) {
                if (!bag.room("action"))
                    break;
                QString where;
                if (containsFold(run.workflowName, query))
                    where = QStringLiteral("workflow");
                else if (containsFold(run.workflowPath, query))
                    where = run.workflowPath;
                else if (containsFold(run.workflowContent, query))
                    where = snippetAround(run.workflowContent, query);
                else if (containsFold(run.ref, query))
                    where = run.ref;
                else if (containsFold(run.commit, query))
                    where = run.commit.left(12);
                else
                    continue;
                GlobalSearchHit hit;
                hit.number = run.id;
                hit.primary = QStringLiteral("%1  %2").arg(
                    run.workflowName.isEmpty() ? run.workflowPath
                                               : run.workflowName,
                    QStringLiteral("#%1").arg(run.id));
                hit.detail = QStringLiteral("%1/%2 · %3 · %4")
                                 .arg(run.owner, run.name, run.status, where);
                for (const RepoBrief &repo : repos) {
                    if (repo.owner == run.owner && repo.name == run.name)
                        hit.repoIndex = repo.index;
                }
                bag.add("action", hit);
            }

            // The retained app/network log is newest-last. Search backwards so
            // a broad term returns the most useful recent events first, while
            // still honoring the same per-category cap as every other result.
            for (auto it = networkLog.crbegin(); it != networkLog.crend(); ++it) {
                if (!bag.room("log"))
                    break;
                if (!containsFold(*it, query))
                    continue;
                GlobalSearchHit hit;
                hit.path = *it; // exact stored line; activation scrolls to it
                hit.primary = snippetAround(it->trimmed(), query);
                hit.detail = QStringLiteral("network and runtime log");
                bag.add("log", hit);
            }

            return bag.hits;
        },
        deliver);

    // Branches, worktrees and tags of every repository on this node — not only
    // the open one — in their own wave, since it costs a few git calls per
    // repository.
    runOffThread<QVector<GlobalSearchHit>>(
        [=]() -> QVector<GlobalSearchHit> {
            return scanAllGitCatalogHits(gitRepos, openIndex, ref, query,
                                         kMaxPerCategory);
        },
        deliver);

    runOffThread<QVector<GlobalSearchHit>>(
        [=]() -> QVector<GlobalSearchHit> {
            return scanWorktreeCodeHits(gitDir, ref, query, kMaxCodeHits);
        },
        deliver);

    runOffThread<QVector<GlobalSearchHit>>(
        [=]() -> QVector<GlobalSearchHit> {
            HitBag bag;
            if (gitDir.isEmpty())
                return bag.hits;

            // Every branch, so work that only exists on an agent's worktree
            // branch or a remote-tracking branch is found too.
            for (const QString &row :
                 gitLines(gitDir,
                          QStringList{QStringLiteral("log"), QStringLiteral("-i"),
                                      QStringLiteral("--fixed-strings"),
                                      QStringLiteral("--grep=") + query,
                                      QStringLiteral(
                                          "--format=%H%x1f%h%x1f%ar%x1f%s%x1f%S"),
                                      QStringLiteral("-n"),
                                      QString::number(kMaxCommits)} +
                              allBranchRevisionArgs(ref),
                          kGrepTimeoutMs)) {
                if (!bag.room("commit"))
                    break;
                const QStringList f = row.split(QChar(0x1f));
                if (f.size() < 4)
                    continue;
                GlobalSearchHit hit;
                hit.path = f.at(0); // full hash — what showCommit() wants
                hit.primary = QStringLiteral("%1  %2").arg(f.at(1), f.at(3));
                hit.detail = f.value(4).isEmpty()
                                 ? f.at(2)
                                 : f.at(2) + QStringLiteral(" · ") + f.at(4);
                bag.add("commit", hit);
            }

            QProcess grep;
            grep.start(QStringLiteral("git"),
                       {QStringLiteral("-C"), gitDir, QStringLiteral("grep"),
                        QStringLiteral("-n"), QStringLiteral("-I"),
                        QStringLiteral("-i"), QStringLiteral("-F"),
                        QStringLiteral("-e"), query, ref, QStringLiteral("--"),
                        QStringLiteral(":!.forkmesh/issues"),
                        QStringLiteral(":!pulls")});
            if (grep.waitForFinished(kGrepTimeoutMs)) {
                const QList<QByteArray> lines =
                    grep.readAllStandardOutput().split('\n');
                for (const QByteArray &raw : lines) {
                    if (!bag.room("code", kMaxCodeHits))
                        break;
                    const CodeRow row =
                        parseGrepRow(QString::fromUtf8(raw), ref);
                    if (!row.valid)
                        continue;
                    GlobalSearchHit hit;
                    hit.path = row.path;
                    hit.line = row.line;
                    hit.primary =
                        QStringLiteral("%1:%2").arg(hit.path).arg(hit.line);
                    hit.detail = row.text.left(160);
                    bag.add("code", hit);
                }
            } else {
                grep.kill();
                grep.waitForFinished(1000);
            }
            return bag.hits;
        },
        deliver);
}

void MainWindow::rebuildGlobalSearchList(const QString &query)
{
    if (!m_searchList || !m_searchStatus)
        return;

    const int keepRow = m_searchList->currentRow();
    m_searchList->clear();
    SearchHitDelegate::setQuery(m_searchList, query);

    QStringList summary;
    for (int cat = 0; cat < categoryCount(); ++cat) {
        const Category &category = categoryAt(cat);
        const QColor color(QLatin1String(category.color));
        QVector<const GlobalSearchHit *> rows;
        for (const GlobalSearchHit &hit : m_searchHits)
            if (categoryIndexOf(hit.kind) == cat &&
                !forkmesh::hiddenterms::hides(
                    QStringList{searchScopeLabel(hit.repoIndex >= 0 ? hit.repoIndex
                                                                    : m_repoDetailIndex),
                                hit.primary, hit.path, hit.detail}
                        .join(QLatin1Char('\n'))))
                rows.append(&hit);
        if (rows.isEmpty())
            continue;
        summary.append(QStringLiteral("%1 %2")
                           .arg(rows.size())
                           .arg(QString::fromLatin1(category.title).toLower()));

        auto *header = new QListWidgetItem(
            QStringLiteral("%1 (%2)")
                .arg(QString::fromLatin1(category.title))
                .arg(rows.size()));
        header->setFlags(Qt::NoItemFlags);
        QFont headerFont = header->font();
        headerFont.setBold(true);
        header->setFont(headerFont);
        header->setForeground(Theme::remapColor(QColor("#8b949e")));
        m_searchList->addItem(header);

        for (const GlobalSearchHit *hit : rows) {
            auto *item = new QListWidgetItem(
                themedOcticon(QString::fromLatin1(category.icon), color, 16),
                hit->detail.isEmpty()
                    ? hit->primary
                    : hit->primary + QStringLiteral("\n") + hit->detail);
            const StatusBadge badge = setStatusBadge(item, hit->kind, hit->status);
            if (!badge.icon.isEmpty())
                item->setIcon(themedOcticon(badge.icon, QColor(badge.color), 16));
            item->setData(Qt::UserRole, hit->kind);
            item->setData(Qt::UserRole + 1, hit->number);
            item->setData(Qt::UserRole + 2, hit->path);
            item->setData(Qt::UserRole + 3, hit->line);
            item->setData(Qt::UserRole + 4, hit->repoIndex);
            m_searchList->addItem(item);
        }
    }

    if (keepRow >= 0 && keepRow < m_searchList->count())
        m_searchList->setCurrentRow(keepRow);

    const bool running = m_searchWavesPending > 0;
    if (m_searchHits.isEmpty()) {
        m_searchStatus->setText(
            running ? QStringLiteral("Searching for “%1”…").arg(query)
                    : QStringLiteral("No matches for “%1”.").arg(query));
        return;
    }
    QString text = summary.join(QStringLiteral(" · "));
    if (running)
        text += QStringLiteral("  ·  still searching…");
    m_searchStatus->setText(text);
}

void MainWindow::activateGlobalSearchHit(const GlobalSearchHit &hit)
{
    if (hit.repoIndex >= 0 && hit.repoIndex != m_repoDetailIndex &&
        hit.repoIndex < m_repositories.size())
        openRepoDetail(hit.repoIndex);

    // Records live on the repository page. openRepoDetail() only brings that
    // page up when it had to switch repositories, so a hit in the repository
    // that is already open (the common case from the Search page) must show
    // it here, or the click lands on a tab nobody can see.
    const bool repoOpen =
        m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size();
    const bool repoRecord =
        hit.kind == QLatin1String("repo") || hit.kind == QLatin1String("issue") ||
        hit.kind == QLatin1String("pull") ||
        hit.kind == QLatin1String("discussion") ||
        hit.kind == QLatin1String("project") ||
        hit.kind == QLatin1String("milestone") ||
        hit.kind == QLatin1String("agent");
    if (repoRecord && repoOpen)
        showSection(0);

    // Click the tab rather than just checking it: the click builds a deferred
    // tab and loads its records, which showDiscussion()/showProject() look in.
    auto selectTab = [this](int tab) {
        if (tab < 0)
            return;
        if (m_repoDetailTabs && m_repoDetailTabs->button(tab)) {
            m_repoDetailTabs->button(tab)->click();
            return;
        }
        ensureRepoDetailTabBuilt(tab);
        if (m_repoDetailStack)
            m_repoDetailStack->setCurrentIndex(tab);
    };

    if (hit.kind == QLatin1String("repo")) {
        if (hit.number != m_repoDetailIndex) // the step above may have opened it
            openRepoDetail(hit.number);
        else
            selectTab(kRepoLandingTab);
    } else if (hit.kind == QLatin1String("issue")) {
        openIssueReference(hit.number);
    } else if (hit.kind == QLatin1String("pull")) {
        openPullReference(hit.number);
    } else if (hit.kind == QLatin1String("discussion")) {
        selectTab(5);
        showDiscussion(hit.number);
    } else if (hit.kind == QLatin1String("project")) {
        selectTab(m_projectsTabIndex);
        showProject(hit.number);
    } else if (hit.kind == QLatin1String("milestone")) {
        selectTab(2); // Issues, then its Milestones sub-tab
        if (m_issueTabGroup && m_issueTabGroup->button(1))
            m_issueTabGroup->button(1)->click();
    } else if (hit.kind == QLatin1String("agent")) {
        selectTab(3);
        showAgentSession(hit.number);
    } else if (hit.kind == QLatin1String("action")) {
        openActionRunFromNotification(hit.number);
    } else if (hit.kind == QLatin1String("log")) {
        openFullLogAtFooterLine(hit.path);
    } else if (hit.kind == QLatin1String("branch")) {
        showSection(0);
        switchToBranch(hit.path);
    } else if (hit.kind == QLatin1String("worktree")) {
        showSection(0);
        switchToWorktree(hit.path);
    } else if (hit.kind == QLatin1String("tag") ||
               hit.kind == QLatin1String("commit")) {
        showSection(0);
        openCommitHashReference(hit.path);
    } else if (hit.kind == QLatin1String("file") ||
               hit.kind == QLatin1String("code")) {
        openRepoFileAtLine(hit.path, hit.line);
    } else if (hit.kind == QLatin1String("worktree-code") ||
               hit.kind == QLatin1String("project-file")) {
        revealFileInExplorer(hit.path, hit.line); // the worktree's file on disk
    }
}

// --- Packages the desktop hosts itself --------------------------------------
//
// A marketplace package with its own page answers the top search box through
// NativePackageWidget::searchHits. Notes, Tasks, Calendar and Timers are
// packages too, but their pages are part of the window, so the window answers
// for them here from the records those pages already hold. Packages that only
// add a launcher (Ledger, Capture, ...) are found by their name instead.

namespace {

QString packageRecordText(const QJsonObject &record, const char *field)
{
    return record.value(QLatin1String(field)).toString().trimmed();
}

// The first of `fields` holding the query, or -1.
int packageRecordMatch(const QJsonObject &record, const QString &query,
                       std::initializer_list<const char *> fields)
{
    int index = 0;
    for (const char *field : fields) {
        if (containsFold(record.value(QLatin1String(field)).toString(), query))
            return index;
        ++index;
    }
    return -1;
}

} // namespace

bool MainWindow::builtinPackageSearchable(const QString &packageId) const
{
    return packageSurfaceShown(packageId);
}

QList<MainWindow::BuiltinPackageSearchHit>
MainWindow::builtinPackageSearchHits(const QString &rawQuery,
                                     int limitPerPackage) const
{
    QList<BuiltinPackageSearchHit> hits;
    const QString query = rawQuery.trimmed();
    if (query.isEmpty())
        return hits;
    const QString dot = QString::fromUtf8("  \xC2\xB7  ");

    if (builtinPackageSearchable(QStringLiteral("notes"))) {
        int found = 0;
        QSet<QString> mirrored; // cloud copies of a local note list once
        for (const QJsonValue &value : m_localNotes)
            mirrored.insert(packageRecordText(value.toObject(), "cloudId"));
        for (const bool local : {true, false}) {
            for (const QJsonValue &value : local ? m_localNotes : m_cloudNotes) {
                if (found >= limitPerPackage)
                    break;
                const QJsonObject note = value.toObject();
                const QString id = packageRecordText(note, "id");
                if (id.isEmpty() || (!local && mirrored.contains(id)))
                    continue;
                const int field =
                    packageRecordMatch(note, query, {"title", "markdown"});
                if (field < 0)
                    continue;
                QString label = packageRecordText(note, "title");
                if (label.isEmpty())
                    label = QStringLiteral("Untitled note");
                if (field == 1)
                    label += dot + snippetAround(
                        note.value(QStringLiteral("markdown")).toString(), query);
                hits.append({QStringLiteral("notes"), QStringLiteral("Notes"),
                             QStringLiteral("note"), label,
                             (local ? QStringLiteral("local:")
                                    : QStringLiteral("cloud:")) + id});
                ++found;
            }
        }
    }

    if (builtinPackageSearchable(QStringLiteral("tasks"))) {
        int found = 0;
        for (const QJsonValue &value : m_organizationTasks) {
            if (found >= limitPerPackage)
                break;
            const QJsonObject task = value.toObject();
            const QString id = packageRecordText(task, "id");
            if (id.isEmpty())
                continue;
            int field = packageRecordMatch(
                task, query,
                {"title", "details", "assignee", "repository", "completionNote",
                 "department", "status", "id"});
            if (field < 0 &&
                containsFold(task.value(QStringLiteral("qa"))
                                 .toObject()
                                 .value(QStringLiteral("howToTest"))
                                 .toString(),
                             query))
                field = 8;
            if (field < 0)
                continue;
            QString label = packageRecordText(task, "title");
            if (label.isEmpty())
                label = id;
            if (field == 1 || field == 4)
                label += dot + snippetAround(
                    task.value(QLatin1String(field == 1 ? "details"
                                                        : "completionNote"))
                        .toString(), query);
            const QString status = packageRecordText(task, "status");
            if (!status.isEmpty())
                label += dot + status;
            hits.append({QStringLiteral("tasks"), QStringLiteral("Tasks"),
                         QStringLiteral("list-unordered"), label, id});
            ++found;
        }
    }

    if (builtinPackageSearchable(QStringLiteral("calendar"))) {
        int found = 0;
        for (const QJsonValue &value : m_calendarEvents) {
            if (found >= limitPerPackage)
                break;
            const QJsonObject event = value.toObject();
            const QString id = packageRecordText(event, "id");
            if (id.isEmpty() ||
                packageRecordMatch(event, query,
                                   {"title", "location", "description"}) < 0)
                continue;
            QString label = packageRecordText(event, "title");
            if (label.isEmpty())
                label = QStringLiteral("Untitled event");
            const qint64 startAt = qint64(
                event.value(QStringLiteral("startAt")).toDouble());
            if (startAt > 0)
                label += dot + QLocale().toString(
                    QDateTime::fromMSecsSinceEpoch(startAt).toLocalTime(),
                    QLocale::ShortFormat);
            const QString location = packageRecordText(event, "location");
            if (!location.isEmpty())
                label += dot + location;
            hits.append({QStringLiteral("calendar"), QStringLiteral("Calendar"),
                         QStringLiteral("calendar"), label, id});
            ++found;
        }
    }

    if (m_timersFeature) {
        const qint64 now = timersNowMs();
        int found = 0;
        for (const forkmesh::timers::Timer &timer : m_timersFeature->ordered(now)) {
            if (found >= limitPerPackage)
                break;
            const QString kind =
                forkmesh::timers::TimersFeature::kindName(timer.kind);
            if (!containsFold(timer.label, query) && !containsFold(kind, query))
                continue;
            hits.append({QStringLiteral("timers"), QStringLiteral("Timers"),
                         QStringLiteral("stopwatch"),
                         (timer.label.isEmpty() ? kind : timer.label) + dot +
                             forkmesh::timers::TimersFeature::spokenStatus(timer,
                                                                          now),
                         timer.id});
            ++found;
        }
    }

    // Every other installed package is at least findable by what it is called
    // and what it says it does; choosing it opens the package.
    if (m_packageRegistry && m_packageRegistry->isReady()) {
        int found = 0;
        for (const forkmesh::market::PackageManifest &manifest :
             m_packageRegistry->packages()) {
            if (found >= limitPerPackage)
                break;
            if (!m_packageRegistry->isInstalled(manifest.id) ||
                !m_packageRegistry->isPlacedHere(manifest.id) ||
                (!containsFold(manifest.name, query) &&
                 !containsFold(manifest.summary, query)))
                continue;
            QString label = manifest.name;
            if (!manifest.summary.trimmed().isEmpty())
                label += dot + manifest.summary.trimmed();
            hits.append({manifest.id, QStringLiteral("Installed packages"),
                         QStringLiteral("package"), label, QString()});
            ++found;
        }
    }
    return hits;
}

void MainWindow::activateBuiltinPackageSearchHit(const QString &packageId,
                                                 const QString &hitId)
{
    if (!hitId.isEmpty() && packageId == QLatin1String("notes")) {
        const int colon = hitId.indexOf(QLatin1Char(':'));
        openNoteFromSearch(hitId.mid(colon + 1), hitId.left(colon));
        return;
    }
    if (!hitId.isEmpty() && packageId == QLatin1String("tasks")) {
        openOrganizationTask(hitId);
        return;
    }
    if (!hitId.isEmpty() && packageId == QLatin1String("calendar")) {
        showSection(kCalendarSectionIndex);
        openCalendarEventDialog(hitId, QDate());
        return;
    }
    if (!hitId.isEmpty() && packageId == QLatin1String("timers")) {
        // The chip's own menu is where a timer is paused, restarted or
        // dropped, so the hit opens it under the chip.
        const QList<QPushButton *> chips =
            m_timersNavStrip ? m_timersNavStrip->findChildren<QPushButton *>()
                             : QList<QPushButton *>();
        for (QPushButton *chip : chips) {
            if (chip->property("timerId").toString() == hitId) {
                showTimerChipMenu(
                    hitId, chip->mapToGlobal(QPoint(0, chip->height())));
                return;
            }
        }
        return;
    }

    // A package found by name: its page if it has one, else where it lives.
    for (const NativePackagePage &page : std::as_const(m_nativePackagePages)) {
        if (page.id == packageId && page.widget)
            return showNativePackage(packageId);
        if (page.id == packageId && page.navigation && !page.widget)
            return openNativePackageLink(packageId);
    }
    const forkmesh::market::PackageManifest *manifest =
        m_packageRegistry ? m_packageRegistry->package(packageId) : nullptr;
    const QString entrypoint = manifest ? manifest->desktopEntrypoint : QString();
    if (entrypoint == QLatin1String("builtin:notes"))
        showSection(kNotesSectionIndex);
    else if (entrypoint == QLatin1String("builtin:tasks"))
        showSection(kOrganizationTasksSectionIndex);
    else if (entrypoint == QLatin1String("builtin:calendar"))
        showSection(kCalendarSectionIndex);
    else if (entrypoint == QLatin1String("builtin:ledger") && m_ledgerFeature)
        showSection(kLedgerSectionIndex);
    else if (entrypoint == QLatin1String("builtin:timers") && m_timersClockButton)
        showTimersClockMenu();
    else
        showPackageSettings(packageId);
}

void MainWindow::prepareBuiltinPackageSearch()
{
    if (m_builtinPackageSearchPrepared)
        return;
    m_builtinPackageSearchPrepared = true;
    // Off the keystroke: a page that was never opened is built here, and its
    // load callback redraws the dropdown through nativePackageSearchIndexChanged.
    QTimer::singleShot(0, this, [this] {
        if (builtinPackageSearchable(QStringLiteral("notes")) &&
            m_localNotes.isEmpty() && m_cloudNotes.isEmpty()) {
            ensureSectionBuilt(kNotesSectionIndex);
            refreshNotes();
        }
        if (builtinPackageSearchable(QStringLiteral("tasks")) &&
            !m_organizationTasksLoaded) {
            ensureSectionBuilt(kOrganizationTasksSectionIndex);
            refreshOrganizationTasks();
        }
        if (builtinPackageSearchable(QStringLiteral("calendar")) &&
            m_calendarEvents.isEmpty()) {
            ensureSectionBuilt(kCalendarSectionIndex);
            refreshCalendar();
        }
    });
}
