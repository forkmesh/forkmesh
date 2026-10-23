
#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "SecretDiffBanner.h"
#include "UiSounds.h"
#include "AccountCapability.h"
#include "AgentActivityFeed.h"
#include "ForkMeshPackageRegistry.h"
#include "RepoStatsStore.h"
#include "AgentCliLocator.h"
#include "AgentJail.h"
#include "AgentCapabilityBroker.h"
#include "AgentCapabilityStore.h"
#include "AgentPermissionController.h"
#include "AgentPromptImages.h"
#include "AgentToolCatalog.h"
#include "AgentToolEfficiencyBoard.h"
#include "AgentGuardrailBoard.h"
#include "AgentToolPolicy.h"
#include "AgentRuntimeRegistry.h"
#include "AgentResumeIdentity.h"
#include "AgentWorktree.h"
#include "UpstreamCheckoutSync.h"
#include "FlowLayout.h"
#include "KebabHeaderView.h"
#include "CodexAppServerSession.h"
#include "NodeEventSocket.h"
#include "UsageLimitCalendar.h"

#include <QActionGroup>
#include <QComboBox>
#include <QDebug>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTabBar>
#include <QTimer>
#include <QTextLayout>
#include <QTextOption>
#include <QUrl>
#include <QEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QApplication>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QButtonGroup>
#include <QFrame>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QShowEvent>
#include <QSignalBlocker>

#include <algorithm>
#include <limits>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#else
#include <csignal>
#endif
#include "MainWindowAgentsInternal.h"
#include "MainWindowNextAccess.h"

using namespace forkmesh::ui;

namespace {

// A reference that names a directory and a file with an extension —
// "docs/operations/audit.md" — is a path, whatever the link it arrived on says.
// Branch prefixes and directory names overlap (docs/, test/, build/), so a
// reference can only be told apart by what it resolves to, and the two click
// handlers below hand ambiguous ones to each other rather than reporting that
// something plainly present is missing.
static bool transcriptReferenceLooksLikePath(const QString &ref)
{
    const QString last = ref.section(QLatin1Char('/'), -1);
    const int dot = last.lastIndexOf(QLatin1Char('.'));
    if (dot <= 0 || dot + 1 >= last.size())
        return false;
    const QString extension = last.mid(dot + 1);
    return extension.size() <= 12 && extension.at(0).isLetter() &&
           std::all_of(extension.cbegin(), extension.cend(),
                       [](QChar c) { return c.isLetterOrNumber(); });
}

// The age label shares this column with the branch/worktree metadata. Keep
// enough room for both the lead glyph and the full right-aligned metadata
// group; a narrower fixed width lets the worktree glyph paint over the age.
static constexpr int kAgentIdColumnWidth = 170;

// The Agents list owns its ordering rather than letting QTableWidget move cell
// objects behind our back. A drop reports the two durable session ids and the
// window rewrites their 1..99 priorities before repainting from AgentStore.
class AgentPriorityTable final : public QTableWidget
{
public:
    using QTableWidget::QTableWidget;

    std::function<void(const QList<int> &, int, const QString &, bool)>
        onSessionsDrop;
    std::function<void(const QString &, const QString &, bool)> onFolderDrop;
    std::function<void(const QString &)> onFolderToggle;

    QString folderAt(int row) const
    {
        const QTableWidgetItem *lead = item(row, kAgentIdColumn);
        return lead ? lead->data(kAgentFolderRole).toString() : QString();
    }

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        const QString folderId = folderAt(indexAt(event->pos()).row());
        if (folderId.isEmpty()) {
            m_pressedFolderId.clear();
            QTableWidget::mousePressEvent(event);
            return;
        }
        m_pressedFolderId =
            event->button() == Qt::LeftButton ? folderId : QString();
        m_pressPos = event->pos();
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (m_pressedFolderId.isEmpty()) {
            QTableWidget::mouseMoveEvent(event);
            return;
        }
        if (!(event->buttons() & Qt::LeftButton) ||
            (event->pos() - m_pressPos).manhattanLength() <
                QApplication::startDragDistance())
            return;
        const QString folderId = m_pressedFolderId;
        m_pressedFolderId.clear();
        startFolderDrag(folderId);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (m_pressedFolderId.isEmpty()) {
            QTableWidget::mouseReleaseEvent(event);
            return;
        }
        const QString folderId = m_pressedFolderId;
        m_pressedFolderId.clear();
        if (folderAt(indexAt(event->pos()).row()) == folderId && onFolderToggle)
            onFolderToggle(folderId);
        event->accept();
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        if (folderAt(indexAt(event->pos()).row()).isEmpty()) {
            QTableWidget::mouseDoubleClickEvent(event);
            return;
        }
        m_pressedFolderId.clear();
        event->accept();
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        const QString folderId = folderAt(currentRow());
        const bool collapsed =
            item(currentRow(), kAgentIdColumn)
                ? item(currentRow(), kAgentIdColumn)
                      ->data(kAgentFolderCollapsedRole)
                      .toBool()
                : false;
        if (!folderId.isEmpty() && onFolderToggle &&
            (event->key() == Qt::Key_Space || event->key() == Qt::Key_Return ||
             event->key() == Qt::Key_Enter ||
             (event->key() == Qt::Key_Left && !collapsed) ||
             (event->key() == Qt::Key_Right && collapsed))) {
            onFolderToggle(folderId);
            event->accept();
            return;
        }
        QTableWidget::keyPressEvent(event);
    }

    void startDrag(Qt::DropActions supportedActions) override
    {
        m_draggedSessionIds.clear();
        int dragged = 0;
        if (const QTableWidgetItem *item =
                this->item(currentRow(), kAgentIdColumn))
            dragged = item->data(kAgentFolderRole).isValid()
                          ? 0
                          : item->data(Qt::UserRole).toInt();
        if (dragged == 0)
            return;
        for (const QModelIndex &row : selectionModel()->selectedRows()) {
            const QTableWidgetItem *lead = item(row.row(), kAgentIdColumn);
            const int id = lead ? lead->data(Qt::UserRole).toInt() : 0;
            if (id != 0 && !lead->data(kAgentFolderRole).isValid())
                m_draggedSessionIds.append(id);
        }
        if (!m_draggedSessionIds.contains(dragged))
            m_draggedSessionIds = {dragged};
        m_draggedSessionIds.removeAll(dragged);
        m_draggedSessionIds.prepend(dragged);
        QTableWidget::startDrag(supportedActions);
        m_draggedSessionIds.clear();
        clearDropIndicator();
    }

    void dragEnterEvent(QDragEnterEvent *event) override
    {
        if (!dragging(event)) {
            event->ignore();
            return;
        }
        event->setDropAction(Qt::MoveAction);
        event->accept();
    }

    void dragMoveEvent(QDragMoveEvent *event) override
    {
        if (!dragging(event)) {
            event->ignore();
            return;
        }
        QTableWidget::dragMoveEvent(event);
        const QRect indicator = resolveDrop(event->position().toPoint()).indicator;
        if (indicator != m_dropIndicator) {
            m_dropIndicator = indicator;
            viewport()->update();
        }
        event->setDropAction(Qt::MoveAction);
        event->accept();
    }

    void dragLeaveEvent(QDragLeaveEvent *event) override
    {
        QTableWidget::dragLeaveEvent(event);
        clearDropIndicator();
    }

    void dropEvent(QDropEvent *event) override
    {
        if (!dragging(event)) {
            event->ignore();
            return;
        }
        const Drop drop = resolveDrop(event->position().toPoint());
        clearDropIndicator();
        if (!m_draggedFolderId.isEmpty()) {
            if (onFolderDrop && drop.folderId != m_draggedFolderId)
                onFolderDrop(m_draggedFolderId, drop.folderId, drop.after);
        } else if (onSessionsDrop &&
                   !(drop.sessionId != 0 &&
                     m_draggedSessionIds.contains(drop.sessionId) &&
                     m_draggedSessionIds.size() == 1)) {
            onSessionsDrop(m_draggedSessionIds, drop.sessionId, drop.folderId,
                           drop.after);
        }
        event->setDropAction(Qt::MoveAction);
        event->accept();
    }

    void paintEvent(QPaintEvent *event) override
    {
        QTableWidget::paintEvent(event);
        if (m_dropIndicator.isNull())
            return;
        QPainter painter(viewport());
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QColor accent(currentThemeIsDark() ? "#58a6ff" : "#0969da");
        if (m_dropIndicator.height() > 2) {
            QColor wash = accent;
            wash.setAlpha(40);
            painter.setBrush(wash);
            painter.setPen(QPen(accent, 1.5));
            painter.drawRoundedRect(
                QRectF(m_dropIndicator).adjusted(1.5, 1.5, -1.5, -1.5), 5, 5);
        } else {
            painter.setPen(Qt::NoPen);
            painter.setBrush(accent);
            painter.drawRoundedRect(QRectF(m_dropIndicator).adjusted(4, -1, -4, 1),
                                    1.5, 1.5);
        }
    }

private:
    struct Drop {
        int sessionId = 0;
        QString folderId;
        bool after = false;
        QRect indicator;
    };

    bool dragging(const QDropEvent *event) const
    {
        return event->source() == this &&
               (!m_draggedSessionIds.isEmpty() || !m_draggedFolderId.isEmpty());
    }

    QRect rowRect(int row) const
    {
        const QRect rect = visualRect(model()->index(row, kAgentIssueColumn));
        return QRect(0, rect.top(), viewport()->width(), rect.height());
    }

    QRect lineAt(int y) const { return QRect(0, y - 1, viewport()->width(), 2); }

    int folderBlockEnd(int header) const
    {
        const QString folderId = folderAt(header);
        int last = header;
        for (int row = header + 1; row < rowCount(); ++row) {
            const QTableWidgetItem *lead = item(row, kAgentIdColumn);
            if (!lead || lead->data(kAgentFolderRole).isValid() ||
                lead->data(kAgentRowFolderRole).toString() != folderId)
                break;
            last = row;
        }
        return last;
    }

    int lastFolderBlockEnd() const
    {
        int end = -1;
        for (int row = 0; row < rowCount(); ++row)
            if (!folderAt(row).isEmpty())
                end = folderBlockEnd(row);
        return end;
    }

    Drop resolveDrop(const QPoint &pos) const
    {
        Drop drop;
        const int row = indexAt(pos).row();
        const QTableWidgetItem *lead = row >= 0 ? item(row, kAgentIdColumn) : nullptr;
        const QString headerFolder = folderAt(row);
        const QString rowFolder =
            lead ? lead->data(kAgentRowFolderRole).toString() : QString();
        if (!m_draggedFolderId.isEmpty()) {
            if (!headerFolder.isEmpty()) {
                drop.folderId = headerFolder;
                drop.after = pos.y() > rowRect(row).center().y();
                drop.indicator =
                    drop.after ? lineAt(rowRect(folderBlockEnd(row)).bottom() + 1)
                               : lineAt(rowRect(row).top());
                return drop;
            }
            if (!rowFolder.isEmpty()) {
                for (int header = row; header >= 0; --header)
                    if (folderAt(header) == rowFolder) {
                        drop.folderId = rowFolder;
                        drop.after = true;
                        drop.indicator =
                            lineAt(rowRect(folderBlockEnd(header)).bottom() + 1);
                        return drop;
                    }
            }
            const int end = lastFolderBlockEnd();
            drop.after = true;
            if (end >= 0)
                drop.indicator = lineAt(rowRect(end).bottom() + 1);
            return drop;
        }
        if (!headerFolder.isEmpty()) {
            drop.folderId = headerFolder;
            drop.indicator = rowRect(row);
            return drop;
        }
        if (row < 0) {
            int last = rowCount() - 1;
            while (last >= 0 && !folderAt(last).isEmpty())
                --last;
            const QTableWidgetItem *tail =
                last >= 0 ? item(last, kAgentIdColumn) : nullptr;
            if (tail && tail->data(kAgentRowFolderRole).toString().isEmpty())
                drop.sessionId = tail->data(Qt::UserRole).toInt();
            drop.after = true;
            drop.indicator =
                rowCount() > 0 ? lineAt(rowRect(rowCount() - 1).bottom() + 1)
                               : QRect();
            return drop;
        }
        drop.sessionId = lead ? lead->data(Qt::UserRole).toInt() : 0;
        drop.folderId = rowFolder;
        const QRect rect = rowRect(row);
        drop.after = pos.y() > rect.center().y();
        drop.indicator = lineAt(drop.after ? rect.bottom() + 1 : rect.top());
        return drop;
    }

    void startFolderDrag(const QString &folderId)
    {
        int header = -1;
        for (int row = 0; row < rowCount() && header < 0; ++row)
            if (folderAt(row) == folderId)
                header = row;
        if (header < 0)
            return;
        auto *mime = new QMimeData;
        mime->setData(kAgentFolderMimeType, folderId.toUtf8());
        auto *drag = new QDrag(this);
        drag->setMimeData(mime);
        const QRect rect = rowRect(header);
        const QPixmap preview = viewport()->grab(rect);
        if (!preview.isNull()) {
            drag->setPixmap(preview);
            drag->setHotSpot(m_pressPos - rect.topLeft());
        }
        m_draggedFolderId = folderId;
        drag->exec(Qt::MoveAction, Qt::MoveAction);
        m_draggedFolderId.clear();
        clearDropIndicator();
    }

    void clearDropIndicator()
    {
        if (m_dropIndicator.isNull())
            return;
        m_dropIndicator = QRect();
        viewport()->update();
    }

    QList<int> m_draggedSessionIds;
    QString m_draggedFolderId;
    QString m_pressedFolderId;
    QPoint m_pressPos;
    QRect m_dropIndicator;
};

void paintAgentFolderRow(QPainter *painter, const QStyleOptionViewItem &option,
                         const QModelIndex &index)
{
    const bool dark = currentThemeIsDark();
    const bool hot = option.state & QStyle::State_MouseOver;
    const bool collapsed = index.data(kAgentFolderCollapsedRole).toBool();
    const QRect r = option.rect;
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->fillRect(r, QColor(dark ? (hot ? "#1f262f" : "#161b22")
                                     : (hot ? "#eef1f4" : "#f6f8fa")));
    painter->setPen(QColor(dark ? "#30363d" : "#d8dee4"));
    painter->drawLine(r.bottomLeft(), r.bottomRight());
    const QColor ink(dark ? "#c9d1d9" : "#1f2328");
    const QColor muted(dark ? "#8b949e" : "#656d76");
    constexpr int kGlyph = 14;
    int x = r.left() + 8;
    const int glyphTop = r.center().y() - kGlyph / 2 + 1;
    themedOcticon(collapsed ? QStringLiteral("chevron-right")
                            : QStringLiteral("chevron-down"),
                  muted, kGlyph)
        .paint(painter, QRect(x, glyphTop, kGlyph, kGlyph));
    x += kGlyph + 6;
    themedOcticon(QStringLiteral("file-directory"),
                  QColor(dark ? "#d29922" : "#9a6700"), kGlyph)
        .paint(painter, QRect(x, glyphTop, kGlyph, kGlyph));
    x += kGlyph + 8;
    QFont nameFont = option.font;
    nameFont.setWeight(QFont::DemiBold);
    QFont summaryFont = option.font;
    if (summaryFont.pixelSize() > 0)
        summaryFont.setPixelSize(qMax(9, summaryFont.pixelSize() - 1));
    else
        summaryFont.setPointSizeF(qMax(7.0, summaryFont.pointSizeF() - 1.0));
    const QString summary = index.data(kAgentFolderSummaryRole).toString();
    const int summaryWidth =
        QFontMetrics(summaryFont).horizontalAdvance(summary);
    const QRect summaryRect(r.right() - 10 - summaryWidth, r.top(), summaryWidth,
                            r.height());
    const QRect nameRect(x, r.top(), qMax(0, summaryRect.left() - 12 - x),
                         r.height());
    painter->setFont(nameFont);
    painter->setPen(ink);
    painter->drawText(nameRect, Qt::AlignVCenter | Qt::AlignLeft,
                      QFontMetrics(nameFont).elidedText(
                          index.data(Qt::DisplayRole).toString(),
                          Qt::ElideRight, nameRect.width()));
    painter->setFont(summaryFont);
    painter->setPen(muted);
    painter->drawText(summaryRect, Qt::AlignVCenter | Qt::AlignRight, summary);
    painter->restore();
}

class AgentBranchButtonDelegate : public SelectionBorderRowDelegate
{
public:
    AgentBranchButtonDelegate(QAbstractItemView *view,
                              std::function<void(int)> onClick,
                              std::function<void(int)> onConflictClick)
        : SelectionBorderRowDelegate(view), m_onClick(std::move(onClick)),
          m_onConflictClick(std::move(onConflictClick))
    {
    }

    QSize sizeHint(const QStyleOptionViewItem &opt, const QModelIndex &idx) const override
    {
        QSize s = SelectionBorderRowDelegate::sizeHint(opt, idx);
        s.rwidth() += chipWidth(opt) + countWidth(opt) + 2 * kGlyphSize +
                      4 * kButtonMargin + 3 * kChipGap + kBarWidth;
        s.rheight() = qMax(s.height(), kBarHeight + 6);
        return s;
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        if (index.data(kAgentFolderRole).isValid()) {
            paintAgentFolderRow(painter, option, index);
            return;
        }
        SelectionBorderRowDelegate::paint(painter, option, index);
        paintChurnBars(painter, option, index);
        if (index.data(kAgentBranchRole).toString().isEmpty())
            return;
        const QRect r = buttonRect(option, index);
        if (r.width() <= 0)
            return;
        const bool hot = option.state & QStyle::State_MouseOver;
        const bool live = !index.data(kAgentBranchWorktreeRole).toString().isEmpty();
        const bool conflicted = index.data(kAgentConflictRole).toBool();
        const bool behind = index.data(kAgentBehindRole).toInt() > 0;
        const bool dark = currentThemeIsDark();
        const QColor accent = Theme::nextOr(Theme::Role::Warning,
                                            QColor(dark ? "#e3742f" : "#bc4c00"));
        const Theme::Role fillRole =
            hot ? Theme::Role::SurfaceActive : Theme::Role::SurfaceRaised;
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        QLinearGradient fill(r.topLeft(), r.bottomLeft());
        if (conflicted || behind) {
            QColor wash = accent;
            wash.setAlpha(hot ? (dark ? 70 : 40) : (dark ? 34 : 20));
            fill.setColorAt(0.0, wash);
            fill.setColorAt(1.0, wash);
        } else if (dark) {
            fill.setColorAt(0.0, Theme::nextOr(fillRole,
                                               QColor(hot ? "#3b424c" : "#2b313a")));
            fill.setColorAt(1.0, Theme::nextOr(fillRole,
                                               QColor(hot ? "#2c323b" : "#1f242b")));
        } else {
            fill.setColorAt(0.0, Theme::nextOr(fillRole, QColor("#ffffff")));
            fill.setColorAt(1.0, Theme::nextOr(fillRole,
                                               QColor(hot ? "#e8ebef" : "#f0f2f5")));
        }
        painter->setBrush(fill);
        const QColor border =
            (conflicted || behind)
                ? accent
                : (live ? Theme::nextOr(Theme::Role::AccentFg,
                                        QColor(dark ? (hot ? "#58a6ff" : "#3d6ea8")
                                                    : "#0969da"))
                        : Theme::nextOr(hot ? Theme::Role::BorderStrong
                                            : Theme::Role::Border,
                                        QColor(dark ? (hot ? "#484f58" : "#30363d")
                                                    : (hot ? "#afb8c1" : "#d0d7de"))));
        painter->setPen(QPen(border, 1));
        painter->drawRoundedRect(QRectF(r).adjusted(0.5, 0.5, -0.5, -0.5), 5, 5);
        const QColor ink = Theme::nextOr(
            hot ? Theme::Role::Fg : Theme::Role::FgMuted,
            QColor(dark ? (hot ? "#c9d1d9" : "#8b949e")
                        : (hot ? "#1f2328" : "#656d76")));
        QRect glyph(r.center().x() - kGlyphSize / 2,
                    r.center().y() - kGlyphSize / 2,
                    kGlyphSize, kGlyphSize);
        themedOcticon("git-branch", ink, kGlyphSize).paint(painter, glyph);
        painter->restore();

        if (live) {
            const QColor liveColor = Theme::nextOr(
                Theme::Role::AccentFg, QColor(dark ? "#58a6ff" : "#0969da"));
            themedOcticon("worktree", liveColor, kGlyphSize)
                .paint(painter, worktreeRect(option, index));
        }

        if (conflicted)
            themedOcticon("alert", accent, kGlyphSize)
                .paint(painter, conflictRect(option, index));
        else if (behind)
            themedOcticon("download", accent, kGlyphSize)
                .paint(painter, conflictRect(option, index));
        const QString files = filesText(index);
        if (!files.isEmpty()) {
            const QRect count = countRect(option, index);
            painter->save();
            painter->setPen(ink);
            painter->setFont(chipFont(option));
            painter->drawText(count, Qt::AlignCenter, files);
            painter->restore();
        }
        if (branchDirty(index) > 0) {
            painter->setPen(QPen(Theme::remapColor(QColor(dark ? "#0d1117" : "#ffffff")), 1.5));
            painter->setBrush(QColor(dark ? "#d29922" : "#bf8700"));
            painter->drawEllipse(QPointF(r.right() - 0.5, r.top() + 1.5), 3.0, 3.0);
        }
    }

    bool editorEvent(QEvent *event, QAbstractItemModel *model,
                     const QStyleOptionViewItem &option,
                     const QModelIndex &index) override
    {
        if (index.data(kAgentFolderRole).isValid())
            return false;
        if (event->type() == QEvent::MouseButtonRelease) {
            auto *me = static_cast<QMouseEvent *>(event);
            const QString branch = index.data(kAgentBranchRole).toString();
            const int sessionId = index.data(kAgentConflictSessionRole).toInt();
            if (me->button() == Qt::LeftButton && !branch.isEmpty()) {
                if (m_onConflictClick && index.data(kAgentConflictRole).toBool() &&
                    sessionId > 0 &&
                    conflictRect(option, index).contains(me->pos())) {
                    m_onConflictClick(sessionId);
                    return true;
                }
                if (m_onClick && buttonRect(option, index).contains(me->pos())) {
                    m_onClick(index.data(Qt::UserRole).toInt());
                    return true;
                }
            }
        }
        return SelectionBorderRowDelegate::editorEvent(event, model, option, index);
    }

private:
    static constexpr int kButtonSize = 18;   // chip height
    // Keep the branch metadata compact enough for the age text in the same
    // fixed column. This also pulls the file count toward the churn bars.
    static constexpr int kButtonMargin = 2;
    static constexpr int kGlyphSize = 12;
    static constexpr int kChipPadding = 4;   // chip edge -> glyph / count text
    static constexpr int kChipGap = 3;       // glyph -> count text
    static constexpr int kBarThickness = 3;
    static constexpr int kBarGap = 2;
    static constexpr int kBarHeight = 12;  // a bar at full scale
    static constexpr int kBarWidth = 2 * kBarThickness + kBarGap;
    static constexpr double kBarFullScaleLines = 800.0;

    static QFont chipFont(const QStyleOptionViewItem &opt)
    {
        QFont f = opt.font;
        if (f.pixelSize() > 0)
            f.setPixelSize(qMax(9, f.pixelSize() - 3));
        else
            f.setPointSizeF(qMax(7.0, f.pointSizeF() - 2.0));
        f.setWeight(QFont::DemiBold);
        return f;
    }

    static QString filesText(const QModelIndex &idx)
    {
        const int files = idx.data(kAgentBranchFilesRole).toInt();
        if (files < 0)
            return QString();
        return files > 999 ? QStringLiteral("999+") : QString::number(files);
    }

    static int branchDirty(const QModelIndex &idx)
    {
        const QVariant v = idx.data(kAgentBranchDirtyRole);
        return v.isValid() ? v.toInt() : -1;
    }

    static int chipWidth(const QStyleOptionViewItem &)
    {
        return kButtonSize;
    }

    static QRect buttonRect(const QStyleOptionViewItem &opt, const QModelIndex &)
    {
        const QRect cell = opt.rect;
        const int h = qMin(kButtonSize, cell.height() - 2);
        const int metadata = kGlyphSize + kChipGap + countWidth(opt) + kChipGap;
        const int right = cell.right() - kBarWidth - kButtonMargin - metadata;
        const int w = qMin(chipWidth(opt), qMax(0, cell.width() - 2 * kButtonMargin));
        return QRect(right - kButtonMargin - w + 1, cell.center().y() - h / 2 + 1, w,
                     h);
    }

    static int countWidth(const QStyleOptionViewItem &opt)
    {
        return QFontMetrics(chipFont(opt)).horizontalAdvance(QStringLiteral("999+"));
    }

    static QRect worktreeRect(const QStyleOptionViewItem &opt, const QModelIndex &idx)
    {
        const QRect chip = buttonRect(opt, idx);
        return QRect(chip.left() - kChipGap - kGlyphSize,
                     chip.center().y() - kGlyphSize / 2, kGlyphSize, kGlyphSize);
    }

    static QRect conflictRect(const QStyleOptionViewItem &opt, const QModelIndex &idx)
    {
        const QRect chip = buttonRect(opt, idx);
        return QRect(chip.right() + kChipGap + 1,
                     chip.center().y() - kGlyphSize / 2, kGlyphSize, kGlyphSize);
    }

    static QRect countRect(const QStyleOptionViewItem &opt, const QModelIndex &idx)
    {
        const QRect chip = buttonRect(opt, idx);
        return QRect(conflictRect(opt, idx).right() + kChipGap + 1, chip.top(),
                     countWidth(opt), chip.height());
    }

    static int barHeight(int lines)
    {
        if (lines <= 0)
            return 0;
        const double scale = std::log1p(lines) / std::log1p(kBarFullScaleLines);
        return qBound(2, qRound(qMin(1.0, scale) * kBarHeight), kBarHeight);
    }

    void paintChurnBars(QPainter *painter, const QStyleOptionViewItem &option,
                        const QModelIndex &index) const
    {
        const int added = index.data(kAgentAddedRole).toInt();
        const int removed = index.data(kAgentRemovedRole).toInt();
        if (added <= 0 && removed <= 0)
            return;
        const bool dark = currentThemeIsDark();
        const int baseline = option.rect.center().y() + kBarHeight / 2;
        const int left = option.rect.right() - kBarWidth + 1;
        painter->save();
        painter->setPen(Qt::NoPen);
        const QColor green(dark ? "#3fb950" : "#1a7f37");
        const QColor red(dark ? "#f85149" : "#cf222e");
        const int addH = barHeight(added);
        if (addH > 0) {
            painter->setBrush(green);
            painter->drawRect(left, baseline - addH, kBarThickness, addH);
        }
        const int delH = barHeight(removed);
        if (delH > 0) {
            painter->setBrush(red);
            painter->drawRect(left + kBarThickness + kBarGap, baseline - delH,
                              kBarThickness, delH);
        }
        painter->restore();
    }

    std::function<void(int)> m_onClick;
    std::function<void(int)> m_onConflictClick;
};

void fitRailActionButton(ActivityRailButton *button, const QString &caption)
{
    QFont f = QGuiApplication::font();
    f.setPixelSize(10);
    f.setWeight(QFont::DemiBold);
    button->setFixedSize(
        qMax(railItemWidth(), QFontMetrics(f).horizontalAdvance(caption) + 12),
        kRailItemHeight);
}

ActivityRailButton *railActionButton(const QString &icon, const QString &caption,
                                     const QString &tooltip)
{
    auto *button = new ActivityRailButton(icon, caption);
    button->setCheckable(false);
    fitRailActionButton(button, caption);
    button->setToolTip(tooltip);
    return button;
}

} // namespace

// adhoc #32: ClaudeTranscriptView::linkifyReferences makes a filename an agent
// mentions in prose clickable exactly as it was written — "read
// discord-quality-intake.skill" links the bare name, and an agent that printed
// paths from its own working directory links a partial one. Both only ever
// resolved against the repository root, so a reference to anything below the top
// level — nearly every file — reported a file that is plainly present as
// missing. Take the reference as a path suffix and match it against the tree.
QStringList MainWindow::transcriptFilePathMatches(const QString &repoDir,
                                                  const QString &ref,
                                                  const QString &path)
{
    if (repoDir.isEmpty() || ref.isEmpty() || path.isEmpty())
        return {};
    QByteArray listing;
    if (!runGitCapture(repoDir,
                       {QStringLiteral("ls-tree"), QStringLiteral("-r"),
                        QStringLiteral("--name-only"), QStringLiteral("-z"), ref},
                       &listing, nullptr))
        return {};
    // -z keeps unusual filenames literal instead of C-quoting them.
    const QString suffix = QLatin1Char('/') + path;
    QStringList matches;
    const QList<QByteArray> entries = listing.split('\0');
    for (const QByteArray &entry : entries) {
        if (entry.isEmpty())
            continue;
        const QString candidate = QString::fromUtf8(entry);
        if (candidate == path || candidate.endsWith(suffix))
            matches.append(candidate);
    }
    // Shallowest first, so a reference to a name that also exists in a vendored
    // or test copy opens the top-level one, with ties broken by name to keep the
    // pick stable from click to click.
    std::sort(matches.begin(), matches.end(),
              [](const QString &a, const QString &b) {
                  const qsizetype depthA = a.count(QLatin1Char('/'));
                  const qsizetype depthB = b.count(QLatin1Char('/'));
                  if (depthA != depthB)
                      return depthA < depthB;
                  return a < b;
              });
    return matches;
}

QString MainWindow::agentProviderName(const QString &provider) const
{
    if (provider == QLatin1String("claude-code"))
        return QStringLiteral("CC");
    if (agentIsCodexProvider(provider))
        return QStringLiteral("Codex");
    if (agentIsCloudflareAiProvider(provider))
        return QStringLiteral("CF AI");
    if (agentIsLocalLlamaProvider(provider))
        return QStringLiteral("Local Llama");
    if (provider.startsWith(QLatin1String("claude")))
        return QStringLiteral("Claude API");
    return QStringLiteral("OpenAI API");
}

void MainWindow::openAgentTranscriptReference(const QString &href)
{
    const QString filePrefix = QStringLiteral("forkmesh-file:");
    const QString branchPrefix = QStringLiteral("forkmesh-branch:");
    const QString windowPrefix = QStringLiteral("forkmesh-window:");
    if (href.startsWith(windowPrefix)) {
        if (!m_agentWindows || !m_agentWindows->raise(href.mid(windowPrefix.size())))
            flashMessage(QStringLiteral("That window is closed."), true);
        return;
    }
    if (href.startsWith(branchPrefix)) {
        const QString encoded = href.mid(branchPrefix.size());
        const QString branch =
            QUrl::fromPercentEncoding(encoded.toUtf8()).trimmed();
        const QString dir = repoGitDir();
        if (dir.isEmpty()) {
            flashMessage(QStringLiteral("Open this Agent's repository to view %1.")
                             .arg(branch),
                         true);
            return;
        }
        if (!localBranchExists(dir, branch)) {
            // No such branch, and it reads like a path: open it as one. The
            // file handler reports its own miss, so nothing comes back here.
            if (transcriptReferenceLooksLikePath(branch) ||
                branch.contains(QLatin1Char('/')) ||
                QFileInfo::exists(QDir(dir).filePath(branch))) {
                openAgentTranscriptReference(filePrefix + encoded);
                return;
            }
            flashMessage(QStringLiteral("Branch %1 is not available in this repository.")
                             .arg(branch),
                         true, QString(), forkmesh::kLocalOperationNoticeKind);
            return;
        }
        switchToBranch(branch);
        return;
    }

    if (href.startsWith(filePrefix)) {
        QString encoded = href.mid(filePrefix.size());
        int line = 0;
        const int queryAt = encoded.indexOf(QStringLiteral("?line="));
        if (queryAt >= 0) {
            bool ok = false;
            line = encoded.mid(queryAt + 6).toInt(&ok);
            if (!ok || line < 1)
                line = 0;
            encoded.truncate(queryAt);
        }
        QString path = QUrl::fromPercentEncoding(encoded.toUtf8()).trimmed();
        // The user explicitly followed a path an agent put in its transcript.
        // Build artefacts and screenshots commonly live beside the checkout,
        // so they are useful even though Git cannot resolve them. Keep those
        // files read-only: images and videos use the attachment viewer, while
        // other files use the Files editor's local-file tab.
        const auto openExternalFile = [this, line](const QString &absolute) {
            if (QImageReader(absolute).canRead() ||
                isVideoAttachmentPath(absolute)) {
                showQuickAddImageDetail(absolute);
                return;
            }
            if (line > 0)
                openRepoFileAtLine(absolute, line);
            else
                openLocalFileTab(absolute);
        };
        const QString dir = repoGitDir();
        // An absolute transcript reference is still useful after the user has
        // navigated away from its repository. It opens read-only, so it does
        // not need a repository context.
        if (QDir::isAbsolutePath(path) && dir.isEmpty()) {
            openExternalFile(QDir::cleanPath(path));
            return;
        }
        if (dir.isEmpty()) {
            flashMessage(QStringLiteral("Open this Agent's repository to view %1.")
                             .arg(path),
                         true);
            return;
        }

        const QString repoRoot = QDir(dir).absolutePath();
        QString fileRoot = repoRoot;
        QString agentBranch;
        if (const AgentSession *session = findAgentSession(m_selectedAgentSessionId)) {
            if (localBranchExists(dir, session->branchName)) {
                agentBranch = session->branchName;
                const QString worktree =
                    worktreePathForBranch(dir, session->branchName);
                if (!worktree.isEmpty())
                    fileRoot = QDir(worktree).absolutePath();
            }
        }
        if (QDir::isAbsolutePath(path)) {
            const QString absolute = QDir::cleanPath(path);
            const QString repoPrefix = repoRoot + QLatin1Char('/');
            const QString filePrefixPath = fileRoot + QLatin1Char('/');
            if (absolute.startsWith(filePrefixPath))
                path = absolute.mid(filePrefixPath.size());
            else if (absolute.startsWith(repoPrefix))
                path = absolute.mid(repoPrefix.size());
            else {
                openExternalFile(absolute);
                return;
            }
        }
        while (path.startsWith(QStringLiteral("./")))
            path.remove(0, 2);
        if (path.startsWith(QStringLiteral("a/")) ||
            path.startsWith(QStringLiteral("b/")))
            path.remove(0, 2);
        path = QDir::cleanPath(path);
        if (path.isEmpty() || path == QLatin1String(".") ||
            path == QLatin1String("..") || path.startsWith(QStringLiteral("../"))) {
            flashMessage(QStringLiteral("That transcript file reference is not valid."),
                         true);
            return;
        }

        const QFileInfo inWorktree(QDir(fileRoot).filePath(path));
        const QFileInfo inCheckout(QDir(repoRoot).filePath(path));
        if (inWorktree.isDir() || (!inWorktree.exists() && inCheckout.isDir())) {
            showSection(kFilesSectionIndex);
            if (m_filesNavButton)
                m_filesNavButton->setChecked(true);
            setFileExplorerRoot((inWorktree.isDir() ? inWorktree : inCheckout)
                                    .absoluteFilePath());
            return;
        }
        if (!inWorktree.exists() && inCheckout.isFile() && fileRoot != repoRoot) {
            openExternalFile(inCheckout.absoluteFilePath());
            return;
        }
        const bool workingFile = inWorktree.isFile();
        QByteArray ignored;
        QString gitError;
        const QString ref = !agentBranch.isEmpty()
                                ? agentBranch
                                : (currentRef().isEmpty() ? QStringLiteral("HEAD")
                                                          : currentRef());
        const bool trackedFile = runGitCapture(
            dir, {QStringLiteral("cat-file"), QStringLiteral("-e"),
                  QStringLiteral("%1:%2").arg(ref, path)},
            &ignored, &gitError);
        if (!workingFile && !trackedFile) {
            const QStringList matches =
                transcriptFilePathMatches(dir, ref, path);
            if (matches.isEmpty()) {
                // The other half of the ambiguity: nothing by that path, but a
                // branch of the same name is what the agent meant.
                if (localBranchExists(dir, path)) {
                    switchToBranch(path);
                    return;
                }
                flashMessage(QStringLiteral("File %1 is not available in this repository.")
                                 .arg(path),
                             true, QString(), forkmesh::kLocalOperationNoticeKind);
                return;
            }
            if (matches.size() > 1)
                flashMessage(QStringLiteral("%1 files are named %2 — opened %3.")
                                 .arg(matches.size())
                                 .arg(path, matches.constFirst()));
            path = matches.constFirst();
        }
        if (!agentBranch.isEmpty() && currentRef() != agentBranch)
            setRepoBranch(agentBranch);
        if (line > 0)
            openRepoFileAtLine(path, line);
        else
            openRepoFile(path);
        return;
    }

    openBodyReference(href);
}

void MainWindow::openAgentCommandTerminal(const QString &command,
                                          const QString &recordedCwd)
{
    if (command.trimmed().isEmpty())
        return;

    QString cwd = recordedCwd.trimmed();
    const QString sessionDir = sessionWorkdir(m_selectedAgentSessionId);
    if (!cwd.isEmpty() && QDir::isRelativePath(cwd) && !sessionDir.isEmpty())
        cwd = QDir(sessionDir).absoluteFilePath(cwd);
    if (cwd.isEmpty() || !QDir(cwd).exists())
        cwd = sessionDir;
    if (cwd.isEmpty() || !QDir(cwd).exists()) {
        flashMessage(QStringLiteral(
                         "This command's working directory is no longer available."),
                     true);
        return;
    }

    if (forkmesh::vm::active()) {
        const QString vmError = forkmesh::vm::availabilityError().isEmpty()
                                    ? forkmesh::vm::pathError(cwd)
                                    : forkmesh::vm::availabilityError();
        if (!vmError.isEmpty()) {
            flashMessage(vmError, true);
            return;
        }
    }

    auto *dialog = new QDialog(this);
    dialog->setObjectName(QStringLiteral("agentCommandTerminalDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QStringLiteral("Command terminal"));
    dialog->resize(960, 600);
    auto *layout = new QVBoxLayout(dialog);
    auto *notice = new QLabel(
        QStringLiteral("Running in <code>%1</code> %2.")
            .arg(cwd.toHtmlEscaped(),
                 forkmesh::vm::active()
                     ? QStringLiteral("inside the KVM guest")
                     : QStringLiteral("on this host")),
        dialog);
    notice->setWordWrap(true);
    layout->addWidget(notice);

    auto *terminal = new TerminalWidget(dialog);
    terminal->setObjectName(QStringLiteral("agentCommandTerminal"));
    layout->addWidget(terminal, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    layout->addWidget(buttons);

    dialog->show();
    dialog->raise();
    dialog->activateWindow();
    const bool guest = forkmesh::vm::active();
    const QString executable =
        guest ? forkmesh::vm::interactiveCommand(
                    QStringLiteral("bash"), cwd,
                    {QStringLiteral("-lc"), command})
              : command;
    terminal->runCommand(executable, cwd, {}, !guest);
    terminal->setFocus();
}

#ifdef FORKMESH_WINDOW_TESTS
QStringList MainWindow::testAgentOwnedDiffPaths(const QString &gitDir,
                                                const QString &base,
                                                const QString &branch) const
{
    QSet<QString> paths;
    if (!readAgentOwnedPaths(gitDir, base, branch, &paths))
        return {};
    QStringList sorted = paths.values();
    std::sort(sorted.begin(), sorted.end());
    return sorted;
}
#endif

QWidget *MainWindow::buildAgentsTab()
{
    auto *page = new QWidget;
    m_agentsPage = page;
    page->setObjectName(QStringLiteral("agentsPage"));

    auto *listPane = new QWidget;
    listPane->setObjectName(QStringLiteral("agentsListPane"));
    listPane->setMinimumWidth(260);

    auto *agentTable = new AgentPriorityTable(0, 2);
    m_agentTable = agentTable;
    m_agentTable->setObjectName("issueTable");
    m_agentTable->setItemDelegate(new SelectionBorderRowDelegate(m_agentTable));
    m_agentTable->setStyleSheet(Theme::remapSheet(
        "#issueTable { selection-background-color: transparent; border: none; }"
        "#issueTable::item:selected { background: transparent; }"));
    m_agentTable->setFrameShape(QFrame::NoFrame);
    m_agentTable->setHorizontalHeaderLabels({"#", "Issue"});
    m_agentTable->verticalHeader()->setVisible(false);
    m_agentTable->horizontalHeader()->setVisible(false);
    m_agentTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    // Keep the familiar file-list gestures: Shift extends a contiguous range,
    // and Ctrl/Cmd toggles individual rows. The detail pane still follows the
    // current selected row while the action bar can operate on the whole set.
    m_agentTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_agentTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_agentTable->setShowGrid(false);
    m_agentTable->setWordWrap(false);
    m_agentTable->setTextElideMode(Qt::ElideNone);
    // Priority is the sort key, and moving a row rewrites that key. Header
    // sorting would fight the drop order and make the row spring elsewhere.
    m_agentTable->setSortingEnabled(false);
    m_agentTable->setDragEnabled(true);
    m_agentTable->setAcceptDrops(true);
    m_agentTable->viewport()->setAcceptDrops(true);
    m_agentTable->setDropIndicatorShown(false);
    m_agentTable->setDragDropMode(QAbstractItemView::InternalMove);
    m_agentTable->setDragDropOverwriteMode(false);
    m_agentTable->setDefaultDropAction(Qt::MoveAction);
    agentTable->onSessionsDrop = [this](const QList<int> &sessionIds,
                                        int targetSessionId,
                                        const QString &folderId, bool after) {
        dropAgentSessions(sessionIds, targetSessionId, folderId, after);
    };
    agentTable->onFolderDrop = [this](const QString &folderId,
                                      const QString &targetFolderId, bool after) {
        dropAgentFolder(folderId, targetFolderId, after);
    };
    agentTable->onFolderToggle = [this](const QString &folderId) {
        toggleAgentFolder(folderId);
    };
    QHeaderView *agentHeader = m_agentTable->horizontalHeader();
    agentHeader->setHighlightSections(false);
    // ResizeToContents re-shaped every ID/status string whenever a row role
    // changed. With active agents updating several times a second that forced a
    // full-column sizeHint scan on the GUI thread. The glyph/chips have a fixed
    // geometry, so reserve it once and leave only the Issue column flexible.
    agentHeader->setSectionResizeMode(kAgentIdColumn, QHeaderView::Fixed);
    agentHeader->resizeSection(kAgentIdColumn, kAgentIdColumnWidth);
    agentHeader->setSectionResizeMode(kAgentIssueColumn, QHeaderView::Stretch);
    m_agentTable->setItemDelegateForColumn(
        kAgentIdColumn,
        new AgentBranchButtonDelegate(
            m_agentTable, [this](int sessionId) { switchToAgentBranch(sessionId); },
            [this](int sessionId) { fixAgentConflictsWithAgent(sessionId); }));
    m_agentTable->setItemDelegateForColumn(
        kAgentIssueColumn,
        new AgentRowActionsDelegate(
            m_agentTable,
            [this](int sessionId) { showAgentSessionImages(sessionId); },
            [this](int sessionId) { toggleAgentSessionRun(sessionId); },
            [this](int sessionId) {
                moveAgentSessionToPriorityEdge(sessionId, /*bottom=*/false);
            },
            [this](int sessionId) {
                moveAgentSessionToPriorityEdge(sessionId, /*bottom=*/true);
            },
            [this](int sessionId) { deleteAgentSessionFromRow(sessionId); }));
    m_agentTable->setIconSize(
        QSize(qMax(kAgentThumbnailPx, kAgentLeadGlyphsPx), kAgentThumbnailPx));
    QHeaderView *agentRows = m_agentTable->verticalHeader();
    agentRows->setDefaultSectionSize(
        qMax(agentRows->defaultSectionSize(), kAgentThumbnailPx + 8));
    m_agentCompactRowHeight = agentRows->defaultSectionSize();
    m_scannerTimer = new QTimer(this);
    m_scannerTimer->setInterval(45);
    connect(m_scannerTimer, &QTimer::timeout, this, &MainWindow::onScannerTick);

    m_externalClaudeTimer = new QTimer(this);
    m_externalClaudeTimer->setInterval(3 * 1000);
    connect(m_externalClaudeTimer, &QTimer::timeout, this, [this] {
        const forkmesh::BackgroundRoutine routine(
            QStringLiteral("external Claude Code scan timer (every 3s)"));
        onExternalClaudeTick();
    });
    m_externalClaudeTimer->start();

    auto *listLayout = new QVBoxLayout(listPane);
    listLayout->setContentsMargins(0, 0, 0, 0);
    listLayout->setSpacing(0);

    listLayout->addWidget(buildAgentListToolBar());
    listLayout->addWidget(buildAgentRecycleBinBar());
    listLayout->addWidget(m_agentTable, 1);
    m_agentSearchEmpty = createSearchEmptyNotice();
    listLayout->addWidget(m_agentSearchEmpty, 1);

    auto *detailPane = new QWidget;
    detailPane->setObjectName(QStringLiteral("agentsDetailPane"));
    m_agentTitle = new WrappedTitleLabel(QStringLiteral("Select a session"));
    m_agentTitle->setObjectName("agentPromptTitle");
    m_agentMeta = new QLabel;
    m_agentMeta->setObjectName("statusLine");
    m_agentMeta->setTextFormat(Qt::RichText);
    m_agentMeta->setTextInteractionFlags(Qt::TextSelectableByMouse |
                                         Qt::LinksAccessibleByMouse);
    m_agentMeta->setWordWrap(true);
    connect(m_agentMeta, &QLabel::linkActivated, this, [this](const QString &href) {
        if (href.startsWith(kPullLinkScheme))
            openPullDiffInGitView(href.mid(kPullLinkScheme.size()).toInt());
        else if (href.startsWith(kIssueLinkScheme)) {
            if (const AgentSession *s = findAgentSession(m_selectedAgentSessionId)) {
                NotificationLink link;
                link.kind = QStringLiteral("issue");
                link.owner = s->owner;
                link.name = s->name;
                link.number = href.mid(kIssueLinkScheme.size()).toInt();
                openNotificationLink(link);
            }
        } else if (href.startsWith(kTaskLinkScheme))
            openOrganizationTask(href.mid(kTaskLinkScheme.size()));
        if (m_agentMetaPopup)
            m_agentMetaPopup->hide(); // the click navigated away from this pane
    });

    m_agentMetaPopup = new QFrame(this, Qt::Popup);
    m_agentMetaPopup->setObjectName("agentMetaPopup"); // themed like #reactionPicker
    m_agentMetaPopup->installEventFilter(this);
    auto *metaPopupLayout = new QVBoxLayout(m_agentMetaPopup);
    metaPopupLayout->setContentsMargins(12, 10, 12, 10);
    metaPopupLayout->addWidget(m_agentMeta);
    m_agentPopOutButton = railActionButton(QStringLiteral("terminal"),
                                           QStringLiteral("Pop out"), QString());
    m_agentPopOutButton->setObjectName("agentPopOutButton");
    connect(m_agentPopOutButton, &QPushButton::clicked, this,
            [this] { popOutAgentSessionToTerminal(m_selectedAgentSessionId); });
    auto *popOutRow = new QHBoxLayout;
    popOutRow->setContentsMargins(0, 6, 0, 0);
    popOutRow->addStretch(1);
    popOutRow->addWidget(m_agentPopOutButton);
    metaPopupLayout->addLayout(popOutRow);
    buildAgentDetailActionButtons();

    auto *actionRow = new QHBoxLayout;
    actionRow->setContentsMargins(0, 0, 0, 0);
    actionRow->setSpacing(6);
    actionRow->addWidget(m_agentMoveToCloudButton);
    actionRow->addWidget(m_agentContinueHereButton);
    actionRow->addWidget(m_agentSignInRunnerButton);
    actionRow->addWidget(m_agentRetryInCloudButton);
    actionRow->addWidget(m_agentCloudMergeButton);
    actionRow->addWidget(m_agentCloudCleanupButton);
    actionRow->addWidget(m_agentCloudUpdateButton);
    actionRow->addWidget(m_agentCloudActionsButton);
    actionRow->addWidget(m_agentViewTaskButton);
    actionRow->addWidget(m_agentCreateIssueButton);
    actionRow->addWidget(m_agentViewIssueButton);
    actionRow->addWidget(m_agentCreatePrButton);
    actionRow->addWidget(m_agentViewPrButton);
    actionRow->addWidget(m_agentStartButton);
    actionRow->addWidget(m_agentTaskGradeCombo);
    actionRow->addWidget(m_agentStopButton);
    actionRow->addWidget(m_agentDeleteAllButton);
    actionRow->addWidget(m_agentWorktreeButton);

    auto *topRow = new QVBoxLayout;
    topRow->setContentsMargins(0, 0, 0, 0);
    topRow->setSpacing(4);
    auto *statusRow = new QHBoxLayout;
    statusRow->setContentsMargins(0, 0, 0, 0);
    statusRow->setSpacing(8);
    statusRow->addStretch(1);
    statusRow->addWidget(m_agentStatusPill, 0, Qt::AlignVCenter);
    statusRow->addSpacing(4);
    statusRow->addLayout(actionRow);
    topRow->addLayout(statusRow);
    m_agentPromptImages = new QWidget;
    m_agentPromptImages->setObjectName(QStringLiteral("agentPromptImages"));
    auto *promptImagesRow = new QHBoxLayout(m_agentPromptImages);
    promptImagesRow->setContentsMargins(0, 0, 0, 2);
    promptImagesRow->setSpacing(6);
    m_agentPromptImages->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    m_agentPromptImages->hide();
    topRow->addWidget(m_agentPromptImages);
    auto *promptTitleBlock = new QWidget;
    promptTitleBlock->setObjectName(QStringLiteral("agentPromptTitleBlock"));
    promptTitleBlock->setAttribute(Qt::WA_StyledBackground);
    auto *promptTitleLayout = new QVBoxLayout(promptTitleBlock);
    promptTitleLayout->setContentsMargins(0, 0, 0, 0);
    promptTitleLayout->setSpacing(0);
    promptTitleLayout->addWidget(m_agentTitle);
    topRow->addWidget(promptTitleBlock);

    buildAgentOutputViews(detailPane);

    statusRow->addWidget(m_agentListenButton, 0, Qt::AlignVCenter);
    statusRow->addWidget(m_agentOutputModeButton, 0, Qt::AlignVCenter);
    statusRow->addWidget(m_agentCodeButton, 0, Qt::AlignVCenter);
    statusRow->addWidget(m_agentSyncLabel, 0, Qt::AlignVCenter);
    statusRow->addWidget(m_agentSyncButton, 0, Qt::AlignVCenter);
    statusRow->addWidget(m_agentBranchButton, 0, Qt::AlignVCenter);

    buildAgentFilesPanel();

    auto *filesActionBar = new QHBoxLayout;
    filesActionBar->setContentsMargins(0, 0, 0, 0);
    filesActionBar->addStretch();
    filesActionBar->addWidget(m_agentUpdateButton);
    filesActionBar->addWidget(m_agentMergeButton);
    filesActionBar->addWidget(m_agentMergeDeleteButton);
    filesActionBar->addWidget(m_agentWtDeleteButton);

    auto *filesDiffSplit = new QSplitter(Qt::Horizontal);
    filesDiffSplit->setChildrenCollapsible(false);
    filesDiffSplit->addWidget(m_agentFilesPanel);
    auto *diffColumn = new QWidget;
    auto *diffColumnLayout = new QVBoxLayout(diffColumn);
    diffColumnLayout->setContentsMargins(0, 0, 0, 0);
    diffColumnLayout->setSpacing(6);
    m_agentSecretBanner = new forkmesh::ui::SecretDiffBanner(diffColumn);
    m_agentSecretBanner->onIgnore =
        [this](const QList<RepoSecurityFinding> &findings) {
            ignoreAgentDiffSecrets(findings);
        };
    diffColumnLayout->addWidget(m_agentSecretBanner);
    diffColumnLayout->addWidget(m_agentDiffView, 1);
    filesDiffSplit->addWidget(diffColumn);
    filesDiffSplit->setStretchFactor(0, 0);
    filesDiffSplit->setStretchFactor(1, 1);
    filesDiffSplit->setSizes({220, 700});

    // The Code page: the busiest files as chips over the list + diff split.
    m_agentCodeFileStrip = new QWidget;
    m_agentCodeFileStrip->setObjectName(QStringLiteral("agentCodeFileStrip"));
    new FlowLayout(m_agentCodeFileStrip, 0, 10, 4);
    m_agentCodeViewedLabel = new QLabel;
    m_agentCodeViewedLabel->setObjectName(QStringLiteral("agentCodeViewedCount"));
    m_agentCodeViewedLabel->setTextFormat(Qt::RichText);
    m_agentCodeViewedLabel->setToolTip(
        QStringLiteral("A green check means every file is viewed as it stands now"));
    m_agentCodeViewedLabel->hide();
    m_agentCodePage = new QWidget;
    m_agentCodePage->setObjectName(QStringLiteral("agentCodePage"));
    auto *codeLayout = new QVBoxLayout(m_agentCodePage);
    codeLayout->setContentsMargins(0, 0, 0, 0);
    codeLayout->setSpacing(6);
    codeLayout->addWidget(m_agentCodeViewedLabel);
    codeLayout->addWidget(m_agentCodeFileStrip);
    codeLayout->addWidget(filesDiffSplit, 1);
    m_agentCodePage->hide();

    m_agentCodeSplit = new QSplitter(Qt::Horizontal);
    m_agentCodeSplit->setObjectName(QStringLiteral("agentCodeSplit"));
    m_agentCodeSplit->setChildrenCollapsible(false);
    m_agentCodeSplit->addWidget(m_agentOutputStack);
    m_agentCodeSplit->addWidget(m_agentCodePage);
    m_agentCodeSplit->setStretchFactor(0, 1);
    m_agentCodeSplit->setStretchFactor(1, 1);

    m_agentCodePollTimer = new QTimer(this);
    m_agentCodePollTimer->setInterval(3000);
    connect(m_agentCodePollTimer, &QTimer::timeout, this, [this] {
        // Transcript events already refresh on each tool result; this catches
        // edits made between them while a run is live and the page is on screen.
        const int sid = m_selectedAgentSessionId;
        const AgentSession *s = findAgentSession(sid);
        if (!m_agentCodePage || !m_agentCodePage->isVisible() || !s ||
            s->status != AgentStatus::Running)
            return;
        refreshAgentFilesPanel(sid);
    });

    auto *filesChangedPage = new QWidget;
    auto *filesChangedLayout = new QVBoxLayout(filesChangedPage);
    filesChangedLayout->setContentsMargins(0, 8, 0, 0);
    filesChangedLayout->setSpacing(6);
    filesChangedLayout->addLayout(filesActionBar);

    auto *agentOutputPage = new QWidget;
    auto *agentOutputLayout = new QVBoxLayout(agentOutputPage);
    agentOutputLayout->setContentsMargins(0, 8, 0, 0);
    agentOutputLayout->setSpacing(6);
    agentOutputLayout->addWidget(m_agentCodeSplit, 1);

    filesChangedPage->setParent(detailPane);
    filesChangedPage->hide();

    auto *outputContainer = agentOutputPage;
    outputContainer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);


    m_agentHourlyTimer = new QTimer(this);
    m_agentHourlyTimer->setInterval(60 * 60 * 1000);
    connect(m_agentHourlyTimer, &QTimer::timeout, this, [this] {
        const forkmesh::BackgroundRoutine routine(
            QStringLiteral("agent hourly timer (Claude spend, files panel)"));
        refreshClaudeSpend();
        if (m_selectedAgentSessionId > 0)
            refreshAgentFilesPanel(m_selectedAgentSessionId);
    });
    m_agentHourlyTimer->start();

    // Push owner-encrypted snapshots of this node's agent sessions to the
    // relay once after launch. There is no periodic push: every real change
    // already schedules a debounced one (scheduleAgentSessionsPush), and
    // encrypted prompts arrive over the repo's control socket ("agents"
    // topic) into the shared /api/sync, so a timer only re-sent per-repo
    // POSTs the relay had already seen.
    QTimer::singleShot(10 * 1000, this, &MainWindow::pushAgentSessionsSnapshot);



    m_agentNetPanel = new QLabel;
    m_agentNetPanel->setObjectName("agentNetPanel");
    m_agentNetPanel->setTextFormat(Qt::RichText);
    m_agentNetPanel->setWordWrap(true);
    m_agentNetPanel->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto *detailLayout = new QVBoxLayout(detailPane);
    detailLayout->setContentsMargins(12, 12, 22, 14);
    detailLayout->setSpacing(8);
    detailLayout->addLayout(topRow);
    detailLayout->addWidget(m_agentNetPanel);
    detailLayout->addWidget(outputContainer, 1); // the Agent transcript + Raw toggle

    m_agentDetail = detailPane;
    m_agentDetailBaseBottomMargin = detailLayout->contentsMargins().bottom();
    // The docked composer spans this pane, so every move of it — the splitter
    // being dragged, the pane opening on a first selection — has to re-place
    // the footer strip over it.
    detailPane->installEventFilter(this);
    detailPane->hide();

    auto *splitter = new QSplitter(Qt::Horizontal);
    splitter->setChildrenCollapsible(true);
    splitter->setOpaqueResize(false);
    splitter->addWidget(listPane);
    splitter->addWidget(detailPane);
    // Workspace view's shell terminal (MainWindowViews.cpp); hidden otherwise.
    splitter->addWidget(buildWorkspaceTerminalDock());
    splitter->setCollapsible(0, false);
    splitter->setCollapsible(1, true);
    splitter->setCollapsible(2, false);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    splitter->setStretchFactor(2, 0);
    splitter->setSizes({600, 600, 0});
    m_agentWorkspaceSplit = splitter;

    // The page stacks the session list/transcript splitter over the live
    // tool-efficiency and guardrail boards; the list's action bar opens
    // each board and the board's header returns. Zero margins keep the
    // list pane flush with the page bottom so the docked composer maths
    // stays unchanged.
    m_agentPageStack = new QStackedWidget;
    m_agentPageStack->setObjectName(QStringLiteral("agentPageStack"));
    m_agentPageStack->addWidget(splitter);
    m_agentPageStack->addWidget(buildAgentEfficiencyPage());
    m_agentPageStack->addWidget(buildAgentGuardrailPage());
    m_agentPageStack->addWidget(buildAgentModelsPage());

    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_agentPageStack, 1);

    m_agentTable->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_agentTable, &QWidget::customContextMenuRequested, this,
            &MainWindow::showAgentListContextMenu);
    connect(m_agentTable, &QTableWidget::itemSelectionChanged, this, [this] {
        const QModelIndexList rows = m_agentTable->selectionModel()->selectedRows();
        if (rows.isEmpty()) {
            updateAgentActionState();
            return;
        }
        QModelIndex current = m_agentTable->currentIndex();
        if (!current.isValid() ||
            !m_agentTable->selectionModel()->isSelected(current))
            current = rows.first();
        QTableWidgetItem *first = m_agentTable->item(current.row(), 0);
        if (first) {
            // Do this before showAgentSession() reveals the detail pane. A
            // visible QScrollArea otherwise briefly lays out at the top and
            // visibly runs down to the transcript tail after the click.
            if (m_agentTranscript)
                m_agentTranscript->jumpToBottom();
            showAgentSession(first->data(Qt::UserRole).toInt());
        }
        updateAgentActionState();
    });
    forkmesh::ui::NextAccess::installWorkspace(*this);
    // A Workspace view restored at startup, or picked before Agents was ever
    // opened, shows its terminal pane now that the pane exists.
    applyAppViewMode();
    return page;
}

// The thin action bar across the top of the Agents list: bulk actions, the
// per-provider account buttons with their usage gauges, sort, filter and the
// concurrency control.
QWidget *MainWindow::buildAgentListToolBar()
{
    m_agentDeleteMergedButton = railActionButton(
        QStringLiteral("trash"), QStringLiteral("Del merged"),
        "Delete the worktree, branch and session of every merged agent");
    m_agentDeleteMergedButton->setObjectName("agentDeleteMergedButton");
    connect(m_agentDeleteMergedButton, &QPushButton::clicked, this,
            &MainWindow::deleteAllMergedAgentSessions);

    m_agentDeleteSelectedButton = railActionButton(
        QStringLiteral("trash"), QStringLiteral("Delete selected"),
        "Delete every selected agent session, including its unmerged pull request, "
        "worktree folder and branch when present; merged pull requests are retained");
    m_agentDeleteSelectedButton->setObjectName("agentDeleteSelectedButton");
    m_agentDeleteSelectedButton->setEnabled(false);
    connect(m_agentDeleteSelectedButton, &QPushButton::clicked, this,
            &MainWindow::deleteSelectedAgentSessions);

    m_agentModelsButton = railActionButton(
        QStringLiteral("smart-toy"), QStringLiteral("Models"),
        "Open the model board: every model with its runs, success and failure "
        "rates, approximate cost, merges (backfilled from commit messages) and "
        "tool-call efficiency, sortable by any column");
    m_agentModelsButton->setObjectName("agentModelsButton");
    connect(m_agentModelsButton, &QPushButton::clicked, this,
            [this] { setAgentPageTab(3); });

    m_agentEfficiencyButton = railActionButton(
        QStringLiteral("graph"), QStringLiteral("Tool efficiency"),
        "Open the live tool-efficiency board: context and cost per tool family, "
        "on/off switches, and the realtime log of potential improvements");
    m_agentEfficiencyButton->setObjectName("agentEfficiencyButton");
    connect(m_agentEfficiencyButton, &QPushButton::clicked, this,
            [this] { setAgentPageTab(1); });

    m_agentGuardrailsButton = railActionButton(
        QStringLiteral("shield-check"), QStringLiteral("Guardrails"),
        "Open the guardrail board: every repository's agent boundary with "
        "blocked tool calls, Approve/Deny answers, and agents stopped");
    m_agentGuardrailsButton->setObjectName("agentGuardrailsButton");
    connect(m_agentGuardrailsButton, &QPushButton::clicked, this,
            [this] { setAgentPageTab(2); });

    m_agentStopAllButton = railActionButton(
        QStringLiteral("circle-slash"), QStringLiteral("Stop all"),
        "Stop every running agent and cancel the queued ones. External "
        "Claude Code sessions started outside ForkMesh are left alone.");
    m_agentStopAllButton->setObjectName("agentStopAllButton");
    connect(m_agentStopAllButton, &QPushButton::clicked, this,
            &MainWindow::stopAllRunningAgents);

    m_agentStartAllButton = railActionButton(
        QStringLiteral("rocket"), QStringLiteral("Start all"),
        "Resume every stopped or failed agent. Merged sessions and external "
        "Claude Code sessions started outside ForkMesh are left alone.");
    m_agentStartAllButton->setObjectName("agentStartAllButton");
    connect(m_agentStartAllButton, &QPushButton::clicked, this,
            &MainWindow::startAllStoppedAgents);

    m_agentUpdateAllButton = railActionButton(
        QStringLiteral("sync"), QStringLiteral("Update all"),
        "Merge each agent's base branch into its own worktree branch, and re-read "
        "the list's diff and ahead/behind counts. Uncommitted work is stashed and "
        "restored on top; a branch that can't merge cleanly is left exactly as it "
        "was.");
    m_agentUpdateAllButton->setObjectName("agentUpdateAllButton");
    connect(m_agentUpdateAllButton, &QPushButton::clicked, this,
            &MainWindow::updateAllAgentWorktreesFromMain);

    m_agentAnnouncerButton = railActionButton(
        QStringLiteral("broadcast"), QStringLiteral("Announcer"), QString());
    m_agentAnnouncerButton->setObjectName("agentAnnouncerButton");
    m_agentAnnouncerButton->setCheckable(true);
    {
        // Fixed to the widest caption it ever shows, so going on air or
        // counting a download up does not shuffle the toolbar.
        QFont f = QGuiApplication::font();
        f.setPixelSize(10);
        f.setWeight(QFont::DemiBold);
        m_agentAnnouncerButton->setFixedSize(
            qMax(railItemWidth(),
                 QFontMetrics(f).horizontalAdvance(QStringLiteral("Voice 100%")) + 12),
            kRailItemHeight);
    }
    connect(m_agentAnnouncerButton, &QPushButton::clicked, this,
            &MainWindow::toggleAgentAnnouncer);
    refreshAgentAnnouncerButton();
    // Left on last time: pick the microphone back up, but only with a voice
    // that is already here - a download is never started unasked.
    QTimer::singleShot(4000, this, [this] {
        if (QSettings().value(QStringLiteral("agents/announcerOn"), false).toBool()
            && !agentAnnouncerOn())
            startAgentAnnouncer(/*remember=*/false);
    });

    // A thin bar across the top of the list, like the code page's mode bar,
    // rather than a card floating over the first rows of the table.
    auto *agentToolBar = new QWidget;
    agentToolBar->setObjectName(QStringLiteral("agentsActionBar"));
    agentToolBar->setAttribute(Qt::WA_StyledBackground);
    auto *agentQueueLayout = new QHBoxLayout(agentToolBar);
    agentQueueLayout->setContentsMargins(8, 2, 8, 2);
    agentQueueLayout->setSpacing(6);

    m_agentUsageButton = railActionButton(
        QStringLiteral("terminal"), QStringLiteral("Usage"), QString());
    m_agentUsageButton->setObjectName(QStringLiteral("agentUsageButton"));
    m_agentUsageButton->setIconPainter(
        [this](QPainter &p, const QRect &iconRect, const QColor &fg) {
            const auto *chart = static_cast<const TokenUsageMiniChart *>(
                agentIsCodexProvider(m_agentUsageProvider) ? m_navCodexUsage
                                                           : m_navTokenUsage);
            p.save();
            p.setRenderHint(QPainter::Antialiasing, true);
            QRectF screen(0, 0, 30, 18);
            screen.moveCenter(QRectF(iconRect).center() - QPointF(0, 1));
            QColor glass = fg;
            glass.setAlpha(22);
            QPen frame(fg, 1.3);
            p.setPen(frame);
            p.setBrush(glass);
            p.drawRoundedRect(screen.adjusted(0.65, 0.65, -0.65, -0.65), 3.5, 3.5);

            QPen prompt(fg, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
            p.setPen(prompt);
            p.setBrush(Qt::NoBrush);
            const qreal left = screen.left() + 4.5;
            const qreal top = screen.top() + 4.5;
            p.drawPolyline(QPolygonF{QPointF(left, top), QPointF(left + 3, top + 2.5),
                                     QPointF(left, top + 5)});
            p.drawLine(QPointF(left + 4.5, top + 5), QPointF(left + 7.5, top + 5));

            const int windows = chart ? qMax(1, int(chart->percentages().size())) : 2;
            const qreal barW = 3.0;
            const qreal gap = 1.8;
            const qreal barsW = windows * barW + (windows - 1) * gap;
            const qreal trackTop = screen.top() + 3.5;
            const qreal trackH = screen.height() - 7.0;
            qreal x = screen.right() - 4.0 - barsW;
            QColor track = fg;
            track.setAlpha(60);
            p.setPen(Qt::NoPen);
            for (int i = 0; i < windows; ++i) {
                const QRectF bar(x, trackTop, barW, trackH);
                p.setBrush(track);
                p.drawRoundedRect(bar, barW / 2.0, barW / 2.0);
                const int filled = chart ? chart->filledPercent(i) : -1;
                if (filled > 0) {
                    const qreal fillH = qMax(barW, trackH * filled / 100.0);
                    p.setBrush(chart->barColor(chart->percentages().at(i)));
                    p.drawRoundedRect(
                        QRectF(x, trackTop + trackH - fillH, barW, fillH),
                        barW / 2.0, barW / 2.0);
                }
                x += barW + gap;
            }
            p.restore();
        });
    connect(m_agentUsageButton, &QPushButton::clicked, this, [this] {
        if (!openAgentUsagePage())
            showAgentUsageMenu(m_agentUsageButton->mapToGlobal(
                QPoint(0, m_agentUsageButton->height())));
    });
    m_agentUsageButton->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_agentUsageButton, &QWidget::customContextMenuRequested, this,
            [this](const QPoint &position) {
                showAgentUsageMenu(m_agentUsageButton->mapToGlobal(position));
            });
    m_agentUsageButton->installEventFilter(new RailEventHook(
        [this](QObject *, QEvent *event) {
            if (event->type() != QEvent::Enter)
                return false;
            if (agentIsCodexProvider(m_agentUsageProvider))
                refreshCodexUsageRemaining();
            else
                refreshClaudeCodeUsage(true);
            return false;
        },
        m_agentUsageButton));
    attachAgentUsageHoverCard(m_agentUsageButton,
                              [this] { return m_agentUsageProvider; });

    m_agentQueueButton = railActionButton(QStringLiteral("workflow"),
                                          QStringLiteral("0/0"), QString());
    m_agentQueueButton->setObjectName("agentQueueButton");
    connect(m_agentQueueButton, &QPushButton::clicked, this,
            &MainWindow::showAgentQueueLimitMenu);

    m_agentSortButton = railActionButton(
        QStringLiteral("sort-desc"), QStringLiteral("Sort"),
        QStringLiteral("Sort agent sessions"));
    m_agentSortButton->setObjectName(QStringLiteral("agentSortButton"));
    auto *sortMenu = new QMenu(m_agentSortButton);
    sortMenu->setTitle(QStringLiteral("Sort agents by"));
    auto *sortGroup = new QActionGroup(sortMenu);
    sortGroup->setExclusive(true);
    auto addSort = [sortMenu, sortGroup](const QString &label,
                                         const QString &objectName,
                                         AgentListSort sort) {
        QAction *action = sortMenu->addAction(label);
        action->setObjectName(objectName);
        action->setCheckable(true);
        action->setData(static_cast<int>(sort));
        sortGroup->addAction(action);
        return action;
    };
    addSort(QStringLiteral("Priority (highest first)"),
            QStringLiteral("agentSortPriority"), AgentListSort::Priority);
    sortMenu->addSeparator();
    addSort(QStringLiteral("Date added (newest first)"),
            QStringLiteral("agentSortNewest"), AgentListSort::NewestAdded);
    addSort(QStringLiteral("Date added (oldest first)"),
            QStringLiteral("agentSortOldest"), AgentListSort::OldestAdded);
    addSort(QStringLiteral("Recent activity"),
            QStringLiteral("agentSortRecent"), AgentListSort::RecentActivity)
        ->setChecked(true);
    addSort(QStringLiteral("Status (active first)"),
            QStringLiteral("agentSortStatus"), AgentListSort::Status);
    addSort(QStringLiteral("Conflicts (conflicting first)"),
            QStringLiteral("agentSortConflicts"), AgentListSort::Conflicts);
    sortMenu->addSeparator();
    addSort(QStringLiteral("Title (A-Z)"),
            QStringLiteral("agentSortTitle"), AgentListSort::Title);
    addSort(QStringLiteral("Provider and model"),
            QStringLiteral("agentSortProvider"), AgentListSort::Provider);
    addSort(QStringLiteral("Issue number"),
            QStringLiteral("agentSortIssue"), AgentListSort::IssueNumber);
    sortMenu->addSeparator();
    addSort(QStringLiteral("Token usage (highest first)"),
            QStringLiteral("agentSortTokens"), AgentListSort::TokenUsage);
    addSort(QStringLiteral("Cost (highest first)"),
            QStringLiteral("agentSortCost"), AgentListSort::Cost);
    addSort(QStringLiteral("Runtime (longest first)"),
            QStringLiteral("agentSortRuntime"), AgentListSort::Runtime);
    addSort(QStringLiteral("Files changed (most first)"),
            QStringLiteral("agentSortFiles"), AgentListSort::FilesChanged);
    connect(sortGroup, &QActionGroup::triggered, this, [this](QAction *action) {
        setAgentListSort(
            static_cast<AgentListSort>(action->data().toInt()));
    });
    m_agentSortButton->setMenu(sortMenu);

    m_agentTeammateFilterButton = railActionButton(
        QStringLiteral("people"), QStringLiteral("Special agents"),
        QStringLiteral("Show sessions started by special agents"));
    m_agentTeammateFilterButton->setCheckable(true);
    m_agentTeammateFilterButton->setObjectName(
        QStringLiteral("agentTeammateFilterButton"));
    m_agentTeammateFilterButton->setAccessibleDescription(
        QStringLiteral("Filter the Agents list by special agent attribution: "
                       "all sessions (the default), sessions no special agent "
                       "started, or only special agent sessions"));
    // Each click steps to the next filter; the small glyph over the button
    // says which of the three the list is showing.
    connect(m_agentTeammateFilterButton, &QPushButton::clicked, this, [this] {
        switch (m_agentTeammateFilter) {
        case AgentTeammateFilter::WithoutTeammates:
            setAgentTeammateFilter(AgentTeammateFilter::TeammatesOnly);
            break;
        case AgentTeammateFilter::TeammatesOnly:
            setAgentTeammateFilter(AgentTeammateFilter::All);
            break;
        case AgentTeammateFilter::All:
            setAgentTeammateFilter(AgentTeammateFilter::WithoutTeammates);
            break;
        }
    });

    m_agentViewButton = railActionButton(
        QStringLiteral("list-unordered"), QStringLiteral("Compact"),
        QStringLiteral("Change how the agent list shows each agent"));
    m_agentViewButton->setObjectName(QStringLiteral("agentViewButton"));
    connect(m_agentViewButton, &QPushButton::clicked, this, [this] {
        setAgentListView(forkmesh::ui::nextListViewMode(m_agentListView));
    });

    if (!m_cloudAgentRefreshHooked && m_repoDetailStack) {
        m_cloudAgentRefreshHooked = true;
        connect(m_repoDetailStack, &QStackedWidget::currentChanged, this,
                [this](int index) {
                    if (index == kRepoAgentsTab)
                        refreshCloudAgentSessions();
                });
    }

    agentQueueLayout->addWidget(m_agentStartAllButton);
    agentQueueLayout->addWidget(m_agentStopAllButton);
    agentQueueLayout->addWidget(m_agentUpdateAllButton);
    agentQueueLayout->addWidget(m_agentDeleteSelectedButton);
    agentQueueLayout->addWidget(m_agentEfficiencyButton);
    agentQueueLayout->addWidget(m_agentGuardrailsButton);
    agentQueueLayout->addWidget(m_agentModelsButton);
    agentQueueLayout->addWidget(m_agentAnnouncerButton);
    agentQueueLayout->addWidget(m_agentDeleteMergedButton);
    agentQueueLayout->addSpacing(8);
    agentQueueLayout->addWidget(m_agentUsageButton);
    agentQueueLayout->addStretch(1);
    m_agentMoveToDesktopButton = railActionButton(
        QStringLiteral("device-desktop"), QStringLiteral("To desktop"),
        QStringLiteral("Move every cloud run on the relay to this desktop"));
    m_agentMoveToDesktopButton->setObjectName(
        QStringLiteral("agentMoveToDesktopButton"));
    connect(m_agentMoveToDesktopButton, &QPushButton::clicked, this,
            [this] { moveQueuedCloudAgentsHere(); });
    agentQueueLayout->addWidget(m_agentMoveToDesktopButton);
    updateMoveToDesktopButton();
    agentQueueLayout->addWidget(m_agentViewButton);
    agentQueueLayout->addWidget(m_agentTeammateFilterButton);
    agentQueueLayout->addWidget(m_agentSortButton);
    agentQueueLayout->addWidget(m_agentQueueButton);
    setAgentListSort(AgentListSort::RecentActivity);
    setAgentTeammateFilter(m_agentTeammateFilter);
    setAgentListView(forkmesh::ui::loadListViewMode(
        QStringLiteral("agents/listView"), AgentListView::Compact));
    refreshCloudAgentSessions();
    refreshAgentToolbarAccountButtons();
    refreshAgentQueueControls();
    return agentToolBar;
}

// A strip across the top of the list, under the toolbar, while the open
// repository has deleted agents waiting in the recycle bin (issue #578).
QWidget *MainWindow::buildAgentRecycleBinBar()
{
    m_agentRecycleBinBar = new QWidget;
    m_agentRecycleBinBar->setObjectName(QStringLiteral("agentsActionBar"));
    m_agentRecycleBinBar->setAttribute(Qt::WA_StyledBackground);
    auto *row = new QHBoxLayout(m_agentRecycleBinBar);
    row->setContentsMargins(10, 3, 8, 3);
    row->setSpacing(6);

    m_agentRecycleBinLabel = new QLabel;
    m_agentRecycleBinLabel->setObjectName(QStringLiteral("agentRecycleBinLabel"));
    m_agentRecycleBinLabel->setTextFormat(Qt::PlainText);

    auto *showButton = new QPushButton(QStringLiteral("Show"));
    showButton->setObjectName(QStringLiteral("ghostButton"));
    showButton->setCursor(Qt::PointingHandCursor);
    showButton->setToolTip(
        QStringLiteral("List the deleted agents to restore or delete some of them"));
    setOcticon(showButton, QStringLiteral("trash"), 14);
    connect(showButton, &QPushButton::clicked, this,
            [this] { showAgentRecycleBinDialog(); });

    auto *restoreButton = new QPushButton(QStringLiteral("Restore all"));
    restoreButton->setObjectName(QStringLiteral("ghostButton"));
    restoreButton->setCursor(Qt::PointingHandCursor);
    restoreButton->setToolTip(
        QStringLiteral("Bring back every deleted agent of this repository, "
                       "with its transcript"));
    setOcticon(restoreButton, QStringLiteral("arrow-left"), 14);
    connect(restoreButton, &QPushButton::clicked, this,
            &MainWindow::restoreAllRecycledAgentsInOpenRepo);

    auto *deleteButton = new QPushButton(QStringLiteral("Delete all"));
    deleteButton->setObjectName(QStringLiteral("ghostButton"));
    deleteButton->setCursor(Qt::PointingHandCursor);
    deleteButton->setToolTip(
        QStringLiteral("Permanently delete every deleted agent of this "
                       "repository now, instead of at the end of the "
                       "retention window"));
    setOcticon(deleteButton, QStringLiteral("x"), 14);
    connect(deleteButton, &QPushButton::clicked, this,
            &MainWindow::deleteAllRecycledAgentsInOpenRepo);

    row->addWidget(m_agentRecycleBinLabel, 0, Qt::AlignVCenter);
    row->addStretch(1);
    row->addWidget(showButton);
    row->addWidget(restoreButton);
    row->addWidget(deleteButton);

    m_agentRecycleBinBar->hide();
    reloadAgentRecycleBin();
    return m_agentRecycleBinBar;
}

void MainWindow::reloadAgentRecycleBin()
{
    m_agentRecycled =
        m_agentStore ? m_agentStore->loadRecycledSessions() : QList<AgentSession>();
    updateAgentRecycleBinBar();
}

QList<AgentSession> MainWindow::agentRecycledInOpenRepo() const
{
    QList<AgentSession> sessions;
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return sessions;
    const auto &repo = m_repositories.at(m_repoDetailIndex);
    for (const AgentSession &session : m_agentRecycled)
        if (session.owner == repo.owner && session.name == repo.name)
            sessions.append(session);
    return sessions;
}

void MainWindow::updateAgentRecycleBinBar()
{
    if (!m_agentRecycleBinBar)
        return;
    const int count = agentRecycledInOpenRepo().size();
    if (count > 0) {
        m_agentRecycleBinLabel->setText(
            QStringLiteral("Recycle bin: %1 deleted agent%2, kept for %3 hours")
                .arg(count)
                .arg(count == 1 ? QString() : QStringLiteral("s"))
                .arg(agentRecycleRetentionHours()));
    }
    m_agentRecycleBinBar->setVisible(count > 0);
}

bool MainWindow::restoreRecycledAgentSession(int sessionId)
{
    if (!m_agentStore)
        return false;
    AgentSession binned;
    for (const AgentSession &session : m_agentStore->loadRecycledSessions()) {
        if (session.id == sessionId) {
            binned = session;
            break;
        }
    }
    if (!m_agentStore->restoreRecycledSession(sessionId))
        return false;
    if (binned.id == sessionId)
        restoreDeletedAgentBranch(binned);
    return true;
}

void MainWindow::restoreDeletedAgentBranch(const AgentSession &session)
{
    if (session.directCheckout || session.branchName.trimmed().isEmpty() ||
        isExternalSession(session.id))
        return;
    const int repoIndex = repoIndexFor(session.owner, session.name);
    if (repoIndex < 0)
        return;
    const QString repoPath = m_repositories.at(repoIndex).localPath;
    if (repoPath.isEmpty() ||
        !QFileInfo::exists(QDir(repoPath).filePath(QStringLiteral(".git"))))
        return;
    const int sid = session.id;
    const QString branch = session.branchName.trimmed();
    const QString worktreeName =
        session.worktreeName.isEmpty()
            ? forkmesh::agentwt::dirName(sid, session.issueTitle)
            : session.worktreeName;
    const QString wtRoot = forkmesh::vm::active()
                               ? forkmesh::vm::worktreeRoot()
                               : forkmesh::agentwt::root(repoPath);
    runOffThread<QPair<QString, QString>>(
        [repoPath, branch, worktreeName, wtRoot] {
            const forkmesh::BackgroundScope activity(
                QStringLiteral("agents"),
                QStringLiteral("restore deleted agent branch"));
            const QString tip =
                forkmesh::upstream::restoreDeletedBranch(repoPath, branch);
            if (tip.isEmpty())
                return qMakePair(QString(), QString());
            QString wtPath = readWorktreePathForBranch(repoPath, branch);
            if (wtPath.isEmpty()) {
                forkmesh::agentwt::ensureRoot(wtRoot);
                const QString target = QDir(wtRoot).filePath(worktreeName);
                if (!QFileInfo::exists(target) &&
                    runGitCapture(repoPath,
                                  {QStringLiteral("worktree"), QStringLiteral("add"),
                                   target, branch},
                                  nullptr, nullptr))
                    wtPath = target;
            }
            return qMakePair(tip, wtPath);
        },
        [this, sid, branch](QPair<QString, QString> result) {
            if (result.first.isEmpty())
                return;
            m_sessionWorkdirCache.remove(sid);
            logSystem(
                result.second.isEmpty()
                    ? QStringLiteral("Agents: restored branch %1 at %2 for "
                                     "agent #%3; its worktree could not be "
                                     "added back.")
                          .arg(branch, result.first.left(10))
                          .arg(sid)
                    : QStringLiteral("Agents: restored branch %1 at %2 and its "
                                     "worktree %3 for agent #%4.")
                          .arg(branch, result.first.left(10), result.second)
                          .arg(sid));
            loadWorktreesPanel();
            reloadAgents();
        });
}

void MainWindow::restoreAllRecycledAgentsInOpenRepo()
{
    if (!m_agentStore)
        return;
    const QList<AgentSession> binned = agentRecycledInOpenRepo();
    int restored = 0;
    for (const AgentSession &session : binned)
        if (restoreRecycledAgentSession(session.id))
            restored += 1;
    reloadAgentRecycleBin();
    if (restored > 0) {
        reloadAgents();
        scheduleAgentSessionsPush();
    }
    if (restored < binned.size()) {
        flashMessage(QStringLiteral("Could not restore %1 agent session(s); a "
                                    "live session may already use that id.")
                         .arg(binned.size() - restored),
                     true);
    } else if (restored > 0) {
        flashMessage(restored == 1
                         ? QStringLiteral("Agent session restored.")
                         : QStringLiteral("%1 agent sessions restored.")
                               .arg(restored));
    }
}

void MainWindow::deleteAllRecycledAgentsInOpenRepo()
{
    if (!m_agentStore)
        return;
    const QList<AgentSession> binned = agentRecycledInOpenRepo();
    if (binned.isEmpty())
        return;
    if (QMessageBox::question(
            this, QStringLiteral("Delete permanently"),
            QStringLiteral("Permanently delete all %1 deleted agent session(s) "
                           "of this repository? This cannot be undone.")
                .arg(binned.size())) != QMessageBox::Yes)
        return;
    for (const AgentSession &session : binned)
        m_agentStore->purgeRecycledSession(session.id);
    reloadAgentRecycleBin();
    flashMessage(QStringLiteral("Recycle bin emptied for this repository."));
}

// Opens a provider CLI (claude / codex) in a terminal dialog on the open
// repository, refusing while its agent guardrails are on.
void MainWindow::openAgentProviderTerminal(const QString &program,
                                           const QString &providerName,
                                           const QString &dialogObjectName,
                                           const QString &terminalObjectName)
{
    const QString repoPath = repoGitDir();
    if (repoPath.isEmpty()) {
        flashMessage(QStringLiteral("Open a repository before starting %1.")
                         .arg(providerName),
                     true, QString(),
                     forkmesh::kLocalOperationNoticeKind);
        return;
    }
    if (m_repoDetailIndex >= 0 &&
        m_repoDetailIndex < m_repositories.size() &&
        m_repositories.at(m_repoDetailIndex).agentGuardrailsEnabled) {
        flashMessage(
            QStringLiteral(
                "%1's free-form terminal is unavailable while this "
                "repository's agent guardrails are active. Start a "
                "managed Agent session, or turn guardrails off in "
                "Repository Settings.")
                .arg(providerName),
            true);
        return;
    }
    if (forkmesh::vm::active()) {
        const QString vmError = forkmesh::vm::availabilityError().isEmpty()
                                    ? forkmesh::vm::pathError(repoPath)
                                    : forkmesh::vm::availabilityError();
        if (!vmError.isEmpty()) {
            flashMessage(vmError, true);
            return;
        }
    }

    auto *dialog = new QDialog(this);
    dialog->setObjectName(dialogObjectName);
    if (m_repoDetailIndex >= 0 &&
        m_repoDetailIndex < m_repositories.size()) {
        const RepositoryRecord &repo =
            m_repositories.at(m_repoDetailIndex);
        dialog->setProperty("forkmeshRepoOwner", repo.owner);
        dialog->setProperty("forkmeshRepoName", repo.name);
    }
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QStringLiteral("%1 terminal").arg(providerName));
    dialog->resize(900, 560);
    auto *layout = new QVBoxLayout(dialog);
    auto *notice = new QLabel(
        QStringLiteral("<b>%1</b> is running %2 in <code>%3</code>.")
            .arg(providerName.toHtmlEscaped(),
                 forkmesh::vm::active()
                     ? QStringLiteral("inside the KVM guest")
                     : QStringLiteral("on this host"),
                 repoPath.toHtmlEscaped()),
        dialog);
    notice->setWordWrap(true);
    layout->addWidget(notice);
    auto *terminal = new TerminalWidget(dialog);
    terminal->setObjectName(terminalObjectName);
    layout->addWidget(terminal, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    layout->addWidget(buttons);
    dialog->show();
    dialog->raise();
    const QString command =
        forkmesh::vm::active()
            ? forkmesh::vm::interactiveCommand(program, repoPath)
            : program;
    terminal->runCommand(command, repoPath, {}, !forkmesh::vm::active());
    terminal->setFocus();
}

// The detail pane's per-session action buttons (stop, continue, grade, PR,
// issue, task, cloud, branch, worktree) and its status pill.
void MainWindow::buildAgentDetailActionButtons()
{
    m_agentStopButton = railActionButton(QStringLiteral("circle-slash"),
                                         QStringLiteral("Stop"),
                                         "Stop this session's running agent");
    connect(m_agentStopButton, &QPushButton::clicked, this, [this] {
        if (isCloudSession(m_selectedAgentSessionId)) {
            stopCloudAgentRun(m_selectedAgentSessionId);
            return;
        }
        if (isExternalSession(m_selectedAgentSessionId)) {
            stopExternalSession(m_selectedAgentSessionId);
            return;
        }
        if (AgentRunner *runner = runnerForSession(m_selectedAgentSessionId))
            runner->stop();
        stopStreamSession(m_selectedAgentSessionId);
    });

    m_agentStartButton = railActionButton(
        QStringLiteral("play"), QStringLiteral("Continue"),
        "Resume this session with the agent, model and mode currently chosen in "
        "the composer");
    m_agentStartButton->setObjectName("agentContinueButton");
    m_agentStartButton->hide();
    connect(m_agentStartButton, &QPushButton::clicked, this, [this] {
        if (isCloudSession(m_selectedAgentSessionId)) {
            sendCloudAgentFollowUp(m_selectedAgentSessionId,
                                   continueWhereLeftOffPrompt());
            return;
        }
        if (const AgentSession *held = findAgentSession(m_selectedAgentSessionId);
            held && held->awaitingStart()) {
            startHeldAgentSession(m_selectedAgentSessionId);
            return;
        }
        applyComposerSelectionToAgentSession(m_selectedAgentSessionId);
        continueSelectedAgentSession();
    });

    m_agentTaskGradeCombo = new QComboBox;
    m_agentTaskGradeCombo->setObjectName(
        QStringLiteral("agentTeammateTaskGrade"));
    m_agentTaskGradeCombo->setAccessibleName(
        QStringLiteral("Grade finished special agent task"));
    m_agentTaskGradeCombo->setToolTip(
        QStringLiteral("Rate a finished special agent task. The rating appears in "
                       "the Special agent performance chart."));
    m_agentTaskGradeCombo->addItem(QStringLiteral("Grade task"), 0);
    m_agentTaskGradeCombo->addItem(QStringLiteral("1 · Needs work"), 1);
    m_agentTaskGradeCombo->addItem(QStringLiteral("2 · Below expectations"), 2);
    m_agentTaskGradeCombo->addItem(QStringLiteral("3 · Meets expectations"), 3);
    m_agentTaskGradeCombo->addItem(QStringLiteral("4 · Strong"), 4);
    m_agentTaskGradeCombo->addItem(QStringLiteral("5 · Excellent"), 5);
    m_agentTaskGradeCombo->hide();
    connect(m_agentTaskGradeCombo, &QComboBox::currentIndexChanged, this,
            [this](int index) {
                if (!m_agentTaskGradeCombo)
                    return;
                setTeammateTaskGrade(
                    m_selectedAgentSessionId,
                    m_agentTaskGradeCombo->itemData(index).toInt());
            });

    m_agentDeleteAllButton = railActionButton(
        QStringLiteral("trash"), QStringLiteral("Delete"),
        "Delete this agent session and any attached unmerged pull request, along "
        "with its worktree folder and branch when it still has them; merged pull "
        "requests are retained");
    connect(m_agentDeleteAllButton, &QPushButton::clicked, this,
            [this] { deleteAgentSessionAndCleanup(m_selectedAgentSessionId); });

    m_agentViewPrButton = railActionButton(
        QStringLiteral("git-pull-request"), QStringLiteral("PR"),
        "Open this session's pull request");
    m_agentViewPrButton->setObjectName(QStringLiteral("agentViewPrButton"));
    m_agentViewPrButton->hide();
    connect(m_agentViewPrButton, &QPushButton::clicked, this, [this] {
        const AgentSession *s = findAgentSession(m_selectedAgentSessionId);
        if (!s || s->prNumber <= 0)
            return;
        const int number = s->prNumber;
        const int repoIndex = repoIndexFor(s->owner, s->name);
        if (repoIndex >= 0 && bindRepoDetailToRepo(repoIndex))
            switchToPullTab(number);
    });

    m_agentViewIssueButton = railActionButton(
        QStringLiteral("issue-opened"), QStringLiteral("Issue"),
        "Open the issue this session was started from");
    m_agentViewIssueButton->setObjectName(QStringLiteral("agentViewIssueButton"));
    m_agentViewIssueButton->hide();
    connect(m_agentViewIssueButton, &QPushButton::clicked, this, [this] {
        const AgentSession *s = findAgentSession(m_selectedAgentSessionId);
        if (!s || s->issueNumber <= 0)
            return;
        const int number = s->issueNumber;
        const QString repoLabel = QStringLiteral("%1/%2").arg(s->owner, s->name);
        const int repoIndex = repoIndexFor(s->owner, s->name);
        if (repoIndex >= 0 && bindRepoDetailToRepo(repoIndex))
            openIssueReference(number);
        else // never leave the link dead: say why it went nowhere
            flashMessage(QStringLiteral("Issue #%1 belongs to %2, which is not "
                                        "open here.")
                             .arg(number)
                             .arg(repoLabel),
                         true);
    });

    m_agentViewTaskButton = railActionButton(
        QStringLiteral("checklist"), QStringLiteral("Task"),
        "Open the organization task linked to this agent session");
    m_agentViewTaskButton->setObjectName(QStringLiteral("agentViewTaskButton"));
    m_agentViewTaskButton->hide();
    connect(m_agentViewTaskButton, &QPushButton::clicked, this, [this] {
        const AgentSession *session =
            findAgentSession(m_selectedAgentSessionId);
        if (session && !session->orgTaskId.isEmpty())
            openOrganizationTask(session->orgTaskId);
        else
            showAgentTaskLinkMenu(m_selectedAgentSessionId);
    });

    m_agentCreatePrButton = railActionButton(
        QStringLiteral("git-pull-request"), QStringLiteral("Create PR"),
        "Open a pull request from this session's branch (its diff since the "
        "base commit, committed or not)");
    m_agentCreatePrButton->hide();
    connect(m_agentCreatePrButton, &QPushButton::clicked, this, [this] {
        const int sid = m_selectedAgentSessionId;
        if (isCloudSession(sid)) {
            createCloudAgentPull(sid);
            return;
        }
        AgentSession *s = findAgentSession(sid);
        if (!s || s->branchName.isEmpty())
            return;
        if (s->prNumber > 0) {
            flashMessage(QStringLiteral("This session already has PR #%1.")
                             .arg(s->prNumber));
            return;
        }
        const QString branch = s->branchName;
        const int ri = repoIndexFor(s->owner, s->name);
        if (ri < 0)
            return;
        // Only ever diff the session's own worktree — resolving to the shared
        // checkout is exactly how sessions bled into each other.
        // cachedSessionWorktree can shell git, which pumps the event loop, so
        // `s` is not touched after this point (git-pump UAF family).
        const QString wt =
            cachedSessionWorktree(sid, m_repositories.at(ri).localPath, branch);
        if (wt.isEmpty() || !QDir(wt).exists()) {
            flashMessage(
                QStringLiteral("This session's worktree is gone — there is no "
                               "isolated tree left to diff into a PR."),
                true);
            return;
        }
        // cachedSessionWorktree() may have rebuilt the session list while git
        // was running, so re-find before persisting this explicit opt-in.
        AgentSession *requested = findAgentSession(sid);
        if (!requested)
            return;
        requested->createPr = true;
        m_agentStore->saveSession(*requested);
        maybeCreatePullForStreamSession(sid, [this, sid] {
            if (const AgentSession *after = findAgentSession(sid);
                after && after->prNumber > 0)
                flashMessage(QStringLiteral("Created pull request #%1.")
                                 .arg(after->prNumber));
            else
                flashMessage(QStringLiteral(
                    "No local pull request was created — see the session log."));
            reloadAgents();
            if (sid == m_selectedAgentSessionId)
                showAgentSession(sid);
        });
    });

    m_agentCreateIssueButton = railActionButton(
        QStringLiteral("issue-opened"), QStringLiteral("+ issue"),
        "Create a tracked issue from this run and link it to this session");
    m_agentCreateIssueButton->hide();
    connect(m_agentCreateIssueButton, &QPushButton::clicked, this,
            &MainWindow::createLinkedIssueForSelectedSession);

    m_agentMoveToCloudButton = railActionButton(
        QStringLiteral("cloud"), QStringLiteral("to cloud"),
        "Continue this session in the cloud: commit and publish its branch, "
        "queue the continuation for one of this account's mirror nodes, and "
        "stop running it here");
    m_agentMoveToCloudButton->setObjectName(
        QStringLiteral("agentMoveToCloudButton"));
    m_agentMoveToCloudButton->hide();
    connect(m_agentMoveToCloudButton, &QPushButton::clicked, this,
            [this] { moveAgentSessionToCloud(m_selectedAgentSessionId); });

    // The other direction, offered on a cloud row: a run the website queued
    // and no node has picked up can be claimed and run here instead.
    m_agentContinueHereButton = railActionButton(
        QStringLiteral("device-desktop"), QStringLiteral("Continue here"),
        "Continue this queued cloud agent on this desktop: claim it from the "
        "relay and run it in this machine's checkout");
    m_agentContinueHereButton->setObjectName(
        QStringLiteral("agentContinueHereButton"));
    m_agentContinueHereButton->hide();
    connect(m_agentContinueHereButton, &QPushButton::clicked, this,
            [this] { continueCloudAgentHere(m_selectedAgentSessionId); });
    m_agentSignInRunnerButton = railActionButton(
        QStringLiteral("key"), QStringLiteral("Sign in runner"),
        "Sign the runner's agent CLI in from this desktop: the provider's own "
        "login runs on the runner and its terminal is relayed here");
    m_agentSignInRunnerButton->setObjectName(
        QStringLiteral("agentSignInRunnerButton"));
    m_agentSignInRunnerButton->hide();
    connect(m_agentSignInRunnerButton, &QPushButton::clicked, this,
            [this] { signInRunnerForCloudAgent(m_selectedAgentSessionId); });
    m_agentRetryInCloudButton = railActionButton(
        QStringLiteral("sync"), QStringLiteral("Retry in cloud"),
        "Queue this run's original prompt again for the account's cloud "
        "agent nodes");
    m_agentRetryInCloudButton->setObjectName(
        QStringLiteral("agentRetryInCloudButton"));
    m_agentRetryInCloudButton->hide();
    connect(m_agentRetryInCloudButton, &QPushButton::clicked, this,
            [this] { retryCloudAgentInCloud(m_selectedAgentSessionId); });
    m_agentCloudMergeButton = railActionButton(
        QStringLiteral("check-circle"), QStringLiteral("Merge & clean up"),
        "Merge this cloud run's branch into its base on ForkMesh, then remove "
        "the branch");
    m_agentCloudMergeButton->setObjectName(
        QStringLiteral("agentCloudMergeButton"));
    m_agentCloudMergeButton->hide();
    connect(m_agentCloudMergeButton, &QPushButton::clicked, this, [this] {
        forkmesh::sonify::UiSounds::play(forkmesh::sonify::Cue::Merge);
        mergeCloudAgentBranch(m_selectedAgentSessionId);
    });
    m_agentCloudCleanupButton = railActionButton(
        QStringLiteral("trash"), QStringLiteral("Clean up"),
        "Remove this cloud run's merged branch from every core that still "
        "holds it");
    m_agentCloudCleanupButton->setObjectName(
        QStringLiteral("agentCloudCleanupButton"));
    m_agentCloudCleanupButton->hide();
    connect(m_agentCloudCleanupButton, &QPushButton::clicked, this,
            [this] { cleanUpCloudAgentBranch(m_selectedAgentSessionId); });
    m_agentCloudUpdateButton = railActionButton(
        QStringLiteral("download"), QStringLiteral("Pull main"),
        "Merge the base branch into this cloud run's branch on ForkMesh");
    m_agentCloudUpdateButton->setObjectName(
        QStringLiteral("agentCloudUpdateButton"));
    m_agentCloudUpdateButton->hide();
    connect(m_agentCloudUpdateButton, &QPushButton::clicked, this,
            [this] { updateCloudAgentBranch(m_selectedAgentSessionId); });
    m_agentCloudActionsButton = railActionButton(
        QStringLiteral("workflow"), QStringLiteral("Actions"),
        "Open the Actions the merge started on the base branch");
    m_agentCloudActionsButton->setObjectName(
        QStringLiteral("agentCloudActionsButton"));
    m_agentCloudActionsButton->hide();
    connect(m_agentCloudActionsButton, &QPushButton::clicked, this,
            [this] { openCloudAgentActions(m_selectedAgentSessionId); });

    m_agentStatusPill = new QToolButton;
    m_agentStatusPill->setObjectName("agentStatusPill");
    m_agentStatusPill->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_agentStatusPill->setIconSize(
        QSize(kAgentStatusPillIconPx, kAgentStatusPillIconPx));
    m_agentStatusPill->setFixedHeight(kAgentStatusPillIconPx + 6);
    m_agentStatusPill->setCursor(Qt::PointingHandCursor);
    m_agentStatusPill->installEventFilter(this);
    connect(m_agentStatusPill, &QToolButton::clicked, this,
            &MainWindow::toggleAgentMetaPopup);

    auto *branchButton = railActionButton(
        QStringLiteral("git-branch"), QStringLiteral("Branch"),
        "Open this session's branch in the Git view");
    branchButton->setAccentTint(true);
    m_agentBranchButton = branchButton;
    m_agentBranchButton->hide(); // shown per-session in refreshAgentDetailMeta
    connect(m_agentBranchButton, &QPushButton::clicked, this,
            [this] { switchToAgentBranch(m_selectedAgentSessionId); });
    m_agentWorktreeButton = railActionButton(
        QStringLiteral("worktree"), QStringLiteral("Worktree"),
        "Open this session's worktree in the Worktrees tab");
    m_agentWorktreeButton->hide();
    connect(m_agentWorktreeButton, &QPushButton::clicked, this, [this] {
        if (const AgentSession *s = findAgentSession(m_selectedAgentSessionId);
            s && !s->branchName.isEmpty())
            switchToWorktree(s->branchName);
    });

    m_agentSyncLabel = new QLabel;
    m_agentSyncLabel->setObjectName(QStringLiteral("agentSyncLabel"));
    m_agentSyncLabel->setTextFormat(Qt::RichText);
    m_agentSyncLabel->hide();
    m_agentSyncButton = railActionButton(QStringLiteral("download"),
                                         QStringLiteral("Pull main"), QString());
    m_agentSyncButton->setObjectName(QStringLiteral("agentSyncButton"));
    m_agentSyncButton->hide();
    connect(m_agentSyncButton, &QPushButton::clicked, this,
            [this] { pullBaseIntoAgentBranch(m_selectedAgentSessionId); });
}

void MainWindow::refreshAgentSyncControls(int sessionId)
{
    const auto pullBusy = [this](const QString &worktree) {
        return m_branchMergeFlowBusy || m_worktreeMergeInProgress ||
               (!worktree.isEmpty() && m_worktreeGitBusy.contains(worktree));
    };
    if (m_agentSyncPullPending) {
        if (pullBusy(m_agentSyncPullWorktree)) {
            if (!m_agentSyncPollQueued) {
                m_agentSyncPollQueued = true;
                QTimer::singleShot(400, this, [this] {
                    m_agentSyncPollQueued = false;
                    refreshAgentSyncControls(m_selectedAgentSessionId);
                });
            }
        } else {
            m_agentSyncPullPending = false;
            m_agentSyncPullWorktree.clear();
            m_agentDiffRefreshPending = true;
            QTimer::singleShot(0, this, [this] { refreshAgentTable(); });
        }
    }
    if (!m_agentSyncLabel || !m_agentSyncButton)
        return;
    const AgentSession *s = findAgentSession(sessionId);
    const auto stat = m_agentDiffStats.constFind(sessionId);
    const bool known = s && !s->cloud && !s->merged && !s->branchName.isEmpty() &&
                       stat != m_agentDiffStats.constEnd() && stat->ahead >= 0 &&
                       stat->behind >= 0 &&
                       repoIndexFor(s->owner, s->name) == m_repoDetailIndex;
    if (!known) {
        m_agentSyncLabel->hide();
        m_agentSyncButton->hide();
        return;
    }
    const QString base = stat->base.isEmpty() ? agentMergeBase(*s) : stat->base;
    const bool dark = currentThemeIsDark();
    const QString muted = QStringLiteral("#8b949e");
    const QString warn = dark ? QStringLiteral("#d29922") : QStringLiteral("#9a6700");
    const QString danger = dark ? QStringLiteral("#f85149") : QStringLiteral("#cf222e");
    const QString dot = QString::fromUtf8(" \xC2\xB7 ");
    QString plain;
    QString html;
    if (stat->ahead == 0 && stat->behind == 0) {
        plain = QStringLiteral("up to date with %1").arg(base);
        html = QStringLiteral("<span style='color:%1'>%2</span>")
                   .arg(muted, plain.toHtmlEscaped());
    } else {
        QStringList plainParts;
        QStringList htmlParts;
        if (stat->ahead > 0) {
            plainParts << QString::fromUtf8("\xE2\x86\x91%1 ahead").arg(stat->ahead);
            htmlParts << plainParts.last().toHtmlEscaped();
        }
        if (stat->behind > 0) {
            plainParts << QString::fromUtf8("\xE2\x86\x93%1 behind").arg(stat->behind);
            htmlParts << QStringLiteral("<span style='color:%1'>%2</span>")
                             .arg(warn, plainParts.last().toHtmlEscaped());
        }
        const QString of = stat->behind > 0 ? QStringLiteral(" %1").arg(base)
                                            : QStringLiteral(" of %1").arg(base);
        plain = plainParts.join(dot) + of;
        html = htmlParts.join(dot.toHtmlEscaped()) +
               QStringLiteral("<span style='color:%1'>%2</span>")
                   .arg(muted, of.toHtmlEscaped());
    }
    if (stat->conflicted) {
        plain += dot + QStringLiteral("conflicts");
        html += dot.toHtmlEscaped() +
                QStringLiteral("<span style='color:%1'>conflicts</span>").arg(danger);
    }
    m_agentSyncLabel->setText(html);
    m_agentSyncLabel->setAccessibleName(plain);
    m_agentSyncLabel->setToolTip(
        (stat->conflicted
             ? QStringLiteral("%1 is %2 commit(s) ahead of %3 and %4 behind. "
                              "Merging %3 into it conflicts.")
             : QStringLiteral("%1 is %2 commit(s) ahead of %3 and %4 behind."))
            .arg(s->branchName)
            .arg(stat->ahead)
            .arg(base)
            .arg(stat->behind));
    m_agentSyncLabel->show();

    const QString worktree = stat->worktree;
    const bool external = isExternalSession(sessionId);
    const bool agentFix = stat->conflicted && !external;
    const bool editorFix = stat->conflicted && external && !worktree.isEmpty();
    const bool pull = !stat->conflicted && stat->behind > 0;
    if (!agentFix && !editorFix && !pull) {
        m_agentSyncButton->hide();
        return;
    }
    const QString caption = stat->conflicted ? QStringLiteral("Fix conflicts")
                                             : QStringLiteral("Pull %1").arg(base);
    m_agentSyncButton->setGlyph(stat->conflicted ? QStringLiteral("alert")
                                                 : QStringLiteral("download"),
                                caption);
    m_agentSyncButton->setAccessibleName(caption);
    fitRailActionButton(m_agentSyncButton, caption);
    m_agentSyncButton->setAlertTint(stat->conflicted);
    m_agentSyncButton->setBadgeCount(stat->behind);
    const bool busy = !agentFix && pullBusy(worktree);
    m_agentSyncButton->setEnabled(!busy);
    m_agentSyncButton->setToolTip(
        busy ? QString::fromUtf8("A merge is already running \xE2\x80\x94 wait "
                                 "for it to finish")
        : agentFix
            ? QStringLiteral("Ask this agent to merge %1 into %2 and resolve the "
                             "conflicts, then commit")
                  .arg(base, s->branchName)
        : editorFix
            ? QStringLiteral("Merge %1 into %2 and resolve the conflicts in the "
                             "merge editor")
                  .arg(base, s->branchName)
            : QStringLiteral("Merge %1 into %2 (%3 commit(s) behind)")
                  .arg(base, s->branchName)
                  .arg(stat->behind));
    m_agentSyncButton->show();
}

void MainWindow::pullBaseIntoAgentBranch(int sessionId)
{
    const AgentSession *s = findAgentSession(sessionId);
    const auto stat = m_agentDiffStats.constFind(sessionId);
    if (!s || s->branchName.isEmpty() || stat == m_agentDiffStats.constEnd())
        return;
    if (stat->conflicted &&
        (isCloudSession(sessionId) || !isExternalSession(sessionId))) {
        fixAgentConflictsWithAgent(sessionId);
        return;
    }
    const QString base = stat->base.isEmpty() ? agentMergeBase(*s) : stat->base;
    const QString branch = s->branchName;
    const QString worktree = stat->worktree;
    if (!stat->conflicted && base == repoDefaultBranchFast()) {
        updateBranchFromBase(branch);
    } else if (!worktree.isEmpty()) {
        updateWorktreeFromMain(worktree, branch, base, [this, sessionId] {
            if (m_selectedAgentSessionId == sessionId && findAgentSession(sessionId))
                refreshAgentFilesPanel(sessionId);
        });
    } else {
        flashMessage(QStringLiteral("%1 has no worktree on this machine to merge "
                                    "%2 into.")
                         .arg(branch, base),
                     true);
        return;
    }
    m_agentSyncPullPending = true;
    m_agentSyncPullWorktree = worktree;
    refreshAgentSyncControls(sessionId);
}

#ifdef FORKMESH_WINDOW_TESTS
QString MainWindow::testAgentSyncControls(int sessionId, const AgentDiffStat &stat)
{
    const auto previous = m_agentDiffStats.constFind(sessionId);
    const bool hadPrevious = previous != m_agentDiffStats.constEnd();
    const AgentDiffStat saved = hadPrevious ? previous.value() : AgentDiffStat();
    m_agentDiffStats.insert(sessionId, stat);
    refreshAgentSyncControls(sessionId);
    const QString result =
        QStringLiteral("%1|%2|%3")
            .arg(m_agentSyncLabel && !m_agentSyncLabel->isHidden()
                     ? m_agentSyncLabel->accessibleName()
                     : QString(),
                 m_agentSyncButton && !m_agentSyncButton->isHidden()
                     ? m_agentSyncButton->accessibleName()
                     : QString())
            .arg(m_agentSyncButton && !m_agentSyncButton->isHidden()
                     ? m_agentSyncButton->badgeCount()
                     : -1);
    if (hadPrevious)
        m_agentDiffStats.insert(sessionId, saved);
    else
        m_agentDiffStats.remove(sessionId);
    refreshAgentSyncControls(m_selectedAgentSessionId);
    return result;
}
#endif

// The output surfaces behind the Raw/Code toggles: raw log, embedded terminal,
// rich transcript, plus the transcript search fields parented to detailPane.
void MainWindow::buildAgentOutputViews(QWidget *detailPane)
{
    m_agentLog = new QPlainTextEdit;
    m_agentLog->setReadOnly(true);
    m_agentLog->setObjectName("actionLog");
    applyLogFont(m_agentLog);
    new AgentLogHighlighter(m_agentLog->document());
    m_agentLog->setMaximumBlockCount(30000);
    auto *rawJump = new ScrollJumpButtons(m_agentLog);
    connect(rawJump, &ScrollJumpButtons::topClicked, this, [this] {
        if (m_agentLog)
            m_agentLog->verticalScrollBar()->setValue(0);
    });
    connect(rawJump, &ScrollJumpButtons::bottomClicked, this, [this] {
        if (m_agentLog)
            m_agentLog->verticalScrollBar()->setValue(
                m_agentLog->verticalScrollBar()->maximum());
    });

    m_agentTerminal = new TerminalWidget;
    connect(m_agentTerminal, &TerminalWidget::finished, this, [this](int) {
        if (m_terminalSessionId <= 0)
            return;
        const int sid = m_terminalSessionId; // reloadAgents() below dangles `s`
        bool finished = false;
        if (AgentSession *s = findAgentSession(sid)) {
            if (s->status == AgentStatus::Running) {
                s->status = AgentStatus::Success;
                s->finishedAtMs = QDateTime::currentMSecsSinceEpoch();
                m_agentStore->saveSession(*s);
                scheduleAgentSessionsPush();
                reloadAgents();
                finished = true;
            }
        }
        if (finished) {
            maybeAutoMergeForSession(sid);
            completeOrgTaskForSession(sid);
        }
    });
    m_agentTranscript = new ClaudeTranscriptView;
    m_agentTranscript->setSplitDiffs(
        QSettings().value(kClaudeDiffSplitSetting, false).toBool());
    m_agentTranscript->setTerminalOutputMode(
        QSettings().value(kTerminalTranscriptOutputSetting, true).toBool());
    m_agentTranscript->setMinimumHeight(320); // never collapse to a thin strip
    m_agentTranscript->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    connect(m_agentTranscript, &ClaudeTranscriptView::referenceActivated, this,
            &MainWindow::openAgentTranscriptReference);
    connect(m_agentTranscript, &ClaudeTranscriptView::commandRunRequested, this,
            &MainWindow::openAgentCommandTerminal);
    installAgentCommandApprovals();
    connect(m_agentTranscript, &ClaudeTranscriptView::loadEarlierRequested, this,
            &MainWindow::loadEarlierTranscriptEvents);
    connect(m_agentTranscript, &ClaudeTranscriptView::questionAnswered, this,
            [this](const QString &toolUseId, const QString &answer,
                   bool sensitive) {
                answerAgentQuestion(m_selectedAgentSessionId, toolUseId, answer,
                                    sensitive);
            });
    connect(m_agentTranscript, &ClaudeTranscriptView::inlineChoiceAnswered, this,
            [this](const QString &answer) { sendPromptToSelectedAgent(answer); });
    connect(m_agentTranscript, &ClaudeTranscriptView::rawLogRequested, this,
            &MainWindow::showAgentRawLogEvent);

    m_agentOutputStack = new QStackedWidget;
    m_agentOutputStack->addWidget(m_agentLog);        // page 0: raw / piped log
    m_agentOutputStack->addWidget(m_agentTerminal);   // page 1: embedded terminal
    m_agentOutputStack->addWidget(m_agentTranscript); // page 2: rich transcript

    m_agentOutputModeButton =
        railActionButton(QStringLiteral("terminal"), QStringLiteral("Raw"),
                         "Show this run's raw, unformatted output");
    m_agentOutputModeButton->setObjectName(QStringLiteral("agentOutputModeButton"));
    {
        QFont f = QGuiApplication::font();
        f.setPixelSize(10);
        f.setWeight(QFont::DemiBold);
        m_agentOutputModeButton->setFixedSize(
            qMax(railItemWidth(),
                 QFontMetrics(f).horizontalAdvance(QStringLiteral("Transcript"))
                     + 12),
            kRailItemHeight);
    }
    connect(m_agentOutputModeButton, &QPushButton::clicked, this,
            [this] { setAgentRawOutputMode(!m_agentRawOutputMode); });

    m_agentListenButton =
        railActionButton(QStringLiteral("unmute"), QStringLiteral("Listen"),
                         "Play this run's transcript as music");
    m_agentListenButton->setObjectName(QStringLiteral("agentListenButton"));
    m_agentListenButton->setCheckable(true);
    {
        // Fixed to the wider of the two captions, so switching it on does not
        // shuffle the row of buttons beside it.
        QFont f = QGuiApplication::font();
        f.setPixelSize(10);
        f.setWeight(QFont::DemiBold);
        m_agentListenButton->setFixedSize(
            qMax(railItemWidth(),
                 QFontMetrics(f).horizontalAdvance(QStringLiteral("Listening")) + 12),
            kRailItemHeight);
    }
    connect(m_agentListenButton, &QPushButton::clicked, this,
            [this] { toggleAgentListening(m_selectedAgentSessionId); });

    m_agentCodeButton =
        railActionButton(QStringLiteral("code"), QStringLiteral("Code"),
                         "Show this run's file edits as a live diff");
    m_agentCodeButton->setObjectName(QStringLiteral("agentCodeButton"));
    m_agentCodeButton->setCheckable(true);
    connect(m_agentCodeButton, &QPushButton::clicked, this,
            [this] { setAgentCodeMode(!m_agentCodeMode); });

    m_transcriptSearch = new QLineEdit(detailPane);
    m_transcriptSearch->hide();
    m_transcriptSearchCount = new QLabel(detailPane);
    m_transcriptSearchCount->hide();
    connect(m_transcriptSearch, &QLineEdit::textChanged, this,
            [this](const QString &t) {
                if (!m_agentTranscript)
                    return;
                const QString q = t.trimmed();
                if (q.isEmpty())
                    m_agentTranscript->clearSearch();
                else
                    m_agentTranscript->search(q);
            });
    connect(m_transcriptSearch, &QLineEdit::returnPressed, this, [this] {
        if (m_agentTranscript)
            m_agentTranscript->searchNext();
    });
    connect(m_agentTranscript, &ClaudeTranscriptView::searchResultsChanged, this,
            [this](int current, int total) {
                if (!m_transcriptSearchCount)
                    return;
                const bool empty = !m_transcriptSearch
                                   || m_transcriptSearch->text().trimmed().isEmpty();
                m_transcriptSearchCount->setText(
                    empty ? QString()
                          : QStringLiteral("%1/%2").arg(current).arg(total));
            });
}

// The Code page's files/commits list, diff view and worktree action buttons.
void MainWindow::buildAgentFilesPanel()
{
    m_agentFilesList = new QListWidget;
    m_agentFilesList->setObjectName("agentFilesList");
    m_agentFilesList->setMinimumWidth(190);
    m_agentFilesList->setItemDelegate(
        new SelectionBorderRowDelegate(m_agentFilesList));
    connect(m_agentFilesList, &QListWidget::itemActivated, this,
            [](QListWidgetItem *it) {
                const QString path = it->data(Qt::UserRole).toString();
                if (!path.isEmpty())
                    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
            });

    m_agentCommitsHeading = new QLabel;
    m_agentCommitsHeading->setObjectName("agentFilesHeading");
    m_agentCommitsList = new QListWidget;
    m_agentCommitsList->setObjectName("agentCommitsList");
    m_agentCommitsList->setMinimumWidth(190);
    m_agentCommitsList->setMaximumHeight(120);
    m_agentCommitsList->setSelectionMode(QAbstractItemView::NoSelection);
    m_agentCommitsList->setFocusPolicy(Qt::NoFocus);
    m_agentCommitsHeading->setVisible(false); // shown once a render finds commits
    m_agentCommitsList->setVisible(false);

    auto *filesV = new QVBoxLayout;
    filesV->setContentsMargins(0, 0, 0, 0);
    filesV->setSpacing(4);
    m_agentFilesChangedSummary = new QLabel;
    m_agentFilesChangedSummary->setObjectName("agentFilesHeading");
    m_agentFilesChangedSummary->setTextFormat(Qt::RichText);
    filesV->addWidget(m_agentFilesChangedSummary);
    filesV->addWidget(m_agentCommitsHeading);
    filesV->addWidget(m_agentCommitsList);
    filesV->addWidget(m_agentFilesList, 1);
    m_agentFilesPanel = new QWidget;
    m_agentFilesPanel->setLayout(filesV);

    m_agentDiffView = new QTextBrowser;
    m_agentDiffView->setObjectName("diffView");
    m_agentDiffView->setOpenExternalLinks(false);
    registerDiffView(m_agentDiffView);
    connect(m_agentDiffView, &QTextBrowser::anchorClicked, this,
            &MainWindow::onAgentCodeAnchorClicked);

    m_agentUpdateButton = railActionButton(
        QStringLiteral("sync"), QStringLiteral("Update"),
        "Merge the default branch into this session's worktree branch");
    connect(m_agentUpdateButton, &QPushButton::clicked, this, [this] {
        AgentSession *s = findAgentSession(m_selectedAgentSessionId);
        if (!s || s->branchName.isEmpty())
            return;
        const int ri = repoIndexFor(s->owner, s->name);
        if (ri < 0)
            return;
        const int sessionId = m_selectedAgentSessionId;
        updateWorktreeFromMain(
            worktreePathForBranch(m_repositories.at(ri).localPath, s->branchName),
            s->branchName, agentMergeBase(*s), [this, sessionId] {
                if (m_selectedAgentSessionId == sessionId &&
                    findAgentSession(sessionId))
                    refreshAgentFilesPanel(sessionId);
            });
    });
    m_agentMergeButton = railActionButton(
        QStringLiteral("check-circle"), QStringLiteral("Merge"),
        "Merge this session's branch into the default branch, then delete the "
        "worktree and its branch");
    connect(m_agentMergeButton, &QPushButton::clicked, this, [this] {
        AgentSession *s = findAgentSession(m_selectedAgentSessionId);
        if (!s || s->branchName.isEmpty())
            return;
        const int ri = repoIndexFor(s->owner, s->name);
        if (ri < 0)
            return;
        mergeAgentBranchIntoBase(ri, s->branchName, /*cleanUp=*/false);
    });
    m_agentMergeDeleteButton = railActionButton(
        QStringLiteral("check-circle"), QStringLiteral("Merge & clean"),
        "Merge this session's branch into the default branch, then delete the "
        "worktree, source branch, pull request and Agent record");
    connect(m_agentMergeDeleteButton, &QPushButton::clicked, this, [this] {
        AgentSession *s = findAgentSession(m_selectedAgentSessionId);
        if (!s || s->branchName.isEmpty())
            return;
        const int ri = repoIndexFor(s->owner, s->name);
        if (ri < 0)
            return;
        forkmesh::sonify::UiSounds::play(forkmesh::sonify::Cue::Merge);
        mergeAgentBranchIntoBase(ri, s->branchName, /*cleanUp=*/true);
    });
    m_agentWtDeleteButton = railActionButton(
        QStringLiteral("trash"), QStringLiteral("Delete"),
        "Remove this session's worktree, branch, attached pull request and agent "
        "session");
    connect(m_agentWtDeleteButton, &QPushButton::clicked, this, [this] {
        AgentSession *s = findAgentSession(m_selectedAgentSessionId);
        if (!s || s->branchName.isEmpty())
            return;
        const int ri = repoIndexFor(s->owner, s->name);
        if (ri < 0)
            return;
        // Same cross-repo bind the merge buttons need: deleteWorktreeBranchAndAgent
        // resolves the checkout from the repo the detail view holds, not from the
        // session, and the Agents tab is global. Snapshot before the bind — it pumps
        // the event loop and can reallocate both lists (git-pump UAF family).
        const QString branch = s->branchName;
        const QString localPath = m_repositories.at(ri).localPath;
        if (!bindRepoDetailToRepo(ri))
            return;
        deleteWorktreeBranchAndAgent(
            worktreePathForBranch(localPath, branch, WorktreeLookup::Any), branch);
    });
}

bool MainWindow::agentSessionHasLiveTransport(int sessionId) const
{
#ifdef FORKMESH_WINDOW_TESTS
    if (m_testLiveTransportSessions.contains(sessionId))
        return true;
#endif
    const ClaudeStreamSession *claude = m_streamSessions.value(sessionId);
    const CodexAppServerSession *codex = m_codexStreams.value(sessionId);
    return (claude && claude->running()) || (codex && codex->running()) ||
           runnerForSession(sessionId) != nullptr;
}

bool MainWindow::discardWedgedAgentTransport(int sessionId)
{
    bool dropped = false;
    if (ClaudeStreamSession *claude = m_streamSessions.value(sessionId);
        claude && claude->running() && !claude->acceptsInput()) {
        m_streamSessions.remove(sessionId);
        claude->stop();
        claude->deleteLater();
        dropped = true;
    }
    if (CodexAppServerSession *codex = m_codexStreams.value(sessionId);
        codex && codex->running() && !codex->acceptsInput()) {
        m_codexStreams.remove(sessionId);
        codex->stop();
        codex->deleteLater();
        dropped = true;
    }
    return dropped;
}

bool MainWindow::handOffAgentSessionToProvider(int sessionId, const QString &provider)
{
    AgentSession *session = findAgentSession(sessionId);
    if (!session || session->associationOnly || provider.isEmpty() ||
        provider == session->provider)
        return false;
    const QString previousProvider = session->provider;
    // A completed turn normally leaves its CLI transport alive. Remove
    // that route before changing the metadata; otherwise the send path
    // sees a live process and hands the new prompt straight back to the
    // agent the user just switched away from.
    if (m_streamSessions.contains(sessionId) || m_codexStreams.contains(sessionId))
        stopStreamSession(sessionId, /*refreshUi=*/false);
    if (AgentRunner *runner = runnerForSession(sessionId))
        runner->stop();
    session = findAgentSession(sessionId);
    if (!session)
        return false;
    session->handoffFromProvider = previousProvider;
    session->provider = provider;
    // The saved runtime picks which CLI launches; left on the old one,
    // "add" relaunched the agent the user just switched away from.
    session->runtimeId = agentRuntimeIdForLegacyProvider(provider);
    session->model.clear();
    if (m_agentStore) {
        m_agentStore->saveSession(*session);
        m_agentStore->appendLog(
            *session,
            QStringLiteral("\n==> Agent handoff: %1 to %2. The saved "
                           "transcript will be included as context.\n")
                .arg(agentProviderName(previousProvider), agentProviderName(provider)));
    }
    return true;
}

void MainWindow::applyComposerSelectionToAgentSession(int sessionId)
{
    if (AgentSession *session = findAgentSession(sessionId);
        session && !session->associationOnly) {
        bool changed = false;
        bool providerChanged = false;
        const QString composerProvider =
            m_quickAddAgentProvider ? m_quickAddAgentProvider->currentData().toString()
                                    : QString();
        const bool composerIsAgent =
            !composerProvider.isEmpty() &&
            !quickAddProviderIsPromptAction(composerProvider);
        if (composerIsAgent && composerProvider != session->provider) {
            if (!handOffAgentSessionToProvider(sessionId, composerProvider))
                return;
            session = findAgentSession(sessionId);
            if (!session)
                return;
            changed = true;
            providerChanged = true;
        }
        if (composerIsAgent && m_quickAddClaudeModel) {
            const QString selectedModel =
                selectedModelComboValue(m_quickAddClaudeModel);
            const QString chosen = agentIsCodexProvider(composerProvider)
                                       ? codexChatGptModelId(selectedModel)
                                       : selectedModel;
            if (session->model != chosen &&
                agentModelMatchesProvider(session->provider, chosen)) {
                // Moving off a model the provider rejected: that run's native
                // thread may not hold the task, so hand over the transcript.
                if (!providerChanged && !session->model.trimmed().isEmpty()) {
                    QString failure = session->lastError;
                    if (m_agentStore)
                        failure += QLatin1Char('\n') +
                                   m_agentStore->readLog(*session).right(16 * 1024);
                    failure += QLatin1Char('\n') +
                               m_streamRaw.value(sessionId).right(16 * 1024);
                    if (forkmesh::teammates::modelIsUnavailable(failure,
                                                                session->model))
                        m_agentModelHandoffs.insert(sessionId, session->model);
                }
                session->model = chosen;
                changed = true;
            }
        }
        // The composer's mode dropdown is the user's live permission-mode choice
        // for what runs next; capture it the same way as the model so the next
        // resume honors it (a still-running turn can't be retargeted, but the
        // stored label is picked up on the following resume) and the detail
        // header shows which mode this session runs.
        if (m_quickAddModeSelector) {
            const QString chosenMode = m_quickAddModeSelector->currentText();
            if (session->mode != chosenMode) {
                session->mode = chosenMode;
                changed = true;
            }
        }
        if (const QString chosenStrength = composerAgentStrength();
            session->strength != chosenStrength) {
            session->strength = chosenStrength;
            changed = true;
        }
        if (!changed)
            return;
        if (m_agentStore)
            m_agentStore->saveSession(*session);
        // The mode is named on the session divider and in the detail table, so
        // redraw them now: the user picked a mode and pressed send, and the
        // header still naming the old one reads as the pick not landing.
        if (sessionId == m_selectedAgentSessionId)
            showAgentSession(sessionId);
    }
}

void MainWindow::sendPromptToSelectedAgent(const QString &prompt)
{
    applyComposerSelectionToAgentSession(m_selectedAgentSessionId);
    if (m_agentTranscript)
        m_agentTranscript->jumpToBottom();
    sendPromptToAgentSession(m_selectedAgentSessionId, prompt);
    forkmesh::sonify::UiSounds::play(forkmesh::sonify::Cue::Send);
}

void MainWindow::answerAgentQuestion(int sessionId, const QString &toolUseId,
                                     const QString &answer, bool sensitive,
                                     bool deferRefresh)
{
    if (sessionId <= 0 || toolUseId.isEmpty() || answer.trimmed().isEmpty() ||
        !findAgentSession(sessionId) || isExternalSession(sessionId))
        return;
    QTimer::singleShot(0, this, [this] { ringLocalControlAgentOpsBell(); });

    applyTranscriptEvent(
        sessionId,
        QJsonObject{
            {QStringLiteral("type"), QStringLiteral("_local_ask_answer")},
            {QStringLiteral("tool_use_id"), toolUseId},
            {QStringLiteral("text"),
             sensitive ? QStringLiteral("[secret answer hidden]") : answer}});

    AgentRuntimeSession *runtime = m_codexStreams.value(sessionId);
    if (!runtime)
        runtime = m_streamSessions.value(sessionId);
    bool delivered = false;
    const bool brokeredPermission =
        m_agentPermissionController &&
        m_agentPermissionController->isPending(QString::number(sessionId),
                                               toolUseId);
    QString brokeredGrantId;
    bool brokeredPersistent = false;
    if (brokeredPermission && (!runtime || !runtime->running())) {
        m_agentPermissionController->resolve(
            QString::number(sessionId), toolUseId, AgentGrantLifetime::Once,
            AgentCapabilityDecision::Deny);
        noteAgentSessionNotice(
            sessionId,
            QStringLiteral(
                "The requesting runtime is no longer available; the operation remains denied."),
            /*error=*/true);
        return;
    }
    if (runtime && runtime->running()) {
        AgentRuntimeCommand command{
            1, AgentRuntimeCommandTypes::RequestRespond, toolUseId, 0,
            {{QStringLiteral("answer"), answer}}};
        if (brokeredPermission) {
            const QString normalized = answer.trimmed().toLower();
            AgentCapabilityDecision decision = AgentCapabilityDecision::Deny;
            AgentGrantLifetime lifetime = AgentGrantLifetime::Once;
            if (normalized.startsWith(QStringLiteral("allow")) ||
                normalized.startsWith(QStringLiteral("accept"))) {
                decision = AgentCapabilityDecision::Allow;
                if (normalized.contains(QStringLiteral("always")) ||
                    normalized.contains(QStringLiteral("persistent")))
                    lifetime = AgentGrantLifetime::Persistent;
                else if (normalized.contains(QStringLiteral("worktree")))
                    lifetime = AgentGrantLifetime::Worktree;
                else if (normalized.contains(QStringLiteral("session")))
                    lifetime = AgentGrantLifetime::Session;
            }
            AgentPermissionResult resolution =
                m_agentPermissionController->resolve(
                    QString::number(sessionId), toolUseId, lifetime, decision);
            if (resolution.status == AgentPermissionStatus::Resolved) {
                command = resolution.response;
                brokeredGrantId = resolution.matchedGrantId;
                brokeredPersistent =
                    lifetime == AgentGrantLifetime::Persistent &&
                    resolution.decision == AgentCapabilityDecision::Allow;
            } else {
                noteAgentSessionNotice(sessionId, resolution.error,
                                       /*error=*/true);
                return;
            }
        }
        delivered = runtime->sendCommand(command);
    }

    if (delivered) {
        if (brokeredPersistent) {
            QString saveError;
            if (!AgentCapabilityStore::save(
                    AgentCapabilityStore::defaultPath(),
                    *m_agentCapabilityBroker, &saveError)) {
                m_agentCapabilityBroker->revoke(brokeredGrantId);
                noteAgentSessionNotice(
                    sessionId,
                    QStringLiteral(
                        "This operation was allowed once, but the persistent permission could not be saved: %1")
                        .arg(saveError),
                    /*error=*/true);
            }
        }
        markAgentSessionRunning(sessionId);
        scheduleAgentSessionsPush();
        return;
    }
    if (brokeredPermission) {
        if (!brokeredGrantId.isEmpty()) {
            m_agentCapabilityBroker->revoke(brokeredGrantId);
        }
        noteAgentSessionNotice(
            sessionId,
            QStringLiteral(
                "The permission response could not be delivered; the operation remains denied."),
            /*error=*/true);
        return;
    }

    // A card can outlive the process that issued it (for example, the app was
    // restarted while the agent was waiting). The card has already accepted the
    // click, so silently returning here strands the session in Waiting. Carry the
    // answer into a live replacement turn when possible, or park it for the
    // normal resume path. `_local_ask_answer` above is the visible transcript
    // record, so the fallback prompt is not recorded a second time (and a secret
    // answer is not copied into the persisted transcript).
    const QString continuation =
        QStringLiteral("My answer to your pending question is:\n%1\n\n"
                       "Continue the session.")
            .arg(answer);
    if (!deliverPromptToLiveAgentTransport(sessionId, continuation,
                                           /*recordPrompt=*/false)) {
        queueAgentSteerMessage(sessionId, continuation);
        continueAgentSession(sessionId, deferRefresh);
    }
    scheduleAgentSessionsPush();
}

void MainWindow::sendPromptToAgentSession(int sessionId, const QString &prompt)
{
    // A cloud run has no transport here; its follow-up goes to the relay.
    if (isCloudSession(sessionId)) {
        sendCloudAgentFollowUp(sessionId, prompt);
        return;
    }
    if (prompt.isEmpty() || sessionId < 0)
        return;
    if (prompt.contains(QLatin1String("Attached image:")) ||
        prompt.contains(QLatin1String("Attached video:")))
        m_agentImageScanPending = true;
    if (deliverPromptToLiveAgentTransport(sessionId, prompt))
        return;
    AgentSession *session = findAgentSession(sessionId);
    if (!session)
        return;
    const int sid = session->id;
    queueAgentSteerMessage(sid, prompt);
    if (session->provider == QLatin1String("claude-code") ||
        agentIsCodexProvider(session->provider))
        applyTranscriptEvent(
            sid, QJsonObject{
                     {QStringLiteral("type"), QStringLiteral("_local_user")},
                     {QStringLiteral("text"), prompt}});
    else if (m_agentStore)
        m_agentStore->appendLog(
            *session,
            QStringLiteral("\n==> User steering prompt (queued for restart)\n%1")
                .arg(prompt));
    continueAgentSession(sid);
}

bool MainWindow::syncLivePermissionMode(int sessionId, ClaudeStreamSession *claude)
{
    const AgentSession *session = findAgentSession(sessionId);
    if (!claude || !claude->running() || !session)
        return true;
    const QString wanted = agentPermissionModeId(session->mode);
    if (wanted.isEmpty() || wanted == claude->permissionMode())
        return true;
    if (!claude->setPermissionMode(wanted)) {
        // Only the promotion to Auto can fail. Restarting is what makes it
        // stick, but not at the cost of a turn in flight: an agent that is
        // still working keeps its mode and is told when the new one lands.
        if (isStoppableAgentSession(sessionId)) {
            noteAgentSessionNotice(
                sessionId,
                QString::fromUtf8("This agent is mid-turn under %1, so it keeps "
                               "asking until the turn ends \xE2\x80\x94 Auto "
                               "takes over the next time the session starts.")
                    .arg(session->mode.trimmed()),
                /*error=*/false);
            return true;
        }
        return false;
    }
    noteAgentSessionNotice(
        sessionId,
        QStringLiteral("Permission mode switched to %1 for the rest of this "
                       "session.")
            .arg(session->mode.trimmed()),
        /*error=*/false);
    return true;
}

bool MainWindow::deliverPromptToLiveAgentTransport(int sessionId,
                                                   const QString &prompt,
                                                   bool recordPrompt)
{
    auto restarting = [this, sessionId] {
        noteAgentSessionNotice(
            sessionId,
            QStringLiteral("The agent's process was no longer accepting input, "
                           "so ForkMesh is restarting the session with your "
                           "message."),
            /*error=*/false);
        return false;
    };
    if (discardWedgedAgentTransport(sessionId))
        return restarting();
    ClaudeStreamSession *claude = m_streamSessions.value(sessionId);
    CodexAppServerSession *codex = m_codexStreams.value(sessionId);
    const bool claudeLive = claude && claude->running();
    const bool codexLive = codex && codex->running();
#ifdef FORKMESH_WINDOW_TESTS
    const bool testLive = m_testLiveTransportSessions.contains(sessionId);
#else
    const bool testLive = false;
#endif
    if (!claudeLive && !codexLive && !testLive) {
        if (AgentRunner *runner = runnerForSession(sessionId))
            return runner->steer(prompt);
        return false;
    }
    bool sent = false;
    if (testLive) {
        sent = true;
    } else if (codexLive) {
        if (const AgentSession *session = findAgentSession(sessionId)) {
            const QString effort =
                QSettings()
                    .value(kClaudeEffortSetting, QStringLiteral("high"))
                    .toString();
            QString model = session->model.trimmed();
            if (!model.isEmpty())
                model = codexChatGptModelId(model);
            if (!agentModelMatchesProvider(kCodexProvider, model))
                model.clear();
            codex->setTurnOptions(model, session->mode, effort);
        }
        sent = codex->sendUserText(prompt);
    } else {
        // The mode the composer is showing is the mode this message should run
        // under, so switch the live CLI before the turn rather than only
        // stamping the stored session (Codex does the same above, per turn).
        if (!syncLivePermissionMode(sessionId, claude)) {
            if (ClaudeStreamSession *dead = m_streamSessions.take(sessionId)) {
                dead->stop();
                dead->deleteLater();
            }
            noteAgentSessionNotice(
                sessionId,
                QStringLiteral("Auto mode needs a process launched for it, so "
                               "ForkMesh is restarting this session under Auto "
                               "with your message. The conversation carries "
                               "over."),
                /*error=*/false);
            return false;
        }
        sent = claude->sendUserText(prompt);
    }
    if (!sent) {
        if (ClaudeStreamSession *dead = m_streamSessions.take(sessionId)) {
            dead->stop();
            dead->deleteLater();
        }
        if (CodexAppServerSession *dead = m_codexStreams.take(sessionId)) {
            dead->stop();
            dead->deleteLater();
        }
        return restarting();
    }
    if (recordPrompt)
        applyTranscriptEvent(
            sessionId,
            QJsonObject{{QStringLiteral("type"), QStringLiteral("_local_user")},
                        {QStringLiteral("text"), prompt}});
    // A clean result can leave the session labelled Running while its child
    // processes drain. A follow-up accepted during that window is a new turn,
    // even though the label does not change. Funnel every accepted prompt
    // through the normal turn-start transition so the previous turn's watcher
    // cannot later mark this turn done or kill one of its subprocesses.
    markAgentSessionRunning(sessionId);
    return true;
}

void MainWindow::queueAgentSteerMessage(int sessionId, const QString &prompt)
{
    const QString trimmed = prompt.trimmed();
    if (trimmed.isEmpty())
        return;
    AgentSession *session = findAgentSession(sessionId);
    QString &pending = m_pendingSteerMessage[sessionId];
    if (pending.trimmed().isEmpty() && session)
        pending = session->pendingSteerMessage.trimmed();
    if (pending.trimmed().isEmpty()) {
        pending = trimmed;
    } else if (pending != trimmed &&
               !pending.endsWith(QStringLiteral("\n\n") + trimmed)) {
        pending += QStringLiteral("\n\n") + trimmed;
    }
    if (session) {
        session->pendingSteerMessage = pending;
        if (m_agentStore && !isExternalSession(sessionId))
            m_agentStore->saveSession(*session);
    }
}

QString MainWindow::takeAgentSteerMessageForLaunch(int sessionId)
{
    AgentSession *session = findAgentSession(sessionId);
    QString steer = m_pendingSteerMessage.take(sessionId);
    // Disk is the source of truth after a restart. Startup normally mirrors
    // this into the map, but falling back here prevents a later reload or UI
    // reconstruction from separating the prompt from its queued session.
    if (steer.isEmpty() && session)
        steer = session->pendingSteerMessage.trimmed();
    if (steer.isEmpty()) {
        m_inFlightSteerMessage.remove(sessionId);
        return QString();
    }
    m_inFlightSteerMessage.insert(sessionId, steer);
    if (session) {
        session->pendingSteerMessage.clear();
        session->inFlightSteerMessage = steer;
        if (m_agentStore && !isExternalSession(sessionId))
            m_agentStore->saveSession(*session);
    }
    return steer;
}

void MainWindow::restoreAgentSteerMessageAfterFailedLaunch(int sessionId)
{
    AgentSession *session = findAgentSession(sessionId);
    // The durable marker is authoritative. AgentRunner clears it only after
    // the actual agent subprocess starts; its in-memory mirror can legitimately
    // linger until the runner finishes.
    if (session && session->inFlightSteerMessage.isEmpty()) {
        m_inFlightSteerMessage.remove(sessionId);
        return;
    }
    QString unsent = session ? session->inFlightSteerMessage
                             : m_inFlightSteerMessage.value(sessionId);
    m_inFlightSteerMessage.remove(sessionId);
    if (session)
        session->inFlightSteerMessage.clear();
    if (!unsent.isEmpty())
        queueAgentSteerMessage(sessionId, unsent);
    else if (session && m_agentStore && !isExternalSession(sessionId))
        m_agentStore->saveSession(*session);
}

void MainWindow::acknowledgeAgentSteerMessage(int sessionId)
{
    m_inFlightSteerMessage.remove(sessionId);
    if (AgentSession *session = findAgentSession(sessionId);
        session && !session->inFlightSteerMessage.isEmpty()) {
        session->inFlightSteerMessage.clear();
        if (m_agentStore && !isExternalSession(sessionId))
            m_agentStore->saveSession(*session);
    }
}

void MainWindow::sendIssueContextToSelectedAgent()
{
    AgentSession *session = findAgentSession(m_selectedAgentSessionId);
    if (!session || session->issueNumber <= 0) {
        logSystem(QString::fromUtf8(
            "No issue-linked agent open above to send context to \xE2\x80\x94 "
            "open one first."));
        return;
    }
    const int repoIndex = repoIndexFor(session->owner, session->name);
    if (repoIndex < 0) {
        logSystem(QStringLiteral("Can't find this session's repository."));
        return;
    }
    const RepositoryRecord &repo = m_repositories.at(repoIndex);
    const QList<Issue> issues =
        IssueStore(repo.localPath, repo.mirrorPath, &m_profileIdentity, m_userName)
            .loadAll();
    const Issue *issue = nullptr;
    for (const Issue &candidate : issues)
        if (candidate.number == session->issueNumber) {
            issue = &candidate;
            break;
        }
    if (!issue) {
        logSystem(QStringLiteral("Could not find issue #%1 to resend its context.")
                      .arg(session->issueNumber));
        return;
    }
    sendPromptToSelectedAgent(issueContextPrompt(*issue));
}
