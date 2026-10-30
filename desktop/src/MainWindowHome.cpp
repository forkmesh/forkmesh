#include "HomeBoard.h"
#include "HomeLayout.h"
#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "RailGroups.h"
#include "SystemStats.h"

#include <QDateTime>
#include <QDir>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QKeySequence>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QShortcut>
#include <QStackedWidget>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QTransform>
#include <QVBoxLayout>
#include <QWidgetAction>

#include <functional>

#ifdef Q_OS_UNIX
#include <pwd.h>
#include <unistd.h>
#endif

using namespace forkmesh::ui;

namespace {

constexpr int kHomeTickMs = 1000;
constexpr int kHomeSearchRevealMs = 160;
const QString kHomePagesSetting = QStringLiteral("ui/homePages");
constexpr int kHomeContentMaxWidth = 1180;
constexpr int kHomeComposerMaxWidth = 800;
constexpr int kHomeNeedsWidth = 360;
constexpr int kHomeNeedsRows = 5;
constexpr int kHomeSuggestions = 4;
constexpr int kPickerWidth = 340;
constexpr int kPickerMaxListHeight = 380;
constexpr int kPickerHeadingHeight = 28;
constexpr int kPickerRowHeight = 36;
constexpr int kPickerTwoLineHeight = 50;

enum PickerRole {
    PickerSubtitle = Qt::UserRole + 40,
    PickerIcon,
    PickerCurrent,
    PickerHeading,
    PickerValue,
};

struct PickerRow {
    QString heading;
    QString title;
    QString subtitle;
    QString icon;
    QVariant value;
    bool current = false;
};

QString plural(int count, const QString &noun)
{
    return QStringLiteral("%1 %2%3")
        .arg(count)
        .arg(noun, count == 1 ? QString() : QStringLiteral("s"));
}

QString tooltipSummary(const QString &tooltip)
{
    const QString line = tooltip.section(QLatin1Char('\n'), 0, 0);
    const QString dash = QString::fromUtf8(" \xE2\x80\x94 ");
    int at = line.indexOf(dash);
    int skip = dash.size();
    if (at < 0) {
        at = line.indexOf(QLatin1String(" - "));
        skip = 3;
    }
    if (at < 0)
        return QString();
    QString rest = line.mid(at + skip).trimmed();
    int cut = rest.size();
    for (const QChar stop : {QLatin1Char(','), QLatin1Char(';'),
                             QLatin1Char(':'), QLatin1Char('.')}) {
        const int found = rest.indexOf(stop);
        if (found > 0)
            cut = qMin(cut, found);
    }
    rest.truncate(cut);
    if (!rest.isEmpty())
        rest[0] = rest.at(0).toUpper();
    return rest;
}

QString firstNameOf(const QString &account)
{
    QString name = account.section(QLatin1Char('@'), 0, 0).trimmed();
    for (const QChar stop : {QLatin1Char('.'), QLatin1Char('_'),
                             QLatin1Char('-'), QLatin1Char(' ')}) {
        const int at = name.indexOf(stop);
        if (at > 0)
            name.truncate(at);
    }
    if (name.isEmpty() || !name.at(0).isLetter())
        return QString();
    name = name.toLower();
    name[0] = name.at(0).toUpper();
    return name;
}

QString systemFirstName()
{
#ifdef Q_OS_UNIX
    const passwd *entry = getpwuid(getuid());
    if (!entry || !entry->pw_gecos)
        return QString();
    const QStringList words =
        QString::fromLocal8Bit(entry->pw_gecos)
            .section(QLatin1Char(','), 0, 0)
            .split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (int i = 0; i < words.size(); ++i) {
        const QString &word = words.at(i);
        const bool abbreviation =
            word.endsWith(QLatin1Char('.')) || word.size() <= 2;
        if (abbreviation && i + 1 < words.size())
            continue;
        if (word.isEmpty() || !word.at(0).isLetter())
            return QString();
        return word.at(0).toUpper() + word.mid(1);
    }
#endif
    return QString();
}

QString greetingNameFor(const QString &account)
{
    const QString system = systemFirstName();
    if (!system.isEmpty())
        return system;
    QString name = firstNameOf(account);
    while (!name.isEmpty() && name.back().isDigit())
        name.chop(1);
    return name;
}

QString providerTitle(const QString &provider, const QString &fallback)
{
    if (provider == QLatin1String("claude-code"))
        return QStringLiteral("Claude");
    if (agentIsCodexProvider(provider))
        return QStringLiteral("Codex");
    if (agentIsCloudflareAiProvider(provider))
        return QStringLiteral("Workers AI");
    if (agentIsLocalLlamaProvider(provider))
        return QStringLiteral("Llama");
    if (provider.startsWith(QLatin1String("claude")))
        return QStringLiteral("Claude API");
    if (provider.startsWith(QLatin1String("openai")))
        return QStringLiteral("OpenAI API");
    return fallback;
}

QString tildePath(const QString &path)
{
    const QString home = QDir::homePath();
    if (!home.isEmpty() && path.startsWith(home))
        return QLatin1Char('~') + path.mid(home.size());
    return path;
}

QString providerSubtitle(const QString &provider)
{
    if (provider == QLatin1String("claude-code"))
        return QString::fromUtf8("Anthropic \xC2\xB7 Claude Code");
    if (agentIsCodexProvider(provider))
        return QString::fromUtf8("OpenAI \xC2\xB7 Codex CLI");
    if (agentIsCloudflareAiProvider(provider))
        return QString::fromUtf8("Cloudflare \xC2\xB7 Workers AI");
    if (agentIsLocalLlamaProvider(provider))
        return QString::fromUtf8("Local \xC2\xB7 on this node");
    if (provider.startsWith(QLatin1String("claude")))
        return QString::fromUtf8("Anthropic \xC2\xB7 API");
    if (provider.startsWith(QLatin1String("openai")))
        return QString::fromUtf8("OpenAI \xC2\xB7 API");
    return QString();
}

QString modeIconOf(const QString &mode)
{
    if (mode.compare(QLatin1String("Ask"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("hand");
    if (mode.compare(QLatin1String("Plan"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("eye");
    if (mode.compare(QLatin1String("Edit"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("pencil");
    return QStringLiteral("unlock");
}

QPixmap dotPixmap(const QColor &color, int px)
{
    const qreal ratio = qApp ? qApp->devicePixelRatio() : 1.0;
    QPixmap pixmap(QSize(px, px) * ratio);
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawEllipse(QRectF(0, 0, px, px));
    return pixmap;
}

QPixmap upArrowPixmap(const QColor &color, int px)
{
    return tintedOcticonPixmap(QStringLiteral("arrow-left"), color, px)
        .transformed(QTransform().rotate(90), Qt::SmoothTransformation);
}

class HomePickerDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override
    {
        if (index.data(PickerHeading).toBool())
            return QSize(option.rect.width(), kPickerHeadingHeight);
        return QSize(option.rect.width(),
                     index.data(PickerSubtitle).toString().isEmpty()
                         ? kPickerRowHeight
                         : kPickerTwoLineHeight);
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        const QRect rect = option.rect.adjusted(0, 1, 0, -1);
        if (index.data(PickerHeading).toBool()) {
            QFont font = option.font;
            font.setPixelSize(11);
            font.setWeight(QFont::Medium);
            font.setLetterSpacing(QFont::AbsoluteSpacing, 1.0);
            painter->setFont(font);
            painter->setPen(Theme::color(Theme::Role::FgSubtle));
            painter->drawText(rect.adjusted(10, 0, -10, -4),
                              Qt::AlignLeft | Qt::AlignBottom,
                              index.data().toString().toUpper());
            painter->restore();
            return;
        }
        const bool hovered = option.state & QStyle::State_MouseOver;
        const bool selected = option.state & QStyle::State_Selected;
        if (hovered || selected) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(Theme::color(selected ? Theme::Role::SurfaceActive
                                                    : Theme::Role::SurfaceRaised));
            painter->drawRoundedRect(QRectF(rect), 8, 8);
        }
        int x = rect.left() + 10;
        const QString icon = index.data(PickerIcon).toString();
        if (!icon.isEmpty()) {
            painter->drawPixmap(
                QRect(x, rect.center().y() - 8, 16, 16),
                tintedOcticonPixmap(icon, Theme::color(Theme::Role::FgMuted), 16));
            x += 28;
        }
        const bool current = index.data(PickerCurrent).toBool();
        const int right = rect.right() - (current ? 34 : 10);
        const QString title = index.data().toString();
        const QString subtitle = index.data(PickerSubtitle).toString();
        QFont titleFont = option.font;
        titleFont.setPixelSize(14);
        QFont subtitleFont = option.font;
        subtitleFont.setPixelSize(12);
        painter->setFont(titleFont);
        painter->setPen(Theme::color(Theme::Role::Fg));
        const int width = qMax(0, right - x);
        if (subtitle.isEmpty()) {
            painter->drawText(QRect(x, rect.top(), width, rect.height()),
                              Qt::AlignLeft | Qt::AlignVCenter,
                              QFontMetrics(titleFont).elidedText(
                                  title, Qt::ElideRight, width));
        } else {
            painter->drawText(QRect(x, rect.top() + 6, width, 20),
                              Qt::AlignLeft | Qt::AlignVCenter,
                              QFontMetrics(titleFont).elidedText(
                                  title, Qt::ElideRight, width));
            painter->setFont(subtitleFont);
            painter->setPen(Theme::color(Theme::Role::FgMuted));
            painter->drawText(QRect(x, rect.top() + 26, width, 16),
                              Qt::AlignLeft | Qt::AlignVCenter,
                              QFontMetrics(subtitleFont).elidedText(
                                  subtitle, Qt::ElideMiddle, width));
        }
        if (current)
            painter->drawPixmap(
                QRect(right + 10, rect.center().y() - 8, 16, 16),
                tintedOcticonPixmap(QStringLiteral("check"),
                                    Theme::color(Theme::Role::AccentFg), 16));
        painter->restore();
    }
};

class HomeKeyFilter : public QObject
{
public:
    HomeKeyFilter(QObject *parent, std::function<bool(QEvent *)> handler)
        : QObject(parent), m_handler(std::move(handler))
    {
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (m_handler && m_handler(event))
            return true;
        return QObject::eventFilter(watched, event);
    }

private:
    std::function<bool(QEvent *)> m_handler;
};

void showHomePicker(QWidget *anchor, const QString &placeholder,
                    const QVector<PickerRow> &rows,
                    const std::function<void(const QVariant &)> &pick,
                    const QString &footerText = QString(),
                    const std::function<void()> &footerPick = {},
                    const std::function<QWidget *(QWidget *, QMenu *)> &extra = {})
{
    if (!anchor)
        return;
    auto *menu = new QMenu(anchor);
    menu->setObjectName(QStringLiteral("homePickerMenu"));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    auto *body = new QWidget(menu);
    body->setObjectName(QStringLiteral("homePickerBody"));
    body->setFixedWidth(kPickerWidth);
    auto *layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    QLineEdit *search = nullptr;
    if (!placeholder.isEmpty()) {
        search = new QLineEdit(body);
        search->setObjectName(QStringLiteral("homePickerSearch"));
        search->setPlaceholderText(placeholder);
        search->addAction(QIcon(tintedOcticonPixmap(
                              QStringLiteral("search"),
                              Theme::color(Theme::Role::FgMuted), 16)),
                          QLineEdit::LeadingPosition);
        layout->addWidget(search);
    }

    auto *list = new QListWidget(body);
    list->setObjectName(QStringLiteral("homePickerList"));
    list->setItemDelegate(new HomePickerDelegate(list));
    list->setMouseTracking(true);
    list->setFrameShape(QFrame::NoFrame);
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    list->setAttribute(Qt::WA_MacShowFocusRect, false);
    list->viewport()->setAutoFillBackground(false);
    int height = 0;
    QListWidgetItem *currentItem = nullptr;
    for (const PickerRow &row : rows) {
        auto *item = new QListWidgetItem(
            row.heading.isEmpty() ? row.title : row.heading, list);
        if (!row.heading.isEmpty()) {
            item->setData(PickerHeading, true);
            item->setFlags(Qt::NoItemFlags);
            height += kPickerHeadingHeight;
            continue;
        }
        item->setData(PickerSubtitle, row.subtitle);
        item->setData(PickerIcon, row.icon);
        item->setData(PickerCurrent, row.current);
        item->setData(PickerValue, row.value);
        if (row.current && !currentItem)
            currentItem = item;
        height += row.subtitle.isEmpty() ? kPickerRowHeight : kPickerTwoLineHeight;
    }
    list->setFixedHeight(qMin(height + 4, kPickerMaxListHeight));
    if (rows.isEmpty())
        list->hide();
    layout->addWidget(list);

    if (extra)
        if (QWidget *widget = extra(body, menu))
            layout->addWidget(widget);

    if (!footerText.isEmpty()) {
        auto *rule = new QFrame(body);
        rule->setObjectName(QStringLiteral("homePickerRule"));
        rule->setFixedHeight(1);
        layout->addWidget(rule);
        auto *footer = new QPushButton(footerText, body);
        footer->setObjectName(QStringLiteral("homePickerFooter"));
        footer->setCursor(Qt::PointingHandCursor);
        footer->setIcon(QIcon(tintedOcticonPixmap(
            QStringLiteral("plus"), Theme::color(Theme::Role::FgMuted), 16)));
        layout->addWidget(footer);
        QObject::connect(footer, &QPushButton::clicked, menu,
                         [menu, footerPick] {
                             const std::function<void()> run = footerPick;
                             menu->close();
                             if (run)
                                 run();
                         });
    }

    auto *action = new QWidgetAction(menu);
    action->setDefaultWidget(body);
    menu->addAction(action);

    const auto choose = [menu, pick](QListWidgetItem *item) {
        if (!item || !(item->flags() & Qt::ItemIsEnabled) || item->isHidden())
            return;
        const QVariant value = item->data(PickerValue);
        const std::function<void(const QVariant &)> run = pick;
        menu->close();
        if (run)
            run(value);
    };
    QObject::connect(list, &QListWidget::itemClicked, menu, choose);
    QObject::connect(list, &QListWidget::itemActivated, menu, choose);
    if (search) {
        QObject::connect(search, &QLineEdit::textChanged, list,
                         [list](const QString &text) {
                             const QString needle = text.trimmed();
                             QListWidgetItem *first = nullptr;
                             for (int i = 0; i < list->count(); ++i) {
                                 QListWidgetItem *item = list->item(i);
                                 const bool heading =
                                     item->data(PickerHeading).toBool();
                                 const bool match =
                                     needle.isEmpty() ||
                                     (!heading &&
                                      (item->text().contains(needle, Qt::CaseInsensitive) ||
                                       item->data(PickerSubtitle).toString().contains(
                                           needle, Qt::CaseInsensitive)));
                                 item->setHidden(!match);
                                 if (match && !heading && !first)
                                     first = item;
                             }
                             list->setCurrentItem(needle.isEmpty() ? nullptr : first);
                         });
        QObject::connect(search, &QLineEdit::returnPressed, list,
                         [list, choose] {
                             QListWidgetItem *item = list->currentItem();
                             if (!item || item->isHidden())
                                 for (int i = 0; i < list->count(); ++i)
                                     if (!list->item(i)->isHidden() &&
                                         (list->item(i)->flags() & Qt::ItemIsEnabled)) {
                                         item = list->item(i);
                                         break;
                                     }
                             choose(item);
                         });
        search->installEventFilter(new HomeKeyFilter(search, [list](QEvent *event) {
            if (event->type() != QEvent::KeyPress)
                return false;
            const int key = static_cast<QKeyEvent *>(event)->key();
            if (key != Qt::Key_Down && key != Qt::Key_Up)
                return false;
            int row = list->currentRow();
            const int step = key == Qt::Key_Down ? 1 : -1;
            for (int i = row + step; i >= 0 && i < list->count(); i += step) {
                QListWidgetItem *item = list->item(i);
                if (!item->isHidden() && (item->flags() & Qt::ItemIsEnabled)) {
                    list->setCurrentItem(item);
                    list->scrollToItem(item);
                    break;
                }
            }
            return true;
        }));
    }
    if (currentItem)
        list->scrollToItem(currentItem);
    menu->popup(anchor->mapToGlobal(QPoint(0, anchor->height() + 6)));
    if (search)
        search->setFocus(Qt::PopupFocusReason);
}

class HomeHero : public QObject
{
public:
    using QObject::QObject;

    QPlainTextEdit *input = nullptr;
    QPushButton *attach = nullptr;
    QPushButton *repo = nullptr;
    QPushButton *branch = nullptr;
    QPushButton *agent = nullptr;
    QPushButton *mode = nullptr;
    QPushButton *model = nullptr;
    QPushButton *mic = nullptr;
    QPushButton *send = nullptr;
    QWidget *suggestions = nullptr;
    QHBoxLayout *suggestionRow = nullptr;
    QString suggestionSignature;
    QWidget *needs = nullptr;
    QVBoxLayout *needsLayout = nullptr;
    QPushButton *needsAll = nullptr;
    QString needsSignature;
    QString branchChoice;
    int branchRepo = -1;
    QString styleSignature;
    std::function<void()> refresh;
    std::function<QString()> headline;
};

HomeHero *heroOf(const QWidget *board)
{
    if (!board)
        return nullptr;
    return static_cast<HomeHero *>(
        board->property("homeHero").value<QObject *>());
}

QPushButton *makeComposerChip(const QString &icon, const QString &name,
                              bool chevron)
{
    auto *button = new QPushButton;
    button->setObjectName(name);
    button->setProperty("homeRole", QStringLiteral("chip"));
    button->setCursor(Qt::PointingHandCursor);
    button->setFocusPolicy(Qt::TabFocus);
    auto *row = new QHBoxLayout(button);
    row->setContentsMargins(8, 0, 8, 0);
    row->setSpacing(6);
    if (!icon.isEmpty()) {
        auto *glyph = new QLabel(button);
        glyph->setObjectName(QStringLiteral("homeChipIcon"));
        glyph->setProperty("homeIcon", icon);
        glyph->setPixmap(tintedOcticonPixmap(
            icon, Theme::color(Theme::Role::FgMuted), 16));
        row->addWidget(glyph);
    }
    auto *text = new QLabel(button);
    text->setObjectName(QStringLiteral("homeChipText"));
    row->addWidget(text);
    auto *detail = new QLabel(button);
    detail->setObjectName(QStringLiteral("homeChipDetail"));
    detail->hide();
    row->addWidget(detail);
    if (chevron) {
        auto *arrow = new QLabel(button);
        arrow->setObjectName(QStringLiteral("homeChipChevron"));
        arrow->setPixmap(tintedOcticonPixmap(
            QStringLiteral("chevron-down"), Theme::color(Theme::Role::FgSubtle), 12));
        row->addWidget(arrow);
    }
    for (QLabel *label : button->findChildren<QLabel *>())
        label->setAttribute(Qt::WA_TransparentForMouseEvents);
    button->setFixedHeight(32);
    return button;
}

void setComposerChip(QPushButton *button, const QString &text,
                     const QString &detail = QString(),
                     const QString &icon = QString())
{
    if (!button)
        return;
    bool changed = false;
    if (auto *label = button->findChild<QLabel *>(QStringLiteral("homeChipText"))) {
        if (label->text() != text) {
            label->setText(text);
            changed = true;
        }
        label->setVisible(!text.isEmpty());
    }
    if (auto *label = button->findChild<QLabel *>(QStringLiteral("homeChipDetail"))) {
        if (label->text() != detail) {
            label->setText(detail);
            changed = true;
        }
        label->setVisible(!detail.isEmpty());
    }
    if (!icon.isEmpty())
        if (auto *glyph = button->findChild<QLabel *>(QStringLiteral("homeChipIcon")))
            if (glyph->property("homeIcon").toString() != icon) {
                glyph->setProperty("homeIcon", icon);
                glyph->setPixmap(tintedOcticonPixmap(
                    icon, Theme::color(Theme::Role::FgMuted), 16));
            }
    if (changed || !button->property("homeSized").toBool()) {
        button->setProperty("homeSized", true);
        button->layout()->invalidate();
        button->layout()->activate();
        button->setFixedWidth(button->layout()->sizeHint().width());
    }
}

void clearLayout(QLayout *layout)
{
    while (QLayoutItem *item = layout->takeAt(0)) {
        if (QWidget *widget = item->widget())
            widget->deleteLater();
        delete item;
    }
}

HomeBoardStyle nextBoardStyle()
{
    HomeBoardStyle style;
    style.flat = true;
    style.page = Theme::color(Theme::Role::Bg);
    style.plate = Theme::color(Theme::Role::Surface);
    style.plateBorder = Theme::color(Theme::Role::BorderStrong);
    style.plateHoverBorder = Theme::color(Theme::Role::FgSubtle);
    style.icon = Theme::color(Theme::Role::FgSecondary);
    style.title = Theme::color(Theme::Role::Fg);
    style.detail = Theme::color(Theme::Role::FgSubtle);
    style.badge = Theme::color(Theme::Role::Accent);
    style.onBadge = Theme::color(Theme::Role::OnAccent);
    style.ring = Theme::color(Theme::Role::Success);
    style.success = Theme::color(Theme::Role::Success);
    style.warning = Theme::color(Theme::Role::Warning);
    style.danger = Theme::color(Theme::Role::Danger);
    return style;
}

QLabel *sectionHeading(const QString &text)
{
    auto *label = new QLabel(text.toUpper());
    label->setProperty("homeRole", QStringLiteral("sectionHeading"));
    return label;
}

}

QWidget *MainWindow::buildHomeGridSection()
{
    const bool next = Theme::isNext();
    auto *page = new QWidget;
    page->setObjectName(QStringLiteral("homeSection"));
    auto *outer = new QVBoxLayout(page);
    if (next) {
        outer->setContentsMargins(0, 0, 0, 0);
        outer->setSpacing(0);
    } else {
        outer->setContentsMargins(20, 18, 20, 8);
        outer->setSpacing(6);
    }

    m_homeGridGreeting = new QLabel(QStringLiteral("Home"));
    m_homeGridGreeting->setObjectName(QStringLiteral("pageTitle"));

    m_homeGridHeadline = new QLabel;
    m_homeGridHeadline->setObjectName(QStringLiteral("mutedLabel"));
    m_homeGridHeadline->setTextInteractionFlags(Qt::TextSelectableByMouse);

    m_homeGridSearch = new QWidget;
    m_homeGridSearch->setObjectName(QStringLiteral("homeSearchRow"));
    auto *searchRow = new QHBoxLayout(m_homeGridSearch);
    searchRow->setContentsMargins(0, 4, 0, 4);
    m_homeGridFilter = new QLineEdit;
    m_homeGridFilter->setObjectName(QStringLiteral("homeFilter"));
    m_homeGridFilter->setPlaceholderText(QStringLiteral("Search pages…"));
    m_homeGridFilter->setClearButtonEnabled(true);
    m_homeGridFilter->setMinimumWidth(260);
    m_homeGridFilter->setMaximumWidth(520);
    m_homeGridFilter->setToolTip(QStringLiteral(
        "Show only the pages whose name, group or status contains this text; "
        "Enter opens the first one, Esc closes the search"));
    searchRow->addStretch();
    searchRow->addWidget(m_homeGridFilter, 1);
    searchRow->addStretch();
    m_homeGridSearch->hide();
    connect(m_homeGridFilter, &QLineEdit::textChanged, this,
            [this](const QString &text) {
                if (m_homeGridBoard)
                    m_homeGridBoard->setFilter(text);
            });
    connect(m_homeGridFilter, &QLineEdit::returnPressed, this, [this] {
        if (m_homeGridBoard)
            activateHomeGridTile(m_homeGridBoard->firstVisibleKey());
    });
    connect(m_homeGridFilter, &QLineEdit::editingFinished, this, [this] {
        QTimer::singleShot(0, this, [this] {
            if (m_homeGridFilter && !m_homeGridFilter->hasFocus() &&
                m_homeGridFilter->text().trimmed().isEmpty())
                hideHomeGridSearch();
        });
    });
    auto *escape = new QShortcut(QKeySequence(Qt::Key_Escape), m_homeGridFilter);
    escape->setContext(Qt::WidgetShortcut);
    connect(escape, &QShortcut::activated, this, [this] { hideHomeGridSearch(); });

    m_homeGridBoard = new HomeBoard;
    m_homeGridBoard->setIconProvider(
        [](const QString &name, const QColor &color, int size) {
            return tintedOcticonPixmap(name.isEmpty() ? QStringLiteral("link")
                                                      : name,
                                       color, size);
        });
    m_homeGridBoard->setOnActivate(
        [this](const QString &key) { activateHomeGridTile(key); });
    m_homeGridBoard->setTileMenu([this](QMenu *menu, const QString &key) {
        addHomeTilePackageActions(menu, key);
        addHomeTileRailActions(menu, key);
    });
    m_homeGridBoard->setSavedPages(HomeLayout::fromJson(
        QSettings().value(kHomePagesSetting).toString()));
    m_homeGridBoard->setOnLayoutChanged([](const HomeLayout::Pages &pages) {
        QSettings settings;
        if (pages.isEmpty())
            settings.remove(kHomePagesSetting);
        else
            settings.setValue(kHomePagesSetting, HomeLayout::toJson(pages));
    });

    auto *scroll = new QScrollArea;
    scroll->setObjectName(QStringLiteral("homeScroll"));
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->viewport()->setAutoFillBackground(false);

    m_homeGridDots = new HomePageDots;
    m_homeGridDots->setOnPick([this](int index) {
        if (m_homeGridBoard)
            m_homeGridBoard->setPage(index);
    });

    if (next) {
        auto *hero = new HomeHero(page);
        page->setProperty("homeComposer", true);
        m_homeGridBoard->setProperty("homeHero",
                                     QVariant::fromValue<QObject *>(hero));
        m_homeGridBoard->setStyle(nextBoardStyle());
        m_homeGridGreeting->setProperty("homeRole", QStringLiteral("greeting"));
        m_homeGridGreeting->setAlignment(Qt::AlignHCenter);
        m_homeGridHeadline->setProperty("homeRole", QStringLiteral("headline"));
        m_homeGridHeadline->setAlignment(Qt::AlignHCenter);

        auto *content = new QWidget;
        content->setObjectName(QStringLiteral("homeContent"));
        auto *centre = new QHBoxLayout(content);
        centre->setContentsMargins(32, 48, 32, 28);
        centre->setSpacing(0);
        auto *inner = new QWidget;
        inner->setObjectName(QStringLiteral("homeInner"));
        inner->setMaximumWidth(kHomeContentMaxWidth);
        centre->addStretch(1);
        centre->addWidget(inner, 1000);
        centre->addStretch(1);
        auto *column = new QVBoxLayout(inner);
        column->setContentsMargins(0, 0, 0, 0);
        column->setSpacing(0);
        column->addWidget(m_homeGridGreeting);
        column->addSpacing(8);
        column->addWidget(m_homeGridHeadline);
        column->addSpacing(36);

        auto *composer = new QFrame;
        composer->setObjectName(QStringLiteral("homeComposer"));
        composer->setAttribute(Qt::WA_StyledBackground, true);
        composer->setMaximumWidth(kHomeComposerMaxWidth);
        composer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        auto *composerColumn = new QVBoxLayout(composer);
        composerColumn->setContentsMargins(14, 12, 10, 10);
        composerColumn->setSpacing(6);
        hero->input = new QPlainTextEdit;
        hero->input->setObjectName(QStringLiteral("homeComposerInput"));
        hero->input->setPlaceholderText(
            QStringLiteral("What should your agents work on?"));
        hero->input->setFrameShape(QFrame::NoFrame);
        hero->input->setTabChangesFocus(true);
        hero->input->setFixedHeight(64);
        hero->input->viewport()->setAutoFillBackground(false);
        hero->input->setToolTip(QStringLiteral(
            "Starts a new agent session with this prompt; Shift+Enter adds a line"));
        composerColumn->addWidget(hero->input);

        auto *bar = new QHBoxLayout;
        bar->setContentsMargins(0, 0, 0, 0);
        bar->setSpacing(2);
        hero->attach = makeComposerChip(QStringLiteral("plus"),
                                        QStringLiteral("homeComposerAttach"), false);
        hero->attach->setToolTip(QStringLiteral("Attach files to the prompt"));
        hero->repo = makeComposerChip(QStringLiteral("repo"),
                                      QStringLiteral("homeComposerRepo"), true);
        hero->repo->setToolTip(QStringLiteral("Repository the session works in"));
        hero->branch = makeComposerChip(QStringLiteral("git-branch"),
                                        QStringLiteral("homeComposerBranch"), true);
        hero->branch->setToolTip(QStringLiteral("Branch the session starts from"));
        hero->agent = makeComposerChip(QStringLiteral("sparkle"),
                                       QStringLiteral("homeComposerAgent"), true);
        hero->agent->setToolTip(QStringLiteral("Agent that runs the session"));
        hero->mode = makeComposerChip(QStringLiteral("unlock"),
                                      QStringLiteral("homeComposerMode"), false);
        hero->mode->setToolTip(QStringLiteral("What the agent may do without asking"));
        hero->model = makeComposerChip(QString(), QStringLiteral("homeComposerModel"),
                                       false);
        hero->model->setToolTip(QStringLiteral("Model and effort"));
        hero->mic = makeComposerChip(QStringLiteral("mic"),
                                     QStringLiteral("homeComposerMic"), false);
        hero->mic->setToolTip(QStringLiteral("Dictate the prompt"));
        hero->send = new QPushButton;
        hero->send->setObjectName(QStringLiteral("homeComposerSend"));
        hero->send->setCursor(Qt::PointingHandCursor);
        hero->send->setFixedSize(32, 32);
        hero->send->setIconSize(QSize(16, 16));
        hero->send->setToolTip(QStringLiteral("Start a session (Enter)"));
        bar->addWidget(hero->attach);
        bar->addWidget(hero->repo);
        bar->addWidget(hero->branch);
        bar->addWidget(hero->agent);
        bar->addStretch(1);
        bar->addWidget(hero->mode);
        bar->addWidget(hero->model);
        bar->addWidget(hero->mic);
        bar->addSpacing(4);
        bar->addWidget(hero->send);
        composerColumn->addLayout(bar);

        auto *composerRow = new QHBoxLayout;
        composerRow->addStretch(1);
        composerRow->addWidget(composer, 1000);
        composerRow->addStretch(1);
        column->addLayout(composerRow);
        column->addSpacing(14);

        hero->suggestions = new QWidget;
        hero->suggestions->setObjectName(QStringLiteral("homeSuggestions"));
        hero->suggestionRow = new QHBoxLayout(hero->suggestions);
        hero->suggestionRow->setContentsMargins(0, 0, 0, 0);
        hero->suggestionRow->setSpacing(8);
        auto *suggestionLine = new QHBoxLayout;
        suggestionLine->addStretch(1);
        suggestionLine->addWidget(hero->suggestions);
        suggestionLine->addStretch(1);
        column->addLayout(suggestionLine);
        column->addSpacing(10);
        column->addWidget(m_homeGridSearch);
        column->addSpacing(40);

        auto *body = new QHBoxLayout;
        body->setContentsMargins(0, 0, 0, 0);
        body->setSpacing(40);
        auto *apps = new QVBoxLayout;
        apps->setContentsMargins(0, 0, 0, 0);
        apps->setSpacing(10);
        auto *appsHeading = sectionHeading(QStringLiteral("Apps"));
        appsHeading->setContentsMargins(HomeBoard::kGap, 0, 0, 0);
        apps->addWidget(appsHeading);
        apps->addWidget(m_homeGridBoard);
        apps->addWidget(m_homeGridDots);
        apps->addStretch(1);
        body->addLayout(apps, 1);

        auto *needsColumn = new QWidget;
        needsColumn->setObjectName(QStringLiteral("homeNeedsColumn"));
        needsColumn->setFixedWidth(kHomeNeedsWidth);
        auto *needsLayout = new QVBoxLayout(needsColumn);
        needsLayout->setContentsMargins(0, 0, 0, 0);
        needsLayout->setSpacing(10);
        auto *needsHeader = new QHBoxLayout;
        needsHeader->setContentsMargins(0, 0, 0, 0);
        needsHeader->addWidget(sectionHeading(QStringLiteral("Needs you")));
        needsHeader->addStretch(1);
        hero->needsAll = new QPushButton;
        hero->needsAll->setObjectName(QStringLiteral("homeNeedsAll"));
        hero->needsAll->setProperty("homeRole", QStringLiteral("sectionLink"));
        hero->needsAll->setCursor(Qt::PointingHandCursor);
        hero->needsAll->setToolTip(QStringLiteral("Open every ping"));
        connect(hero->needsAll, &QPushButton::clicked, this,
                [this] { showNotifications(); });
        needsHeader->addWidget(hero->needsAll);
        needsLayout->addLayout(needsHeader);
        hero->needs = new QFrame;
        hero->needs->setObjectName(QStringLiteral("homeNeedsYou"));
        hero->needs->setAttribute(Qt::WA_StyledBackground, true);
        hero->needsLayout = new QVBoxLayout(hero->needs);
        hero->needsLayout->setContentsMargins(1, 1, 1, 1);
        hero->needsLayout->setSpacing(0);
        needsLayout->addWidget(hero->needs);
        needsLayout->addStretch(1);
        body->addWidget(needsColumn, 0, Qt::AlignTop);
        column->addLayout(body);
        column->addStretch(1);

        scroll->setWidget(content);
        outer->addWidget(scroll, 1);
        if (m_sectionStack)
            connect(m_sectionStack, &QStackedWidget::currentChanged, hero,
                    [this] { schedulePositionGlobalFooterOverlays(); });

        const auto refreshSend = [hero] {
            const bool ready = !hero->input->toPlainText().trimmed().isEmpty();
            hero->send->setEnabled(ready);
            hero->send->setIcon(QIcon(upArrowPixmap(
                Theme::color(ready ? Theme::Role::Bg : Theme::Role::FgSubtle), 16)));
        };
        refreshSend();
        connect(hero->input, &QPlainTextEdit::textChanged, hero, refreshSend);
        if (m_issueQuickAdd)
            connect(m_issueQuickAdd, &QPlainTextEdit::textChanged, hero,
                    [this, hero] {
                        const QString text = m_issueQuickAdd->toPlainText();
                        if (!text.isEmpty() && hero->input->toPlainText() != text &&
                            !hero->input->hasFocus()) {
                            hero->input->setPlainText(text);
                            hero->input->moveCursor(QTextCursor::End);
                        }
                    });

        const auto isAgentProvider = [](const QString &provider) {
            return !provider.isEmpty() && provider != QLatin1String("manual") &&
                   provider != kNotePromptProvider &&
                   provider != kTaskPromptProvider &&
                   provider != kActivityPubPromptProvider;
        };
        const auto currentProvider = [this, isAgentProvider]() -> QString {
            if (!m_quickAddAgentProvider)
                return QString();
            const QString provider = m_quickAddAgentProvider->currentData().toString();
            return isAgentProvider(provider) ? provider : QString();
        };
        const auto ensureAgentProvider = [this, currentProvider, isAgentProvider] {
            if (!m_quickAddAgentProvider || !currentProvider().isEmpty())
                return;
            int index = m_quickAddAgentProvider->findData(defaultAgentProvider());
            for (int i = 0; index < 0 && i < m_quickAddAgentProvider->count(); ++i)
                if (isAgentProvider(m_quickAddAgentProvider->itemData(i).toString()))
                    index = i;
            if (index >= 0)
                m_quickAddAgentProvider->setCurrentIndex(index);
        };
        const auto activeRepo = [this]() -> int {
            const int chosen = issuesRepoIndex();
            return chosen >= 0 ? chosen : resolveAgentRepositoryIndex(-1);
        };
        const auto repoPath = [this, activeRepo]() -> QString {
            const int index = activeRepo();
            return index >= 0 && index < m_repositories.size()
                       ? m_repositories.at(index).localPath
                       : QString();
        };
        const auto defaultBranchOf = [this](const QStringList &branches) {
            return branches.isEmpty() ? QString() : repoDefaultBranch(branches);
        };

        connect(hero->attach, &QPushButton::clicked, this, [this] {
            if (m_quickAddImageButton)
                m_quickAddImageButton->click();
        });
        connect(hero->mic, &QPushButton::clicked, this, [this, hero] {
            if (!m_quickAddMicButton)
                return;
            if (m_issueQuickAdd)
                m_issueQuickAdd->setPlainText(hero->input->toPlainText());
            hero->input->clearFocus();
            m_quickAddMicButton->click();
        });

        connect(hero->repo, &QPushButton::clicked, this, [this, hero, activeRepo] {
            if (!m_issuesRepoCombo)
                return;
            const int active = activeRepo();
            QVector<PickerRow> rows;
            for (int i = 0; i < m_issuesRepoCombo->count(); ++i) {
                bool ok = false;
                const int index = m_issuesRepoCombo->itemData(i).toInt(&ok);
                if (!ok || index < 0 || index >= m_repositories.size())
                    continue;
                PickerRow row;
                row.title = m_issuesRepoCombo->itemText(i);
                row.subtitle = tildePath(m_repositories.at(index).localPath);
                row.icon = QStringLiteral("repo");
                row.value = i;
                row.current = index == active;
                rows.append(row);
            }
            showHomePicker(
                hero->repo, QStringLiteral("Search repositories…"), rows,
                [this, hero](const QVariant &value) {
                    if (m_issuesRepoCombo)
                        m_issuesRepoCombo->setCurrentIndex(value.toInt());
                    hero->branchChoice.clear();
                    hero->refresh();
                },
                QStringLiteral("Open folder…"),
                [this] { promptAddRepository(); });
        });

        connect(hero->branch, &QPushButton::clicked, this,
                [this, hero, repoPath, defaultBranchOf, activeRepo] {
                    const QStringList branches = listRepoBranches(repoPath());
                    const QString fallback = defaultBranchOf(branches);
                    const QString current = hero->branchChoice.isEmpty()
                                                ? fallback
                                                : hero->branchChoice;
                    QVector<PickerRow> rows;
                    QStringList ordered = branches;
                    if (!fallback.isEmpty()) {
                        ordered.removeAll(fallback);
                        ordered.prepend(fallback);
                    }
                    for (const QString &name : std::as_const(ordered)) {
                        PickerRow row;
                        row.title = name;
                        row.icon = QStringLiteral("git-branch");
                        row.value = name == fallback ? QString() : name;
                        row.current = name == current;
                        rows.append(row);
                    }
                    showHomePicker(hero->branch, QStringLiteral("Find a branch…"),
                                   rows, [hero, activeRepo](const QVariant &value) {
                                       hero->branchChoice = value.toString();
                                       hero->branchRepo = activeRepo();
                                       hero->refresh();
                                   });
                });

        connect(hero->agent, &QPushButton::clicked, this,
                [this, hero, currentProvider, isAgentProvider] {
                    if (!m_quickAddAgentProvider)
                        return;
                    const QString current = currentProvider();
                    QVector<PickerRow> rows;
                    rows.append({QStringLiteral("Agent"), {}, {}, {}, {}, false});
                    for (int i = 0; i < m_quickAddAgentProvider->count(); ++i) {
                        const QString provider =
                            m_quickAddAgentProvider->itemData(i).toString();
                        if (!isAgentProvider(provider))
                            continue;
                        PickerRow row;
                        row.title = providerTitle(
                            provider, m_quickAddAgentProvider->itemText(i));
                        row.subtitle = providerSubtitle(provider);
                        row.icon = QStringLiteral("sparkle");
                        row.value = i;
                        row.current = provider == current;
                        rows.append(row);
                    }
                    showHomePicker(hero->agent, QString(), rows,
                                   [this, hero](const QVariant &value) {
                                       if (m_quickAddAgentProvider)
                                           m_quickAddAgentProvider->setCurrentIndex(
                                               value.toInt());
                                       hero->refresh();
                                   });
                });

        connect(hero->mode, &QPushButton::clicked, this, [this, hero] {
            if (!m_quickAddModeSelector)
                return;
            QVector<PickerRow> rows;
            rows.append({QStringLiteral("Permissions"), {}, {}, {}, {}, false});
            for (int i = 0; i < m_quickAddModeSelector->count(); ++i) {
                PickerRow row;
                row.title = m_quickAddModeSelector->itemText(i);
                row.subtitle = m_quickAddModeSelector->itemData(i, Qt::ToolTipRole)
                                   .toString();
                row.icon = modeIconOf(row.title);
                row.value = i;
                row.current = i == m_quickAddModeSelector->currentIndex();
                rows.append(row);
            }
            showHomePicker(hero->mode, QString(), rows,
                           [this, hero](const QVariant &value) {
                               if (m_quickAddModeSelector)
                                   m_quickAddModeSelector->setCurrentIndex(
                                       value.toInt());
                               hero->refresh();
                           });
        });

        connect(hero->model, &QPushButton::clicked, this,
                [this, hero, currentProvider] {
                    QVector<PickerRow> rows;
                    const QString provider = currentProvider();
                    if (m_quickAddClaudeModel && m_quickAddClaudeModel->count() > 0) {
                        rows.append({QStringLiteral("Model"), {}, {}, {}, {}, false});
                        for (int i = 0; i < m_quickAddClaudeModel->count(); ++i) {
                            PickerRow row;
                            row.title = m_quickAddClaudeModel->itemText(i);
                            row.subtitle = m_quickAddClaudeModel
                                               ->itemData(i, Qt::ToolTipRole)
                                               .toString();
                            row.value = i;
                            row.current = i == m_quickAddClaudeModel->currentIndex();
                            rows.append(row);
                        }
                    }
                    const bool effort = m_quickAddSpeedSelector &&
                                        m_quickAddSpeedSelector->count() > 0 &&
                                        !m_quickAddSpeedSelector->isHidden();
                    std::function<QWidget *(QWidget *, QMenu *)> extra;
                    if (effort)
                        extra = [this, hero](QWidget *parent, QMenu *) -> QWidget * {
                            auto *box = new QWidget(parent);
                            auto *layout = new QVBoxLayout(box);
                            layout->setContentsMargins(0, 4, 0, 4);
                            layout->setSpacing(4);
                            auto *heading = new QLabel(QStringLiteral("EFFORT"), box);
                            heading->setProperty("homeRole",
                                                 QStringLiteral("pickerHeading"));
                            layout->addWidget(heading);
                            QStringList labels;
                            for (int i = 0; i < m_quickAddSpeedSelector->count(); ++i)
                                labels << m_quickAddSpeedSelector->itemText(i);
                            auto *segments = new kit::SegmentedControl(labels, box);
                            segments->setObjectName(QStringLiteral("homeEffort"));
                            segments->setCurrentIndex(
                                m_quickAddSpeedSelector->currentIndex());
                            segments->onChanged([this, hero](int index) {
                                if (m_quickAddSpeedSelector)
                                    m_quickAddSpeedSelector->setCurrentIndex(index);
                                hero->refresh();
                            });
                            auto *centre = new QHBoxLayout;
                            centre->setContentsMargins(8, 0, 8, 0);
                            centre->addWidget(segments, 1);
                            layout->addLayout(centre);
                            return box;
                        };
                    if (rows.isEmpty() && !effort)
                        return;
                    Q_UNUSED(provider);
                    showHomePicker(hero->model, QString(), rows,
                                   [this, hero](const QVariant &value) {
                                       if (m_quickAddClaudeModel)
                                           m_quickAddClaudeModel->setCurrentIndex(
                                               value.toInt());
                                       hero->refresh();
                                   },
                                   QString(), {}, extra);
                });

        const auto submit = [this, hero, ensureAgentProvider, currentProvider,
                             activeRepo] {
            const QString text = hero->input->toPlainText().trimmed();
            if (text.isEmpty() || !m_issueQuickAdd)
                return;
            ensureAgentProvider();
            int before = 0;
            for (const AgentSession &session : m_agentSessions)
                before = qMax(before, session.id);
            const bool customBranch = !hero->branchChoice.isEmpty() &&
                                      hero->branchRepo == activeRepo() &&
                                      !(m_promptCloudButton &&
                                        m_promptCloudButton->isChecked());
            if (customBranch) {
                const QString provider = currentProvider();
                const QString model =
                    (provider == QLatin1String("claude-code") ||
                     agentIsCodexProvider(provider) ||
                     agentIsCloudflareAiProvider(provider) ||
                     agentIsLocalLlamaProvider(provider))
                        ? selectedModelComboValue(m_quickAddClaudeModel)
                        : QString();
                QString prompt = text;
                for (const QString &attachment : std::as_const(m_quickAddImages)) {
                    if (!prompt.endsWith(QLatin1Char('\n')))
                        prompt += QLatin1Char('\n');
                    prompt += promptAttachmentLine(attachment);
                }
                const int id = startAdHocAgentForRepo(
                    issuesRepoIndex(), prompt, provider,
                    m_quickAddCreatePr && m_quickAddCreatePr->isChecked(), model,
                    QString(), false, false, QString(), QString(),
                    hero->branchChoice, 50,
                    m_promptWorktreeCheck && !m_promptWorktreeCheck->isChecked(),
                    QString(), m_promptVibeCheck && m_promptVibeCheck->isChecked());
                if (id > 0) {
                    recordQuickAddHistory(text);
                    m_issueQuickAdd->clear();
                    clearQuickAddImages();
                    if (m_promptWorktreeCheck)
                        m_promptWorktreeCheck->setChecked(true);
                }
            } else {
                m_issueQuickAdd->setPlainText(text);
                quickAddIssue();
            }
            int after = 0;
            for (const AgentSession &session : m_agentSessions)
                after = qMax(after, session.id);
            if (after > before) {
                hero->input->clear();
                m_issueQuickAdd->clear();
                showSessionsPage(after);
            }
        };
        connect(hero->send, &QPushButton::clicked, this, submit);
        hero->input->installEventFilter(new HomeKeyFilter(
            hero->input, [hero, composer, submit](QEvent *event) {
                if (event->type() == QEvent::FocusIn ||
                    event->type() == QEvent::FocusOut) {
                    composer->setProperty("focused",
                                          event->type() == QEvent::FocusIn);
                    composer->style()->unpolish(composer);
                    composer->style()->polish(composer);
                    return false;
                }
                if (event->type() != QEvent::KeyPress)
                    return false;
                auto *key = static_cast<QKeyEvent *>(event);
                if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) &&
                    !(key->modifiers() & Qt::ShiftModifier)) {
                    submit();
                    return true;
                }
                if (key->key() == Qt::Key_Escape && hero->input->toPlainText().isEmpty()) {
                    hero->input->clearFocus();
                    return true;
                }
                return false;
            }));

        hero->headline = [this]() -> QString {
            int running = 0;
            int waiting = 0;
            int failed = 0;
            for (const AgentSession &session : m_agentSessions) {
                if (session.merged)
                    continue;
                if (session.status == AgentStatus::Running ||
                    session.status == AgentStatus::Queued)
                    ++running;
                else if (session.status == AgentStatus::Waiting)
                    ++waiting;
                else if (session.status == AgentStatus::Failed)
                    ++failed;
            }
            const auto badgeOf = [](QWidget *widget) {
                auto *rail = dynamic_cast<ActivityRailButton *>(widget);
                return rail && !widget->isHidden() ? rail->badgeCount() : 0;
            };
            const int pings = badgeOf(m_notificationButton);
            const int tasks = badgeOf(railWidgetForKey(QStringLiteral("tasks")));
            QStringList parts;
            if (pings > 0)
                parts << plural(pings, QStringLiteral("ping"));
            if (tasks > 0)
                parts << QStringLiteral("%1 open task%2")
                             .arg(tasks)
                             .arg(tasks == 1 ? QString() : QStringLiteral("s"));
            if (running > 0)
                parts << plural(running, QStringLiteral("session")) +
                             QStringLiteral(" running");
            if (waiting > 0)
                parts << QStringLiteral("%1 waiting on you").arg(waiting);
            if (failed > 0)
                parts << QStringLiteral("%1 failed").arg(failed);
            if (parts.isEmpty())
                return QStringLiteral("All quiet");
            return parts.mid(0, 4).join(QString::fromUtf8("  \xC2\xB7  "));
        };

        hero->refresh = [this, hero, currentProvider, repoPath, defaultBranchOf,
                         activeRepo] {
            const HomeBoardStyle style = nextBoardStyle();
            const QString styleKey = style.plate.name() + style.title.name() +
                                     style.badge.name();
            if (hero->styleSignature != styleKey) {
                hero->styleSignature = styleKey;
                m_homeGridBoard->setStyle(style);
                hero->send->setIcon(QIcon(upArrowPixmap(
                    Theme::color(hero->send->isEnabled() ? Theme::Role::Bg
                                                         : Theme::Role::FgSubtle),
                    16)));
            }

            const int active = activeRepo();
            QString repoText = QStringLiteral("Choose a repository");
            if (m_issuesRepoCombo && m_issuesRepoCombo->currentIndex() >= 0)
                repoText = m_issuesRepoCombo->currentText();
            else if (active >= 0 && active < m_repositories.size())
                repoText = m_repositories.at(active).owner + QLatin1Char('/') +
                           m_repositories.at(active).name;
            setComposerChip(hero->repo, repoText);
            if (hero->branchRepo != active)
                hero->branchChoice.clear();
            const QString path = repoPath();
            hero->branch->setVisible(!path.isEmpty());
            if (!path.isEmpty()) {
                QString label = hero->branchChoice;
                if (label.isEmpty()) {
                    label = hero->branch->property("homeDefaultBranch").toString();
                    if (hero->branch->property("homeDefaultFor").toString() != path) {
                        label = defaultBranchOf(listRepoBranches(path));
                        hero->branch->setProperty("homeDefaultBranch", label);
                        hero->branch->setProperty("homeDefaultFor", path);
                    }
                }
                setComposerChip(hero->branch,
                                label.isEmpty() ? QStringLiteral("Default branch")
                                                : label);
            }

            const QString provider = currentProvider();
            const int providerIndex =
                m_quickAddAgentProvider && !provider.isEmpty()
                    ? m_quickAddAgentProvider->findData(provider)
                    : -1;
            setComposerChip(hero->agent,
                            providerIndex >= 0
                                ? providerTitle(provider, m_quickAddAgentProvider
                                                              ->itemText(providerIndex))
                                : QStringLiteral("Choose an agent"));

            const bool cli = provider == QLatin1String("claude-code") ||
                             agentIsCodexProvider(provider);
            const bool showMode = m_quickAddModeSelector && cli;
            hero->mode->setVisible(showMode);
            if (showMode)
                setComposerChip(hero->mode, m_quickAddModeSelector->currentText(),
                                QString(),
                                modeIconOf(m_quickAddModeSelector->currentText()));

            const QString modelText =
                m_quickAddClaudeModel && m_quickAddClaudeModel->currentIndex() >= 0
                    ? agentModelLabel(selectedModelComboValue(m_quickAddClaudeModel))
                    : QString();
            const QString modelLabel =
                !modelText.isEmpty()
                    ? modelText
                    : (m_quickAddClaudeModel ? m_quickAddClaudeModel->currentText()
                                             : QString());
            const QString effort =
                cli && m_quickAddSpeedSelector ? m_quickAddSpeedSelector->currentText()
                                               : QString();
            hero->model->setVisible(!modelLabel.isEmpty() || !effort.isEmpty());
            setComposerChip(hero->model, modelLabel, effort);

            hero->mic->setVisible(m_quickAddMicButton &&
                                  !m_quickAddMicButton->isHidden());
            setComposerChip(hero->mic, QString());
            hero->attach->setVisible(m_quickAddImageButton != nullptr);
            hero->attach->setToolTip(
                m_quickAddImages.isEmpty()
                    ? QStringLiteral("Attach files to the prompt")
                    : QStringLiteral("%1 attached; click to add more")
                          .arg(m_quickAddImages.size()));
            setComposerChip(hero->attach,
                            m_quickAddImages.isEmpty()
                                ? QString()
                                : QString::number(m_quickAddImages.size()));

            const auto badgeOf = [](QWidget *widget) {
                auto *rail = dynamic_cast<ActivityRailButton *>(widget);
                return rail && !widget->isHidden() ? rail->badgeCount() : 0;
            };
            const int pings = badgeOf(m_notificationButton);
            const int tasks = badgeOf(railWidgetForKey(QStringLiteral("tasks")));
            const int pulls = repoOpenPullCount();
            int failed = 0;
            for (const AgentSession &session : m_agentSessions)
                if (!session.merged && session.status == AgentStatus::Failed)
                    ++failed;
            QList<QPair<QString, QString>> ideas;
            if (pings > 0)
                ideas.append({QStringLiteral("Triage today's pings"),
                              QStringLiteral("Go through today's pings and tell me "
                                             "which ones need me, most urgent first.")});
            if (pulls > 0)
                ideas.append({QStringLiteral("Review open pull requests"),
                              QStringLiteral("Review the open pull requests in this "
                                             "repository and summarize what each "
                                             "one needs before it can merge.")});
            if (failed > 0)
                ideas.append({QStringLiteral("Look into failed sessions"),
                              QStringLiteral("Look into the agent sessions that "
                                             "failed and tell me why each one "
                                             "stopped.")});
            if (tasks > 0)
                ideas.append({QStringLiteral("Plan the open tasks"),
                              QStringLiteral("Look at the open tasks and suggest "
                                             "which ones to do first and why.")});
            ideas.append({QStringLiteral("Summarize this week's changes"),
                          QStringLiteral("Summarize what changed in this "
                                         "repository over the last week.")});
            ideas.append({QStringLiteral("Find bugs in recent commits"),
                          QStringLiteral("Read the most recent commits in this "
                                         "repository and point out likely bugs.")});
            ideas = ideas.mid(0, kHomeSuggestions);
            QStringList titles;
            for (const auto &idea : std::as_const(ideas))
                titles << idea.first;
            const QString suggestionKey = titles.join(QLatin1Char('|'));
            if (hero->suggestionSignature != suggestionKey) {
                hero->suggestionSignature = suggestionKey;
                clearLayout(hero->suggestionRow);
                for (const auto &idea : std::as_const(ideas)) {
                    auto *chip = new QPushButton(idea.first);
                    chip->setProperty("homeRole", QStringLiteral("suggestion"));
                    chip->setCursor(Qt::PointingHandCursor);
                    chip->setToolTip(idea.second);
                    const QString prompt = idea.second;
                    connect(chip, &QPushButton::clicked, hero, [hero, prompt] {
                        hero->input->setPlainText(prompt);
                        hero->input->setFocus(Qt::OtherFocusReason);
                        hero->input->moveCursor(QTextCursor::End);
                    });
                    hero->suggestionRow->addWidget(chip);
                }
            }

            hero->needsAll->setText(pings > 0
                                        ? QString::fromUtf8("All %1  \xE2\x80\xBA")
                                              .arg(pings)
                                        : QString::fromUtf8("All pings  \xE2\x80\xBA"));
            QList<const AppNotification *> shown;
            for (const AppNotification &item : std::as_const(m_notifications)) {
                if (item.quiet)
                    continue;
                shown.append(&item);
                if (shown.size() >= kHomeNeedsRows)
                    break;
            }
            QString needsKey =
                QString::number(QDateTime::currentSecsSinceEpoch() / 60) +
                QLatin1Char('|') + QString::number(m_localPingsSeenAtMs);
            for (const AppNotification *item : std::as_const(shown))
                needsKey += QLatin1Char('|') + QString::number(item->id) +
                            QLatin1Char(':') + QString::number(item->timestampMs) +
                            item->title;
            if (hero->needsSignature == needsKey)
                return;
            hero->needsSignature = needsKey;
            clearLayout(hero->needsLayout);
            if (shown.isEmpty()) {
                auto *empty = new QLabel(QStringLiteral("Nothing needs you right now."));
                empty->setProperty("homeRole", QStringLiteral("empty"));
                empty->setAlignment(Qt::AlignCenter);
                hero->needsLayout->addWidget(empty);
                return;
            }
            const int textWidth = kHomeNeedsWidth - 2 - 16 - 16 - 8 - 12;
            for (int i = 0; i < shown.size(); ++i) {
                const AppNotification &item = *shown.at(i);
                auto *row = new QPushButton;
                row->setObjectName(QStringLiteral("homeNeedsRow"));
                row->setProperty("homeRole", QStringLiteral("ping"));
                row->setProperty("last", i == shown.size() - 1);
                row->setCursor(Qt::PointingHandCursor);
                row->setFixedHeight(62);
                row->setToolTip(item.body.isEmpty()
                                    ? item.title
                                    : item.title + QLatin1Char('\n') + item.body);
                auto *line = new QHBoxLayout(row);
                line->setContentsMargins(16, 10, 16, 10);
                line->setSpacing(12);
                const bool unseen = item.timestampMs > m_localPingsSeenAtMs ||
                                    (item.dismissOnly && !item.acknowledged);
                auto *dot = new QLabel(row);
                dot->setFixedSize(8, 8);
                dot->setPixmap(dotPixmap(
                    Theme::color(item.warning ? Theme::Role::Danger
                                 : unseen     ? Theme::Role::Accent
                                              : Theme::Role::FgSubtle),
                    8));
                line->addWidget(dot, 0, Qt::AlignTop);
                auto *texts = new QVBoxLayout;
                texts->setContentsMargins(0, 0, 0, 0);
                texts->setSpacing(2);
                auto *title = new QLabel(row);
                title->setProperty("homeRole", QStringLiteral("pingTitle"));
                title->setTextFormat(Qt::PlainText);
                QFont titleFont = title->font();
                titleFont.setPixelSize(14);
                title->setText(QFontMetrics(titleFont).elidedText(
                    item.title.simplified(), Qt::ElideRight, textWidth));
                QString source = item.actor;
                if (source.isEmpty() && item.link.isValid())
                    source = notificationLinkLabel(item.link);
                if (source.isEmpty())
                    source = item.repo;
                if (source.isEmpty() && !item.kind.isEmpty()) {
                    source = item.kind;
                    source[0] = source.at(0).toUpper();
                }
                if (source.isEmpty())
                    source = QStringLiteral("ForkMesh");
                const QString age = formatShortRelativeTime(item.timestampMs / 1000);
                auto *meta = new QLabel(row);
                meta->setProperty("homeRole", QStringLiteral("pingMeta"));
                meta->setTextFormat(Qt::PlainText);
                QFont metaFont = meta->font();
                metaFont.setPixelSize(12);
                meta->setText(QFontMetrics(metaFont).elidedText(
                    age.isEmpty() ? source
                                  : source + QString::fromUtf8(" \xC2\xB7 ") + age,
                    Qt::ElideRight, textWidth));
                texts->addWidget(title);
                texts->addWidget(meta);
                line->addLayout(texts, 1);
                for (QLabel *label : row->findChildren<QLabel *>())
                    label->setAttribute(Qt::WA_TransparentForMouseEvents);
                const qint64 id = item.id;
                const qint64 stamp = item.timestampMs;
                const QString key = item.title;
                connect(row, &QPushButton::clicked, this, [this, id, stamp, key] {
                    for (const AppNotification &notice : std::as_const(m_notifications)) {
                        if (id != 0 ? notice.id != id
                                    : (notice.timestampMs != stamp || notice.title != key))
                            continue;
                        if (notice.runId > 0)
                            openActionRunFromNotification(notice.runId);
                        else if (notice.link.isValid())
                            openNotificationLink(notice.link);
                        else
                            showNotifications();
                        return;
                    }
                    showNotifications();
                });
                hero->needsLayout->addWidget(row);
            }
        };
    } else {
        auto *heading = new QHBoxLayout;
        heading->addWidget(m_homeGridGreeting);
        heading->addStretch();
        outer->addLayout(heading);
        outer->addWidget(m_homeGridHeadline);
        outer->addWidget(m_homeGridSearch);
        scroll->setWidget(m_homeGridBoard);
        outer->addWidget(scroll, 1);
    }

    m_homeGridBoard->setPullGate([this, scroll] {
        const QScrollBar *bar = scroll->verticalScrollBar();
        return m_homeGridSearch && m_homeGridSearch->isHidden() &&
               bar->value() == bar->minimum();
    });
    m_homeGridBoard->setOnPull([this] { showHomeGridSearch(); });
    m_homeGridBoard->setOnType(
        [this](const QString &text) { showHomeGridSearch(text); });

    m_homeGridBoard->setOnPagesChanged([this, scroll](int count, int current) {
        if (m_homeGridDots)
            m_homeGridDots->setPages(count, current);
        if (!heroOf(m_homeGridBoard))
            scroll->verticalScrollBar()->setValue(0);
    });
    m_homeGridDots->setPages(m_homeGridBoard->pageCount(),
                             m_homeGridBoard->page());
    if (!next)
        outer->addWidget(m_homeGridDots);

    m_homeGridTimer = new QTimer(page);
    m_homeGridTimer->setInterval(kHomeTickMs);
    connect(m_homeGridTimer, &QTimer::timeout, this, [this] {
        if (!homeGridOnScreen()) {
            m_homeGridTimer->stop();
            return;
        }
        refreshHomeGridPage();
    });
    return page;
}

void MainWindow::showHomeGridSearch(const QString &typed)
{
    if (!m_homeGridSearch || !m_homeGridFilter)
        return;
    if (m_homeGridSearch->isHidden()) {
        const int height = m_homeGridSearch->sizeHint().height();
        m_homeGridSearch->setMaximumHeight(0);
        m_homeGridSearch->show();
        auto *grow = new QPropertyAnimation(m_homeGridSearch, "maximumHeight",
                                            m_homeGridSearch);
        grow->setDuration(kHomeSearchRevealMs);
        grow->setStartValue(0);
        grow->setEndValue(height);
        grow->setEasingCurve(QEasingCurve::OutCubic);
        connect(grow, &QPropertyAnimation::finished, m_homeGridSearch,
                [search = m_homeGridSearch] {
                    search->setMaximumHeight(QWIDGETSIZE_MAX);
                });
        grow->start(QAbstractAnimation::DeleteWhenStopped);
    }
    m_homeGridFilter->setFocus(Qt::OtherFocusReason);
    if (!typed.isEmpty())
        m_homeGridFilter->insert(typed);
}

void MainWindow::hideHomeGridSearch()
{
    if (!m_homeGridSearch || !m_homeGridFilter)
        return;
    const bool hadFocus = m_homeGridFilter->hasFocus();
    m_homeGridFilter->clear();
    m_homeGridSearch->hide();
    if (hadFocus && m_homeGridBoard)
        m_homeGridBoard->setFocus(Qt::OtherFocusReason);
}

bool MainWindow::homeGridOnScreen() const
{
    return m_sectionStack && m_homeGridBoard &&
           m_sectionStack->currentIndex() == kHomeGridSectionIndex &&
           isVisible() && !isMinimized();
}

QVector<HomeGroup> MainWindow::homeGridGroups() const
{
    const QList<QWidget *> railWidgets = railItemWidgets();

    QHash<QString, QVector<HomeTile>> byGroup;
    for (QWidget *widget : std::as_const(railWidgets)) {
        auto *button = qobject_cast<QAbstractButton *>(widget);
        const QString key = widget->property("forkmeshRailKey").toString();
        if (!button || key.isEmpty() || key == QLatin1String("home") ||
            widget->isHidden())
            continue;
        auto *rail = dynamic_cast<ActivityRailButton *>(widget);
        HomeTile tile;
        tile.key = key;
        tile.title = rail ? rail->label() : button->text();
        if (tile.title.isEmpty())
            tile.title = button->accessibleName();
        if (tile.title.isEmpty())
            tile.title = key;
        tile.icon = rail ? rail->iconName() : QString();
        tile.tooltip = button->toolTip();
        if (rail) {
            tile.badge = rail->badgeCount();
            tile.urgent = rail->badgeUrgent();
            tile.pending = rail->pendingSyncCount();
            tile.live = rail->syncing() || rail->activityBlink();
            tile.alert = rail->alertTint();
            tile.desktopOffline = rail->desktopOffline();
        }
        if (key == QLatin1String("code"))
            tile.badge = repoOpenPullCount();
        if (key == QLatin1String("agents")) {
            for (const AgentSession &session : m_agentSessions)
                if (!session.merged && session.status == AgentStatus::Running) {
                    tile.live = true;
                    break;
                }
        }
        tile.detail = homeGridTileDetail(key, widget);
        byGroup[RailGroups::defaultGroupOf(key)].append(tile);
    }

    QVector<HomeGroup> groups;
    for (const RailGroups::BuiltIn &spec : RailGroups::builtInGroups()) {
        const QString id = QString::fromLatin1(spec.id);
        const QVector<HomeTile> tiles = byGroup.value(id);
        if (tiles.isEmpty())
            continue;
        groups.append({id, QString::fromLatin1(spec.title), tiles});
    }
    return groups;
}

QString MainWindow::homeGridTileDetail(const QString &key, QWidget *widget) const
{
    auto *rail = dynamic_cast<ActivityRailButton *>(widget);
    const int badge = rail ? rail->badgeCount() : 0;
    if (key == QLatin1String("agents")) {
        int running = 0;
        int waiting = 0;
        int failed = 0;
        for (const AgentSession &session : m_agentSessions) {
            if (session.merged)
                continue;
            if (session.status == AgentStatus::Running ||
                session.status == AgentStatus::Queued)
                ++running;
            else if (session.status == AgentStatus::Waiting)
                ++waiting;
            else if (session.status == AgentStatus::Failed)
                ++failed;
        }
        if (failed > 0)
            return QStringLiteral("%1 failed").arg(failed);
        if (waiting > 0)
            return QStringLiteral("%1 waiting").arg(waiting);
        if (running > 0)
            return QStringLiteral("%1 running").arg(running);
        return m_agentSessions.isEmpty() ? QStringLiteral("Idle")
                                         : plural(m_agentSessions.size(),
                                                  QStringLiteral("session"));
    }
    if (key == QLatin1String("code")) {
        const int pulls = repoOpenPullCount();
        if (pulls > 0)
            return plural(pulls, QStringLiteral("open pull"));
    }
    if (key == QLatin1String("chat"))
        return badge > 0 ? QStringLiteral("%1 unread").arg(badge)
                         : QStringLiteral("Caught up");
    if (key == QLatin1String("notifications"))
        return badge > 0 ? QStringLiteral("%1 new").arg(badge)
                         : QStringLiteral("All read");
    if (key == QLatin1String("git") && rail) {
        if (rail->syncing())
            return QStringLiteral("Syncing");
        if (rail->pendingSyncCount() > 0)
            return QStringLiteral("%1 to push").arg(rail->pendingSyncCount());
        return badge > 0 ? QStringLiteral("%1 changed").arg(badge)
                         : QStringLiteral("Clean");
    }
    if (rail && rail->activityBlink())
        return QStringLiteral("Working");
    if (rail && rail->syncing())
        return QStringLiteral("Syncing");
    if (key == QLatin1String("tasks") && badge > 0)
        return QStringLiteral("%1 open").arg(badge);
    if (key == QLatin1String("network") && badge > 0)
        return QStringLiteral("%1 online").arg(badge);
    if (key == QLatin1String("memory")) {
        const qint64 rss = SystemStats::residentBytes();
        if (rss > 0)
            return QStringLiteral("%1 MB").arg(rss / (1024 * 1024));
    }
    if (auto *button = qobject_cast<QAbstractButton *>(widget))
        return tooltipSummary(button->toolTip());
    return QString();
}

int MainWindow::repoOpenPullCount() const
{
    const auto *pulls = dynamic_cast<const VerticalIconButton *>(m_repoPullsTab);
    return pulls ? int(pulls->badgeCount()) : 0;
}

QString MainWindow::homeGridHeadline() const
{
    int running = 0;
    int waiting = 0;
    int failed = 0;
    for (const AgentSession &session : m_agentSessions) {
        if (session.merged)
            continue;
        if (session.status == AgentStatus::Running ||
            session.status == AgentStatus::Queued)
            ++running;
        else if (session.status == AgentStatus::Waiting)
            ++waiting;
        else if (session.status == AgentStatus::Failed)
            ++failed;
    }
    const auto badgeOf = [](QWidget *widget) {
        auto *rail = dynamic_cast<ActivityRailButton *>(widget);
        return rail && !widget->isHidden() ? rail->badgeCount() : 0;
    };
    const int pings = badgeOf(m_notificationButton);
    const int unread = badgeOf(m_chatButton);
    QStringList parts;
    if (running > 0)
        parts << plural(running, QStringLiteral("agent")) +
                     QStringLiteral(" working");
    if (waiting > 0)
        parts << QStringLiteral("%1 waiting on you").arg(waiting);
    if (failed > 0)
        parts << QStringLiteral("%1 failed").arg(failed);
    if (pings > 0)
        parts << plural(pings, QStringLiteral("ping"));
    if (unread > 0)
        parts << QStringLiteral("%1 unread").arg(unread);
    if (parts.isEmpty())
        return QStringLiteral("All quiet");
    return parts.mid(0, 4).join(QString::fromUtf8(" \xC2\xB7 "));
}

void MainWindow::refreshHomeGridPage()
{
    if (!m_homeGridBoard)
        return;
    const int hour = QTime::currentTime().hour();
    const QString greeting = hour >= 5 && hour < 12   ? QStringLiteral("Good morning")
                             : hour >= 12 && hour < 17 ? QStringLiteral("Good afternoon")
                             : hour >= 17 && hour < 22 ? QStringLiteral("Good evening")
                                                       : QStringLiteral("Up late");
    HomeHero *hero = heroOf(m_homeGridBoard);
    QString title = greeting;
    if (hero) {
        const QString name = greetingNameFor(topBarUserName());
        if (!name.isEmpty())
            title += QStringLiteral(", ") + name;
    }
    if (m_homeGridGreeting && m_homeGridGreeting->text() != title)
        m_homeGridGreeting->setText(title);
    const QString headline = hero ? hero->headline() : homeGridHeadline();
    if (m_homeGridHeadline && m_homeGridHeadline->text() != headline)
        m_homeGridHeadline->setText(headline);
    m_homeGridBoard->setGroups(homeGridGroups());
    if (hero && hero->refresh)
        hero->refresh();
    if (m_homeGridTimer && !m_homeGridTimer->isActive() && homeGridOnScreen())
        m_homeGridTimer->start();
}

QWidget *MainWindow::railWidgetForKey(const QString &key) const
{
    if (key.isEmpty())
        return nullptr;
    for (QWidget *widget : railItemWidgets()) {
        if (widget->property("forkmeshRailKey").toString() == key)
            return widget;
    }
    return nullptr;
}

void MainWindow::addHomeTileRailActions(QMenu *menu, const QString &key)
{
    if (!menu || key.isEmpty() || key == QLatin1String("home") ||
        !railWidgetForKey(key))
        return;
    const bool hidden = railHiddenKeys().contains(key);
    QAction *toggle = menu->addAction(
        hidden ? QStringLiteral("Show in left rail")
               : QStringLiteral("Hide from left rail (keep on Home)"),
        this, [this, key, hidden] {
            QTimer::singleShot(0, this,
                               [this, key, hidden] { setRailItemHidden(key, !hidden); });
        });
    toggle->setObjectName(hidden ? QStringLiteral("homeShowInRail")
                                 : QStringLiteral("homeHideFromRail"));
}

bool MainWindow::activateHomeGridTile(const QString &key)
{
    auto *button = qobject_cast<QAbstractButton *>(railWidgetForKey(key));
    if (!button || !button->isEnabled())
        return false;
    button->click();
    return true;
}

#ifdef FORKMESH_WINDOW_TESTS
QVector<HomeGroup> MainWindow::testHomeGridGroups() const
{
    return homeGridGroups();
}

QString MainWindow::testHomeGridHeadline() const
{
    return m_homeGridHeadline ? m_homeGridHeadline->text() : QString();
}

bool MainWindow::testHomeGridSearchShown() const
{
    return m_homeGridSearch && !m_homeGridSearch->isHidden();
}

QStringList MainWindow::testHomeTileMenuActions(const QString &key)
{
    QStringList names;
    QMenu menu;
    addHomeTileRailActions(&menu, key);
    for (QAction *action : menu.actions())
        if (!action->isSeparator())
            names << action->objectName();
    return names;
}
#endif
