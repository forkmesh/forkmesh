#include "SettingsSectionsView.h"

#include <QAbstractButton>
#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QEvent>
#include <QFontMetrics>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace {

constexpr int kSidebarWidth = 200;
// How far below the top edge a heading may sit and still count as the section
// being read, so a jump that lands a few pixels short does not light the
// previous entry.
constexpr int kReadingLine = 16;
constexpr int kSearchDelayMs = 150;
const char kFlattenerName[] = "settingsScrollAreaFlattener";
constexpr int kContentMaxWidth = 920;
constexpr int kSubIndent = 22;
constexpr int kSubJumpMargin = 8;
constexpr int kBuildAheadScreens = 2;
constexpr int kSubTitlesRole = Qt::UserRole + 1;
constexpr int kCurrentSubRole = Qt::UserRole + 2;
constexpr int kHoverSubRole = Qt::UserRole + 3;
constexpr int kNextSidebarWidth = 256;
constexpr int kNextContentMaxWidth = 824;
constexpr int kNextTitleHeight = 34;
constexpr int kNextSubHeight = 28;
constexpr int kNextSubIndent = 16;
const char kNextNavColors[] = "nextNavColors";
const char kNextLayout[] = "nextLayout";

QVariantMap nextNavColors(const QWidget *widget)
{
    return widget ? widget->property(kNextNavColors).toMap() : QVariantMap();
}

QColor navColor(const QVariantMap &colors, const char *name)
{
    return QColor(colors.value(QString::fromLatin1(name)).toString());
}

void applyNextChrome(QWidget *view, QLineEdit *search, QListWidget *nav,
                     QScrollArea *scroll, QWidget *content)
{
    if (view->property("nextLayoutApplied").toBool())
        return;
    auto *row = qobject_cast<QHBoxLayout *>(view->layout());
    auto *sidebar = view->findChild<QWidget *>(QStringLiteral("settingsSidebar"),
                                               Qt::FindDirectChildrenOnly);
    auto *side = sidebar ? qobject_cast<QVBoxLayout *>(sidebar->layout()) : nullptr;
    if (!row || !side)
        return;
    view->setProperty("nextLayoutApplied", true);
    row->setSpacing(0);
    sidebar->setFixedWidth(kNextSidebarWidth);
    sidebar->setAttribute(Qt::WA_StyledBackground, true);
    side->setContentsMargins(0, 0, 0, 0);
    side->setSpacing(0);

    auto *band = new QWidget(sidebar);
    band->setObjectName(QStringLiteral("settingsSearchBand"));
    auto *bandColumn = new QVBoxLayout(band);
    bandColumn->setContentsMargins(12, 12, 12, 12);
    bandColumn->setSpacing(0);
    side->removeWidget(search);
    bandColumn->addWidget(search);
    side->insertWidget(0, band);
    auto *bandRule = new QFrame(sidebar);
    bandRule->setObjectName(QStringLiteral("settingsSidebarRule"));
    bandRule->setFrameShape(QFrame::NoFrame);
    bandRule->setFixedHeight(1);
    side->insertWidget(1, bandRule);
    nav->setIconSize(QSize(16, 16));

    auto *column = new QWidget(view);
    column->setObjectName(QStringLiteral("settingsMainColumn"));
    auto *main = new QVBoxLayout(column);
    main->setContentsMargins(0, 0, 0, 0);
    main->setSpacing(0);
    row->removeWidget(scroll);
    main->addWidget(scroll, 1);
    auto *slot = new QWidget(column);
    slot->setObjectName(QStringLiteral("settingsFooterSlot"));
    auto *slotColumn = new QVBoxLayout(slot);
    slotColumn->setContentsMargins(0, 0, 0, 0);
    slotColumn->setSpacing(0);
    main->addWidget(slot);
    row->addWidget(column, 1);

    content->setMaximumWidth(kNextContentMaxWidth);
    if (QLayout *contentColumn = content->layout())
        contentColumn->setContentsMargins(32, 0, 32, 40);
}

QString plainText(const QString &text)
{
    if (!Qt::mightBeRichText(text))
        return text;
    static const QRegularExpression tags(QStringLiteral("<[^>]*>"));
    QString plain = text;
    plain.replace(tags, QStringLiteral(" "));
    plain.replace(QLatin1String("&nbsp;"), QLatin1String(" "));
    plain.replace(QLatin1String("&amp;"), QLatin1String("&"));
    plain.replace(QLatin1String("&lt;"), QLatin1String("<"));
    plain.replace(QLatin1String("&gt;"), QLatin1String(">"));
    plain.replace(QLatin1String("&quot;"), QLatin1String("\""));
    return plain;
}

QString subSectionTitle(const QString &text)
{
    const QString plain = plainText(text).simplified();
    if (plain.isEmpty() || plain != plain.toUpper())
        return plain;
    static const QRegularExpression vowel(QStringLiteral("[AEIOUY]"));
    QStringList words = plain.split(QLatin1Char(' '));
    for (int i = 0; i < words.size(); ++i) {
        QString &word = words[i];
        if (!word.contains(vowel))
            continue;
        word = word.toLower();
        if (i == 0)
            word[0] = word.at(0).toUpper();
    }
    return words.join(QLatin1Char(' '));
}

class SettingsNavDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    int subLineHeight() const { return m_subHeight; }

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override
    {
        QSize size = QStyledItemDelegate::sizeHint(option, index);
        const int subs = index.data(kSubTitlesRole).toStringList().size();
        if (!nextNavColors(option.widget).isEmpty()) {
            m_subHeight = kNextSubHeight;
            return QSize(size.width(), kNextTitleHeight + subs * kNextSubHeight);
        }
        if (subs > 0)
            size.setHeight(size.height() + subs * measureSubLine(option, index));
        return size;
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        const QStringList subs = index.data(kSubTitlesRole).toStringList();
        const QVariantMap colors = nextNavColors(option.widget);
        if (!colors.isEmpty()) {
            paintNext(painter, option, index, subs, colors);
            return;
        }
        if (subs.isEmpty()) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }
        const int current = index.data(kCurrentSubRole).toInt();
        const QVariant hoverData = index.data(kHoverSubRole);
        const int hover = hoverData.isValid() ? hoverData.toInt() : -2;
        const int line = measureSubLine(option, index);

        QStyleOptionViewItem title = option;
        title.rect.setHeight(option.rect.height() - int(subs.size()) * line);
        if (current >= 0) {
            title.state &= ~QStyle::State_Selected;
            title.font.setBold(true);
        }
        if (hover != -1)
            title.state &= ~QStyle::State_MouseOver;
        QStyledItemDelegate::paint(painter, title, index);

        QStyleOptionViewItem sub = option;
        initStyleOption(&sub, index);
        sub.icon = QIcon();
        sub.features &= ~QStyleOptionViewItem::HasDecoration;
        sub.decorationSize = QSize(0, 0);
        sub.font = option.font;
        const QWidget *widget = option.widget;
        QStyle *style = widget ? widget->style() : QApplication::style();
        int top = title.rect.bottom() + 1;
        for (int i = 0; i < subs.size(); ++i, top += line) {
            sub.rect = QRect(option.rect.left() + kSubIndent, top,
                             option.rect.width() - kSubIndent, line);
            sub.text = subs.at(i);
            sub.state = option.state &
                        ~(QStyle::State_Selected | QStyle::State_MouseOver |
                          QStyle::State_HasFocus);
            if (i == current)
                sub.state |= QStyle::State_Selected;
            if (i == hover)
                sub.state |= QStyle::State_MouseOver;
            style->drawControl(QStyle::CE_ItemViewItem, &sub, painter, widget);
        }
    }

private:
    void paintNext(QPainter *painter, const QStyleOptionViewItem &option,
                   const QModelIndex &index, const QStringList &subs,
                   const QVariantMap &colors) const
    {
        const int current = subs.isEmpty() ? -1 : index.data(kCurrentSubRole).toInt();
        const QVariant hoverData = index.data(kHoverSubRole);
        const int hover = hoverData.isValid() ? hoverData.toInt() : -2;
        const bool selected = option.state & QStyle::State_Selected;
        const bool subActive = selected && current >= 0;
        const bool rowActive = selected && !subActive;
        const bool hovered = (option.state & QStyle::State_MouseOver) &&
                             (subs.isEmpty() || hover == -1);
        const QColor fg = navColor(colors, "fg");
        const QColor muted = navColor(colors, "muted");

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        const QRect title(option.rect.left(), option.rect.top(), option.rect.width(),
                          kNextTitleHeight);
        const QRectF titleFill = QRectF(title.adjusted(0, 1, 0, -1));
        if (rowActive || hovered) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(navColor(colors, rowActive ? "active" : "hover"));
            painter->drawRoundedRect(titleFill, 6, 6);
        }
        const QIcon icon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
        const int iconSide = 16;
        const QRect iconRect(title.left() + 8, title.center().y() - iconSide / 2 + 1,
                             iconSide, iconSide);
        if (!icon.isNull())
            icon.paint(painter, iconRect, Qt::AlignCenter,
                       selected ? QIcon::Selected : QIcon::Disabled);
        QFont font = option.font;
        font.setPixelSize(13);
        font.setWeight(rowActive ? QFont::Medium : QFont::Normal);
        painter->setFont(font);
        painter->setPen(selected || hovered ? fg : muted);
        const QRect textRect = title.adjusted(8 + iconSide + 8, 0, -8, 0);
        painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter,
                          QFontMetrics(font).elidedText(index.data(Qt::DisplayRole).toString(),
                                                        Qt::ElideRight, textRect.width()));

        if (!subs.isEmpty()) {
            const int top = title.bottom() + 1;
            const qreal guide = title.left() + kNextSubIndent + 0.5;
            painter->setPen(QPen(navColor(colors, "border"), 1));
            painter->drawLine(QPointF(guide, top + 2),
                              QPointF(guide, top + subs.size() * kNextSubHeight - 2));
            QFont subFont = option.font;
            subFont.setPixelSize(12);
            for (int i = 0; i < subs.size(); ++i) {
                const QRect line(title.left() + kNextSubIndent + 8,
                                 top + i * kNextSubHeight,
                                 title.width() - kNextSubIndent - 8, kNextSubHeight);
                const bool lit = subActive && i == current;
                const bool over = i == hover;
                if (lit || over) {
                    painter->setPen(Qt::NoPen);
                    painter->setBrush(navColor(colors, lit ? "active" : "hover"));
                    painter->drawRoundedRect(QRectF(line.adjusted(0, 1, 0, -1)), 6, 6);
                }
                subFont.setWeight(lit ? QFont::Medium : QFont::Normal);
                painter->setFont(subFont);
                painter->setPen(lit || over ? fg : muted);
                const QRect subText = line.adjusted(8, 0, -8, 0);
                painter->drawText(subText, Qt::AlignLeft | Qt::AlignVCenter,
                                  QFontMetrics(subFont).elidedText(subs.at(i), Qt::ElideRight,
                                                                   subText.width()));
            }
        }
        painter->restore();
    }

    int measureSubLine(const QStyleOptionViewItem &option, const QModelIndex &index) const
    {
        QStyleOptionViewItem sub = option;
        initStyleOption(&sub, index);
        sub.icon = QIcon();
        sub.features &= ~QStyleOptionViewItem::HasDecoration;
        sub.decorationSize = QSize(0, 0);
        sub.text = QStringLiteral("Xg");
        const QWidget *widget = option.widget;
        QStyle *style = widget ? widget->style() : QApplication::style();
        m_subHeight = qMax(1, style->sizeFromContents(QStyle::CT_ItemViewItem, &sub,
                                                       QSize(), widget)
                                  .height());
        return m_subHeight;
    }

    mutable int m_subHeight = 24;
};

// The words a person reads on or about a control. A field's value is left
// out: it is their data, not the name of a setting.
QString searchableText(const QWidget *widget)
{
    QStringList parts;
    if (const auto *label = qobject_cast<const QLabel *>(widget)) {
        parts << plainText(label->text());
    } else if (const auto *button = qobject_cast<const QAbstractButton *>(widget)) {
        parts << QString(button->text()).remove(QLatin1Char('&'));
    } else if (const auto *group = qobject_cast<const QGroupBox *>(widget)) {
        parts << QString(group->title()).remove(QLatin1Char('&'));
    } else if (const auto *edit = qobject_cast<const QLineEdit *>(widget)) {
        parts << edit->placeholderText();
    } else if (const auto *combo = qobject_cast<const QComboBox *>(widget)) {
        for (int i = 0; i < combo->count(); ++i)
            parts << combo->itemText(i);
    }
    if (!widget->toolTip().isEmpty())
        parts << plainText(widget->toolTip());
    return parts.join(QLatin1Char(' ')).simplified();
}

// Draws a box around every search hit, over the page and under nothing: it
// takes no mouse input, so the controls beneath stay usable.
class SearchHitOverlay : public QWidget
{
public:
    SearchHitOverlay(QWidget *content, const SettingsSectionsView *view)
        : QWidget(content), m_view(view)
    {
        // The app stylesheet paints every plain QWidget; this one is only
        // outlines, so the theme gives #settingsSearchHits no background.
        setObjectName(QStringLiteral("settingsSearchHits"));
        setAttribute(Qt::WA_TransparentForMouseEvents);
        hide();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QList<QWidget *> hits = m_view->searchHits();
        for (int i = 0; i < hits.size(); ++i) {
            QWidget *hit = hits.at(i);
            const bool current = i == m_view->currentHit();
            if (!hit->isVisibleTo(parentWidget()))
                continue;
            QRect box(hit->mapTo(parentWidget(), QPoint(0, 0)), hit->size());
            box.adjust(-3, -2, 3, 2);
            painter.setPen(QPen(current ? QColor(240, 180, 40)
                                        : QColor(210, 153, 34, 170),
                                current ? 2 : 1));
            painter.setBrush(QColor(210, 153, 34, current ? 60 : 36));
            painter.drawRoundedRect(QRectF(box), 4, 4);
        }
    }

private:
    const SettingsSectionsView *m_view;
};

// Keeps a flattened scroll area exactly as tall as its content at the width
// it is given.
class ScrollAreaFlattener : public QObject
{
public:
    explicit ScrollAreaFlattener(QScrollArea *area) : QObject(area), m_area(area)
    {
        setObjectName(QString::fromLatin1(kFlattenerName));
        area->viewport()->installEventFilter(this);
        if (area->widget())
            area->widget()->installEventFilter(this);
        fit();
    }

    void release()
    {
        m_area->viewport()->removeEventFilter(this);
        if (m_area->widget())
            m_area->widget()->removeEventFilter(this);
    }

protected:
    bool eventFilter(QObject *, QEvent *event) override
    {
        if ((event->type() == QEvent::Resize ||
             event->type() == QEvent::LayoutRequest) &&
            !m_fitQueued) {
            m_fitQueued = true;
            QTimer::singleShot(0, this, [this] {
                m_fitQueued = false;
                fit();
            });
        }
        return false;
    }

private:
    void fit()
    {
        QWidget *content = m_area->widget();
        if (!content)
            return;
        int width = m_area->viewport()->width();
        if (width <= 0)
            width = m_area->width();
        int height = content->hasHeightForWidth()
                         ? content->heightForWidth(width)
                         : content->sizeHint().height();
        height = qMax(height, content->minimumSizeHint().height()) +
                 2 * m_area->frameWidth();
        if (m_area->minimumHeight() != height || m_area->maximumHeight() != height)
            m_area->setFixedHeight(height);
    }

    QScrollArea *m_area;
    bool m_fitQueued = false;
};

} // namespace

SettingsSectionsView::SettingsSectionsView(QWidget *parent) : QWidget(parent)
{
    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(18);

    auto *sidebar = new QWidget;
    sidebar->setObjectName(QStringLiteral("settingsSidebar"));
    sidebar->setFixedWidth(kSidebarWidth);
    auto *sideColumn = new QVBoxLayout(sidebar);
    sideColumn->setContentsMargins(0, 2, 0, 0);
    sideColumn->setSpacing(8);

    m_search = new QLineEdit;
    m_search->setObjectName(QStringLiteral("settingsSearch"));
    m_search->setPlaceholderText(QStringLiteral("Search settings"));
    m_search->setClearButtonEnabled(true);
    m_search->setToolTip(QStringLiteral(
        "Shows only the sections that mention every word and marks each "
        "matching setting. Enter moves to the next match, Esc clears."));
    m_search->installEventFilter(this);
    sideColumn->addWidget(m_search);

    m_nav = new QListWidget;
    m_nav->setObjectName(QStringLiteral("settingsNav"));
    m_nav->setFrameShape(QFrame::NoFrame);
    m_nav->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_nav->setIconSize(QSize(14, 14));
    m_nav->setUniformItemSizes(false);
    m_nav->setSelectionMode(QAbstractItemView::SingleSelection);
    m_nav->setItemDelegate(new SettingsNavDelegate(m_nav));
    m_nav->setMouseTracking(true);
    m_nav->viewport()->installEventFilter(this);
    sideColumn->addWidget(m_nav, 1);

    m_scroll = new QScrollArea;
    m_scroll->setObjectName(QStringLiteral("settingsSectionsScroll"));
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setWidgetResizable(true);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    m_scroll->setAlignment(Qt::AlignHCenter | Qt::AlignTop);

    m_content = new QWidget;
    m_content->setObjectName(QStringLiteral("settingsSectionsContent"));
    m_content->setMaximumWidth(kContentMaxWidth);
    auto *contentColumn = new QVBoxLayout(m_content);
    contentColumn->setContentsMargins(0, 0, 10, 0);
    contentColumn->setSpacing(0);
    m_noMatches = new QLabel;
    m_noMatches->setObjectName(QStringLiteral("settingsNoMatches"));
    m_noMatches->setWordWrap(true);
    m_noMatches->hide();
    contentColumn->addWidget(m_noMatches);
    m_sectionsLayout = new QVBoxLayout;
    m_sectionsLayout->setContentsMargins(0, 0, 0, 0);
    m_sectionsLayout->setSpacing(0);
    contentColumn->addLayout(m_sectionsLayout);
    // Room under the last section so it, too, can be scrolled to the top
    // when its entry is clicked.
    m_bottomSpacer = new QWidget;
    m_bottomSpacer->setObjectName(QStringLiteral("settingsSectionsTail"));
    m_bottomSpacer->setFixedHeight(0);
    contentColumn->addWidget(m_bottomSpacer);
    contentColumn->addStretch(1);
    m_scroll->setWidget(m_content);
    m_hitOverlay = new SearchHitOverlay(m_content, this);

    row->addWidget(sidebar);
    row->addWidget(m_scroll, 1);

    m_searchTimer = new QTimer(this);
    m_searchTimer->setSingleShot(true);
    m_searchTimer->setInterval(kSearchDelayMs);
    connect(m_searchTimer, &QTimer::timeout, this, &SettingsSectionsView::applySearch);
    connect(m_search, &QLineEdit::textChanged, m_searchTimer,
            qOverload<>(&QTimer::start));
    connect(m_search, &QLineEdit::returnPressed, this, [this] {
        if (m_searchTimer->isActive()) {
            m_searchTimer->stop();
            applySearch();
        }
        nextSearchHit();
    });

    const auto jumpFromList = [this](int row) {
        if (m_syncingNav || row < 0)
            return;
        const int index = m_nav->item(row)->data(Qt::UserRole).toInt();
        setCurrentIndex(index);
        emit sectionActivated(index);
    };
    connect(m_nav, &QListWidget::currentRowChanged, this, jumpFromList);
    // Clicking the entry already lit returns to the top of its section.
    connect(m_nav, &QListWidget::itemClicked, this,
            [this, jumpFromList](QListWidgetItem *item) {
                jumpFromList(m_nav->row(item));
            });

    connect(m_scroll->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
        buildPendingAbove(-1);
        if (!m_jumping)
            updateCurrentFromScroll();
        updateRevealed();
    });
    m_scroll->viewport()->installEventFilter(this);
    m_content->installEventFilter(this);
    installEventFilter(this);
}

int SettingsSectionsView::addSection(QWidget *body, const QString &title,
                                     const QString &iconName)
{
    return appendSection(body, {}, title, iconName);
}

int SettingsSectionsView::addLazySection(std::function<QWidget *()> build,
                                         const QString &title,
                                         const QString &iconName)
{
    return appendSection(nullptr, std::move(build), title, iconName);
}

int SettingsSectionsView::appendSection(QWidget *body,
                                        std::function<QWidget *()> build,
                                        const QString &title,
                                        const QString &iconName)
{
    const int index = count();
    const bool next = property(kNextLayout).toBool();
    auto *frame = new QWidget;
    frame->setObjectName(QStringLiteral("settingsSection"));
    // Each section is exactly as tall as it wants: a long page must not
    // squeeze a list down to its minimum the way a full tab never did. Its
    // width is the page's: one section with a wide row must not widen, and
    // so clip, every other section the way it would only have clipped its
    // own tab.
    frame->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    m_sectionsLayout->addWidget(frame);
    auto *column = new QVBoxLayout(frame);
    column->setContentsMargins(0, index == 0 ? 2 : 26, 0, 6);
    column->setSpacing(6);

    auto *header = new QWidget;
    header->setObjectName(QStringLiteral("settingsSectionHeader"));
    header->setCursor(Qt::PointingHandCursor);
    auto *heading = new QHBoxLayout(header);
    heading->setContentsMargins(0, 0, 0, 0);
    heading->setSpacing(8);
    auto *toggle = new QToolButton;
    toggle->setObjectName(QStringLiteral("settingsSectionToggle"));
    toggle->setAutoRaise(true);
    toggle->setFixedSize(18, 18);
    toggle->setArrowType(Qt::DownArrow);
    auto *icon = new QLabel;
    icon->setObjectName(QStringLiteral("settingsSectionIcon"));
    icon->setFixedSize(16, 16);
    auto *titleLabel = new QLabel(title);
    titleLabel->setObjectName(QStringLiteral("settingsSectionTitle"));
    auto *rule = new QFrame;
    rule->setObjectName(QStringLiteral("settingsSectionRule"));
    rule->setFrameShape(QFrame::NoFrame);
    rule->setFixedHeight(1);
    if (next) {
        column->setSpacing(0);
        heading->setSpacing(8);
        toggle->setFixedSize(20, 20);
        heading->addWidget(icon);
        heading->addWidget(titleLabel);
        heading->addStretch(1);
        heading->addWidget(toggle);
        column->addWidget(rule);
        column->addWidget(header);
        rule->setVisible(index > 0);
    } else {
        heading->addWidget(toggle);
        heading->addWidget(icon);
        heading->addWidget(titleLabel);
        heading->addStretch(1);
        column->addWidget(header);
        column->addWidget(rule);
    }
    if (body)
        column->addWidget(body);

    frame->installEventFilter(this);
    header->installEventFilter(this);
    connect(toggle, &QToolButton::clicked, this, [this, frame] {
        for (int i = 0; i < count(); ++i) {
            if (m_sections.at(i).frame == frame) {
                setExpanded(i, !m_sections.at(i).expanded);
                return;
            }
        }
    });

    auto *item = new QListWidgetItem(title, m_nav);
    item->setData(Qt::UserRole, index);

    Section section;
    section.frame = frame;
    section.body = body;
    section.header = header;
    section.toggle = toggle;
    section.icon = icon;
    section.build = std::move(build);
    section.title = title;
    section.iconName = iconName;
    section.expanded = body != nullptr;
    m_sections.append(section);
    applyExpanded(index);
    if (m_current < 0) {
        m_current = 0;
        syncNavRow();
    }
    scheduleRefresh();
    return index;
}

bool SettingsSectionsView::isBuilt(int index) const
{
    return index >= 0 && index < count() && m_sections.at(index).body;
}

bool SettingsSectionsView::isExpanded(int index) const
{
    return index >= 0 && index < count() && m_sections.at(index).expanded;
}

void SettingsSectionsView::setExpanded(int index, bool expanded)
{
    if (index < 0 || index >= count() || m_sections.at(index).expanded == expanded)
        return;
    m_sections[index].expanded = expanded;
    if (expanded)
        ensureBuilt(index);
    applyExpanded(index);
    scheduleRefresh();
    emit expandedChanged(index, expanded);
}

QStringList SettingsSectionsView::collapsedTitles() const
{
    QStringList titles;
    for (const Section &section : m_sections) {
        if (!section.expanded)
            titles << section.title;
    }
    return titles;
}

void SettingsSectionsView::setCollapsedTitles(const QStringList &titles)
{
    for (int i = 0; i < count(); ++i) {
        const bool expanded = !titles.contains(m_sections.at(i).title);
        if (m_sections.at(i).expanded == expanded)
            continue;
        m_sections[i].expanded = expanded;
        applyExpanded(i);
    }
    scheduleRefresh();
}

void SettingsSectionsView::ensureBuilt(int index)
{
    Section &section = m_sections[index];
    if (section.body || !section.build)
        return;
    const std::function<QWidget *()> build = std::move(section.build);
    section.build = nullptr;
    QWidget *body = build();
    if (!body)
        body = new QWidget;
    section.body = body;
    section.subsDirty = true;
    static_cast<QVBoxLayout *>(section.frame->layout())->addWidget(body);
    applyExpanded(index);
}

void SettingsSectionsView::buildPendingAbove(int index)
{
    int limit = index;
    if (limit < 0) {
        if (!isVisible())
            return;
        const int reach = m_scroll->verticalScrollBar()->value() +
                          kBuildAheadScreens * qMax(1, m_scroll->viewport()->height());
        for (int i = 0; i < count(); ++i) {
            if (!m_sections.at(i).frame->isHidden() && m_sections.at(i).frame->y() < reach)
                limit = i;
        }
    }
    bool built = false;
    for (int i = 0; i <= limit && i < count(); ++i) {
        const Section &section = m_sections.at(i);
        if (section.expanded && !section.body && section.build) {
            ensureBuilt(i);
            built = true;
        }
    }
    if (built)
        scheduleRefresh();
}

void SettingsSectionsView::applyExpanded(int index)
{
    const Section &section = m_sections.at(index);
    const bool open = section.expanded || searching();
    if (section.body)
        section.body->setVisible(open);
    section.toggle->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
    const QString verb = open ? QStringLiteral("Collapse") : QStringLiteral("Expand");
    section.toggle->setToolTip(QStringLiteral("%1 %2").arg(verb, section.title));
    section.toggle->setAccessibleName(QStringLiteral("%1 %2").arg(verb, section.title));
    if (property(kNextLayout).toBool()) {
        section.frame->layout()->setContentsMargins(0, 0, 0, open ? 28 : 0);
        section.header->layout()->setContentsMargins(0, index == 0 ? 24 : open ? 28 : 16,
                                                     0, 16);
        return;
    }
    section.frame->layout()->setContentsMargins(0, index == 0 ? 2 : open ? 26 : 10, 0,
                                                open ? 6 : 2);
}

bool SettingsSectionsView::searching() const
{
    return !searchText().isEmpty();
}

QString SettingsSectionsView::tabText(int index) const
{
    return index >= 0 && index < count() ? m_sections.at(index).title : QString();
}

QWidget *SettingsSectionsView::widget(int index) const
{
    return index >= 0 && index < count() ? m_sections.at(index).body : nullptr;
}

QString SettingsSectionsView::sectionIconName(int index) const
{
    return index >= 0 && index < count() ? m_sections.at(index).iconName : QString();
}

void SettingsSectionsView::setSectionIcon(int index, const QIcon &icon)
{
    if (index < 0 || index >= count())
        return;
    m_nav->item(index)->setIcon(icon);
    m_sections[index].icon->setPixmap(icon.pixmap(QSize(16, 16), devicePixelRatioF()));
}

void SettingsSectionsView::setSectionHidden(int index, bool hidden)
{
    if (index < 0 || index >= count() || m_sections.at(index).hidden == hidden)
        return;
    m_sections[index].hidden = hidden;
    if (searching()) {
        applySearch();
        return;
    }
    m_sections.at(index).frame->setVisible(!hidden);
    m_syncingNav = true;
    m_nav->item(index)->setHidden(hidden);
    m_syncingNav = false;
    scheduleRefresh();
}

bool SettingsSectionsView::isSectionHidden(int index) const
{
    return index >= 0 && index < count() && m_sections.at(index).hidden;
}

void SettingsSectionsView::setCurrentIndex(int index)
{
    if (index < 0 || index >= count() || m_sections.at(index).hidden)
        return;
    if (m_sections.at(index).frame->isHidden() && !searchText().isEmpty())
        setSearchText(QString());
    setExpanded(index, true);
    setCurrent(index);
    if (!isVisible()) {
        // Nothing is laid out while the page is hidden; land on it once shown.
        m_pendingJump = index;
        return;
    }
    scrollToSection(index);
    // Sections still settling (a panel moving in, a list filling) can shift
    // the heading after this pass, so land once more when the queue drains.
    m_pendingJump = index;
    scheduleRefresh();
}

void SettingsSectionsView::revealWidget(QWidget *target)
{
    if (!target)
        return;
    for (int i = 0; i < count(); ++i) {
        const Section &section = m_sections.at(i);
        if (section.body &&
            (section.body == target || section.body->isAncestorOf(target))) {
            m_pendingReveal = target;
            setCurrentIndex(i);
            return;
        }
    }
}

QStringList SettingsSectionsView::subSectionTitles(int index) const
{
    QStringList titles;
    if (index < 0 || index >= count())
        return titles;
    auto *self = const_cast<SettingsSectionsView *>(this);
    for (const SubSection &sub : self->visibleSubSections(index))
        titles << sub.title;
    return titles;
}

void SettingsSectionsView::setCurrentSubSection(int index, int sub)
{
    if (index < 0 || index >= count())
        return;
    setCurrentIndex(index);
    const QVector<SubSection> subs = visibleSubSections(index);
    if (sub < 0 || sub >= subs.size() || !isVisible())
        return;
    QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
    if (QLayout *layout = m_content->layout())
        layout->activate();
    m_pendingJump = -1;
    const int y = subs.at(sub).anchor->mapTo(m_content, QPoint(0, 0)).y();
    m_jumping = true;
    m_scroll->verticalScrollBar()->setValue(qMax(0, y - kSubJumpMargin));
    m_jumping = false;
    setCurrent(index);
    updateCurrentSub();
    updateRevealed();
}

void SettingsSectionsView::collectSubSections(int index)
{
    Section &section = m_sections[index];
    section.subsDirty = false;
    section.subs.clear();
    if (!section.body)
        return;
    const QList<QLabel *> labels =
        section.body->findChildren<QLabel *>(QStringLiteral("sectionLabel"));
    for (QLabel *label : labels) {
        QWidget *card = nullptr;
        for (QWidget *up = label->parentWidget(); up && up != section.body;
             up = up->parentWidget()) {
            if (up->property("sectionCard").toBool()) {
                card = up;
                break;
            }
        }
        if (card && card->parentWidget() != section.body)
            continue;
        QWidget *anchor = card ? card : label;
        const QString title = subSectionTitle(label->text());
        if (title.isEmpty())
            continue;
        bool seen = false;
        for (const SubSection &sub : std::as_const(section.subs))
            seen = seen || sub.anchor == anchor;
        if (!seen)
            section.subs.append({anchor, label, title});
    }
}

QVector<SettingsSectionsView::SubSection> SettingsSectionsView::visibleSubSections(int index)
{
    QVector<SubSection> visible;
    if (index < 0 || index >= count())
        return visible;
    if (m_sections.at(index).subsDirty)
        collectSubSections(index);
    const Section &section = m_sections.at(index);
    if (!section.body || section.frame->isHidden() || !section.body->isVisibleTo(section.frame))
        return visible;
    QVector<QPair<int, int>> order;
    for (int i = 0; i < section.subs.size(); ++i) {
        const SubSection &sub = section.subs.at(i);
        if (!sub.anchor || !sub.label || !sub.label->isVisibleTo(section.frame))
            continue;
        order.append({sub.anchor->mapTo(m_content, QPoint(0, 0)).y(), i});
    }
    std::stable_sort(order.begin(), order.end(),
                     [](const QPair<int, int> &a, const QPair<int, int> &b) {
                         return a.first < b.first;
                     });
    if (order.size() < 2)
        return visible;
    for (const auto &entry : std::as_const(order))
        visible.append(section.subs.at(entry.second));
    return visible;
}

void SettingsSectionsView::updateCurrentSub()
{
    int current = -1;
    if (m_current >= 0) {
        const QVector<SubSection> subs = visibleSubSections(m_current);
        const int line = m_scroll->verticalScrollBar()->value() + kReadingLine;
        if (!subs.isEmpty())
            current = 0;
        for (int i = 1; i < subs.size(); ++i) {
            if (subs.at(i).anchor->mapTo(m_content, QPoint(0, 0)).y() > line)
                break;
            current = i;
        }
    }
    m_currentSub = current;
    syncNavSubs();
}

void SettingsSectionsView::syncNavSubs()
{
    const int row = m_current >= 0 && m_current < m_nav->count() ? m_current : -1;
    QStringList titles;
    if (row >= 0) {
        for (const SubSection &sub : visibleSubSections(row))
            titles << sub.title;
    }
    bool relayout = false;
    if (m_navSubRow >= 0 && m_navSubRow != row && m_navSubRow < m_nav->count()) {
        QListWidgetItem *old = m_nav->item(m_navSubRow);
        old->setData(kSubTitlesRole, QVariant());
        old->setData(kCurrentSubRole, QVariant());
        relayout = true;
    }
    m_navSubRow = row;
    if (row >= 0) {
        QListWidgetItem *item = m_nav->item(row);
        if (item->data(kSubTitlesRole).toStringList() != titles) {
            item->setData(kSubTitlesRole, titles.isEmpty() ? QVariant() : QVariant(titles));
            relayout = true;
        }
        const int current = titles.isEmpty() ? -1 : m_currentSub;
        if (item->data(kCurrentSubRole).toInt() != current ||
            !item->data(kCurrentSubRole).isValid())
            item->setData(kCurrentSubRole, current);
        if (relayout) {
            m_nav->doItemsLayout();
            m_nav->scrollToItem(item);
        }
    } else if (relayout) {
        m_nav->doItemsLayout();
    }
}

bool SettingsSectionsView::handleNavMouse(QEvent *event)
{
    const auto clearHover = [this] {
        if (m_navHoverRow >= 0 && m_navHoverRow < m_nav->count())
            m_nav->item(m_navHoverRow)->setData(kHoverSubRole, QVariant());
        m_navHoverRow = -1;
        m_navHoverSub = -2;
    };
    if (event->type() == QEvent::Leave) {
        clearHover();
        return false;
    }
    if (event->type() != QEvent::MouseMove && event->type() != QEvent::MouseButtonPress &&
        event->type() != QEvent::MouseButtonRelease &&
        event->type() != QEvent::MouseButtonDblClick)
        return false;
    auto *mouse = static_cast<QMouseEvent *>(event);
    const QPoint pos = mouse->position().toPoint();
    int row = -1;
    int sub = -2;
    if (QListWidgetItem *item = m_nav->itemAt(pos)) {
        row = m_nav->row(item);
        const int subs = int(item->data(kSubTitlesRole).toStringList().size());
        sub = -1;
        if (subs > 0) {
            const int line =
                static_cast<SettingsNavDelegate *>(m_nav->itemDelegate())->subLineHeight();
            const QRect rect = m_nav->visualItemRect(item);
            const int titleBottom = rect.bottom() + 1 - subs * line;
            if (pos.y() >= titleBottom)
                sub = qMin(subs - 1, (pos.y() - titleBottom) / line);
        }
    }
    if (row != m_navHoverRow || sub != m_navHoverSub) {
        clearHover();
        m_navHoverRow = row;
        m_navHoverSub = sub;
        if (row >= 0)
            m_nav->item(row)->setData(kHoverSubRole, sub);
    }
    if (event->type() == QEvent::MouseMove || sub < 0 || mouse->button() != Qt::LeftButton)
        return false;
    if (event->type() == QEvent::MouseButtonRelease) {
        const int index = m_nav->item(row)->data(Qt::UserRole).toInt();
        setCurrentSubSection(index, sub);
        emit sectionActivated(index);
    }
    return true;
}

void SettingsSectionsView::focusSearch()
{
    m_search->setFocus(Qt::ShortcutFocusReason);
    m_search->selectAll();
}

QString SettingsSectionsView::searchText() const
{
    return m_search->text().simplified();
}

void SettingsSectionsView::setSearchText(const QString &text)
{
    m_search->setText(text);
    m_searchTimer->stop();
    applySearch();
}

QStringList SettingsSectionsView::visibleSectionTitles() const
{
    QStringList titles;
    for (const Section &section : m_sections) {
        if (!section.frame->isHidden())
            titles << section.title;
    }
    return titles;
}

QList<QWidget *> SettingsSectionsView::searchHits() const
{
    QList<QWidget *> hits;
    for (const QPointer<QWidget> &hit : m_hits) {
        if (hit)
            hits << hit.data();
    }
    return hits;
}

void SettingsSectionsView::nextSearchHit()
{
    const QList<QWidget *> hits = searchHits();
    if (hits.isEmpty())
        return;
    m_hitCursor = (m_hitCursor + 1) % int(hits.size());
    QWidget *hit = hits.at(m_hitCursor);
    scrollToWidget(hit);
    m_hitOverlay->update();
    for (int i = 0; i < count(); ++i) {
        if (m_sections.at(i).frame->isAncestorOf(hit)) {
            emit sectionActivated(i);
            break;
        }
    }
}

void SettingsSectionsView::setScrollAreaFlattened(QScrollArea *area, bool flattened)
{
    if (!area)
        return;
    auto *existing = static_cast<ScrollAreaFlattener *>(area->findChild<QObject *>(
        QString::fromLatin1(kFlattenerName), Qt::FindDirectChildrenOnly));
    if (flattened) {
        if (existing)
            return;
        area->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        new ScrollAreaFlattener(area);
        return;
    }
    if (!existing)
        return;
    existing->release();
    delete existing;
    area->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    area->setMinimumHeight(0);
    area->setMaximumHeight(QWIDGETSIZE_MAX);
}

bool SettingsSectionsView::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == this) {
        if (event->type() == QEvent::DynamicPropertyChange &&
            static_cast<QDynamicPropertyChangeEvent *>(event)->propertyName() ==
                kNextLayout &&
            property(kNextLayout).toBool())
            applyNextChrome(this, m_search, m_nav, m_scroll, m_content);
        return false;
    }
    if (m_nav && watched == m_nav->viewport())
        return handleNavMouse(event) || QWidget::eventFilter(watched, event);
    if (watched == m_search && event->type() == QEvent::KeyPress &&
        static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape &&
        !m_search->text().isEmpty()) {
        setSearchText(QString());
        return true;
    }
    if (event->type() == QEvent::MouseButtonRelease &&
        static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
        for (int i = 0; i < count(); ++i) {
            if (m_sections.at(i).header == watched) {
                setExpanded(i, !m_sections.at(i).expanded);
                if (m_sections.at(i).expanded)
                    emit sectionActivated(i);
                return true;
            }
        }
    }
    switch (event->type()) {
    case QEvent::Resize:
    case QEvent::Move:
    case QEvent::LayoutRequest:
        if (watched != m_search)
            scheduleRefresh();
        if (event->type() == QEvent::LayoutRequest) {
            for (Section &section : m_sections) {
                if (section.frame == watched)
                    section.subsDirty = true;
            }
        }
        break;
    default:
        break;
    }
    return QWidget::eventFilter(watched, event);
}

void SettingsSectionsView::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    // Coming back to Settings reveals what is on screen afresh, so a section
    // that loads on sight (Apps, Organizations) refreshes like a tab did.
    for (Section &section : m_sections) {
        section.inView = false;
        section.subsDirty = true;
    }
    scheduleRefresh();
}

void SettingsSectionsView::applySearch()
{
    const QString query = searchText();
    const QStringList terms = query.toLower().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    m_hits.clear();
    m_hitCursor = -1;
    int shown = 0;
    for (int i = 0; i < count(); ++i) {
        if (!terms.isEmpty() && !m_sections.at(i).hidden)
            ensureBuilt(i);
        applyExpanded(i);
    }
    for (int i = 0; i < count(); ++i) {
        const Section &section = m_sections.at(i);
        bool keep = !section.hidden;
        QList<QWidget *> sectionHits;
        if (keep && !terms.isEmpty()) {
            QString haystack = section.title.toLower();
            const QList<QWidget *> widgets =
                section.body ? section.body->findChildren<QWidget *>() : QList<QWidget *>();
            for (QWidget *widget : widgets) {
                // Controls the section hides for its own reasons (a feature
                // that is off, a row for another platform) are not offered.
                if (!widget->isVisibleTo(section.frame))
                    continue;
                const QString text = searchableText(widget).toLower();
                if (text.isEmpty())
                    continue;
                haystack += QLatin1Char('\n') + text;
                for (const QString &term : terms) {
                    if (text.contains(term)) {
                        sectionHits << widget;
                        break;
                    }
                }
            }
            for (const QString &term : terms) {
                if (!haystack.contains(term)) {
                    keep = false;
                    break;
                }
            }
        }
        section.frame->setVisible(keep);
        // Hiding the lit row makes the list move its current row by itself;
        // that is not the person choosing a section.
        m_syncingNav = true;
        m_nav->item(i)->setHidden(!keep);
        m_syncingNav = false;
        if (keep) {
            ++shown;
            for (QWidget *hit : std::as_const(sectionHits))
                m_hits << hit;
        }
    }
    m_noMatches->setText(QStringLiteral("No settings match “%1”.").arg(query));
    m_noMatches->setVisible(!terms.isEmpty() && shown == 0);
    m_hitOverlay->setVisible(!m_hits.isEmpty());
    m_hitOverlay->update();
    // Start the narrowed page from its top, so the first match is in view.
    m_jumping = true;
    m_scroll->verticalScrollBar()->setValue(0);
    m_jumping = false;
    m_pendingJump = -1;
    scheduleRefresh();
}

void SettingsSectionsView::scheduleRefresh()
{
    if (m_refreshQueued)
        return;
    m_refreshQueued = true;
    QTimer::singleShot(0, this, [this] {
        m_refreshQueued = false;
        refreshGeometry();
    });
}

void SettingsSectionsView::refreshGeometry()
{
    buildPendingAbove(-1);
    int last = -1;
    for (int i = count() - 1; i >= 0; --i) {
        if (!m_sections.at(i).frame->isHidden()) {
            last = i;
            break;
        }
    }
    const int tail =
        last < 0 ? 0
                 : qMax(0, m_scroll->viewport()->height() -
                               m_sections.at(last).frame->height());
    if (m_bottomSpacer->height() != tail)
        m_bottomSpacer->setFixedHeight(tail);
    if (property(kNextLayout).toBool()) {
        bool first = true;
        for (const Section &section : std::as_const(m_sections)) {
            if (section.frame->isHidden())
                continue;
            if (auto *rule = section.frame->findChild<QFrame *>(
                    QStringLiteral("settingsSectionRule"), Qt::FindDirectChildrenOnly);
                rule && rule->isHidden() != first)
                rule->setVisible(!first);
            first = false;
        }
    }

    for (const Section &section : std::as_const(m_sections)) {
        QWidget *frame = section.frame;
        int wanted = 0;
        if (frame && !frame->isHidden() && frame->width() > 0 &&
            section.body && section.body->isVisibleTo(frame)) {
            if (QLayout *layout = frame->layout();
                layout && layout->hasHeightForWidth())
                wanted = layout->totalHeightForWidth(frame->width());
        }
        if (frame && frame->minimumHeight() != wanted)
            frame->setMinimumHeight(wanted);
    }

    m_hitOverlay->setGeometry(m_content->rect());
    m_hitOverlay->raise();
    m_hitOverlay->update();

    if (m_pendingJump >= 0 && isVisible()) {
        const int jump = m_pendingJump;
        m_pendingJump = -1;
        scrollToSection(jump);
    } else if (!m_jumping) {
        updateCurrentFromScroll();
    }
    if (m_pendingReveal && m_pendingJump < 0 && isVisible()) {
        QWidget *target = m_pendingReveal;
        m_pendingReveal = nullptr;
        scrollToWidget(target);
    }
    updateCurrentSub();
    updateRevealed();
}

void SettingsSectionsView::updateCurrentFromScroll()
{
    const int line = m_scroll->verticalScrollBar()->value() + kReadingLine;
    int current = -1;
    for (int i = 0; i < count(); ++i) {
        const QWidget *frame = m_sections.at(i).frame;
        if (frame->isHidden())
            continue;
        if (current >= 0 && frame->y() > line)
            break;
        current = i;
    }
    if (current >= 0)
        setCurrent(current);
    updateCurrentSub();
}

void SettingsSectionsView::updateRevealed()
{
    if (!isVisible())
        return;
    const int top = m_scroll->verticalScrollBar()->value();
    const int bottom = top + m_scroll->viewport()->height();
    for (int i = 0; i < count(); ++i) {
        Section &section = m_sections[i];
        const QWidget *frame = section.frame;
        const bool inView = !frame->isHidden() && section.body &&
                            section.body->isVisibleTo(frame) && frame->y() < bottom &&
                            frame->y() + frame->height() > top;
        if (inView && !section.inView) {
            section.inView = true;
            emit sectionRevealed(i);
        } else if (!inView) {
            section.inView = false;
        }
    }
}

void SettingsSectionsView::scrollToSection(int index)
{
    if (index < 0 || index >= count())
        return;
    buildPendingAbove(index);
    // A search just cleared or a section just shown has posted its relayout
    // but not run it; scrolling first would clamp to the old, shorter page.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
    if (QLayout *layout = m_content->layout())
        layout->activate();
    m_jumping = true;
    m_scroll->verticalScrollBar()->setValue(m_sections.at(index).frame->y());
    m_jumping = false;
    setCurrent(index);
    updateCurrentSub();
    updateRevealed();
}

void SettingsSectionsView::scrollToWidget(QWidget *target)
{
    if (!target)
        return;
    m_scroll->ensureWidgetVisible(target, 40, 80);
}

void SettingsSectionsView::setCurrent(int index)
{
    if (m_current == index) {
        syncNavRow();
        return;
    }
    m_current = index;
    syncNavRow();
    emit currentChanged(index);
}

void SettingsSectionsView::syncNavRow()
{
    if (m_current < 0 || m_current >= m_nav->count())
        return;
    QListWidgetItem *item = m_nav->item(m_current);
    if (m_nav->currentRow() == m_current && item->isSelected())
        return;
    m_syncingNav = true;
    m_nav->setCurrentRow(m_current, QItemSelectionModel::ClearAndSelect);
    m_nav->scrollToItem(item);
    m_syncingNav = false;
}
