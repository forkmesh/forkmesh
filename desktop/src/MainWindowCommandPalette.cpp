#include "MainWindowInternal.h"
#include "MainWindowNextAccess.h"
#include "SettingsSectionsView.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QApplication>
#include <QBoxLayout>
#include <QEvent>
#include <QFile>
#include <QFocusEvent>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QHash>
#include <QIcon>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLayoutItem>
#include <QLineEdit>
#include <QList>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMouseEvent>
#include <QPixmap>
#include <QPointer>
#include <QSet>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QStringList>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <utility>

namespace forkmesh::ui {

namespace {

constexpr int kCommandHeadingRole = Qt::UserRole + 1;
constexpr int kCommandIndexRole = Qt::UserRole + 2;
constexpr int kCommandQueryIndex = -2;
constexpr int kSettingsSectionIndex = 1;
constexpr int kSettingsBuildDelayMs = 30;
constexpr int kCommandCardWidth = 640;
constexpr int kCommandCardMinWidth = 280;
constexpr int kCommandCardGutter = 16;
constexpr int kCommandIconPx = 16;
constexpr int kCommandRowHeight = 36;
constexpr int kCommandHeadingHeight = 28;
constexpr int kCommandListMargin = 6;
constexpr int kCommandMinListHeight = 72;
constexpr double kCommandCardTop = 0.16;
constexpr double kCommandCardMaxHeight = 0.6;

struct CommandEntry {
    QString group;
    QString label;
    QIcon icon;
    std::function<void()> run;
    bool queryOnly = false;
};

struct CommandQueryEntry {
    QString group;
    QString label;
    QIcon icon;
    std::function<void(const QString &)> run;
};

struct CommandMatch {
    int rank = 0;
    int index = 0;
};

int commandMatchRank(const QString &label, const QString &query)
{
    if (query.isEmpty())
        return 0;
    if (label.startsWith(query, Qt::CaseInsensitive))
        return 0;
    for (qsizetype i = 1; i < label.size(); ++i) {
        if (label.at(i).isLetterOrNumber() && !label.at(i - 1).isLetterOrNumber() &&
            QStringView(label).mid(i).startsWith(QStringView(query),
                                                 Qt::CaseInsensitive))
            return 1;
    }
    if (label.contains(query, Qt::CaseInsensitive))
        return 2;
    qsizetype from = 0;
    for (const QChar wanted : query) {
        if (wanted.isSpace())
            continue;
        const qsizetype found = label.indexOf(wanted, from, Qt::CaseInsensitive);
        if (found < 0)
            return -1;
        from = found + 1;
    }
    return 3;
}

bool commandGlyphAvailable(const QString &name)
{
    if (name.isEmpty() || name.startsWith(QLatin1Char(':')))
        return false;
    return QFile::exists(QStringLiteral(":/icons/octicons/%1.svg").arg(name)) ||
           QFile::exists(QStringLiteral(":/icons/codicons/%1.svg").arg(name));
}

QIcon commandBlankIcon(qreal devicePixelRatio)
{
    const qreal ratio = devicePixelRatio > 0.0 ? devicePixelRatio : 1.0;
    const int side = qMax(1, qRound(kCommandIconPx * ratio));
    QPixmap pixmap(side, side);
    pixmap.fill(Qt::transparent);
    pixmap.setDevicePixelRatio(ratio);
    QIcon icon;
    icon.addPixmap(pixmap, QIcon::Normal, QIcon::Off);
    icon.addPixmap(pixmap, QIcon::Selected, QIcon::Off);
    return icon;
}

QIcon commandGlyphIcon(const QString &name, qreal devicePixelRatio)
{
    if (!commandGlyphAvailable(name))
        return commandBlankIcon(devicePixelRatio);
    QIcon icon;
    icon.addPixmap(kit::tintedIcon(name, Theme::color(Theme::Role::FgMuted),
                                   kCommandIconPx, devicePixelRatio),
                   QIcon::Normal, QIcon::Off);
    icon.addPixmap(kit::tintedIcon(name, Theme::color(Theme::Role::Fg),
                                   kCommandIconPx, devicePixelRatio),
                   QIcon::Selected, QIcon::Off);
    return icon;
}

QIcon commandButtonIcon(const QAbstractButton *button, const QString &fallback,
                        qreal devicePixelRatio)
{
    const QString stored = button->property("forkmeshOcticon").toString();
    if (commandGlyphAvailable(stored))
        return commandGlyphIcon(stored, devicePixelRatio);
    const QIcon own = button->icon();
    if (!own.isNull())
        return own;
    return commandGlyphIcon(fallback, devicePixelRatio);
}

bool commandRowSelectable(const QListWidgetItem *item)
{
    return item && !item->data(kCommandHeadingRole).toBool() &&
           item->flags().testFlag(Qt::ItemIsSelectable);
}

bool commandNavigationKey(int key)
{
    switch (key) {
    case Qt::Key_Escape:
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Up:
    case Qt::Key_Down:
    case Qt::Key_Tab:
    case Qt::Key_Backtab:
        return true;
    default:
        return false;
    }
}

class CommandPaletteOverlay : public QWidget
{
public:
    explicit CommandPaletteOverlay(QWidget *host) : QWidget(host)
    {
        setObjectName(QStringLiteral("commandPaletteScrim"));
        setAttribute(Qt::WA_StyledBackground, true);
        setAttribute(Qt::WA_NoMousePropagation, true);

        m_card = new QFrame(this);
        m_card->setObjectName(QStringLiteral("commandPaletteCard"));
        m_card->setFrameShape(QFrame::NoFrame);
        m_card->setAttribute(Qt::WA_StyledBackground, true);

        auto *cardLayout = new QVBoxLayout(m_card);
        cardLayout->setContentsMargins(1, 1, 1, 1);
        cardLayout->setSpacing(0);

        m_input = new QLineEdit(m_card);
        m_input->setObjectName(QStringLiteral("commandPaletteInput"));
        m_input->setPlaceholderText(QStringLiteral("Search pages, settings and actions") +
                                    QChar(0x2026));
        m_input->setFrame(false);
        m_input->setAttribute(Qt::WA_MacShowFocusRect, false);
        cardLayout->addWidget(m_input);

        m_list = new QListWidget(m_card);
        m_list->setObjectName(QStringLiteral("commandPaletteList"));
        m_list->setFrameShape(QFrame::NoFrame);
        m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        m_list->setSelectionMode(QAbstractItemView::SingleSelection);
        m_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_list->setIconSize(QSize(kCommandIconPx, kCommandIconPx));
        m_list->setTextElideMode(Qt::ElideRight);
        m_list->setFocusPolicy(Qt::NoFocus);
        m_list->setMouseTracking(true);
        m_list->setAttribute(Qt::WA_MacShowFocusRect, false);
        m_list->setFixedHeight(0);
        auto *listLayout = new QVBoxLayout;
        listLayout->setContentsMargins(kCommandListMargin, kCommandListMargin,
                                       kCommandListMargin, kCommandListMargin);
        listLayout->setSpacing(0);
        listLayout->addWidget(m_list);
        cardLayout->addLayout(listLayout);

        m_footer = new QFrame(m_card);
        m_footer->setObjectName(QStringLiteral("commandPaletteFooter"));
        m_footer->setFrameShape(QFrame::NoFrame);
        m_footer->setAttribute(Qt::WA_StyledBackground, true);
        auto *footerLayout = new QHBoxLayout(m_footer);
        footerLayout->setContentsMargins(14, 8, 14, 8);
        footerLayout->setSpacing(6);
        const auto addHint = [this, footerLayout](const QStringList &keys,
                                                  const QString &caption) {
            for (const QString &key : keys)
                footerLayout->addWidget(new kit::KeyHint(key, m_footer), 0,
                                        Qt::AlignVCenter);
            footerLayout->addWidget(new QLabel(caption, m_footer), 0,
                                    Qt::AlignVCenter);
            footerLayout->addSpacing(10);
        };
        addHint({QString(QChar(0x2191)), QString(QChar(0x2193))},
                QStringLiteral("Navigate"));
        addHint({QStringLiteral("Enter")}, QStringLiteral("Select"));
        addHint({QStringLiteral("Esc")}, QStringLiteral("Close"));
        footerLayout->addStretch(1);
        cardLayout->addWidget(m_footer);

        host->installEventFilter(this);
        m_input->installEventFilter(this);
        m_list->installEventFilter(this);

        QObject::connect(m_input, &QLineEdit::textChanged, this,
                         [this] { rebuild(); });
        QObject::connect(m_list, &QListWidget::itemClicked, this,
                         [this](QListWidgetItem *item) { activate(item); });
        hide();
    }

    void open(const QList<CommandEntry> &entries,
              const CommandQueryEntry &queryEntry)
    {
        m_entries = entries;
        m_queryEntry = queryEntry;
        m_previousFocus = QApplication::focusWidget();
        {
            const QSignalBlocker blocker(m_input);
            m_input->clear();
        }
        rebuild();
        show();
        raise();
        relayout();
        focusInput();
    }

    void setEntries(const QList<CommandEntry> &entries,
                    const CommandQueryEntry &queryEntry)
    {
        m_entries = entries;
        m_queryEntry = queryEntry;
        rebuild();
    }

    void focusInput()
    {
        raise();
        m_input->setFocus(Qt::ShortcutFocusReason);
    }

    void closePalette(bool restoreFocus)
    {
        if (isHidden())
            return;
        QWidget *previous = m_previousFocus.data();
        m_previousFocus.clear();
        QWidget *focused = QApplication::focusWidget();
        const bool holdsFocus =
            focused && (focused == this || isAncestorOf(focused));
        if (restoreFocus && previous && previous->isVisible() &&
            previous->isEnabled() && !isAncestorOf(previous))
            previous->setFocus(Qt::OtherFocusReason);
        else if (holdsFocus)
            focused->clearFocus();
        hide();
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == parentWidget()) {
            if (event->type() == QEvent::Resize && !isHidden())
                relayout();
            return QWidget::eventFilter(watched, event);
        }
        if (watched != m_input && watched != m_list)
            return QWidget::eventFilter(watched, event);
        switch (event->type()) {
        case QEvent::ShortcutOverride: {
            auto *key = static_cast<QKeyEvent *>(event);
            if (commandNavigationKey(key->key())) {
                key->accept();
                return true;
            }
            break;
        }
        case QEvent::KeyPress: {
            auto *key = static_cast<QKeyEvent *>(event);
            if (handleKey(key))
                return true;
            if (watched == m_list) {
                forwardToInput(key);
                return true;
            }
            break;
        }
        case QEvent::FocusOut:
            if (watched == m_input) {
                const Qt::FocusReason reason =
                    static_cast<QFocusEvent *>(event)->reason();
                if (reason != Qt::ActiveWindowFocusReason &&
                    reason != Qt::PopupFocusReason)
                    QTimer::singleShot(0, this, [this] { closeIfFocusLeft(); });
            }
            break;
        default:
            break;
        }
        return QWidget::eventFilter(watched, event);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (!m_card->geometry().contains(event->position().toPoint()))
            closePalette(true);
        event->accept();
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if (!handleKey(event) && QApplication::focusWidget() != m_input)
            forwardToInput(event);
        event->accept();
    }

private:
    bool handleKey(QKeyEvent *event)
    {
        switch (event->key()) {
        case Qt::Key_Escape:
            closePalette(true);
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            activate(m_list->currentItem());
            return true;
        case Qt::Key_Down:
        case Qt::Key_Tab:
            moveSelection(1);
            return true;
        case Qt::Key_Up:
        case Qt::Key_Backtab:
            moveSelection(-1);
            return true;
        default:
            return false;
        }
    }

    void forwardToInput(QKeyEvent *event)
    {
        if (m_forwarding)
            return;
        m_forwarding = true;
        m_input->setFocus(Qt::OtherFocusReason);
        QCoreApplication::sendEvent(m_input, event);
        m_forwarding = false;
    }

    void closeIfFocusLeft()
    {
        if (isHidden())
            return;
        QWidget *focused = QApplication::focusWidget();
        if (!focused || focused == this || isAncestorOf(focused))
            return;
        if (focused->window() != window())
            return;
        closePalette(false);
    }

    void addHeading(const QString &text, const QFont &font)
    {
        auto *item = new QListWidgetItem(text, m_list);
        item->setData(kCommandHeadingRole, true);
        item->setFlags(Qt::NoItemFlags);
        item->setFont(font);
        item->setSizeHint(QSize(0, kCommandHeadingHeight));
    }

    void rebuild()
    {
        const QString query = m_input->text().trimmed();
        m_list->clear();

        QStringList groups;
        for (const CommandEntry &entry : std::as_const(m_entries)) {
            if (!groups.contains(entry.group))
                groups.append(entry.group);
        }
        QFont headingFont = m_list->font();
        headingFont.setPixelSize(11);
        headingFont.setWeight(QFont::DemiBold);

        for (const QString &group : std::as_const(groups)) {
            QList<CommandMatch> matches;
            for (int index = 0; index < int(m_entries.size()); ++index) {
                const CommandEntry &entry = m_entries.at(index);
                if (entry.group != group)
                    continue;
                if (entry.queryOnly && query.isEmpty())
                    continue;
                const int rank = commandMatchRank(entry.label, query);
                if (rank >= 0)
                    matches.append(CommandMatch{rank, index});
            }
            if (matches.isEmpty())
                continue;
            std::stable_sort(matches.begin(), matches.end(),
                             [](const CommandMatch &a, const CommandMatch &b) {
                                 return a.rank < b.rank;
                             });
            addHeading(group, headingFont);
            for (const CommandMatch &match : std::as_const(matches)) {
                const CommandEntry &entry = m_entries.at(match.index);
                auto *item = new QListWidgetItem(entry.icon, entry.label, m_list);
                item->setData(kCommandHeadingRole, false);
                item->setData(kCommandIndexRole, match.index);
                item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
                item->setSizeHint(QSize(0, kCommandRowHeight));
            }
        }
        if (!query.isEmpty() && m_queryEntry.run) {
            addHeading(m_queryEntry.group, headingFont);
            auto *item = new QListWidgetItem(m_queryEntry.icon,
                                             m_queryEntry.label.arg(query),
                                             m_list);
            item->setData(kCommandHeadingRole, false);
            item->setData(kCommandIndexRole, kCommandQueryIndex);
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            item->setSizeHint(QSize(0, kCommandRowHeight));
        }
        if (m_list->count() == 0)
            addHeading(QStringLiteral("No matching pages, settings or actions"),
                       headingFont);
        selectFirst();
        relayout();
    }

    void selectFirst()
    {
        for (int row = 0; row < m_list->count(); ++row) {
            if (commandRowSelectable(m_list->item(row))) {
                m_list->setCurrentRow(row);
                break;
            }
        }
        m_list->scrollToTop();
    }

    void moveSelection(int delta)
    {
        const int count = m_list->count();
        if (count <= 0 || delta == 0)
            return;
        const int step = delta > 0 ? 1 : -1;
        int row = m_list->currentRow();
        if (row < 0)
            row = step > 0 ? -1 : 0;
        for (int tried = 0; tried < count; ++tried) {
            row = (row + step + count) % count;
            if (!commandRowSelectable(m_list->item(row)))
                continue;
            m_list->setCurrentRow(row);
            bool first = true;
            for (int before = 0; before < row && first; ++before)
                first = !commandRowSelectable(m_list->item(before));
            if (first)
                m_list->scrollToTop();
            else
                m_list->scrollToItem(m_list->item(row));
            return;
        }
    }

    void activate(QListWidgetItem *item)
    {
        if (!commandRowSelectable(item))
            return;
        const int index = item->data(kCommandIndexRole).toInt();
        if (index == kCommandQueryIndex) {
            const QString query = m_input->text().trimmed();
            const std::function<void(const QString &)> run = m_queryEntry.run;
            closePalette(true);
            if (run && !query.isEmpty())
                QTimer::singleShot(0, this, [run, query] { run(query); });
            return;
        }
        if (index < 0 || index >= int(m_entries.size()))
            return;
        const std::function<void()> run = m_entries.at(index).run;
        closePalette(true);
        if (run)
            QTimer::singleShot(0, this, [run] { run(); });
    }

    void relayout()
    {
        QWidget *host = parentWidget();
        QLayout *cardLayout = m_card->layout();
        if (!host || !cardLayout)
            return;
        const QRect area = host->rect();
        if (geometry() != area)
            setGeometry(area);

        const int cardWidth =
            qMax(kCommandCardMinWidth,
                 qMin(kCommandCardWidth, area.width() - 2 * kCommandCardGutter));
        m_card->setFixedWidth(cardWidth);

        int rows = 0;
        for (int row = 0; row < m_list->count(); ++row)
            rows += m_list->item(row)->sizeHint().height();

        cardLayout->activate();
        const int chrome = m_card->sizeHint().height() - m_list->maximumHeight();
        const int limit = qMax(chrome + kCommandMinListHeight,
                               qRound(area.height() * kCommandCardMaxHeight));
        const int listHeight = qMax(0, qMin(rows, limit - chrome));
        if (m_list->minimumHeight() != listHeight ||
            m_list->maximumHeight() != listHeight) {
            m_list->setFixedHeight(listHeight);
            cardLayout->activate();
        }

        const int cardHeight = chrome + listHeight;
        int top = qRound(area.height() * kCommandCardTop);
        if (top + cardHeight > area.height() - kCommandCardGutter)
            top = qMax(0, area.height() - kCommandCardGutter - cardHeight);
        m_card->setGeometry((area.width() - cardWidth) / 2, top, cardWidth,
                            cardHeight);
    }

    QFrame *m_card = nullptr;
    QLineEdit *m_input = nullptr;
    QListWidget *m_list = nullptr;
    QFrame *m_footer = nullptr;
    QList<CommandEntry> m_entries;
    CommandQueryEntry m_queryEntry;
    QPointer<QWidget> m_previousFocus;
    bool m_forwarding = false;
};

class CommandPaletteShortcutGate : public QObject
{
public:
    explicit CommandPaletteShortcutGate(QShortcut *shortcut)
        : QObject(shortcut), m_shortcut(shortcut)
    {
        sync();
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        switch (event->type()) {
        case QEvent::StyleChange:
        case QEvent::PaletteChange:
        case QEvent::ApplicationPaletteChange:
        case QEvent::Polish:
        case QEvent::Show:
        case QEvent::WindowActivate:
            sync();
            break;
        default:
            break;
        }
        return QObject::eventFilter(watched, event);
    }

private:
    void sync()
    {
        if (m_shortcut && m_shortcut->isEnabled() != Theme::isNext())
            m_shortcut->setEnabled(Theme::isNext());
    }

    QPointer<QShortcut> m_shortcut;
};

}

void NextAccess::installCommandPalette(MainWindow &window)
{
    const QString name = QStringLiteral("commandPaletteShortcut");
    if (window.findChild<QShortcut *>(name, Qt::FindDirectChildrenOnly))
        return;
    auto *shortcut = new QShortcut(&window);
    QList<QKeySequence> keys{QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_P)};
#ifdef Q_OS_MACOS
    keys.append(QKeySequence(Qt::CTRL | Qt::Key_P));
#endif
    shortcut->setKeys(keys);
    shortcut->setObjectName(name);
    shortcut->setContext(Qt::WindowShortcut);
    shortcut->setAutoRepeat(false);
    MainWindow *target = &window;
    QObject::connect(shortcut, &QShortcut::activated, target,
                     [target] { NextAccess::showCommandPalette(*target); });
    window.installEventFilter(new CommandPaletteShortcutGate(shortcut));
}

void NextAccess::showCommandPalette(MainWindow &window, bool refresh)
{
    if (!Theme::isNext())
        return;

    QWidget *host = overlayParent(window);
    if (!host)
        host = window.centralWidget();
    if (host) {
        auto *scrim = host->findChild<QWidget *>(
            QStringLiteral("welcomeScrim"), Qt::FindDirectChildrenOnly);
        if (scrim && scrim->isVisible())
            return;
    }
    if (!host)
        host = &window;

    CommandPaletteOverlay *overlay = nullptr;
    const QList<QWidget *> scrims = host->findChildren<QWidget *>(
        QStringLiteral("commandPaletteScrim"), Qt::FindDirectChildrenOnly);
    for (QWidget *scrim : scrims) {
        overlay = dynamic_cast<CommandPaletteOverlay *>(scrim);
        if (overlay)
            break;
    }
    if (refresh) {
        if (!overlay || !overlay->isVisible())
            return;
    } else {
        if (!overlay)
            overlay = new CommandPaletteOverlay(host);
        if (overlay->isVisible()) {
            overlay->focusInput();
            return;
        }
    }

    const qreal devicePixelRatio = window.devicePixelRatioF();
    QBoxLayout *railLayout = window.m_appNavigationRailLayout;
    const QHash<QString, QPointer<RailFolderWidget>> railFolders =
        window.m_railFolderWidgets;
    QBoxLayout *tabRow = window.m_repoTabRowLayout;
    auto *registry = window.m_packageRegistry.get();
    const QPointer<MainWindow> target(&window);

    QHash<QAbstractButton *, QString> glyphs;
    glyphs.insert(window.m_chatButton, QStringLiteral("comment"));
    glyphs.insert(window.m_settingsNavButton, QStringLiteral("gear"));
    glyphs.insert(window.m_notificationButton, QStringLiteral("bell"));
    glyphs.insert(window.m_navScreenshotButton, QStringLiteral("screen-full"));
    glyphs.insert(window.m_navResizeButton, QStringLiteral("device-desktop"));
    glyphs.insert(window.m_railCodeButton, QStringLiteral("code"));
    glyphs.insert(window.m_railGitButton, QStringLiteral("git-branch"));

    QList<CommandEntry> entries;
    QSet<QAbstractButton *> seen;
    const auto appendButton = [&entries, &seen, &glyphs, devicePixelRatio](
                                  const QString &group, QWidget *widget,
                                  bool onScreen) {
        auto *button = qobject_cast<QAbstractButton *>(widget);
        if (!button || seen.contains(button) || !button->isEnabled())
            return;
        if (onScreen ? !button->isVisible() : button->isHidden())
            return;
        const QString label = button->text().simplified();
        if (label.isEmpty())
            return;
        seen.insert(button);
        CommandEntry entry;
        entry.group = group;
        entry.label = label;
        entry.icon =
            commandButtonIcon(button, glyphs.value(button), devicePixelRatio);
        const QPointer<QAbstractButton> guarded(button);
        entry.run = [guarded] {
            if (guarded && guarded->isEnabled())
                guarded->click();
        };
        entries.append(entry);
    };

    const QString pages = QStringLiteral("Pages");
    if (railLayout) {
        for (int i = 0; i < railLayout->count(); ++i) {
            QLayoutItem *item = railLayout->itemAt(i);
            QWidget *widget = item ? item->widget() : nullptr;
            if (!widget || widget->isHidden())
                continue;
            if (auto *folder = dynamic_cast<RailFolderWidget *>(widget)) {
                const QList<QWidget *> members = folder->members();
                for (QWidget *member : members)
                    appendButton(pages, member, false);
            } else {
                appendButton(pages, widget, false);
            }
        }
    }
    for (const QPointer<RailFolderWidget> &folder : railFolders) {
        if (!folder || folder->isHidden())
            continue;
        const QList<QWidget *> members = folder->members();
        for (QWidget *member : members)
            appendButton(pages, member, false);
    }

    const QString repository = QStringLiteral("Repository");
    if (tabRow) {
        for (int i = 0; i < tabRow->count(); ++i) {
            QLayoutItem *item = tabRow->itemAt(i);
            QWidget *widget = item ? item->widget() : nullptr;
            if (!widget)
                continue;
            if (widget->objectName() != QLatin1String("repoTab") &&
                !dynamic_cast<VerticalIconButton *>(widget) &&
                !dynamic_cast<ActivityRailButton *>(widget))
                continue;
            appendButton(repository, widget, true);
        }
    }

    const QString settings = QStringLiteral("Settings");
    if (SettingsSectionsView *sections = window.m_settingsSections) {
        const auto appendSettingsPlace =
            [&entries, &settings, target, devicePixelRatio](
                const QString &label, const QString &glyph, int section,
                const QString &subTitle) {
                CommandEntry entry;
                entry.group = settings;
                entry.label = label;
                entry.icon = commandGlyphIcon(glyph, devicePixelRatio);
                entry.queryOnly = !subTitle.isEmpty();
                entry.run = [target, section, subTitle] {
                    if (!target)
                        return;
                    target->showSection(kSettingsSectionIndex);
                    SettingsSectionsView *view = target->m_settingsSections;
                    if (!view || section >= view->count())
                        return;
                    const int sub = subTitle.isEmpty()
                                        ? -1
                                        : int(view->subSectionTitles(section)
                                                  .indexOf(subTitle));
                    if (sub >= 0)
                        view->setCurrentSubSection(section, sub);
                    else
                        view->setCurrentIndex(section);
                };
                entries.append(entry);
            };
        for (int section = 0; section < sections->count(); ++section) {
            const QString title = sections->tabText(section).simplified();
            if (title.isEmpty())
                continue;
            const QString glyph = sections->sectionIconName(section);
            appendSettingsPlace(title, glyph, section, QString());
            const QStringList subs = sections->subSectionTitles(section);
            for (const QString &sub : subs) {
                appendSettingsPlace(title + QStringLiteral(" ") + QChar(0x203A) +
                                        QStringLiteral(" ") + sub,
                                    glyph, section, sub);
            }
        }
    }

    const QString actions = QStringLiteral("Actions");
    const auto appendAction = [&entries, &actions, target, devicePixelRatio](
                                  const QString &label, const QString &glyph,
                                  std::function<void(MainWindow &)> run) {
        CommandEntry entry;
        entry.group = actions;
        entry.label = label;
        entry.icon = commandGlyphIcon(glyph, devicePixelRatio);
        entry.run = [target, action = std::move(run)] {
            if (target)
                action(*target);
        };
        entries.append(entry);
    };
    const auto appendSetting = [&appendAction, registry](
                                   const QString &label, const QString &glyph,
                                   const QString &key, const QString &value) {
        appendAction(label, glyph, [key, value, registry](MainWindow &) {
            QSettings().setValue(key, value);
            MainWindow::applyTheme(registry);
        });
    };

    appendAction(QStringLiteral("Toggle sidebar"),
                 QStringLiteral("list-unordered"), [](MainWindow &shell) {
                     NextAccess::toggleRailExpanded(shell);
                 });
    const QString designKey = QString::fromLatin1(Theme::kDesignSetting);
    appendSetting(QStringLiteral("Theme: follow system"),
                  QStringLiteral("device-desktop"), kThemeSetting,
                  QStringLiteral("system"));
    appendSetting(QStringLiteral("Theme: dark"), QStringLiteral("moon"),
                  kThemeSetting, QStringLiteral("dark"));
    appendSetting(QStringLiteral("Theme: light"), QStringLiteral("sun"),
                  kThemeSetting, QStringLiteral("light"));
    appendSetting(QStringLiteral("Theme: Halloween"), QStringLiteral("moon"),
                  kThemeSetting, kHalloweenTheme);
    appendSetting(QStringLiteral("Interface: Classic"),
                  QStringLiteral("history"), designKey,
                  Theme::designSettingValue(Theme::Design::Classic));
    appendSetting(QStringLiteral("Interface: New"), QStringLiteral("sparkle"),
                  designKey, Theme::designSettingValue(Theme::Design::Next));

    CommandQueryEntry settingsSearch;
    settingsSearch.group = settings;
    settingsSearch.label = QStringLiteral("Search settings for ") +
                           QChar(0x201C) + QStringLiteral("%1") + QChar(0x201D);
    settingsSearch.icon = commandGlyphIcon(QStringLiteral("search"),
                                           devicePixelRatio);
    settingsSearch.run = [target](const QString &query) {
        if (!target)
            return;
        target->showSection(kSettingsSectionIndex);
        if (SettingsSectionsView *view = target->m_settingsSections) {
            view->setSearchText(query);
            view->focusSearch();
        }
    };

    if (refresh) {
        overlay->setEntries(entries, settingsSearch);
        return;
    }
    overlay->open(entries, settingsSearch);
    if (!window.m_settingsSections) {
        QTimer::singleShot(kSettingsBuildDelayMs, &window, [target] {
            if (!target || target->m_settingsSections)
                return;
            target->ensureSectionBuilt(kSettingsSectionIndex);
            if (target->m_settingsSections)
                NextAccess::showCommandPalette(*target, true);
        });
    }
}

}
