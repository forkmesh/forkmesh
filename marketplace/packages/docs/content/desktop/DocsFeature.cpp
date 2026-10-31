#include "DocsFeature.h"

#include "DesignTokens.h"
#include "MarkdownDocument.h"
#include "MarkdownRender.h"

#include <QAbstractTextDocumentLayout>
#include <QDesktopServices>
#include <QEvent>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QFontInfo>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMargins>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextTable>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QUrl>
#include <QVBoxLayout>

#include <utility>

namespace forkmesh::docs {
namespace {

constexpr int kPathRole = Qt::UserRole;
constexpr int kPositionRole = Qt::UserRole + 1;
constexpr int kNavigationResultLimit = 200;
constexpr int kPageMargin = 20;
constexpr int kNavigationWidth = 260;
constexpr int kOutlineWidth = 200;
constexpr int kReaderWidthForOutline = 560;
constexpr int kReaderMaximumWidth = 900;
constexpr int kNavigationRowHeight = 30;
constexpr int kNavigationIndentation = 14;
constexpr int kPagerMinimumWidth = 64;
constexpr int kDocumentMargin = 28;
constexpr int kClassicDocumentMargin = 4;
constexpr qreal kTableWidthPercent = 99.75;
constexpr int kScrollSlack = 16;

QLabel *mutedLabel(const QString &text)
{
    auto *label = new QLabel(text);
    label->setObjectName(QStringLiteral("mutedLabel"));
    label->setWordWrap(true);
    return label;
}

QPushButton *ghostButton(const QString &text, const QString &tip)
{
    auto *button = new QPushButton(text);
    button->setObjectName(QStringLiteral("ghostButton"));
    button->setCursor(Qt::PointingHandCursor);
    button->setToolTip(tip);
    return button;
}

QString elided(const QString &text, int limit)
{
    return text.size() <= limit ? text : text.left(limit - 1).trimmed() + QChar(0x2026);
}

class NavigationDelegate final : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QSize size = QStyledItemDelegate::sizeHint(option, index);
        if (Theme::isNext())
            size.setHeight(kNavigationRowHeight);
        return size;
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        if (!Theme::isNext()) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }
        const bool group = index.model()->hasChildren(index);
        const bool enabled = index.flags().testFlag(Qt::ItemIsEnabled);
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        const bool hovered = enabled && option.state.testFlag(QStyle::State_MouseOver);
        QString label = index.data(Qt::DisplayRole).toString();
        QString count;
        const int open = label.lastIndexOf(QStringLiteral(" ("));
        if (group && open > 0 && label.endsWith(QLatin1Char(')'))) {
            count = label.mid(open + 2, label.size() - open - 3);
            label.truncate(open);
        }

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        if (selected || hovered) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(Theme::color(selected ? Theme::Role::SurfaceActive
                                                    : Theme::Role::SurfaceRaised));
            painter->drawRoundedRect(QRectF(option.rect).adjusted(0, 1, -2, -1), 6, 6);
        }
        QRect textRect = option.rect.adjusted(8, 0, -10, 0);
        QFont font = option.font;
        font.setBold(false);
        if (!count.isEmpty()) {
            QFont countFont = font;
            countFont.setWeight(QFont::Normal);
            if (countFont.pixelSize() > 11)
                countFont.setPixelSize(countFont.pixelSize() - 1);
            painter->setFont(countFont);
            painter->setPen(Theme::color(Theme::Role::FgMuted));
            painter->drawText(textRect, Qt::AlignVCenter | Qt::AlignRight, count);
            textRect.setRight(textRect.right() - QFontMetrics(countFont).horizontalAdvance(count) - 8);
        }
        font.setWeight(group ? QFont::DemiBold : QFont::Normal);
        painter->setFont(font);
        Theme::Role role = Theme::Role::FgMuted;
        if (!enabled)
            role = Theme::Role::FgSubtle;
        else if (selected || hovered)
            role = Theme::Role::Fg;
        else if (group)
            role = Theme::Role::FgSecondary;
        painter->setPen(Theme::color(role));
        painter->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft,
                          QFontMetrics(font).elidedText(label, Qt::ElideRight, textRect.width()));
        painter->restore();
    }
};

class OutlineDelegate final : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        if (!Theme::isNext())
            return QStyledItemDelegate::sizeHint(option, index);
        const Entry entry = entryFor(index);
        const int width = qMax(40, viewWidth(option) - kHorizontalPadding * 2 - entry.indent);
        const QRect bounds = QFontMetrics(option.font)
                                 .boundingRect(QRect(0, 0, width, 4000),
                                               Qt::TextWordWrap | Qt::AlignLeft, entry.text);
        return QSize(viewWidth(option), bounds.height() + kVerticalPadding * 2);
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        if (!Theme::isNext()) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }
        const Entry entry = entryFor(index);
        const bool enabled = index.flags().testFlag(Qt::ItemIsEnabled);
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        const bool hovered = enabled && option.state.testFlag(QStyle::State_MouseOver);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        if (selected || hovered) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(Theme::color(selected ? Theme::Role::SurfaceActive
                                                    : Theme::Role::SurfaceRaised));
            painter->drawRoundedRect(QRectF(option.rect).adjusted(0, 1, 0, -1), 6, 6);
        }
        Theme::Role role = Theme::Role::FgMuted;
        if (!enabled)
            role = Theme::Role::FgSubtle;
        else if (selected || hovered)
            role = Theme::Role::Fg;
        painter->setFont(option.font);
        painter->setPen(Theme::color(role));
        painter->drawText(option.rect.adjusted(kHorizontalPadding + entry.indent, kVerticalPadding,
                                               -kHorizontalPadding, -kVerticalPadding),
                          Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignTop, entry.text);
        painter->restore();
    }

private:
    struct Entry {
        QString text;
        int indent = 0;
    };

    static constexpr int kHorizontalPadding = 8;
    static constexpr int kVerticalPadding = 5;

    static Entry entryFor(const QModelIndex &index)
    {
        const QString raw = index.data(Qt::DisplayRole).toString();
        int spaces = 0;
        while (spaces < raw.size() && raw.at(spaces) == QLatin1Char(' '))
            ++spaces;
        return {raw.mid(spaces), spaces * 4};
    }

    static int viewWidth(const QStyleOptionViewItem &option)
    {
        if (const auto *view = qobject_cast<const QAbstractItemView *>(option.widget))
            return view->viewport()->width();
        return option.rect.width();
    }
};

bool isCodeBlock(const QTextBlockFormat &format)
{
    return format.hasProperty(QTextFormat::BlockCodeFence) || format.nonBreakableLines();
}

void styleNextDocument(QTextBrowser *view)
{
    QTextDocument *doc = view->document();
    const QColor strong = Theme::color(Theme::Role::Fg);
    const QColor muted = Theme::color(Theme::Role::FgMuted);
    const QColor panel = Theme::color(Theme::Role::SurfaceRaised);
    const QColor hairline = Theme::color(Theme::Role::Border);
    const int codeSize = qMax(11, QFontInfo(view->font()).pixelSize() - 1);

    QTextCharFormat codeText;
    codeText.setFontFamilies(Theme::monospaceFont(
                                 QFontDatabase::systemFont(QFontDatabase::FixedFont))
                                 .families());
    codeText.setFontFixedPitch(true);
    codeText.setProperty(QTextFormat::FontPixelSize, codeSize);
    QTextCharFormat inlineCode = codeText;
    inlineCode.setBackground(panel);
    QTextCharFormat headingText;
    headingText.setFontWeight(QFont::DemiBold);
    headingText.setForeground(strong);
    QTextCharFormat tableHeading;
    tableHeading.setFontWeight(QFont::Medium);
    tableHeading.setForeground(muted);

    struct CodeRun {
        int start = 0;
        int end = 0;
        QTextBlockFormat format;
        bool framed = false;
    };
    QList<CodeRun> runs;
    QList<QPair<int, int>> spans;
    QTextFrame *runFrame = nullptr;
    bool inRun = false;
    QTextCursor cursor(doc);
    cursor.beginEditBlock();
    const QList<QTextFrame *> frames = doc->rootFrame()->childFrames();
    for (QTextFrame *frame : frames) {
        auto *table = qobject_cast<QTextTable *>(frame);
        if (!table)
            continue;
        for (int row = 0; row < table->rows(); ++row) {
            for (int column = 0; column < table->columns(); ++column) {
                const QTextTableCell cell = table->cellAt(row, column);
                const QTextBlock first = doc->findBlock(cell.firstPosition());
                if (first.length() != 1 || first.position() >= cell.lastPosition())
                    continue;
                cursor.setPosition(first.position());
                cursor.deleteChar();
            }
        }
    }
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
        QTextBlockFormat format = block.blockFormat();
        const int start = block.position();
        const int end = start + block.length() - 1;
        if (isCodeBlock(format)) {
            QTextFrame *frame = QTextCursor(block).currentFrame();
            format.setNonBreakableLines(false);
            format.clearBackground();
            format.setLeftMargin(0);
            format.setRightMargin(0);
            format.setTopMargin(0);
            format.setBottomMargin(0);
            format.setLineHeight(135, QTextBlockFormat::ProportionalHeight);
            cursor.setPosition(start);
            cursor.setBlockFormat(format);
            cursor.setPosition(end, QTextCursor::KeepAnchor);
            cursor.mergeCharFormat(codeText);
            if (inRun && frame == runFrame)
                runs.last().end = end;
            else
                runs.append({start, end, format, frame == doc->rootFrame()});
            inRun = true;
            runFrame = frame;
            continue;
        }
        inRun = false;
        const int level = format.headingLevel();
        if (level > 0) {
            format.setTopMargin(block.previous().isValid() ? (level <= 2 ? 28 : 20) : 0);
            format.setBottomMargin(level == 1 ? 14 : 8);
            cursor.setPosition(start);
            cursor.setBlockFormat(format);
            cursor.setPosition(end, QTextCursor::KeepAnchor);
            cursor.mergeCharFormat(headingText);
            continue;
        }
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (fragment.isValid() && fragment.charFormat().fontFixedPitch())
                spans.append({fragment.position(), fragment.length()});
        }
    }
    for (const QPair<int, int> &span : std::as_const(spans)) {
        cursor.setPosition(span.first);
        cursor.setPosition(span.first + span.second, QTextCursor::KeepAnchor);
        cursor.mergeCharFormat(inlineCode);
    }

    for (QTextFrame *frame : frames) {
        auto *table = qobject_cast<QTextTable *>(frame);
        if (!table)
            continue;
        QTextTableFormat tableFormat = table->format();
        tableFormat.setBorder(0);
        tableFormat.setBorderCollapse(true);
        tableFormat.setCellPadding(8);
        tableFormat.setCellSpacing(0);
        tableFormat.setWidth(QTextLength(QTextLength::PercentageLength, kTableWidthPercent));
        tableFormat.setTopMargin(4);
        tableFormat.setBottomMargin(14);
        table->setFormat(tableFormat);
        for (int row = 0; row < table->rows(); ++row) {
            for (int column = 0; column < table->columns(); ++column) {
                QTextTableCell cell = table->cellAt(row, column);
                QTextTableCellFormat cellFormat;
                cellFormat.setBottomBorder(1);
                cellFormat.setBottomBorderStyle(QTextFrameFormat::BorderStyle_Solid);
                cellFormat.setBottomBorderBrush(hairline);
                cell.setFormat(cellFormat);
                if (row > 0)
                    continue;
                cursor.setPosition(cell.firstCursorPosition().position());
                cursor.setPosition(cell.lastCursorPosition().position(), QTextCursor::KeepAnchor);
                cursor.mergeCharFormat(tableHeading);
            }
        }
    }

    QTextFrameFormat box;
    box.setMargin(0);
    box.setTopMargin(2);
    box.setBottomMargin(12);
    box.setPadding(12);
    box.setBorder(1);
    box.setBorderStyle(QTextFrameFormat::BorderStyle_Solid);
    box.setBorderBrush(hairline);
    box.setBackground(panel);
    QTextBlockFormat gap;
    gap.setLineHeight(1, QTextBlockFormat::FixedHeight);
    for (int i = runs.size() - 1; i >= 0; --i) {
        const CodeRun &run = runs.at(i);
        cursor.setPosition(run.start);
        cursor.setPosition(run.end, QTextCursor::KeepAnchor);
        QTextFrame *frame = run.framed ? cursor.insertFrame(box) : nullptr;
        if (!frame) {
            QTextBlockFormat filled;
            filled.setBackground(panel);
            cursor.setPosition(run.start);
            cursor.setPosition(run.end, QTextCursor::KeepAnchor);
            cursor.mergeBlockFormat(filled);
            continue;
        }
        cursor.setPosition(frame->firstPosition());
        cursor.setBlockFormat(run.format);
        cursor.setPosition(frame->firstPosition() - 1);
        cursor.setBlockFormat(gap);
        cursor.setPosition(frame->lastPosition() + 1);
        cursor.setBlockFormat(gap);
    }
    cursor.endEditBlock();
}

}

DocsFeature::DocsFeature(const forkmesh::market::NativePackageHost &host, QWidget *parent)
    : forkmesh::market::NativePackageWidget(parent), m_packageId(host.packageId)
{
    setObjectName(QStringLiteral("docsSection"));
    buildPage();
    syncDesign();
}

void DocsFeature::buildPage()
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(kPageMargin, 18, kPageMargin, kPageMargin);
    outer->setSpacing(10);

    auto *heading = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("Docs"));
    title->setObjectName(QStringLiteral("pageTitle"));
    heading->addWidget(title);
    m_summary = mutedLabel(QString());
    heading->addWidget(m_summary, 1);
    m_backButton = ghostButton(QStringLiteral("Back"), QStringLiteral("Previous document (Alt+Left)"));
    m_forwardButton =
        ghostButton(QStringLiteral("Forward"), QStringLiteral("Next document in your history (Alt+Right)"));
    auto *homeButton =
        ghostButton(QStringLiteral("Library home"), QStringLiteral("Open the library overview"));
    m_webButton = ghostButton(QStringLiteral("Open on web"),
                              QStringLiteral("Open this document on forkmesh.com/docs"));
    for (QPushButton *button : {m_backButton, m_forwardButton, homeButton, m_webButton})
        heading->addWidget(button);
    outer->addLayout(heading);
    connect(m_backButton, &QPushButton::clicked, this, [this] { goBack(); });
    connect(m_forwardButton, &QPushButton::clicked, this, [this] { goForward(); });
    connect(homeButton, &QPushButton::clicked, this, [this] { openPage(m_library.homePath()); });
    connect(m_webButton, &QPushButton::clicked, this, [this] {
        const QString url = DocsLibrary::webUrl(m_current);
        if (!url.isEmpty())
            QDesktopServices::openUrl(QUrl(url));
    });
    auto *backShortcut = new QShortcut(QKeySequence(Qt::ALT | Qt::Key_Left), this);
    backShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(backShortcut, &QShortcut::activated, this, [this] { goBack(); });
    auto *forwardShortcut = new QShortcut(QKeySequence(Qt::ALT | Qt::Key_Right), this);
    forwardShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(forwardShortcut, &QShortcut::activated, this, [this] { goForward(); });

    auto *splitter = new QSplitter(Qt::Horizontal);
    splitter->setObjectName(QStringLiteral("docsSplitter"));
    m_splitter = splitter;

    auto *navigationPane = new QWidget;
    auto *navigationLayout = new QVBoxLayout(navigationPane);
    navigationLayout->setContentsMargins(0, 0, 0, 0);
    navigationLayout->setSpacing(8);
    m_filterEdit = new QLineEdit;
    m_filterEdit->setObjectName(QStringLiteral("docsFilter"));
    m_filterEdit->setPlaceholderText(QStringLiteral("Search the docs"));
    m_filterEdit->setClearButtonEnabled(true);
    navigationLayout->addWidget(m_filterEdit);
    m_navigation = new QTreeWidget;
    m_navigation->setObjectName(QStringLiteral("docsNavigation"));
    m_navigation->setHeaderHidden(true);
    m_navigation->setColumnCount(1);
    m_navigation->setUniformRowHeights(true);
    m_navigation->setSelectionMode(QAbstractItemView::SingleSelection);
    m_navigation->header()->setStretchLastSection(true);
    m_navigation->setItemDelegate(new NavigationDelegate(m_navigation));
    m_classicIndentation = m_navigation->indentation();
    m_classicHover = m_navigation->viewport()->testAttribute(Qt::WA_Hover);
    m_classicTracking = m_navigation->hasMouseTracking();
    navigationLayout->addWidget(m_navigation, 1);
    navigationPane->setMinimumWidth(220);
    splitter->addWidget(navigationPane);
    connect(m_filterEdit, &QLineEdit::textChanged, this, [this](const QString &text) {
        m_filter = text.trimmed();
        renderNavigation();
    });
    connect(m_filterEdit, &QLineEdit::returnPressed, this, [this] {
        for (int i = 0; i < m_navigation->topLevelItemCount(); ++i) {
            const QString path = m_navigation->topLevelItem(i)->data(0, kPathRole).toString();
            if (!path.isEmpty()) {
                openPage(path);
                return;
            }
        }
    });
    connect(m_navigation, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem *item) {
                const QString path = item ? item->data(0, kPathRole).toString() : QString();
                if (!path.isEmpty() && path != m_current)
                    openPage(path);
            });
    connect(m_navigation, &QTreeWidget::itemClicked, this, [](QTreeWidgetItem *item) {
        if (item->childCount() > 0)
            item->setExpanded(!item->isExpanded());
    });

    auto *readerPane = new QWidget;
    readerPane->setProperty("docsPane", QStringLiteral("reader"));
    readerPane->installEventFilter(this);
    m_readerPane = readerPane;
    auto *readerLayout = new QVBoxLayout(readerPane);
    readerLayout->setContentsMargins(0, 0, 0, 0);
    readerLayout->setSpacing(6);
    m_breadcrumb = mutedLabel(QString());
    m_breadcrumb->setObjectName(QStringLiteral("docsBreadcrumb"));
    m_breadcrumb->setTextInteractionFlags(Qt::TextSelectableByMouse);
    readerLayout->addWidget(m_breadcrumb);
    m_view = new QTextBrowser;
    m_view->setObjectName(QStringLiteral("docsView"));
    m_view->setOpenLinks(false);
    m_view->setOpenExternalLinks(false);
    readerLayout->addWidget(m_view, 1);
    connect(m_view, &QTextBrowser::anchorClicked, this, [this](const QUrl &url) { followLink(url); });
    connect(m_view->horizontalScrollBar(), &QScrollBar::rangeChanged, this, [this] {
        QTimer::singleShot(0, this, [this] { fitScrollBars(); });
    });
    m_backlinks = mutedLabel(QString());
    m_backlinks->setObjectName(QStringLiteral("docsBacklinks"));
    m_backlinks->setTextFormat(Qt::RichText);
    m_backlinks->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
    readerLayout->addWidget(m_backlinks);
    connect(m_backlinks, &QLabel::linkActivated, this, [this](const QString &path) { openPage(path); });
    auto *pager = new QHBoxLayout;
    m_previousButton = ghostButton(QString(), QStringLiteral("Previous document in the library"));
    m_nextButton = ghostButton(QString(), QStringLiteral("Next document in the library"));
    m_previousButton->setMinimumWidth(kPagerMinimumWidth);
    m_nextButton->setMinimumWidth(kPagerMinimumWidth);
    m_pager = pager;
    pager->addWidget(m_previousButton);
    pager->addStretch();
    pager->addWidget(m_nextButton);
    readerLayout->addLayout(pager);
    connect(m_previousButton, &QPushButton::clicked, this,
            [this] { openPage(m_library.previousPath(m_current)); });
    connect(m_nextButton, &QPushButton::clicked, this,
            [this] { openPage(m_library.nextPath(m_current)); });
    splitter->addWidget(readerPane);

    auto *outlinePane = new QWidget;
    m_outlinePane = outlinePane;
    auto *outlineLayout = new QVBoxLayout(outlinePane);
    outlineLayout->setContentsMargins(0, 0, 0, 0);
    outlineLayout->setSpacing(8);
    outlineLayout->addWidget(mutedLabel(QStringLiteral("On this page")));
    m_outline = new QListWidget;
    m_outline->setObjectName(QStringLiteral("docsOutline"));
    m_outline->setWordWrap(true);
    m_outline->setItemDelegate(new OutlineDelegate(m_outline));
    m_outlineHover = m_outline->viewport()->testAttribute(Qt::WA_Hover);
    m_outlineTracking = m_outline->hasMouseTracking();
    outlineLayout->addWidget(m_outline, 1);
    outlinePane->setMinimumWidth(160);
    splitter->addWidget(outlinePane);
    connect(m_outline, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        const QVariant position = item->data(kPositionRole);
        if (!position.isValid())
            return;
        const QTextBlock block = m_view->document()->findBlock(position.toInt());
        if (!block.isValid())
            return;
        const QRectF rect = m_view->document()->documentLayout()->blockBoundingRect(block);
        m_view->verticalScrollBar()->setValue(int(rect.top()));
    });

    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setStretchFactor(2, 0);
    splitter->setSizes({kNavigationWidth, 720, kOutlineWidth});
    outer->addWidget(splitter, 1);
    updateHistoryButtons();
}

void DocsFeature::load()
{
    if (m_loaded)
        return;
    m_loaded = true;
    const bool library = m_library.load(DocsLibrary::bundledRoot());
    const bool dependencies = m_library.loadDependencies(DocsLibrary::bundledDependencies());
    if (!library && !dependencies)
        m_summary->setText(QStringLiteral("This build does not include the documentation library."));
    renderNavigation();
}

void DocsFeature::refresh()
{
    load();
    if (!m_current.isEmpty() || m_library.isEmpty())
        return;
    const QString last = QSettings().value(lastPageKey()).toString();
    openPage(m_library.page(last) ? last : m_library.homePath(), QString(), false);
}

void DocsFeature::prepareSearch()
{
    load();
}

void DocsFeature::setHostSearchQuery(const QString &query)
{
    load();
    if (m_filterEdit->text() != query)
        m_filterEdit->setText(query);
}

QList<forkmesh::market::NativePackageSearchHit>
DocsFeature::searchHits(const QString &query, int limit) const
{
    QList<forkmesh::market::NativePackageSearchHit> hits;
    for (const DocsHit &hit : m_library.search(query, limit)) {
        const DocsPage *page = m_library.page(hit.path);
        const DocsArea *area = DocsLibrary::area(page->area);
        const QString where = area ? area->label : page->area;
        hits.append({hit.path, page->title,
                     hit.snippet.isEmpty() ? where
                                           : QStringLiteral("%1 · %2").arg(where, elided(hit.snippet, 120))});
    }
    return hits;
}

void DocsFeature::activateSearchHit(const QString &id)
{
    load();
    m_filterEdit->clear();
    openPage(id);
}

void DocsFeature::openPage(const QString &path, const QString &fragment, bool record)
{
    load();
    const DocsPage *page = m_library.page(path);
    if (!page)
        return;
    if (path == m_current) {
        if (!fragment.isEmpty())
            scrollToFragment(fragment);
        return;
    }
    if (record && !m_current.isEmpty()) {
        m_back.append({m_current, QString()});
        m_forward.clear();
    }
    m_current = path;
    QSettings().setValue(lastPageKey(), path);

    renderDocument(*page);
    m_view->verticalScrollBar()->setValue(0);

    renderBreadcrumb();
    const QString webUrl = DocsLibrary::webUrl(path);
    m_webButton->setToolTip(webUrl.isEmpty() ? QStringLiteral("This page is only in the desktop library")
                                             : webUrl);

    renderOutline();
    renderFooter();
    selectInNavigation();
    updateHistoryButtons();
    if (!fragment.isEmpty()) {
        const QString target = fragment;
        QTimer::singleShot(0, this, [this, path, target] {
            if (m_current == path)
                scrollToFragment(target);
        });
    }
}

void DocsFeature::renderDocument(const DocsPage &page)
{
    const QString directory =
        QFileInfo(DocsLibrary::bundledRoot() + QLatin1Char('/') + page.path).path();
    m_view->setSearchPaths({directory});
    m_view->setMarkdown(MarkdownRender::forPreview(page.markdown));
    forkmesh::ui::anchorReadmeHeadings(m_view->document());
    const int inset = m_next ? 2 * kDocumentMargin : 8;
    forkmesh::ui::fitReadmeImages(m_view, qMax(1, m_view->viewport()->width() - inset));
    forkmesh::ui::styleReadmeDocument(m_view);
    if (m_next)
        styleNextDocument(m_view);
}

void DocsFeature::renderBreadcrumb()
{
    const DocsPage *page = m_library.page(m_current);
    if (!page)
        return;
    const DocsArea *area = DocsLibrary::area(page->area);
    QStringList crumbs{QStringLiteral("Library"), area ? area->label : page->area};
    crumbs.append(DocsLibrary::repositoryPath(page->path));
    const int minutes = DocsLibrary::readingMinutes(page->words);
    if (minutes > 0 && !page->asset)
        crumbs.append(QStringLiteral("%1 min read").arg(minutes));
    if (!m_next) {
        m_breadcrumb->setTextFormat(Qt::AutoText);
        m_breadcrumb->setText(crumbs.join(QStringLiteral(" · ")));
        return;
    }
    const QString separator = QStringLiteral("<span style=\"color:%1\">&nbsp;&nbsp;/&nbsp;&nbsp;</span>")
                                  .arg(Theme::hex(Theme::Role::FgSubtle));
    const QString family =
        Theme::monospaceFont(QFontDatabase::systemFont(QFontDatabase::FixedFont)).families().value(0);
    const QString trail =
        crumbs.at(0).toHtmlEscaped() + separator + crumbs.at(1).toHtmlEscaped() + separator +
        QStringLiteral("<span style=\"font-family:'%1'\">%2</span>")
            .arg(family.toHtmlEscaped(), crumbs.at(2).toHtmlEscaped());
    const QString reading =
        crumbs.size() > 3
            ? QStringLiteral("<td align=\"right\" style=\"white-space:nowrap; padding-left:16px\">%1</td>")
                  .arg(crumbs.at(3).toHtmlEscaped())
            : QString();
    m_breadcrumb->setTextFormat(Qt::RichText);
    m_breadcrumb->setText(QStringLiteral("<table width=\"100%\" cellspacing=\"0\" cellpadding=\"0\">"
                                         "<tr><td>%1</td>%2</tr></table>")
                              .arg(trail, reading));
}

void DocsFeature::renderNavigation()
{
    const QSignalBlocker blocker(m_navigation);
    m_navigation->clear();
    if (m_library.isEmpty())
        return;
    if (!m_filter.isEmpty()) {
        const QList<DocsHit> hits = m_library.search(m_filter, kNavigationResultLimit);
        for (const DocsHit &hit : hits) {
            const DocsPage *page = m_library.page(hit.path);
            auto *item = new QTreeWidgetItem(m_navigation, {page->title});
            item->setData(0, kPathRole, page->path);
            item->setToolTip(0, QStringLiteral("%1\n\n%2").arg(DocsLibrary::repositoryPath(page->path),
                                                                  hit.snippet));
        }
        if (hits.isEmpty()) {
            auto *item = new QTreeWidgetItem(m_navigation, {QStringLiteral("No documents match")});
            item->setFlags(Qt::NoItemFlags);
        }
        m_summary->setText(QStringLiteral("%1 of %2 documents match \"%3\"")
                               .arg(hits.size())
                               .arg(m_library.pages().size())
                               .arg(m_filter));
        selectInNavigation();
        return;
    }
    const qsizetype dependencies = m_library.dependencies().size();
    const qsizetype documents = m_library.pages().size() - (dependencies > 0 ? 1 : 0);
    m_summary->setText(dependencies > 0
                           ? QStringLiteral("%1 documents from the ForkMesh library, the same pages as "
                                            "forkmesh.com/docs, and %2 dependencies")
                                 .arg(documents)
                                 .arg(dependencies)
                           : QStringLiteral("%1 documents from the ForkMesh library, the same pages as "
                                            "forkmesh.com/docs")
                                 .arg(documents));
    const DocsPage *current = m_library.page(m_current);
    for (const DocsArea &area : DocsLibrary::navigationAreas()) {
        const QList<const DocsPage *> pages = m_library.pagesIn(area.key);
        if (pages.isEmpty())
            continue;
        auto *group = new QTreeWidgetItem(
            m_navigation, {QStringLiteral("%1 (%2)").arg(area.label).arg(pages.size())});
        group->setFlags(Qt::ItemIsEnabled);
        group->setToolTip(0, area.blurb);
        QFont font = group->font(0);
        font.setBold(true);
        group->setFont(0, font);
        for (const DocsPage *page : pages) {
            auto *item = new QTreeWidgetItem(group, {page->title});
            item->setData(0, kPathRole, page->path);
            const QString source = DocsLibrary::repositoryPath(page->path);
            item->setToolTip(0, page->description.isEmpty()
                                    ? source
                                    : QStringLiteral("%1\n\n%2").arg(source, page->description));
        }
        group->setExpanded(current ? current->area == area.key : area.key == QLatin1String("guides"));
    }
    selectInNavigation();
}

void DocsFeature::selectInNavigation()
{
    const QSignalBlocker blocker(m_navigation);
    for (QTreeWidgetItemIterator it(m_navigation); *it; ++it) {
        if ((*it)->data(0, kPathRole).toString() != m_current)
            continue;
        if ((*it)->parent())
            (*it)->parent()->setExpanded(true);
        m_navigation->setCurrentItem(*it);
        m_navigation->scrollToItem(*it);
        return;
    }
    m_navigation->setCurrentItem(nullptr);
}

void DocsFeature::renderOutline()
{
    m_outline->clear();
    bool firstTitle = true;
    for (QTextBlock block = m_view->document()->begin(); block.isValid(); block = block.next()) {
        const int level = block.blockFormat().headingLevel();
        if (level <= 0 || level > 3 || block.text().trimmed().isEmpty())
            continue;
        if (level == 1 && firstTitle) {
            firstTitle = false;
            continue;
        }
        auto *item = new QListWidgetItem(QString(qMax(0, level - 2) * 3, QLatin1Char(' ')) +
                                         block.text().trimmed());
        item->setData(kPositionRole, block.position());
        item->setToolTip(block.text().trimmed());
        m_outline->addItem(item);
    }
    if (m_outline->count() == 0) {
        auto *item = new QListWidgetItem(QStringLiteral("No sections"));
        item->setFlags(Qt::NoItemFlags);
        m_outline->addItem(item);
    }
}

void DocsFeature::renderFooter()
{
    const DocsPage *page = m_library.page(m_current);
    if (!page)
        return;
    const QString linkStyle =
        m_next ? QStringLiteral(" style=\"color:%1; text-decoration:none\"")
                     .arg(Theme::hex(Theme::Role::AccentFg))
               : QString();
    QStringList links;
    for (const QString &source : page->backlinks) {
        if (const DocsPage *linking = m_library.page(source))
            links.append(QStringLiteral("<a href=\"%1\"%3>%2</a>")
                             .arg(source.toHtmlEscaped(), linking->title.toHtmlEscaped(), linkStyle));
    }
    m_backlinks->setVisible(!links.isEmpty());
    m_backlinks->setText(QStringLiteral("Pages that link here: ") + links.join(QStringLiteral(", ")));

    const DocsPage *previous = m_library.page(m_library.previousPath(m_current));
    const DocsPage *next = m_library.page(m_library.nextPath(m_current));
    m_previousButton->setVisible(previous != nullptr);
    m_nextButton->setVisible(next != nullptr);
    if (previous)
        m_previousLabel = elided(previous->title, 60);
    if (next)
        m_nextLabel = elided(next->title, 60);
    fitPager();
}

void DocsFeature::fitPager()
{
    const QMargins margins = m_pager->contentsMargins();
    const int width = m_pager->geometry().width() - margins.left() - margins.right();
    const int spacing = m_pager->spacing() >= 0 ? m_pager->spacing() : 6;
    const bool both = !m_previousButton->isHidden() && !m_nextButton->isHidden();
    const int budget = width <= 0 ? 0 : (both ? (width - 2 * spacing) / 2 : width);
    const auto fit = [budget](QPushButton *button, const QString &label, bool forward) {
        if (label.isEmpty())
            return;
        const QString arrow(forward ? QChar(0x2192) : QChar(0x2190));
        const auto compose = [&arrow, forward](const QString &title) {
            return forward ? title + QLatin1Char(' ') + arrow : arrow + QLatin1Char(' ') + title;
        };
        QString text = compose(label);
        if (budget > 0) {
            if (button->text().isEmpty())
                button->setText(text);
            const QFontMetrics metrics = button->fontMetrics();
            const int chrome = button->sizeHint().width() - metrics.horizontalAdvance(button->text());
            const int room = budget - chrome - metrics.horizontalAdvance(compose(QString()));
            if (metrics.horizontalAdvance(label) > room)
                text = compose(metrics.elidedText(label, Qt::ElideRight, qMax(0, room)));
        }
        if (button->text() != text)
            button->setText(text);
    };
    fit(m_previousButton, m_previousLabel, false);
    fit(m_nextButton, m_nextLabel, true);
}

void DocsFeature::syncDesign()
{
    const bool next = Theme::isNext();
    const bool dark = Theme::activeDark();
    if (m_designSynced && next == m_next && dark == m_dark)
        return;
    const bool restyle = m_designSynced && (next || m_next);
    m_designSynced = true;
    m_next = next;
    m_dark = dark;

    layout()->setSpacing(next ? 14 : 10);
    m_navigation->parentWidget()->layout()->setSpacing(next ? 10 : 8);
    m_readerPane->layout()->setContentsMargins(next ? QMargins(1, 1, 1, 1) : QMargins());
    m_readerPane->layout()->setSpacing(next ? 0 : 6);
    m_pager->setContentsMargins(next ? QMargins(10, 6, 10, 9) : QMargins());
    m_pager->setSpacing(next ? 8 : -1);
    m_outlinePane->layout()->setContentsMargins(next ? QMargins(6, 9, 0, 0) : QMargins());
    m_navigation->setIndentation(next ? kNavigationIndentation : m_classicIndentation);
    m_navigation->viewport()->setAttribute(Qt::WA_Hover, next || m_classicHover);
    m_navigation->setMouseTracking(next || m_classicTracking);
    m_outline->viewport()->setAttribute(Qt::WA_Hover, next || m_outlineHover);
    m_outline->setMouseTracking(next || m_outlineTracking);
    m_view->document()->setDocumentMargin(next ? kDocumentMargin : kClassicDocumentMargin);
    fitScrollBars();
    fitPage();
    if (!restyle || m_current.isEmpty())
        return;
    QTimer::singleShot(0, this, [this] {
        const DocsPage *page = m_library.page(m_current);
        if (!page)
            return;
        const int position = m_view->verticalScrollBar()->value();
        renderDocument(*page);
        renderBreadcrumb();
        renderOutline();
        renderFooter();
        m_view->verticalScrollBar()->setValue(position);
    });
}

void DocsFeature::fitScrollBars()
{
    const bool slack = m_next && m_view->horizontalScrollBar()->maximum() <= kScrollSlack;
    const Qt::ScrollBarPolicy policy = slack ? Qt::ScrollBarAlwaysOff : Qt::ScrollBarAsNeeded;
    if (m_view->horizontalScrollBarPolicy() != policy)
        m_view->setHorizontalScrollBarPolicy(policy);
}

void DocsFeature::fitPage()
{
    QMargins margins(kPageMargin, 18, kPageMargin, kPageMargin);
    bool outline = true;
    if (m_next) {
        const int handle = m_splitter->handleWidth();
        const int available = width() - margins.left() - margins.right();
        outline = available >= kNavigationWidth + kReaderWidthForOutline + kOutlineWidth + 2 * handle;
        const int content = kNavigationWidth + handle + kReaderMaximumWidth +
                            (outline ? handle + kOutlineWidth : 0);
        margins.setRight(margins.right() + qMax(0, available - content));
    }
    if (layout()->contentsMargins() != margins)
        layout()->setContentsMargins(margins);
    if (m_outlinePane->isHidden() == outline)
        m_outlinePane->setVisible(outline);
}

QSize DocsFeature::minimumSizeHint() const
{
    QSize hint = forkmesh::market::NativePackageWidget::minimumSizeHint();
    if (layout())
        hint.rwidth() -= qMax(0, layout()->contentsMargins().right() - kPageMargin);
    return hint;
}

bool DocsFeature::event(QEvent *event)
{
    const bool handled = forkmesh::market::NativePackageWidget::event(event);
    if (m_splitter && (event->type() == QEvent::Polish || event->type() == QEvent::StyleChange))
        syncDesign();
    return handled;
}

bool DocsFeature::eventFilter(QObject *watched, QEvent *event)
{
    if (m_pager && watched == m_readerPane && event->type() == QEvent::Resize)
        fitPager();
    return forkmesh::market::NativePackageWidget::eventFilter(watched, event);
}

void DocsFeature::resizeEvent(QResizeEvent *event)
{
    forkmesh::market::NativePackageWidget::resizeEvent(event);
    if (m_splitter)
        fitPage();
}

void DocsFeature::scrollToFragment(const QString &fragment)
{
    m_view->scrollToAnchor(fragment);
}

void DocsFeature::followLink(const QUrl &url)
{
    const DocsTarget target = m_library.resolve(m_current, url.toString());
    switch (target.kind) {
    case DocsTarget::Kind::Anchor:
        scrollToFragment(target.fragment);
        break;
    case DocsTarget::Kind::Page:
        openPage(target.path, target.fragment);
        break;
    case DocsTarget::Kind::External:
        QDesktopServices::openUrl(target.url);
        break;
    case DocsTarget::Kind::None:
        break;
    }
}

void DocsFeature::goBack()
{
    if (m_back.isEmpty())
        return;
    const Visit visit = m_back.takeLast();
    m_forward.append({m_current, QString()});
    openPage(visit.path, visit.fragment, false);
}

void DocsFeature::goForward()
{
    if (m_forward.isEmpty())
        return;
    const Visit visit = m_forward.takeLast();
    m_back.append({m_current, QString()});
    openPage(visit.path, visit.fragment, false);
}

void DocsFeature::updateHistoryButtons()
{
    m_backButton->setEnabled(!m_back.isEmpty());
    m_forwardButton->setEnabled(!m_forward.isEmpty());
    m_webButton->setEnabled(!m_current.isEmpty() && !DocsLibrary::webUrl(m_current).isEmpty());
}

QString DocsFeature::lastPageKey() const
{
    return QStringLiteral("market/packages/%1/lastPage").arg(m_packageId);
}

}

namespace {
const forkmesh::market::NativePackageRegistration docsRegistration({
    QStringLiteral("builtin:docs"),
    {QStringLiteral("extension.native")},
    QStringLiteral("Docs"),
    QStringLiteral("book"),
    QString(),
    QString(),
    [](const forkmesh::market::NativePackageHost &host, QWidget *parent) {
        return new forkmesh::docs::DocsFeature(host, parent);
    },
});
}
