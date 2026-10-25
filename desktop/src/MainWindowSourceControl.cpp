
#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "DiffChangeBlock.h"
#include "SecretDiffBanner.h"
#include "FlowLayout.h"
#include "HeaderWrapLayout.h"
#include "RepoStatsStore.h"
#include "KebabHeaderView.h"
#include "FontSizing.h"

#include <QAbstractTextDocumentLayout>
#include <QCheckBox>
#include <QClipboard>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QLayout>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QGraphicsOpacityEffect>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPromise>
#include <QProcess>
#include <QPropertyAnimation>
#include <QScrollArea>
#include <QScrollBar>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QTemporaryFile>
#include <QTextBlock>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QWidgetAction>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <functional>
#include <memory>
#include <utility>

using namespace forkmesh::ui;

namespace {
constexpr int kScmNextRowHeight = 32;
constexpr int kScmNextRowRadius = 6;
constexpr int kScmNextCommitRowHeight = 36;
constexpr int kGitNextSideWidth = 240;
constexpr int kGitNextSideRowHeight = 28;
constexpr int kGitNextSideDetailRowHeight = 44;
constexpr int kGitNextFieldHeight = 34;
constexpr int kGitNextDescriptionHeight = 76;
constexpr int kScmClassicCommitRowHeight = 24;
constexpr int kScmNextComposeButton = 28;
constexpr int kScmClassicComposeButtonWidth = 30;
constexpr int kScmNextCardRadius = 12;
constexpr int kScmNextChipRadius = 4;
const char kScmDiffCardName[] = "scmDiffCard";

void scmSetShown(QWidget *widget, bool shown)
{
    if (!widget)
        return;
    if (!shown) {
        widget->hide();
        return;
    }
    if (widget->isHidden() && widget->testAttribute(Qt::WA_WState_ExplicitShowHide))
        widget->show();
}

struct ScmRowParts {
    QLabel *icon = nullptr;
    QLabel *name = nullptr;
    QLabel *directory = nullptr;
    QString directoryText;
    QLabel *status = nullptr;
    QCheckBox *check = nullptr;
    QWidget *actions = nullptr;
};

class ScmFileRow : public QWidget
{
public:
    explicit ScmFileRow(QWidget *parent = nullptr) : QWidget(parent)
    {
        setObjectName(QStringLiteral("scmFileRow"));
        setAttribute(Qt::WA_Hover);
        setAttribute(Qt::WA_StyledBackground);
        applyStyle();
    }

    void setActionsWidget(QWidget *actions)
    {
        m_actions = actions;
        if (m_actions)
            m_actions->hide();
    }

    void setSelected(bool selected)
    {
        if (m_selected == selected)
            return;
        m_selected = selected;
        applyStyle();
    }

    bool isSelected() const { return m_selected; }

    void setParts(const ScmRowParts &parts)
    {
        m_parts = parts;
        m_arranged = -1;
        arrange();
    }

    void refreshStyle()
    {
        arrange();
        applyStyle();
    }

    std::function<void()> onClicked;

protected:
    void paintEvent(QPaintEvent *event) override
    {
        if (Theme::isNext() && (m_selected || m_hovered)) {
            QPainter painter(this);
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.setPen(Qt::NoPen);
            painter.setBrush(Theme::color(m_selected ? Theme::Role::SurfaceActive
                                                     : Theme::Role::SurfaceRaised));
            painter.drawRoundedRect(QRectF(rect()).adjusted(0, 1, 0, -1),
                                    kScmNextRowRadius, kScmNextRowRadius);
        }
        QWidget::paintEvent(event);
    }

    void enterEvent(QEnterEvent *event) override
    {
        QWidget::enterEvent(event);
        m_hovered = true;
        applyStyle();
    }

    void leaveEvent(QEvent *event) override
    {
        QWidget::leaveEvent(event);
        QTimer::singleShot(0, this, [this] {
            // QWidget::underMouse() becomes false when the pointer moves from
            // the row onto one of its child tool buttons. Hiding the actions at
            // that point removes the discard button between mouse press and
            // release, so QToolButton never emits clicked(). Test the cursor
            // against the whole row rectangle, including child widgets.
            if (!rect().contains(mapFromGlobal(QCursor::pos()))) {
                m_hovered = false;
                applyStyle();
            }
        });
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && onClicked)
            onClicked();
        QWidget::mousePressEvent(event);
    }

private:
    void arrange()
    {
        auto *box = qobject_cast<QHBoxLayout *>(layout());
        if (!box || !m_parts.name || !m_parts.status || !m_parts.actions)
            return;
        const bool next = Theme::isNext();
        if (m_arranged == int(next))
            return;
        m_arranged = int(next);
        while (QLayoutItem *item = box->takeAt(0))
            delete item;
        if (m_parts.icon)
            scmSetShown(m_parts.icon, !next);
        if (m_parts.check)
            m_parts.check->setVisible(next);
        if (next) {
            box->setContentsMargins(8, 0, 6, 0);
            box->setSpacing(8);
            if (m_parts.check)
                box->addWidget(m_parts.check);
            box->addWidget(m_parts.status);
            auto *path = new QHBoxLayout;
            path->setContentsMargins(0, 0, 0, 0);
            path->setSpacing(0);
            if (m_parts.directory) {
                m_parts.directory->setText(m_parts.directoryText + QLatin1Char('/'));
                path->addWidget(m_parts.directory);
            }
            path->addWidget(m_parts.name);
            box->addLayout(path);
            box->addStretch();
            box->addWidget(m_parts.actions);
            return;
        }
        box->setContentsMargins(4, 1, 6, 1);
        box->setSpacing(5);
        if (m_parts.icon)
            box->addWidget(m_parts.icon);
        box->addWidget(m_parts.name);
        if (m_parts.directory) {
            m_parts.directory->setText(m_parts.directoryText);
            box->addWidget(m_parts.directory);
        }
        box->addStretch();
        box->addWidget(m_parts.status);
        box->addWidget(m_parts.actions);
    }

    void applyStyle()
    {
        if (Theme::isNext()) {
            const QString sheet = QStringLiteral(
                "QWidget#scmFileRow{background:transparent;border:none;}");
            if (styleSheet() != sheet)
                setStyleSheet(sheet);
            update();
            if (m_actions)
                m_actions->setVisible(m_hovered);
            return;
        }
        const QString border = m_selected
                                   ? QStringLiteral("rgba(45,164,78,0.75)")
                                   : QStringLiteral("transparent");
        const QString background =
            !m_selected && m_hovered ? QStringLiteral("rgba(139,148,158,0.12)")
                                     : QStringLiteral("transparent");
        setStyleSheet(
            QStringLiteral("QWidget#scmFileRow{background:%1;"
                           "border:1px solid %2;border-radius:5px;}")
                .arg(background, border));
        if (m_actions)
            m_actions->setVisible(m_hovered);
    }

    QWidget *m_actions = nullptr;
    ScmRowParts m_parts;
    int m_arranged = -1;
    bool m_hovered = false;
    bool m_selected = false;
};

class ScmGroupRow : public QWidget
{
public:
    explicit ScmGroupRow(QWidget *parent = nullptr) : QWidget(parent)
    {
        setObjectName(QStringLiteral("scmGroupRow"));
    }

    void setParts(QLabel *label, const QString &classicText, const QString &nextText,
                  const QList<QWidget *> &classicOnly,
                  const QList<QWidget *> &nextOnly)
    {
        m_label = label;
        m_classicText = classicText;
        m_nextText = nextText;
        m_classicOnly = classicOnly;
        m_nextOnly = nextOnly;
        m_arranged = -1;
        arrange();
    }

    void arrange()
    {
        const bool next = Theme::isNext();
        if (!m_label || m_arranged == int(next))
            return;
        m_arranged = int(next);
        m_label->setText(next ? m_nextText : m_classicText);
        QFont font = m_label->font();
        font.setBold(!next);
        if (next)
            font.setWeight(QFont::Medium);
        if (next || font.letterSpacing() != 0.0)
            font.setLetterSpacing(QFont::AbsoluteSpacing, next ? 0.6 : 0.0);
        m_label->setFont(font);
        for (QWidget *widget : std::as_const(m_classicOnly))
            scmSetShown(widget, !next);
        for (QWidget *widget : std::as_const(m_nextOnly))
            widget->setVisible(next);
        if (layout())
            layout()->setContentsMargins(next ? QMargins(8, 0, 4, 0)
                                              : QMargins(0, 0, 6, 0));
    }

private:
    QLabel *m_label = nullptr;
    QString m_classicText;
    QString m_nextText;
    QList<QWidget *> m_classicOnly;
    QList<QWidget *> m_nextOnly;
    int m_arranged = -1;
};

void scmTagRowLabels(QLabel *name, QLabel *status, QChar code)
{
    if (name)
        name->setProperty("scmPart", QStringLiteral("name"));
    if (status) {
        status->setProperty("scmPart", QStringLiteral("status"));
        status->setProperty("scmStatus", QString(code));
    }
}

void scmTagGroupRow(QWidget *row, QLabel *label)
{
    if (row)
        row->setProperty("scmPart", QStringLiteral("groupRow"));
    if (label)
        label->setProperty("scmPart", QStringLiteral("group"));
}

QString scmTreeStyleSheet()
{
    return Theme::remapSheet(
        QStringLiteral("#fileTree{border:none;"
                       "show-decoration-selected:0;}"
                       "#fileTree::branch:selected{"
                       "background:transparent;}"
                       "#fileTree::item:selected{"
                       "background:%1;"
                       "color:palette(text);"
                       "border:none;"
                       "border-radius:5px;}")
            .arg(Theme::isNext() ? QStringLiteral("transparent")
                                 : QStringLiteral("rgba(45,164,78,0.14)")));
}

QString scmMessageStyleSheet()
{
    return Theme::isNext()
               ? QString()
               : Theme::remapSheet(QStringLiteral(
                     "QPlainTextEdit#scmMessageInput{font-size:11px;padding:4px 6px;}"));
}

QString scmOutlineStyleSheet()
{
    if (Theme::isNext())
        return QStringLiteral("#diffActiveOutline{background:transparent;"
                              "border:1px solid %1;border-radius:0px;}")
            .arg(Theme::hex(Theme::Role::BorderStrong));
    return Theme::remapSheet(
        QStringLiteral("#diffActiveOutline{background:transparent;"
                       "border:2px solid %1;border-radius:6px;}")
            .arg(QStringLiteral("#2da44e")));
}

QString scmStickyHeaderStyleSheet()
{
    if (Theme::isNext())
        return QStringLiteral(
                   "#diffStickyHeader{background:%1;border:1px solid %2;}"
                   "#diffStickyHeader QLabel{background:transparent;color:%3;}")
            .arg(Theme::hex(Theme::Role::Surface), Theme::hex(Theme::Role::Border),
                 Theme::hex(Theme::Role::FgMuted));
    const bool dark = qApp->palette().color(QPalette::Base).lightness() < 128;
    return Theme::remapSheet(
        QStringLiteral("#diffStickyHeader{background:%1;border:none;}"
                       "#diffStickyHeader QLabel{background:transparent;color:%2;}")
            .arg(dark ? "#161b22" : "#f6f8fa", dark ? "#8b949e" : "#57606a"));
}

QString scmNextFileBarStyleSheet(int fontPt)
{
    if (Theme::isNext())
        return QStringLiteral("background:%1;border:none;border-top:1px solid %2;"
                              "padding:6px 12px;font-family:monospace;font-size:%3px;")
            .arg(Theme::hex(Theme::Role::Surface), Theme::hex(Theme::Role::Border))
            .arg(qBound(8, fontPt, 28));
    return Theme::remapSheet(diffStickyStyleSheet(fontPt));
}

void scmSetSheet(QWidget *widget, const QString &sheet)
{
    if (widget && widget->styleSheet() != sheet)
        widget->setStyleSheet(sheet);
}

void scmSyncHover(QAbstractItemView *view, bool next)
{
    QWidget *viewport = view ? view->viewport() : nullptr;
    if (!viewport)
        return;
    const bool owned = viewport->property("scmHover").toBool();
    if (next && !viewport->testAttribute(Qt::WA_Hover)) {
        viewport->setAttribute(Qt::WA_Hover, true);
        viewport->setProperty("scmHover", true);
    } else if (!next && owned) {
        viewport->setAttribute(Qt::WA_Hover, false);
        viewport->setProperty("scmHover", false);
    }
}

void scmTintButtonIcon(QPushButton *button, Theme::Role role)
{
    if (!button)
        return;
    const QString name = button->property("forkmeshOcticon").toString();
    if (name.isEmpty() || button->property("fmSpinning").toBool())
        return;
    const int stored = button->property("forkmeshOcticonSize").toInt();
    const int px = stored > 0 ? stored : 16;
    QIcon icon = themedOcticon(name, Theme::color(role), px);
    if (button->isCheckable())
        icon.addPixmap(tintedOcticonPixmap(name, Theme::color(Theme::Role::Fg), px),
                       QIcon::Normal, QIcon::On);
    button->setIcon(icon);
}

class ScmTreeRowDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override
    {
        QSize hint = QStyledItemDelegate::sizeHint(option, index);
        if (Theme::isNext())
            hint.setHeight(qMax(hint.height(), kScmNextRowHeight));
        return hint;
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        if (Theme::isNext()) {
            const auto *view =
                qobject_cast<const QAbstractItemView *>(option.widget);
            if (view && view->indexWidget(index))
                return;
            const bool selected = option.state.testFlag(QStyle::State_Selected);
            const bool hovered = option.state.testFlag(QStyle::State_MouseOver);
            if (selected || hovered) {
                painter->save();
                painter->setRenderHint(QPainter::Antialiasing, true);
                painter->setPen(Qt::NoPen);
                painter->setBrush(Theme::color(selected
                                                   ? Theme::Role::SurfaceActive
                                                   : Theme::Role::SurfaceRaised));
                painter->drawRoundedRect(QRectF(option.rect).adjusted(0, 1, 0, -1),
                                         kScmNextRowRadius, kScmNextRowRadius);
                painter->restore();
            }
        }
        QStyledItemDelegate::paint(painter, option, index);
    }
};

class ScmCommitRowDelegate : public CommitSummaryDelegate
{
public:
    ScmCommitRowDelegate(QObject *parent, std::function<QString()> currentRef)
        : CommitSummaryDelegate(parent), m_currentRef(std::move(currentRef))
    {
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        if (!Theme::isNext()) {
            CommitSummaryDelegate::paint(painter, option, index);
            return;
        }
        const QWidget *widget = option.widget;
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        const bool hovered = option.state.testFlag(QStyle::State_MouseOver);
        const int kind = index.data(kCommitRowKindRole).toInt();
        const bool fileRow = kind == 1;

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        if (selected || hovered) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(Theme::color(selected ? Theme::Role::SurfaceActive
                                                    : Theme::Role::SurfaceRaised));
            painter->drawRoundedRect(QRectF(option.rect).adjusted(0, 1, -2, -1),
                                     kScmNextRowRadius, kScmNextRowRadius);
        }
        painter->restore();

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
        if (fileRow)
            rowMaxLane =
                std::max(rowMaxLane, index.data(kGraphNodeLaneRole).toInt());
        const int laneCount = std::max(
            rowMaxLane + 1,
            widget ? widget->property(kGraphLaneCountProperty).toInt() : 0);
        const CommitGraphMetrics metrics = commitGraphMetrics(laneCount);
        paintCommitGraphGutter(painter, option.rect, topLanes, botLanes, nodeLane,
                               graphIdx.data(kGraphIsMergeRole).toBool(),
                               index.data(kCommitOutgoingRole).toBool(), metrics);
        const int indent = commitGraphTextIndent(rowMaxLane, metrics);

        const int basePx = option.font.pixelSize() > 0
                               ? option.font.pixelSize()
                               : QFontInfo(option.font).pixelSize();
        QFont subjectFont = option.font;
        subjectFont.setPixelSize(qMax(10, basePx - 1));
        QFont metaFont = option.font;
        metaFont.setPixelSize(qMax(9, basePx - 2));
        QFont chipFont = option.font;
        chipFont.setPixelSize(qMax(9, basePx - 3));
        chipFont.setWeight(QFont::Medium);
        const QFontMetrics subjectFm(subjectFont);
        const QFontMetrics metaFm(metaFont);
        const QFontMetrics chipFm(chipFont);

        QRect r = option.rect.adjusted(indent, 0, -10, 0);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        if (fileRow) {
            r.adjust(18, 0, 0, 0);
            const QString addsTxt = QStringLiteral("+%1").arg(
                index.data(kCommitFileAddsRole).toInt());
            const QString delsTxt = QString::fromUtf8("\xE2\x88\x92%1").arg(
                index.data(kCommitFileDelsRole).toInt());
            const int delsW = metaFm.horizontalAdvance(delsTxt);
            const int addsW = metaFm.horizontalAdvance(addsTxt);
            painter->setFont(metaFont);
            painter->setPen(Theme::color(Theme::Role::Danger));
            painter->drawText(QRect(r.right() - delsW, r.top(), delsW, r.height()),
                              Qt::AlignVCenter | Qt::AlignRight, delsTxt);
            painter->setPen(Theme::color(Theme::Role::Success));
            painter->drawText(
                QRect(r.right() - delsW - 6 - addsW, r.top(), addsW, r.height()),
                Qt::AlignVCenter | Qt::AlignRight, addsTxt);
            int fx = r.left();
            const QIcon fic = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
            if (!fic.isNull()) {
                fic.paint(painter, QRect(fx, r.center().y() - 7, 14, 14));
                fx += 20;
            }
            const int textW = std::max(0, r.right() - fx - delsW - addsW - 20);
            painter->setPen(Theme::color(selected || hovered
                                             ? Theme::Role::Fg
                                             : Theme::Role::FgSecondary));
            painter->drawText(QRect(fx, r.top(), textW, r.height()),
                              Qt::AlignVCenter | Qt::AlignLeft,
                              metaFm.elidedText(
                                  index.data(Qt::DisplayRole).toString(),
                                  Qt::ElideMiddle, textW));
            painter->restore();
            return;
        }

        int x = r.left();
        const QIcon icon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
        if (!icon.isNull()) {
            icon.paint(painter, QRect(x, r.center().y() - 7, 14, 14));
            x += 20;
        }
        int rightEdge = r.right();
        if (index.data(kCommitUnsyncedRole).toBool()) {
            const QString mark = QString::fromUtf8("\xE2\x96\xB2");
            const int mw = chipFm.horizontalAdvance(mark);
            painter->setFont(chipFont);
            painter->setPen(Theme::color(Theme::Role::Warning));
            painter->drawText(QRect(rightEdge - mw, r.top(), mw, r.height()),
                              Qt::AlignVCenter | Qt::AlignRight, mark);
            rightEdge -= mw + 8;
        }
        if (kind == 0 && rightEdge - x > 240) {
            const QString age =
                index.model()->index(index.row(), 1).data(Qt::DisplayRole).toString();
            QString hash = index.model()
                               ->index(index.row(), kCommitHashCol)
                               .data(Qt::DisplayRole)
                               .toString();
            hash.remove(QString::fromUtf8("\xE2\x96\xB2 "));
            if (!age.isEmpty()) {
                const int ageW = qMax(metaFm.horizontalAdvance(age), 40);
                painter->setFont(metaFont);
                painter->setPen(Theme::color(Theme::Role::FgMuted));
                painter->drawText(QRect(rightEdge - ageW, r.top(), ageW, r.height()),
                                  Qt::AlignVCenter | Qt::AlignRight, age);
                rightEdge -= ageW + 12;
            }
            if (!hash.isEmpty()) {
                QFont monoFont = metaFont;
                const QString mono = Theme::monoFontFamily();
                if (!mono.isEmpty())
                    monoFont.setFamily(mono);
                const QFontMetrics monoFm(monoFont);
                const int hashW = monoFm.horizontalAdvance(hash);
                painter->setFont(monoFont);
                painter->setPen(Theme::color(Theme::Role::FgSubtle));
                painter->drawText(QRect(rightEdge - hashW, r.top(), hashW, r.height()),
                                  Qt::AlignVCenter | Qt::AlignLeft, hash);
                rightEdge -= hashW + 12;
            }
        }
        const QStringList refs = index.data(kCommitRefsRole).toStringList();
        const QStringList refKinds =
            index.data(kCommitRefKindsRole).toStringList();
        if (!refs.isEmpty()) {
            const QString current = m_currentRef ? m_currentRef() : QString();
            constexpr int iconSize = 11;
            constexpr int iconGap = 4;
            constexpr int chipPad = 6;
            const int chipHeight = qMin(r.height() - 8, chipFm.height() + 4);
            painter->setFont(chipFont);
            for (int refIndex = refs.size() - 1; refIndex >= 0; --refIndex) {
                const QString ref = refs.at(refIndex);
                const QString refKind = refKinds.value(refIndex);
                const bool remote = refKind == QLatin1String("remote");
                const bool tag = refKind == QLatin1String("tag");
                const bool local = !remote && !tag;
                const bool head =
                    local && !current.isEmpty() &&
                    (ref == current ||
                     ref.startsWith(current + QLatin1Char(' ')));
                const int rw = chipFm.horizontalAdvance(ref) + chipPad * 2 +
                               iconSize + iconGap;
                if (x + rw > rightEdge - 80)
                    break;
                rightEdge -= rw;
                const QRectF chip(rightEdge,
                                  r.top() + (r.height() - chipHeight) / 2.0, rw,
                                  chipHeight);
                const QColor ink =
                    Theme::color(local ? Theme::Role::AccentFg
                                       : Theme::Role::FgMuted);
                painter->setBrush(Theme::color(local ? Theme::Role::AccentSubtle
                                                     : Theme::Role::SurfaceRaised));
                if (head) {
                    QColor ring = Theme::color(Theme::Role::AccentFg);
                    ring.setAlpha(110);
                    painter->setPen(QPen(ring, 1));
                } else if (local) {
                    painter->setPen(Qt::NoPen);
                } else {
                    painter->setPen(QPen(Theme::color(Theme::Role::Border), 1));
                }
                painter->drawRoundedRect(chip.adjusted(0.5, 0.5, -0.5, -0.5),
                                         kScmNextChipRadius, kScmNextChipRadius);
                painter->drawPixmap(
                    QRectF(chip.left() + chipPad,
                           chip.top() + (chip.height() - iconSize) / 2.0, iconSize,
                           iconSize)
                        .toRect(),
                    tintedOcticonPixmap(remote ? QStringLiteral("cloud")
                                               : tag ? QStringLiteral("tag")
                                                     : QStringLiteral("git-branch"),
                                        ink, iconSize));
                painter->setPen(ink);
                painter->drawText(
                    chip.adjusted(chipPad + iconSize + iconGap, 0, -chipPad, 0),
                    Qt::AlignVCenter | Qt::AlignLeft, ref);
                rightEdge -= 6;
            }
            painter->setBrush(Qt::NoBrush);
        }

        const int textW = std::max(0, rightEdge - x);
        const QString subject = index.data(Qt::DisplayRole).toString();
        const QString author = index.data(kCommitAuthorRole).toString();
        if (kind == 2)
            subjectFont.setWeight(QFont::Medium);
        const QFontMetrics drawFm(subjectFont);
        const QString elidedSubject =
            drawFm.elidedText(subject, Qt::ElideRight, textW);
        const int subjectW = drawFm.horizontalAdvance(elidedSubject);
        const QVariant fgVar = index.data(Qt::ForegroundRole);
        painter->setFont(subjectFont);
        painter->setPen(fgVar.isValid()
                            ? Theme::remapColor(
                                  qvariant_cast<QBrush>(fgVar).color())
                            : Theme::color(Theme::Role::Fg));
        painter->drawText(QRect(x, r.top(), subjectW, r.height()),
                          Qt::AlignVCenter | Qt::AlignLeft, elidedSubject);
        if (!author.isEmpty()) {
            const QString suffix = QString::fromUtf8("  \xC2\xB7  ") + author;
            const int rem = std::max(0, textW - subjectW);
            painter->setFont(metaFont);
            painter->setPen(Theme::color(Theme::Role::FgMuted));
            painter->drawText(QRect(x + subjectW, r.top(), rem, r.height()),
                              Qt::AlignVCenter | Qt::AlignLeft,
                              metaFm.elidedText(suffix, Qt::ElideRight, rem));
        }
        painter->restore();
    }

private:
    std::function<QString()> m_currentRef;
};

class ScmDiffCard : public QWidget
{
public:
    explicit ScmDiffCard(QWidget *parent) : QWidget(parent)
    {
        setObjectName(QLatin1String(kScmDiffCardName));
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setFocusPolicy(Qt::NoFocus);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        if (!Theme::isNext())
            return;
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QRectF frame = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        QPainterPath outside;
        outside.addRect(QRectF(rect()));
        QPainterPath inside;
        inside.addRoundedRect(frame, kScmNextCardRadius, kScmNextCardRadius);
        painter.fillPath(outside.subtracted(inside),
                         Theme::color(Theme::Role::Bg));
        painter.setPen(QPen(Theme::color(Theme::Role::Border), 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(frame, kScmNextCardRadius, kScmNextCardRadius);
    }
};

class ScmDesignHook : public QObject
{
public:
    enum class Events { Design, DesignAndGeometry };

    ScmDesignHook(QWidget *watched, Events events,
                  std::function<void(bool)> action)
        : QObject(watched), m_watched(watched), m_action(std::move(action)),
          m_geometry(events == Events::DesignAndGeometry)
    {
        watched->installEventFilter(this);
    }

    void run(bool settled = true)
    {
        if (m_running || !m_action)
            return;
        m_running = true;
        m_action(settled);
        m_running = false;
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == m_watched) {
            switch (event->type()) {
            case QEvent::Polish:
            case QEvent::Show:
                run();
                break;
            case QEvent::Resize:
            case QEvent::Move:
            case QEvent::Hide:
                if (m_geometry)
                    run();
                break;
            case QEvent::ParentChange:
                if (m_geometry)
                    queue();
                break;
            case QEvent::StyleChange:
                run(false);
                queue();
                break;
            default:
                break;
            }
        }
        return QObject::eventFilter(watched, event);
    }

private:
    void queue()
    {
        if (m_queued)
            return;
        m_queued = true;
        QTimer::singleShot(0, this, [this] {
            m_queued = false;
            run();
        });
    }

    QWidget *m_watched = nullptr;
    std::function<void(bool)> m_action;
    bool m_geometry = false;
    bool m_running = false;
    bool m_queued = false;
};

struct GitSideItem {
    enum class Kind { Heading, Entry, Link };
    Kind kind = Kind::Entry;
    QString icon;
    QString title;
    QString detail;
    QString toolTip;
    int count = -1;
    bool open = true;
    bool current = false;
    bool strong = false;
    std::function<void()> activate;
};

class GitSideRow : public QWidget
{
public:
    GitSideRow(const GitSideItem &item, QObject *context, QWidget *parent)
        : QWidget(parent), m_item(item), m_context(context)
    {
        setObjectName(QStringLiteral("gitNextSideRow"));
        setAttribute(Qt::WA_Hover);
        setCursor(Qt::PointingHandCursor);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        setFixedHeight(item.kind == GitSideItem::Kind::Entry && !item.detail.isEmpty()
                           ? kGitNextSideDetailRowHeight
                           : kGitNextSideRowHeight);
        QString tip = item.toolTip;
        if (tip.isEmpty() && item.kind == GitSideItem::Kind::Entry)
            tip = item.detail.isEmpty() ? item.title
                                        : item.title + QLatin1Char('\n') + item.detail;
        setToolTip(tip);
    }

protected:
    void enterEvent(QEnterEvent *event) override
    {
        QWidget::enterEvent(event);
        m_hovered = true;
        update();
    }

    void leaveEvent(QEvent *event) override
    {
        QWidget::leaveEvent(event);
        m_hovered = false;
        update();
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        QWidget::mouseReleaseEvent(event);
        if (event->button() != Qt::LeftButton ||
            !rect().contains(event->position().toPoint()) || !m_item.activate)
            return;
        const std::function<void()> activate = m_item.activate;
        QTimer::singleShot(0, m_context ? m_context.data() : this, activate);
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QRectF frame = QRectF(rect()).adjusted(0, 1, 0, -1);
        if (m_item.current || m_hovered) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(Theme::color(m_item.current ? Theme::Role::AccentSubtle
                                                         : Theme::Role::SurfaceRaised));
            painter.drawRoundedRect(frame, kScmNextRowRadius, kScmNextRowRadius);
        }
        QFont base = font();
        const int lineHeight = kGitNextSideRowHeight;
        if (m_item.kind == GitSideItem::Kind::Heading) {
            painter.drawPixmap(
                QRect(6, (height() - 12) / 2, 12, 12),
                tintedOcticonPixmap(m_item.open ? QStringLiteral("chevron-down")
                                                : QStringLiteral("chevron-right"),
                                    Theme::color(Theme::Role::FgSubtle), 12));
            QFont caption = base;
            caption.setPixelSize(11);
            caption.setWeight(QFont::Medium);
            caption.setLetterSpacing(QFont::AbsoluteSpacing, 0.6);
            painter.setFont(caption);
            painter.setPen(Theme::color(Theme::Role::FgSubtle));
            const QFontMetrics fm(caption);
            QString count;
            if (m_item.count >= 0)
                count = QLocale().toString(m_item.count);
            const int countW = count.isEmpty() ? 0 : fm.horizontalAdvance(count);
            if (!count.isEmpty())
                painter.drawText(QRect(width() - 8 - countW, 0, countW, height()),
                                 Qt::AlignVCenter | Qt::AlignRight, count);
            const int textW = qMax(0, width() - 24 - countW - 16);
            painter.drawText(QRect(24, 0, textW, height()),
                             Qt::AlignVCenter | Qt::AlignLeft,
                             fm.elidedText(m_item.title.toUpper(), Qt::ElideRight, textW));
            return;
        }
        QFont title = base;
        title.setPixelSize(13);
        if (m_item.kind == GitSideItem::Kind::Link) {
            title.setPixelSize(12);
            painter.setFont(title);
            painter.setPen(Theme::color(Theme::Role::AccentFg));
            const QFontMetrics fm(title);
            const int textW = qMax(0, width() - 30 - 8);
            painter.drawText(QRect(30, 0, textW, height()),
                             Qt::AlignVCenter | Qt::AlignLeft,
                             fm.elidedText(m_item.title, Qt::ElideRight, textW));
            return;
        }
        if (!m_item.icon.isEmpty())
            painter.drawPixmap(
                QRect(8, (lineHeight - 14) / 2, 14, 14),
                tintedOcticonPixmap(m_item.icon, Theme::color(Theme::Role::FgMuted), 14));
        int right = width() - 8;
        if (m_item.current) {
            painter.drawPixmap(
                QRect(right - 14, (lineHeight - 14) / 2, 14, 14),
                tintedOcticonPixmap(QStringLiteral("check"),
                                    Theme::color(Theme::Role::AccentFg), 14));
            right -= 22;
        }
        if (m_item.current || m_item.strong)
            title.setWeight(QFont::Medium);
        painter.setFont(title);
        painter.setPen(Theme::color(m_item.current || m_item.strong
                                        ? Theme::Role::Fg
                                        : Theme::Role::FgMuted));
        const QFontMetrics fm(title);
        const int textW = qMax(0, right - 30);
        painter.drawText(QRect(30, 0, textW, lineHeight),
                         Qt::AlignVCenter | Qt::AlignLeft,
                         fm.elidedText(m_item.title, Qt::ElideRight, textW));
        if (m_item.detail.isEmpty())
            return;
        QFont detail = base;
        const QString mono = Theme::monoFontFamily();
        if (!mono.isEmpty())
            detail.setFamily(mono);
        detail.setPixelSize(11);
        painter.setFont(detail);
        painter.setPen(Theme::color(Theme::Role::FgSubtle));
        const QFontMetrics dfm(detail);
        const int detailW = qMax(0, width() - 30 - 8);
        painter.drawText(QRect(30, lineHeight - 6, detailW, height() - lineHeight + 4),
                         Qt::AlignTop | Qt::AlignLeft,
                         dfm.elidedText(m_item.detail, Qt::ElideRight, detailW));
    }

private:
    GitSideItem m_item;
    QPointer<QObject> m_context;
    bool m_hovered = false;
};

class GitSidePanel : public QFrame
{
public:
    explicit GitSidePanel(QWidget *parent) : QFrame(parent)
    {
        setObjectName(QStringLiteral("gitNextSide"));
        setFixedWidth(kGitNextSideWidth);
        auto *outer = new QVBoxLayout(this);
        outer->setContentsMargins(0, 0, 0, 0);
        outer->setSpacing(0);
        m_scroll = new QScrollArea(this);
        m_scroll->setObjectName(QStringLiteral("gitNextSideScroll"));
        m_scroll->setFrameShape(QFrame::NoFrame);
        m_scroll->setWidgetResizable(true);
        m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_content = new QWidget;
        m_content->setObjectName(QStringLiteral("gitNextSideContent"));
        m_rows = new QVBoxLayout(m_content);
        m_rows->setContentsMargins(8, 8, 8, 8);
        m_rows->setSpacing(1);
        m_rows->addStretch(1);
        m_scroll->setWidget(m_content);
        outer->addWidget(m_scroll);
    }

    void setItems(const QList<GitSideItem> &items, const QString &signature)
    {
        if (signature == m_signature)
            return;
        m_signature = signature;
        while (QLayoutItem *item = m_rows->takeAt(0))
            delete item;
        for (const QPointer<QWidget> &row : std::as_const(m_rowWidgets)) {
            if (row) {
                row->hide();
                row->deleteLater();
            }
        }
        m_rowWidgets.clear();
        bool first = true;
        for (const GitSideItem &item : items) {
            if (item.kind == GitSideItem::Kind::Heading && !first)
                m_rows->addSpacing(6);
            first = false;
            auto *row = new GitSideRow(item, this, m_content);
            m_rows->addWidget(row);
            m_rowWidgets.append(row);
        }
        m_rows->addStretch(1);
    }

private:
    QScrollArea *m_scroll = nullptr;
    QWidget *m_content = nullptr;
    QVBoxLayout *m_rows = nullptr;
    QList<QPointer<QWidget>> m_rowWidgets;
    QString m_signature;
};

class ScmNextChrome : public QObject
{
public:
    explicit ScmNextChrome(QObject *parent) : QObject(parent)
    {
        setObjectName(QStringLiteral("scmNextChrome"));
        m_timer.setSingleShot(true);
        m_timer.setInterval(0);
        connect(&m_timer, &QTimer::timeout, this, [this] {
            if (sync)
                sync();
        });
        open.insert(QStringLiteral("branches"), true);
        open.insert(QStringLiteral("remotes"), false);
        open.insert(QStringLiteral("worktrees"), true);
        open.insert(QStringLiteral("tags"), false);
    }

    ~ScmNextChrome() override
    {
        if (commitLayout && !commitLayout->parent())
            delete commitLayout;
    }

    void watch(QObject *object)
    {
        if (object && !m_watched.contains(object)) {
            m_watched.append(object);
            object->installEventFilter(this);
        }
    }

    void schedule() { m_timer.start(); }

    std::function<void()> sync;
    std::function<void()> mirrorEnabled;
    QPointer<QWidget> header;
    QPointer<GitSidePanel> side;
    QPointer<QWidget> branchPair;
    QPointer<QWidget> historyHeader;
    QPointer<QLabel> summary;
    QPointer<QPushButton> changesTab;
    QPointer<QPushButton> historyTab;
    QPointer<QPushButton> pushButton;
    QPointer<QPushButton> commitMore;
    QPointer<QHBoxLayout> actions;
    QPointer<QVBoxLayout> commitLayout;
    QHash<QString, bool> open;
    bool installed = false;
    bool syncingMessage = false;
    int tab = 0;
    int arranged = -1;
    int pageArranged = -1;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        switch (event->type()) {
        case QEvent::ChildAdded:
        case QEvent::Show:
            schedule();
            break;
        case QEvent::EnabledChange:
            if (mirrorEnabled)
                mirrorEnabled();
            break;
        default:
            break;
        }
        return QObject::eventFilter(watched, event);
    }

private:
    QTimer m_timer;
    QList<QPointer<QObject>> m_watched;
};

ScmNextChrome *scmNextChrome(QWidget *panel)
{
    return panel ? dynamic_cast<ScmNextChrome *>(panel->findChild<QObject *>(
                       QStringLiteral("scmNextChrome"), Qt::FindDirectChildrenOnly))
                 : nullptr;
}

void scmNextChromeSync(QWidget *panel)
{
    if (ScmNextChrome *chrome = scmNextChrome(panel))
        chrome->schedule();
}

struct ScmOutgoingProbe {
    bool branchResolved = false;
    QString branch;
    int pending = 0;
    // scmOutgoingInputSignature() as read by the worker that produced this
    // answer; the next refresh replays the answer while it still matches.
    QString inputSignature;
};

struct ScmCombinedPatchRead {
    QString patch;
    int stagedFiles = 0;
    QList<RepoSecurityFinding> secrets;
};

struct ScmRepositoryPatchRead {
    QString label;
    QString path;
    QString patch;
    QStringList files;
    QStringList states;
};

// Worker-thread half of renderScmCombinedDiff(). A working tree can carry a
// very large patch (and each untracked file needs its own no-index diff), so
// none of these process waits belongs in the GUI-thread rendering half.
ScmCombinedPatchRead readScmCombinedPatch(const QString &dir)
{
    ScmCombinedPatchRead read;
    const QString stagedPatch =
        QString::fromUtf8(gitCaptureStdout(dir, {"diff", "--cached"}));
    QString rest = QString::fromUtf8(gitCaptureStdout(dir, {"diff"}));
    QByteArray others;
    runGitCapture(dir, {"ls-files", "--others", "--exclude-standard", "-z"},
                  &others, nullptr);
    for (const QByteArray &path : others.split('\0')) {
        if (path.isEmpty())
            continue;
        rest += QString::fromUtf8(gitCaptureStdout(
            dir, {"diff", "--no-index", "--", "/dev/null",
                  QString::fromUtf8(path)}));
    }
    read.stagedFiles =
        stagedPatch.count(QStringLiteral("\ndiff --git ")) +
        (stagedPatch.startsWith(QLatin1String("diff --git ")) ? 1 : 0);
    read.patch = stagedPatch + rest;
    return read;
}

QString readScmPathPatch(const QString &dir, const QString &path)
{
    QString patch = QString::fromUtf8(gitCaptureStdout(
        dir, {QStringLiteral("diff"), QStringLiteral("--cached"),
              QStringLiteral("--"), path}));
    patch += QString::fromUtf8(gitCaptureStdout(
        dir, {QStringLiteral("diff"), QStringLiteral("--"), path}));
    QByteArray others;
    runGitCapture(dir,
                  {QStringLiteral("ls-files"), QStringLiteral("--others"),
                   QStringLiteral("--exclude-standard"), QStringLiteral("-z"),
                   QStringLiteral("--"), path},
                  &others, nullptr);
    for (const QByteArray &relative : others.split('\0')) {
        if (relative.isEmpty())
            continue;
        patch += QString::fromUtf8(gitCaptureStdout(
            dir, {QStringLiteral("diff"), QStringLiteral("--no-index"),
                  QStringLiteral("--"), QStringLiteral("/dev/null"),
                  QString::fromUtf8(relative)}));
    }
    return patch;
}

QList<ScmRepositoryPatchRead> readChangedRepositoryPatches(
    const QList<QPair<QString, QString>> &checkouts)
{
    QList<ScmRepositoryPatchRead> changed;
    for (const auto &checkout : checkouts) {
        QByteArray status;
        if (!runGitCapture(checkout.second,
                           {QStringLiteral("status"),
                            QStringLiteral("--porcelain=v1"),
                            QStringLiteral("--untracked-files=all")},
                           &status, nullptr) || status.trimmed().isEmpty())
            continue;
        ScmCombinedPatchRead read = readScmCombinedPatch(checkout.second);
        QStringList files;
        QStringList states;
        for (const QByteArray &line : status.split('\n')) {
            if (line.size() >= 4) {
                files.append(QString::fromUtf8(line.mid(3)).trimmed());
                states.append(QString::fromUtf8(line.left(2)));
            }
        }
        changed.append(
            {checkout.first, checkout.second, std::move(read.patch), files,
             states});
    }
    return changed;
}

// Everything probeSourceControlOutgoing() reads lives under these paths, and
// git renames into them on every commit, checkout, fetch or push, which bumps
// the directory and file mtimes. A stat of each is all it costs to know the
// answer cannot have changed since the last probe.
QString scmOutgoingInputSignature(const RepositoryRecord &repo)
{
    const auto stamp = [](const QString &path) {
        return QString::number(
            QFileInfo(path).lastModified().toMSecsSinceEpoch());
    };
    const QString gitDir = resolveGitDirPath(repo.localPath);
    QString sig = repo.localPath + QLatin1Char('|') + repo.mirrorPath +
                  QLatin1Char('|') + gitDir + QLatin1Char('|');
    if (!gitDir.isEmpty()) {
        // HEAD (branch switches), the reflog (every commit/reset), and every
        // ref recursively (a nested agent/x branch only bumps its own dir).
        sig += stamp(gitDir + QStringLiteral("/logs/HEAD")) + QLatin1Char(',') +
               gitRefsDirSignature(gitDir) + QLatin1Char(',');
        // A linked worktree keeps HEAD in its own gitdir but shares refs with
        // the main repository named by `commondir`.
        QFile common(gitDir + QStringLiteral("/commondir"));
        if (common.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QString target =
                QString::fromUtf8(common.readLine(4096)).trimmed();
            const QString commonDir =
                QDir::isAbsolutePath(target) ? target
                                             : QDir(gitDir).absoluteFilePath(target);
            sig += gitRefsDirSignature(commonDir) + QLatin1Char(',');
        }
    }
    if (!repo.mirrorPath.isEmpty())
        sig += QLatin1Char('|') + gitRefsDirSignature(repo.mirrorPath);
    return sig;
}

ScmOutgoingProbe probeSourceControlOutgoing(const RepositoryRecord &repo,
                                            const QString &relayHost)
{
    ScmOutgoingProbe result;
    if (repo.localPath.isEmpty() ||
        !QDir(repo.localPath).exists(QStringLiteral(".git"))) {
        return result;
    }

    result.branch = headBranchFromFile(repo.localPath);
    if (result.branch.isEmpty()) {
        const GitCaptureResult branch = runGitCaptureDirect(
            repo.localPath,
            {QStringLiteral("symbolic-ref"), QStringLiteral("--short"),
             QStringLiteral("HEAD")});
        if (!branch.ok)
            return result; // detached HEAD has no branch to publish
        result.branch = QString::fromUtf8(branch.output).trimmed();
    }
    if (result.branch.isEmpty())
        return result;
    result.branchResolved = true;

    const bool haveMirror = !repo.mirrorPath.isEmpty() &&
                            QDir(repo.mirrorPath).exists();
    QString mirrorTip = haveMirror
                            ? mirrorBranchCommit(repo.mirrorPath, result.branch)
                            : QString();
    if (haveMirror && mirrorTip.isEmpty()) {
        mirrorTip = mirrorBranchCommit(repo.mirrorPath,
                                       mirrorHeadBranch(repo.mirrorPath));
    }
    QStringList countArgs{QStringLiteral("rev-list"), QStringLiteral("--count")};
    if (haveMirror) {
        countArgs << (mirrorTip.isEmpty()
                          ? result.branch
                          : mirrorTip + QStringLiteral("..") + result.branch);
    } else {
        countArgs << QStringLiteral("@{upstream}..HEAD");
    }
    const GitCaptureResult count = runGitCaptureDirect(repo.localPath, countArgs);
    if (count.ok)
        result.pending = QString::fromUtf8(count.output).trimmed().toInt();
    // A mirror already holding the branch still leaves the commits ahead of
    // the upstream waiting to sync. The Commits page falls back to
    // @{upstream} the same way, so without this the page read "52 local
    // commits waiting to sync" while the rail's Git icon showed no count.
    // Not when the upstream is the relay or the mirror itself: Sync Changes
    // publishes through the mirror and never moves the tracking ref, so the
    // count stuck at "Sync Changes 24↑" behind an "already up to date" sync.
    if (haveMirror && result.pending <= 0 &&
        !upstreamPublishesThroughMirror(repo.localPath, repo.mirrorPath,
                                        relayHost)) {
        const GitCaptureResult upstream = runGitCaptureDirect(
            repo.localPath, {QStringLiteral("rev-list"), QStringLiteral("--count"),
                             QStringLiteral("@{upstream}..HEAD")});
        if (upstream.ok)
            result.pending = QString::fromUtf8(upstream.output).trimmed().toInt();
    }
    return result;
}
} // namespace


static bool runSourceControlMutation(const QString &dir, const QStringList &args,
                                     QByteArray *out, QString *err)
{
    constexpr int kAttempts = 8;
    QString lastError;
    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        lastError.clear();
        if (runGitCapture(dir, args, out, &lastError)) {
            if (err)
                err->clear();
            return true;
        }
        const bool indexBusy =
            lastError.contains(QStringLiteral("index.lock"), Qt::CaseInsensitive) &&
            (lastError.contains(QStringLiteral("File exists"),
                                Qt::CaseInsensitive) ||
             lastError.contains(QStringLiteral("Unable to create"),
                                Qt::CaseInsensitive));
        if (!indexBusy || attempt + 1 == kAttempts)
            break;
        QThread::msleep(static_cast<unsigned long>(100 + attempt * 50));
    }
    if (err)
        *err = lastError;
    return false;
}

MainWindow::ScmMutationResult MainWindow::scmMutate(const QString &dir,
                                                   const QStringList &args)
{
    MainWindow::ScmMutationResult result;
    result.ok = runSourceControlMutation(dir, args, &result.out, &result.error);
    return result;
}

bool MainWindow::queueScmMutation(
    const QString &key, std::function<ScmMutationResult()> work,
    std::function<void(const ScmMutationResult &)> apply)
{
    bool duplicate = m_scmMutationRunning && m_scmMutationRunningKey == key;
    for (const ScmMutationJob &job : std::as_const(m_scmMutationQueue)) {
        if (job.key == key)
            duplicate = true;
    }
    if (duplicate) {
        flashMessage(QStringLiteral("That change is already running; wait for it "
                                    "to finish."));
        return false;
    }
    m_scmMutationQueue.append(
        ScmMutationJob{key, std::move(work), std::move(apply)});
    pumpScmMutationQueue();
    return true;
}

void MainWindow::pumpScmMutationQueue()
{
    if (m_scmMutationRunning || m_scmMutationQueue.isEmpty())
        return;
    ScmMutationJob job = m_scmMutationQueue.takeFirst();
    m_scmMutationRunning = true;
    m_scmMutationRunningKey = job.key;
    runOffThread<ScmMutationResult>(
        std::move(job.work),
        [this, apply = std::move(job.apply)](ScmMutationResult result) {
            m_scmMutationRunning = false;
            m_scmMutationRunningKey.clear();
            if (apply)
                apply(result);
            pumpScmMutationQueue();
        });
}

int MainWindow::scmRepoIndexFor(int hint, const QString &localPath) const
{
    if (localPath.isEmpty())
        return -1;
    if (hint >= 0 && hint < m_repositories.size() &&
        m_repositories.at(hint).localPath == localPath)
        return hint;
    for (int i = 0; i < m_repositories.size(); ++i) {
        if (m_repositories.at(i).localPath == localPath)
            return i;
    }
    return -1;
}

static QString scmStatusTip(QChar status)
{
    switch (status.toLatin1()) {
    case 'M': return QStringLiteral("Modified");
    case 'A': return QStringLiteral("Added");
    case 'D': return QStringLiteral("Deleted");
    case 'R': return QStringLiteral("Renamed");
    case 'C': return QStringLiteral("Copied");
    case 'U': return QStringLiteral("Unmerged (conflict)");
    case 'T': return QStringLiteral("Type changed");
    case '?': return QStringLiteral("Untracked");
    default: return QStringLiteral("Changed");
    }
}

bool MainWindow::sourceControlShowsRange() const
{
    return m_overviewBodyStack && m_overviewBodyStack->currentIndex() == 1 &&
           m_commitsStack &&
           m_commitsStack->currentIndex() == kCommitWorkspaceRangePage &&
           !m_branchDiffBranch.isEmpty();
}

QString MainWindow::sourceControlGitDir() const
{
    if (sourceControlShowsRange() && !m_branchDiffWorkDir.isEmpty() &&
        QDir(m_branchDiffWorkDir).exists())
        return m_branchDiffWorkDir;
    return repoGitDir();
}

QString MainWindow::repoChangeBadgeGitDir() const
{
    // The branch/range stacks retain their state while another section is
    // displayed. The rail badge belongs to the open repository on those
    // pages, not to a hidden branch worktree.
    return m_scmPanel && m_scmPanel->isVisible() ? sourceControlGitDir()
                                                 : repoGitDir();
}

void MainWindow::scrollBranchDiffToFile(const QString &path, bool moveFocus)
{
    if (!m_branchDiffView || path.isEmpty())
        return;
    const int index = m_branchDiffFilePaths.indexOf(path);
    if (index < 0 || index >= m_branchDiffFileAnchors.size())
        return;
    m_lastSourceControlDiffPath = path;
    m_branchActiveFile = path;
    // valueChanged updates the file selected from free scrolling. Do not let
    // that synchronous callback replace an explicit tree/keyboard selection
    // with the section that happened to be at the old viewport edge.
    m_scmSuppressFileScroll = true;
    scrollDiffToAnchor(m_branchDiffView, m_branchDiffFileAnchors.at(index));
    m_scmSuppressFileScroll = false;
    selectScmFileInTree(path, /*staged=*/false);
    updateBranchDiffActiveOutline();
    if (moveFocus)
        m_branchDiffView->setFocus();
}

#ifdef FORKMESH_WINDOW_TESTS
QString MainWindow::testScmStrokedRowFile() const
{
    if (!m_scmTree)
        return QString();
    for (int g = 0; g < m_scmTree->topLevelItemCount(); ++g) {
        QTreeWidgetItem *group = m_scmTree->topLevelItem(g);
        for (int c = 0; group && c < group->childCount(); ++c) {
            QTreeWidgetItem *item = group->child(c);
            auto *row = dynamic_cast<ScmFileRow *>(m_scmTree->itemWidget(item, 0));
            if (row && row->isSelected())
                return item->data(0, Qt::UserRole).toString();
        }
    }
    return QString();
}

QString MainWindow::testScmOutlinedFile() const
{
    if (!m_scmDiffActiveOutline || !m_scmDiffActiveOutline->isVisible())
        return QString();
    const int index = m_scmSectionKeys.indexOf(m_scmActiveSectionKey);
    return index < 0 ? QString() : m_scmSectionPaths.value(index);
}

QRect MainWindow::testScmOutlineRect() const
{
    return m_scmDiffActiveOutline ? m_scmDiffActiveOutline->geometry() : QRect();
}
#endif

void MainWindow::setScmCurrentItemSilently(QTreeWidgetItem *item)
{
    if (!m_scmTree || !item)
        return;
    QSignalBlocker block(m_scmTree);
    if (QTreeWidgetItem *previous = m_scmTree->currentItem()) {
        if (auto *row =
                dynamic_cast<ScmFileRow *>(m_scmTree->itemWidget(previous, 0)))
            row->setSelected(false);
    }
    m_scmTree->setCurrentItem(item);
    if (auto *row = dynamic_cast<ScmFileRow *>(m_scmTree->itemWidget(item, 0)))
        row->setSelected(true);
}

bool MainWindow::stepScmTreeFile(int delta)
{
    if (!m_scmTree || delta == 0)
        return false;
    QList<QTreeWidgetItem *> rows;
    for (int g = 0; g < m_scmTree->topLevelItemCount(); ++g) {
        QTreeWidgetItem *group = m_scmTree->topLevelItem(g);
        if (!group->isExpanded())
            continue;
        for (int c = 0; c < group->childCount(); ++c) {
            QTreeWidgetItem *item = group->child(c);
            if (!item->isHidden() &&
                !item->data(0, Qt::UserRole).toString().isEmpty())
                rows.append(item);
        }
    }
    if (rows.isEmpty())
        return false;

    int index = rows.indexOf(m_scmTree->currentItem());
    if (index < 0)
        index = delta > 0 ? -1 : rows.size();
    const int next = index + delta;
    if (next < 0 || next >= rows.size())
        return false;
    m_scmTree->setCurrentItem(rows.at(next));
    m_scmTree->scrollToItem(rows.at(next));
    return true;
}

QWidget *MainWindow::buildSourceControlPanel()
{
    auto *panel = new QWidget;
    m_scmPanel = panel;
    panel->setObjectName(QStringLiteral("sourceControlPanel"));
    auto *root = new QVBoxLayout(panel);
    root->setContentsMargins(6, 10, 6, 4);
    root->setSpacing(6);

    m_scmMessage = new QPlainTextEdit;
    m_scmMessage->setObjectName("scmMessageInput");
    m_scmMessage->setPlaceholderText("Message (Ctrl+Enter to commit)");
    m_scmMessage->setTabChangesFocus(true);
    m_scmMessage->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_scmMessage->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scmMessage->setFixedHeight(
        m_scmMessage->fontMetrics().lineSpacing() * 2 + 14);
    m_scmMessage->setStyleSheet(scmMessageStyleSheet());
    auto *commitShortcut =
        new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return), m_scmMessage);
    commitShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(commitShortcut, &QShortcut::activated, this, &MainWindow::scmCommit);

    m_scmGenerateButton = new QPushButton(QString::fromUtf8("\xE2\x9C\xA8 Generate"));
    m_scmGenerateButton->setCursor(Qt::PointingHandCursor);
    m_scmGenerateButton->setToolTip(
        "Draft the message from the changes using Claude or OpenAI");
    m_scmGenerateButton->setStyleSheet(Theme::remapSheet(
        QStringLiteral(
            "QPushButton{background:%1;color:%2;border:1px solid %3;"
            "border-radius:6px;padding:4px 12px;font-weight:600;}"
            "QPushButton:hover{background:%4;border-color:%5;}"
            "QPushButton:disabled{background:%6;border-color:%7;"
            "color:%8;}")
            .arg(Theme::nextOrHex(Theme::Role::SurfaceRaised,
                                  QStringLiteral("#8957e5")),
                 Theme::nextOrHex(Theme::Role::Fg, QStringLiteral("#ffffff")),
                 Theme::nextOrHex(Theme::Role::Border,
                                  QStringLiteral("#8957e5")),
                 Theme::nextOrHex(Theme::Role::SurfaceActive,
                                  QStringLiteral("#9a6ff0")),
                 Theme::nextOrHex(Theme::Role::BorderStrong,
                                  QStringLiteral("#9a6ff0")),
                 Theme::nextOrHex(Theme::Role::Surface,
                                  QStringLiteral("#6e6a86")),
                 Theme::nextOrHex(Theme::Role::Border,
                                  QStringLiteral("#6e6a86")),
                 Theme::nextOrHex(Theme::Role::FgSubtle,
                                  QStringLiteral("#d9d9e3")))));
    connect(m_scmGenerateButton, &QPushButton::clicked, this,
            &MainWindow::generateScmMessage);

    m_scmGenModel = new QComboBox;
    for (int i = 0; i < kScmAiModelCount; ++i)
        m_scmGenModel->addItem(QString::fromLatin1(kScmAiModels[i].label), i);
    m_scmGenModel->addItem(QStringLiteral("On-device (no AI)"), -1);
    m_scmGenModel->setCurrentIndex(m_scmGenModel->findData(-1));
    m_scmGenModel->setToolTip(
        "How to draft the message: on-device (no AI), or a Claude/OpenAI model");
    connect(m_scmGenModel, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { autoFillScmMessage(); });

    m_scmGenKind = new QComboBox;
    m_scmGenKind->addItem("Commit message");
    m_scmGenKind->addItem("X post");
    m_scmGenKind->setToolTip("What to generate from the changes");

    m_scmGenDuration = new QComboBox;
    m_scmGenDuration->addItem("Current changes");
    m_scmGenDuration->addItem("Past hour");
    m_scmGenDuration->addItem("All day");
    m_scmGenDuration->setToolTip("Which changes to summarise");
    connect(m_scmGenDuration, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) {
                refreshSourceControl();
            });

    m_scmCopyButton = new QPushButton("Copy");
    m_scmCopyButton->setToolTip("Copy the message to the clipboard");
    setOcticon(m_scmCopyButton, "copy", 14);
    connect(m_scmCopyButton, &QPushButton::clicked, this, [this] {
        const QString text =
            m_scmMessage ? m_scmMessage->toPlainText().trimmed() : QString();
        if (text.isEmpty()) {
            if (m_scmGenStatus)
                m_scmGenStatus->setText("Nothing to copy.");
            return;
        }
        QApplication::clipboard()->setText(text);
        if (m_scmGenStatus)
            m_scmGenStatus->setText("Copied.");
    });

    m_scmGenStatus = new QLabel;
    m_scmGenStatus->setObjectName("statusLine");
    m_scmGenStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto updateCharCount = [this] {
        if (!m_scmGenStatus || !m_scmMessage)
            return;
        const int n = m_scmMessage->toPlainText().size();
        const bool tweet = m_scmGenKind && m_scmGenKind->currentIndex() == 1;
        m_scmGenStatus->setText(tweet ? QStringLiteral("%1/280").arg(n)
                                      : QStringLiteral("%1 chars").arg(n));
    };
    connect(m_scmMessage, &QPlainTextEdit::textChanged, this, updateCharCount);
    connect(m_scmGenKind, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [updateCharCount](int) { updateCharCount(); });
    updateCharCount();

    m_scmCommitButton = new QPushButton("Commit");
    m_scmCommitButton->setObjectName("primaryButton");
    connect(m_scmCommitButton, &QPushButton::clicked, this, &MainWindow::scmCommit);
    m_scmCommitPushButton = new QPushButton("Commit && push");
    m_scmCommitPushButton->setObjectName("primaryButton");
    m_scmCommitPushButton->setProperty("scmSecondary", true);
    m_scmCommitPushButton->setToolTip(
        "Commit the staged changes, then publish them to the network mirror "
        "(or push to the upstream branch).");
    connect(m_scmCommitPushButton, &QPushButton::clicked, this,
            &MainWindow::scmCommitAndPush);
    m_scmStageCommitPushButton = new QPushButton("Stage && push");
    m_scmStageCommitPushButton->setObjectName("primaryButton");
    m_scmStageCommitPushButton->setProperty("scmSecondary", true);
    m_scmStageCommitPushButton->setToolTip(
        "Stage every change, commit them, then publish to the network mirror "
        "(or push to the upstream branch) — in one click.");
    connect(m_scmStageCommitPushButton, &QPushButton::clicked, this,
            &MainWindow::scmStageAllCommitAndPush);
    for (QPushButton *b : {m_scmCopyButton, m_scmCommitButton,
                           m_scmCommitPushButton, m_scmStageCommitPushButton}) {
        if (b == m_scmCopyButton)
            b->setObjectName("ghostButton");
        b->setProperty("buttonSize", b == m_scmCopyButton ? "sm" : "xs");
        b->setCursor(Qt::PointingHandCursor);
    }

    auto *generationMenu = new QMenu(panel);
    generationMenu->setObjectName(QStringLiteral("scmGenerationMenu"));
    auto *generationAction = new QWidgetAction(generationMenu);
    auto *generationPanel = new QWidget(generationMenu);
    generationPanel->setMinimumWidth(260);
    auto *generationLayout = new QVBoxLayout(generationPanel);
    generationLayout->setContentsMargins(10, 10, 10, 10);
    generationLayout->setSpacing(6);
    auto addGenerationField = [&](const QString &label, QWidget *field) {
        auto *caption = new QLabel(label, generationPanel);
        caption->setObjectName(QStringLiteral("statusLine"));
        generationLayout->addWidget(caption);
        field->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        generationLayout->addWidget(field);
    };
    generationLayout->addWidget(m_scmGenerateButton);
    addGenerationField(QStringLiteral("Draft with"), m_scmGenModel);
    addGenerationField(QStringLiteral("Create"), m_scmGenKind);
    addGenerationField(QStringLiteral("Include"), m_scmGenDuration);
    auto *copyStatusRow = new QHBoxLayout;
    copyStatusRow->setContentsMargins(0, 2, 0, 0);
    copyStatusRow->addWidget(m_scmCopyButton);
    copyStatusRow->addWidget(m_scmGenStatus, 1);
    generationLayout->addLayout(copyStatusRow);
    generationAction->setDefaultWidget(generationPanel);
    generationMenu->addAction(generationAction);

    auto *generationMenuButton = new QPushButton(panel);
    generationMenuButton->setObjectName(QStringLiteral("ghostButton"));
    generationMenuButton->setText(QString::fromUtf8("\xE2\x8B\xAF"));
    generationMenuButton->setToolTip(
        QStringLiteral("Message generation and copy options"));
    generationMenuButton->setMenu(generationMenu);
    generationMenuButton->setCursor(Qt::PointingHandCursor);
    generationMenuButton->setFixedWidth(kScmClassicComposeButtonWidth);
    generationMenuButton->setProperty("scmCompose", QStringLiteral("menu"));

    // Drafting the message is the one thing in that menu worth a click of its
    // own: the button inside it is two clicks deep, which is two too many for
    // the step every commit starts with. The menu keeps the model, kind and
    // duration pickers; this just runs them.
    m_scmGenerateIconButton = new QPushButton(panel);
    m_scmGenerateIconButton->setObjectName(QStringLiteral("ghostButton"));
    setOcticon(m_scmGenerateIconButton, QStringLiteral("sparkle"), 16);
    m_scmGenerateIconButton->setToolTip(
        QStringLiteral("Draft the commit message from the changes"));
    m_scmGenerateIconButton->setCursor(Qt::PointingHandCursor);
    m_scmGenerateIconButton->setFixedWidth(kScmClassicComposeButtonWidth);
    m_scmGenerateIconButton->setProperty("scmCompose", QStringLiteral("generate"));
    connect(m_scmGenerateIconButton, &QPushButton::clicked, this,
            &MainWindow::generateScmMessage);

    auto *composeRow = new QHBoxLayout;
    composeRow->setContentsMargins(0, 0, 0, 0);
    composeRow->setSpacing(4);
    composeRow->addWidget(m_scmMessage, 1);
    composeRow->addWidget(m_scmGenerateIconButton, 0, Qt::AlignTop);
    composeRow->addWidget(generationMenuButton, 0, Qt::AlignTop);
    root->addLayout(composeRow);

    m_scmControlsPanel = new QWidget;
    m_scmControlsPanel->setObjectName(QStringLiteral("scmControlsPanel"));
    auto *controlsRow = new FlowLayout(m_scmControlsPanel, 0, 6, 6);
    controlsRow->addWidget(m_scmCommitButton);
    controlsRow->addWidget(m_scmCommitPushButton);
    controlsRow->addWidget(m_scmStageCommitPushButton);

    QSizePolicy controlsPolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    controlsPolicy.setHeightForWidth(true);
    m_scmControlsPanel->setSizePolicy(controlsPolicy);
    root->addWidget(m_scmControlsPanel);

    m_scmOutgoingPanel = new QWidget(panel);
    m_scmOutgoingPanel->setObjectName(QStringLiteral("scmOutgoingPanel"));
    auto *outgoingLayout = new QVBoxLayout(m_scmOutgoingPanel);
    outgoingLayout->setContentsMargins(0, 2, 0, 4);
    outgoingLayout->setSpacing(5);
    auto *outgoingHeader = new QHBoxLayout;
    outgoingHeader->setContentsMargins(0, 0, 0, 0);
    outgoingHeader->setSpacing(6);
    auto *outgoingTitle = new QLabel(QStringLiteral("OUTGOING CHANGES"));
    outgoingTitle->setObjectName(QStringLiteral("sectionLabel"));
    outgoingHeader->addWidget(outgoingTitle);
    outgoingHeader->addStretch();
    outgoingLayout->addLayout(outgoingHeader);

    m_scmOutgoingLabel = new QLabel(m_scmOutgoingPanel);
    m_scmOutgoingLabel->setObjectName(QStringLiteral("scmOutgoingLabel"));
    m_scmOutgoingLabel->setTextFormat(Qt::RichText);
    m_scmOutgoingLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_scmOutgoingLabel->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_scmOutgoingLabel, &QWidget::customContextMenuRequested, this,
            [this](const QPoint &position) {
                if (!m_scmOutgoingLabel || m_scmOutgoingProbePendingCount <= 0)
                    return;
                QMenu menu(m_scmOutgoingLabel);
                QAction *undo = menu.addAction(
                    QStringLiteral("Undo latest commit (keep changes)"));
                undo->setIcon(themedOcticon(QStringLiteral("arrow-left"),
                                            QColor(QStringLiteral("#8b949e")), 14));
                connect(undo, &QAction::triggered, this,
                        &MainWindow::undoLatestOutgoingCommit);
                menu.exec(m_scmOutgoingLabel->mapToGlobal(position));
            });
    outgoingLayout->addWidget(m_scmOutgoingLabel);

    m_scmSyncButton = new QPushButton(QStringLiteral("Sync Changes"),
                                      m_scmOutgoingPanel);
    m_scmSyncButton->setObjectName(QStringLiteral("scmSyncButton"));
    m_scmSyncButton->setCursor(Qt::PointingHandCursor);
    m_scmSyncButton->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    m_scmSyncButton->setToolTip(
        QStringLiteral("Publish outgoing commits to the network mirror or push "
                       "them to the configured upstream branch"));
    setOcticon(m_scmSyncButton, QStringLiteral("sync"), 14);
    connect(m_scmSyncButton, &QPushButton::clicked, this,
            &MainWindow::pushCurrentRepoUpstream);
    // Publishing is several minutes of invisible work on a large repo (secret
    // scan, clone, seal, encrypt, push). The button says what state it is in and
    // the label below it says what that state is *doing* right now, fed live by
    // git's own progress output and by each stage of the mirror seal. Keep the
    // line allocated while it is empty so progress never reflows the controls.
    m_scmSyncStatus = new ElidingStatusLabel(m_scmOutgoingPanel);
    m_scmSyncStatus->setObjectName(QStringLiteral("statusLine"));
    m_scmSyncStatus->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_scmSyncStatus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    m_scmSyncStatus->setMinimumHeight(
        QFontMetrics(m_scmSyncStatus->font()).lineSpacing());
    m_scmSyncRow = new QWidget(m_scmOutgoingPanel);
    m_scmSyncRow->setProperty("flowFill", true); // see FlowLayout::doLayout
    auto *syncColumn = new QVBoxLayout(m_scmSyncRow);
    syncColumn->setContentsMargins(0, 0, 0, 0);
    syncColumn->setSpacing(0);
    syncColumn->addWidget(m_scmSyncButton, 0, Qt::AlignLeft);
    syncColumn->addWidget(m_scmSyncStatus, 0);
    controlsRow->addWidget(m_scmSyncRow);
    m_scmSyncRow->hide();
    m_scmOutgoingPanel->hide();

    auto *headerHost = new QWidget(panel);
    headerHost->setObjectName(QStringLiteral("scmChangesHeader"));
    headerHost->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    auto *header = new HeaderWrapLayout(headerHost);
    auto *title = new QLabel(QString::fromUtf8("\xE2\x96\xBE CHANGES"));
    title->setObjectName("sectionLabel");
    m_scmCountLabel = new QLabel;
    m_scmCountLabel->setObjectName("statusLine");
    m_scmViewedLabel = new QLabel;
    m_scmViewedLabel->setObjectName("statusLine");
    m_scmViewedLabel->setTextFormat(Qt::RichText);
    m_scmViewedLabel->setToolTip("Files you have scrolled all the way through; a green "
                                 "check means every file is viewed as it stands now");

    m_scmPrevButton = new QPushButton;
    m_scmPrevButton->setToolTip("Previous change");
    setOcticon(m_scmPrevButton, "chevron-up", 14);
    connect(m_scmPrevButton, &QPushButton::clicked, this,
            [this] { scmSelectAdjacentChange(-1); });
    m_scmNextButton = new QPushButton;
    m_scmNextButton->setToolTip("Next change");
    setOcticon(m_scmNextButton, "chevron-down", 14);
    connect(m_scmNextButton, &QPushButton::clicked, this,
            [this] { scmSelectAdjacentChange(1); });

    m_scmRefreshButton = new QPushButton;
    setOcticon(m_scmRefreshButton, "sync", 14);
    m_scmRefreshButton->setToolTip("Rescan the working tree for changes");
    connect(m_scmRefreshButton, &QPushButton::clicked, this,
            [this] {
                if (sourceControlShowsRange()) {
                    m_branchDiffLastValid = false;
                    forgetBranchDiff(m_branchDiffBranch); // rescan means re-read
                    renderBranchScopeDiff();
                } else {
                    refreshSourceControl(true);
                }
            });
    addRefreshSpin(m_scmRefreshButton);

    m_scmAutoViewedButton = new QPushButton;
    m_scmAutoViewedButton->setCheckable(true);
    m_scmAutoViewedButton->setChecked(autoMarkViewedOnScrollPref());
    setOcticon(m_scmAutoViewedButton, "eye", 14);
    m_scmAutoViewedButton->setToolTip(
        "Automatically mark files as viewed while scrolling");
    connect(m_scmAutoViewedButton, &QPushButton::clicked, this, [this](bool on) {
        setAutoMarkViewedOnScrollPref(on);
        if (on) {
            if (sourceControlShowsRange())
                applyBranchAutoMarkViewedOnScroll();
            else
                applyScmAutoMarkViewedOnScroll();
        }
    });

    m_scmAllReposButton = new QPushButton(QStringLiteral("All repos"));
    setOcticon(m_scmAllReposButton, QStringLiteral("diff"), 14);
    m_scmAllReposButton->setToolTip(QStringLiteral(
        "Show every local repository with working-tree changes in this diff pane"));
    connect(m_scmAllReposButton, &QPushButton::clicked, this, [this] {
        if (m_scmAllReposView) {
            m_scmAllReposView = false;
            m_scmAllReposButton->setText(QStringLiteral("All repos"));
            refreshSourceControl(true);
        } else {
            showChangedRepositoriesDiff();
        }
    });

    m_scmTreeModeButton = new QPushButton(QStringLiteral("Folders"));
    setOcticon(m_scmTreeModeButton, QStringLiteral("file-directory"), 14);
    m_scmTreeModeButton->setCheckable(true);
    m_scmTreeModeButton->setToolTip(
        QStringLiteral("Switch between flat and nested-folder changes"));
    connect(m_scmTreeModeButton, &QPushButton::toggled, this, [this](bool nested) {
        m_scmTreeFolders = nested;
        m_scmTreeModeButton->setText(nested ? QStringLiteral("Flat")
                                             : QStringLiteral("Folders"));
        if (m_scmAllReposView)
            showChangedRepositoriesDiff();
        else
            refreshSourceControl(true);
    });

    for (QPushButton *b : {m_scmPrevButton, m_scmNextButton, m_scmRefreshButton,
                           m_scmAutoViewedButton, m_scmTreeModeButton,
                           m_scmAllReposButton}) {
        b->setObjectName("ghostButton");
        b->setProperty("buttonSize", "sm");
        b->setCursor(Qt::PointingHandCursor);
    }

    // Three captions and three buttons on one line used to be what stopped the
    // Git splitter being dragged in: the row's width was the pane's minimum. It
    // wraps now, the same way the range header does -- the review counts fall
    // below the section caption once they no longer fit beside the buttons.
    auto *counts = new QWidget(panel);
    counts->setObjectName(QStringLiteral("scmChangesCounts"));
    auto *countsRow = new QHBoxLayout(counts);
    countsRow->setContentsMargins(0, 0, 0, 0);
    countsRow->setSpacing(6);
    countsRow->addWidget(m_scmCountLabel);
    countsRow->addWidget(m_scmViewedLabel);
    countsRow->addStretch();
    header->setLead(title);
    header->setTitle(counts);
    header->addAction(m_scmPrevButton);
    header->addAction(m_scmNextButton);
    header->addAction(m_scmTreeModeButton);
    header->addAction(m_scmAllReposButton);
    header->addAction(m_scmAutoViewedButton);
    root->addWidget(headerHost);

    m_scmTree = new QTreeWidget;
    m_scmTree->setObjectName("fileTree");
    m_scmTree->setColumnCount(1);
    m_scmTree->setHeaderHidden(true);
    m_scmTree->setMinimumWidth(160);
    m_scmTree->setRootIsDecorated(true);
    m_scmTree->setIndentation(10);
    m_scmTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_scmTree->setFrameShape(QFrame::NoFrame);
    m_scmTree->setItemDelegate(new ScmTreeRowDelegate(m_scmTree));
    m_scmTree->setStyleSheet(scmTreeStyleSheet());
    m_scmTree->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Ignored);
    m_scmTree->setMinimumHeight(0);
    m_scmTree->setFocusPolicy(Qt::StrongFocus);
    m_scmTree->installEventFilter(this);
    m_scmTree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_scmTree, &QWidget::customContextMenuRequested, this,
            &MainWindow::showScmFileMenu);
    connect(m_scmTree, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem *item, QTreeWidgetItem *previous) {
                if (previous) {
                    if (auto *row = dynamic_cast<ScmFileRow *>(
                            m_scmTree->itemWidget(previous, 0)))
                        row->setSelected(false);
                }
                if (!item)
                    return;
                if (auto *row = dynamic_cast<ScmFileRow *>(
                        m_scmTree->itemWidget(item, 0)))
                    row->setSelected(true);
                const QString folderPath =
                    item->data(0, Qt::UserRole + 5).toString();
                if (!folderPath.isEmpty()) {
                    QString repositoryDir =
                        item->data(0, Qt::UserRole + 4).toString();
                    if (repositoryDir.isEmpty())
                        repositoryDir = sourceControlGitDir();
                    if (repositoryDir.isEmpty())
                        return;
                    setDiffHtml(m_scmDiff, QStringLiteral(
                        "<p style='color:#8b949e'>Loading changes in %1&hellip;</p>")
                                               .arg(folderPath.toHtmlEscaped()));
                    runOffThread<QString>(
                        [repositoryDir, folderPath] {
                            return readScmPathPatch(repositoryDir, folderPath);
                        },
                        [this, repositoryDir, folderPath](const QString &patch) {
                            QList<DiffFileEntry> files;
                            const QString html = renderDiffHtml(
                                patch, files, repositoryDir, QString(), QString());
                            setDiffHtml(
                                m_scmDiff,
                                html.isEmpty()
                                    ? QStringLiteral(
                                          "<p style='color:#8b949e'>No changes in %1.</p>")
                                          .arg(folderPath.toHtmlEscaped())
                                    : html,
                                true);
                        });
                    return;
                }
                const QString path = item->data(0, Qt::UserRole).toString();
                if (path.isEmpty())
                    return; // group header
                const QString repositoryDir =
                    item->data(0, Qt::UserRole + 4).toString();
                if (!repositoryDir.isEmpty()) {
                    setDiffHtml(m_scmDiff, QStringLiteral(
                        "<p style='color:#8b949e'>Loading %1&hellip;</p>")
                                               .arg(path.toHtmlEscaped()));
                    runOffThread<QString>(
                        [repositoryDir, path] {
                            QString patch = QString::fromUtf8(gitCaptureStdout(
                                repositoryDir,
                                {QStringLiteral("diff"), QStringLiteral("HEAD"),
                                 QStringLiteral("--"), path}));
                            if (patch.isEmpty() &&
                                QFileInfo::exists(QDir(repositoryDir).filePath(path)))
                                patch = QString::fromUtf8(gitCaptureStdout(
                                    repositoryDir,
                                    {QStringLiteral("diff"),
                                     QStringLiteral("--no-index"),
                                     QStringLiteral("--"), QStringLiteral("/dev/null"),
                                     path}));
                            return patch;
                        },
                        [this, repositoryDir](const QString &patch) {
                            QList<DiffFileEntry> files;
                            setDiffHtml(m_scmDiff,
                                        renderDiffHtml(patch, files, repositoryDir,
                                                       QString(), QString()),
                                        true);
                        });
                    return;
                }
                if (item->data(0, Qt::UserRole + 3).toBool()) {
                    scrollBranchDiffToFile(path, /*moveFocus=*/false);
                    return;
                }
                showScmDiff(path, item->data(0, Qt::UserRole + 1).toBool(),
                            item->data(0, Qt::UserRole + 2).toBool());
            });

    root->addWidget(m_scmTree, 1);

    m_scmEmptyNote =
        new QLabel("No working tree on this node \xE2\x80\x94 changes are read-only here.");
    m_scmEmptyNote->setObjectName("statusLine");
    m_scmEmptyNote->setProperty("scmEmpty", true);
    m_scmEmptyNote->setAlignment(Qt::AlignCenter);
    m_scmEmptyNote->hide();
    root->addWidget(m_scmEmptyNote);

    if (m_commitsTable) {
        QAbstractItemDelegate *previous =
            m_commitsTable->itemDelegateForColumn(kCommitSummaryCol);
        m_commitsTable->setItemDelegateForColumn(
            kCommitSummaryCol,
            new ScmCommitRowDelegate(m_commitsTable,
                                     [this] { return currentRef(); }));
        if (previous)
            previous->deleteLater();
    }

    auto *chrome = new ScmNextChrome(panel);
    chrome->commitLayout = new QVBoxLayout;
    chrome->commitLayout->setContentsMargins(12, 12, 12, 12);
    chrome->commitLayout->setSpacing(8);

    auto *summaryField = new QLineEdit(panel);
    summaryField->setObjectName(QStringLiteral("scmSummaryInput"));
    summaryField->setPlaceholderText(QStringLiteral("Summary (required)"));
    summaryField->hide();
    auto *descriptionField = new QPlainTextEdit(panel);
    descriptionField->setObjectName(QStringLiteral("scmDescriptionInput"));
    descriptionField->setPlaceholderText(QStringLiteral("Description"));
    descriptionField->setTabChangesFocus(true);
    descriptionField->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    descriptionField->hide();
    composeRow->insertWidget(0, summaryField, 1);
    for (QWidget *field : {static_cast<QWidget *>(summaryField),
                           static_cast<QWidget *>(descriptionField)}) {
        auto *shortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return), field);
        shortcut->setContext(Qt::WidgetWithChildrenShortcut);
        connect(shortcut, &QShortcut::activated, this, &MainWindow::scmCommit);
    }
    const auto messageToFields = [this, chrome, summaryField, descriptionField] {
        if (chrome->syncingMessage || chrome->arranged != 1 || !m_scmMessage)
            return;
        const QString text = m_scmMessage->toPlainText();
        const int newline = text.indexOf(QLatin1Char('\n'));
        const QString first = newline < 0 ? text : text.left(newline);
        QString rest = newline < 0 ? QString() : text.mid(newline + 1);
        while (rest.startsWith(QLatin1Char('\n')))
            rest.remove(0, 1);
        chrome->syncingMessage = true;
        if (summaryField->text() != first)
            summaryField->setText(first);
        if (descriptionField->toPlainText() != rest)
            descriptionField->setPlainText(rest);
        chrome->syncingMessage = false;
    };
    const auto fieldsToMessage = [this, chrome, summaryField, descriptionField] {
        if (chrome->syncingMessage || !m_scmMessage)
            return;
        const QString description = descriptionField->toPlainText();
        const QString message =
            description.trimmed().isEmpty()
                ? summaryField->text()
                : summaryField->text() + QStringLiteral("\n\n") + description;
        if (m_scmMessage->toPlainText() == message)
            return;
        chrome->syncingMessage = true;
        m_scmMessage->setPlainText(message);
        chrome->syncingMessage = false;
    };
    connect(m_scmMessage, &QPlainTextEdit::textChanged, chrome, messageToFields);
    connect(summaryField, &QLineEdit::textEdited, chrome, fieldsToMessage);
    connect(descriptionField, &QPlainTextEdit::textChanged, chrome, fieldsToMessage);

    auto *commitRule = new QFrame(panel);
    commitRule->setObjectName(QStringLiteral("scmNextCommitRule"));
    commitRule->setFrameShape(QFrame::NoFrame);
    commitRule->setFixedHeight(1);
    commitRule->hide();

    auto *commitMore = new QPushButton(m_scmControlsPanel);
    commitMore->setObjectName(QStringLiteral("scmNextCommitMore"));
    commitMore->setCursor(Qt::PointingHandCursor);
    commitMore->setToolTip(QStringLiteral("More ways to commit"));
    commitMore->hide();
    chrome->commitMore = commitMore;
    auto *commitMenu = new QMenu(commitMore);
    commitMenu->setObjectName(QStringLiteral("scmNextCommitMenu"));
    commitMenu->setToolTipsVisible(true);
    commitMore->setMenu(commitMenu);
    connect(commitMenu, &QMenu::aboutToShow, commitMenu, [this, commitMenu] {
        commitMenu->clear();
        const auto mirror = [commitMenu](QPushButton *button, const QString &label) {
            if (!button)
                return;
            QAction *action = commitMenu->addAction(label);
            action->setEnabled(button->isEnabled());
            action->setToolTip(button->toolTip());
            QPointer<QPushButton> target(button);
            QObject::connect(action, &QAction::triggered, commitMenu, [target] {
                if (target)
                    target->click();
            });
        };
        mirror(m_scmCommitButton,
               m_scmCommitButton ? m_scmCommitButton->text() : QString());
        mirror(m_scmCommitPushButton, QStringLiteral("Commit and push"));
        mirror(m_scmStageCommitPushButton,
               QStringLiteral("Stage all, commit and push"));
    });

    chrome->mirrorEnabled = [this, summaryField, descriptionField, commitMore] {
        if (m_scmMessage) {
            summaryField->setEnabled(m_scmMessage->isEnabled());
            descriptionField->setEnabled(m_scmMessage->isEnabled());
        }
        if (m_scmCommitButton)
            commitMore->setEnabled(m_scmCommitButton->isEnabled());
    };
    chrome->watch(m_scmMessage);
    chrome->watch(m_scmCommitButton);

    connect(m_scmTreeModeButton, &QPushButton::toggled, chrome, [this, chrome](bool nested) {
        if (chrome->arranged == 1 && m_scmTree)
            m_scmTree->setIndentation(nested ? 14 : 0);
    });

    const auto arrangePanel = [this, chrome, root, composeRow, header, title,
                               headerHost, summaryField, descriptionField,
                               commitRule, commitMore, generationMenuButton,
                               messageToFields](bool next) {
        while (QLayoutItem *item = chrome->commitLayout->takeAt(0)) {
            if (!item->layout())
                delete item;
        }
        while (QLayoutItem *item = root->takeAt(0)) {
            if (!item->layout())
                delete item;
        }
        if (QLayout *old = m_scmControlsPanel->layout()) {
            while (QLayoutItem *item = old->takeAt(0))
                delete item;
            delete old;
        }
        if (next) {
            root->setContentsMargins(0, 0, 0, 0);
            root->setSpacing(0);
            root->addWidget(headerHost);
            root->addWidget(m_scmTree, 1);
            root->addWidget(m_scmEmptyNote);
            root->addWidget(commitRule);
            chrome->commitLayout->addLayout(composeRow);
            chrome->commitLayout->addWidget(descriptionField);
            chrome->commitLayout->addWidget(m_scmControlsPanel);
            root->addLayout(chrome->commitLayout);
            auto *controls = new QHBoxLayout(m_scmControlsPanel);
            controls->setContentsMargins(0, 0, 0, 0);
            controls->setSpacing(0);
            controls->addWidget(m_scmCommitButton, 1);
            controls->addWidget(commitMore);
            controls->addWidget(m_scmCommitPushButton);
            controls->addWidget(m_scmStageCommitPushButton);
            controls->addWidget(m_scmSyncRow, 1);
        } else {
            root->setContentsMargins(6, 10, 6, 4);
            root->setSpacing(6);
            root->addLayout(composeRow);
            root->addWidget(m_scmControlsPanel);
            root->addWidget(headerHost);
            root->addWidget(m_scmTree, 1);
            root->addWidget(m_scmEmptyNote);
            auto *controls = new FlowLayout(m_scmControlsPanel, 0, 6, 6);
            controls->addWidget(m_scmCommitButton);
            controls->addWidget(m_scmCommitPushButton);
            controls->addWidget(m_scmStageCommitPushButton);
            controls->addWidget(m_scmSyncRow);
        }
        header->setContentsMargins(next ? QMargins(10, 8, 8, 4) : QMargins(0, 0, 0, 0));
        scmSetShown(title, !next);
        scmSetShown(m_scmCountLabel, !next);
        scmSetShown(m_scmMessage, !next);
        summaryField->setVisible(next);
        descriptionField->setVisible(next);
        commitRule->setVisible(next);
        if (!next)
            commitMore->hide();
        for (QWidget *button :
             {static_cast<QWidget *>(m_scmGenerateIconButton),
              static_cast<QWidget *>(generationMenuButton)})
            composeRow->setAlignment(button, next ? Qt::AlignVCenter : Qt::AlignTop);
        if (next) {
            summaryField->setFixedHeight(kGitNextFieldHeight);
            descriptionField->setFixedHeight(kGitNextDescriptionHeight);
            m_scmCommitButton->setIcon(
                themedOcticon(QStringLiteral("git-commit"),
                              Theme::color(Theme::Role::OnAccent), 14));
            commitMore->setIcon(themedOcticon(QStringLiteral("chevron-down"),
                                              Theme::color(Theme::Role::OnAccent), 14));
        } else {
            m_scmCommitButton->setIcon(QIcon());
            m_scmCommitButton->setText(QStringLiteral("Commit"));
        }
        m_scmTree->setRootIsDecorated(!next);
        m_scmTree->setIndentation(next ? (m_scmTreeFolders ? 14 : 0) : 10);
        updateScmCommitControlVisibility();
        if (chrome->mirrorEnabled)
            chrome->mirrorEnabled();
        if (next)
            messageToFields();
    };

    const auto applyTab = [this, chrome](bool next) {
        const bool changes = !next || chrome->tab == 0;
        const bool history = !next || chrome->tab == 1;
        if (m_gitFilesSlot)
            scmSetShown(m_gitFilesSlot, changes);
        if (m_gitHistorySlot)
            scmSetShown(m_gitHistorySlot, history);
        if (chrome->changesTab)
            chrome->changesTab->setChecked(chrome->tab == 0);
        if (chrome->historyTab)
            chrome->historyTab->setChecked(chrome->tab == 1);
    };

    const auto arrangePage = [this, chrome, applyTab](bool next) {
        if (!chrome->installed || !chrome->actions)
            return;
        auto *history = chrome->historyHeader
                            ? dynamic_cast<HeaderWrapLayout *>(
                                  chrome->historyHeader->layout())
                            : nullptr;
        if (chrome->header)
            chrome->header->setVisible(next);
        if (chrome->side)
            chrome->side->setVisible(next);
        while (QLayoutItem *item = chrome->actions->takeAt(0))
            delete item;
        const QList<QPushButton *> moved{m_commitsFetchButton, m_commitsPullButton,
                                         m_scmRefreshButton};
        if (m_commitsBranchButton)
            m_commitsBranchButton->setMinimumWidth(next ? 112 : 0);
        if (next) {
            if (chrome->branchPair)
                chrome->actions->addWidget(chrome->branchPair);
            for (QPushButton *button : {m_commitsFetchButton, m_commitsPullButton,
                                        chrome->pushButton.data(), m_scmRefreshButton}) {
                if (button)
                    chrome->actions->addWidget(button);
            }
            for (QPushButton *button : moved)
                scmTintButtonIcon(button, Theme::Role::FgMuted);
            if (chrome->pushButton)
                scmTintButtonIcon(chrome->pushButton, Theme::Role::FgMuted);
        } else if (history) {
            for (int i = history->count() - 1; i >= 0; --i)
                delete history->takeAt(i);
            if (chrome->branchPair)
                history->setTitle(chrome->branchPair);
            for (QPushButton *button : {m_commitsFetchButton, m_commitsPullButton,
                                        m_commitsLeaderboardButton, m_scmRefreshButton})
                history->addAction(button);
            for (QPushButton *button : moved)
                applyStoredOcticon(button);
        }
        applyTab(next);
    };

    const auto buildSide = [this, chrome] {
        if (!chrome->side)
            return;
        QList<GitSideItem> items;
        QString signature;
        const auto addHeading = [&](const QString &key, const QString &caption,
                                    int count) {
            const bool isOpen = chrome->open.value(key, true);
            GitSideItem item;
            item.kind = GitSideItem::Kind::Heading;
            item.title = caption;
            item.count = count;
            item.open = isOpen;
            item.toolTip = isOpen ? QStringLiteral("Fold %1").arg(caption.toLower())
                                  : QStringLiteral("Show %1").arg(caption.toLower());
            QPointer<ScmNextChrome> owner(chrome);
            item.activate = [owner, key] {
                if (!owner)
                    return;
                owner->open[key] = !owner->open.value(key, true);
                owner->schedule();
            };
            signature += QStringLiteral("H|%1|%2|%3\x1e").arg(key).arg(count).arg(int(isOpen));
            items.append(item);
            return isOpen;
        };
        const auto addItem = [&](const GitSideItem &item) {
            signature += QStringLiteral("E|%1|%2|%3|%4|%5\x1e")
                             .arg(item.title, item.detail, item.toolTip)
                             .arg(int(item.current))
                             .arg(int(item.kind));
            items.append(item);
        };
        const auto addLink = [&](const QString &caption, QPushButton *button) {
            GitSideItem link;
            link.kind = GitSideItem::Kind::Link;
            link.title = caption;
            link.toolTip = button ? button->toolTip() : QString();
            QPointer<QPushButton> target(button);
            link.activate = [target] {
                if (target)
                    target->click();
            };
            addItem(link);
        };

        QList<QAction *> branches;
        if (QMenu *menu = m_commitsBranchButton ? m_commitsBranchButton->menu() : nullptr) {
            for (QAction *action : menu->actions()) {
                if (action->isCheckable())
                    branches.append(action);
            }
        }
        if (addHeading(QStringLiteral("branches"), QStringLiteral("Branches"),
                       branches.size())) {
            for (QAction *action : std::as_const(branches)) {
                GitSideItem item;
                item.icon = QStringLiteral("git-branch");
                item.title = action->text();
                item.current = action->isChecked();
                item.toolTip = QString::fromUtf8(
                                   "%1 \xE2\x80\x94 open its history here and its "
                                   "diff against the base")
                                   .arg(item.title);
                QPointer<QAction> target(action);
                item.activate = [target] {
                    if (target)
                        target->trigger();
                };
                addItem(item);
            }
        }

        if (QMenu *menu = m_remotesButton ? m_remotesButton->menu() : nullptr) {
            QList<QAction *> remotes;
            for (QAction *action : menu->actions()) {
                if (action->isSeparator())
                    break;
                if (action->isEnabled())
                    remotes.append(action);
            }
            if (addHeading(QStringLiteral("remotes"), QStringLiteral("Remotes"),
                           remotes.size())) {
                for (QAction *action : std::as_const(remotes)) {
                    GitSideItem item;
                    item.icon = QStringLiteral("cloud");
                    item.title = action->text().section(
                        QString::fromUtf8(" \xE2\x80\x94 "), 0, 0);
                    item.toolTip = QStringLiteral("Copy the %1 URL\n%2")
                                       .arg(item.title, action->toolTip());
                    QPointer<QAction> target(action);
                    item.activate = [target] {
                        if (target)
                            target->trigger();
                    };
                    addItem(item);
                }
            }
        }

        if (auto *worktrees = dynamic_cast<VerticalIconButton *>(m_worktreesButton)) {
            const QString localPath =
                m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size()
                    ? m_repositories.at(m_repoDetailIndex).localPath
                    : QString();
            QList<GitSideItem> rows;
            bool listed = false;
            if (m_worktreesTable && !localPath.isEmpty()) {
                const QString mainPath = QDir(localPath).absolutePath();
                const QString home = QDir::homePath();
                for (int row = 0; row < m_worktreesTable->rowCount(); ++row) {
                    QTableWidgetItem *name = m_worktreesTable->item(row, 0);
                    QTableWidgetItem *path = m_worktreesTable->item(row, 1);
                    if (!name || !path)
                        continue;
                    if (QDir(path->text()).absolutePath() == mainPath)
                        listed = true;
                    GitSideItem item;
                    item.icon = QStringLiteral("file-directory");
                    item.title = name->text();
                    QString shown = path->text();
                    if (!home.isEmpty() && shown.startsWith(home + QLatin1Char('/')))
                        shown = QLatin1Char('~') + shown.mid(home.size());
                    item.detail = QDir::toNativeSeparators(shown);
                    item.strong = true;
                    const QString branch = name->data(Qt::UserRole).toString();
                    item.activate = [this, branch] { switchToWorktree(branch); };
                    rows.append(item);
                }
            }
            const int count = listed ? int(rows.size()) : int(worktrees->badgeCount());
            if (addHeading(QStringLiteral("worktrees"), QStringLiteral("Worktrees"),
                           count)) {
                if (listed) {
                    for (const GitSideItem &item : std::as_const(rows))
                        addItem(item);
                } else {
                    addLink(QStringLiteral("Open the worktree list"), m_worktreesButton);
                }
            }
        }

        if (auto *tags = dynamic_cast<VerticalIconButton *>(m_tagsButton)) {
            if (addHeading(QStringLiteral("tags"), QStringLiteral("Tags"),
                           int(tags->badgeCount())))
                addLink(QStringLiteral("Open tags and releases"), m_tagsButton);
        }
        chrome->side->setItems(items, signature);
    };

    chrome->sync = [this, chrome, buildSide] {
        if (!chrome->installed || chrome->arranged != 1)
            return;
        const int index = m_repoDetailIndex;
        const bool haveRepo = index >= 0 && index < m_repositories.size();
        const QString localPath = haveRepo ? m_repositories.at(index).localPath : QString();
        const bool probed = haveRepo && m_scmOutgoingProbeHaveResult &&
                            m_scmOutgoingProbeBranchResolved &&
                            m_scmOutgoingProbeRepoPath == localPath;
        const int ahead = probed ? m_scmOutgoingProbePendingCount : 0;
        QStringList parts;
        if (haveRepo && !m_repositories.at(index).name.isEmpty())
            parts << m_repositories.at(index).name;
        if (probed)
            parts << QStringLiteral("%1 ahead").arg(ahead);
        if (chrome->summary)
            chrome->summary->setText(parts.join(QString::fromUtf8(" \xC2\xB7 ")));
        if (chrome->changesTab) {
            const QString count =
                m_scmCountLabel ? m_scmCountLabel->text().trimmed() : QString();
            chrome->changesTab->setText(count.isEmpty()
                                            ? QStringLiteral("Changes")
                                            : QStringLiteral("Changes (%1)").arg(count));
        }
        if (chrome->pushButton) {
            const bool busy = haveRepo && (m_pushingRepos.contains(index) ||
                                           m_syncingRepos.contains(index));
            chrome->pushButton->setText(
                probed ? QString::fromUtf8("Push  \xE2\x86\x91%1").arg(ahead)
                       : QStringLiteral("Push"));
            chrome->pushButton->setEnabled(repoHasWorkingTree() && !busy &&
                                           (!probed || ahead > 0));
        }
        if (m_scmCommitButton) {
            QString branch = headBranchFromFile(sourceControlGitDir());
            if (branch.isEmpty() && probed)
                branch = m_scmOutgoingProbeBranch;
            branch.replace(QLatin1Char('&'), QStringLiteral("&&"));
            const QString label = branch.isEmpty()
                                      ? QStringLiteral("Commit")
                                      : QStringLiteral("Commit to %1").arg(branch);
            if (m_scmCommitButton->text() != label)
                m_scmCommitButton->setText(label);
        }
        buildSide();
    };

    const auto installPage = [this, chrome, applyTab]() {
        if (chrome->installed)
            return true;
        if (!m_gitWorkspaceStack || !m_commitsBranchButton || !m_commitsStack)
            return false;
        QWidget *page = m_gitWorkspaceStack->parentWidget();
        auto *outer = page ? qobject_cast<QVBoxLayout *>(page->layout()) : nullptr;
        QWidget *pair = m_commitsBranchButton->parentWidget();
        QWidget *history = pair ? pair->parentWidget() : nullptr;
        if (!outer || outer->indexOf(m_gitWorkspaceStack) < 0 || !history ||
            history->objectName() != QLatin1String("gitHistoryHeader") ||
            !dynamic_cast<HeaderWrapLayout *>(history->layout()))
            return false;
        chrome->installed = true;
        chrome->branchPair = pair;
        chrome->historyHeader = history;

        auto *bar = new QFrame(page);
        bar->setObjectName(QStringLiteral("gitNextHeader"));
        bar->hide();
        auto *barLayout = new QVBoxLayout(bar);
        barLayout->setContentsMargins(20, 14, 20, 0);
        barLayout->setSpacing(10);
        auto *top = new QHBoxLayout;
        top->setContentsMargins(0, 0, 0, 0);
        top->setSpacing(12);
        auto *titles = new QVBoxLayout;
        titles->setContentsMargins(0, 0, 0, 0);
        titles->setSpacing(2);
        auto *pageTitle = new QLabel(QStringLiteral("Git"), bar);
        pageTitle->setObjectName(QStringLiteral("gitNextTitle"));
        auto *summary = new QLabel(bar);
        summary->setObjectName(QStringLiteral("gitNextSummary"));
        summary->setTextFormat(Qt::PlainText);
        summary->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        titles->addWidget(pageTitle);
        titles->addWidget(summary);
        top->addLayout(titles, 1);
        auto *actions = new QHBoxLayout;
        actions->setContentsMargins(0, 0, 0, 0);
        actions->setSpacing(6);
        top->addLayout(actions);
        barLayout->addLayout(top);
        auto *tabs = new QHBoxLayout;
        tabs->setContentsMargins(0, 0, 0, 0);
        tabs->setSpacing(18);
        auto *changesTab = new QPushButton(QStringLiteral("Changes"), bar);
        auto *historyTab = new QPushButton(QStringLiteral("History"), bar);
        changesTab->setToolTip(QStringLiteral("Working-tree changes and the commit box"));
        historyTab->setToolTip(QStringLiteral("The commit history of the branch shown"));
        for (QPushButton *tab : {changesTab, historyTab}) {
            tab->setObjectName(QStringLiteral("gitNextTab"));
            tab->setCheckable(true);
            tab->setCursor(Qt::PointingHandCursor);
            tabs->addWidget(tab);
        }
        tabs->addStretch(1);
        barLayout->addLayout(tabs);

        auto *push = new QPushButton(QStringLiteral("Push"), bar);
        push->setObjectName(QStringLiteral("ghostButton"));
        push->setCursor(Qt::PointingHandCursor);
        push->setToolTip(m_scmSyncButton ? m_scmSyncButton->toolTip() : QString());
        setOcticon(push, QStringLiteral("upload"), 14);
        connect(push, &QPushButton::clicked, this, &MainWindow::pushCurrentRepoUpstream);

        auto *side = new GitSidePanel(page);
        side->hide();

        const int at = outer->indexOf(m_gitWorkspaceStack);
        outer->removeWidget(m_gitWorkspaceStack);
        auto *body = new QHBoxLayout;
        body->setContentsMargins(0, 0, 0, 0);
        body->setSpacing(0);
        outer->insertWidget(at, bar);
        outer->insertLayout(at + 1, body, 1);
        outer->setSpacing(0);
        body->addWidget(side);
        body->addWidget(m_gitWorkspaceStack, 1);

        chrome->header = bar;
        chrome->side = side;
        chrome->summary = summary;
        chrome->changesTab = changesTab;
        chrome->historyTab = historyTab;
        chrome->pushButton = push;
        chrome->actions = actions;
        chrome->tab = m_commitsStack->currentIndex() == kCommitWorkspaceCommitPage ? 1 : 0;

        connect(changesTab, &QPushButton::clicked, chrome, [this, chrome, applyTab] {
            chrome->tab = 0;
            applyTab(chrome->arranged == 1);
            if (m_commitsStack &&
                m_commitsStack->currentIndex() == kCommitWorkspaceCommitPage)
                setCommitWorkspacePage(kCommitWorkspaceChangesPage);
            refreshSourceControl();
            chrome->schedule();
        });
        connect(historyTab, &QPushButton::clicked, chrome, [this, chrome, applyTab] {
            chrome->tab = 1;
            applyTab(chrome->arranged == 1);
            refreshCommitMarkersIfStale();
            if (m_commitsStack && m_commitsTable && !m_currentCommitHash.isEmpty() &&
                m_commitsStack->currentIndex() == kCommitWorkspaceChangesPage) {
                for (int row = 0; row < m_commitsTable->rowCount(); ++row) {
                    QTableWidgetItem *item = m_commitsTable->item(row, kCommitSummaryCol);
                    if (!item || item->data(kCommitRowKindRole).toInt() != 0)
                        continue;
                    const QString hash = item->data(Qt::UserRole).toString();
                    if (!hash.isEmpty() && (hash.startsWith(m_currentCommitHash) ||
                                            m_currentCommitHash.startsWith(hash))) {
                        setCommitWorkspacePage(kCommitWorkspaceCommitPage);
                        break;
                    }
                }
            }
            chrome->schedule();
        });
        connect(m_commitsStack, &QStackedWidget::currentChanged, chrome,
                [chrome, applyTab](int index) {
                    if (chrome->arranged != 1)
                        return;
                    const int tab = index == kCommitWorkspaceCommitPage ? 1 : 0;
                    if (chrome->tab == tab)
                        return;
                    chrome->tab = tab;
                    applyTab(true);
                });
        chrome->watch(m_commitsBranchButton);
        chrome->watch(m_remotesButton);
        chrome->watch(bar);
        chrome->watch(side);
        return true;
    };

    const auto applyPage = [chrome, arrangePage, installPage] {
        const bool next = chrome->arranged == 1;
        if (next && !chrome->installed && !installPage())
            return;
        if (chrome->pageArranged == int(next))
            return;
        chrome->pageArranged = int(next);
        arrangePage(next);
        chrome->schedule();
    };

    const auto applyNextLayout = [chrome, arrangePanel, applyPage](bool next) {
        if (chrome->arranged != int(next)) {
            const bool first = chrome->arranged == -1;
            chrome->arranged = int(next);
            if (!first || next)
                arrangePanel(next);
        }
        QTimer::singleShot(0, chrome, applyPage);
    };

    auto *designHook = new ScmDesignHook(
        panel, ScmDesignHook::Events::Design,
        [this, generationMenuButton, applyNextLayout](bool) {
            const bool next = Theme::isNext();
            applyNextLayout(next);
            const QString design =
                next ? QStringLiteral("next") : QStringLiteral("classic");
            if (m_scmMessage) {
                scmSetSheet(m_scmMessage, scmMessageStyleSheet());
                m_scmMessage->setFixedHeight(
                    next ? m_scmMessage->fontMetrics().lineSpacing() * 2 + 22
                         : QFontMetrics(QApplication::font(m_scmMessage))
                                   .lineSpacing() * 2 + 14);
            }
            for (QPushButton *button :
                 {m_scmGenerateIconButton, generationMenuButton}) {
                if (!button)
                    continue;
                if (next) {
                    button->setFixedSize(kScmNextComposeButton,
                                         kScmNextComposeButton);
                } else {
                    button->setMinimumHeight(0);
                    button->setMaximumHeight(QWIDGETSIZE_MAX);
                    button->setFixedWidth(kScmClassicComposeButtonWidth);
                }
            }
            if (m_scmTree) {
                scmSetSheet(m_scmTree, scmTreeStyleSheet());
                scmSyncHover(m_scmTree, next);
                if (m_scmTree->property("scmDesign").toString() != design) {
                    const bool first =
                        !m_scmTree->property("scmDesign").isValid();
                    m_scmTree->setProperty("scmDesign", design);
                    if (!first) {
                        const QList<QWidget *> rows =
                            m_scmTree->findChildren<QWidget *>(
                                QStringLiteral("scmFileRow"));
                        for (QWidget *widget : rows) {
                            if (auto *row = dynamic_cast<ScmFileRow *>(widget))
                                row->refreshStyle();
                        }
                        const QList<QWidget *> groups =
                            m_scmTree->findChildren<QWidget *>(
                                QStringLiteral("scmGroupRow"));
                        for (QWidget *widget : groups) {
                            if (auto *group = dynamic_cast<ScmGroupRow *>(widget))
                                group->arrange();
                        }
                        m_scmTree->doItemsLayout();
                    }
                }
            }
            if (m_commitsTable) {
                const int height = next ? kScmNextCommitRowHeight
                                        : kScmClassicCommitRowHeight;
                QHeaderView *rows = m_commitsTable->verticalHeader();
                if (rows->defaultSectionSize() != height)
                    rows->setDefaultSectionSize(height);
                scmSyncHover(m_commitsTable, next);
            }
            if (next) {
                for (QPushButton *button :
                     {m_scmPrevButton, m_scmNextButton, m_scmTreeModeButton,
                      m_scmAllReposButton, m_scmAutoViewedButton,
                      m_scmGenerateIconButton})
                    scmTintButtonIcon(button, Theme::Role::FgMuted);
            }
        });
    designHook->run();

    return panel;
}

// code-health: allow(deep-nested-loop, dir-walk-in-loop) innermost loop is one path's folder depth with a hash lookup; removeRecursively only runs in a confirmed Discard click handler, not per row
void MainWindow::showChangedRepositoriesDiff()
{
    if (!m_scmDiff)
        return;
    m_scmAllReposView = true;
    if (m_scmAllReposButton)
        m_scmAllReposButton->setText(QStringLiteral("Current repo"));
    QList<QPair<QString, QString>> checkouts;
    QSet<QString> seenPaths;
    for (const RepositoryRecord &repo : std::as_const(m_repositories)) {
        const QString path = QDir::cleanPath(repo.localPath.trimmed());
        if (repo.previewOnly || path.isEmpty() || seenPaths.contains(path) ||
            !QDir(path).exists(QStringLiteral(".git")))
            continue;
        seenPaths.insert(path);
        checkouts.append(qMakePair(repo.owner + QLatin1Char('/') + repo.name,
                                   path));
    }

    m_scmSectionKeys.clear();
    m_scmSectionAnchors.clear();
    m_scmSectionPaths.clear();
    m_scmDiffRenderKey.clear();
    m_scmDiffSourceKey.clear();
    setDiffHtml(m_scmDiff, QStringLiteral(
        "<p style='color:#8b949e'>Scanning all local repositories for "
        "working-tree changes&hellip;</p>"));

    runOffThread<QList<ScmRepositoryPatchRead>>(
        [checkouts] { return readChangedRepositoryPatches(checkouts); },
        [this](QList<ScmRepositoryPatchRead> changed) {
            if (!m_scmDiff)
                return;
            if (changed.isEmpty()) {
                if (m_scmTree)
                    m_scmTree->clear();
                setDiffHtml(m_scmDiff, QStringLiteral(
                    "<p style='color:#8b949e'>All local repositories are clean.</p>"));
                return;
            }
            if (m_scmTree) {
                m_scmTree->clear();
                m_scmTree->setEnabled(true);
                int fileCount = 0;
                for (const ScmRepositoryPatchRead &repo : changed) {
                    auto *repoItem = new QTreeWidgetItem(m_scmTree);
                    repoItem->setText(0, repo.label);
                    repoItem->setIcon(0, themedOcticon(
                                             QStringLiteral("repo"),
                                             QColor(QStringLiteral("#8b949e")), 14));
                    QHash<QString, QTreeWidgetItem *> folders;
                    for (int fileIndex = 0; fileIndex < repo.files.size();
                         ++fileIndex) {
                        const QString path = repo.files.at(fileIndex);
                        const QString state = repo.states.value(fileIndex);
                        const bool untracked = state == QLatin1String("??");
                        const bool staged = !untracked && !state.isEmpty() &&
                                            state.at(0) != QLatin1Char(' ');
                        const QChar statusCode =
                            untracked
                                ? QLatin1Char('?')
                                : (state.size() > 1 && state.at(1) != QLatin1Char(' ')
                                       ? state.at(1)
                                       : (state.isEmpty() ? QLatin1Char('M')
                                                          : state.at(0)));
                        QTreeWidgetItem *parent = repoItem;
                        if (m_scmTreeFolders) {
                            QString prefix;
                            const QStringList parts = path.split(QLatin1Char('/'));
                            for (int i = 0; i + 1 < parts.size(); ++i) {
                                prefix += (prefix.isEmpty() ? QString() : QStringLiteral("/")) +
                                          parts.at(i);
                                if (!folders.contains(prefix)) {
                                    auto *folder = new QTreeWidgetItem(parent);
                                    folder->setText(0, parts.at(i));
                                    folder->setIcon(0, themedOcticon(
                                                           QStringLiteral("file-directory"),
                                                           QColor(QStringLiteral("#8b949e")), 14));
                                    folder->setData(0, Qt::UserRole + 4, repo.path);
                                    folder->setData(0, Qt::UserRole + 5, prefix);
                                    folders.insert(prefix, folder);
                                }
                                parent = folders.value(prefix);
                            }
                        }
                        auto *file = new QTreeWidgetItem(parent);
                        file->setText(0, m_scmTreeFolders ? path.section('/', -1)
                                                         : path);
                        file->setIcon(0, iconForFile(path.section('/', -1)));
                        file->setToolTip(0, repo.path + QLatin1Char('/') + path);
                        file->setData(0, Qt::UserRole, path);
                        file->setData(0, Qt::UserRole + 1, staged);
                        file->setData(0, Qt::UserRole + 2, untracked);
                        file->setData(0, Qt::UserRole + 4, repo.path);

                        auto *row = new ScmFileRow;
                        auto *layout = new QHBoxLayout(row);
                        layout->setContentsMargins(4, 1, 6, 1);
                        layout->setSpacing(5);
                        auto *icon = new QLabel(row);
                        icon->setPixmap(iconForFile(path.section('/', -1)).pixmap(14, 14));
                        icon->setFixedSize(14, 14);
                        icon->setAttribute(Qt::WA_TransparentForMouseEvents);
                        layout->addWidget(icon);
                        auto *name = new QLabel(m_scmTreeFolders
                                                    ? path.section('/', -1)
                                                    : path,
                                                row);
                        name->setAttribute(Qt::WA_TransparentForMouseEvents);
                        layout->addWidget(name);
                        layout->addStretch();
                        auto *status = new QLabel(QString(statusCode), row);
                        status->setToolTip(scmStatusTip(statusCode));
                        layout->addWidget(status);
                        scmTagRowLabels(name, status, statusCode);
                        auto *actions = new QWidget(row);
                        auto *actionLayout = new QHBoxLayout(actions);
                        actionLayout->setContentsMargins(0, 0, 0, 0);
                        actionLayout->setSpacing(0);
                        const auto button = [actions](const QString &text,
                                                      const QString &tip) {
                            auto *result = new QToolButton(actions);
                            result->setText(text);
                            result->setToolTip(tip);
                            result->setAutoRaise(true);
                            result->setCursor(Qt::PointingHandCursor);
                            return result;
                        };
                        if (staged) {
                            auto *unstage = button(QString::fromUtf8("\xE2\x88\x92"),
                                                   QStringLiteral("Unstage"));
                            connect(unstage, &QToolButton::clicked, this,
                                    [this, repoDir = repo.path, path] {
                                        queueScmMutation(
                                            QStringLiteral("unstage|") + repoDir +
                                                QLatin1Char('|') + path,
                                            [repoDir, path] {
                                                return scmMutate(
                                                    repoDir,
                                                    {QStringLiteral("reset"),
                                                     QStringLiteral("HEAD"),
                                                     QStringLiteral("--"), path});
                                            },
                                            [this](const ScmMutationResult &) {
                                                if (m_scmAllReposView)
                                                    showChangedRepositoriesDiff();
                                                else
                                                    refreshSourceControl(true);
                                            });
                                    });
                            actionLayout->addWidget(unstage);
                        }
                        auto *discard = button(QString::fromUtf8("\xE2\x86\xBA"),
                                               QStringLiteral("Discard changes"));
                        connect(discard, &QToolButton::clicked, this,
                                [this, repoDir = repo.path, path, untracked] {
                                    if (QMessageBox::warning(
                                            this, QStringLiteral("Discard changes"),
                                            QStringLiteral("Discard changes to \"%1\"? "
                                                           "This cannot be undone.")
                                                .arg(path),
                                            QMessageBox::Discard |
                                                QMessageBox::Cancel) !=
                                        QMessageBox::Discard)
                                        return;
                                    const auto finish = [this, path](
                                                            bool changed,
                                                            const QString &error) {
                                        if (!changed)
                                            QMessageBox::warning(
                                                this, QStringLiteral("Discard changes"),
                                                error.isEmpty()
                                                    ? QStringLiteral("Could not discard %1.")
                                                          .arg(path)
                                                    : error.left(300));
                                        if (m_scmAllReposView)
                                            showChangedRepositoriesDiff();
                                        else
                                            refreshSourceControl(true);
                                    };
                                    if (untracked) {
                                        const QString absolute =
                                            QDir(repoDir).absoluteFilePath(path);
                                        if (!QFileInfo(absolute).isDir()) {
                                            finish(QFile::remove(absolute), QString());
                                            return;
                                        }
                                        queueScmMutation(
                                            QStringLiteral("discard|") + repoDir +
                                                QLatin1Char('|') + path,
                                            [repoDir, path] {
                                                return scmMutate(
                                                    repoDir,
                                                    {QStringLiteral("clean"),
                                                     QStringLiteral("-fd"),
                                                     QStringLiteral("--"), path});
                                            },
                                            [finish](const ScmMutationResult &result) {
                                                finish(result.ok, result.error);
                                            });
                                        return;
                                    }
                                    queueScmMutation(
                                        QStringLiteral("discard|") + repoDir +
                                            QLatin1Char('|') + path,
                                        [repoDir, path] {
                                            return scmMutate(
                                                repoDir,
                                                {QStringLiteral("restore"),
                                                 QStringLiteral("--source=HEAD"),
                                                 QStringLiteral("--staged"),
                                                 QStringLiteral("--worktree"),
                                                 QStringLiteral("--"), path});
                                        },
                                        [finish](const ScmMutationResult &result) {
                                            finish(result.ok, result.error);
                                        });
                                });
                        actionLayout->addWidget(discard);
                        if (!staged) {
                            auto *stage = button(QStringLiteral("+"),
                                                 QStringLiteral("Stage"));
                            connect(stage, &QToolButton::clicked, this,
                                    [this, repoDir = repo.path, path] {
                                        queueScmMutation(
                                            QStringLiteral("stage|") + repoDir +
                                                QLatin1Char('|') + path,
                                            [repoDir, path] {
                                                return scmMutate(
                                                    repoDir,
                                                    {QStringLiteral("add"),
                                                     QStringLiteral("-A"),
                                                     QStringLiteral("--"), path});
                                            },
                                            [this](const ScmMutationResult &) {
                                                if (m_scmAllReposView)
                                                    showChangedRepositoriesDiff();
                                                else
                                                    refreshSourceControl(true);
                                            });
                                    });
                            actionLayout->addWidget(stage);
                        }
                        auto *open = new QToolButton(actions);
                        open->setIcon(themedOcticon(QStringLiteral("file"),
                                                   QColor(QStringLiteral("#8b949e")), 14));
                        open->setToolTip(QStringLiteral("Open file"));
                        open->setAutoRaise(true);
                        connect(open, &QToolButton::clicked, this,
                                [repoDir = repo.path, path] {
                                    QDesktopServices::openUrl(QUrl::fromLocalFile(
                                        QDir(repoDir).absoluteFilePath(path)));
                                });
                        actionLayout->addWidget(open);
                        layout->addWidget(actions);
                        row->setActionsWidget(actions);
                        row->onClicked = [this, file] {
                            m_scmTree->setCurrentItem(file);
                        };
                        m_scmTree->setItemWidget(file, 0, row);
                        ++fileCount;
                    }
                    repoItem->setExpanded(true);
                }
                if (m_scmCountLabel)
                    m_scmCountLabel->setText(QString::number(fileCount));
            }
            QString html;
            for (const ScmRepositoryPatchRead &repo : changed) {
                html += QStringLiteral(
                            "<h2 style='margin:22px 0 8px;color:%3'>%1"
                            "</h2><p style='color:#8b949e'>%2</p>")
                            .arg(repo.label.toHtmlEscaped(),
                                 QDir::toNativeSeparators(repo.path).toHtmlEscaped(),
                                 Theme::nextOrHex(Theme::Role::Fg,
                                                  QStringLiteral("#f0f3f6")));
                QList<DiffFileEntry> files;
                const QString rendered = renderDiffHtml(
                    repo.patch, files, repo.path, QString(), QString());
                html += rendered.isEmpty()
                            ? QStringLiteral(
                                  "<p style='color:#8b949e'>Only untracked or "
                                  "metadata changes were found.</p>")
                            : rendered;
            }
            setDiffHtml(m_scmDiff, html, /*endCap=*/true);
        });
}

void MainWindow::clearRangeFilesInSourceControl()
{
    if (!m_scmTree)
        return;
    if (m_scmOutgoingPanel)
        m_scmOutgoingPanel->hide();
    m_scmStatusCache.clear(); // returning to main must rebuild its working tree
    m_scmTree->clear();
    if (m_scmEmptyNote)
        m_scmEmptyNote->hide();
    if (m_scmCountLabel)
        m_scmCountLabel->clear();
    if (m_scmViewedLabel)
        m_scmViewedLabel->clear();
    scmNextChromeSync(m_scmPanel);
}

// The review counter beside the range's file tree. Split out of the tree build
// so folding one file in place can restate it without rebuilding the tree.
void MainWindow::updateRangeViewedLabel(const QStringList &paths)
{
    if (!m_scmViewedLabel)
        return;
    const QSet<QString> viewed = loadDiffViewed(m_branchDiffViewedContext);
    int seen = 0;
    for (const QString &path : paths)
        if (viewed.contains(path))
            ++seen;
    m_scmViewedLabel->setText(diffViewedCountHtml(
        seen, paths.size(), diffViewedMarksCurrent(m_branchDiffViewedContext, paths)));
}

namespace {
constexpr int kScmRangeRowsPerBatch = 60;
constexpr int kScmRangeRowsFirstBatchMin = 8;
constexpr qint64 kScmRangeBatchBudgetMs = 12;
}

void MainWindow::showRangeFilesInSourceControl(const QStringList &paths,
                                               const QStringList &statuses)
{
    if (!m_scmTree || m_branchDiffBranch.isEmpty())
        return;

    if (m_scmOutgoingPanel)
        m_scmOutgoingPanel->hide();

    m_scmStatusCache.clear(); // returning to main must rebuild its working tree
    m_scmTree->setEnabled(true); // range files remain reviewable on read-only mirrors
    if (m_scmEmptyNote)
        m_scmEmptyNote->hide();
    const QString previousPath =
        m_scmTree->currentItem()
            ? m_scmTree->currentItem()->data(0, Qt::UserRole).toString()
            : QString();
    // A render that lists the same files against the same base — a Viewed tick
    // the diff could not fold in place, a restyle, a poll — has nothing to say
    // to the tree. Tearing down and recreating nine widgets per file made the
    // whole list blink and lose its scroll position on every such click. The
    // key rides the group row, so anything else that clears the tree drops it.
    const QString rowsKey = m_branchDiffBranch + QLatin1Char('\x1f') +
                            branchCompareBase() + QLatin1Char('\x1f') +
                            paths.join(QLatin1Char('\x1e')) + QLatin1Char('\x1f') +
                            statuses.join(QLatin1Char('\x1e'));
    const QTreeWidgetItem *shown =
        m_scmTree->topLevelItemCount() == 1 ? m_scmTree->topLevelItem(0) : nullptr;
    const bool sameRows = !paths.isEmpty() && shown &&
                          shown->childCount() == paths.size() &&
                          shown->data(0, Qt::UserRole + 6).toString() == rowsKey;
    // Nine widgets per file are created below; one layout/paint pass at the
    // end instead of one per row.
    TableRepaintGuard repaintGuard(m_scmTree);
    if (!sameRows)
        m_scmTree->clear();

    if (!paths.isEmpty() && !sameRows) {
        auto *group = new QTreeWidgetItem(m_scmTree);
        group->setFirstColumnSpanned(true);
        group->setData(0, Qt::UserRole + 6, rowsKey);
        auto *groupWidget = new ScmGroupRow;
        auto *groupLayout = new QHBoxLayout(groupWidget);
        groupLayout->setContentsMargins(0, 0, 6, 0);
        auto *label = new QLabel(
            QStringLiteral("Changes against %1 (%2)")
                .arg(branchCompareBase())
                .arg(paths.size()),
            groupWidget);
        QFont font = label->font();
        font.setBold(true);
        label->setFont(font);
        scmTagGroupRow(groupWidget, label);
        groupLayout->addWidget(label);
        groupLayout->addStretch();
        auto *openAll = new QToolButton(groupWidget);
        openAll->setIcon(themedOcticon("diff", Theme::remapColor(QColor("#8b949e")), 14));
        openAll->setToolTip(QStringLiteral("Open all branch changes"));
        openAll->setAutoRaise(true);
        openAll->setCursor(Qt::PointingHandCursor);
        connect(openAll, &QToolButton::clicked, this, [this, paths] {
            if (!paths.isEmpty())
                scrollBranchDiffToFile(paths.first());
        });
        groupLayout->addWidget(openAll);
        groupWidget->setParts(label, label->text(),
                              QStringLiteral("AGAINST %1 %2")
                                  .arg(branchCompareBase())
                                  .arg(paths.size()),
                              {}, {});
        m_scmTree->setItemWidget(group, 0, groupWidget);

        QFont statusFont = m_scmTree->font();
        statusFont.setBold(true);
        const auto addRow = [this, group, statusFont, paths, statuses](int i) {
            const QString path = paths.at(i);
            const QString status = statuses.value(i, QStringLiteral("modified"));
            QChar statusCode = QLatin1Char('M');
            if (status == QLatin1String("added"))
                statusCode = QLatin1Char('A');
            else if (status == QLatin1String("deleted"))
                statusCode = QLatin1Char('D');
            else if (status == QLatin1String("renamed"))
                statusCode = QLatin1Char('R');

            auto *item = new QTreeWidgetItem(group);
            item->setFirstColumnSpanned(true);
            item->setToolTip(0, path);
            item->setData(0, Qt::UserRole, path);
            item->setData(0, Qt::UserRole + 3, true); // branch-range row

            auto *row = new ScmFileRow;
            new QHBoxLayout(row);
            ScmRowParts parts;
            auto *fileIcon = new QLabel(row);
            fileIcon->setPixmap(iconForFile(path.section('/', -1)).pixmap(14, 14));
            fileIcon->setFixedSize(14, 14);
            fileIcon->setAttribute(Qt::WA_TransparentForMouseEvents);
            parts.icon = fileIcon;

            const QString fileName = path.section('/', -1);
            QString directory = path.left(path.size() - fileName.size());
            if (directory.endsWith('/'))
                directory.chop(1);
            auto *fileLabel = new QLabel(fileName, row);
            fileLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
            parts.name = fileLabel;
            if (!directory.isEmpty()) {
                auto *directoryLabel = new QLabel(directory, row);
                directoryLabel->setObjectName(QStringLiteral("statusLine"));
                directoryLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
                parts.directory = directoryLabel;
                parts.directoryText = directory;
            }

            auto *statusLabel = new QLabel(QString(statusCode), row);
            statusLabel->setToolTip(scmStatusTip(statusCode));
            statusLabel->setFont(statusFont); // bold, resolved once above
            parts.status = statusLabel;
            scmTagRowLabels(fileLabel, statusLabel, statusCode);

            auto *actions = new QWidget(row);
            auto *actionsLayout = new QHBoxLayout(actions);
            actionsLayout->setContentsMargins(0, 0, 0, 0);
            auto *open = new QToolButton(actions);
            open->setIcon(themedOcticon("file", Theme::remapColor(QColor("#8b949e")), 14));
            open->setToolTip(QStringLiteral("Open file"));
            open->setAutoRaise(true);
            open->setCursor(Qt::PointingHandCursor);
            connect(open, &QToolButton::clicked, this,
                    [this, path] { openRepoFile(path); });
            actionsLayout->addWidget(open);
            row->setActionsWidget(actions);
            parts.actions = actions;
            row->setParts(parts);
            row->onClicked = [this, item, path] {
                m_scmTree->setCurrentItem(item);
                scrollBranchDiffToFile(path, /*moveFocus=*/false);
                m_scmTree->setFocus(Qt::MouseFocusReason);
            };
            m_scmTree->setItemWidget(item, 0, row);
        };
        QElapsedTimer firstBudget;
        firstBudget.start();
        int firstBatch = 0;
        while (firstBatch < paths.size() && firstBatch < kScmRangeRowsPerBatch &&
               (firstBatch < kScmRangeRowsFirstBatchMin ||
                firstBudget.elapsed() < kScmRangeBatchBudgetMs))
            addRow(firstBatch++);
        if (firstBatch < paths.size()) {
            auto step = std::make_shared<std::function<void(int)>>();
            const std::weak_ptr<std::function<void(int)>> weakStep = step;
            *step = [this, group, rowsKey, previousPath, addRow,
                     total = int(paths.size()), weakStep](int from) {
                if (!m_scmTree || m_scmTree->topLevelItemCount() != 1 ||
                    m_scmTree->topLevelItem(0) != group ||
                    group->data(0, Qt::UserRole + 6).toString() != rowsKey ||
                    group->childCount() != from)
                    return;
                int to = from;
                {
                    TableRepaintGuard repaintGuard(m_scmTree);
                    QElapsedTimer budget;
                    budget.start();
                    while (to < total && to - from < kScmRangeRowsPerBatch &&
                           (to == from ||
                            budget.elapsed() < kScmRangeBatchBudgetMs))
                        addRow(to++);
                }
                if (to < total) {
                    if (const auto next = weakStep.lock())
                        QTimer::singleShot(0, m_scmTree, [next, to] { (*next)(to); });
                    return;
                }
                fitTreeToWidestEntry(m_scmTree);
                if (!previousPath.isEmpty() &&
                    m_scmTree->currentItem() == group->child(0)) {
                    for (int c = 0; c < group->childCount(); ++c) {
                        if (group->child(c)->data(0, Qt::UserRole).toString() ==
                            previousPath) {
                            setScmCurrentItemSilently(group->child(c));
                            m_branchActiveFile = previousPath;
                            break;
                        }
                    }
                }
                updateBranchDiffActiveOutline();
            };
            QTimer::singleShot(0, m_scmTree, [step, firstBatch] { (*step)(firstBatch); });
        }
        group->setExpanded(true);

        QTreeWidgetItem *restore = group->child(0);
        if (!previousPath.isEmpty()) {
            for (int c = 0; c < group->childCount(); ++c) {
                if (group->child(c)->data(0, Qt::UserRole).toString() ==
                    previousPath) {
                    restore = group->child(c);
                    break;
                }
            }
        }
        if (restore) {
            setScmCurrentItemSilently(restore);
            m_branchActiveFile = restore->data(0, Qt::UserRole).toString();
            updateBranchDiffActiveOutline();
        }
    }

    if (!sameRows)
        fitTreeToWidestEntry(m_scmTree);
    if (m_scmCountLabel)
        m_scmCountLabel->setText(paths.isEmpty() ? QString()
                                                  : QString::number(paths.size()));
    scmNextChromeSync(m_scmPanel);
    updateRangeViewedLabel(paths);

    // The commit buttons follow the working tree's dirtiness. That used to be
    // one more synchronous `git status` on the GUI thread at the end of every
    // range render (~700 ms samples on wide diffs); seed the state from the
    // last badge probe and let a detached status confirm it.
    const auto applyDirty = [this](bool dirty) {
        for (QPushButton *button : {m_scmCommitButton, m_scmCommitPushButton,
                                    m_scmStageCommitPushButton})
            if (button)
                button->setEnabled(dirty);
        setScmGenerateEnabled(dirty ||
                              (m_scmGenDuration &&
                               m_scmGenDuration->currentIndex() > 0));
    };
    applyDirty(m_scmPendingChangeCount > 0);
    const QString dir = sourceControlGitDir();
    if (!dir.isEmpty()) {
        const int forIndex = m_repoDetailIndex;
        runGitDetached(dir, {QStringLiteral("status"), QStringLiteral("--porcelain=v1"),
                             QStringLiteral("-z")},
                       [this, forIndex, applyDirty](bool ok, const QByteArray &out) {
                           if (!ok || forIndex != m_repoDetailIndex)
                               return;
                           applyDirty(!out.isEmpty());
                       });
    }
}

// The compose row's sparkle and the menu's "Generate" run the same request, so
// they go dim and come back together.
void MainWindow::setScmGenerateEnabled(bool enabled)
{
    for (QPushButton *button : {m_scmGenerateButton, m_scmGenerateIconButton})
        if (button)
            button->setEnabled(enabled);
}

static int scmChangeCount(const QByteArray &porcelain)
{
    int count = 0;
    const QList<QByteArray> fields = porcelain.split('\0');
    for (int i = 0; i < fields.size(); ++i) {
        const QByteArray f = fields.at(i);
        if (f.size() < 3)
            continue; // "XY <path>"
        const char x = f.at(0), y = f.at(1);
        if (x == 'R' || x == 'C')
            ++i;
        if (x == '?' && y == '?') {
            ++count;
            continue;
        }
        if (x != ' ')
            ++count;
        if (y != ' ')
            ++count;
    }
    return count;
}

void MainWindow::scheduleRepoChangeBadgeRefresh()
{
    if (m_repoChangeBadgeRefreshQueued)
        return;
    m_repoChangeBadgeRefreshQueued = true;
    QTimer::singleShot(0, this, [this] {
        m_repoChangeBadgeRefreshQueued = false;
        refreshRepoChangeBadge();
    });
}

void MainWindow::refreshRepoChangeBadge()
{
    if (!m_railGitButton)
        return;
    const QString dir = repoChangeBadgeGitDir();
    if (dir.isEmpty() || !repoHasWorkingTree()) {
        m_railGitButton->setBadgeCount(0);
        return;
    }
    const int forIndex = m_repoDetailIndex;
    runGitDetached(dir,
                   {QStringLiteral("status"), QStringLiteral("--porcelain=v1"),
                    QStringLiteral("-z")},
                   [this, forIndex, dir](bool ok, const QByteArray &out) {
                       if (!ok || !m_railGitButton ||
                           m_repoDetailIndex != forIndex ||
                           repoChangeBadgeGitDir() != dir)
                           return;
                       const int changed = scmChangeCount(out);
                       m_railGitButton->setBadgeCount(changed);
                       m_scmPendingChangeCount = changed;
                       updateScmCommitControlVisibility();
                       const QByteArray scanKey = dir.toUtf8() + '\0' + out;
                       if (scanKey != m_scmStatusCache && m_scmTree &&
                           m_scmTree->isVisible())
                           refreshSourceControl();
                   });
}

void MainWindow::refreshSourceControl()
{
    refreshSourceControl(/*force=*/false);
}

void MainWindow::showSourceControlLoading(const QString &branch)
{
    ++m_scmStatusGeneration;
    ++m_scmPatchGeneration;
    m_scmStatusCache.clear();
    m_scmPatchValid = false;
    m_scmSecretFindings.clear();
    if (m_scmSecretBanner)
        m_scmSecretBanner->clearFindings();
    m_scmDiffRenderKey.clear();
    m_scmDiffSourceKey.clear();
    m_scmSectionKeys.clear();
    m_scmSectionAnchors.clear();
    m_scmSectionPaths.clear();
    m_scmStickyLabelHtml.clear();
    m_scmStickyActionHtml.clear();
    m_scmPendingScrollKey.clear();
    m_scmActiveSectionKey.clear();
    m_scmFileTops.clear();
    m_scmStickySection.clear();
    if (m_scmStickyHeader)
        m_scmStickyHeader->hide();
    if (m_scmNextFileBar)
        m_scmNextFileBar->hide();
    if (m_scmDiffActiveOutline)
        m_scmDiffActiveOutline->hide();
    if (m_scmTree) {
        QSignalBlocker block(m_scmTree);
        m_scmTree->clear();
        m_scmTree->setEnabled(false);
    }
    if (m_scmCountLabel)
        m_scmCountLabel->setText(QString::fromUtf8("Loading changes\xE2\x80\xA6"));
    if (m_scmViewedLabel)
        m_scmViewedLabel->clear();
    if (m_scmDiff) {
        const QString label = branch.isEmpty() ? QStringLiteral("repository")
                                               : branch;
        setDiffHtml(
            m_scmDiff,
            QString::fromUtf8("<p style='color:#8b949e'>Loading changes on %1\xE2\x80\xA6</p>")
                .arg(label.toHtmlEscaped()));
    }
}

void MainWindow::refreshSourceControl(bool force)
{
    if (deferUiRefresh(force ? UiRefreshSourceControlForce
                             : UiRefreshSourceControl))
        return;
    if (!m_scmTree)
        return;
    m_scmAllReposView = false;
    if (m_scmAllReposButton)
        m_scmAllReposButton->setText(QStringLiteral("All repos"));
    if (force)
        refreshRepoSyncIndicators();
    if (sourceControlShowsRange()) {
        ++m_scmStatusGeneration; // discard a working-tree scan for the old view
        if (m_scmOutgoingPanel)
            m_scmOutgoingPanel->hide();
        if (force) {
            m_branchDiffLastValid = false;
            forgetBranchDiff(m_branchDiffBranch); // rescan means re-read
            renderBranchScopeDiff();
        }
        return;
    }
    const QString dir = sourceControlGitDir();
    const bool canWrite = !dir.isEmpty() && repoHasWorkingTree();
    const quint64 generation = ++m_scmStatusGeneration;
    refreshSourceControlOutgoing();
    if (m_scmEmptyNote)
        m_scmEmptyNote->setVisible(!canWrite);
    for (QWidget *w : {static_cast<QWidget *>(m_scmMessage),
                       static_cast<QWidget *>(m_scmTree)})
        if (w)
            w->setEnabled(canWrite);

    if (!canWrite) {
        m_scmStatusCache.clear();
        ++m_scmPatchGeneration;
        m_scmPatchValid = false;
        m_scmDiffRenderKey.clear();
        m_scmDiffSourceKey.clear();
        m_scmSectionKeys.clear();
        m_scmSectionAnchors.clear();
        m_scmSectionPaths.clear();
        m_scmStickyLabelHtml.clear();
        m_scmStickyActionHtml.clear();
        m_scmPendingScrollKey.clear();
        m_scmActiveSectionKey.clear();
        m_scmFileTops.clear();
        m_scmStickySection.clear();
        if (m_scmStickyHeader)
            m_scmStickyHeader->hide();
        if (m_scmNextFileBar)
            m_scmNextFileBar->hide();
        if (m_scmDiffActiveOutline)
            m_scmDiffActiveOutline->hide();
        m_scmTree->clear();
        if (m_scmDiff)
            m_scmDiff->clear();
        if (m_scmCountLabel)
            m_scmCountLabel->clear();
        if (m_scmViewedLabel)
            m_scmViewedLabel->clear();
        scmNextChromeSync(m_scmPanel);
        if (m_railGitButton)
            m_railGitButton->setBadgeCount(0);
        m_scmPendingChangeCount = 0;
        updateScmCommitControlVisibility();
        if (m_scmCommitButton)
            m_scmCommitButton->setEnabled(false);
        if (m_scmCommitPushButton)
            m_scmCommitPushButton->setEnabled(false);
        if (m_scmStageCommitPushButton)
            m_scmStageCommitPushButton->setEnabled(false);
        setScmGenerateEnabled(false);
        return;
    }

    runGitDetached(
        dir, {"status", "--porcelain=v1", "--untracked-files=all", "-z"},
        [this, force, dir, generation](bool ok, const QByteArray &out) {
            if (generation != m_scmStatusGeneration ||
                sourceControlShowsRange() || !m_scmTree)
                return;
            if (!ok)
                return; // retain the last known-good tree on a failed scan

    const QByteArray scanKey = dir.toUtf8() + '\0' + out;
    if (!force && scanKey == m_scmStatusCache) {
        // A clean tree has no rows, but its empty-state leaderboard is allowed
        // to refresh. Re-render that cached empty patch without invalidating a
        // slow patch/message worker for an unchanged periodic status result.
        if (m_scmTree->topLevelItemCount() == 0)
            renderScmCombinedDiff();
        return;
    }
    m_scmStatusCache = scanKey;
    ++m_scmPatchGeneration;
    m_scmPatchValid = false; // the tree changed, so the combined patch is stale

    QString prevPath;
    bool prevStaged = false;
    if (QTreeWidgetItem *cur = m_scmTree->currentItem()) {
        prevPath = cur->data(0, Qt::UserRole).toString();
        prevStaged = cur->data(0, Qt::UserRole + 1).toBool();
    }

    m_scmTree->clear();

    struct Row {
        QString path;
        QChar status;
        bool staged;
        bool untracked;
    };
    QList<Row> staged, changes;
    const QList<QByteArray> fields = out.split('\0');
    for (int i = 0; i < fields.size(); ++i) {
        const QByteArray f = fields.at(i);
        if (f.size() < 3)
            continue; // "XY <path>"
        const char x = f.at(0), y = f.at(1);
        const QString path = QString::fromUtf8(f.mid(3));
        if (x == 'R' || x == 'C')
            ++i;
        if (x == '?' && y == '?') {
            changes.append({path, QChar('?'), false, true});
            continue;
        }
        if (x != ' ')
            staged.append({path, QChar(x), true, false});
        if (y != ' ')
            changes.append({path, QChar(y), false, false});
    }

    auto addGroup = [this](const QString &name, const QList<Row> &rows,
                           bool stagedGroup) {
        if (rows.isEmpty())
            return;
        auto *group = new QTreeWidgetItem(m_scmTree);
        group->setFirstColumnSpanned(true);

        auto *gw = new ScmGroupRow;
        auto *gh = new QHBoxLayout(gw);
        gh->setContentsMargins(0, 0, 6, 0);
        gh->setSpacing(0);
        auto *gl =
            new QLabel(QStringLiteral("%1 (%2)").arg(name).arg(rows.size()), gw);
        QFont gf = gl->font();
        gf.setBold(true);
        gl->setFont(gf);
        scmTagGroupRow(gw, gl);
        gh->addWidget(gl);
        gh->addStretch();
        auto groupBtn = [&](const QString &glyph, const QString &tip) {
            auto *b = new QToolButton(gw);
            b->setText(glyph);
            b->setToolTip(tip);
            b->setAutoRaise(true);
            b->setCursor(Qt::PointingHandCursor);
            return b;
        };
        auto groupTextBtn = [&](const QString &text, const QString &tip) {
            auto *b = new QPushButton(text, gw);
            b->setObjectName(QStringLiteral("scmGroupAction"));
            b->setToolTip(tip);
            b->setFlat(true);
            b->setCursor(Qt::PointingHandCursor);
            b->setFocusPolicy(Qt::NoFocus);
            b->hide();
            return b;
        };
        auto *openAll = new QToolButton(gw);
        openAll->setIcon(themedOcticon("diff", Theme::remapColor(QColor("#8b949e")), 14));
        openAll->setToolTip("Open all changes");
        openAll->setAutoRaise(true);
        openAll->setCursor(Qt::PointingHandCursor);
        connect(openAll, &QToolButton::clicked, this,
                [this, stagedGroup] { showScmDiffAll(stagedGroup); });
        gh->addWidget(openAll);
        QList<QWidget *> classicOnly;
        QList<QWidget *> nextOnly;
        if (stagedGroup) {
            auto *u =
                groupBtn(QString::fromUtf8("\xE2\x88\x92"), "Unstage all changes");
            connect(u, &QToolButton::clicked, this, &MainWindow::scmUnstageAll);
            gh->addWidget(u);
            auto *unstageAll =
                groupTextBtn(QStringLiteral("Unstage all"), "Unstage all changes");
            connect(unstageAll, &QPushButton::clicked, this, &MainWindow::scmUnstageAll);
            gh->addWidget(unstageAll);
            classicOnly << u;
            nextOnly << unstageAll;
        } else {
            auto *d =
                groupBtn(QString::fromUtf8("\xE2\x86\xBA"), "Discard all changes");
            connect(d, &QToolButton::clicked, this, &MainWindow::scmDiscardAll);
            auto *s = groupBtn(QStringLiteral("+"), "Stage all changes");
            connect(s, &QToolButton::clicked, this, &MainWindow::scmStageAll);
            gh->addWidget(d);
            gh->addWidget(s);
            auto *stageAll =
                groupTextBtn(QStringLiteral("Stage all"), "Stage all changes");
            connect(stageAll, &QPushButton::clicked, this, &MainWindow::scmStageAll);
            gh->addWidget(stageAll);
            classicOnly << s;
            nextOnly << stageAll;
        }
        gw->setParts(gl, QStringLiteral("%1 (%2)").arg(name).arg(rows.size()),
                     QStringLiteral("%1 %2")
                         .arg(stagedGroup ? QStringLiteral("STAGED")
                                          : QStringLiteral("UNSTAGED"))
                         .arg(rows.size()),
                     classicOnly, nextOnly);
        m_scmTree->setItemWidget(group, 0, gw);

        QHash<QString, QTreeWidgetItem *> folders;
        for (const Row &r : rows) {
            QTreeWidgetItem *parent = group;
            if (m_scmTreeFolders) {
                QString prefix;
                const QStringList parts = r.path.split(QLatin1Char('/'));
                for (int i = 0; i + 1 < parts.size(); ++i) {
                    prefix += (prefix.isEmpty() ? QString() : QStringLiteral("/")) +
                              parts.at(i);
                    if (!folders.contains(prefix)) {
                        auto *folder = new QTreeWidgetItem(parent);
                        folder->setText(0, parts.at(i));
                        folder->setIcon(0, themedOcticon(
                                               QStringLiteral("file-directory"),
                                               QColor(QStringLiteral("#8b949e")), 14));
                        folder->setData(0, Qt::UserRole + 5, prefix);
                        folder->setExpanded(true);
                        folders.insert(prefix, folder);
                    }
                    parent = folders.value(prefix);
                }
            }
            auto *item = new QTreeWidgetItem(parent);
            item->setFirstColumnSpanned(true);
            item->setToolTip(0, r.path);
            item->setData(0, Qt::UserRole, r.path);
            item->setData(0, Qt::UserRole + 1, r.staged);
            item->setData(0, Qt::UserRole + 2, r.untracked);

            auto *w = new ScmFileRow;
            new QHBoxLayout(w);
            ScmRowParts parts;

            auto *fileIcon = new QLabel(w);
            fileIcon->setPixmap(
                iconForFile(r.path.section('/', -1)).pixmap(14, 14));
            fileIcon->setFixedSize(14, 14);
            fileIcon->setAttribute(Qt::WA_TransparentForMouseEvents);
            parts.icon = fileIcon;

            const QString fileName = r.path.section('/', -1);
            QString directory = r.path.left(r.path.size() - fileName.size());
            if (directory.endsWith('/'))
                directory.chop(1);
            auto *fileLabel = new QLabel(fileName, w);
            fileLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
            parts.name = fileLabel;
            if (!directory.isEmpty()) {
                auto *directoryLabel = new QLabel(directory, w);
                directoryLabel->setObjectName("statusLine");
                directoryLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
                parts.directory = directoryLabel;
                parts.directoryText = directory;
            }

            auto *statusLabel = new QLabel(QString(r.status), w);
            statusLabel->setToolTip(scmStatusTip(r.status));
            QFont sf = statusLabel->font();
            sf.setBold(true);
            statusLabel->setFont(sf);
            parts.status = statusLabel;
            scmTagRowLabels(fileLabel, statusLabel, r.status);
            if (Theme::isNext()) {
                auto *check = new QCheckBox(w);
                check->setObjectName(QStringLiteral("scmStageCheck"));
                check->setChecked(r.staged);
                check->setFocusPolicy(Qt::NoFocus);
                check->setCursor(Qt::PointingHandCursor);
                check->setToolTip(r.staged ? QStringLiteral("Unstage")
                                           : QStringLiteral("Stage"));
                const QString checkPath = r.path;
                const bool checkStaged = r.staged;
                connect(check, &QCheckBox::clicked, this, [this, checkPath, checkStaged] {
                    if (checkStaged)
                        scmUnstagePath(checkPath);
                    else
                        scmStagePath(checkPath);
                });
                parts.check = check;
            }

            auto *actions = new QWidget(w);
            auto *actionsLayout = new QHBoxLayout(actions);
            actionsLayout->setContentsMargins(0, 0, 0, 0);
            actionsLayout->setSpacing(0);
            auto makeBtn = [&](const QString &glyph, const QString &tip) {
                auto *b = new QToolButton(actions);
                b->setText(glyph);
                b->setToolTip(tip);
                b->setAutoRaise(true);
                b->setCursor(Qt::PointingHandCursor);
                return b;
            };
            const QString path = r.path;
            const bool untracked = r.untracked;
            if (r.staged) {
                auto *u = makeBtn(QString::fromUtf8("\xE2\x88\x92"), "Unstage");
                connect(u, &QToolButton::clicked, this,
                        [this, path] { scmUnstagePath(path); });
                actionsLayout->addWidget(u);
            } else {
                auto *d = makeBtn(QString::fromUtf8("\xE2\x86\xBA"), "Discard changes");
                connect(d, &QToolButton::clicked, this,
                        [this, path, untracked] { scmDiscardPath(path, untracked); });
                auto *s = makeBtn(QStringLiteral("+"), "Stage");
                connect(s, &QToolButton::clicked, this,
                        [this, path] { scmStagePath(path); });
                actionsLayout->addWidget(d);
                actionsLayout->addWidget(s);
            }
            auto *open = new QToolButton(actions);
            open->setIcon(themedOcticon("file", Theme::remapColor(QColor("#8b949e")), 14));
            open->setToolTip("Open file");
            open->setAutoRaise(true);
            open->setCursor(Qt::PointingHandCursor);
            connect(open, &QToolButton::clicked, this,
                    [this, path] { openRepoFile(path); });
            actionsLayout->addWidget(open);
            w->setActionsWidget(actions);
            parts.actions = actions;
            w->setParts(parts);
            const bool staged = r.staged;
            w->onClicked = [this, item, path, staged, untracked] {
                m_scmTree->setCurrentItem(item);
                showScmDiff(path, staged, untracked);
                m_scmTree->setFocus(Qt::MouseFocusReason);
            };
            m_scmTree->setItemWidget(item, 0, w);
        }
        group->setExpanded(true);
    };
    addGroup("Staged Changes", staged, /*stagedGroup=*/true);
    addGroup("Changes", changes, /*stagedGroup=*/false);
    fitTreeToWidestEntry(m_scmTree);

    const int total = staged.size() + changes.size();
    if (m_scmCountLabel)
        m_scmCountLabel->setText(total ? QString::number(total) : QString());
    scmNextChromeSync(m_scmPanel);
    if (m_railGitButton)
        m_railGitButton->setBadgeCount(total);
    const bool anything = total > 0;
    m_scmPendingChangeCount = total;
    updateScmCommitControlVisibility();
    if (m_scmCommitButton)
        m_scmCommitButton->setEnabled(anything);
    if (m_scmCommitPushButton)
        m_scmCommitPushButton->setEnabled(anything);
    if (m_scmStageCommitPushButton)
        m_scmStageCommitPushButton->setEnabled(anything);
    setScmGenerateEnabled(anything || (m_scmGenDuration &&
                                       m_scmGenDuration->currentIndex() > 0));

    autoFillScmMessage();

    renderScmCombinedDiff();

    if (!prevPath.isEmpty()) {
        for (int g = 0; g < m_scmTree->topLevelItemCount(); ++g) {
            QTreeWidgetItem *grp = m_scmTree->topLevelItem(g);
            for (int c = 0; c < grp->childCount(); ++c) {
                QTreeWidgetItem *item = grp->child(c);
                if (item->data(0, Qt::UserRole).toString() == prevPath &&
                    item->data(0, Qt::UserRole + 1).toBool() == prevStaged) {
                    m_scmTree->setCurrentItem(item);
                    return;
                }
            }
        }
    }
        });
}

void MainWindow::updateScmCommitControlVisibility()
{
    const bool showCommit = !m_scmOutgoingBlocking || m_scmPendingChangeCount > 0;
    ScmNextChrome *chrome = scmNextChrome(m_scmPanel);
    const bool next = chrome && chrome->arranged == 1;
    for (QPushButton *button : {m_scmCommitButton, m_scmCommitPushButton,
                                m_scmStageCommitPushButton}) {
        if (button)
            button->setVisible(showCommit && (!next || button == m_scmCommitButton));
    }
    if (next && chrome->commitMore)
        chrome->commitMore->setVisible(showCommit);
    if (m_scmSyncRow)
        m_scmSyncRow->setVisible(m_scmOutgoingBlocking);
    else if (m_scmSyncButton)
        m_scmSyncButton->setVisible(m_scmOutgoingBlocking);
}

void MainWindow::refreshSourceControlOutgoing()
{
    if (!m_scmOutgoingPanel || !m_scmOutgoingLabel || !m_scmSyncButton)
        return;
    auto showCommitControls = [this](bool show) {
        m_scmOutgoingBlocking = !show;
        updateScmCommitControlVisibility();
    };
    const auto applySyncActivity = [this](int index, bool busy) {
        if (!m_scmSyncStatus)
            return;
        const QString line =
            busy ? m_repoSyncActivity.value(index).trimmed() : QString();
        m_scmSyncStatus->setFullText(line);
    };
    const bool showOutgoingPanel =
        !sourceControlShowsRange() && m_scmPanel && m_scmPanel->isVisible();
    if (!showOutgoingPanel) {
        m_scmOutgoingPanel->hide();
        showCommitControls(true);
    }
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size()) {
        ++m_scmOutgoingGeneration;
        m_scmOutgoingPanel->hide();
        showCommitControls(true);
        if (m_railGitButton) {
            m_railGitButton->setPendingSyncCount(0);
            m_railGitButton->setToolTip(
                QStringLiteral("Source control — view the current changes"));
        }
        return;
    }

    const int repoIndex = m_repoDetailIndex;
    const RepositoryRecord repo = m_repositories.at(repoIndex);
    if (repo.localPath.isEmpty() ||
        !QDir(repo.localPath).exists(QStringLiteral(".git"))) {
        ++m_scmOutgoingGeneration;
        m_scmOutgoingPanel->hide();
        showCommitControls(true);
        if (m_railGitButton)
            m_railGitButton->setPendingSyncCount(0);
        return;
    }

    const bool busy = m_pushingRepos.contains(m_repoDetailIndex) ||
                      m_syncingRepos.contains(m_repoDetailIndex);
    if (showOutgoingPanel && busy && m_scmOutgoingPanel->isVisible()) {
        m_scmSyncButton->setEnabled(false);
        m_scmSyncButton->setText(QStringLiteral("Sync Changes"));
        startButtonSpin(m_scmSyncButton);
        setOcticon(m_scmSyncButton, QStringLiteral("sync"), 14);
        applySyncActivity(m_repoDetailIndex, true);
        showCommitControls(false);
    }

    // Until the worker resolves the branch, do not leave a previous branch's
    // ahead count painted as if it belonged to the current repository state.
    if (showOutgoingPanel)
        m_scmOutgoingPanel->hide();
    // Coalesce into the in-flight probe without invalidating it: this runs on
    // every repository-list frame, and bumping the generation here discarded
    // each answer as stale before it landed, so the rail's "N↑" pill stayed
    // empty while the commits page showed 62 local commits waiting to sync.
    // The landing still re-checks the repository and reruns for the latest.
    if (m_scmOutgoingProbeInFlight) {
        m_scmOutgoingProbePending = true;
        return;
    }
    const int generation = ++m_scmOutgoingGeneration;

    m_scmOutgoingProbeInFlight = true;
    m_scmOutgoingProbePending = false;
    // The input signature walks every ref file of the checkout, and this
    // refresh runs on every repository-list frame. Read it on the worker with
    // the probe: on 2026-09-12 that walk was the GUI thread's sampled frame
    // (getdents64) inside a 95 s freeze while the box was paging
    // (STALL-20260912T133541069-p367098-0020). The worker compares it with the
    // last answer itself, so a cache hit never spawns git either.
    ScmOutgoingProbe cachedResult;
    const bool haveCached = m_scmOutgoingProbeHaveResult &&
                            m_scmOutgoingProbeRepoPath == repo.localPath;
    const QString cachedSignature = haveCached ? m_scmOutgoingProbeSig : QString();
    if (haveCached) {
        cachedResult.branchResolved = m_scmOutgoingProbeBranchResolved;
        cachedResult.branch = m_scmOutgoingProbeBranch;
        cachedResult.pending = m_scmOutgoingProbePendingCount;
    }
    auto *watcher = new QFutureWatcher<ScmOutgoingProbe>(this);
    connect(watcher, &QFutureWatcher<ScmOutgoingProbe>::finished, this,
            [this, watcher, generation, repoIndex, repo, showCommitControls,
             applySyncActivity] {
            const ScmOutgoingProbe result = watcher->result();
            watcher->deleteLater();
            m_scmOutgoingProbeInFlight = false;
            m_scmOutgoingProbeHaveResult = true;
            m_scmOutgoingProbeRepoPath = repo.localPath;
            m_scmOutgoingProbeSig = result.inputSignature;
            m_scmOutgoingProbeBranchResolved = result.branchResolved;
            m_scmOutgoingProbeBranch = result.branch;
            m_scmOutgoingProbePendingCount = result.pending;
            const bool rerun = std::exchange(m_scmOutgoingProbePending, false);
            scmNextChromeSync(m_scmPanel);
            const auto scheduleRerun = [this, rerun] {
                if (rerun) {
                    QTimer::singleShot(
                        0, this, [this] { refreshSourceControlOutgoing(); });
                }
            };
            if (generation != m_scmOutgoingGeneration ||
                repoIndex != m_repoDetailIndex ||
                repoIndex < 0 || repoIndex >= m_repositories.size() ||
                m_repositories.at(repoIndex).localPath != repo.localPath) {
                scheduleRerun();
                return;
            }
            if (!result.branchResolved) {
                m_scmOutgoingPanel->hide();
                stopButtonSpin(m_scmSyncButton);
                showCommitControls(true);
                if (m_railGitButton)
                    m_railGitButton->setPendingSyncCount(0);
                scheduleRerun();
                return;
            }
            const QString branch = result.branch;
            const int pending = result.pending;
            if (m_railGitButton) {
                m_railGitButton->setPendingSyncCount(pending);
                m_railGitButton->setToolTip(
                    pending > 0
                        ? QStringLiteral("Source control — %1 commit%2 waiting to sync")
                              .arg(pending)
                              .arg(pending == 1 ? QString() : QStringLiteral("s"))
                        : QStringLiteral("Source control — view the current changes"));
            }
            if (sourceControlShowsRange() || !m_scmPanel ||
                !m_scmPanel->isVisible()) {
                m_scmOutgoingPanel->hide();
                stopButtonSpin(m_scmSyncButton);
                showCommitControls(true);
                scheduleRerun();
                return;
            }
            const bool stillBusy = m_pushingRepos.contains(repoIndex) ||
                                   m_syncingRepos.contains(repoIndex);
            if (pending <= 0 && !stillBusy) {
                m_scmOutgoingPanel->hide();
                stopButtonSpin(m_scmSyncButton);
                showCommitControls(true);
                scheduleRerun();
                return;
            }

            m_scmOutgoingLabel->setText(
                QStringLiteral(
                    "<span style='color:#1f6feb'>&#9673;</span> "
                    "<b>%1</b><span style='color:#8b949e'> · %2&#8593;</span>")
                    .arg(branch.toHtmlEscaped())
                    .arg(pending));
            m_scmOutgoingPanel->setProperty("branch", branch);
            m_scmSyncButton->setEnabled(!stillBusy && pending > 0);
            m_scmSyncButton->setText(pending > 0
                                          ? QStringLiteral("Sync Changes %1↑")
                                                .arg(pending)
                                          : QStringLiteral("Sync Changes"));
            if (stillBusy)
                startButtonSpin(m_scmSyncButton);
            else
                stopButtonSpin(m_scmSyncButton);
            setOcticon(m_scmSyncButton, QStringLiteral("sync"), 14);
            applySyncActivity(repoIndex, stillBusy);
            m_scmOutgoingPanel->hide();
            showCommitControls(false);
            scheduleRerun();
        });
    const QString relayHost = configuredRelayHost();
    watcher->setFuture(QtConcurrent::run(
        [repo, relayHost, haveCached, cachedSignature, cachedResult] {
            const QString inputSignature = scmOutgoingInputSignature(repo);
            if (haveCached && inputSignature == cachedSignature) {
                // Same inputs as last time: hand the remembered answer through
                // the same landing code so every UI branch above stays on one
                // path.
                ScmOutgoingProbe replay = cachedResult;
                replay.inputSignature = inputSignature;
                return replay;
            }
            ScmOutgoingProbe probe = probeSourceControlOutgoing(repo, relayHost);
            probe.inputSignature = inputSignature;
            return probe;
        }));
}

void MainWindow::undoLatestOutgoingCommit()
{
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size() ||
        m_scmOutgoingProbePendingCount <= 0)
        return;
    const int index = m_repoDetailIndex;
    const RepositoryRecord repo = m_repositories.at(index);
    if (repo.localPath.isEmpty())
        return;
    const QString dir = repo.localPath;
    runOffThread<QByteArray>(
        [dir] {
            QByteArray subject;
            runGitCapture(dir,
                          {QStringLiteral("log"), QStringLiteral("-1"),
                           QStringLiteral("--format=%h %s")},
                          &subject, nullptr);
            return subject;
        },
        [this, index, dir, owner = repo.owner, name = repo.name](
            QByteArray subject) {
            if (m_repoDetailIndex != index || scmRepoIndexFor(index, dir) != index ||
                m_scmOutgoingProbePendingCount <= 0)
                return;
            if (QMessageBox::question(
                    this, QStringLiteral("Undo outgoing commit"),
                    QStringLiteral("Undo the latest outgoing commit?\n\n%1\n\n"
                                   "Its file changes will remain in the working tree.")
                        .arg(QString::fromUtf8(subject).trimmed()),
                    QMessageBox::Yes | QMessageBox::Cancel) != QMessageBox::Yes)
                return;
            queueScmMutation(
                QStringLiteral("undo-outgoing|") + dir,
                [dir] {
                    return scmMutate(dir, {QStringLiteral("reset"),
                                           QStringLiteral("--mixed"),
                                           QStringLiteral("HEAD~1")});
                },
                [this, owner, name](const ScmMutationResult &result) {
                    if (!result.ok) {
                        QMessageBox::warning(
                            this, QStringLiteral("Undo outgoing commit"),
                            result.error.isEmpty()
                                ? QStringLiteral("git reset failed.")
                                : result.error.left(300));
                        return;
                    }
                    logSystem(QStringLiteral("Git: undid the latest outgoing commit "
                                             "in %1/%2; its changes remain in the "
                                             "working tree.")
                                  .arg(owner, name));
                    m_scmOutgoingProbeHaveResult = false;
                    refreshSourceControl(true);
                });
        });
}

// Publish the one-line "what is the sync doing right now" note for a repository
// row. Publishing is a chain of long, silent steps (secret scan, clone, seal,
// encrypt, publish, mirror push) and each of them announces itself here, so the
// button beside it never sits on a bare "Syncing" for minutes. Notes for rows
// other than the open one are remembered, not painted.
void MainWindow::setRepoSyncActivity(int index, const QString &line)
{
    const QString note = line.simplified();
    if (note.isEmpty()) {
        clearRepoSyncActivity(index);
        return;
    }
    if (index < 0)
        return;
    if (m_repoSyncActivity.value(index) == note)
        return;
    m_repoSyncActivity.insert(index, note);
    if (index != m_repoDetailIndex || !m_scmSyncStatus)
        return;
    m_scmSyncStatus->setFullText(note);
}

void MainWindow::clearRepoSyncActivity(int index)
{
    if (index < 0 || !m_repoSyncActivity.remove(index))
        return;
    if (index == m_repoDetailIndex && m_scmSyncStatus) {
        m_scmSyncStatus->setFullText(QString());
    }
}

std::shared_ptr<QString> MainWindow::streamGitProgressActivity(
    QProcess *process, int index, const QString &prefix)
{
    auto buffer = std::make_shared<QString>();
    if (!process)
        return buffer;
    connect(process, &QProcess::readyReadStandardError, this,
            [this, process, index, prefix, buffer] {
                const QString chunk =
                    QString::fromUtf8(process->readAllStandardError());
                if (chunk.isEmpty())
                    return;
                buffer->append(chunk);
                QString newest;
                QString normalized = chunk;
                normalized.replace(QLatin1Char('\r'), QLatin1Char('\n'));
                for (const QString &frame :
                     normalized.split(QLatin1Char('\n'))) {
                    const QString trimmed = frame.trimmed();
                    if (!trimmed.isEmpty())
                        newest = trimmed;
                }
                if (newest.isEmpty())
                    return;
                setRepoSyncActivity(index, prefix + newest);
            });
    return buffer;
}

QString MainWindow::gitErrorsWithoutProgress(const QString &text)
{
    static const QRegularExpression progressLine(
        QStringLiteral("^(remote: )?(Enumerating|Counting|Compressing|Writing|"
                       "Receiving|Resolving|Unpacking|Total|Updating) "));
    QString normalized = text;
    normalized.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    QStringList kept;
    for (const QString &line : normalized.split(QLatin1Char('\n'))) {
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty() || progressLine.match(trimmed).hasMatch())
            continue;
        kept.append(trimmed);
    }
    return kept.join(QLatin1Char('\n')).trimmed();
}

QString MainWindow::scmViewedContext()
{
    return QStringLiteral("worktree");
}

void MainWindow::setupScmDiffPane()
{
    if (!m_scmDiff || m_scmStickyHeader)
        return;
    m_scmDiff->setOpenExternalLinks(false);
    m_scmDiff->setOpenLinks(false); // "viewed:" toggles are handled here
    m_scmDiff->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_scmDiff, &QTextBrowser::anchorClicked, this,
            &MainWindow::onScmDiffAnchorClicked);
    connect(m_scmDiff, &QWidget::customContextMenuRequested, this,
            [this](const QPoint &pos) {
                QString key = m_scmStickySection;
                if (key.isEmpty() && m_scmTree && m_scmTree->currentItem()) {
                    const QString path = m_scmTree->currentItem()
                                             ->data(0, Qt::UserRole)
                                             .toString();
                    if (!path.isEmpty())
                        key = QStringLiteral("u|") + path;
                }
                const QString path = key.size() > 2 ? key.mid(2) : QString();
                const QString cleanPath = QDir::cleanPath(path);
                if (path.isEmpty() || cleanPath != path ||
                    QDir::isAbsolutePath(path) ||
                    path.startsWith(QLatin1String("../")) ||
                    path.contains(QLatin1String("/../")) ||
                    path == QLatin1String(".git") ||
                    path.startsWith(QLatin1String(".git/")))
                    return;
                const QString dir = sourceControlShowsRange()
                                        ? m_branchDiffWorkDir
                                        : sourceControlGitDir();
                if (dir.isEmpty() || !QFileInfo(QDir(dir).filePath(path)).isFile())
                    return;
                QMenu menu(this);
                QAction *open = menu.addAction(QStringLiteral("Open in file editor"));
                open->setIcon(themedOcticon(QStringLiteral("pencil"),
                                            QColor(QStringLiteral("#8b949e")), 14));
                if (menu.exec(m_scmDiff->viewport()->mapToGlobal(pos)) == open)
                    openWorkingTreeFile(dir, path);
            });

    m_scmDiffActiveOutline = new QFrame(diffOverlayParent(m_scmDiff));
    m_scmDiffActiveOutline->setObjectName("diffActiveOutline");
    m_scmDiffActiveOutline->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_scmDiffActiveOutline->setStyleSheet(scmOutlineStyleSheet());
    m_scmDiffActiveOutline->hide();
    m_scmDiff->viewport()->installEventFilter(this);

    m_scmStickyHeader = new QFrame(diffOverlayParent(m_scmDiff));
    m_scmStickyHeader->setObjectName("diffStickyHeader");
    m_scmStickyHeader->setStyleSheet(scmStickyHeaderStyleSheet());
    auto *sl = new QHBoxLayout(m_scmStickyHeader);
    sl->setContentsMargins(12, 9, 12, 9);
    sl->setSpacing(0);
    m_scmStickyPath = new QLabel(m_scmStickyHeader);
    m_scmStickyPath->setTextFormat(Qt::RichText);
    m_scmStickyPath->setTextInteractionFlags(Qt::NoTextInteraction);
    configureDiffStickyPathLabel(m_scmStickyPath);
    sl->addWidget(m_scmStickyPath, 1);
    m_scmStickyControls = new QLabel(m_scmStickyHeader);
    m_scmStickyControls->setTextFormat(Qt::RichText);
    m_scmStickyControls->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
    m_scmStickyControls->setCursor(Qt::PointingHandCursor);
    connect(m_scmStickyControls, &QLabel::linkActivated, this,
            [this](const QString &link) {
        // The bar carries more than one pill now: only the viewed toggle earns
        // the advance-to-the-next-file landing and the fade below.
        const QUrl clicked(link);
        if (handleDiffFileActionLink(m_scmDiff, clicked))
            return;
        if (clicked.scheme() != QLatin1String("viewed")) {
            onScmDiffAnchorClicked(clicked);
            return;
        }
        const int idx = m_scmSectionKeys.indexOf(m_scmStickySection);
        if (idx < 0)
            return;
        const QString path = m_scmSectionPaths.at(idx);
        const QString ctx = scmViewedContext();
        const bool nowViewed = !loadDiffViewed(ctx).contains(path);
        const int landing =
            nowViewed && idx + 1 < m_scmSectionKeys.size() ? idx + 1 : idx;
        const QString landingPath = m_scmSectionPaths.at(landing);
        const bool landingStaged =
            m_scmSectionKeys.at(landing).startsWith(QLatin1String("s|"));
        setDiffViewed(ctx, path, nowViewed);
        auto *effect = new QGraphicsOpacityEffect(m_scmStickyControls);
        effect->setOpacity(0.55);
        // setGraphicsEffect() owns and *deletes* whichever effect is already
        // installed, so it is the single delete for this one too: dropping it
        // with setGraphicsEffect(nullptr) below and then calling deleteLater()
        // on the same pointer was a use-after-free that crashed on the next
        // click. Clicking again inside the 180ms fade likewise
        // destroys this effect early, so hold it (and the pill) by QPointer and
        // only clear the effect that is still ours.
        m_scmStickyControls->setGraphicsEffect(effect);
        auto *animation =
            new QPropertyAnimation(effect, "opacity", m_scmStickyControls);
        animation->setDuration(180);
        animation->setStartValue(0.55);
        animation->setEndValue(1.0);
        connect(animation, &QPropertyAnimation::finished, m_scmStickyControls,
                [pill = QPointer<QLabel>(m_scmStickyControls),
                 faded = QPointer<QGraphicsEffect>(effect)] {
                    if (pill && faded && pill->graphicsEffect() == faded)
                        pill->setGraphicsEffect(nullptr); // deletes `faded`
                });
        animation->start(QAbstractAnimation::DeleteWhenStopped);
        // Folding the file the bar is parked on takes the body out from under
        // the viewport, so this one does land on the next file — but it lands
        // from a document only that file moved in, not a whole fresh render.
        if (!swapScmDiffFileInPlace(path))
            renderScmCombinedDiff();
        scrollScmDiffToFile(landingPath, landingStaged);
    });
    sl->addWidget(m_scmStickyControls, 0);
    m_scmStickyHeader->hide();

    m_scmNextFileBar = new QLabel(diffOverlayParent(m_scmDiff));
    m_scmNextFileBar->setObjectName("diffNextFileBar");
    m_scmNextFileBar->setTextFormat(Qt::RichText);
    m_scmNextFileBar->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
    m_scmNextFileBar->setCursor(Qt::PointingHandCursor);
    configureDiffStickyPathLabel(m_scmNextFileBar);
    connect(m_scmNextFileBar, &QLabel::linkActivated, this, [this] {
        const int idx = m_scmSectionKeys.indexOf(m_scmNextFileKey);
        if (idx < 0)
            return;
        scrollScmDiffToFile(m_scmSectionPaths.at(idx),
                            m_scmNextFileKey.startsWith(QLatin1String("s|")));
    });
    m_scmNextFileBar->hide();

    m_scmAutoViewedDebounce = new QTimer(this);
    m_scmAutoViewedDebounce->setSingleShot(true);
    m_scmAutoViewedDebounce->setInterval(400);
    connect(m_scmAutoViewedDebounce, &QTimer::timeout, this,
            &MainWindow::applyScmAutoMarkViewedOnScroll);
    connect(m_scmDiff->verticalScrollBar(), &QScrollBar::valueChanged, this,
            [this](int value) {
                updateScmDiffScrollState(); // cheap, every tick
                if (m_scmApplyingAutoViewed)
                    return;
                if (value != m_scmLastAutoViewedScrollValue) {
                    m_scmAutoViewedScrollDirection =
                        value < m_scmLastAutoViewedScrollValue ? -1 : 1;
                    m_scmLastAutoViewedScrollValue = value;
                }
                m_scmAutoViewedDebounce->start(); // heavy, debounced
            });

    auto card = std::make_shared<QPointer<QWidget>>();
    auto *designHook = new ScmDesignHook(
        m_scmDiff, ScmDesignHook::Events::DesignAndGeometry,
        [this, card](bool settled) {
            if (!m_scmDiff)
                return;
            const bool next = Theme::isNext();
            const QString design =
                !next ? QStringLiteral("classic")
                      : Theme::activeDark() ? QStringLiteral("next-dark")
                                            : QStringLiteral("next-light");
            for (QFrame *outline :
                 {m_scmDiffActiveOutline, m_branchDiffActiveOutline})
                scmSetSheet(outline, scmOutlineStyleSheet());
            scmSetSheet(m_scmStickyHeader, scmStickyHeaderStyleSheet());
            if (m_scmStickyHeader && m_scmStickyHeader->layout()) {
                const QMargins margins = next ? QMargins(12, 5, 12, 5)
                                              : QMargins(12, 9, 12, 9);
                if (m_scmStickyHeader->layout()->contentsMargins() != margins)
                    m_scmStickyHeader->layout()->setContentsMargins(margins);
            }
            QWidget *host = m_scmDiff->parentWidget();
            if (*card && (*card)->parentWidget() != host)
                delete card->data();
            if (!*card && host && next)
                *card = new ScmDiffCard(host);
            if (*card) {
                (*card)->setGeometry(m_scmDiff->geometry());
                (*card)->setVisible(next && !m_scmDiff->isHidden());
                if (next)
                    (*card)->raise();
            }
            const QVariant shown = m_scmDiff->property("scmDesign");
            if (!settled || shown.toString() == design)
                return;
            m_scmDiff->setProperty("scmDesign", design);
            if (!shown.isValid())
                return;
            m_scmNextFileKey.clear();
            m_scmStickySection.clear();
            m_scmDiffRenderKey.clear();
            m_scmDiffSourceKey.clear();
            if (m_scmPatchValid && !m_scmAllReposView &&
                !sourceControlShowsRange())
                renderScmCombinedDiff();
            else
                updateScmDiffScrollState();
        });
    designHook->run();
}

// Everything the working-tree pane's render is keyed on. Held apart from
// renderScmCombinedDiff() so an in-place Viewed swap can restate the key it
// just invalidated: a stale one would send the next refresh straight back into
// the full re-render the swap exists to avoid.
QString MainWindow::scmDiffSourceKey(const QSet<QString> &viewed,
                                     const QSet<QString> &expanded) const
{
    QStringList viewedKeys(viewed.cbegin(), viewed.cend());
    viewedKeys.sort();
    QStringList expandedKeys(expanded.cbegin(), expanded.cend());
    expandedKeys.sort();
    return QString(diffSplitPref() ? QLatin1Char('s') : QLatin1Char('u')) +
           QLatin1Char('\x1f') + diffStyleSheet(m_diffFontPt) +
           QLatin1Char('\x1f') + scmViewedContext() + QLatin1Char('\x1f') +
           viewedKeys.join(QLatin1Char('\x1e')) + QLatin1Char('\x1f') +
           expandedKeys.join(QLatin1Char('\x1e')) + QLatin1Char('\x1f') +
           m_scmCombinedPatch;
}

void MainWindow::renderScmCombinedDiff()
{
    if (!m_scmDiff)
        return;
    const QString dir = sourceControlGitDir();
    if (dir.isEmpty() || !repoHasWorkingTree())
        return;

    if (!m_scmPatchValid) {
        // The tree already describes the new status. Do not leave the old
        // repository's/file set's anchors live while its replacement is read.
        // Clear them even when an older read is still winding down: a click on
        // the new tree must never resolve against the previous patch merely
        // because its path happens to have the same name.
        m_scmDiffRenderKey.clear();
        m_scmDiffSourceKey.clear();
        m_scmSectionKeys.clear();
        m_scmSectionAnchors.clear();
        m_scmSectionPaths.clear();
        m_scmStickyLabelHtml.clear();
        m_scmStickyActionHtml.clear();
        m_scmFileTops.clear();
        m_scmStickySection.clear();
        if (m_scmStickyHeader)
            m_scmStickyHeader->hide();
        if (m_scmNextFileBar)
            m_scmNextFileBar->hide();
        if (m_scmDiffActiveOutline)
            m_scmDiffActiveOutline->hide();
        if (m_scmSecretBanner)
            m_scmSecretBanner->clearFindings();
        setDiffHtml(
            m_scmDiff,
            QString::fromUtf8("<p style='color:#8b949e'>Reading working-tree "
                           "changes\xE2\x80\xA6</p>"));

        if (m_scmPatchReadInFlight)
            return;
        m_scmPatchReadInFlight = true;
        const quint64 patchGeneration = m_scmPatchGeneration;

        runOffThread<ScmCombinedPatchRead>(
            [dir] {
                ScmCombinedPatchRead read = readScmCombinedPatch(dir);
                read.secrets = RepoSecurity::findSecretsInDiff(read.patch);
                return read;
            },
            [this, dir, patchGeneration](ScmCombinedPatchRead read) {
                m_scmPatchReadInFlight = false;
                if (patchGeneration == m_scmPatchGeneration && m_scmDiff &&
                    !sourceControlShowsRange() && sourceControlGitDir() == dir) {
                    m_scmCombinedPatch = std::move(read.patch);
                    m_scmCombinedStagedFiles = read.stagedFiles;
                    m_scmSecretFindings = std::move(read.secrets);
                    m_scmPatchValid = true;
                }
                // A refresh/repository switch may have superseded this read.
                // Re-enter through the same gate so the newest invalid patch
                // gets one worker of its own; a current result takes the render
                // path on that turn.
                if (m_scmDiff && !sourceControlShowsRange())
                    QTimer::singleShot(
                        0, this, [this] { renderScmCombinedDiff(); });
            });
        return;
    }

    if (m_scmSecretBanner)
        m_scmSecretBanner->setFindings(m_scmSecretFindings, true);
    QList<DiffFileEntry> files;
    const QString ctx = scmViewedContext();
    // An edit to a file the reviewer had ticked drops that tick: the working
    // tree moves under a review all the time, and the mark spoke for the file
    // as it stood. The source key below carries the pruned set, so the pane
    // re-renders the freed checkbox rather than sitting on its memo.
    const QSet<QString> viewed = refreshDiffViewedForPatch(ctx, m_scmCombinedPatch);

    const QSet<QString> expanded = diffExpandedViewed(ctx);
    const QString sourceKey = scmDiffSourceKey(viewed, expanded);
    const bool cleanTree = m_scmCombinedPatch.isEmpty();
    // A clean tree carries the day's agent leaderboard. Let that bounded,
    // cached page refresh even though its empty patch has not changed.
    if (!cleanTree && !m_scmDiffSourceKey.isEmpty() &&
        sourceKey == m_scmDiffSourceKey &&
        !m_scmDiffRenderKey.isEmpty())
        return;

    DiffRenderAppearance appearance = currentDiffRenderAppearance();
    appearance.changeArrows = true;
    appearance.changeArrowsStagedFiles = m_scmCombinedStagedFiles;
    const QString html = renderDiffHtmlSplit(
        diffSplitPref(), m_scmCombinedPatch, files, dir, QString(), QString(),
        QString(), QHash<QString, QString>(), viewed, expanded, appearance);
    QString body = html;
    if (cleanTree) {
        const quint64 statusGeneration = m_scmStatusGeneration;
        const QString base = repoDefaultBranchFast();
        // The leaderboard's git reads run on a worker. The plain message goes
        // up now; when the page lands this render runs again and takes it from
        // the memo — unless a repository or workspace-page/status switch has
        // taken the pane over by then.
        body = branchNoChangesPaneHtml(
            QString(), base, dir,
            QStringLiteral("(no working-tree changes)"), /*remember=*/false,
            [this, dir, statusGeneration](QString) {
                if (!m_scmDiff || sourceControlShowsRange() ||
                    sourceControlGitDir() != dir ||
                    m_scmStatusGeneration != statusGeneration ||
                    !m_scmPatchValid || !m_scmCombinedPatch.isEmpty())
                    return;
                renderScmCombinedDiff();
            });
        if (!m_scmDiff || sourceControlShowsRange() ||
            sourceControlGitDir() != dir ||
            m_scmStatusGeneration != statusGeneration || !m_scmPatchValid ||
            !m_scmCombinedPatch.isEmpty())
            return;
    }
    if (body.isEmpty())
        body = QStringLiteral(
            "<p style='color:#8b949e'>(no working-tree changes)</p>");
    m_scmDiffSourceKey = sourceKey;

    m_scmSectionKeys.clear();
    m_scmSectionAnchors.clear();
    m_scmSectionPaths.clear();
    m_scmStickyLabelHtml.clear();
    m_scmStickyActionHtml.clear();
    m_scmFileTops.clear(); // positions move on re-render; force a recompute
    for (int i = 0; i < files.size(); ++i) {
        const DiffFileEntry &f = files.at(i);
        const QString key = (i < m_scmCombinedStagedFiles ? QStringLiteral("s|")
                                                          : QStringLiteral("u|")) +
                            f.path;
        m_scmSectionKeys.append(key);
        m_scmSectionAnchors.append(f.anchor);
        m_scmSectionPaths.append(f.path);
        m_scmStickyLabelHtml.insert(key,
                                    diffFileLabelHtml(f, viewed.contains(f.path)));
        m_scmStickyActionHtml.insert(key, diffFileActionPillsHtml(f));
    }

    const QSet<QString> live(m_scmSectionPaths.begin(), m_scmSectionPaths.end());
    for (const QString &p : viewed)
        if (!live.contains(p))
            setDiffViewed(ctx, p, false);
    updateScmViewedCount();

    const QString key =
        diffStyleSheet(m_diffFontPt) + QLatin1Char('\x1f') + body;
    if (key == m_scmDiffRenderKey)
        return;
    m_scmDiffRenderKey = key;
    ++m_scmDiffRenders;
    // A small document can finish inside setDiffHtml(), which immediately
    // runs onDiffStreamFinished() and lands any file click queued during the
    // patch read. Reset the old overlay state before that callback, never
    // after it, or the rest of this render would erase a successful landing.
    m_scmStickySection.clear();
    if (!m_scmSectionKeys.contains(m_scmActiveSectionKey))
        m_scmActiveSectionKey.clear();
    if (m_scmDiffActiveOutline)
        m_scmDiffActiveOutline->hide(); // until the new layout is measured
    setDiffHtml(m_scmDiff, body, /*endCap=*/!html.isEmpty());
    // What a later Viewed tick folds in place instead of re-rendering. The
    // working-tree diff draws no per-line comment anchors, so its headers carry
    // no comment pill either and the swap has to rebuild them the same way.
    noteDiffFileBlocks(m_scmDiff, html, files, viewed, expanded,
                       /*comments=*/false);
    QTimer::singleShot(0, this, &MainWindow::updateScmDiffScrollState);
}

// Folds or reopens one file of the working-tree diff without touching the rest
// of the document. The whole Git page is one QTextDocument, so re-rendering it
// to tick one checkbox re-lays out every other file and shoves the rows the
// reviewer was reading out from under them. False when the swap is not
// available and the caller has to re-render after all.
bool MainWindow::swapScmDiffFileInPlace(const QString &path)
{
    if (!m_scmDiff || path.isEmpty() || m_scmDiffSourceKey.isEmpty() ||
        m_scmDiffRenderKey.isEmpty())
        return false;
    const QList<DiffFileEntry> entries = diffRenderedFiles(m_scmDiff);
    if (entries.size() != m_scmSectionPaths.size() ||
        entries.size() != m_scmSectionAnchors.size() ||
        entries.size() != m_scmSectionKeys.size())
        return false; // the pane and the block cache disagree; re-render
    const QString ctx = scmViewedContext();
    const QSet<QString> viewed = loadDiffViewed(ctx);
    const QSet<QString> expanded = diffExpandedViewed(ctx);
    const bool nowViewed = viewed.contains(path);
    const bool nowExpanded = expanded.contains(path);

    // A path that is staged and then edited again sections twice, and the one
    // Viewed mark speaks for both sections: fold every section carrying it, or
    // half the file stays open under a ticked checkbox.
    QString renderKey = m_scmDiffRenderKey;
    bool swapped = false;
    for (int i = 0; i < entries.size(); ++i) {
        if (m_scmSectionPaths.at(i) != path)
            continue;
        const DiffFileBlockSwap swap = swapDiffFileViewedState(
            m_scmDiff, m_scmSectionAnchors.at(i), nowViewed, nowExpanded);
        if (!swap.applied) {
            // A section that declined leaves the document half-swapped, so the
            // re-render that follows must not be able to skip itself on a memo.
            if (swapped) {
                m_scmDiffSourceKey.clear();
                m_scmDiffRenderKey.clear();
            }
            return false;
        }
        renderKey.replace(swap.oldHtml, swap.newHtml);
        m_scmStickyLabelHtml.insert(m_scmSectionKeys.at(i),
                                    diffFileLabelHtml(entries.at(i), nowViewed));
        swapped = true;
    }
    if (!swapped)
        return false;

    // Both keys still describe the document, so restate them rather than
    // dropping them.
    m_scmDiffRenderKey = renderKey;
    m_scmDiffSourceKey = scmDiffSourceKey(viewed, expanded);
    m_scmFileTops.clear();     // measured off the layout the swap just changed
    m_scmStickySection.clear();
    updateScmViewedCount();
    QTimer::singleShot(0, this, &MainWindow::updateScmDiffScrollState);
    return true;
}

void MainWindow::showScmDiff(const QString &path, bool staged, bool untracked)
{
    Q_UNUSED(untracked);
    if (!m_scmDiff || m_scmSuppressFileScroll)
        return; // suppressed: the selection is following the scroll, not driving it
    setCommitWorkspacePage(kCommitWorkspaceChangesPage);
    if (m_scmSectionKeys.isEmpty())
        renderScmCombinedDiff();
    scrollScmDiffToFile(path, staged);
}

void MainWindow::showScmDiffAll(bool staged)
{
    if (!m_scmDiff)
        return;
    setCommitWorkspacePage(kCommitWorkspaceChangesPage);
    if (m_scmSectionKeys.isEmpty())
        renderScmCombinedDiff();
    const QString prefix = staged ? QStringLiteral("s|") : QStringLiteral("u|");
    if (m_scmSectionKeys.isEmpty() && m_scmTree) {
        // The patch reader may still be in flight. Remember the first row in
        // the requested group and let onDiffStreamFinished() land there once
        // its section exists.
        for (int groupIndex = 0;
             groupIndex < m_scmTree->topLevelItemCount(); ++groupIndex) {
            QTreeWidgetItem *group = m_scmTree->topLevelItem(groupIndex);
            for (int row = 0; group && row < group->childCount(); ++row) {
                QTreeWidgetItem *item = group->child(row);
                if (item->data(0, Qt::UserRole + 1).toBool() != staged)
                    continue;
                scrollScmDiffToFile(
                    item->data(0, Qt::UserRole).toString(), staged);
                return;
            }
        }
    }
    for (int i = 0; i < m_scmSectionKeys.size(); ++i) {
        if (m_scmSectionKeys.at(i).startsWith(prefix)) {
            const QString key = m_scmSectionKeys.at(i);
            scrollDiffToAnchor(m_scmDiff, m_scmSectionAnchors.at(i));
            // A streamed document can still grow below this anchor. Keep the
            // landing pending until the final batch so the requested header
            // does not drift away after an apparently successful early pin.
            if (!diffStreamInProgress(m_scmDiff) &&
                pinScmDiffSectionToTop(i))
                m_scmPendingScrollKey.clear();
            else
                m_scmPendingScrollKey = key;
            return;
        }
    }
}

bool MainWindow::pinScmDiffSectionToTop(int sectionIndex)
{
    if (!m_scmDiff || sectionIndex < 0 ||
        sectionIndex >= m_scmSectionAnchors.size())
        return false;
    m_scmDiff->scrollToAnchor(m_scmSectionAnchors.at(sectionIndex));
    m_scmFileTops.clear();
    computeScmFileTops();
    const int top = m_scmFileTops.value(sectionIndex, -1);
    QScrollBar *vbar = m_scmDiff->verticalScrollBar();
    if (top < 0 || !vbar)
        return false;
    vbar->setValue(qBound(vbar->minimum(), top, vbar->maximum()));
    updateScmDiffScrollState();
    return true;
}

void MainWindow::scrollScmDiffToFile(const QString &path, bool staged)
{
    if (!m_scmDiff)
        return;
    const QString requestedKey =
        (staged ? QStringLiteral("s|") : QStringLiteral("u|")) + path;
    int idx = m_scmSectionKeys.indexOf(requestedKey);
    if (idx < 0)
        idx = m_scmSectionPaths.indexOf(path); // only one side has a diff
    if (idx < 0) {
        // Accept clicks while the combined patch worker is still reading.
        // The stream-finished hook resolves this key after the sections land.
        m_lastSourceControlDiffPath = path;
        m_scmActiveSectionKey = requestedKey;
        m_scmPendingScrollKey = requestedKey;
        return;
    }
    m_lastSourceControlDiffPath = path;
    flushDiffStream(m_scmDiff);
    const QString key = m_scmSectionKeys.at(idx);
    m_scmActiveSectionKey = key;
    scrollDiffToAnchor(m_scmDiff, m_scmSectionAnchors.at(idx));
    updateScmDiffActiveOutline();
    // The first streamed diff batch can already contain this file, but later
    // batches still change its final geometry. Leave a marker for the
    // stream-finished hook so an early click is pinned again after layout.
    if (!diffStreamInProgress(m_scmDiff) && pinScmDiffSectionToTop(idx))
        m_scmPendingScrollKey.clear();
    else
        m_scmPendingScrollKey = key;
}

QTreeWidgetItem *MainWindow::scmFindItem(const QString &path, bool staged) const
{
    if (!m_scmTree)
        return nullptr;
    for (int g = 0; g < m_scmTree->topLevelItemCount(); ++g) {
        QTreeWidgetItem *grp = m_scmTree->topLevelItem(g);
        for (int c = 0; c < grp->childCount(); ++c) {
            QTreeWidgetItem *item = grp->child(c);
            if (item->data(0, Qt::UserRole).toString() == path &&
                item->data(0, Qt::UserRole + 1).toBool() == staged)
                return item;
        }
    }
    return nullptr;
}

void MainWindow::selectScmFileInTree(const QString &path, bool staged)
{
    QTreeWidgetItem *item = scmFindItem(path, staged);
    if (!item || m_scmTree->currentItem() == item)
        return;
    // The pane is telling the tree which file it is showing, so the row moves
    // silently: currentItemChanged reads a folder's or another repository's
    // patch back out of Git, and folding one file with the Viewed pill is no
    // reason to run `git diff` — let alone to replace the diff being read with
    // the single file that selection landed on. A click in the tree still goes
    // through the handler; only this echo of the pane's own state skips it.
    setScmCurrentItemSilently(item);
    m_scmTree->scrollToItem(item);
}

void MainWindow::layoutScmStickyHeader()
{
    if (!m_scmStickyHeader || !m_scmDiff)
        return;
    const int height = m_scmStickyHeader->sizeHint().height();
    // m_scmStickyTopOffset is the push-out the last scroll tick worked out; a
    // bare relayout (a viewport resize) must not slam the bar back over the
    // header it had already made room for.
    placeDiffOverlay(m_scmStickyHeader, m_scmDiff,
                     QRect(0, qBound(-height, m_scmStickyTopOffset, 0),
                           m_scmDiff->viewport()->width(), height));
}

// code-health: allow(deep-nested-loop) one pass over the document's fragments; the innermost loop is a fragment's anchor names with a hash lookup
void MainWindow::computeScmFileTops()
{
    m_scmFileTops = QList<int>(m_scmSectionAnchors.size(), -1);
    if (!m_scmDiff || m_scmSectionAnchors.isEmpty())
        return;
    QScrollBar *vbar = m_scmDiff->verticalScrollBar();
    const int viewTop = vbar ? vbar->value() : 0;
    QHash<QString, int> anchorIndex;
    for (int i = 0; i < m_scmSectionAnchors.size(); ++i)
        if (!m_scmSectionAnchors.at(i).isEmpty())
            anchorIndex.insert(m_scmSectionAnchors.at(i), i);
    QTextDocument *doc = m_scmDiff->document();
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment frag = it.fragment();
            if (!frag.isValid() || !frag.charFormat().isAnchor())
                continue;
            for (const QString &name : frag.charFormat().anchorNames()) {
                const auto ai = anchorIndex.constFind(name);
                if (ai == anchorIndex.constEnd())
                    continue;
                QTextCursor cur(doc);
                cur.setPosition(frag.position());
                // The anchor character lives inside the header cell, so its
                // cursor rect starts at the text, a cell padding below the
                // row's own top edge. The row rect gives that edge back.
                const QRectF row = diffFileRowRect(m_scmDiff, frag.position());
                m_scmFileTops[ai.value()] =
                    row.isNull() ? m_scmDiff->cursorRect(cur).top() + viewTop
                                 : qRound(row.top());
            }
        }
    }
}

void MainWindow::updateScmDiffActiveOutline()
{
    if (!m_scmDiff || !m_scmDiffActiveOutline)
        return;
    const int index = m_scmActiveSectionKey.isEmpty()
                          ? -1
                          : m_scmSectionKeys.indexOf(m_scmActiveSectionKey);
    QScrollBar *vbar = m_scmDiff->verticalScrollBar();
    if (index < 0 || !vbar) {
        m_scmDiffActiveOutline->hide();
        return;
    }
    if (m_scmFileTops.size() != m_scmSectionAnchors.size())
        computeScmFileTops();
    if (index >= m_scmFileTops.size() || m_scmFileTops.at(index) < 0) {
        m_scmDiffActiveOutline->hide();
        return;
    }
    const int docHeight =
        m_scmDiff->document()->documentLayout()->documentSize().height();
    const int top = m_scmFileTops.at(index);
    const int bottom =
        (index + 1 < m_scmFileTops.size() && m_scmFileTops.at(index + 1) >= 0)
            ? m_scmFileTops.at(index + 1)
            : docHeight;

    const int viewTop = vbar->value();
    const int height = m_scmDiff->viewport()->height();
    if (bottom - viewTop <= 0 || top - viewTop >= height) {
        m_scmDiffActiveOutline->hide();
        return;
    }
    // A diff row runs the full width of the document, so the outline sits on
    // the viewport's own edges: inset it and the row's border shows alongside,
    // offset it and its right edge is clipped away.
    int outlineTop = top - viewTop;
    if (outlineTop < 0 && m_scmStickyHeader)
        outlineTop = qMax(outlineTop,
                          diffStickyTopOffset(
                              m_scmStickyHeader->sizeHint().height(), viewTop,
                              bottom));
    const bool wasHidden = m_scmDiffActiveOutline->isHidden();
    placeDiffOverlay(m_scmDiffActiveOutline, m_scmDiff,
                     QRect(0, outlineTop, m_scmDiff->viewport()->width(),
                           qMax(1, bottom - viewTop - outlineTop)));
    if (wasHidden) {
        // Only on the way back into view: raise() dirties the widget, and
        // doing that on every scroll tick is what makes the outline flicker.
        m_scmDiffActiveOutline->show();
        m_scmDiffActiveOutline->raise();
    }
}

void MainWindow::updateScmDiffScrollState()
{
    if (!m_scmDiff || !m_scmStickyHeader)
        return;
    if (m_scmSectionKeys.isEmpty()) {
        m_scmStickyHeader->hide();
        if (m_scmNextFileBar)
            m_scmNextFileBar->hide();
        if (m_scmDiffActiveOutline)
            m_scmDiffActiveOutline->hide();
        return;
    }
    QScrollBar *vbar = m_scmDiff->verticalScrollBar();
    if (!vbar)
        return;
    if (vbar->value() <= vbar->minimum()) {
        m_scmStickyHeader->hide();
        updateScmDiffActiveOutline();
        updateScmNextFileBar();
        return;
    }
    const int viewTop = vbar->value();
    const int viewBottom = viewTop + m_scmDiff->viewport()->height();
    const int docHeight =
        m_scmDiff->document()->documentLayout()->documentSize().height();
    if (m_scmFileTops.size() != m_scmSectionKeys.size())
        computeScmFileTops();

    int idx = -1, fileTop = 0, fileBottom = 0;
    for (int i = 0; i < m_scmSectionKeys.size(); ++i) {
        if (m_scmFileTops.at(i) < 0)
            continue;
        const int bottom = (i + 1 < m_scmFileTops.size() &&
                            m_scmFileTops.at(i + 1) >= 0)
                               ? m_scmFileTops.at(i + 1)
                               : docHeight;
        if (bottom > viewTop) {
            idx = i;
            fileTop = m_scmFileTops.at(i);
            fileBottom = bottom;
            break;
        }
    }
    if (idx < 0) {
        idx = m_scmSectionKeys.indexOf(m_scmStickySection);
        if (idx < 0)
            idx = 0;
        fileTop = 0;
        fileBottom = qMax(1, docHeight);
    }

    double progress = 1.0;
    if (fileBottom > fileTop)
        progress = double(viewBottom - fileTop) / double(fileBottom - fileTop);
    progress = qBound(0.0, progress, 1.0);

    const QString key = m_scmSectionKeys.at(idx);
    const QString path = m_scmSectionPaths.at(idx);
    const bool isViewed = loadDiffViewed(scmViewedContext()).contains(path);
    if (key != m_scmStickySection) {
        m_scmStickySection = key;
        m_scmStickyPath->setText(m_scmStickyLabelHtml.contains(key)
                                     ? m_scmStickyLabelHtml.value(key)
                                     : diffStickyPathHtml(path));
        m_scmStickyPath->setToolTip(path); // the bar may elide a long path
        selectScmFileInTree(path, key.startsWith(QLatin1String("s|")));
    }
    m_scmActiveSectionKey = key;
    m_scmStickyControls->setText(diffRowControlsHtml(
        path, progress, isViewed, /*comments=*/false,
        m_scmStickyActionHtml.value(key),
        diffExpandedViewed(scmViewedContext()).contains(path)));

    const bool stickyWasHidden = m_scmStickyHeader->isHidden();
    m_scmStickyTopOffset = diffStickyTopOffset(
        m_scmStickyHeader->sizeHint().height(), viewTop, fileBottom);
    layoutScmStickyHeader();
    if (stickyWasHidden) {
        m_scmStickyHeader->show();
        m_scmStickyHeader->raise();
        if (m_scmDiffActiveOutline && m_scmDiffActiveOutline->isVisible())
            m_scmDiffActiveOutline->raise();
    }
    updateScmDiffActiveOutline();
    updateScmNextFileBar();
}

void MainWindow::updateScmNextFileBar()
{
    if (!m_scmNextFileBar || !m_scmDiff)
        return;
    QScrollBar *vbar = m_scmDiff->verticalScrollBar();
    if (!vbar || m_scmSectionKeys.isEmpty()) {
        m_scmNextFileBar->hide();
        m_scmNextFileKey.clear();
        return;
    }
    if (m_scmFileTops.size() != m_scmSectionKeys.size())
        computeScmFileTops();
    const int barHeight = m_scmNextFileBar->sizeHint().height();
    const int viewBottom =
        vbar->value() + m_scmDiff->viewport()->height() - barHeight;
    int next = -1;
    for (int i = 0; i < m_scmFileTops.size(); ++i) {
        if (m_scmFileTops.at(i) >= viewBottom) {
            next = i;
            break;
        }
    }
    if (next < 0) {
        m_scmNextFileBar->hide();
        m_scmNextFileKey.clear();
        return;
    }
    const QString key = m_scmSectionKeys.at(next);
    if (key != m_scmNextFileKey) {
        m_scmNextFileKey = key;
        const QString path = m_scmSectionPaths.at(next);
        const bool dark = qApp->palette().color(QPalette::Base).lightness() < 128;
        m_scmNextFileBar->setStyleSheet(scmNextFileBarStyleSheet(m_diffFontPt));
        m_scmNextFileBar->setText(
            QStringLiteral("<a href='nextfile:' style='text-decoration:none'>"
                           "<span style='color:%1'>%2&nbsp;Next file&nbsp;&nbsp;"
                           "</span>%3</a>")
                .arg(Theme::nextOrHex(Theme::Role::FgMuted,
                                      dark ? QStringLiteral("#8b949e")
                                           : QStringLiteral("#6e7781")),
                     QString::fromUtf8("\xE2\x86\x93"), diffStickyPathHtml(path)));
        m_scmNextFileBar->setToolTip(QStringLiteral("Jump to %1").arg(path));
    }
    layoutScmNextFileBar();
    if (m_scmNextFileBar->isHidden()) {
        m_scmNextFileBar->show();
        m_scmNextFileBar->raise();
    }
}

void MainWindow::layoutScmNextFileBar()
{
    if (!m_scmNextFileBar || !m_scmDiff)
        return;
    const int height = m_scmNextFileBar->sizeHint().height();
    placeDiffOverlay(m_scmNextFileBar, m_scmDiff,
                     QRect(0, m_scmDiff->viewport()->height() - height,
                           m_scmDiff->viewport()->width(), height));
}

struct ScmAutoViewedDelta {
    QStringList toView;
    QStringList toUnview;
    QString currentPath;
    bool currentStaged = false;
};

static ScmAutoViewedDelta scmAutoViewedDelta(
    const QStringList &keys, const QStringList &paths, const QList<int> &tops,
    int docHeight, int viewTop, int viewBottom, bool scrollingUp,
    const QSet<QString> &viewed)
{
    ScmAutoViewedDelta delta;
    for (int i = 0; i < keys.size() && i < paths.size() && i < tops.size(); ++i) {
        if (tops.at(i) < 0)
            continue;
        const int bottom =
            (i + 1 < tops.size() && tops.at(i + 1) >= 0) ? tops.at(i + 1)
                                                         : docHeight;
        const QString path = paths.at(i);
        if (scrollingUp) {
            if (bottom > viewTop) {
                if (delta.currentPath.isEmpty()) {
                    delta.currentPath = path;
                    delta.currentStaged =
                        keys.at(i).startsWith(QLatin1String("s|"));
                }
                if (viewed.contains(path) && !delta.toUnview.contains(path))
                    delta.toUnview << path;
            }
        } else if (bottom <= viewBottom) {
            if (!viewed.contains(path) && !delta.toView.contains(path))
                delta.toView << path;
        } else if (delta.currentPath.isEmpty()) {
            delta.currentPath = path;
            delta.currentStaged = keys.at(i).startsWith(QLatin1String("s|"));
        }
    }
    return delta;
}

void MainWindow::applyScmAutoMarkViewedOnScroll()
{
    if (!m_scmDiff || m_scmSectionKeys.isEmpty())
        return;
    if (!autoMarkViewedOnScrollPref() || diffIsPaginated(m_scmDiff))
        return;
    QScrollBar *vbar = m_scmDiff->verticalScrollBar();
    if (!vbar)
        return;
    const int viewTop = vbar->value();
    const int viewBottom = viewTop + m_scmDiff->viewport()->height();
    const int docHeight =
        m_scmDiff->document()->documentLayout()->documentSize().height();
    if (m_scmFileTops.size() != m_scmSectionKeys.size())
        computeScmFileTops();

    const QString ctx = scmViewedContext();
    const QSet<QString> viewed = loadDiffViewed(ctx);
    const bool scrollingUp = m_scmAutoViewedScrollDirection < 0;
    const ScmAutoViewedDelta delta =
        scmAutoViewedDelta(m_scmSectionKeys, m_scmSectionPaths, m_scmFileTops,
                           docHeight, viewTop, viewBottom, scrollingUp, viewed);
    if (delta.toView.isEmpty() && delta.toUnview.isEmpty())
        return;
    m_scmApplyingAutoViewed = true;
    for (const QString &path : delta.toView)
        setDiffViewed(ctx, path, true);
    for (const QString &path : delta.toUnview)
        setDiffViewed(ctx, path, false);
    renderScmCombinedDiff();
    if (!delta.currentPath.isEmpty())
        scrollScmDiffToFile(delta.currentPath, delta.currentStaged);
    m_scmLastAutoViewedScrollValue = vbar->value();
    m_scmApplyingAutoViewed = false;
}

void MainWindow::updateScmViewedCount()
{
    if (!m_scmViewedLabel)
        return;
    const QSet<QString> paths(m_scmSectionPaths.begin(), m_scmSectionPaths.end());
    if (paths.isEmpty()) {
        m_scmViewedLabel->clear();
        return;
    }
    const QString context = scmViewedContext();
    const QSet<QString> viewed = loadDiffViewed(context);
    int seen = 0;
    for (const QString &p : paths)
        if (viewed.contains(p))
            ++seen;
    m_scmViewedLabel->setText(diffViewedCountHtml(
        seen, paths.size(),
        diffViewedMarksCurrent(context, QStringList(paths.begin(), paths.end()))));
}

#ifdef FORKMESH_WINDOW_TESTS
bool MainWindow::testScmDiffUsesFullSurface() const
{
    if (!m_scmDiff || !m_scmDiff->parentWidget() ||
        !m_scmDiff->parentWidget()->layout())
        return false;
    const QMargins margins = m_scmDiff->parentWidget()->layout()->contentsMargins();
    return margins.left() == 0 && margins.top() == 0 && margins.right() == 0 &&
           margins.bottom() == 0 && m_scmDiff->document()->documentMargin() == 0;
}

bool MainWindow::testScmDiffFilePinnedToTop(const QString &path) const
{
    if (!m_scmDiff || !m_scmStickyHeader)
        return false;
    const int index = m_scmSectionPaths.indexOf(path);
    QScrollBar *vbar = m_scmDiff->verticalScrollBar();
    if (index < 0 || index >= m_scmFileTops.size() || !vbar)
        return false;
    const int top = m_scmFileTops.at(index);
    const int expected = qBound(vbar->minimum(), top, vbar->maximum());
    return top >= 0 && vbar->value() == expected &&
           m_scmStickySection == m_scmSectionKeys.value(index) &&
           m_scmStickyHeader->geometry().top() == 0;
}

QString MainWindow::testScmViewedClickKeepsViewport(const QString &path,
                                                    const QString &bodyText)
{
    if (!m_scmDiff)
        return QStringLiteral("no-pane");
    const int index = m_scmSectionPaths.indexOf(path);
    if (index < 0)
        return QStringLiteral("no-file");
    flushDiffStream(m_scmDiff);
    QScrollBar *vbar = m_scmDiff->verticalScrollBar();
    if (!vbar)
        return QStringLiteral("no-scrollbar");
    computeScmFileTops();
    const int top = m_scmFileTops.value(index, -1);
    if (top < 0)
        return QStringLiteral("no-top");
    const quint64 renders = m_scmDiffRenders;
    const QUrl anchor(QStringLiteral("viewed:") +
                      QString::fromLatin1(QUrl::toPercentEncoding(path)));
    auto round = [&](int park, bool *folded, bool *reopened) {
        vbar->setValue(qBound(vbar->minimum(), park, vbar->maximum()));
        const int before = vbar->value();
        onScmDiffAnchorClicked(anchor);
        QApplication::processEvents();
        const int afterFold = vbar->value();
        *folded = !m_scmDiff->document()->toPlainText().contains(bodyText);
        onScmDiffAnchorClicked(anchor);
        QApplication::processEvents();
        const int afterReopen = vbar->value();
        *reopened = m_scmDiff->document()->toPlainText().contains(bodyText);
        return QStringLiteral("%1/%2/%3").arg(before).arg(afterFold).arg(afterReopen);
    };
    bool folded = false;
    bool reopened = false;
    const QString open = round(top - 40, &folded, &reopened);
    bool stuckFolded = false;
    bool stuckReopened = false;
    const QString stuck = round(top + 40, &stuckFolded, &stuckReopened);
    return QStringLiteral("renders=%1 scroll=%2 stuck=%3 top=%4 folded=%5 reopened=%6")
        .arg(m_scmDiffRenders - renders)
        .arg(open, stuck)
        .arg(top)
        .arg(folded && stuckFolded ? 1 : 0)
        .arg(reopened && stuckReopened ? 1 : 0);
}

QString MainWindow::testScmNextFileBarText(int scrollTo)
{
    if (!m_scmDiff || !m_scmNextFileBar)
        return QStringLiteral("no-pane");
    flushDiffStream(m_scmDiff);
    QScrollBar *vbar = m_scmDiff->verticalScrollBar();
    if (!vbar)
        return QStringLiteral("no-scrollbar");
    vbar->setValue(qBound(vbar->minimum(), scrollTo, vbar->maximum()));
    QApplication::processEvents();
    updateScmDiffScrollState();
    if (m_scmNextFileBar->isHidden())
        return QString();
    const int idx = m_scmSectionKeys.indexOf(m_scmNextFileKey);
    return idx < 0 ? QStringLiteral("no-key") : m_scmSectionPaths.at(idx);
}

QString MainWindow::testScmStickyControlsHtml(int scrollTo)
{
    if (!m_scmDiff || !m_scmStickyControls)
        return QStringLiteral("no-pane");
    flushDiffStream(m_scmDiff);
    QScrollBar *vbar = m_scmDiff->verticalScrollBar();
    if (!vbar)
        return QStringLiteral("no-scrollbar");
    vbar->setValue(qBound(vbar->minimum(), scrollTo, vbar->maximum()));
    QApplication::processEvents();
    updateScmDiffScrollState();
    return m_scmStickyHeader->isHidden() ? QString() : m_scmStickyControls->text();
}

bool MainWindow::testScmAutoViewedRoundTrip()
{
    const QStringList keys{QStringLiteral("u|a"), QStringLiteral("u|b")};
    const QStringList paths{QStringLiteral("a"), QStringLiteral("b")};
    const ScmAutoViewedDelta down =
        scmAutoViewedDelta(keys, paths, {0, 100}, 200, 100, 200, false, {});
    const QSet<QString> viewed(down.toView.begin(), down.toView.end());
    const ScmAutoViewedDelta up =
        scmAutoViewedDelta(keys, paths, {0, 20}, 40, 0, 100, true, viewed);
    return down.toView == paths && up.toUnview == paths &&
           up.currentPath == QStringLiteral("a");
}
#endif

void MainWindow::applyScmChangeBlock(const QUrl &url)
{
    if (sourceControlShowsRange() || !m_scmPatchValid)
        return;
    const QString dir = sourceControlGitDir();
    const QUrlQuery query(url);
    const QString path = url.path(QUrl::FullyDecoded);
    const bool staged = query.queryItemValue(QStringLiteral("s")) == QLatin1String("1");
    const bool toNew = query.queryItemValue(QStringLiteral("d")) == QLatin1String("new");
    bool oldOk = false;
    bool newOk = false;
    const int oldStart = query.queryItemValue(QStringLiteral("o")).toInt(&oldOk);
    const int newStart = query.queryItemValue(QStringLiteral("n")).toInt(&newOk);
    if (dir.isEmpty() || path.isEmpty() || !oldOk || !newOk || (staged && toNew))
        return;

    const QStringList sections =
        forkmesh::diffblock::splitFilePatches(m_scmCombinedPatch);
    QString blockPatch;
    for (int i = 0; i < sections.size() && blockPatch.isEmpty(); ++i) {
        if ((i < m_scmCombinedStagedFiles) != staged ||
            forkmesh::diffblock::filePatchPath(sections.at(i)) != path)
            continue;
        blockPatch = forkmesh::diffblock::changeBlockPatch(sections.at(i), oldStart,
                                                           newStart);
    }
    if (blockPatch.isEmpty()) {
        flashMessage(QStringLiteral("That change has moved; refreshing the diff."));
        refreshSourceControl();
        return;
    }

    QStringList args{QStringLiteral("apply"), QStringLiteral("--unidiff-zero"),
                     QStringLiteral("--whitespace=nowarn")};
    if (staged || toNew)
        args << QStringLiteral("--cached");
    if (!toNew)
        args << QStringLiteral("-R");
    const QString title = staged  ? QStringLiteral("Unstage change")
                          : toNew ? QStringLiteral("Stage change")
                                  : QStringLiteral("Revert change");
    queueScmMutation(
        QStringLiteral("block|") + dir + QLatin1Char('|') + path + QLatin1Char('|') +
            url.query(),
        [dir, args, input = blockPatch.toUtf8()] {
            ScmMutationResult result;
            result.ok = runGitCaptureInput(dir, args, input, &result.out,
                                           &result.error);
            return result;
        },
        [this, title](const ScmMutationResult &result) {
            if (!result.ok)
                QMessageBox::warning(this, title,
                                     result.error.isEmpty()
                                         ? QStringLiteral("git apply failed.")
                                         : result.error);
            refreshSourceControl();
        });
}

void MainWindow::onScmDiffAnchorClicked(const QUrl &url)
{
    if (url.scheme() == QLatin1String("scmblock")) {
        applyScmChangeBlock(url);
        return;
    }
    if (url.scheme() == QLatin1String("fmmergerange")) {
        m_mergeRange = mergeRangeIndexForKey(url.path());
        renderScmCombinedDiff(); // the pane memo is keyed by range
        return;
    }
    if (url.scheme() != QLatin1String("viewed") &&
        url.scheme() != QLatin1String("expand"))
        return;
    const QString path = url.path();
    if (path.isEmpty())
        return;
    const QString ctx = scmViewedContext();
    if (!toggleDiffExpandedFromAnchor(url, ctx))
        setDiffViewed(ctx, path, !loadDiffViewed(ctx).contains(path));
    // The pill sits in the file's own header, so every row above it keeps its
    // place across the fold: swap the one block and leave the viewport where
    // the reviewer left it. Only a re-render moves the page under them.
    if (swapScmDiffFileInPlace(path))
        return;
    renderScmCombinedDiff();
    scrollScmDiffToFile(path, m_scmStickySection.startsWith(QLatin1String("s|")));
}

void MainWindow::scmSelectAdjacentChange(int delta)
{
    if (sourceControlShowsRange() && m_scmTree) {
        stepScmTreeFile(delta);
        return;
    }
    scmScrollToAdjacentHunk(delta);
}

bool MainWindow::scmScrollToAdjacentHunk(int delta, bool fromEnd)
{
    if (!m_scmDiff)
        return false;
    if (fromEnd)
        m_scmDiff->moveCursor(QTextCursor::End);
    const QTextDocument::FindFlags flags =
        delta < 0 ? QTextDocument::FindBackward : QTextDocument::FindFlags();
    if (!m_scmDiff->find(QStringLiteral("@@ -"), flags)) {
        return showDiffPageContainingText(
            m_scmDiff, QStringLiteral("@@ -"), delta,
            /*wrap=*/false);
    }
    const QTextCursor found = m_scmDiff->textCursor();
    QTextCursor lineCur(found);
    lineCur.setPosition(found.selectionStart());
    lineCur.movePosition(QTextCursor::StartOfLine);
    const QRect r = m_scmDiff->cursorRect(lineCur);
    if (QScrollBar *vbar = m_scmDiff->verticalScrollBar())
        vbar->setValue(vbar->value() + r.top() - 4);
    return true;
}

void MainWindow::scmStagePath(const QString &path)
{
    QString nextPath;
    bool nextStaged = false;
    if (m_scmTree) {
        QList<QTreeWidgetItem *> files;
        for (int g = 0; g < m_scmTree->topLevelItemCount(); ++g) {
            QTreeWidgetItem *grp = m_scmTree->topLevelItem(g);
            for (int c = 0; c < grp->childCount(); ++c)
                files.append(grp->child(c));
        }
        QTreeWidgetItem *cur = m_scmTree->currentItem();
        const int idx = cur ? files.indexOf(cur) : -1;
        if (idx >= 0 && cur->data(0, Qt::UserRole).toString() == path &&
            !cur->data(0, Qt::UserRole + 1).toBool()) {
            for (int i = idx + 1; i < files.size(); ++i) {
                if (!files.at(i)->data(0, Qt::UserRole + 1).toBool()) {
                    nextPath = files.at(i)->data(0, Qt::UserRole).toString();
                    break;
                }
            }
            if (nextPath.isEmpty() && idx > 0) {
                nextPath = files.at(idx - 1)->data(0, Qt::UserRole).toString();
                nextStaged = files.at(idx - 1)->data(0, Qt::UserRole + 1).toBool();
            }
        }
    }

    const QString dir = sourceControlGitDir();
    queueScmMutation(
        QStringLiteral("stage|") + dir + QLatin1Char('|') + path,
        [dir, path] {
            return scmMutate(dir, {QStringLiteral("add"), QStringLiteral("-A"),
                                   QStringLiteral("--"), path});
        },
        [this, dir, nextPath, nextStaged](const ScmMutationResult &result) {
            if (!result.ok)
                QMessageBox::warning(this, "Stage",
                                     result.error.isEmpty() ? "git add failed."
                                                            : result.error);
            refreshSourceControl();
            if (nextPath.isEmpty() || !m_scmTree || sourceControlGitDir() != dir)
                return;
            if (QTreeWidgetItem *item = scmFindItem(nextPath, nextStaged)) {
                m_scmTree->setCurrentItem(item);
                m_scmTree->scrollToItem(item);
            }
        });
}

void MainWindow::scmUnstagePath(const QString &path)
{
    const QString dir = sourceControlGitDir();
    queueScmMutation(
        QStringLiteral("unstage|") + dir + QLatin1Char('|') + path,
        [dir, path] {
            ScmMutationResult result;
            result.ok = runGitCapture(dir, {"restore", "--staged", "--", path},
                                      nullptr, &result.error) ||
                        runGitCapture(dir, {"reset", "-q", "--", path}, nullptr,
                                      nullptr);
            return result;
        },
        [this](const ScmMutationResult &result) {
            if (!result.ok)
                QMessageBox::warning(this, "Unstage",
                                     result.error.isEmpty() ? "git restore failed."
                                                            : result.error);
            refreshSourceControl();
        });
}

static bool scmRelPathIsSafe(const QString &path);

void MainWindow::scmDiscardPath(const QString &path, bool untracked)
{
    if (!scmRelPathIsSafe(path))
        return;
    if (QMessageBox::warning(
            this, "Discard changes",
            QStringLiteral("Discard changes to \"%1\"? This cannot be undone.").arg(path),
            QMessageBox::Discard | QMessageBox::Cancel) != QMessageBox::Discard)
        return;
    const QString dir = sourceControlGitDir();
    bool changed = false;
    if (untracked && QFileInfo(QDir(dir).absoluteFilePath(path)).isDir()) {
        queueScmMutation(
            QStringLiteral("discard|") + dir + QLatin1Char('|') + path,
            [dir, path] {
                return scmMutate(dir, {QStringLiteral("clean"), QStringLiteral("-fd"),
                                       QStringLiteral("--"), path});
            },
            [this, path](const ScmMutationResult &result) {
                if (!result.ok)
                    QMessageBox::warning(this, "Discard changes",
                                         QStringLiteral("Could not remove %1.").arg(path));
                refreshSourceControl(true);
            });
        return;
    }
    if (untracked) {
        const QString absolute = QDir(dir).absoluteFilePath(path);
        changed = QFile::remove(absolute);
        if (!changed)
            QMessageBox::warning(this, "Discard changes",
                                 QStringLiteral("Could not remove %1.").arg(path));
    } else {
        // Restore both the index and working tree. Restoring only the worktree
        // from the index made the round-arrow appear to do nothing whenever a
        // file also had staged changes.
        queueScmMutation(
            QStringLiteral("discard|") + dir + QLatin1Char('|') + path,
            [dir, path] {
                ScmMutationResult result = scmMutate(
                    dir, {QStringLiteral("restore"), QStringLiteral("--source=HEAD"),
                          QStringLiteral("--staged"), QStringLiteral("--worktree"),
                          QStringLiteral("--"), path});
                if (!result.ok) {
                    runGitCapture(dir, {QStringLiteral("reset"), QStringLiteral("HEAD"),
                                        QStringLiteral("--"), path}, nullptr, nullptr);
                    result.ok = runGitCapture(
                        dir, {QStringLiteral("checkout"), QStringLiteral("HEAD"),
                              QStringLiteral("--"), path}, nullptr, &result.error);
                }
                return result;
            },
            [this](const ScmMutationResult &result) {
                if (!result.ok) {
                    QMessageBox::warning(this, "Discard changes",
                                         result.error.isEmpty() ? "git restore failed."
                                                                : result.error);
                    return;
                }
                refreshSourceControl(true);
            });
        return;
    }
    if (changed)
        refreshSourceControl(true);
}

static bool scmRelPathIsSafe(const QString &path)
{
    const QString clean = QDir::cleanPath(path);
    return clean == path && !clean.isEmpty() && clean != QLatin1String(".") &&
           !clean.startsWith(QLatin1String("../")) &&
           !clean.contains(QLatin1String("/../")) &&
           !QDir::isAbsolutePath(clean) && clean != QLatin1String(".git") &&
           !clean.startsWith(QLatin1String(".git/"));
}

void MainWindow::showScmFileMenu(const QPoint &pos)
{
    if (!m_scmTree)
        return;
    QTreeWidgetItem *item = m_scmTree->itemAt(pos);
    const QString path = item ? item->data(0, Qt::UserRole).toString() : QString();
    if (path.isEmpty()) {
        const QString dir = sourceControlGitDir();
        if (dir.isEmpty() || !QDir(dir).exists())
            return;
        QMenu menu(this);
        QAction *openLocation =
            menu.addAction(QStringLiteral("Open repository location"));
        openLocation->setIcon(themedOcticon(QStringLiteral("file-directory"),
                                            QColor(QStringLiteral("#8b949e")), 14));
        if (menu.exec(m_scmTree->viewport()->mapToGlobal(pos)) == openLocation &&
            !QDesktopServices::openUrl(QUrl::fromLocalFile(dir))) {
            flashMessage(QStringLiteral("Could not open repository location: %1")
                             .arg(QDir::toNativeSeparators(dir)),
                         true);
        }
        return;
    }
    if (!scmRelPathIsSafe(path))
        return;
    const QString dir = sourceControlGitDir();
    if (dir.isEmpty())
        return;
    if (item != m_scmTree->currentItem())
        m_scmTree->setCurrentItem(item);

    const bool onDisk = QFileInfo(QDir(dir).filePath(path)).isFile();
    const bool rangeRow = item->data(0, Qt::UserRole + 3).toBool();
    // Undoing a file's branch work lands as a commit, so the branch has to be
    // checked out somewhere: a range read straight out of the repository has no
    // worktree to write it into.
    const bool canRevert = rangeRow && !m_branchDiffWorkDir.isEmpty() &&
                           QDir(m_branchDiffWorkDir).exists();

    QMenu menu(this);
    QAction *edit = menu.addAction(QStringLiteral("Open in file editor"));
    edit->setIcon(themedOcticon(QStringLiteral("pencil"),
                                QColor(QStringLiteral("#8b949e")), 14));
    QAction *revert = nullptr;
    if (rangeRow) {
        menu.addSeparator();
        revert = menu.addAction(QStringLiteral("Revert to %1 and commit")
                                    .arg(branchCompareBase()));
        // Deliberately outside the on-disk gate below: a file the branch deleted
        // is missing from disk and is exactly the case worth reverting.
        revert->setEnabled(canRevert);
        if (!canRevert) {
            menu.setToolTipsVisible(true);
            revert->setToolTip(
                QStringLiteral("%1 is not checked out in a worktree here, so "
                               "there is nowhere to commit the revert.")
                    .arg(m_branchDiffBranch));
        }
    }
    menu.addSeparator();
    QAction *ignore = menu.addAction(QStringLiteral("Add to .gitignore"));
    QAction *remove = menu.addAction(QStringLiteral("Delete file"));
    for (QAction *action : {edit, ignore, remove})
        action->setEnabled(onDisk);

    QAction *chosen = menu.exec(m_scmTree->viewport()->mapToGlobal(pos));
    if (chosen == edit)
        openWorkingTreeFile(rangeRow ? m_branchDiffWorkDir : dir, path);
    else if (revert && chosen == revert)
        confirmScmRevertPathToBase(path);
    else if (chosen == ignore)
        scmIgnorePath(path);
    else if (chosen == remove)
        scmDeletePath(path);
}

void MainWindow::confirmScmRevertPathToBase(const QString &path)
{
    if (m_scmRevertInFlight) {
        flashMessage(QStringLiteral("A file revert is already running."));
        return;
    }
    const QString base = branchCompareBase();
    if (QMessageBox::warning(
            this, "Revert file",
            QStringLiteral("Undo every change to \"%1\" on %2 and commit that "
                           "revert?\n\nThe file goes back to its %3 content in a "
                           "new commit of its own — anything else you have "
                           "staged is left alone.")
                .arg(path, m_branchDiffBranch, base),
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    ScmRevertRequest request;
    QString error;
    const auto fail = [this, path](const QString &summary) {
        QMessageBox::warning(this, "Revert file",
                             summary.isEmpty()
                                 ? QStringLiteral("Could not revert %1.").arg(path)
                                 : summary);
    };
    if (!scmRevertRequestFor(path, &request, &error)) {
        fail(error);
        return;
    }
    m_scmRevertInFlight = true;
    runOffThread<ScmRevertResult>(
        [request] { return runScmRevertPathToBase(request); },
        [this, request, fail](ScmRevertResult result) {
            m_scmRevertInFlight = false;
            applyScmRevertPathToBase(request, result);
            if (!result.ok) {
                fail(result.summary);
                return;
            }
            flashMessage(result.summary);
        });
}

// Undo one file's share of the branch range and commit that revert on its own.
// Nothing here prompts: the caller confirms and reports, so a test can drive the
// whole operation headless.
bool MainWindow::scmRevertPathToBase(const QString &path, QString *summary)
{
    ScmRevertRequest request;
    QString error;
    if (!scmRevertRequestFor(path, &request, &error)) {
        if (summary)
            *summary = error;
        return false;
    }
    const ScmRevertResult result = runScmRevertPathToBase(request);
    applyScmRevertPathToBase(request, result);
    if (summary)
        *summary = result.summary;
    return result.ok;
}

bool MainWindow::scmRevertRequestFor(const QString &path,
                                     ScmRevertRequest *request,
                                     QString *error) const
{
    if (!scmRelPathIsSafe(path)) {
        *error = QStringLiteral("\"%1\" is not a path inside this repository.")
                     .arg(path);
        return false;
    }
    request->path = path;
    request->branch = m_branchDiffBranch;
    request->dir = m_branchDiffWorkDir;
    if (request->branch.isEmpty()) {
        *error = QStringLiteral("No branch comparison is open.");
        return false;
    }
    if (request->dir.isEmpty() || !QDir(request->dir).exists()) {
        *error = QStringLiteral("%1 is not checked out in a worktree here, so "
                                "there is nowhere to commit the revert.")
                     .arg(request->branch);
        return false;
    }
    request->base = branchCompareBase();
    return true;
}

MainWindow::ScmRevertResult
MainWindow::runScmRevertPathToBase(const ScmRevertRequest &request)
{
    ScmRevertResult result;
    const auto report = [&result](bool ok, const QString &text) {
        result.ok = ok;
        result.summary = text;
        return result;
    };
    const QString &path = request.path;
    const QString &branch = request.branch;
    const QString &dir = request.dir;
    const QString &base = request.base;

    // The panel diffs against the merge base, so undo against that same commit:
    // taking the file from the tip of the base would import its later work too.
    QByteArray mergeBaseOut;
    const bool foundMergeBase =
        runGitCapture(dir, {"merge-base", base, branch}, &mergeBaseOut, nullptr);
    const QString contentBase = foundMergeBase && !mergeBaseOut.trimmed().isEmpty()
                                    ? QString::fromUtf8(mergeBaseOut).trimmed()
                                    : base;

    const QString absolute = QDir(dir).filePath(path);
    const bool inBase = runGitCapture(
        dir, {"cat-file", "-e", contentBase + QLatin1Char(':') + path}, nullptr,
        nullptr);
    const bool tracked = runGitCapture(
        dir, {"ls-files", "--error-unmatch", "--", path}, nullptr, nullptr);
    if (!inBase && !tracked && !QFileInfo::exists(absolute))
        return report(false,
                      QStringLiteral("%1 already matches %2.").arg(path, base));

    QString err;
    if (inBase) {
        // One step restores the content and stages it, whether the branch
        // modified the file or deleted it outright.
        if (!runSourceControlMutation(dir, {"checkout", contentBase, "--", path},
                                      nullptr, &err))
            return report(false, err.isEmpty()
                                     ? QStringLiteral("git checkout failed.")
                                     : err.left(300));
    } else if (tracked) {
        if (!runSourceControlMutation(dir, {"rm", "-f", "-q", "--", path}, nullptr,
                                      &err))
            return report(false, err.isEmpty() ? QStringLiteral("git rm failed.")
                                               : err.left(300));
    } else if (!QFile::remove(absolute)) {
        return report(false,
                      QStringLiteral("Could not delete %1.").arg(absolute));
    }
    result.closeTabs = !inBase;
    result.refresh = true;

    // Exactly what a path-limited commit would record: the worktree against
    // HEAD. Nothing there means the file's whole change was uncommitted, so the
    // revert above already finished the job.
    QByteArray pending;
    if (runGitCapture(dir, {"diff", "--name-only", "HEAD", "--", path}, &pending,
                      nullptr) &&
        pending.trimmed().isEmpty()) {
        result.logLine = QStringLiteral("Git: reverted %1 to %2 in the %3 worktree.")
                             .arg(path, base, branch);
        return report(true, QStringLiteral("Reverted %1 to %2 — the change was "
                                           "uncommitted, so there was nothing "
                                           "to commit.")
                                .arg(path, base));
    }

    QString ratchetReason;
    if (!RepoStatsStore::stagedCommitAllowed(dir, &ratchetReason))
        return report(false, QStringLiteral("%1 is reverted in the worktree but "
                                            "not committed. %2")
                                 .arg(path, ratchetReason));

    // Naming the path keeps this to a one-file commit: whatever else is staged
    // in the worktree stays staged and uncommitted.
    const QString message = QStringLiteral("Revert %1 to %2").arg(path, base);
    if (!runSourceControlMutation(dir, {"commit", "-m", message, "--", path},
                                  nullptr, &err))
        return report(false, QStringLiteral("%1 is reverted in the worktree but "
                                            "the commit failed. %2")
                                 .arg(path, err.isEmpty()
                                                ? QStringLiteral("git commit failed.")
                                                : err.left(300)));
    result.committed = true;
    result.logLine = QStringLiteral("Git: reverted %1 to %2 on %3 with a new commit.")
                         .arg(path, base, branch);
    return report(true, QStringLiteral("Reverted %1 to %2 and committed it.")
                            .arg(path, base));
}

void MainWindow::applyScmRevertPathToBase(const ScmRevertRequest &request,
                                          const ScmRevertResult &result)
{
    const bool stillOpen = m_branchDiffBranch == request.branch &&
                           m_branchDiffWorkDir == request.dir;
    if (result.closeTabs) { // the file is gone now; an editor tab on it is a ghost
        const QString absolute = QDir(request.dir).filePath(request.path);
        for (const QString &key : {absolute, worktreeTabKey(absolute)})
            closeRepoFileTabsUnder(key, false);
        if (stillOpen)
            closeRepoFileTabsUnder(request.path, false);
    }
    if (!result.logLine.isEmpty())
        logSystem(result.logLine);
    if (!result.refresh)
        return;
    if (result.committed && stillOpen)
        loadCommits(); // history first: the range read below wants the new tip
    forgetBranchDiff(request.branch); // the cached patch predates the revert
    if (stillOpen) {
        m_branchDiffLastValid = false;
        refreshSourceControl(/*force=*/true);
    }
    if (result.committed && stillOpen && m_repoDetailIndex >= 0)
        propagateRepoUpdate(m_repoDetailIndex);
}

void MainWindow::scmIgnorePath(const QString &path)
{
    const QString dir = sourceControlGitDir();
    if (dir.isEmpty() || !scmRelPathIsSafe(path))
        return;
    const QString rule = gitignoreRuleForPath(path);
    const QString ignoreFile = QDir(dir).filePath(QStringLiteral(".gitignore"));

    QStringList lines;
    QFile file(ignoreFile);
    if (file.exists()) {
        if (!file.open(QIODevice::ReadOnly)) {
            QMessageBox::warning(this, "Add to .gitignore",
                                 QStringLiteral("Could not read %1.").arg(ignoreFile));
            return;
        }
        lines = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));
        file.close();
        for (const QString &line : std::as_const(lines)) {
            const QString trimmed = line.trimmed();
            if (trimmed == rule || trimmed == path) {
                flashMessage(QStringLiteral("%1 is already in .gitignore.").arg(path));
                return;
            }
        }
    }

    if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        QMessageBox::warning(this, "Add to .gitignore",
                             QStringLiteral("Could not write %1.").arg(ignoreFile));
        return;
    }
    QByteArray out;
    if (!lines.isEmpty() && !lines.constLast().isEmpty())
        out.append('\n');
    out.append(rule.toUtf8());
    out.append('\n');
    const bool wrote = file.write(out) == out.size();
    file.close();
    if (!wrote) {
        QMessageBox::warning(this, "Add to .gitignore",
                             QStringLiteral("Could not write %1.").arg(ignoreFile));
        return;
    }

    const auto finish = [this, path] {
        flashMessage(QStringLiteral("Added %1 to .gitignore.").arg(path));
        refreshSourceControl(true);
    };
    runOffThread<bool>(
        [dir, path] {
            return runGitCapture(dir, {"ls-files", "--error-unmatch", "--", path},
                                 nullptr, nullptr);
        },
        [this, dir, path, finish](bool tracked) {
            if (!tracked ||
                QMessageBox::question(
                    this, QStringLiteral("Add to .gitignore"),
                    QStringLiteral("\"%1\" is tracked by git, so the new rule has "
                                   "no effect until git stops tracking it.\n\nStop "
                                   "tracking it? The file stays on disk.")
                        .arg(path),
                    QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes) {
                finish();
                return;
            }
            const bool queued = queueScmMutation(
                QStringLiteral("untrack|") + dir + QLatin1Char('|') + path,
                [dir, path] {
                    return scmMutate(dir, {"rm", "--cached", "-f", "-q", "--", path});
                },
                [this, finish](const ScmMutationResult &result) {
                    if (!result.ok)
                        QMessageBox::warning(
                            this, "Add to .gitignore",
                            result.error.isEmpty()
                                ? QStringLiteral("git rm --cached failed.")
                                : result.error.left(240));
                    finish();
                });
            if (!queued)
                finish();
        });
}

void MainWindow::scmDeletePath(const QString &path)
{
    deleteWorkingTreeFile(sourceControlGitDir(), path);
}

bool MainWindow::handleDiffFileActionLink(QTextEdit *view, const QUrl &url)
{
    DiffFileMenuTarget target;
    if (diffFileMenuTarget(url, &target)) {
        showDiffFileMenu(view, target);
        return true;
    }
    QString dir;
    QString path;
    if (diffEditTarget(url, &dir, &path)) {
        openWorkingTreeFile(dir, path);
        return true;
    }
    if (!diffDeleteTarget(url, &dir, &path))
        return false;
    deleteDiffFile(view, dir, path);
    return true;
}

void MainWindow::deleteDiffFile(QTextEdit *view, const QString &dir,
                                const QString &path)
{
    if (deleteWorkingTreeFile(dir, path))
        refreshDiffAfterFileDelete(view);
}

void MainWindow::ignoreScmDiffSecrets(const QList<RepoSecurityFinding> &findings)
{
    const QString dir = sourceControlGitDir();
    if (dir.isEmpty() || !repoHasWorkingTree())
        return;
    const QList<RepoSecurityFinding> marked = addSecretIgnoreMarkers(dir, findings);
    if (marked.isEmpty())
        return;
    m_scmSecretFindings =
        forkmesh::ui::secretFindingsWithout(m_scmSecretFindings, marked);
    if (m_scmSecretBanner)
        m_scmSecretBanner->removeFindings(marked);
    refreshSourceControl(true);
}

void MainWindow::refreshDiffAfterFileDelete(QTextEdit *view)
{
    if (!view)
        return;
    if (view == m_branchDiffView && !sourceControlShowsRange() &&
        !m_branchDiffBranch.isEmpty()) {
        m_branchDiffLastValid = false;
        forgetBranchDiff(m_branchDiffBranch);
        renderBranchScopeDiff();
    } else if (view == m_agentDiffView) {
        scheduleAgentFilesDiff(m_selectedAgentSessionId);
    }
}

bool MainWindow::diffBranchDeleteRequestFor(const DiffFileMenuTarget &target,
                                            ScmRevertRequest *request)
{
    const QString repoDir = target.repoDir;
    if (repoDir.isEmpty() || !scmRelPathIsSafe(target.path))
        return false;
    const QString checkedOut = headBranchFromFile(repoDir);
    const QString branch = target.branch.isEmpty() ? checkedOut : target.branch;
    if (branch.isEmpty() ||
        !runGitCapture(repoDir,
                       {"cat-file", "-e",
                        QStringLiteral("refs/heads/%1:%2").arg(branch, target.path)},
                       nullptr, nullptr))
        return false;
    request->path = target.path;
    request->branch = branch;
    request->dir = checkedOut == branch
                       ? repoDir
                       : worktreePathForBranch(repoDir, branch);
    return true;
}

void MainWindow::confirmDeleteDiffFileFromBranch(QTextEdit *view,
                                                 const ScmRevertRequest &request)
{
    if (m_scmRevertInFlight) {
        flashMessage(QStringLiteral("A file commit is already running."));
        return;
    }
    if (QMessageBox::warning(
            this, "Delete from branch",
            QStringLiteral("Delete \"%1\" from %2 and commit that?\n\nThe file "
                           "is removed in a new commit of its own — anything "
                           "else you have staged is left alone. Uncommitted "
                           "edits to this file are lost.")
                .arg(request.path, request.branch),
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    QPointer<QTextEdit> guard(view);
    m_scmRevertInFlight = true;
    runOffThread<ScmRevertResult>(
        [request] { return runScmDeletePathFromBranch(request); },
        [this, request, guard](ScmRevertResult result) {
            m_scmRevertInFlight = false;
            const bool stillOpen = m_branchDiffBranch == request.branch &&
                                   m_branchDiffWorkDir == request.dir;
            applyScmRevertPathToBase(request, result);
            if (result.refresh) {
                if (!stillOpen)
                    refreshSourceControl(true);
                refreshDiffAfterFileDelete(guard.data());
            }
            if (!result.ok) {
                QMessageBox::warning(
                    this, "Delete from branch",
                    result.summary.isEmpty()
                        ? QStringLiteral("Could not delete %1.").arg(request.path)
                        : result.summary);
                return;
            }
            flashMessage(result.summary);
        });
}

MainWindow::ScmRevertResult
MainWindow::runScmDeletePathFromBranch(const ScmRevertRequest &request)
{
    ScmRevertResult result;
    const auto report = [&result](bool ok, const QString &text) {
        result.ok = ok;
        result.summary = text;
        return result;
    };
    const QString &path = request.path;
    const QString &branch = request.branch;
    const QString &dir = request.dir;
    if (dir.isEmpty() || !QDir(dir).exists() || headBranchFromFile(dir) != branch)
        return report(false, QStringLiteral("%1 is not checked out in a worktree "
                                            "here, so there is nowhere to commit "
                                            "the deletion.")
                                 .arg(branch));

    QString err;
    if (!runSourceControlMutation(dir, {"rm", "-f", "-q", "--", path}, nullptr,
                                  &err))
        return report(false, err.isEmpty() ? QStringLiteral("git rm failed.")
                                           : err.left(300));
    result.closeTabs = true;
    result.refresh = true;

    QString ratchetReason;
    if (!RepoStatsStore::stagedCommitAllowed(dir, &ratchetReason))
        return report(false, QStringLiteral("%1 is deleted in the worktree but "
                                            "not committed. %2")
                                 .arg(path, ratchetReason));

    const QString message = QStringLiteral("Delete %1").arg(path);
    if (!runSourceControlMutation(dir, {"commit", "-m", message, "--", path},
                                  nullptr, &err))
        return report(false, QStringLiteral("%1 is deleted in the worktree but "
                                            "the commit failed. %2")
                                 .arg(path, err.isEmpty()
                                                ? QStringLiteral("git commit failed.")
                                                : err.left(300)));
    result.committed = true;
    result.logLine = QStringLiteral("Git: deleted %1 from %2 with a new commit.")
                         .arg(path, branch);
    return report(true, QStringLiteral("Deleted %1 from %2 and committed it.")
                            .arg(path, branch));
}

void MainWindow::showDiffFileMenu(QTextEdit *view, const DiffFileMenuTarget &target)
{
    const QColor iconColor(QStringLiteral("#8b949e"));
    const bool onDisk = !target.dir.isEmpty();
    const QString abs =
        onDisk ? QDir(target.dir).absoluteFilePath(target.path) : QString();
    QMenu menu(this);
    menu.setObjectName(QStringLiteral("diffFileMenu"));
    menu.setToolTipsVisible(true);
    QAction *edit = menu.addAction(QStringLiteral("Edit file"));
    edit->setIcon(themedOcticon(QStringLiteral("pencil"), iconColor, 14));
    QAction *openDefault = menu.addAction(QStringLiteral("Open with default app"));
    openDefault->setIcon(themedOcticon(QStringLiteral("link-external"), iconColor, 14));
    QAction *reveal = menu.addAction(QStringLiteral("Show in folder"));
    reveal->setIcon(themedOcticon(QStringLiteral("file-directory"), iconColor, 14));
    for (QAction *action : {edit, openDefault, reveal}) {
        action->setEnabled(onDisk);
        if (!onDisk)
            action->setToolTip(QStringLiteral(
                "This file is not in a checkout on this machine."));
    }
    menu.addSeparator();
    QAction *copyPath = menu.addAction(QStringLiteral("Copy path"));
    copyPath->setIcon(themedOcticon(QStringLiteral("copy"), iconColor, 14));
    QAction *copyFullPath = nullptr;
    if (onDisk)
        copyFullPath = menu.addAction(QStringLiteral("Copy full path"));
    QPointer<QTextEdit> guard(view);
    ScmRevertRequest branchDelete;
    const bool inBranch = diffBranchDeleteRequestFor(target, &branchDelete);
    QAction *remove = nullptr;
    QAction *removeFromBranch = nullptr;
    if (target.deletable || inBranch)
        menu.addSeparator();
    if (target.deletable) {
        remove = menu.addAction(QStringLiteral("Delete file…"));
        remove->setIcon(themedOcticon(QStringLiteral("trash"),
                                      QColor(QStringLiteral("#f85149")), 14));
    }
    if (inBranch) {
        removeFromBranch = menu.addAction(
            QStringLiteral("Delete from %1 and commit…").arg(branchDelete.branch));
        removeFromBranch->setIcon(themedOcticon(
            QStringLiteral("trash"), QColor(QStringLiteral("#f85149")), 14));
        if (branchDelete.dir.isEmpty()) {
            removeFromBranch->setEnabled(false);
            removeFromBranch->setToolTip(
                QStringLiteral("%1 is not checked out in a worktree here, so "
                               "there is nowhere to commit the deletion.")
                    .arg(branchDelete.branch));
        }
    }
    QAction *chosen = menu.exec(QCursor::pos());
    if (!chosen)
        return;
    if (chosen == edit) {
        openWorkingTreeFile(target.dir, target.path);
    } else if (chosen == openDefault) {
        if (!QDesktopServices::openUrl(QUrl::fromLocalFile(abs)))
            flashMessage(QStringLiteral("Could not open %1")
                             .arg(QDir::toNativeSeparators(abs)),
                         true);
    } else if (chosen == reveal) {
        const QString folder = QFileInfo(abs).absolutePath();
        if (!QDesktopServices::openUrl(QUrl::fromLocalFile(folder)))
            flashMessage(QStringLiteral("Could not open %1")
                             .arg(QDir::toNativeSeparators(folder)),
                         true);
    } else if (chosen == copyPath) {
        QGuiApplication::clipboard()->setText(target.path);
        flashMessage(QStringLiteral("Copied %1").arg(target.path));
    } else if (copyFullPath && chosen == copyFullPath) {
        const QString native = QDir::toNativeSeparators(abs);
        QGuiApplication::clipboard()->setText(native);
        flashMessage(QStringLiteral("Copied %1").arg(native));
    } else if (remove && chosen == remove) {
        deleteDiffFile(guard.data(), target.dir, target.path);
    } else if (removeFromBranch && chosen == removeFromBranch) {
        confirmDeleteDiffFileFromBranch(guard.data(), branchDelete);
    }
}

bool MainWindow::deleteWorkingTreeFile(const QString &dir, const QString &path)
{
    if (dir.isEmpty() || !scmRelPathIsSafe(path))
        return false;
    if (QMessageBox::warning(
            this, "Delete file",
            QStringLiteral("Delete \"%1\" from the working tree?\n\nA tracked file "
                           "becomes a deletion you can still discard; an untracked "
                           "one is gone for good.")
                .arg(path),
            QMessageBox::Yes | QMessageBox::Cancel) != QMessageBox::Yes)
        return false;
    const QString abs = QDir(dir).absoluteFilePath(path);
    if (!QFile::remove(abs)) {
        QMessageBox::warning(this, "Delete file",
                             QStringLiteral("Could not delete %1.").arg(abs));
        return false;
    }
    for (const QString &key : {path, abs, worktreeTabKey(abs)})
        closeRepoFileTabsUnder(key, false);
    logSystem(QStringLiteral("Deleted %1.").arg(abs));
    refreshSourceControl(true);
    return true;
}

void MainWindow::scmStageAll()
{
    const QString dir = sourceControlGitDir();
    queueScmMutation(
        QStringLiteral("stage-all|") + dir,
        [dir] { return scmMutate(dir, {"add", "-A"}); },
        [this](const ScmMutationResult &result) {
            if (!result.ok)
                QMessageBox::warning(this, "Stage all",
                                     result.error.isEmpty() ? "git add failed."
                                                            : result.error);
            refreshSourceControl();
        });
}

void MainWindow::scmUnstageAll()
{
    const QString dir = sourceControlGitDir();
    queueScmMutation(
        QStringLiteral("unstage-all|") + dir,
        [dir] {
            ScmMutationResult result;
            result.ok = runGitCapture(dir, {"reset", "-q"}, nullptr, nullptr);
            return result;
        },
        [this](const ScmMutationResult &) { refreshSourceControl(); });
}

void MainWindow::scmDiscardAll()
{
    if (QMessageBox::warning(
            this, "Discard all changes",
            "Discard ALL uncommitted changes, including untracked files? "
            "This cannot be undone.",
            QMessageBox::Discard | QMessageBox::Cancel) != QMessageBox::Discard)
        return;
    const QString dir = sourceControlGitDir();
    queueScmMutation(
        QStringLiteral("discard-all|") + dir,
        [dir] {
            ScmMutationResult result;
            result.ok = runGitCapture(dir, {"restore", "--", "."}, nullptr, nullptr);
            result.ok = runGitCapture(dir, {"clean", "-fd"}, nullptr, nullptr) &&
                        result.ok;
            return result;
        },
        [this](const ScmMutationResult &) { refreshSourceControl(); });
}

void MainWindow::performScmCommit(std::function<void(bool)> done)
{
    const QString msg =
        m_scmMessage ? m_scmMessage->toPlainText().trimmed() : QString();
    commitWorkingTree(msg, [this, msg, done = std::move(done)](bool ok) {
        if (ok && m_scmMessage && m_scmMessage->toPlainText().trimmed() == msg)
            m_scmMessage->clear();
        if (done)
            done(ok);
    });
}

void MainWindow::commitWorkingTree(const QString &message,
                                   std::function<void(bool)> done)
{
    const auto finish = [done](bool ok) {
        if (done)
            done(ok);
    };
    const QString dir = sourceControlGitDir();
    if (dir.isEmpty() || !repoHasWorkingTree()) {
        finish(false);
        return;
    }
    const QString msg = message.trimmed();
    if (msg.isEmpty()) {
        QMessageBox::information(this, "Commit", "Enter a commit message first.");
        finish(false);
        return;
    }
    if (m_scmCommitInFlight) {
        flashMessage(QStringLiteral("A commit is already running."));
        finish(false);
        return;
    }
    m_scmCommitInFlight = true;
    const int index = m_repoDetailIndex;
    const QString localPath = index >= 0 && index < m_repositories.size()
                                  ? m_repositories.at(index).localPath
                                  : QString();
    const auto commit = [this, dir, msg, index, localPath,
                         finish](bool stageAll) {
        runOffThread<ScmMutationResult>(
            [dir, msg, stageAll] {
                ScmMutationResult result;
                if (stageAll) {
                    result = scmMutate(dir, {"add", "-A"});
                    if (!result.ok) {
                        if (result.error.isEmpty())
                            result.error = QStringLiteral("git add failed.");
                        return result;
                    }
                }
                QString ratchetReason;
                if (!RepoStatsStore::stagedCommitAllowed(dir, &ratchetReason)) {
                    result.ok = false;
                    result.flag = true;
                    result.error = ratchetReason;
                    return result;
                }
                result = scmMutate(dir, {"commit", "-m", msg});
                if (!result.ok)
                    result.error = result.error.isEmpty()
                                       ? QStringLiteral("git commit failed.")
                                       : result.error.left(300);
                return result;
            },
            [this, dir, msg, index, localPath, finish](ScmMutationResult result) {
                m_scmCommitInFlight = false;
                if (!result.ok) {
                    QMessageBox::warning(this,
                                         result.flag ? QStringLiteral("Ratchet Mode")
                                                     : QStringLiteral("Commit"),
                                         result.error);
                    finish(false);
                    return;
                }
                logSystem(QStringLiteral("Committed: %1").arg(msg.section('\n', 0, 0)));
                const int repoIndex = scmRepoIndexFor(index, localPath);
                if (sourceControlGitDir() == dir && repoIndex == m_repoDetailIndex)
                    loadCommits(); // refresh history + the working-changes panel
                if (repoIndex >= 0)
                    propagateRepoUpdate(repoIndex);
                finish(true);
            });
    };
    runOffThread<bool>(
        [dir] {
            QByteArray staged;
            runGitCapture(dir, {"diff", "--cached", "--name-only"}, &staged, nullptr);
            return staged.trimmed().isEmpty();
        },
        [this, commit, finish](bool nothingStaged) {
            if (!nothingStaged) {
                commit(false);
                return;
            }
            if (QMessageBox::question(
                    this, "Commit",
                    "Nothing is staged. Stage all changes and commit?") !=
                QMessageBox::Yes) {
                m_scmCommitInFlight = false;
                finish(false);
                return;
            }
            commit(true);
        });
}

void MainWindow::scmCommit()
{
    performScmCommit();
}

void MainWindow::scmCommitAndPush()
{
    performScmCommit([this](bool ok) {
        if (ok)
            pushCurrentRepoUpstream();
    });
}

void MainWindow::scmStageAllCommitAndPush()
{
    const QString dir = sourceControlGitDir();
    if (dir.isEmpty() || !repoHasWorkingTree())
        return;
    if (!m_scmMessage || m_scmMessage->toPlainText().trimmed().isEmpty()) {
        QMessageBox::information(this, "Commit", "Enter a commit message first.");
        return;
    }
    queueScmMutation(
        QStringLiteral("stage-all-commit-push|") + dir,
        [dir] { return scmMutate(dir, {"add", "-A"}); },
        [this, dir](const ScmMutationResult &result) {
            if (!result.ok) {
                QMessageBox::warning(this, "Stage all",
                                     result.error.isEmpty() ? "git add failed."
                                                            : result.error);
                return;
            }
            refreshSourceControl();
            if (sourceControlGitDir() != dir)
                return;
            performScmCommit([this](bool ok) {
                if (ok)
                    pushCurrentRepoUpstream();
            });
        });
}

namespace {

// These readers are deliberately free functions: every input is captured on
// the GUI thread and passed by value to runOffThread(), so the worker cannot
// accidentally reach a widget or mutable MainWindow state.
QString scmContextDiffFromGit(const QString &dir, int duration)
{
    if (dir.isEmpty())
        return QString();

    QByteArray diff;
    if (duration == 1 || duration == 2) {
        const QString cutoff =
            duration == 1 ? QStringLiteral("1 hour ago")
                          : QStringLiteral("midnight");
        QByteArray revOut;
        runGitCapture(dir, {"rev-list", "-1", "--before=" + cutoff, "HEAD"}, &revOut,
                      nullptr);
        const QString rev = QString::fromUtf8(revOut).trimmed();
        if (!rev.isEmpty())
            diff = gitCaptureStdout(dir, {"diff", rev});
    }
    if (diff.trimmed().isEmpty()) {
        QByteArray staged = gitCaptureStdout(dir, {"diff", "--cached"});
        diff = staged.trimmed().isEmpty() ? gitCaptureStdout(dir, {"diff"}) : staged;
    }
    QString text = QString::fromUtf8(diff);
    QByteArray untracked;
    runGitCapture(dir, {"ls-files", "--others", "--exclude-standard"}, &untracked,
                  nullptr);
    const QString others = QString::fromUtf8(untracked).trimmed();
    if (!others.isEmpty())
        text += QStringLiteral("\n\n# New (untracked) files:\n") + others;
    constexpr int kCap = 12000;
    if (text.size() > kCap)
        text = text.left(kCap) + QStringLiteral("\n... [diff truncated]");
    return text;
}

QStringList scmDiffScopeArgsFromGit(const QString &dir, int duration)
{
    if (!dir.isEmpty() && (duration == 1 || duration == 2)) {
        const QString cutoff =
            duration == 1 ? QStringLiteral("1 hour ago")
                          : QStringLiteral("midnight");
        QByteArray revOut;
        runGitCapture(dir, {"rev-list", "-1", "--before=" + cutoff, "HEAD"}, &revOut,
                      nullptr);
        const QString rev = QString::fromUtf8(revOut).trimmed();
        if (!rev.isEmpty())
            return {rev};
    }
    QByteArray staged;
    if (!dir.isEmpty())
        runGitCapture(dir, {"diff", "--cached", "--name-only"}, &staged, nullptr);
    if (!staged.trimmed().isEmpty())
        return {QStringLiteral("--cached")};
    return {};
}

QString scmHeuristicCommitMessageFromGit(const QString &dir, int duration,
                                         int variant)
{
    if (dir.isEmpty())
        return QString();
    const QStringList scope = scmDiffScopeArgsFromGit(dir, duration);

    struct Change {
        QChar status;
        QString path;    // new path (for renames)
        QString oldPath; // only set for renames
    };
    QList<Change> changes;
    const QString ns = QString::fromUtf8(gitCaptureStdout(
        dir, QStringList{"diff"} + scope + QStringList{"--name-status"}));
    for (const QString &row : ns.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const QStringList f = row.split(QLatin1Char('\t'), Qt::SkipEmptyParts);
        if (f.isEmpty())
            continue;
        const QChar st = f.first().isEmpty() ? QChar('M') : f.first().at(0);
        Change c{st, f.last(), QString()};
        if ((st == QLatin1Char('R') || st == QLatin1Char('C')) && f.size() >= 3)
            c.oldPath = f.at(1);
        changes.append(c);
    }
    if (scope.isEmpty()) {
        const QString u = QString::fromUtf8(gitCaptureStdout(
            dir, {"ls-files", "--others", "--exclude-standard"}));
        for (const QString &p : u.split(QLatin1Char('\n'), Qt::SkipEmptyParts))
            changes.append({QChar('A'), p.trimmed(), QString()});
    }
    if (changes.isEmpty())
        return QString();

    auto category = [](const QString &path) -> QString {
        const QString p = path.toLower();
        const QString base = p.section(QLatin1Char('/'), -1);
        if (p.contains(QLatin1String(".github/workflows/")))
            return QStringLiteral("ci");
        if (base.endsWith(QLatin1String(".md")) || base.endsWith(QLatin1String(".rst"))
            || p.contains(QLatin1String("/docs/")) || p.startsWith(QLatin1String("docs/"))
            || base.startsWith(QLatin1String("readme"))
            || base.startsWith(QLatin1String("changelog")))
            return QStringLiteral("docs");
        if (p.contains(QLatin1String("/tests/")) || p.contains(QLatin1String("/test/"))
            || base.startsWith(QLatin1String("test_"))
            || base.contains(QLatin1String("_test."))
            || base.contains(QLatin1String(".test."))
            || base.contains(QLatin1String("spec.")))
            return QStringLiteral("test");
        if (base == QLatin1String("cmakelists.txt") || base.endsWith(QLatin1String(".cmake"))
            || base == QLatin1String("makefile") || base == QLatin1String("dockerfile")
            || base == QLatin1String("package.json") || base.endsWith(QLatin1String(".lock"))
            || base.endsWith(QLatin1String(".yml")) || base.endsWith(QLatin1String(".yaml"))
            || base.endsWith(QLatin1String(".toml")) || base == QLatin1String(".gitignore"))
            return QStringLiteral("build");
        return QStringLiteral("code");
    };

    int added = 0, removed = 0, renamed = 0;
    QString commonCat;
    bool uniformCat = true;
    QStringList commonDir;
    bool firstDir = true;
    for (const Change &c : changes) {
        if (c.status == QLatin1Char('A')) ++added;
        else if (c.status == QLatin1Char('D')) ++removed;
        else if (c.status == QLatin1Char('R')) ++renamed;

        const QString cat = category(c.path);
        if (commonCat.isEmpty()) commonCat = cat;
        else if (commonCat != cat) uniformCat = false;

        const int slash = c.path.lastIndexOf(QLatin1Char('/'));
        const QStringList parts =
            (slash >= 0 ? c.path.left(slash) : QString()).split(QLatin1Char('/'),
                                                                Qt::SkipEmptyParts);
        if (firstDir) { commonDir = parts; firstDir = false; }
        else {
            int k = 0;
            while (k < commonDir.size() && k < parts.size() && commonDir[k] == parts[k])
                ++k;
            commonDir = commonDir.mid(0, k);
        }
    }

    QHash<QString, int> addedType, addedFn, removedType, removedFn, touchedFn;
    QString addedText; // added code lines, for the bug-fix keyword scan
    {
        const QString patch = QString::fromUtf8(gitCaptureStdout(
            dir, QStringList{"diff"} + scope + QStringList{"--unified=0"}));
        auto funcFromContext = [](const QString &ctx) -> QString {
            const int paren = ctx.indexOf(QLatin1Char('('));
            if (paren < 0)
                return QString();
            static const QRegularExpression tail(
                QStringLiteral("([A-Za-z_][A-Za-z0-9_]*)\\s*$"));
            const auto m = tail.match(ctx.left(paren));
            if (!m.hasMatch())
                return QString();
            static const QSet<QString> kw = {
                QStringLiteral("if"),    QStringLiteral("for"),
                QStringLiteral("while"), QStringLiteral("switch"),
                QStringLiteral("catch"), QStringLiteral("return"),
                QStringLiteral("sizeof")};
            return kw.contains(m.captured(1)) ? QString() : m.captured(1);
        };
        static const QRegularExpression typeDef(QStringLiteral(
            "^(?:typedef\\s+)?(?:class|struct|enum)\\s+(?:class\\s+)?([A-Za-z_]\\w*)"));
        static const QRegularExpression langFn(QStringLiteral(
            "^(?:export\\s+|public\\s+|private\\s+|pub\\s+|async\\s+|static\\s+)*"
            "(?:def|func|fn|function|sub)\\s+([A-Za-z_]\\w*)"));
        static const QRegularExpression headerDecl(QStringLiteral(
            "^[A-Za-z_][\\w:<>,*&\\s]*\\s[*&]*([A-Za-z_]\\w*)\\s*\\([^;{]*\\)\\s*"
            "(?:const|override|noexcept|final|=\\s*\\w+|\\s)*;\\s*$"));
        auto scanDefs = [&](const QString &content, bool header,
                            QHash<QString, int> &types, QHash<QString, int> &fns) {
            QString t = content.trimmed();
            if (t.isEmpty() || t.startsWith(QLatin1String("//"))
                || t.startsWith(QLatin1Char('*')) || t.startsWith(QLatin1String("/*"))
                || t.startsWith(QLatin1Char('#')))
                return;
            const int cmt = t.indexOf(QLatin1String("//"));
            if (cmt > 0)
                t = t.left(cmt).trimmed();
            const auto mt = typeDef.match(t);
            if (mt.hasMatch()) { ++types[mt.captured(1)]; return; }
            const auto ml = langFn.match(t);
            if (ml.hasMatch()) { ++fns[ml.captured(1)]; return; }
            if (header) {
                const auto mh = headerDecl.match(t);
                if (mh.hasMatch())
                    ++fns[mh.captured(1)];
            }
        };

        QString file;
        bool header = false;
        int seen = 0;
        const QStringList lines = patch.split(QLatin1Char('\n'));
        for (const QString &line : lines) {
            if (++seen > 40000) // bound the work on a very large diff
                break;
            if (line.startsWith(QLatin1String("+++ "))) {
                file = line.mid(4).trimmed();
                if (file.startsWith(QLatin1String("b/")))
                    file = file.mid(2);
                const QString lower = file.toLower();
                header = lower.endsWith(QLatin1String(".h"))
                         || lower.endsWith(QLatin1String(".hpp"))
                         || lower.endsWith(QLatin1String(".hh"))
                         || lower.endsWith(QLatin1String(".hxx"));
                continue;
            }
            if (line.startsWith(QLatin1String("--- ")))
                continue;
            if (line.startsWith(QLatin1String("@@"))) {
                const int second = line.indexOf(QLatin1String("@@"), 2);
                if (second >= 0) {
                    const QString fn = funcFromContext(line.mid(second + 2));
                    if (!fn.isEmpty())
                        ++touchedFn[fn];
                }
                continue;
            }
            if (line.startsWith(QLatin1Char('+'))) {
                const QString content = line.mid(1);
                scanDefs(content, header, addedType, addedFn);
                addedText += content.toLower();
                addedText += QLatin1Char('\n');
            } else if (line.startsWith(QLatin1Char('-'))) {
                scanDefs(line.mid(1), header, removedType, removedFn);
            }
        }
    }

    auto humanize = [](const QString &name) -> QString {
        QString out;
        for (int i = 0; i < name.size(); ++i) {
            const QChar c = name.at(i);
            if (c == QLatin1Char('_') || c == QLatin1Char('-')) {
                if (!out.isEmpty() && !out.endsWith(QLatin1Char(' ')))
                    out += QLatin1Char(' ');
                continue;
            }
            const bool prevLower = i > 0 && name.at(i - 1).isLower();
            const bool prevUpper = i > 0 && name.at(i - 1).isUpper();
            const bool nextLower = i + 1 < name.size() && name.at(i + 1).isLower();
            const bool boundary = (c.isUpper() && prevLower)              // fooBar
                                  || (c.isUpper() && prevUpper && nextLower) // HTMLParser
                                  || (c.isDigit() && i > 0 && !name.at(i - 1).isDigit());
            if (boundary && !out.isEmpty() && !out.endsWith(QLatin1Char(' ')))
                out += QLatin1Char(' ');
            out += c.toLower();
        }
        return out.simplified();
    };

    static const QSet<QString> kGeneric = {
        QStringLiteral("change"),  QStringLiteral("data"),    QStringLiteral("item"),
        QStringLiteral("info"),    QStringLiteral("helper"),  QStringLiteral("impl"),
        QStringLiteral("result"),  QStringLiteral("options"), QStringLiteral("option"),
        QStringLiteral("config"),  QStringLiteral("context"), QStringLiteral("entry"),
        QStringLiteral("node"),    QStringLiteral("pair"),    QStringLiteral("tmp"),
        QStringLiteral("temp"),    QStringLiteral("foo"),     QStringLiteral("bar"),
        QStringLiteral("base"),    QStringLiteral("value"),   QStringLiteral("object"),
        QStringLiteral("list"),    QStringLiteral("map"),     QStringLiteral("util"),
        QStringLiteral("utils"),   QStringLiteral("main"),    QStringLiteral("init"),
        QStringLiteral("state"),   QStringLiteral("params"),  QStringLiteral("args"),
        QStringLiteral("type"),    QStringLiteral("types"),   QStringLiteral("handler")};

    struct Bucket {
        const QHash<QString, int> *freq;
        const QHash<QString, int> *exclude;
        int bonus;
    };
    auto rankMerged = [&](std::initializer_list<Bucket> buckets) -> QStringList {
        struct Cand { QString name; int score; };
        QList<Cand> cs;
        for (const Bucket &b : buckets)
            for (auto i = b.freq->cbegin(); i != b.freq->cend(); ++i) {
                const QString &n = i.key();
                if (b.exclude->contains(n) || kGeneric.contains(n.toLower())
                    || n.size() < 4)
                    continue;
                const int words = humanize(n).count(QLatin1Char(' ')) + 1;
                cs.append({n, b.bonus + qMin(words, 4) + qMin(i.value(), 3)});
            }
        std::sort(cs.begin(), cs.end(), [](const Cand &a, const Cand &b) {
            if (a.score != b.score) return a.score > b.score;
            return a.name < b.name;
        });
        QStringList out;
        for (const Cand &c : cs) out << c.name;
        return out;
    };
    auto fileStems = [&](QChar status) -> QStringList {
        QStringList out;
        for (const Change &c : changes)
            if (c.status == status) {
                QString b = c.path.section(QLatin1Char('/'), -1);
                const int dot = b.lastIndexOf(QLatin1Char('.'));
                if (dot > 0) b = b.left(dot);
                if (!kGeneric.contains(b.toLower()))
                    out << b;
            }
        return out;
    };

    const QHash<QString, int> none;
    QStringList newNames =
        rankMerged({{&addedType, &removedType, 2}, {&addedFn, &removedFn, 1}});
    QStringList goneNames =
        rankMerged({{&removedType, &addedType, 2}, {&removedFn, &addedFn, 1}});
    if (newNames.isEmpty() && added > 0)
        newNames = fileStems(QChar('A'));
    if (goneNames.isEmpty() && removed > 0)
        goneNames = fileStems(QChar('D'));
    QHash<QString, int> changedFreq = touchedFn;
    for (const QString &n : addedFn.keys()) changedFreq.remove(n);
    for (const QString &n : addedType.keys()) changedFreq.remove(n);
    for (const QString &n : removedFn.keys()) changedFreq.remove(n);
    for (const QString &n : removedType.keys()) changedFreq.remove(n);
    const QStringList chgNames = rankMerged({{&changedFreq, &none, 0}});

    static const QRegularExpression fixWords(QStringLiteral(
        "\\b(fix|bug|crash|freeze|hang|leak|deadlock|segfault|overflow|regression|"
        "guard|workaround|race)\\b"));
    const bool fixy = fixWords.match(addedText).hasMatch();

    QString type;
    if (uniformCat && commonCat != QLatin1String("code"))
        type = commonCat;
    if (type.isEmpty()) {
        const QString branch = QString::fromUtf8(gitCaptureStdout(
            dir, {"rev-parse", "--abbrev-ref", "HEAD"})).trimmed().toLower();
        if (branch.contains(QLatin1String("fix")) || branch.contains(QLatin1String("bug"))
            || branch.contains(QLatin1String("hotfix")))
            type = QStringLiteral("fix");
        else if (branch.contains(QLatin1String("feat")))
            type = QStringLiteral("feat");
        else if (branch.contains(QLatin1String("refactor")))
            type = QStringLiteral("refactor");
    }
    if (type.isEmpty()) {
        if (!newNames.isEmpty() && goneNames.isEmpty() && removed == 0 && renamed == 0)
            type = QStringLiteral("feat");
        else if (fixy)
            type = QStringLiteral("fix");
        else if (!chgNames.isEmpty() || !goneNames.isEmpty() || !newNames.isEmpty()
                 || removed > 0 || renamed > 0)
            type = QStringLiteral("refactor");
        else
            type = QStringLiteral("chore");
    }

    auto rotate = [&](QStringList names) -> QStringList {
        if (names.size() > 4)
            names = names.mid(0, 4);
        if (variant > 0 && names.size() > 1) {
            const int off = variant % names.size();
            names = names.mid(off) + names.mid(0, off);
        }
        return names;
    };
    auto headline = [&](const QStringList &all) -> QStringList {
        const QStringList names = rotate(all);
        QStringList out;
        for (const QString &n : names) {
            if (out.isEmpty()) { out << n; continue; }
            if (humanize(n).section(QLatin1Char(' '), 0, 0)
                != humanize(out.first()).section(QLatin1Char(' '), 0, 0)) {
                out << n;
                break;
            }
        }
        return out;
    };

    QString subject;
    QString verb;     // "" means let the conventional type carry the verb
    QStringList pick; // the chosen symbol name(s)
    const bool renameOne =
        changes.size() == 1 && renamed == 1 && !changes.first().oldPath.isEmpty();
    if (renameOne) {
        subject = QStringLiteral("rename %1 to %2")
                      .arg(changes.first().oldPath.section(QLatin1Char('/'), -1),
                           changes.first().path.section(QLatin1Char('/'), -1));
    } else if (!newNames.isEmpty()) {
        verb = QStringLiteral("add");
        pick = headline(newNames);
    } else if (!goneNames.isEmpty()) {
        verb = QStringLiteral("remove");
        pick = headline(goneNames);
    } else if (!chgNames.isEmpty()) {
        pick = headline(chgNames);
    }
    if (variant > 0 && !renameOne) {
        if (verb == QLatin1String("add")) {
            static const QStringList syn = {QStringLiteral("add"),
                QStringLiteral("introduce"), QStringLiteral("implement"),
                QStringLiteral("create"), QStringLiteral("wire up")};
            verb = syn.at(variant % syn.size());
        } else if (verb == QLatin1String("remove")) {
            static const QStringList syn = {QStringLiteral("remove"),
                QStringLiteral("drop"), QStringLiteral("delete"),
                QStringLiteral("strip out")};
            verb = syn.at(variant % syn.size());
        } else if (verb.isEmpty() && !pick.isEmpty()) {
            static const QStringList syn = {QString(), QStringLiteral("tweak"),
                QStringLiteral("rework"), QStringLiteral("refine"),
                QStringLiteral("revise"), QStringLiteral("adjust")};
            verb = syn.at(variant % syn.size());
        }
    }
    auto compose = [&](int maxNames) -> QString {
        QStringList words;
        for (const QString &n : pick.mid(0, maxNames))
            words << humanize(n);
        QString ph;
        if (words.size() >= 2) {
            const QString sep = (words[0].contains(QStringLiteral(" and "))
                                 || words[1].contains(QStringLiteral(" and ")))
                                    ? QStringLiteral(", ")
                                    : QStringLiteral(" and ");
            ph = words[0] + sep + words[1];
        } else {
            ph = words.value(0);
        }
        if (ph.isEmpty())
            return QString();
        return verb.isEmpty() ? ph : verb + QLatin1Char(' ') + ph;
    };
    if (subject.isEmpty() && !pick.isEmpty())
        subject = compose(2);
    if (subject.isEmpty()) {
        const QString fv = added == changes.size()    ? QStringLiteral("add")
                           : removed == changes.size() ? QStringLiteral("remove")
                           : renamed == changes.size() ? QStringLiteral("rename")
                                                       : QStringLiteral("update");
        subject = changes.size() == 1
                      ? QStringLiteral("%1 %2").arg(
                            fv, changes.first().path.section(QLatin1Char('/'), -1))
                      : QStringLiteral("%1 %2 files").arg(fv).arg(changes.size());
    }

    static const QSet<QString> kGenericScope = {
        QStringLiteral("src"),     QStringLiteral("lib"),  QStringLiteral("source"),
        QStringLiteral("sources"), QStringLiteral("app"),  QStringLiteral("code"),
        QStringLiteral("include"), QStringLiteral("dist")};
    QString scopeName = commonDir.isEmpty() ? QString() : commonDir.last();
    if (kGenericScope.contains(scopeName.toLower()))
        scopeName.clear();

    auto assemble = [&](const QString &subj) {
        QString m = type;
        if (!scopeName.isEmpty() && scopeName.size() <= 20)
            m += QStringLiteral("(%1)").arg(scopeName);
        return m + QStringLiteral(": ") + subj;
    };
    QString msg = assemble(subject);
    if (msg.size() > 72 && pick.size() >= 2)
        msg = assemble(compose(1));
    return msg;
}

} // namespace

void MainWindow::autoFillScmMessage()
{
    if (m_scmGenerating || !m_scmMessage || !m_scmGenModel
        || m_scmGenModel->currentData().toInt() != -1
        || !m_scmMessage->toPlainText().trimmed().isEmpty())
        return;
    const QString dir = sourceControlGitDir();
    if (dir.isEmpty())
        return;
    if (m_scmAutoFillInFlight) {
        m_scmAutoFillPending = true;
        return;
    }

    const int duration =
        m_scmGenDuration ? m_scmGenDuration->currentIndex() : 0;
    const quint64 patchGeneration = m_scmPatchGeneration;
    const quint64 requestGeneration = ++m_scmMessageReadGeneration;
    m_scmAutoFillInFlight = true;
    m_scmAutoFillPending = false;
    runOffThread<QString>(
        [dir, duration] {
            return scmHeuristicCommitMessageFromGit(dir, duration, 0);
        },
        [this, dir, duration, patchGeneration,
         requestGeneration](QString msg) {
            m_scmAutoFillInFlight = false;
            const bool rerun = std::exchange(m_scmAutoFillPending, false);
            const bool current =
                requestGeneration == m_scmMessageReadGeneration &&
                patchGeneration == m_scmPatchGeneration &&
                sourceControlGitDir() == dir && m_scmMessage &&
                m_scmGenModel &&
                m_scmGenModel->currentData().toInt() == -1 &&
                (!m_scmGenDuration ||
                 m_scmGenDuration->currentIndex() == duration) &&
                m_scmMessage->toPlainText().trimmed().isEmpty();
            if (current && !msg.isEmpty()) {
                // A fresh auto-fill restarts the click-to-vary cycle.
                m_scmHeuristicVariant = 0;
                m_scmMessage->setPlainText(msg);
                if (m_scmGenStatus)
                    m_scmGenStatus->setText(QString::fromUtf8(
                        "%1 chars \xc2\xb7 on-device, auto \xc2\xb7 "
                        "\xE2\x86\xBB click to vary")
                                                .arg(msg.size()));
            }
            if (rerun && m_scmMessage &&
                m_scmMessage->toPlainText().trimmed().isEmpty()) {
                QTimer::singleShot(
                    0, this, [this] { autoFillScmMessage(); });
            }
        });
}

void MainWindow::finishScmGeneration()
{
    m_scmGenerating = false;
    // Match the pre-worker request lifecycle: once a started request settles,
    // both Generate entry points become available for another attempt.
    setScmGenerateEnabled(true);
    if (m_scmGenerateButton)
        m_scmGenerateButton->setText(
            QString::fromUtf8("\xE2\x9C\xA8 Generate"));
}

void MainWindow::startScmAiRequest(const QString &diff, int modelIndex,
                                   bool isCommit,
                                   quint64 requestGeneration,
                                   quint64 patchGeneration,
                                   const QString &dir, int duration)
{
    if (modelIndex < 0 || modelIndex >= kScmAiModelCount) {
        finishScmGeneration();
        return;
    }
    const ScmAiModel model = kScmAiModels[modelIndex];
    const int outTok = isCommit ? 120 : 280;

    const QString system =
        isCommit ? QStringLiteral("You write concise, conventional git commit "
                                  "messages.")
                 : QStringLiteral("You write short, engaging X (Twitter) posts "
                                  "for developers.");
    const QString task =
        isCommit
            ? QStringLiteral(
                  "Write a single-line git commit message for the following "
                  "changes: a short imperative subject (max ~70 chars), no body. "
                  "Output ONLY that one line, with no backticks or commentary."
                  "\n\nDiff:\n%1")
                  .arg(diff)
            : QStringLiteral(
                  "Write a single engaging X (Twitter) post, at most 280 "
                  "characters, announcing these code changes to developer "
                  "followers. At most two relevant hashtags. Output ONLY the post "
                  "text.\n\nChanges:\n%1")
                  .arg(diff);

    const bool claude =
        QString::fromLatin1(model.provider) == QLatin1String("claude");
    const QString apiKey =
        claude ? QSettings().value(kClaudeApiKeySetting).toString().trimmed()
               : QSettings().value(kCodexApiKeySetting).toString().trimmed();
    if (apiKey.isEmpty()) {
        finishScmGeneration();
        if (m_scmGenStatus)
            m_scmGenStatus->setText(claude ? "Add a Claude API key in Settings."
                                           : "Add an OpenAI API key in Settings.");
        return;
    }

    QNetworkReply *reply = nullptr;
    if (claude) {
        QJsonObject payload;
        payload.insert("model", QString::fromLatin1(model.id));
        payload.insert("max_tokens", outTok);
        QJsonArray messages;
        QJsonObject um;
        um.insert("role", "user");
        um.insert("content", system + "\n\n" + task);
        messages.append(um);
        payload.insert("messages", messages);
        QNetworkRequest req(QUrl("https://api.anthropic.com/v1/messages"));
        req.setRawHeader("x-api-key", apiKey.toUtf8());
        req.setRawHeader("anthropic-version", "2023-06-01");
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        reply = m_networkAccess->post(
            req, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    } else {
        QJsonObject payload;
        payload.insert("model", QString::fromLatin1(model.id));
        payload.insert("instructions", system);
        payload.insert("input", task);
        payload.insert("max_output_tokens", outTok);
        QNetworkRequest req =
            openAiRequest(QUrl("https://api.openai.com/v1/responses"), apiKey);
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        reply = m_networkAccess->post(
            req, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    }

    if (m_scmGenStatus)
        m_scmGenStatus->setText(QString::fromUtf8("Asking %1\xE2\x80\xA6")
                                    .arg(QString::fromLatin1(model.label)));

    connect(reply, &QNetworkReply::finished, this,
            [this, reply, claude, model, modelIndex, isCommit,
             requestGeneration, patchGeneration, dir, duration] {
                const QByteArray body = reply->readAll();
                const QNetworkReply::NetworkError error = reply->error();
                const QString errorSummary =
                    error == QNetworkReply::NoError
                        ? QString()
                        : apiErrorSummary(reply, body);
                reply->deleteLater();
                if (requestGeneration != m_scmMessageReadGeneration)
                    return;
                const bool current =
                    patchGeneration == m_scmPatchGeneration &&
                    sourceControlGitDir() == dir && m_scmGenModel &&
                    m_scmGenModel->currentData().toInt() == modelIndex &&
                    (!m_scmGenDuration ||
                     m_scmGenDuration->currentIndex() == duration) &&
                    (!m_scmGenKind ||
                     (m_scmGenKind->currentIndex() != 1) == isCommit);
                finishScmGeneration();
                if (!current) {
                    if (m_scmGenStatus)
                        m_scmGenStatus->setText(
                            "Changes changed while generating; try again.");
                    return;
                }
                if (error != QNetworkReply::NoError) {
                    if (m_scmGenStatus)
                        m_scmGenStatus->setText("Failed: " + errorSummary);
                    return;
                }
                const QJsonObject obj = QJsonDocument::fromJson(body).object();
                QString text;
                double cost = 0.0;
                qint64 inTok = 0, outTokActual = 0;
                if (claude) {
                    for (const QJsonValue &v : obj.value("content").toArray()) {
                        const QJsonObject o = v.toObject();
                        if (o.value("type").toString() == QLatin1String("text"))
                            text += o.value("text").toString();
                    }
                    const QJsonObject usage = obj.value("usage").toObject();
                    inTok = usage.value("input_tokens").toInt();
                    outTokActual = usage.value("output_tokens").toInt();
                    cost = inTok / 1e6 * model.inPerM +
                           outTokActual / 1e6 * model.outPerM;
                } else {
                    text = openAiResponseText(obj);
                    cost = openAiAskCostUsd(obj, &inTok, &outTokActual);
                }
                text = text.trimmed();
                if (text.isEmpty()) {
                    if (m_scmGenStatus)
                        m_scmGenStatus->setText("Empty response.");
                    return;
                }
                if (m_scmMessage) {
                    if (isCommit) {
                        QString oneLine = text;
                        oneLine.replace(QLatin1Char('\n'), QLatin1Char(' '));
                        m_scmMessage->setPlainText(oneLine.simplified());
                    } else {
                        m_scmMessage->setPlainText(text);
                    }
                }
                const int n = m_scmMessage
                                  ? m_scmMessage->toPlainText().size()
                                           : text.size();
                QString line = isCommit ? QStringLiteral("%1 chars").arg(n)
                                        : QStringLiteral("%1/280").arg(n);
                line += QString::fromUtf8(" \xC2\xB7 $%1 \xC2\xB7 %2 in / %3 out")
                            .arg(QString::number(cost, 'f', 4))
                            .arg(inTok)
                            .arg(outTokActual);
                if (!isCommit) {
                    QApplication::clipboard()->setText(text);
                    line += QStringLiteral(", copied");
                }
                if (m_scmGenStatus)
                    m_scmGenStatus->setText(line);
            });
}

void MainWindow::generateScmMessage()
{
    if (m_scmGenerating)
        return;
    const QString dir = sourceControlGitDir();
    if (dir.isEmpty()) {
        if (m_scmGenStatus)
            m_scmGenStatus->setText("No changes to describe.");
        return;
    }
    const int modelIndex =
        m_scmGenModel ? m_scmGenModel->currentData().toInt() : 0;
    if (modelIndex >= kScmAiModelCount)
        return;
    const int duration =
        m_scmGenDuration ? m_scmGenDuration->currentIndex() : 0;
    const bool isCommit =
        !(m_scmGenKind && m_scmGenKind->currentIndex() == 1);
    const quint64 patchGeneration = m_scmPatchGeneration;
    const quint64 requestGeneration = ++m_scmMessageReadGeneration;
    m_scmAutoFillPending = false;
    m_scmGenerating = true;
    setScmGenerateEnabled(false);
    if (m_scmGenerateButton)
        m_scmGenerateButton->setText(
            QString::fromUtf8("Generating\xE2\x80\xA6"));
    if (m_scmGenStatus)
        m_scmGenStatus->setText(
            QString::fromUtf8("Reading changes\xE2\x80\xA6"));

    if (modelIndex < 0) {
        const int variant = ++m_scmHeuristicVariant;
        runOffThread<QString>(
            [dir, duration, variant] {
                return scmHeuristicCommitMessageFromGit(dir, duration,
                                                        variant);
            },
            [this, dir, duration, patchGeneration,
             requestGeneration](QString msg) {
                if (requestGeneration != m_scmMessageReadGeneration)
                    return;
                const bool current =
                    patchGeneration == m_scmPatchGeneration &&
                    sourceControlGitDir() == dir && m_scmGenModel &&
                    m_scmGenModel->currentData().toInt() == -1 &&
                    (!m_scmGenDuration ||
                     m_scmGenDuration->currentIndex() == duration);
                finishScmGeneration();
                if (!current) {
                    if (m_scmGenStatus)
                        m_scmGenStatus->setText(
                            "Changes changed while generating; try again.");
                    return;
                }
                if (msg.isEmpty()) {
                    m_scmHeuristicVariant = 0;
                    if (m_scmGenStatus)
                        m_scmGenStatus->setText("No changes to describe.");
                    return;
                }
                if (m_scmMessage)
                    m_scmMessage->setPlainText(msg);
                const int n = m_scmMessage
                                  ? m_scmMessage->toPlainText().size()
                                  : msg.size();
                if (m_scmGenStatus)
                    m_scmGenStatus->setText(QString::fromUtf8(
                        "%1 chars \xc2\xb7 on-device, no cost \xc2\xb7 "
                        "\xE2\x86\xBB click to vary")
                                                .arg(n));
            });
        return;
    }

    runOffThread<QString>(
        [dir, duration] { return scmContextDiffFromGit(dir, duration); },
        [this, dir, duration, modelIndex, isCommit, patchGeneration,
         requestGeneration](QString diff) {
            if (requestGeneration != m_scmMessageReadGeneration)
                return;
            const bool current =
                patchGeneration == m_scmPatchGeneration &&
                sourceControlGitDir() == dir && m_scmGenModel &&
                m_scmGenModel->currentData().toInt() == modelIndex &&
                (!m_scmGenDuration ||
                 m_scmGenDuration->currentIndex() == duration) &&
                (!m_scmGenKind ||
                 (m_scmGenKind->currentIndex() != 1) == isCommit);
            if (!current) {
                finishScmGeneration();
                if (m_scmGenStatus)
                    m_scmGenStatus->setText(
                        "Changes changed while reading; try again.");
                return;
            }
            if (diff.trimmed().isEmpty()) {
                finishScmGeneration();
                if (m_scmGenStatus)
                    m_scmGenStatus->setText("No changes to describe.");
                return;
            }
            startScmAiRequest(diff, modelIndex, isCommit,
                              requestGeneration, patchGeneration, dir,
                              duration);
        });
}

QWidget *MainWindow::buildRepoSecurityTab()
{
    auto *page = new QWidget;
    page->setObjectName("mainContent");

    auto *scroll = new QScrollArea;
    scroll->setObjectName("mainContent");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto *content = new QWidget;
    content->setObjectName("insightsPage");
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(18, 18, 18, 18);
    layout->setSpacing(14);

    auto *heading = new QLabel("Security");
    heading->setObjectName("securityQualityTitle");
    heading->setProperty("class", "channelTitle");
    heading->setStyleSheet(Theme::remapSheet(QStringLiteral("font-size:16px;font-weight:700;")));
    auto *subtitle = new QLabel(
        "Local security evidence from this node. External advisory matching is "
        "not part of the MVP; quality metrics live on the Quality tab.");
    subtitle->setObjectName("statusLine");
    subtitle->setWordWrap(true);

    m_securityRefreshButton = new QPushButton("Refresh");
    m_securityRefreshButton->setObjectName("ghostButton");
    m_securityRefreshButton->setProperty("buttonSize", "sm");
    m_securityRefreshButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_securityRefreshButton, "sync", 16);
    connect(m_securityRefreshButton, &QPushButton::clicked, this,
            &MainWindow::refreshRepoSecurity);

    auto *headingCol = new QVBoxLayout;
    headingCol->setContentsMargins(0, 0, 0, 0);
    headingCol->setSpacing(3);
    headingCol->addWidget(heading);
    headingCol->addWidget(subtitle);

    auto *headerRow = new QHBoxLayout;
    headerRow->setContentsMargins(0, 0, 0, 0);
    headerRow->setSpacing(8);
    headerRow->addLayout(headingCol, 1);
    headerRow->addWidget(m_securityRefreshButton, 0, Qt::AlignTop);
    layout->addLayout(headerRow);

    m_securitySummary = new QLabel;
    m_securitySummary->setObjectName("insightsCard");
    m_securitySummary->setTextFormat(Qt::RichText);
    m_securitySummary->setWordWrap(true);
    m_securitySummary->setMinimumHeight(92);
    layout->addWidget(m_securitySummary);

    auto *signalsLabel = new QLabel("SIGNALS");
    signalsLabel->setObjectName("sectionLabel");
    layout->addWidget(signalsLabel);

    m_securitySignalsPanel = new QWidget;
    m_securitySignalsGrid = new QGridLayout(m_securitySignalsPanel);
    m_securitySignalsGrid->setContentsMargins(0, 0, 0, 0);
    m_securitySignalsGrid->setSpacing(10);
    layout->addWidget(m_securitySignalsPanel);

    auto *findingsLabel = new QLabel("FINDINGS");
    findingsLabel->setObjectName("sectionLabel");
    layout->addWidget(findingsLabel);

    m_securityFindingsTable = new QTableWidget(0, 4);
    installColumnHeaderMenu(m_securityFindingsTable);
    m_securityFindingsTable->setObjectName("issueTable");
    enableHoverRowHighlight(m_securityFindingsTable);
    m_securityFindingsTable->setHorizontalHeaderLabels(
        {"Severity", "Category", "Finding", "Action"});
    m_securityFindingsTable->verticalHeader()->setVisible(false);
    m_securityFindingsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_securityFindingsTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_securityFindingsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_securityFindingsTable->setShowGrid(false);
    m_securityFindingsTable->setWordWrap(false);
    QHeaderView *header = m_securityFindingsTable->horizontalHeader();
    header->setHighlightSections(false);
    header->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(2, QHeaderView::Stretch);
    header->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    makeColumnsResizable(m_securityFindingsTable);
    m_securityFindingsTable->setMinimumHeight(220);
    layout->addWidget(m_securityFindingsTable);
    layout->addStretch();
    connect(m_securityFindingsTable, &QTableWidget::cellClicked,
            this, [this](int row, int /*col*/) {
        auto *item = m_securityFindingsTable->item(row, 2);
        if (!item)
            return;
        const QString path = item->data(Qt::UserRole).toString();
        if (path.isEmpty())
            return;
        openRepoFileAtLine(path, item->data(Qt::UserRole + 1).toInt());
    });

    scroll->setWidget(content);
    auto *pageLayout = new QVBoxLayout(page);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    pageLayout->addWidget(scroll);
    refreshRepoSecurity();
    return page;
}

// Severity -> badge/text colour for the Security and Quality tabs. Mirrors the
// palette used elsewhere (green pass, blue info, amber warning, orange/red severe).
static QString repoSecuritySeverityColor(RepoSecuritySeverity severity)
{
    switch (severity) {
    case RepoSecuritySeverity::Pass:
        return QStringLiteral("#3fb950");
    case RepoSecuritySeverity::Info:
        return QStringLiteral("#58a6ff");
    case RepoSecuritySeverity::Warning:
        return QStringLiteral("#d29922");
    case RepoSecuritySeverity::High:
        return QStringLiteral("#f0883e");
    case RepoSecuritySeverity::Critical:
        return QStringLiteral("#f85149");
    }
    return QStringLiteral("#8b949e");
}

// Render one security signal as an "insightsCard" HTML body: severity-tinted
// title, the summary, and (when present) the longer detail.
static QString repoSecuritySignalHtml(const RepoSecuritySignal &signal)
{
    const QString color = repoSecuritySeverityColor(signal.severity);
    QString html = QStringLiteral(
                       "<div style='font-weight:700; font-size:13px; color:%1'>%2</div>"
                       "<div style='color:#8b949e; font-size:11px; font-weight:600; "
                       "text-transform:uppercase; margin-top:2px'>%3</div>")
                       .arg(color, signal.title.toHtmlEscaped(),
                            RepoSecurity::severityText(signal.severity).toHtmlEscaped());
    if (!signal.summary.isEmpty())
        html += QStringLiteral(
                    "<div style='color:#c9d1d9; font-size:12px; margin-top:6px'>%1</div>")
                    .arg(signal.summary.toHtmlEscaped());
    if (!signal.detail.isEmpty())
        html += QStringLiteral(
                    "<div style='color:#8b949e; font-size:11px; margin-top:4px'>%1</div>")
                    .arg(signal.detail.toHtmlEscaped());
    for (const QString &item : signal.items) {
        const QString href = QString::fromUtf8(QUrl::toPercentEncoding(item));
        html += QStringLiteral(
                    "<div style='color:#c9d1d9; font-size:12px; margin-top:4px'>"
                    "&#8226;&nbsp;<a style='color:#58a6ff; text-decoration:none' "
                    "href='manifest:%1'>%2</a></div>")
                    .arg(href, item.toHtmlEscaped());
    }
    return html;
}

// Fill a Security/Quality findings table: one row per finding (or a single
// "Pass" row when there are none). Column 2 carries the path/line payload the
// tables' cellClicked handlers use to open the file.
static void fillRepoFindingsTable(QTableWidget *table,
                                  const QList<RepoSecurityFinding> &findings,
                                  const QString &emptyText)
{
    if (findings.isEmpty()) {
        const int row = table->rowCount();
        table->insertRow(row);
        auto *severity = new QTableWidgetItem("Pass");
        severity->setForeground(QColor(repoSecuritySeverityColor(
            RepoSecuritySeverity::Pass)));
        table->setItem(row, 0, severity);
        table->setItem(row, 1, new QTableWidgetItem("Local scan"));
        table->setItem(row, 2, new QTableWidgetItem(emptyText));
        table->setItem(row, 3, new QTableWidgetItem("-"));
        return;
    }
    for (const RepoSecurityFinding &finding : findings) {
        const int row = table->rowCount();
        table->insertRow(row);
        auto *severity =
            new QTableWidgetItem(RepoSecurity::severityText(finding.severity));
        severity->setForeground(QColor(repoSecuritySeverityColor(finding.severity)));
        auto *category = new QTableWidgetItem(finding.category);
        const QString location =
            finding.path.isEmpty()
                ? QString()
                : QStringLiteral(" (%1%2)")
                      .arg(finding.path,
                           finding.line > 0
                               ? QStringLiteral(":%1").arg(finding.line)
                               : QString());
        auto *detail =
            new QTableWidgetItem(finding.title + QStringLiteral(": ") +
                                 finding.detail + location);
        if (!finding.path.isEmpty()) {
            detail->setData(Qt::UserRole, finding.path);
            detail->setData(Qt::UserRole + 1, finding.line);
            detail->setToolTip(
                QStringLiteral("Click to open %1:%2")
                    .arg(finding.path)
                    .arg(finding.line > 0 ? QString::number(finding.line)
                                          : QStringLiteral("?")));
        }
        auto *action = new QTableWidgetItem(finding.recommendedAction);
        for (QTableWidgetItem *item : {severity, category, action})
            item->setToolTip(item->text());
        table->setItem(row, 0, severity);
        table->setItem(row, 1, category);
        table->setItem(row, 2, detail);
        table->setItem(row, 3, action);
    }
}

static QWidget *buildDepScanCard(const RepoSecuritySignal &signal,
                                  const QList<RepoSecurityFinding> &findings,
                                  const QHash<QString, int> &dependencyCounts,
                                  qint64 generatedAtMs,
                                  const QString &localBase,
                                  QObject *context,
                                  bool scanning,
                                  const QString &scanningPath,
                                  std::function<void(const QString &)> runScan)
{
    auto *card = new QFrame;
    card->setObjectName("insightsCard");
    auto *vlay = new QVBoxLayout(card);
    vlay->setContentsMargins(12, 12, 12, 12);
    vlay->setSpacing(6);

    const QString color = repoSecuritySeverityColor(signal.severity);
    auto *titleRow = new QHBoxLayout;
    titleRow->setContentsMargins(0, 0, 0, 0);
    titleRow->setSpacing(8);
    auto *titleLbl = new QLabel(
        QStringLiteral("<span style='font-weight:700;font-size:13px;color:%1'>%2</span>"
                       "&nbsp;<span style='color:#8b949e;font-size:11px;font-weight:600;"
                       "text-transform:uppercase'>%3</span>")
            .arg(color, signal.title.toHtmlEscaped(),
                 RepoSecurity::severityText(signal.severity).toHtmlEscaped()));
    titleLbl->setTextFormat(Qt::RichText);
    titleRow->addWidget(titleLbl, 1);
    if (scanning) {
        auto *scanningLbl = new QLabel(
            QStringLiteral("<span style='color:#388bfd;font-size:11px;font-weight:600'>"
                           "Scanning&#8230;</span>"));
        scanningLbl->setTextFormat(Qt::RichText);
        titleRow->addWidget(scanningLbl, 0);
    } else if (generatedAtMs > 0) {
        const QString ts = QDateTime::fromMSecsSinceEpoch(generatedAtMs)
                               .toString(QStringLiteral("yyyy-MM-dd hh:mm:ss"));
        auto *timeLbl = new QLabel(
            QStringLiteral("<span style='color:#8b949e;font-size:11px'>Last scanned: %1</span>")
                .arg(ts));
        timeLbl->setTextFormat(Qt::RichText);
        titleRow->addWidget(timeLbl, 0);
    }
    vlay->addLayout(titleRow);

    if (scanning) {
        auto *bar = new QProgressBar;
        bar->setRange(0, 0);
        bar->setTextVisible(false);
        bar->setFixedHeight(4);
        bar->setStyleSheet(Theme::remapSheet(
            QStringLiteral("QProgressBar{border:none;border-radius:2px;"
                           "background-color:#30363d;}"
                           "QProgressBar::chunk{border-radius:2px;"
                           "background-color:#388bfd;}")));
        vlay->addWidget(bar);
    }

    if (!signal.summary.isEmpty()) {
        auto *summaryLbl = new QLabel(
            QStringLiteral("<span style='color:#c9d1d9;font-size:12px'>%1</span>")
                .arg(signal.summary.toHtmlEscaped()));
        summaryLbl->setTextFormat(Qt::RichText);
        vlay->addWidget(summaryLbl);
    }

    if (signal.items.isEmpty()) {
        if (!signal.detail.isEmpty()) {
            auto *detailLbl = new QLabel(
                QStringLiteral("<span style='color:#8b949e;font-size:11px'>%1</span>")
                    .arg(signal.detail.toHtmlEscaped()));
            detailLbl->setTextFormat(Qt::RichText);
            vlay->addWidget(detailLbl);
        }
        return card;
    }

    QHash<QString, int> outdatedByPath;
    for (const RepoSecurityFinding &f : findings) {
        if (f.category == QLatin1String("Dependency"))
            outdatedByPath[f.path]++;
    }

    auto *grid = new QGridLayout;
    grid->setContentsMargins(0, 8, 0, 0);
    grid->setSpacing(4);
    grid->setColumnStretch(0, 1);

    auto makeHdr = [](const QString &text) {
        auto *lbl = new QLabel(
            QStringLiteral("<span style='color:#8b949e;font-size:11px;font-weight:600'>"
                           "%1</span>")
                .arg(text.toHtmlEscaped()));
        lbl->setTextFormat(Qt::RichText);
        return lbl;
    };
    grid->addWidget(makeHdr(QStringLiteral("Manifest")), 0, 0);
    grid->addWidget(makeHdr(QStringLiteral("Dependencies")), 0, 1, Qt::AlignRight);
    grid->addWidget(makeHdr(QStringLiteral("Outdated")), 0, 2, Qt::AlignRight);
    grid->addWidget(makeHdr(QStringLiteral("Vulnerable")), 0, 3, Qt::AlignRight);

    int row = 1;
    for (const QString &manifest : signal.items) {
        const QString href = QString::fromUtf8(QUrl::toPercentEncoding(manifest));
        auto *pathLbl = new QLabel(
            QStringLiteral("<a style='color:#58a6ff;text-decoration:none' "
                           "href='manifest:%1'>%2</a>")
                .arg(href, manifest.toHtmlEscaped()));
        pathLbl->setTextFormat(Qt::RichText);
        pathLbl->setTextInteractionFlags(Qt::TextBrowserInteraction);
        if (!localBase.trimmed().isEmpty()) {
            const QString abs = QDir(localBase).filePath(manifest);
            QObject::connect(pathLbl, &QLabel::linkActivated, context,
                             [abs](const QString &) {
                                 if (QFileInfo::exists(abs))
                                     QDesktopServices::openUrl(
                                         QUrl::fromLocalFile(abs));
                             });
        }

        const int depCount = dependencyCounts.value(manifest, 0);
        const QString depHtml =
            QStringLiteral("<span style='color:#c9d1d9;font-size:12px'>%1</span>")
                .arg(depCount);
        auto *depLbl = new QLabel(depHtml);
        depLbl->setTextFormat(Qt::RichText);
        depLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

        const int outdated = outdatedByPath.value(manifest, 0);
        const QString outdatedHtml =
            outdated > 0
                ? QStringLiteral("<span style='color:#d29922;font-size:12px'>%1</span>")
                      .arg(outdated)
                : QStringLiteral("<span style='color:#3fb950;font-size:12px'>0</span>");
        auto *outdatedLbl = new QLabel(outdatedHtml);
        outdatedLbl->setTextFormat(Qt::RichText);
        outdatedLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

        auto *vulnLbl = new QLabel(
            QStringLiteral("<span style='color:#8b949e;font-size:12px'>&ndash;</span>"));
        vulnLbl->setTextFormat(Qt::RichText);
        vulnLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

        const bool isScanning = scanning && (scanningPath == manifest);
        auto *scanBtn = new QPushButton(isScanning ? QString::fromUtf8("Scanning\xE2\x80\xA6")
                                                   : QStringLiteral("Run scan"));
        scanBtn->setObjectName("ghostButton");
        scanBtn->setProperty("buttonSize", "sm");
        scanBtn->setEnabled(!scanning);
        scanBtn->setCursor(scanning ? Qt::BusyCursor : Qt::PointingHandCursor);
        QObject::connect(scanBtn, &QPushButton::clicked, context,
                         [runScan, manifest]() { runScan(manifest); });

        grid->addWidget(pathLbl, row, 0);
        grid->addWidget(depLbl, row, 1, Qt::AlignRight);
        grid->addWidget(outdatedLbl, row, 2, Qt::AlignRight);
        grid->addWidget(vulnLbl, row, 3, Qt::AlignRight);
        grid->addWidget(scanBtn, row, 4, Qt::AlignRight);
        ++row;
    }

    vlay->addLayout(grid);
    return card;
}

RepoSecurityInput MainWindow::buildRepoSecurityInput(const RepositoryRecord &selected,
                                                     const RepositoryRecord &writable) const
{
    RepoSecurityInput input;
    input.owner = selected.owner;
    input.name = selected.name;
    input.localPath = writable.localPath;
    input.mirrorPath = writable.mirrorPath.isEmpty() ? selected.mirrorPath
                                                     : writable.mirrorPath;
    input.publishToNetwork = selected.publishToNetwork;
    input.isPrivate = selected.isPrivate;
    input.previewOnly = selected.previewOnly;
    input.actionsEnabled = selected.actionsEnabled;
    input.integrityWarning = m_repoPinMismatch;
    input.issues =
        IssueStore(writable.localPath, input.mirrorPath, &m_profileIdentity, m_userName)
            .loadAll();
    input.workflows = availableWorkflowsForRepo(writable);
    for (const ActionRun &run : std::as_const(m_actionRuns))
        if (run.owner == selected.owner && run.name == selected.name)
            input.actionRuns.append(run);
    return input;
}

void MainWindow::applyRepoSecuritySnapshot(const RepoSecuritySnapshot &snapshot,
                                           const QString &localBase)
{
    m_lastRepoSecuritySnapshot = snapshot;

    while (QLayoutItem *item = m_securitySignalsGrid->takeAt(0)) {
        if (QWidget *widget = item->widget())
            widget->deleteLater();
        delete item;
    }

    TableRepaintGuard repaintGuard(m_securityFindingsTable);
    m_securityFindingsTable->setSortingEnabled(false);
    m_securityFindingsTable->setRowCount(0);

    const RepoSecuritySeverity highest = RepoSecurity::highestSeverity(snapshot);
    int warnings = 0;
    int severe = 0;
    for (const RepoSecuritySignal &signal : snapshot.signalList) {
        if (signal.severity == RepoSecuritySeverity::Warning)
            ++warnings;
        else if (signal.severity == RepoSecuritySeverity::High ||
                 signal.severity == RepoSecuritySeverity::Critical)
            ++severe;
    }
    for (const RepoSecurityFinding &finding : snapshot.findings) {
        if (finding.severity == RepoSecuritySeverity::Warning)
            ++warnings;
        else if (finding.severity == RepoSecuritySeverity::High ||
                 finding.severity == RepoSecuritySeverity::Critical)
            ++severe;
    }

    m_securitySummary->setText(
        QStringLiteral(
            "<div style='font-size:21px; font-weight:800; color:%1'>%2</div>"
            "<div style='color:#8b949e; font-size:12px; font-weight:600'>"
            "%3 at %4. %5 finding%6, %7 warning%8, %9 severe.</div>")
            .arg(repoSecuritySeverityColor(highest),
                 RepoSecurity::severityText(highest).toHtmlEscaped(),
                 snapshot.repoKey.toHtmlEscaped(), snapshot.ref.toHtmlEscaped())
            .arg(snapshot.findings.size())
            .arg(snapshot.findings.size() == 1 ? QString() : QStringLiteral("s"))
            .arg(warnings)
            .arg(warnings == 1 ? QString() : QStringLiteral("s"))
            .arg(severe));

    int index = 0;
    const RepoSecuritySignal *depSignal = nullptr;
    for (const RepoSecuritySignal &signal : snapshot.signalList) {
        if (signal.key == QLatin1String("dependencies")) {
            depSignal = &signal;
            continue;
        }
        auto *card = new QLabel(repoSecuritySignalHtml(signal));
        card->setObjectName("insightsCard");
        card->setTextFormat(Qt::RichText);
        card->setWordWrap(true);
        card->setMinimumHeight(92);
        const int row = index / 3;
        const int col = index % 3;
        m_securitySignalsGrid->addWidget(card, row, col);
        ++index;
    }
    if (depSignal) {
        const int depRow = (index + 2) / 3;
        auto *depCard = buildDepScanCard(
            *depSignal, snapshot.findings, snapshot.dependencyCounts, snapshot.generatedAtMs,
            localBase, this, m_repoSecurityScanRunning, m_repoSecurityScanningPath,
            [this](const QString &manifestPath) { runRepoDependencyScan(manifestPath); });
        m_securitySignalsGrid->addWidget(depCard, depRow, 0, 1, 4);
    }

    fillRepoFindingsTable(m_securityFindingsTable, snapshot.findings,
                          QStringLiteral("No local findings at this ref."));

    m_securityFindingsTable->setSortingEnabled(true);
}

void MainWindow::refreshRepoSecurity()
{
    if (!m_securitySummary || !m_securitySignalsGrid || !m_securityFindingsTable)
        return;

    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size()) {
        while (QLayoutItem *item = m_securitySignalsGrid->takeAt(0)) {
            if (QWidget *widget = item->widget())
                widget->deleteLater();
            delete item;
        }
        TableRepaintGuard repaintGuard(m_securityFindingsTable);
        m_securityFindingsTable->setSortingEnabled(false);
        m_securityFindingsTable->setRowCount(0);
        m_securitySummary->setText(
            "<b>Security</b><br><span style='color:#8b949e'>"
            "Select a repository to scan local evidence.</span>");
        m_securityFindingsTable->setSortingEnabled(true);
        return;
    }

    const RepositoryRecord &selected = m_repositories.at(m_repoDetailIndex);
    const RepositoryRecord &writable = writableRecordFor(selected);
    m_repoSecurityScanRunning = false;
    m_repoSecurityScanningPath.clear();
    applyRepoSecuritySnapshot(
        RepoSecurity::scan(buildRepoSecurityInput(selected, writable)),
        writable.localPath);
}

void MainWindow::runRepoDependencyScan(const QString &manifestPath)
{
    if (m_repoSecurityScanRunning)
        return;
    if (!m_securitySummary || !m_securitySignalsGrid || !m_securityFindingsTable)
        return;
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return;

    const RepositoryRecord &selected = m_repositories.at(m_repoDetailIndex);
    const RepositoryRecord &writable = writableRecordFor(selected);
    const RepoSecurityInput input = buildRepoSecurityInput(selected, writable);
    const QString startOwner = selected.owner;
    const QString startName = selected.name;

    m_repoSecurityScanRunning = true;
    m_repoSecurityScanningPath = manifestPath;
    applyRepoSecuritySnapshot(m_lastRepoSecuritySnapshot, writable.localPath);

    if (manifestPath.isEmpty()) {
        runOffThread<RepoSecuritySnapshot>(
            [input]() { return RepoSecurity::scan(input); },
            [this, startOwner, startName](RepoSecuritySnapshot snapshot) {
                m_repoSecurityScanRunning = false;
                m_repoSecurityScanningPath.clear();
                if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
                    return;
                const RepositoryRecord &current = m_repositories.at(m_repoDetailIndex);
                if (current.owner != startOwner || current.name != startName)
                    return; // user navigated to a different repo while the scan ran
                applyRepoSecuritySnapshot(snapshot, writableRecordFor(current).localPath);
            });
    } else {
        runOffThread<RepoSecurityManifestScan>(
            [input, manifestPath]() { return RepoSecurity::scanManifest(input, manifestPath); },
            [this, startOwner, startName, manifestPath](RepoSecurityManifestScan result) {
                m_repoSecurityScanRunning = false;
                m_repoSecurityScanningPath.clear();
                if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
                    return;
                const RepositoryRecord &current = m_repositories.at(m_repoDetailIndex);
                if (current.owner != startOwner || current.name != startName)
                    return;

                RepoSecuritySnapshot &snapshot = m_lastRepoSecuritySnapshot;
                snapshot.dependencyCounts[manifestPath] = result.dependencyCount;
                snapshot.findings.erase(
                    std::remove_if(snapshot.findings.begin(), snapshot.findings.end(),
                                   [&manifestPath](const RepoSecurityFinding &f) {
                                       return f.path == manifestPath && f.category == QLatin1String("Dependency");
                                   }),
                    snapshot.findings.end());
                snapshot.findings.append(result.findings);

                applyRepoSecuritySnapshot(snapshot, writableRecordFor(current).localPath);
            });
    }
}

void MainWindow::openRepoFileAtLine(const QString &path, int line)
{
    if (line <= 0) {
        openRepoFile(path);
        return;
    }
    openRepoFile(path, [this, path, line] {
        auto *edit = qobject_cast<QPlainTextEdit *>(m_openFileTabs.value(path));
        if (!edit)
            return;
        QTextCursor lc(edit->document());
        lc.movePosition(QTextCursor::Start);
        if (line > 1)
            lc.movePosition(QTextCursor::Down, QTextCursor::MoveAnchor, line - 1);
        QTextEdit::ExtraSelection lineSel;
        lineSel.cursor = lc;
        QColor lineTint =
            Theme::nextOr(Theme::Role::Accent, QColor(31, 111, 235, 60));
        lineTint.setAlpha(60);
        lineSel.format.setBackground(lineTint);
        lineSel.format.setProperty(QTextFormat::FullWidthSelection, true);
        edit->setTextCursor(lc);
        edit->setFocus();
        edit->centerCursor();
        edit->setExtraSelections({lineSel});
    });
}

QWidget *MainWindow::buildRepoQualityTab()
{
    auto *page = new QWidget;
    page->setObjectName("mainContent");

    auto *scroll = new QScrollArea;
    scroll->setObjectName("mainContent");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto *content = new QWidget;
    content->setObjectName("insightsPage");
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(18, 18, 18, 18);
    layout->setSpacing(14);

    auto *heading = new QLabel("Quality");
    heading->setObjectName("qualityTitle");
    heading->setProperty("class", "channelTitle");
    heading->setStyleSheet(Theme::remapSheet(QStringLiteral("font-size:16px;font-weight:700;")));
    auto *subtitle = new QLabel(
        "Local quality metrics for this repository: check runs, code volume, "
        "documentation, tests, maintenance markers, file sizes, commit "
        "activity and issue hygiene.");
    subtitle->setObjectName("statusLine");
    subtitle->setWordWrap(true);

    m_qualityRefreshButton = new QPushButton("Refresh");
    m_qualityRefreshButton->setObjectName("ghostButton");
    m_qualityRefreshButton->setProperty("buttonSize", "sm");
    m_qualityRefreshButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_qualityRefreshButton, "sync", 16);
    connect(m_qualityRefreshButton, &QPushButton::clicked, this,
            &MainWindow::refreshRepoQuality);

    auto *headingCol = new QVBoxLayout;
    headingCol->setContentsMargins(0, 0, 0, 0);
    headingCol->setSpacing(3);
    headingCol->addWidget(heading);
    headingCol->addWidget(subtitle);

    auto *headerRow = new QHBoxLayout;
    headerRow->setContentsMargins(0, 0, 0, 0);
    headerRow->setSpacing(8);
    headerRow->addLayout(headingCol, 1);
    headerRow->addWidget(m_qualityRefreshButton, 0, Qt::AlignTop);
    layout->addLayout(headerRow);

    m_qualitySummary = new QLabel;
    m_qualitySummary->setObjectName("insightsCard");
    m_qualitySummary->setTextFormat(Qt::RichText);
    m_qualitySummary->setWordWrap(true);
    m_qualitySummary->setMinimumHeight(92);
    layout->addWidget(m_qualitySummary);

    auto *metricsLabel = new QLabel("METRICS");
    metricsLabel->setObjectName("sectionLabel");
    layout->addWidget(metricsLabel);

    m_qualitySignalsPanel = new QWidget;
    m_qualitySignalsGrid = new QGridLayout(m_qualitySignalsPanel);
    m_qualitySignalsGrid->setContentsMargins(0, 0, 0, 0);
    m_qualitySignalsGrid->setSpacing(10);
    layout->addWidget(m_qualitySignalsPanel);

    auto *findingsLabel = new QLabel("FINDINGS");
    findingsLabel->setObjectName("sectionLabel");
    layout->addWidget(findingsLabel);

    m_qualityFindingsTable = new QTableWidget(0, 4);
    installColumnHeaderMenu(m_qualityFindingsTable);
    m_qualityFindingsTable->setObjectName("issueTable");
    enableHoverRowHighlight(m_qualityFindingsTable);
    m_qualityFindingsTable->setHorizontalHeaderLabels(
        {"Severity", "Category", "Finding", "Suggestion"});
    m_qualityFindingsTable->verticalHeader()->setVisible(false);
    m_qualityFindingsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_qualityFindingsTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_qualityFindingsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_qualityFindingsTable->setShowGrid(false);
    m_qualityFindingsTable->setWordWrap(false);
    QHeaderView *header = m_qualityFindingsTable->horizontalHeader();
    header->setHighlightSections(false);
    header->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(2, QHeaderView::Stretch);
    header->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    makeColumnsResizable(m_qualityFindingsTable);
    m_qualityFindingsTable->setMinimumHeight(220);
    layout->addWidget(m_qualityFindingsTable);
    layout->addStretch();
    connect(m_qualityFindingsTable, &QTableWidget::cellClicked,
            this, [this](int row, int /*col*/) {
        auto *item = m_qualityFindingsTable->item(row, 2);
        if (!item)
            return;
        const QString path = item->data(Qt::UserRole).toString();
        if (path.isEmpty())
            return;
        openRepoFileAtLine(path, item->data(Qt::UserRole + 1).toInt());
    });

    scroll->setWidget(content);
    auto *pageLayout = new QVBoxLayout(page);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    pageLayout->addWidget(scroll);
    refreshRepoQuality();
    return page;
}

void MainWindow::refreshRepoQuality()
{
    if (!m_qualitySummary || !m_qualitySignalsGrid || !m_qualityFindingsTable)
        return;

    while (QLayoutItem *item = m_qualitySignalsGrid->takeAt(0)) {
        if (QWidget *widget = item->widget())
            widget->deleteLater();
        delete item;
    }

    TableRepaintGuard repaintGuard(m_qualityFindingsTable);
    m_qualityFindingsTable->setSortingEnabled(false);
    m_qualityFindingsTable->setRowCount(0);

    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size()) {
        m_qualitySummary->setText(
            "<b>Quality</b><br><span style='color:#8b949e'>"
            "Select a repository to compute quality metrics.</span>");
        m_qualityFindingsTable->setSortingEnabled(true);
        return;
    }

    const RepositoryRecord &selected = m_repositories.at(m_repoDetailIndex);
    const RepositoryRecord &writable = writableRecordFor(selected);

    RepoSecurityInput input;
    input.owner = selected.owner;
    input.name = selected.name;
    input.localPath = writable.localPath;
    input.mirrorPath = writable.mirrorPath.isEmpty() ? selected.mirrorPath
                                                     : writable.mirrorPath;
    input.publishToNetwork = selected.publishToNetwork;
    input.isPrivate = selected.isPrivate;
    input.previewOnly = selected.previewOnly;
    input.actionsEnabled = selected.actionsEnabled;
    input.issues =
        IssueStore(writable.localPath, input.mirrorPath, &m_profileIdentity, m_userName)
            .loadAll();
    input.workflows = availableWorkflowsForRepo(writable);
    for (const ActionRun &run : std::as_const(m_actionRuns))
        if (run.owner == selected.owner && run.name == selected.name)
            input.actionRuns.append(run);

    const RepoSecuritySnapshot snapshot = RepoQuality::scan(input);
    const RepoSecuritySeverity highest = RepoSecurity::highestSeverity(snapshot);
    int warnings = 0;
    int severe = 0;
    for (const RepoSecuritySignal &signal : snapshot.signalList) {
        if (signal.severity == RepoSecuritySeverity::Warning)
            ++warnings;
        else if (signal.severity == RepoSecuritySeverity::High ||
                 signal.severity == RepoSecuritySeverity::Critical)
            ++severe;
    }
    for (const RepoSecurityFinding &finding : snapshot.findings) {
        if (finding.severity == RepoSecuritySeverity::Warning)
            ++warnings;
        else if (finding.severity == RepoSecuritySeverity::High ||
                 finding.severity == RepoSecuritySeverity::Critical)
            ++severe;
    }

    m_qualitySummary->setText(
        QStringLiteral(
            "<div style='font-size:21px; font-weight:800; color:%1'>%2</div>"
            "<div style='color:#8b949e; font-size:12px; font-weight:600'>"
            "%3 at %4. %5 finding%6, %7 warning%8, %9 severe.</div>")
            .arg(repoSecuritySeverityColor(highest),
                 RepoSecurity::severityText(highest).toHtmlEscaped(),
                 snapshot.repoKey.toHtmlEscaped(), snapshot.ref.toHtmlEscaped())
            .arg(snapshot.findings.size())
            .arg(snapshot.findings.size() == 1 ? QString() : QStringLiteral("s"))
            .arg(warnings)
            .arg(warnings == 1 ? QString() : QStringLiteral("s"))
            .arg(severe));

    int index = 0;
    for (const RepoSecuritySignal &signal : snapshot.signalList) {
        auto *card = new QLabel(repoSecuritySignalHtml(signal));
        card->setObjectName("insightsCard");
        card->setTextFormat(Qt::RichText);
        card->setWordWrap(true);
        card->setMinimumHeight(92);
        const int row = index / 3;
        const int col = index % 3;
        m_qualitySignalsGrid->addWidget(card, row, col);
        ++index;
    }

    fillRepoFindingsTable(m_qualityFindingsTable, snapshot.findings,
                          QStringLiteral("No quality findings at this ref."));

    m_qualityFindingsTable->setSortingEnabled(true);
}

namespace {
using forkmesh::DirectorySizeScanCancel;
using forkmesh::DirectorySizeScanOptions;
using forkmesh::DirectorySizeScanResult;

constexpr int kSizeMapWorkerLineLimit = 8;

QList<QStorageInfo> sizeMapVolumes()
{
    QList<QStorageInfo> volumes;
    QSet<QString> seen;
    for (const QStorageInfo &volume : QStorageInfo::mountedVolumes()) {
        if (!volume.isValid() || !volume.isReady() || volume.bytesTotal() <= 0)
            continue;
        if (volume.fileSystemType() == "squashfs")
            continue;
        const QString mount = volume.rootPath();
        if (mount.isEmpty() || seen.contains(mount))
            continue;
        seen.insert(mount);
        volumes.append(volume);
    }
    std::sort(volumes.begin(), volumes.end(),
              [](const QStorageInfo &a, const QStorageInfo &b) {
                  return a.bytesTotal() > b.bytesTotal();
              });
    return volumes;
}
} // namespace

QWidget *MainWindow::buildSizeMapTab()
{
    auto *page = new QWidget;
    page->setObjectName("mainContent");

    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(18, 18, 18, 18);
    layout->setSpacing(10);

    auto *heading = new QLabel("Size map");
    heading->setObjectName("channelTitle");
    auto *subtitle = new QLabel(
        "How a folder's bytes spread across directories and files "
        "(.git excluded). Click a directory to zoom in, the centre to zoom "
        "back out; slices with no further subdivision are individual files. "
        "It starts on this repository's working copy — pick any other folder "
        "on disk, or a filesystem on the right, to size that instead. Folders "
        "holding directories this user cannot read — \"/\" above all — ask for "
        "the root password first, so the map covers everything.");
    subtitle->setObjectName("statusLine");
    subtitle->setWordWrap(true);

    auto *refresh = new QPushButton("Rescan");
    refresh->setObjectName("ghostButton");
    refresh->setProperty("buttonSize", "sm");
    refresh->setCursor(Qt::PointingHandCursor);
    setOcticon(refresh, "sync", 16);
    connect(refresh, &QPushButton::clicked, this,
            [this] { refreshSizeMapTab(true, true); });
    addRefreshSpin(refresh);

    auto *stop = new QPushButton("Stop");
    stop->setObjectName("ghostButton");
    stop->setProperty("buttonSize", "sm");
    stop->setCursor(Qt::PointingHandCursor);
    setOcticon(stop, "x", 16);
    stop->setToolTip("Stop the scan in progress.");
    stop->setVisible(false);
    m_sizeMapStop = stop;
    connect(stop, &QPushButton::clicked, this, [this] { stopSizeMapScan(); });

    auto *headingCol = new QVBoxLayout;
    headingCol->setContentsMargins(0, 0, 0, 0);
    headingCol->setSpacing(3);
    headingCol->addWidget(heading);
    headingCol->addWidget(subtitle);

    auto *headerRow = new QHBoxLayout;
    headerRow->setContentsMargins(0, 0, 0, 0);
    headerRow->setSpacing(8);
    headerRow->addLayout(headingCol, 1);
    headerRow->addWidget(refresh, 0, Qt::AlignTop);
    headerRow->addWidget(stop, 0, Qt::AlignTop);
    layout->addLayout(headerRow);

    auto *choose = new QPushButton("Choose folder…");
    choose->setObjectName("ghostButton");
    choose->setProperty("buttonSize", "sm");
    choose->setCursor(Qt::PointingHandCursor);
    setOcticon(choose, "file-directory", 16);
    choose->setToolTip("Scan any folder on this machine instead of the "
                       "repository's working copy.");
    connect(choose, &QPushButton::clicked, this,
            [this] { chooseSizeMapFolder(); });

    auto *resetRoot = new QPushButton("Back to repository");
    resetRoot->setObjectName("ghostButton");
    resetRoot->setProperty("buttonSize", "sm");
    resetRoot->setCursor(Qt::PointingHandCursor);
    resetRoot->setToolTip("Size this repository's working copy again.");
    resetRoot->setVisible(false);
    m_sizeMapResetRoot = resetRoot;
    connect(resetRoot, &QPushButton::clicked, this,
            [this] { setSizeMapRootOverride(QString()); });

    m_sizeMapRootLabel = new QLabel;
    m_sizeMapRootLabel->setObjectName("statusLine");
    m_sizeMapRootLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto *folderRow = new QHBoxLayout;
    folderRow->setContentsMargins(0, 0, 0, 0);
    folderRow->setSpacing(8);
    folderRow->addWidget(choose);
    folderRow->addWidget(resetRoot);
    folderRow->addWidget(m_sizeMapRootLabel, 1);
    layout->addLayout(folderRow);

    auto *hideIgnored = new QCheckBox("Hide .gitignored files");
    hideIgnored->setCursor(Qt::PointingHandCursor);
    hideIgnored->setToolTip(
        "Drop everything git ignores (build output, node_modules, …) from the "
        "map, so only tracked and un-ignored files count toward the sizes.");
    m_sizeMapHideIgnored = hideIgnored;
    connect(hideIgnored, &QCheckBox::toggled, this,
            [this] { refreshSizeMapTab(true, true); });
    layout->addWidget(hideIgnored);

    m_sizeMapStatus = new QLabel;
    m_sizeMapStatus->setObjectName("statusLine");
    m_sizeMapStatus->setWordWrap(true);

    auto *elevate = new QPushButton("Scan as administrator");
    elevate->setObjectName("ghostButton");
    elevate->setProperty("buttonSize", "sm");
    elevate->setCursor(Qt::PointingHandCursor);
    setOcticon(elevate, "lock", 16);
    elevate->setToolTip(
        "Ask for the administrator (root) password and measure the folders this "
        "user cannot read.");
    elevate->setVisible(false);
    m_sizeMapElevate = elevate;
    connect(elevate, &QPushButton::clicked, this,
            [this] { rescanSizeMapElevated(); });

    auto *statusRow = new QHBoxLayout;
    statusRow->setContentsMargins(0, 0, 0, 0);
    statusRow->setSpacing(8);
    statusRow->addWidget(m_sizeMapStatus, 1);
    statusRow->addWidget(elevate, 0, Qt::AlignTop);
    layout->addLayout(statusRow);

    auto *workers = new QWidget;
    workers->setVisible(false);
    auto *workersCol = new QVBoxLayout(workers);
    workersCol->setContentsMargins(0, 0, 0, 0);
    workersCol->setSpacing(1);
    m_sizeMapWorkersBox = workers;
    layout->addWidget(workers);

    auto *chart = new RepoSunburstChart;
    m_sizeMapChart = chart;

    auto *body = new QHBoxLayout;
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(14);
    body->addWidget(chart, 1);
    body->addWidget(buildSizeMapVolumesPanel(), 0);
    layout->addLayout(body, 1);
    refreshSizeMapVolumes();
    return page;
}

QWidget *MainWindow::buildSizeMapVolumesPanel()
{
    auto *panel = new QWidget;
    panel->setFixedWidth(268);
    auto *col = new QVBoxLayout(panel);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(6);

    auto *title = new QLabel("Filesystems");
    title->setObjectName("sectionTitle");
    col->addWidget(title);

    auto *hint = new QLabel(
        "Every mounted filesystem, sized from the mount itself. Click one to "
        "expand it into the full map — nested mounts are left to their own "
        "card, so the totals match df.");
    hint->setObjectName("statusLine");
    hint->setWordWrap(true);
    col->addWidget(hint);

    auto *scroll = new QScrollArea;
    scroll->setObjectName("mainContent");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto *inner = new QWidget;
    m_sizeMapVolumesBox = inner;
    auto *innerCol = new QVBoxLayout(inner);
    innerCol->setContentsMargins(0, 0, 0, 0);
    innerCol->setSpacing(2);
    innerCol->addStretch(1);
    scroll->setWidget(inner);
    col->addWidget(scroll, 1);
    return panel;
}

void MainWindow::refreshSizeMapVolumes()
{
    if (!m_sizeMapVolumesBox)
        return;
    auto *col = qobject_cast<QVBoxLayout *>(m_sizeMapVolumesBox->layout());
    if (!col)
        return;
    while (QLayoutItem *item = col->takeAt(0)) {
        if (QWidget *widget = item->widget())
            widget->deleteLater();
        delete item;
    }
    const QString current = QDir::cleanPath(sizeMapRoot());
    for (const QStorageInfo &volume : sizeMapVolumes()) {
        auto *card = new StorageMiniMap(volume);
        const QString mount = card->mountPoint();
        card->setSelected(!current.isEmpty() && QDir::cleanPath(mount) == current);
        card->setOnClicked([this, mount] { setSizeMapRootOverride(mount); });
        col->addWidget(card);
    }
    col->addStretch(1);
}

QString MainWindow::sizeMapRoot() const
{
    if (!m_sizeMapRootOverride.isEmpty())
        return m_sizeMapRootOverride;
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return QString();
    return writableRecordFor(m_repositories.at(m_repoDetailIndex)).localPath;
}

void MainWindow::chooseSizeMapFolder()
{
    QString start = sizeMapRoot();
    if (start.isEmpty() || !QDir(start).exists())
        start = QDir::homePath();
    QList<QUrl> sidebar;
    for (const QStorageInfo &volume : sizeMapVolumes())
        sidebar.append(QUrl::fromLocalFile(volume.rootPath()));
    const QString chosen = chooseExistingDirectory(
        this, QStringLiteral("Choose a folder to size"), start, sidebar);
    if (chosen.isEmpty())
        return;
    setSizeMapRootOverride(chosen);
}

void MainWindow::setSizeMapRootOverride(const QString &path)
{
    if (m_sizeMapRootOverride == path)
        return;
    m_sizeMapRootOverride = path;
    refreshSizeMapTab(true, true);
}

void MainWindow::refreshSizeMapTab(bool force, bool allowElevation)
{
    auto *chart = static_cast<RepoSunburstChart *>(m_sizeMapChart);
    if (!chart || !m_sizeMapStatus)
        return;
    const bool overridden = !m_sizeMapRootOverride.isEmpty();
    if (!overridden &&
        (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size()))
        return;
    const QString path = sizeMapRoot();
    refreshSizeMapVolumes();
    if (m_sizeMapResetRoot)
        m_sizeMapResetRoot->setVisible(overridden);
    if (m_sizeMapRootLabel) {
        m_sizeMapRootLabel->setText(
            path.isEmpty()
                ? QStringLiteral("No folder selected.")
                : QStringLiteral("%1%2").arg(
                      QDir::toNativeSeparators(path),
                      overridden ? QString()
                                 : QStringLiteral("  ·  working copy")));
    }
    if (path.isEmpty() || !QDir(path).exists()) {
        chart->clear();
        m_sizeMapScannedPath.clear();
        if (m_sizeMapElevate)
            m_sizeMapElevate->setVisible(false);
        m_sizeMapStatus->setText(
            overridden ? "That folder no longer exists — choose another one."
                       : "No local working copy to scan for this repository.");
        return;
    }
    if (!force && m_sizeMapScannedPath == path)
        return; // the chart already shows this folder
    if (m_sizeMapScanning)
        return; // its finish handler notices the root changed and rescans
    if (!sizeMapIgnoredPathsReady(path, [this, allowElevation] {
            refreshSizeMapTab(true, allowElevation);
        }))
        return;
    if (m_sizeMapElevate)
        m_sizeMapElevate->setVisible(false);
    const bool hideIgnored =
        m_sizeMapHideIgnored && m_sizeMapHideIgnored->isChecked();
    DirectorySizeScanOptions options;
    options.pruned = sizeMapPrunedPaths(path);
    if (allowElevation && forkmesh::scanNeedsElevation(path, options.pruned)) {
        const int pending = ++m_sizeMapScanEpoch;
        m_sizeMapStatus->setText(
            QStringLiteral("Asking for administrator access to size %1 …")
                .arg(QDir::toNativeSeparators(path)));
        QTimer::singleShot(0, this, [this, pending] {
            if (pending == m_sizeMapScanEpoch)
                rescanSizeMapElevated(true);
        });
        return;
    }
    m_sizeMapScanning = true;
    if (m_sizeMapStop)
        m_sizeMapStop->setVisible(true);
    clearSizeMapWorkerLines();
    const int epoch = ++m_sizeMapScanEpoch;
    m_sizeMapStatus->setText(
        QStringLiteral("Scanning %1 …").arg(QDir::toNativeSeparators(path)));
    const QPointer<MainWindow> guard(this);
    const forkmesh::DirectorySizeScanProgress progress =
        [this, guard, epoch,
         path](const forkmesh::DirectorySizeScanProgressUpdate &update) {
            if (!guard)
                return;
            QMetaObject::invokeMethod(
                this,
                [this, epoch, path, update] {
                    if (epoch != m_sizeMapScanEpoch)
                        return; // a newer scan owns the lines now
                    showSizeMapScanProgress(path, update, false);
                },
                Qt::QueuedConnection);
        };
    auto *watcher = new QFutureWatcher<DirectorySizeScanResult>(this);
    m_sizeMapWatcher = watcher;
    connect(watcher, &QFutureWatcher<DirectorySizeScanResult>::finished, this,
            [this, watcher, path, epoch, hideIgnored] {
                watcher->deleteLater();
                if (m_sizeMapWatcher == watcher)
                    m_sizeMapWatcher = nullptr;
                m_sizeMapScanning = false;
                if (m_sizeMapStop)
                    m_sizeMapStop->setVisible(false);
                clearSizeMapWorkerLines();
                if (epoch != m_sizeMapScanEpoch)
                    return; // a newer scan superseded this one
                DirectorySizeScanResult result = watcher->result();
                if (sizeMapRoot() != path) {
                    refreshSizeMapTab(false);
                    return;
                }
                applySizeMapResult(path, std::move(result), hideIgnored, false);
            });
    watcher->setFuture(QtConcurrent::run(
        [path, options, progress](QPromise<DirectorySizeScanResult> &promise) {
            const forkmesh::BackgroundScope activity(
                QStringLiteral("scan"), QStringLiteral("Sizing %1").arg(path));
            const DirectorySizeScanCancel canceled = [&promise] {
                return promise.isCanceled();
            };
            promise.addResult(
                forkmesh::scanDirectorySizes(path, options, progress, canceled));
        }));
}

void MainWindow::showSizeMapScanProgress(
    const QString &root, const forkmesh::DirectorySizeScanProgressUpdate &update,
    bool elevated)
{
    if (!m_sizeMapStatus)
        return;
    const QString suffix =
        QStringLiteral("  ·  %1 files · %2 so far · %3 thread%4")
            .arg(QLocale().toString(update.files),
                 QLocale().formattedDataSize(update.bytes),
                 QLocale().toString(qMax(1, update.workers)),
                 update.workers == 1 ? QString() : QStringLiteral("s"));
    const QString prefix = elevated
                               ? QStringLiteral("Scanning as administrator: ")
                               : QStringLiteral("Scanning: ");
    const QFontMetrics metrics(m_sizeMapStatus->font());
    const int budget = qMax(220, m_sizeMapStatus->width() -
                                    metrics.horizontalAdvance(prefix + suffix) -
                                    16);
    m_sizeMapStatus->setText(
        prefix +
        metrics.elidedText(QDir::toNativeSeparators(root), Qt::ElideMiddle,
                           budget) +
        suffix);

    auto *col = m_sizeMapWorkersBox
                    ? qobject_cast<QVBoxLayout *>(m_sizeMapWorkersBox->layout())
                    : nullptr;
    if (!col || update.worker < 0)
        return;
    const int wanted = qMin(update.worker, kSizeMapWorkerLineLimit - 1);
    while (m_sizeMapWorkerLines.size() <= wanted) {
        auto *line = new QLabel(QStringLiteral("…"), m_sizeMapWorkersBox);
        line->setObjectName("statusLine");
        line->setTextFormat(Qt::PlainText);
        const QFont mono = forkmesh::fontSizedLike(
            QFontDatabase::systemFont(QFontDatabase::FixedFont),
            m_sizeMapStatus->font());
        line->setFont(mono);
        line->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        col->addWidget(line);
        m_sizeMapWorkerLines.append(line);
    }
    m_sizeMapWorkersBox->setVisible(true);
    if (update.worker >= kSizeMapWorkerLineLimit) {
        if (!m_sizeMapWorkerOverflow) {
            m_sizeMapWorkerOverflow = new QLabel(m_sizeMapWorkersBox);
            m_sizeMapWorkerOverflow->setObjectName("statusLine");
            m_sizeMapWorkerOverflow->setFont(m_sizeMapWorkerLines.last()->font());
            col->addWidget(m_sizeMapWorkerOverflow);
        }
        m_sizeMapWorkerOverflow->setText(
            QStringLiteral("… and %1 more thread%2 scanning")
                .arg(QLocale().toString(update.workers -
                                        kSizeMapWorkerLineLimit))
                .arg(update.workers - kSizeMapWorkerLineLimit == 1
                         ? QString()
                         : QStringLiteral("s")));
        return;
    }
    QLabel *line = m_sizeMapWorkerLines.at(update.worker);
    const QString lead = QStringLiteral("%1  ").arg(update.worker + 1, 2);
    const QString tail = QStringLiteral("  ·  %1 files · %2")
                             .arg(QLocale().toString(update.workerFiles),
                                  QLocale().formattedDataSize(update.workerBytes));
    if (update.idle) {
        line->setText(lead + QStringLiteral("waiting for the next folder") + tail);
        return;
    }
    const QFontMetrics lineMetrics(line->font());
    const int lineBudget =
        qMax(160, line->width() - lineMetrics.horizontalAdvance(lead + tail) - 16);
    line->setText(lead +
                  lineMetrics.elidedText(QDir::toNativeSeparators(update.path),
                                         Qt::ElideMiddle, lineBudget) +
                  tail);
}

void MainWindow::clearSizeMapWorkerLines()
{
    qDeleteAll(m_sizeMapWorkerLines);
    m_sizeMapWorkerLines.clear();
    delete m_sizeMapWorkerOverflow;
    m_sizeMapWorkerOverflow = nullptr;
    if (m_sizeMapWorkersBox)
        m_sizeMapWorkersBox->setVisible(false);
}

QSet<QString> MainWindow::readSizeMapIgnoredPaths(const QString &path)
{
    QSet<QString> ignored;
    QByteArray out;
    if (!runGitCapture(path,
                       {"ls-files", "--others", "--ignored", "--exclude-standard",
                        "--directory", "-z"},
                       &out, nullptr))
        return ignored;
    const QDir root(path);
    for (const QByteArray &raw : out.split('\0')) {
        if (raw.isEmpty())
            continue;
        QString rel = QString::fromUtf8(raw);
        if (rel.endsWith(QLatin1Char('/')))
            rel.chop(1);
        ignored.insert(root.absoluteFilePath(rel));
    }
    return ignored;
}

bool MainWindow::sizeMapIgnoredPathsReady(const QString &path,
                                          std::function<void()> resume)
{
    if (!m_sizeMapHideIgnored || !m_sizeMapHideIgnored->isChecked())
        return true;
    if (m_sizeMapIgnoredFresh && m_sizeMapIgnoredPath == path)
        return true;
    if (resume)
        m_sizeMapIgnoredResumes.append(std::move(resume));
    if (m_sizeMapIgnoredReading)
        return false;
    m_sizeMapIgnoredReading = true;
    if (m_sizeMapStatus)
        m_sizeMapStatus->setText(
            QStringLiteral("Scanning %1 …").arg(QDir::toNativeSeparators(path)));
    runOffThread<QSet<QString>>(
        [path] { return readSizeMapIgnoredPaths(path); },
        [this, path](QSet<QString> ignored) {
            m_sizeMapIgnoredReading = false;
            m_sizeMapIgnoredPath = path;
            m_sizeMapIgnoredPaths = std::move(ignored);
            const QList<std::function<void()>> resumes =
                std::exchange(m_sizeMapIgnoredResumes, {});
            m_sizeMapIgnoredFresh = true;
            for (const std::function<void()> &resume : resumes)
                resume();
            m_sizeMapIgnoredFresh = false;
        });
    return false;
}

QSet<QString> MainWindow::sizeMapPrunedPaths(const QString &path) const
{
    QSet<QString> pruned;
    if (m_sizeMapHideIgnored && m_sizeMapHideIgnored->isChecked() &&
        m_sizeMapIgnoredPath == path)
        pruned = m_sizeMapIgnoredPaths;
    const QString ownMount = QDir::cleanPath(QStorageInfo(path).rootPath());
    QSet<QString> mounts = forkmesh::systemMountPoints();
    for (const QStorageInfo &volume : QStorageInfo::mountedVolumes())
        mounts.insert(QDir::cleanPath(volume.rootPath()));
    for (const QString &mount : std::as_const(mounts)) {
        if (mount.isEmpty() || mount == ownMount)
            continue;
        pruned.insert(mount); // only matches if it sits inside the scan
    }
    return pruned;
}

void MainWindow::applySizeMapResult(const QString &path,
                                    DirectorySizeScanResult result,
                                    bool hideIgnored, bool elevated)
{
    if (!m_sizeMapStatus)
        return;
    m_sizeMapScannedPath = path;
    QString status =
        QStringLiteral("%1 files · %2 on disk (%3)")
            .arg(QLocale().toString(result.root.fileCount),
                 QLocale().formattedDataSize(result.root.size),
                 hideIgnored ? QStringLiteral(".git & .gitignored excluded")
                             : QStringLiteral(".git excluded"));
    const bool blocked = result.unreadableDirs > 0;
    if (blocked) {
        status += QStringLiteral(" · %1 folder%2 could not be read (%3%4)")
                      .arg(QLocale().toString(result.unreadableDirs),
                           result.unreadableDirs == 1 ? QString()
                                                      : QStringLiteral("s"),
                           result.unreadableSample.join(QStringLiteral(", ")),
                           result.unreadableSample.size() < result.unreadableDirs
                               ? QStringLiteral(", …")
                               : QString());
        status += elevated || forkmesh::runningAsRoot()
                      ? QStringLiteral(" — their contents are not counted.")
                      : QStringLiteral(
                            " — scan as administrator to include them.");
    } else if (elevated) {
        status += QStringLiteral(" · measured with administrator access");
    }
    m_sizeMapStatus->setText(status);
    if (m_sizeMapElevate)
        m_sizeMapElevate->setVisible(blocked && !elevated &&
                                     !forkmesh::runningAsRoot());
    if (auto *liveChart = static_cast<RepoSunburstChart *>(m_sizeMapChart)) {
        liveChart->setBasePath(path);
        liveChart->setRoot(std::move(result.root));
    }
}

void MainWindow::rescanSizeMapElevated(bool upfront)
{
    if (m_sizeMapScanning || !m_sizeMapStatus)
        return;
    const QString path = sizeMapRoot();
    if (path.isEmpty() || !QDir(path).exists())
        return;
    if (!sizeMapIgnoredPathsReady(
            path, [this, upfront] { rescanSizeMapElevated(upfront); }))
        return;
    const bool hideIgnored =
        m_sizeMapHideIgnored && m_sizeMapHideIgnored->isChecked();

    DirectorySizeScanOptions options;
    options.pruned = sizeMapPrunedPaths(path);
    auto *request = new QTemporaryFile(
        QDir::temp().filePath(QStringLiteral("forkmesh-sizemap-XXXXXX.json")));
    if (!request->open()) {
        delete request;
        m_sizeMapStatus->setText(
            QStringLiteral("Could not prepare the elevated scan (no writable "
                           "temporary directory)."));
        if (upfront)
            refreshSizeMapTab(true); // still size what this user can read
        return;
    }
    request->write(forkmesh::encodeScanRequest(path, options));
    request->flush();
    const QString requestPath = request->fileName();

    const QString helper = QCoreApplication::applicationFilePath();
    const QString pkexec =
        QStandardPaths::findExecutable(QStringLiteral("pkexec"));
    QString program;
    QStringList arguments;
    QByteArray stdinPayload;
    if (!pkexec.isEmpty()) {
        program = pkexec;
        arguments = {helper, QStringLiteral("--size-map-scan"), requestPath};
    } else {
        bool accepted = false;
        const QString password = QInputDialog::getText(
            this, QStringLiteral("Administrator password"),
            QStringLiteral("Enter the root password for sudo, so %1 can be "
                           "measured in full — including the folders this user "
                           "cannot read:")
                .arg(QDir::toNativeSeparators(path)),
            QLineEdit::Password, QString(), &accepted);
        if (!accepted) {
            delete request;
            if (upfront)
                refreshSizeMapTab(true);
            return;
        }
        program = QStringLiteral("sudo");
        arguments = {QStringLiteral("-S"), QStringLiteral("-p"), QString(),
                     QStringLiteral("--"), helper,
                     QStringLiteral("--size-map-scan"), requestPath};
        stdinPayload = password.toUtf8() + '\n';
    }

    m_sizeMapScanning = true;
    if (m_sizeMapStop)
        m_sizeMapStop->setVisible(true);
    clearSizeMapWorkerLines();
    const int epoch = ++m_sizeMapScanEpoch;
    if (m_sizeMapElevate)
        m_sizeMapElevate->setVisible(false);
    m_sizeMapStatus->setText(
        QStringLiteral("Scanning %1 with administrator access …")
            .arg(QDir::toNativeSeparators(path)));

    auto *process = new QProcess(this);
    m_sizeMapElevatedProcess = process;
    request->setParent(process); // the temp file dies with the process
    process->setProgram(program);
    process->setArguments(arguments);
    auto pending = std::make_shared<QByteArray>();
    connect(process, &QProcess::readyReadStandardError, this,
            [this, process, pending, epoch, path] {
                pending->append(process->readAllStandardError());
                for (int cut = pending->indexOf('\n'); cut >= 0;
                     cut = pending->indexOf('\n')) {
                    const QByteArray line = pending->left(cut);
                    pending->remove(0, cut + 1);
                    forkmesh::DirectorySizeScanProgressUpdate update;
                    if (!forkmesh::decodeScanProgress(line, &update))
                        continue;
                    if (epoch != m_sizeMapScanEpoch)
                        continue;
                    showSizeMapScanProgress(path, update, true);
                }
                if (pending->size() > 64 * 1024)
                    pending->clear();
            });
    connect(process, &QProcess::finished, this,
            [this, process, path, epoch, hideIgnored, upfront](
                int exitCode, QProcess::ExitStatus) {
                process->deleteLater();
                if (m_sizeMapElevatedProcess == process)
                    m_sizeMapElevatedProcess = nullptr;
                m_sizeMapScanning = false;
                if (m_sizeMapStop)
                    m_sizeMapStop->setVisible(false);
                clearSizeMapWorkerLines();
                if (epoch != m_sizeMapScanEpoch)
                    return; // a newer scan superseded this one
                const QByteArray payload = process->readAllStandardOutput();
                DirectorySizeScanResult result;
                if (exitCode != 0 ||
                    !forkmesh::decodeScanResult(payload, &result)) {
                    if (upfront) {
                        refreshSizeMapTab(true);
                        return;
                    }
                    m_sizeMapStatus->setText(
                        exitCode == 126 || exitCode == 127
                            ? QStringLiteral(
                                  "Administrator access was declined — the map "
                                  "still shows only the readable folders.")
                            : QStringLiteral(
                                  "The elevated scan failed — the map still "
                                  "shows only the readable folders."));
                    if (m_sizeMapElevate)
                        m_sizeMapElevate->setVisible(true);
                    return;
                }
                if (sizeMapRoot() != path) {
                    refreshSizeMapTab(false);
                    return;
                }
                applySizeMapResult(path, std::move(result), hideIgnored, true);
            });
    connect(process, &QProcess::errorOccurred, this,
            [this, process, epoch, upfront](QProcess::ProcessError error) {
                if (error != QProcess::FailedToStart)
                    return;
                process->deleteLater();
                if (m_sizeMapElevatedProcess == process)
                    m_sizeMapElevatedProcess = nullptr;
                m_sizeMapScanning = false;
                if (m_sizeMapStop)
                    m_sizeMapStop->setVisible(false);
                if (epoch != m_sizeMapScanEpoch || !m_sizeMapStatus)
                    return;
                if (upfront) {
                    refreshSizeMapTab(true);
                    return;
                }
                m_sizeMapStatus->setText(QStringLiteral(
                    "No way to ask for administrator access on this machine "
                    "(neither pkexec nor sudo could be started)."));
            });
    process->start();
    if (!stdinPayload.isEmpty()) {
        process->write(stdinPayload);
        stdinPayload.fill('\0');
    }
    process->closeWriteChannel();
}

void MainWindow::stopSizeMapScan()
{
    if (m_sizeMapWatcher) {
        disconnect(m_sizeMapWatcher, nullptr, this, nullptr);
        m_sizeMapWatcher->cancel();
        m_sizeMapWatcher->deleteLater();
        m_sizeMapWatcher = nullptr;
    }
    if (m_sizeMapElevatedProcess) {
        disconnect(m_sizeMapElevatedProcess, nullptr, this, nullptr);
        m_sizeMapElevatedProcess->kill();
        m_sizeMapElevatedProcess->deleteLater();
        m_sizeMapElevatedProcess = nullptr;
    }
    m_sizeMapScanning = false;
    ++m_sizeMapScanEpoch; // discards any progress update already queued
    if (m_sizeMapStop)
        m_sizeMapStop->setVisible(false);
    clearSizeMapWorkerLines();
    if (m_sizeMapStatus)
        m_sizeMapStatus->setText(QStringLiteral("Scan stopped."));
}

QWidget *MainWindow::buildInsightsTab()
{
    auto *page = new QWidget;
    page->setObjectName("mainContent");

    auto *scroll = new QScrollArea;
    scroll->setObjectName("mainContent");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto *content = new QWidget;
    content->setObjectName("insightsPage");
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(18, 18, 18, 18);
    layout->setSpacing(14);

    auto *heading = new QLabel("Insights");
    heading->setObjectName("channelTitle");
    auto *subtitle = new QLabel(
        "Local repository and ForkMesh traffic metrics. No data leaves this node.");
    subtitle->setObjectName("statusLine");
    subtitle->setWordWrap(true);

    m_insightsRefreshButton = new QPushButton("Refresh");
    m_insightsRefreshButton->setObjectName("ghostButton");
    m_insightsRefreshButton->setProperty("buttonSize", "sm");
    m_insightsRefreshButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_insightsRefreshButton, "sync", 16);
    connect(m_insightsRefreshButton, &QPushButton::clicked, this,
            &MainWindow::loadRepoInsights);
    addRefreshSpin(m_insightsRefreshButton);

    auto *headingCol = new QVBoxLayout;
    headingCol->setContentsMargins(0, 0, 0, 0);
    headingCol->setSpacing(3);
    headingCol->addWidget(heading);
    headingCol->addWidget(subtitle);

    auto *headerRow = new QHBoxLayout;
    headerRow->setContentsMargins(0, 0, 0, 0);
    headerRow->setSpacing(8);
    headerRow->addLayout(headingCol, 1);
    headerRow->addWidget(m_insightsRefreshButton, 0, Qt::AlignTop);
    layout->addLayout(headerRow);

    m_insightsSummary = new QLabel;
    m_insightsSummary->setObjectName("insightsCard");
    m_insightsSummary->setTextFormat(Qt::RichText);
    m_insightsSummary->setWordWrap(true);
    m_insightsSummary->setMinimumHeight(110);

    m_insightsTraffic = new QLabel;
    m_insightsTraffic->setObjectName("insightsCard");
    m_insightsTraffic->setTextFormat(Qt::RichText);
    m_insightsTraffic->setWordWrap(true);
    m_insightsTraffic->setMinimumHeight(110);

    auto *summaryRow = new QHBoxLayout;
    summaryRow->setContentsMargins(0, 0, 0, 0);
    summaryRow->setSpacing(10);
    summaryRow->addWidget(m_insightsSummary, 2);
    summaryRow->addWidget(m_insightsTraffic, 1);
    layout->addLayout(summaryRow);

    auto *languageLabel = new QLabel("LANGUAGES");
    languageLabel->setObjectName("sectionLabel");
    m_insightsLanguageBar = new QLabel;
    m_insightsLanguageBar->setObjectName("langBar");
    m_insightsLanguageBar->setFixedHeight(8);
    m_insightsLanguageBar->setTextFormat(Qt::RichText);
    m_insightsLanguageLegend = new QLabel;
    m_insightsLanguageLegend->setObjectName("statusLine");
    m_insightsLanguageLegend->setTextFormat(Qt::RichText);
    m_insightsLanguageLegend->setWordWrap(true);
    layout->addWidget(languageLabel);
    layout->addWidget(m_insightsLanguageBar);
    layout->addWidget(m_insightsLanguageLegend);

    auto configureTable = [](QTableWidget *table) {
        table->setObjectName("issueTable");
        enableHoverRowHighlight(table);
        table->verticalHeader()->setVisible(false);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->setShowGrid(false);
        table->setWordWrap(false);
        table->setSortingEnabled(false);
        table->horizontalHeader()->setHighlightSections(false);
    };

    auto *contributorsLabel = new QLabel("CONTRIBUTORS & ACTIVITY");
    contributorsLabel->setObjectName("sectionLabel");

    m_insightsRangeCombo = new QComboBox;
    m_insightsRangeCombo->setObjectName("ghostCombo");
    m_insightsRangeCombo->setCursor(Qt::PointingHandCursor);
    m_insightsRangeCombo->addItem("All time", 0);
    m_insightsRangeCombo->addItem("Last 12 months", 365);
    m_insightsRangeCombo->addItem("Last 90 days", 90);
    m_insightsRangeCombo->addItem("Last 30 days", 30);
    m_insightsRangeCombo->addItem("Last 7 days", 7);
    connect(m_insightsRangeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this] { loadRepoInsights(); });

    auto *contributorsHeader = new QHBoxLayout;
    contributorsHeader->setContentsMargins(0, 0, 0, 0);
    contributorsHeader->setSpacing(8);
    contributorsHeader->addWidget(contributorsLabel, 1, Qt::AlignBottom);
    contributorsHeader->addWidget(new QLabel("Activity range:"), 0, Qt::AlignVCenter);
    contributorsHeader->addWidget(m_insightsRangeCombo, 0, Qt::AlignVCenter);

    m_insightsActivityAxis = new QLabel;
    m_insightsActivityAxis->setObjectName("statusLine");
    m_insightsActivityAxis->setTextFormat(Qt::RichText);
    m_insightsActivityAxis->setWordWrap(true);

    m_insightsActivity = new QLabel;
    m_insightsActivity->setObjectName("statusLine");
    m_insightsActivity->setTextFormat(Qt::RichText);
    m_insightsActivity->setWordWrap(true);

    m_insightsContributors = new QTableWidget(0, 7);
    installColumnHeaderMenu(m_insightsContributors);
    m_insightsContributors->setHorizontalHeaderLabels(
        {"Contributor", "Email", "Commits", "Share", "First commit", "Last commit",
         "Activity"});
    configureTable(m_insightsContributors);
    for (int column = 0; column < 6; ++column)
        m_insightsContributors->horizontalHeader()->setSectionResizeMode(
            column, QHeaderView::ResizeToContents);
    m_insightsContributors->horizontalHeader()->setSectionResizeMode(
        6, QHeaderView::Stretch);
    makeColumnsResizable(m_insightsContributors);
    m_insightsContributors->verticalHeader()->setDefaultSectionSize(34);
    m_insightsContributors->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    connect(m_insightsContributors->horizontalScrollBar(), &QScrollBar::rangeChanged,
            this, &MainWindow::fitInsightsContributorsHeight);
    m_insightsContributors->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_insightsContributors, &QWidget::customContextMenuRequested, this,
            &MainWindow::showInsightsContributorMenu);
    connect(m_insightsContributors, &QTableWidget::cellClicked, this,
            [this](int row, int column) {
                if (column != 0 && column != 2)
                    return;
                QTableWidgetItem *nameItem = m_insightsContributors->item(row, 0);
                if (!nameItem)
                    return;
                const QString name = nameItem->data(Qt::UserRole).toString().isEmpty()
                                         ? nameItem->text()
                                         : nameItem->data(Qt::UserRole).toString();
                openCommitsForContributor(name);
            });

    layout->addLayout(contributorsHeader);
    layout->addWidget(m_insightsActivityAxis);
    layout->addWidget(m_insightsActivity);
    layout->addWidget(m_insightsContributors);
    layout->addStretch();

    scroll->setWidget(content);
    auto *pageLayout = new QVBoxLayout(page);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    pageLayout->addWidget(scroll);
    return page;
}

QWidget *MainWindow::buildPlaceholderTab(const QString &name)
{
    auto *page = new QWidget;
    auto *label = new QLabel(
        QStringLiteral("<b>%1</b><br><span style='color:#8b949e'>Not available in "
                       "ForkMesh yet.</span>")
            .arg(name));
    label->setObjectName("placeholderPanel");
    label->setAlignment(Qt::AlignCenter);
    label->setTextFormat(Qt::RichText);
    auto *layout = new QVBoxLayout(page);
    layout->addStretch();
    layout->addWidget(label, 0, Qt::AlignCenter);
    layout->addStretch();
    return page;
}
