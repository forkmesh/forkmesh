#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "MainWindowAgentsInternal.h"
#include "MainWindowNextAccess.h"
#include "WorkspaceBrowserPane.h"
#include "WorkspaceLayout.h"
#include "WorkspaceOpenIn.h"
#include "WorkspaceShortcuts.h"

#include <QAbstractButton>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QBoxLayout>
#include <QCheckBox>
#include <QComboBox>
#include <QClipboard>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QEvent>
#include <QFontMetrics>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QLayout>
#include <QLayoutItem>
#include <QLineEdit>
#include <QList>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QShortcut>
#include <QStandardPaths>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyle>
#include <QTableWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace forkmesh::ui {

namespace {

constexpr int kSidebarWidth = 300;
constexpr int kPanelMinWidth = 420;
constexpr int kHeaderHeight = 56;
constexpr int kHeaderMinimum = 430;
constexpr int kHeaderStatusFloor = 540;
constexpr int kRowHeight = 74;
constexpr int kColumnWidth = 780;
constexpr int kColumnGutter = 28;
constexpr int kComposerWidth = 780;
constexpr int kComposerSidePad = 20;
constexpr int kComposerBottomPad = 16;
constexpr int kComposerHeight = 118;
constexpr int kParkedAt = -20000;
constexpr char kListViewSetting[] = "agents/listView";

enum PanelTab { TabChanges, TabTerminal, TabBrowser, TabCount };

enum Command {
    CmdNewSession,
    CmdToggleChanges,
    CmdToggleTerminal,
    CmdToggleBrowser,
    CmdShowChanges,
    CmdShowTerminal,
    CmdShowBrowser,
    CmdClosePanel,
    CmdNewShell,
    CmdDetails,
    CmdQueue,
    CmdSend,
    CmdNextSession,
    CmdPreviousSession,
    CmdModeMenu,
    CmdModelMenu,
    CmdEffortMenu,
    CmdShortcuts,
};

enum MenuKind { MenuSidebar, MenuSession, MenuComposer };

QPixmap paintedGlyph(const QString &kind, const QColor &color, int px)
{
    const qreal ratio = qApp ? qApp->devicePixelRatio() : 1.0;
    QPixmap pixmap(QSize(px, px) * ratio);
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    if (kind == QLatin1String("kebab")) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        const qreal radius = px / 10.0;
        for (const qreal at : {0.2, 0.5, 0.8})
            painter.drawEllipse(QPointF(px * at, px / 2.0), radius, radius);
    } else if (kind == QLatin1String("globe")) {
        QPen pen(color);
        pen.setWidthF(1.2);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        const QRectF circle(1.0, 1.0, px - 2.0, px - 2.0);
        painter.drawEllipse(circle);
        painter.drawEllipse(QRectF(px * 0.31, 1.0, px * 0.38, px - 2.0));
        painter.drawLine(QPointF(1.0, px / 2.0), QPointF(px - 1.0, px / 2.0));
    } else if (kind == QLatin1String("dot")) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        const qreal radius = px * 0.3;
        painter.drawEllipse(QPointF(px / 2.0, px / 2.0), radius, radius);
    } else if (kind == QLatin1String("arrow-up")) {
        QPen pen(color);
        pen.setWidthF(qMax(1.6, px / 8.0));
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        painter.setPen(pen);
        const qreal mid = px / 2.0;
        const qreal top = px * 0.2;
        const qreal wing = px * 0.3;
        painter.drawLine(QPointF(mid, top), QPointF(mid, px * 0.82));
        painter.drawLine(QPointF(mid, top), QPointF(mid - wing, top + wing));
        painter.drawLine(QPointF(mid, top), QPointF(mid + wing, top + wing));
    }
    return pixmap;
}

QPixmap glyphPixmap(const QString &name, const QColor &color, int px)
{
    if (name == QLatin1String("kebab") || name == QLatin1String("globe") ||
        name == QLatin1String("dot") || name == QLatin1String("arrow-up"))
        return paintedGlyph(name, color, px);
    return tintedOcticonPixmap(name, color, px);
}

QIcon glyph(const QString &name, Theme::Role role, int px = 14)
{
    return QIcon(glyphPixmap(name, Theme::color(role), px));
}

QIcon toggleGlyph(const QString &name, int px = 14)
{
    QIcon icon;
    icon.addPixmap(glyphPixmap(name, Theme::color(Theme::Role::FgMuted), px),
                   QIcon::Normal, QIcon::Off);
    icon.addPixmap(glyphPixmap(name, Theme::color(Theme::Role::Fg), px),
                   QIcon::Normal, QIcon::On);
    icon.addPixmap(glyphPixmap(name, Theme::color(Theme::Role::FgSubtle), px),
                   QIcon::Disabled, QIcon::Off);
    return icon;
}

QPixmap badgePixmap(const QString &letter, const QColor &fill, int px)
{
    const qreal ratio = qApp ? qApp->devicePixelRatio() : 1.0;
    QPixmap pixmap(QSize(px, px) * ratio);
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(fill);
    painter.drawRoundedRect(QRectF(0, 0, px, px), px * 0.27, px * 0.27);
    QFont font = QApplication::font();
    font.setPixelSize(qMax(9, int(px * 0.58)));
    font.setWeight(QFont::Bold);
    painter.setFont(font);
    painter.setPen(Theme::color(Theme::Role::OnAccent));
    painter.drawText(QRectF(0, 0, px, px), Qt::AlignCenter, letter.left(1).toUpper());
    return pixmap;
}

void repolish(QWidget *widget)
{
    if (!widget)
        return;
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

void setFlag(QWidget *widget, const char *name, bool on)
{
    if (!widget || widget->property(name).toBool() == on)
        return;
    widget->setProperty(name, on);
    repolish(widget);
}

class SidebarHero : public QWidget
{
public:
    using QWidget::QWidget;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const bool dark = Theme::activeDark();
        QLinearGradient wash(0, 0, 0, height());
        if (dark) {
            QColor top = Theme::color(Theme::Role::Accent).darker(140);
            top.setAlpha(235);
            QColor middle = Theme::color(Theme::Role::Accent).darker(230);
            middle.setAlpha(170);
            QColor end = middle;
            end.setAlpha(0);
            wash.setColorAt(0.0, top);
            wash.setColorAt(0.55, middle);
            wash.setColorAt(1.0, end);
        } else {
            QColor top = Theme::color(Theme::Role::AccentSubtle);
            QColor end = top;
            end.setAlpha(0);
            wash.setColorAt(0.0, top);
            wash.setColorAt(1.0, end);
        }
        painter.fillRect(rect(), wash);
        if (!dark)
            return;
        painter.setPen(Qt::NoPen);
        quint32 seed = 0x9e3779b9u;
        const auto next = [&seed] {
            seed = seed * 1664525u + 1013904223u;
            return (seed >> 8) % 1000u;
        };
        for (int star = 0; star < 28; ++star) {
            const qreal x = next() / 1000.0 * width();
            const qreal y = next() / 1000.0 * height() * 0.66;
            const qreal radius = 0.5 + next() / 1000.0 * 0.8;
            painter.setBrush(QColor(255, 255, 255, 40 + int(next() / 1000.0 * 120)));
            painter.drawEllipse(QPointF(x, y), radius, radius);
        }
    }
};

class Breadcrumb : public QWidget
{
public:
    explicit Breadcrumb(QWidget *parent = nullptr) : QWidget(parent)
    {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    void setParts(const QString &prefix, const QString &title)
    {
        if (m_prefix == prefix && m_title == title)
            return;
        m_prefix = prefix;
        m_title = title;
        setToolTip(title);
        setAccessibleName(prefix.isEmpty() ? title
                                           : prefix + QStringLiteral(" / ") + title);
        updateGeometry();
        update();
    }

    QString title() const { return m_title; }

    QSize sizeHint() const override
    {
        return QSize(prefixWidth() +
                         QFontMetrics(titleFont()).horizontalAdvance(m_title) + 4,
                     QFontMetrics(titleFont()).height() + 6);
    }

    QSize minimumSizeHint() const override
    {
        return QSize(60, QFontMetrics(titleFont()).height() + 6);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        int left = 0;
        const int lead = prefixWidth();
        if (lead > 0 && width() > lead + 80) {
            painter.setFont(prefixFont());
            painter.setPen(Theme::color(Theme::Role::FgMuted));
            const int text = QFontMetrics(prefixFont()).horizontalAdvance(m_prefix);
            painter.drawText(QRect(0, 0, text, height()),
                             Qt::AlignVCenter | Qt::AlignLeft, m_prefix);
            painter.setPen(Theme::color(Theme::Role::FgSubtle));
            painter.drawText(QRect(text, 0, lead - text, height()),
                             Qt::AlignCenter, QStringLiteral("/"));
            left = lead;
        }
        painter.setFont(titleFont());
        painter.setPen(Theme::color(Theme::Role::Fg));
        painter.drawText(QRect(left, 0, width() - left, height()),
                         Qt::AlignVCenter | Qt::AlignLeft,
                         QFontMetrics(titleFont())
                             .elidedText(m_title, Qt::ElideRight, width() - left));
    }

private:
    QFont prefixFont() const
    {
        QFont font = QWidget::font();
        font.setPixelSize(15);
        return font;
    }

    QFont titleFont() const
    {
        QFont font = QWidget::font();
        font.setPixelSize(15);
        font.setWeight(QFont::DemiBold);
        return font;
    }

    int prefixWidth() const
    {
        if (m_prefix.isEmpty())
            return 0;
        return QFontMetrics(prefixFont()).horizontalAdvance(m_prefix) + 28;
    }

    QString m_prefix;
    QString m_title;
};

QPushButton *iconButton(const QString &name, const QString &tip,
                        bool checkable = false)
{
    auto *button = new QPushButton;
    button->setObjectName(name);
    button->setProperty("workspaceIconButton", true);
    button->setCursor(Qt::PointingHandCursor);
    button->setFocusPolicy(Qt::NoFocus);
    button->setFixedSize(30, 30);
    button->setIconSize(QSize(14, 14));
    button->setFlat(true);
    button->setCheckable(checkable);
    button->setToolTip(tip);
    button->setAccessibleName(tip);
    return button;
}

QPushButton *textButton(const QString &name, const QString &text,
                        const QString &tip, int height = 32)
{
    auto *button = new QPushButton(text);
    button->setObjectName(name);
    button->setCursor(Qt::PointingHandCursor);
    button->setFocusPolicy(Qt::NoFocus);
    button->setFixedHeight(height);
    button->setIconSize(QSize(14, 14));
    button->setToolTip(tip);
    return button;
}

QList<QWidget *> layoutWidgets(QLayout *layout)
{
    QList<QWidget *> widgets;
    if (!layout)
        return widgets;
    for (int index = 0; index < layout->count(); ++index) {
        QLayoutItem *item = layout->itemAt(index);
        if (QWidget *widget = item->widget())
            widgets.append(widget);
        else if (QLayout *child = item->layout())
            widgets.append(layoutWidgets(child));
    }
    return widgets;
}

void moveInto(QWidget *widget, QWidget *host)
{
    if (!widget || !host || !host->layout() || widget->parentWidget() == host)
        return;
    const bool shown = !widget->isHidden();
    widget->setParent(host);
    host->layout()->addWidget(widget);
    widget->setVisible(shown);
}

QString sessionTitle(const AgentSession &session)
{
    QString title = session.issueTitle.simplified();
    if (session.issueNumber <= 0 && !session.prompt.trimmed().isEmpty())
        title = session.prompt.section(QLatin1Char('\n'), 0, 0).simplified();
    if (title.isEmpty() && !session.branchName.isEmpty())
        title = session.branchName;
    if (title.isEmpty())
        title = QStringLiteral("Agent #%1").arg(session.id);
    return title;
}

struct StatusInfo {
    QString text;
    Theme::Role tone = Theme::Role::FgSubtle;
};

StatusInfo statusInfo(const AgentSession &session)
{
    if (session.merged)
        return {QStringLiteral("Merged"), Theme::Role::AccentFg};
    if (session.status == AgentStatus::Success)
        return {QStringLiteral("Done"), Theme::Role::Success};
    if (session.status == AgentStatus::Failed)
        return {QStringLiteral("Failed"), Theme::Role::Danger};
    if (session.status == AgentStatus::Stopped)
        return {QStringLiteral("Stopped"), Theme::Role::FgSubtle};
    if (session.finishingBackgroundWork())
        return {QStringLiteral("Finishing"), Theme::Role::AccentFg};
    if (session.status == AgentStatus::Running)
        return {QStringLiteral("Running"), Theme::Role::AccentFg};
    if (session.status == AgentStatus::Waiting)
        return {QStringLiteral("Waiting"), Theme::Role::Warning};
    if (session.status == AgentStatus::Queued)
        return {QStringLiteral("Queued"), Theme::Role::Warning};
    return {QStringLiteral("Idle"), Theme::Role::FgSubtle};
}

void addProxy(QMenu *menu, QAbstractButton *button, const QString &icon,
              const QString &label = QString(), bool evenIfHidden = false)
{
    if (!menu || !button || (button->isHidden() && !evenIfHidden))
        return;
    const QString text = label.isEmpty() ? button->text() : label;
    if (text.isEmpty())
        return;
    QAction *action = menu->addAction(
        icon.isEmpty() ? QIcon() : glyph(icon, Theme::Role::FgMuted), text);
    action->setEnabled(button->isEnabled());
    action->setToolTip(button->toolTip());
    if (button->isCheckable()) {
        action->setCheckable(true);
        action->setChecked(button->isChecked());
    }
    const QPointer<QAbstractButton> target(button);
    QObject::connect(action, &QAction::triggered, button, [target] {
        if (target)
            target->click();
    });
}

struct ChangeStats {
    int files = 0;
    int added = 0;
    int removed = 0;
    QString branch;

    bool any() const { return files > 0 || added > 0 || removed > 0; }
};

struct Chrome : public QObject {
    Chrome(MainWindow *owner, QObject *parent) : QObject(parent), window(owner) {}

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        switch (event->type()) {
        case QEvent::ShowToParent:
        case QEvent::HideToParent:
        case QEvent::EnabledChange:
            if (!syncing)
                refresh.start();
            break;
        case QEvent::Show:
            if (watched == split.data() && !syncing)
                refresh.start();
            if (watched == page.data())
                enableShortcuts();
            break;
        case QEvent::Hide:
            if (watched == page.data())
                enableShortcuts();
            break;
        case QEvent::Resize:
            if (watched == transcript.data())
                column.start();
            else if (watched == split.data() && needsSizing && !syncing)
                refresh.start();
            else if (watched == header && !syncing)
                refresh.start();
            break;
        default:
            break;
        }
        return QObject::eventFilter(watched, event);
    }

    void enableShortcuts()
    {
        const bool on = applied && page && page->isVisible();
        for (QShortcut *shortcut : std::as_const(shortcuts))
            if (shortcut && shortcut->isEnabled() != on)
                shortcut->setEnabled(on);
    }

    MainWindow *window = nullptr;
    QPointer<QWidget> page;
    QList<QPointer<QShortcut>> shortcuts;
    QTimer refresh;
    QTimer column;
    QTimer tick;
    bool applied = false;
    bool syncing = false;
    bool panelOpen = false;
    int tab = TabTerminal;
    AppViewMode lastMode = AppViewMode::Standard;
    bool lastDark = false;
    bool iconsReady = false;
    bool needsSizing = false;
    QPointer<QSplitter> split;
    QString classicListView;

    QPointer<QWidget> listPane;
    QPointer<QWidget> toolbar;
    QWidget *sidebarHeader = nullptr;
    QLabel *sidebarBadge = nullptr;
    QLabel *sidebarBrand = nullptr;
    QLineEdit *sidebarSearch = nullptr;
    QLabel *sidebarCount = nullptr;
    QPushButton *sidebarSort = nullptr;
    QPushButton *sidebarMore = nullptr;
    QPushButton *sidebarNew = nullptr;
    QWidget *sidebarFooter = nullptr;
    QPushButton *sidebarQueue = nullptr;
    QString brandLetter;

    QVBoxLayout *detailLayout = nullptr;
    QVBoxLayout *topRow = nullptr;
    QHBoxLayout *statusRow = nullptr;
    int statusIndex = 0;
    QPointer<QWidget> titleBlock;
    QPointer<QWidget> outputPage;
    QPointer<QScrollArea> transcript;
    QMargins classicDetailMargins;
    QMargins classicOutputMargins;
    QMargins classicColumnMargins;
    int classicDetailSpacing = 0;
    int classicBaseBottom = 0;

    QWidget *header = nullptr;
    Breadcrumb *title = nullptr;
    QPushButton *status = nullptr;
    QPushButton *stop = nullptr;
    QPushButton *resume = nullptr;
    QPushButton *toggles[TabCount] = {nullptr, nullptr, nullptr};
    QPushButton *more = nullptr;
    QWidget *empty = nullptr;
    QLabel *emptyBody = nullptr;
    QWidget *changesBar = nullptr;
    QLabel *changesText = nullptr;
    QPushButton *changesOpen = nullptr;
    QString changesHtml;

    QVBoxLayout *dockLayout = nullptr;
    QList<QPointer<QWidget>> classicDockHeader;
    QMargins classicDockMargins;
    int classicDockSpacing = 0;
    int classicDockMinimum = 0;
    int classicHandleWidth = 0;
    QWidget *panelBar = nullptr;
    QPushButton *tabs[TabCount] = {nullptr, nullptr, nullptr};
    QLabel *panelBranch = nullptr;
    QLabel *panelStats = nullptr;
    QPushButton *panelNewShell = nullptr;
    QPushButton *panelClose = nullptr;
    QStackedWidget *panelStack = nullptr;
    QWidget *hosts[TabCount] = {nullptr, nullptr, nullptr};
    WorkspaceBrowserPane *browser = nullptr;
    QPointer<QSplitter> filesSplit;

    bool composerReady = false;
    bool composerDocked = false;
    QHBoxLayout *composerRow = nullptr;
    QVBoxLayout *sendColumn = nullptr;
    QHBoxLayout *composerBarRow = nullptr;
    bool classicEditFill = true;
    QPushButton *composerMenu = nullptr;
    QPushButton *composerSend = nullptr;
};

QHash<const MainWindow *, QPointer<Chrome>> &registry()
{
    static QHash<const MainWindow *, QPointer<Chrome>> chromes;
    return chromes;
}

Chrome *chromeOf(const MainWindow &window)
{
    return registry().value(&window).data();
}

QWidget *paneHost(const QString &name)
{
    auto *host = new QWidget;
    host->setObjectName(name);
    auto *layout = new QVBoxLayout(host);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    return host;
}

QString statsHtml(const ChangeStats &stats)
{
    return QStringLiteral("<span style='color:%1'>+%2</span>&nbsp;&nbsp;"
                          "<span style='color:%3'>−%4</span>")
        .arg(Theme::hex(Theme::Role::Success))
        .arg(stats.added)
        .arg(Theme::hex(Theme::Role::Danger))
        .arg(stats.removed);
}

void refreshIcons(Chrome *chrome)
{
    const Theme::Role muted = Theme::Role::FgMuted;
    const bool dark = Theme::activeDark();
    chrome->sidebarSort->setIcon(glyph(QStringLiteral("sort-desc"), muted));
    chrome->sidebarMore->setIcon(glyph(QStringLiteral("kebab"), muted));
    chrome->sidebarNew->setIcon(glyph(QStringLiteral("plus"), Theme::Role::Fg));
    chrome->more->setIcon(glyph(QStringLiteral("kebab"), muted));
    chrome->stop->setIcon(glyph(QStringLiteral("stop"), Theme::Role::Danger));
    chrome->resume->setIcon(glyph(QStringLiteral("play"), Theme::Role::Fg));
    chrome->toggles[TabTerminal]->setIcon(toggleGlyph(QStringLiteral("terminal")));
    chrome->toggles[TabBrowser]->setIcon(toggleGlyph(QStringLiteral("globe")));
    chrome->panelNewShell->setIcon(glyph(QStringLiteral("plus"), muted));
    chrome->panelClose->setIcon(glyph(QStringLiteral("x"), muted));
    if (chrome->composerSend) {
        const bool chat = chrome->composerSend->property("workspaceChat").toBool();
        chrome->composerSend->setIcon(
            glyph(QStringLiteral("arrow-up"),
                  chat ? Theme::Role::OnAccent : Theme::Role::Fg, chat ? 14 : 16));
        chrome->composerMenu->setIcon(glyph(QStringLiteral("kebab"), muted));
    }
    if (chrome->browser) {
        WorkspaceBrowserPane::Icons icons;
        icons.back = toggleGlyph(QStringLiteral("chevron-left"));
        icons.forward = toggleGlyph(QStringLiteral("chevron-right"));
        icons.reload = toggleGlyph(QStringLiteral("sync"));
        icons.external = toggleGlyph(QStringLiteral("link-external"));
        chrome->browser->setIcons(icons);
    }
    chrome->sidebarSearch->setStyleSheet(
        dark ? QStringLiteral(
                   "QLineEdit{background:rgba(255,255,255,0.10);"
                   "border:1px solid rgba(255,255,255,0.10);border-radius:9px;"
                   "padding:0px 12px;color:%1;}"
                   "QLineEdit:focus{border-color:rgba(255,255,255,0.32);}")
                   .arg(Theme::hex(Theme::Role::Fg))
             : QStringLiteral(
                   "QLineEdit{background:rgba(255,255,255,0.85);"
                   "border:1px solid %2;border-radius:9px;padding:0px 12px;"
                   "color:%1;}"
                   "QLineEdit:focus{border-color:%3;}")
                   .arg(Theme::hex(Theme::Role::Fg),
                        Theme::hex(Theme::Role::BorderStrong),
                        Theme::hex(Theme::Role::Accent)));
    chrome->brandLetter.clear();
    chrome->changesHtml.clear();
    chrome->status->setProperty("workspaceTone", -1);
    chrome->iconsReady = true;
}

void fitColumn(Chrome *chrome)
{
    if (!chrome->transcript || !chrome->transcript->widget() ||
        !chrome->transcript->widget()->layout())
        return;
    QLayout *column = chrome->transcript->widget()->layout();
    if (!chrome->applied) {
        if (column->contentsMargins() != chrome->classicColumnMargins)
            column->setContentsMargins(chrome->classicColumnMargins);
        return;
    }
    const int width = chrome->transcript->viewport()->width();
    const int side = qMax(kColumnGutter, (width - kColumnWidth) / 2);
    const QMargins wanted(qMax(4, side - 20), 20, side, 24);
    if (column->contentsMargins() != wanted)
        column->setContentsMargins(wanted);
}

void applyRowHeight(QTableWidget *table, int height)
{
    if (!table || table->verticalHeader()->defaultSectionSize() == height)
        return;
    table->verticalHeader()->setDefaultSectionSize(height);
    for (int row = 0; row < table->rowCount(); ++row)
        table->setRowHeight(row, height);
}

void showWorkspaceShortcuts(QWidget *parent)
{
    if (QDialog *open = parent->findChild<QDialog *>(
            QStringLiteral("workspaceShortcutSheet"), Qt::FindDirectChildrenOnly)) {
        open->raise();
        open->activateWindow();
        return;
    }
    auto *dialog = new QDialog(parent);
    dialog->setObjectName(QStringLiteral("workspaceShortcutSheet"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QStringLiteral("Keyboard shortcuts"));
    auto *layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(20, 18, 20, 14);
    layout->setSpacing(12);
    auto *heading = new QLabel(QStringLiteral("Agents workspace"));
    heading->setObjectName(QStringLiteral("workspaceShortcutHeading"));
    layout->addWidget(heading);
    auto *grid = new QGridLayout;
    grid->setHorizontalSpacing(24);
    grid->setVerticalSpacing(6);
    int row = 0;
    for (const forkmesh::workspaceshortcuts::Binding &binding :
         forkmesh::workspaceshortcuts::bindings()) {
        auto *label = new QLabel(binding.label);
        label->setObjectName(QStringLiteral("workspaceShortcutLabel"));
        auto *keys = new QLabel(forkmesh::workspaceshortcuts::keysText(binding.keys));
        keys->setObjectName(QStringLiteral("workspaceShortcutKeys"));
        keys->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        grid->addWidget(label, row, 0);
        grid->addWidget(keys, row, 1);
        ++row;
    }
    layout->addLayout(grid);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    layout->addWidget(buttons);
    dialog->show();
}

void addOpenInMenu(QMenu *menu, QWidget *owner, const QString &folder,
                   const QString &branch)
{
    if (folder.isEmpty() && branch.isEmpty())
        return;
    QMenu *open = menu->addMenu(glyph(QStringLiteral("code"), Theme::Role::FgMuted),
                                QStringLiteral("Open in"));
    open->setObjectName(QStringLiteral("workspaceOpenInMenu"));
    open->setToolTipsVisible(true);
    if (!folder.isEmpty()) {
        const QList<forkmesh::openin::Editor> editors =
            forkmesh::openin::availableEditors([](const QString &program) {
                return QStandardPaths::findExecutable(program);
            });
        for (const forkmesh::openin::Editor &editor : editors) {
            QAction *action = open->addAction(editor.label);
            action->setToolTip(QStringLiteral("%1 %2").arg(editor.program, folder));
            QObject::connect(action, &QAction::triggered, owner,
                             [program = editor.program, folder] {
                                 QProcess::startDetached(program, {folder}, folder);
                             });
        }
        if (editors.isEmpty()) {
            QAction *none = open->addAction(QStringLiteral("No editor found on PATH"));
            none->setEnabled(false);
            none->setToolTip(QStringLiteral(
                "Looks for code, codium, cursor, zed and windsurf"));
        }
        open->addSeparator();
        QAction *files = open->addAction(
            glyph(QStringLiteral("file-directory"), Theme::Role::FgMuted),
            QStringLiteral("File manager"));
        QObject::connect(files, &QAction::triggered, owner, [folder] {
            QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
        });
    }
    menu->addSeparator();
    if (!folder.isEmpty()) {
        QAction *copy = menu->addAction(glyph(QStringLiteral("copy"), Theme::Role::FgMuted),
                                        QStringLiteral("Copy folder path"));
        QObject::connect(copy, &QAction::triggered, owner, [folder] {
            QGuiApplication::clipboard()->setText(QDir::toNativeSeparators(folder));
        });
    }
    if (!branch.isEmpty()) {
        QAction *copy = menu->addAction(glyph(QStringLiteral("copy"), Theme::Role::FgMuted),
                                        QStringLiteral("Copy branch name"));
        QObject::connect(copy, &QAction::triggered, owner,
                         [branch] { QGuiApplication::clipboard()->setText(branch); });
    }
}

}

bool paintWorkspaceSessionRow(QPainter *painter,
                              const QStyleOptionViewItem &option,
                              const QModelIndex &index, bool hovered,
                              QColor *fill, QPainterPath *shape)
{
    const QWidget *view = option.widget;
    if (!view || !view->property("workspaceCards").toBool())
        return false;
    QString words[AgentChipSlotCount];
    QColor statusColor = Theme::color(Theme::Role::FgSubtle);
    for (const QVariant &chip : index.data(kAgentRowChipsRole).toList()) {
        const QStringList parts = chip.toStringList();
        bool ok = false;
        const int slot = parts.value(3).toInt(&ok);
        if (!ok || slot < 0 || slot >= AgentChipSlotCount)
            continue;
        words[slot] = parts.value(2);
        if (slot == AgentChipStatus && QColor(parts.value(1)).isValid())
            statusColor = QColor(parts.value(1));
    }

    const bool selected = option.state & QStyle::State_Selected;
    const QRectF card = QRectF(option.rect).adjusted(6, 2, -6, -2);
    QPainterPath path;
    path.addRoundedRect(card.adjusted(0.5, 0.5, -0.5, -0.5), 10, 10);
    const QColor base = selected  ? Theme::color(Theme::Role::SurfaceRaised)
                        : hovered ? Theme::color(Theme::Role::Surface)
                                  : Theme::color(Theme::Role::BgSubtle);
    if (fill)
        *fill = base;
    if (shape)
        *shape = path;

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    if (selected || hovered) {
        painter->setPen(selected ? QPen(Theme::color(Theme::Role::Border), 1.0)
                                 : QPen(Qt::NoPen));
        painter->setBrush(base);
        painter->drawPath(path);
    }

    const int left = int(card.left()) + 12;
    const int right = int(card.right()) - 12;
    const int top = int(card.top()) + 9;
    QFont small = option.font;
    small.setPixelSize(12);
    small.setWeight(QFont::Normal);
    QFont heading = option.font;
    heading.setPixelSize(14);
    heading.setWeight(selected ? QFont::DemiBold : QFont::Medium);
    const QFontMetrics smallMetrics(small);

    const QString provider =
        words[AgentChipProvider].isEmpty() ? QStringLiteral("Agent")
                                           : words[AgentChipProvider];
    painter->drawPixmap(QPoint(left, top), badgePixmap(provider, statusColor, 16));

    const QString age = words[AgentChipAge];
    const int ageWidth = age.isEmpty() ? 0 : smallMetrics.horizontalAdvance(age);
    painter->setFont(small);
    painter->setPen(Theme::color(Theme::Role::FgSubtle));
    painter->drawText(QRect(right - ageWidth, top, ageWidth, 16),
                      Qt::AlignVCenter | Qt::AlignRight, age);

    const QString scope =
        words[AgentChipStatus].isEmpty()
            ? provider
            : provider + QStringLiteral(" · ") + words[AgentChipStatus];
    const int scopeLeft = left + 16 + 8;
    const int scopeWidth = qMax(0, right - ageWidth - 8 - scopeLeft);
    painter->setPen(Theme::color(Theme::Role::FgMuted));
    painter->drawText(QRect(scopeLeft, top, scopeWidth, 16),
                      Qt::AlignVCenter | Qt::AlignLeft,
                      smallMetrics.elidedText(scope, Qt::ElideRight, scopeWidth));

    painter->setFont(heading);
    painter->setPen(Theme::color(selected ? Theme::Role::Fg
                                          : Theme::Role::FgSecondary));
    painter->drawText(QRect(left, top + 21, right - left, 20),
                      Qt::AlignVCenter | Qt::AlignLeft,
                      QFontMetrics(heading).elidedText(
                          index.data(Qt::DisplayRole).toString(), Qt::ElideRight,
                          right - left));

    QStringList meta;
    if (!words[AgentChipTeammate].isEmpty())
        meta << words[AgentChipTeammate];
    if (!words[AgentChipHost].isEmpty())
        meta << words[AgentChipHost];
    if (!words[AgentChipChurn].isEmpty()) {
        QString churn = words[AgentChipChurn];
        churn.replace(QChar(0x2212), QStringLiteral(" −"));
        meta << churn;
    }
    if (!words[AgentChipSync].isEmpty())
        meta << words[AgentChipSync];
    if (!words[AgentChipDirty].isEmpty())
        meta << QStringLiteral("Uncommitted");
    painter->setFont(small);
    painter->setPen(Theme::color(Theme::Role::FgSubtle));
    painter->drawText(QRect(left, top + 43, right - left, 16),
                      Qt::AlignVCenter | Qt::AlignLeft,
                      smallMetrics.elidedText(meta.join(QStringLiteral(" · ")),
                                              Qt::ElideRight, right - left));
    painter->restore();
    return true;
}

void NextAccess::installWorkspace(MainWindow &window)
{
    if (chromeOf(window) || !window.m_agentsPage || !window.m_agentDetail ||
        !window.m_agentTable || !window.m_agentWorkspaceSplit ||
        !window.m_workspaceTerminalDock || !window.m_workspaceTerminal ||
        !window.m_agentCodePage || !window.m_agentCodeSplit ||
        !window.m_agentStatusPill || !window.m_agentTitle)
        return;
    addThemeHook(&NextAccess::syncWorkspace);
    auto *detailLayout =
        qobject_cast<QVBoxLayout *>(window.m_agentDetail->layout());
    auto *topRow = detailLayout && detailLayout->count() > 0
                       ? qobject_cast<QVBoxLayout *>(
                             detailLayout->itemAt(0)->layout())
                       : nullptr;
    QHBoxLayout *statusRow = nullptr;
    int statusIndex = -1;
    for (int index = 0; topRow && index < topRow->count() && !statusRow; ++index) {
        auto *row = qobject_cast<QHBoxLayout *>(topRow->itemAt(index)->layout());
        if (row && row->indexOf(window.m_agentStatusPill) >= 0) {
            statusRow = row;
            statusIndex = index;
        }
    }
    auto *dockLayout =
        qobject_cast<QVBoxLayout *>(window.m_workspaceTerminalDock->layout());
    QWidget *listPane = window.m_agentTable->parentWidget();
    auto *listLayout =
        listPane ? qobject_cast<QVBoxLayout *>(listPane->layout()) : nullptr;
    QWidget *toolbar = window.m_agentStartAllButton
                           ? window.m_agentStartAllButton->parentWidget()
                           : nullptr;
    if (!statusRow || statusRow->indexOf(window.m_agentStatusPill) < 0 ||
        !dockLayout || dockLayout->count() < 2 || !listLayout || !toolbar)
        return;

    auto *chrome = new Chrome(&window, window.m_agentsPage);
    registry().insert(&window, chrome);
    MainWindow *self = &window;
    const auto command = [self](int id) {
        return [self, id] { NextAccess::workspaceCommand(*self, id); };
    };
    const auto popMenu = [self](QPushButton *anchor, int kind, bool alignRight) {
        QMenu menu(self);
        menu.setToolTipsVisible(true);
        NextAccess::fillWorkspaceMenu(*self, &menu, kind);
        if (menu.isEmpty())
            return;
        const int x = alignRight ? anchor->width() - menu.sizeHint().width() : 0;
        menu.exec(anchor->mapToGlobal(QPoint(x, anchor->height() + 4)));
    };

    chrome->refresh.setSingleShot(true);
    chrome->refresh.setInterval(0);
    QObject::connect(&chrome->refresh, &QTimer::timeout, chrome,
                     [self] { NextAccess::syncWorkspace(*self); });
    chrome->column.setSingleShot(true);
    chrome->column.setInterval(0);
    QObject::connect(&chrome->column, &QTimer::timeout, chrome,
                     [chrome] { fitColumn(chrome); });
    chrome->tick.setInterval(1000);
    QObject::connect(&chrome->tick, &QTimer::timeout, chrome, [self, chrome] {
        if (chrome->applied && chrome->header && chrome->header->isVisible())
            NextAccess::syncWorkspace(*self);
    });

    window.m_agentWorkspaceSplit->setObjectName(
        QStringLiteral("agentWorkspaceSplit"));
    chrome->classicHandleWidth = window.m_agentWorkspaceSplit->handleWidth();
    chrome->split = window.m_agentWorkspaceSplit;
    window.m_agentWorkspaceSplit->installEventFilter(chrome);

    chrome->listPane = listPane;
    chrome->toolbar = toolbar;
    listPane->setAttribute(Qt::WA_StyledBackground, true);
    auto *sidebar = new SidebarHero;
    sidebar->setObjectName(QStringLiteral("workspaceSidebarHeader"));
    auto *sidebarLayout = new QVBoxLayout(sidebar);
    sidebarLayout->setContentsMargins(16, 16, 12, 6);
    sidebarLayout->setSpacing(12);
    auto *brandRow = new QHBoxLayout;
    brandRow->setContentsMargins(0, 0, 0, 0);
    brandRow->setSpacing(10);
    chrome->sidebarBadge = new QLabel;
    chrome->sidebarBadge->setFixedSize(22, 22);
    chrome->sidebarBrand = new QLabel;
    chrome->sidebarBrand->setObjectName(QStringLiteral("workspaceBrand"));
    chrome->sidebarBrand->setSizePolicy(QSizePolicy::Ignored,
                                        QSizePolicy::Preferred);
    chrome->sidebarNew = iconButton(
        QStringLiteral("workspaceNewSession"),
        QStringLiteral("New session: clear the selection and type a prompt for "
                       "a new agent"));
    brandRow->addWidget(chrome->sidebarBadge);
    brandRow->addWidget(chrome->sidebarBrand, 1);
    brandRow->addWidget(chrome->sidebarNew);
    chrome->sidebarSearch = new QLineEdit;
    chrome->sidebarSearch->setObjectName(QStringLiteral("workspaceSearch"));
    chrome->sidebarSearch->setPlaceholderText(QStringLiteral("Search"));
    chrome->sidebarSearch->setFixedHeight(34);
    chrome->sidebarSearch->setClearButtonEnabled(true);
    auto *sectionRow = new QHBoxLayout;
    sectionRow->setContentsMargins(2, 4, 0, 0);
    sectionRow->setSpacing(2);
    auto *sidebarTitle = new QLabel(QStringLiteral("Sessions"));
    sidebarTitle->setObjectName(QStringLiteral("workspaceSidebarTitle"));
    chrome->sidebarCount = new QLabel;
    chrome->sidebarCount->setObjectName(QStringLiteral("workspaceSidebarCount"));
    chrome->sidebarSort = iconButton(QStringLiteral("workspaceSidebarSort"),
                                     QStringLiteral("Sort sessions"));
    chrome->sidebarMore = iconButton(QStringLiteral("workspaceSidebarMore"),
                                     QStringLiteral("Session list actions"));
    chrome->sidebarSort->setFixedSize(26, 26);
    chrome->sidebarMore->setFixedSize(26, 26);
    sectionRow->addWidget(sidebarTitle);
    sectionRow->addStretch(1);
    sectionRow->addWidget(chrome->sidebarCount);
    sectionRow->addSpacing(6);
    sectionRow->addWidget(chrome->sidebarSort);
    sectionRow->addWidget(chrome->sidebarMore);
    sidebarLayout->addLayout(brandRow);
    sidebarLayout->addWidget(chrome->sidebarSearch);
    sidebarLayout->addLayout(sectionRow);
    listLayout->insertWidget(0, sidebar);
    sidebar->hide();
    chrome->sidebarHeader = sidebar;

    auto *footer = new QWidget;
    footer->setObjectName(QStringLiteral("workspaceSidebarFooter"));
    footer->setAttribute(Qt::WA_StyledBackground, true);
    auto *footerRow = new QHBoxLayout(footer);
    footerRow->setContentsMargins(12, 8, 12, 10);
    footerRow->setSpacing(0);
    chrome->sidebarQueue = textButton(
        QStringLiteral("workspaceQueue"), QString(),
        QStringLiteral("Agents running now, out of the most that may run at "
                       "once. Click to change the limit."),
        26);
    footerRow->addWidget(chrome->sidebarQueue);
    footerRow->addStretch(1);
    listLayout->addWidget(footer);
    footer->hide();
    chrome->sidebarFooter = footer;

    QObject::connect(chrome->sidebarNew, &QPushButton::clicked, chrome,
                     command(CmdNewSession));
    QObject::connect(chrome->sidebarQueue, &QPushButton::clicked, chrome,
                     command(CmdQueue));
    QObject::connect(chrome->sidebarSort, &QPushButton::clicked, chrome,
                     [self, chrome] {
                         if (!self->m_agentSortButton ||
                             !self->m_agentSortButton->menu())
                             return;
                         self->m_agentSortButton->menu()->popup(
                             chrome->sidebarSort->mapToGlobal(
                                 QPoint(0, chrome->sidebarSort->height() + 4)));
                     });
    QObject::connect(chrome->sidebarMore, &QPushButton::clicked, chrome,
                     [chrome, popMenu] {
                         popMenu(chrome->sidebarMore, MenuSidebar, false);
                     });
    QObject::connect(chrome->sidebarSearch, &QLineEdit::textChanged, chrome,
                     [self](const QString &text) {
                         if (self->m_globalSearch &&
                             self->m_globalSearch->text() != text)
                             self->m_globalSearch->setText(text);
                     });
    if (window.m_globalSearch)
        QObject::connect(window.m_globalSearch, &QLineEdit::textChanged, chrome,
                         [chrome](const QString &text) {
                             if (chrome->applied &&
                                 chrome->sidebarSearch->text() != text) {
                                 const QSignalBlocker blocker(
                                     chrome->sidebarSearch);
                                 chrome->sidebarSearch->setText(text);
                             }
                         });

    chrome->detailLayout = detailLayout;
    chrome->topRow = topRow;
    chrome->statusRow = statusRow;
    chrome->statusIndex = statusIndex;
    chrome->titleBlock = window.m_agentTitle->parentWidget();
    chrome->outputPage = window.m_agentCodeSplit->parentWidget();
    chrome->transcript = window.m_agentTranscript;
    chrome->classicDetailMargins = detailLayout->contentsMargins();
    chrome->classicDetailSpacing = detailLayout->spacing();
    chrome->classicBaseBottom = window.m_agentDetailBaseBottomMargin;
    if (chrome->outputPage && chrome->outputPage->layout())
        chrome->classicOutputMargins =
            chrome->outputPage->layout()->contentsMargins();
    if (chrome->transcript && chrome->transcript->widget() &&
        chrome->transcript->widget()->layout())
        chrome->classicColumnMargins =
            chrome->transcript->widget()->layout()->contentsMargins();

    auto *header = new QWidget;
    header->setObjectName(QStringLiteral("workspaceHeader"));
    header->setAttribute(Qt::WA_StyledBackground, true);
    header->setFixedHeight(kHeaderHeight);
    header->setMinimumWidth(kHeaderMinimum);
    header->installEventFilter(chrome);
    auto *headerRow = new QHBoxLayout(header);
    headerRow->setContentsMargins(24, 0, 14, 0);
    headerRow->setSpacing(8);
    chrome->title = new Breadcrumb;
    chrome->title->setObjectName(QStringLiteral("workspaceTitle"));
    chrome->status = textButton(
        QStringLiteral("workspaceStatus"), QString(),
        QStringLiteral("Session details: model, branch, tokens and cost"), 26);
    chrome->status->setIconSize(QSize(10, 10));
    chrome->stop = textButton(QStringLiteral("workspaceStop"),
                              QStringLiteral("Stop"),
                              QStringLiteral("Stop this session's running agent"));
    chrome->resume = textButton(
        QStringLiteral("workspaceResume"), QStringLiteral("Continue"),
        QStringLiteral("Resume this session with the agent, model and mode "
                       "chosen in the composer"));
    chrome->toggles[TabChanges] = textButton(
        QStringLiteral("workspaceToggleChanges"), QStringLiteral("Show diff"),
        QStringLiteral("This session's edits as a live diff"));
    chrome->toggles[TabChanges]->setCheckable(true);
    chrome->toggles[TabTerminal] =
        iconButton(QStringLiteral("workspaceToggleTerminal"),
                   QStringLiteral("Terminal: a shell in this session's worktree"),
                   true);
    chrome->toggles[TabBrowser] = iconButton(
        QStringLiteral("workspaceToggleBrowser"),
        QStringLiteral("Browser: preview a page beside the conversation"), true);
    chrome->more = iconButton(QStringLiteral("workspaceSessionMore"),
                              QStringLiteral("Session actions"));
    headerRow->addWidget(chrome->title, 1);
    headerRow->addWidget(chrome->status);
    headerRow->addWidget(chrome->stop);
    headerRow->addWidget(chrome->resume);
    headerRow->addWidget(chrome->toggles[TabChanges]);
    headerRow->addSpacing(2);
    headerRow->addWidget(chrome->toggles[TabTerminal]);
    headerRow->addWidget(chrome->toggles[TabBrowser]);
    headerRow->addWidget(chrome->more);
    detailLayout->insertWidget(0, header);
    header->hide();
    chrome->header = header;

    auto *changesBar = new QWidget;
    changesBar->setObjectName(QStringLiteral("workspaceChangesBar"));
    auto *changesOuter = new QHBoxLayout(changesBar);
    changesOuter->setContentsMargins(kComposerSidePad, 6, kComposerSidePad, 10);
    changesOuter->setSpacing(0);
    auto *changesCard = new QFrame;
    changesCard->setObjectName(QStringLiteral("workspaceChangesCard"));
    changesCard->setMaximumWidth(kComposerWidth);
    auto *changesRow = new QHBoxLayout(changesCard);
    changesRow->setContentsMargins(18, 10, 10, 10);
    changesRow->setSpacing(10);
    chrome->changesText = new QLabel;
    chrome->changesText->setObjectName(QStringLiteral("workspaceChangesText"));
    chrome->changesText->setTextFormat(Qt::RichText);
    chrome->changesText->setSizePolicy(QSizePolicy::Ignored,
                                       QSizePolicy::Preferred);
    chrome->changesOpen = textButton(
        QStringLiteral("workspaceOpenDiff"), QStringLiteral("Open diff"),
        QStringLiteral("Show these changes beside the conversation"));
    changesRow->addWidget(chrome->changesText, 1);
    changesRow->addWidget(chrome->changesOpen);
    changesOuter->addStretch(1);
    changesOuter->addWidget(changesCard, 100);
    changesOuter->addStretch(1);
    detailLayout->insertWidget(
        detailLayout->indexOf(chrome->outputPage) + 1, changesBar);
    changesBar->hide();
    chrome->changesBar = changesBar;

    auto *empty = new QWidget;
    empty->setObjectName(QStringLiteral("workspaceEmpty"));
    auto *emptyLayout = new QVBoxLayout(empty);
    emptyLayout->setContentsMargins(32, 32, 32, 32);
    emptyLayout->setSpacing(8);
    auto *emptyTitle = new QLabel(QStringLiteral("What should the agent work on?"));
    emptyTitle->setObjectName(QStringLiteral("workspaceEmptyTitle"));
    emptyTitle->setAlignment(Qt::AlignHCenter);
    chrome->emptyBody = new QLabel;
    chrome->emptyBody->setObjectName(QStringLiteral("workspaceEmptyBody"));
    chrome->emptyBody->setTextFormat(Qt::PlainText);
    chrome->emptyBody->setAlignment(Qt::AlignHCenter);
    chrome->emptyBody->setWordWrap(true);
    emptyLayout->addStretch(1);
    emptyLayout->addWidget(emptyTitle);
    emptyLayout->addWidget(chrome->emptyBody);
    emptyLayout->addStretch(1);
    detailLayout->addWidget(empty, 1);
    empty->hide();
    chrome->empty = empty;

    QObject::connect(chrome->status, &QPushButton::clicked, chrome,
                     command(CmdDetails));
    QObject::connect(chrome->stop, &QPushButton::clicked, chrome, [self] {
        if (self->m_agentStopButton)
            self->m_agentStopButton->click();
    });
    QObject::connect(chrome->resume, &QPushButton::clicked, chrome, [self] {
        if (self->m_agentStartButton)
            self->m_agentStartButton->click();
    });
    QObject::connect(chrome->toggles[TabChanges], &QPushButton::clicked, chrome,
                     command(CmdToggleChanges));
    QObject::connect(chrome->toggles[TabTerminal], &QPushButton::clicked,
                     chrome, command(CmdToggleTerminal));
    QObject::connect(chrome->toggles[TabBrowser], &QPushButton::clicked, chrome,
                     command(CmdToggleBrowser));
    QObject::connect(chrome->changesOpen, &QPushButton::clicked, chrome,
                     command(CmdToggleChanges));
    QObject::connect(chrome->more, &QPushButton::clicked, chrome,
                     [chrome, popMenu] {
                         popMenu(chrome->more, MenuSession, true);
                     });

    chrome->dockLayout = dockLayout;
    chrome->classicDockMargins = dockLayout->contentsMargins();
    chrome->classicDockSpacing = dockLayout->spacing();
    chrome->classicDockMinimum = window.m_workspaceTerminalDock->minimumWidth();
    for (QWidget *widget : layoutWidgets(dockLayout->itemAt(0)->layout()))
        chrome->classicDockHeader.append(widget);
    window.m_workspaceTerminalDock->setAttribute(Qt::WA_StyledBackground, true);

    auto *panelBar = new QWidget;
    panelBar->setObjectName(QStringLiteral("workspacePanelBar"));
    panelBar->setAttribute(Qt::WA_StyledBackground, true);
    panelBar->setFixedHeight(kHeaderHeight);
    auto *panelRow = new QHBoxLayout(panelBar);
    panelRow->setContentsMargins(16, 0, 12, 0);
    panelRow->setSpacing(6);
    const QString tabLabels[TabCount] = {QStringLiteral("Diff"),
                                         QStringLiteral("Terminal"),
                                         QStringLiteral("Browser")};
    const int tabCommands[TabCount] = {CmdShowChanges, CmdShowTerminal,
                                       CmdShowBrowser};
    for (int index = 0; index < TabCount; ++index) {
        auto *tab = textButton(QStringLiteral("workspacePanelTab"),
                               tabLabels[index], QString(), 30);
        tab->setCheckable(true);
        tab->setProperty("workspaceTab", index);
        tab->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        panelRow->addWidget(tab);
        chrome->tabs[index] = tab;
        QObject::connect(tab, &QPushButton::clicked, chrome,
                         command(tabCommands[index]));
    }
    chrome->panelBranch = new QLabel;
    chrome->panelBranch->setObjectName(QStringLiteral("workspacePanelBranch"));
    chrome->panelBranch->setTextFormat(Qt::PlainText);
    chrome->panelBranch->setSizePolicy(QSizePolicy::Ignored,
                                       QSizePolicy::Preferred);
    chrome->panelStats = new QLabel;
    chrome->panelStats->setObjectName(QStringLiteral("workspacePanelStats"));
    chrome->panelStats->setTextFormat(Qt::RichText);
    panelRow->addSpacing(6);
    panelRow->addWidget(chrome->panelBranch, 1);
    panelRow->addWidget(chrome->panelStats);
    chrome->panelNewShell = iconButton(
        QStringLiteral("workspacePanelNewShell"),
        QStringLiteral("New shell in the selected session's worktree (ends the "
                       "current one)"));
    chrome->panelClose = iconButton(QStringLiteral("workspacePanelClose"),
                                    QStringLiteral("Close the side panel"));
    panelRow->addWidget(chrome->panelNewShell);
    panelRow->addWidget(chrome->panelClose);
    QObject::connect(chrome->panelNewShell, &QPushButton::clicked, chrome,
                     command(CmdNewShell));
    QObject::connect(chrome->panelClose, &QPushButton::clicked, chrome,
                     command(CmdClosePanel));

    auto *stack = new QStackedWidget;
    stack->setObjectName(QStringLiteral("workspacePanelStack"));
    chrome->hosts[TabTerminal] = paneHost(QStringLiteral("workspaceTerminalHost"));
    chrome->hosts[TabChanges] = paneHost(QStringLiteral("workspaceChangesHost"));
    chrome->hosts[TabBrowser] = paneHost(QStringLiteral("workspaceBrowserHost"));
    stack->addWidget(chrome->hosts[TabTerminal]);
    stack->addWidget(chrome->hosts[TabChanges]);
    stack->addWidget(chrome->hosts[TabBrowser]);
    dockLayout->removeWidget(window.m_workspaceTerminal);
    window.m_workspaceTerminal->setParent(chrome->hosts[TabTerminal]);
    chrome->hosts[TabTerminal]->layout()->addWidget(window.m_workspaceTerminal);
    window.m_workspaceTerminal->show();
    dockLayout->insertWidget(0, panelBar);
    dockLayout->addWidget(stack, 1);
    panelBar->hide();
    chrome->panelBar = panelBar;
    chrome->panelStack = stack;
    if (window.m_agentFilesPanel)
        chrome->filesSplit = qobject_cast<QSplitter *>(
            window.m_agentFilesPanel->parentWidget());

    const QList<QAbstractButton *> watched = {
        window.m_agentStopButton,       window.m_agentStartButton,
        window.m_agentViewPrButton,     window.m_agentViewIssueButton,
        window.m_agentViewTaskButton,   window.m_agentCreatePrButton,
        window.m_agentBranchButton,     window.m_agentStatusPill,
        window.m_agentQueueButton};
    for (QAbstractButton *button : watched)
        if (button)
            button->installEventFilter(chrome);
    window.m_agentCodePage->installEventFilter(chrome);
    if (chrome->transcript)
        chrome->transcript->installEventFilter(chrome);
    QObject::connect(window.m_agentTable, &QTableWidget::itemSelectionChanged,
                     chrome, [chrome] { chrome->refresh.start(); });
    if (QAbstractItemModel *model = window.m_agentTable->model()) {
        const auto changed = [chrome] { chrome->refresh.start(); };
        QObject::connect(model, &QAbstractItemModel::rowsInserted, chrome,
                         changed);
        QObject::connect(model, &QAbstractItemModel::rowsRemoved, chrome,
                         changed);
        QObject::connect(model, &QAbstractItemModel::modelReset, chrome,
                         changed);
    }
    chrome->page = window.m_agentsPage;
    window.m_agentsPage->installEventFilter(chrome);
    namespace keys = forkmesh::workspaceshortcuts;
    for (const keys::Binding &binding : keys::bindings()) {
        if (!binding.bound)
            continue;
        int id = -1;
        switch (binding.action) {
        case keys::Action::NextSession:
        case keys::Action::NextSessionAlt: id = CmdNextSession; break;
        case keys::Action::PreviousSession:
        case keys::Action::PreviousSessionAlt: id = CmdPreviousSession; break;
        case keys::Action::ToggleDiff: id = CmdToggleChanges; break;
        case keys::Action::ToggleTerminal: id = CmdToggleTerminal; break;
        case keys::Action::ToggleBrowser: id = CmdToggleBrowser; break;
        case keys::Action::ClosePanel: id = CmdClosePanel; break;
        case keys::Action::ModeMenu: id = CmdModeMenu; break;
        case keys::Action::ModelMenu: id = CmdModelMenu; break;
        case keys::Action::EffortMenu: id = CmdEffortMenu; break;
        case keys::Action::ShowShortcuts: id = CmdShortcuts; break;
        default: break;
        }
        if (id < 0)
            continue;
        auto *shortcut = new QShortcut(binding.keys, &window);
        shortcut->setObjectName(QStringLiteral("workspaceShortcut"));
        shortcut->setContext(Qt::WindowShortcut);
        shortcut->setEnabled(false);
        QObject::connect(shortcut, &QShortcut::activated, chrome, command(id));
        chrome->shortcuts.append(shortcut);
    }
    chrome->tick.start();
    syncWorkspace(window);
}

void NextAccess::syncWorkspace(MainWindow &window)
{
    Chrome *chrome = chromeOf(window);
    if (!chrome || chrome->syncing)
        return;
    chrome->syncing = true;
    const auto release = qScopeGuard([chrome] { chrome->syncing = false; });
    const bool next = workspaceLayoutNew();
    const bool dark = Theme::activeDark();
    QSplitter *split = window.m_agentWorkspaceSplit;
    QWidget *dock = window.m_workspaceTerminalDock;
    QTableWidget *table = window.m_agentTable;
    const bool workspace = window.m_viewMode == AppViewMode::Workspace;

    if (!chrome->composerReady && window.m_promptWrapper &&
        window.m_quickAddSendButton && window.m_promptDragHandle) {
        auto *stack = qobject_cast<QVBoxLayout *>(window.m_promptWrapper->layout());
        auto *row = stack && stack->count() > 1
                        ? qobject_cast<QHBoxLayout *>(stack->itemAt(1)->layout())
                        : nullptr;
        auto *column = row && row->count() > 1
                           ? qobject_cast<QVBoxLayout *>(row->itemAt(1)->layout())
                           : nullptr;
        auto *leftColumn =
            row && row->count() > 1
                ? qobject_cast<QVBoxLayout *>(row->itemAt(0)->layout())
                : nullptr;
        auto *barScroll = window.m_promptWrapper->findChild<QScrollArea *>(
            QStringLiteral("quickAddBottomBarScroll"));
        if (column && column->indexOf(window.m_quickAddSendButton) >= 0 &&
            leftColumn && barScroll && leftColumn->indexOf(barScroll) >= 0) {
            MainWindow *self = &window;
            chrome->composerRow = row;
            chrome->sendColumn = column;
            if (window.m_issueQuickAdd)
                chrome->classicEditFill =
                    window.m_issueQuickAdd->viewport()->autoFillBackground();
            chrome->composerMenu = iconButton(
                QStringLiteral("workspaceComposerMenu"),
                QStringLiteral("Prompt options: worktree, cloud, new session, "
                               "task"));
            chrome->composerSend = new QPushButton;
            chrome->composerSend->setObjectName(QStringLiteral("workspaceSend"));
            chrome->composerSend->setCursor(Qt::PointingHandCursor);
            chrome->composerSend->setFocusPolicy(Qt::NoFocus);
            chrome->composerSend->setFixedSize(34, 34);
            chrome->composerSend->setIconSize(QSize(16, 16));
            chrome->composerSend->setToolTip(QStringLiteral("Send (Enter)"));
            chrome->composerSend->setAccessibleName(QStringLiteral("Send"));
            leftColumn->removeWidget(barScroll);
            auto *barRow = new QHBoxLayout;
            barRow->setContentsMargins(0, 0, 0, 0);
            barRow->setSpacing(0);
            barRow->addWidget(barScroll, 1, Qt::AlignBottom);
            barRow->addWidget(chrome->composerMenu, 0, Qt::AlignBottom);
            barRow->addWidget(chrome->composerSend, 0, Qt::AlignBottom);
            leftColumn->addLayout(barRow);
            chrome->composerBarRow = barRow;
            chrome->composerMenu->hide();
            chrome->composerSend->hide();
            QObject::connect(chrome->composerSend, &QPushButton::clicked, chrome,
                             [self] {
                                 NextAccess::workspaceCommand(*self, CmdSend);
                             });
            QObject::connect(chrome->composerMenu, &QPushButton::clicked, chrome,
                             [self, chrome] {
                                 QMenu menu(self);
                                 menu.setToolTipsVisible(true);
                                 NextAccess::fillWorkspaceMenu(*self, &menu,
                                                               MenuComposer);
                                 const QSize hint = menu.sizeHint();
                                 menu.exec(chrome->composerMenu->mapToGlobal(
                                     QPoint(chrome->composerMenu->width() -
                                                hint.width(),
                                            -hint.height() - 6)));
                             });
            chrome->composerReady = true;
            chrome->iconsReady = false;
            QTimer::singleShot(0, chrome, [self] {
                self->positionGlobalFooterOverlays();
            });
        }
    }

    if (!chrome->iconsReady || chrome->lastDark != dark) {
        chrome->lastDark = dark;
        refreshIcons(chrome);
    }

    if (next != chrome->applied) {
        chrome->applied = next;
        setFlag(chrome->listPane, "workspaceNext", next);
        setFlag(window.m_agentDetail, "workspaceNext", next);
        setFlag(dock, "workspaceNext", next);
        setFlag(table, "workspaceCards", next);
        setFlag(split, "workspaceNext", next);
        if (chrome->toolbar)
            chrome->toolbar->setVisible(!next);
        chrome->sidebarHeader->setVisible(next);
        chrome->sidebarFooter->setVisible(next);
        chrome->enableShortcuts();

        if (next) {
            if (chrome->statusRow->parent() == chrome->topRow) {
                const QList<QWidget *> parked = layoutWidgets(chrome->statusRow);
                chrome->topRow->removeItem(chrome->statusRow);
                for (QWidget *widget : parked)
                    widget->move(kParkedAt, kParkedAt);
            }
        } else if (!chrome->statusRow->parent()) {
            chrome->topRow->insertLayout(
                qMin(chrome->statusIndex, chrome->topRow->count()), chrome->statusRow);
        }
        if (chrome->titleBlock)
            chrome->titleBlock->setVisible(!next);
        chrome->header->setVisible(next);
        const int bottom = chrome->detailLayout->contentsMargins().bottom() -
                           window.m_agentDetailBaseBottomMargin;
        window.m_agentDetailBaseBottomMargin =
            next ? 0 : chrome->classicBaseBottom;
        const QMargins base =
            next ? QMargins(0, 0, 0, 0) : chrome->classicDetailMargins;
        chrome->detailLayout->setContentsMargins(
            base.left(), base.top(), base.right(),
            window.m_agentDetailBaseBottomMargin + qMax(0, bottom));
        chrome->detailLayout->setSpacing(next ? 0
                                              : chrome->classicDetailSpacing);
        if (chrome->outputPage && chrome->outputPage->layout())
            chrome->outputPage->layout()->setContentsMargins(
                next ? QMargins(0, 0, 0, 0) : chrome->classicOutputMargins);
        if (!next) {
            chrome->empty->hide();
            chrome->changesBar->hide();
            if (chrome->outputPage)
                chrome->outputPage->show();
        }

        chrome->dockLayout->setContentsMargins(
            next ? QMargins(0, 0, 0, 0) : chrome->classicDockMargins);
        chrome->dockLayout->setSpacing(next ? 0 : chrome->classicDockSpacing);
        dock->setMinimumWidth(next ? 320 : chrome->classicDockMinimum);
        for (const QPointer<QWidget> &widget : chrome->classicDockHeader)
            if (widget)
                widget->setVisible(!next);
        chrome->panelBar->setVisible(next);
        if (chrome->hosts[TabTerminal]->layout())
            chrome->hosts[TabTerminal]->layout()->setContentsMargins(
                next ? QMargins(14, 12, 10, 12) : QMargins(0, 0, 0, 0));
        if (window.m_agentFilesPanel)
            window.m_agentFilesPanel->setVisible(!next);
        if (next) {
            moveInto(window.m_agentCodePage, chrome->hosts[TabChanges]);
            if (chrome->hosts[TabChanges]->layout())
                chrome->hosts[TabChanges]->layout()->setContentsMargins(
                    14, 12, 10, 10);
            split->setHandleWidth(7);
            split->setStretchFactor(0, 0);
            split->setStretchFactor(1, 1);
            chrome->panelOpen = !dock->isHidden();
            if (window.m_agentDetail->isHidden())
                window.m_agentDetail->show();
            chrome->needsSizing = true;
            chrome->classicListView =
                QSettings().value(QLatin1String(kListViewSetting)).toString();
        } else {
            window.setAgentListView(
                chrome->classicListView == QLatin1String("zen")
                    ? MainWindow::AgentListView::Zen
                    : chrome->classicListView == QLatin1String("full")
                          ? MainWindow::AgentListView::Full
                          : MainWindow::AgentListView::Compact);
            if (window.m_agentCodePage->parentWidget() != window.m_agentCodeSplit) {
                const bool shown = !window.m_agentCodePage->isHidden();
                window.m_agentCodeSplit->addWidget(window.m_agentCodePage);
                window.m_agentCodePage->setVisible(shown);
            }
            chrome->panelStack->setCurrentWidget(chrome->hosts[TabTerminal]);
            split->setHandleWidth(chrome->classicHandleWidth);
            split->setStretchFactor(0, 1);
            split->setStretchFactor(1, 1);
            dock->setVisible(workspace);
            chrome->needsSizing = false;
            if (split->isVisible() && split->width() > 0) {
                const int pane = dock->isHidden() ? 0 : split->sizes().value(2);
                const int rest = qMax(0, split->width() - pane);
                split->setSizes({rest / 2, rest - rest / 2, pane});
            }
        }
        fitColumn(chrome);
        window.positionGlobalFooterOverlays();
        window.refreshAgentTranscriptTheme(dark);
        window.updateQuickAddTargetAgentLabel();
    }

    if (!next) {
        chrome->lastMode = window.m_viewMode;
        return;
    }

    if (chrome->composerSend) {
        const bool chat =
            window.m_quickAddSendToAgentButton &&
            window.m_quickAddSendToAgentButton->property("enterTarget").toBool();
        const QString label = chat ? QStringLiteral("Add") : QString();
        if (chrome->composerSend->text() != label) {
            chrome->composerSend->setText(label);
            chrome->composerSend->setFixedSize(chat ? 78 : 34, 34);
            chrome->composerSend->setToolTip(
                chat ? QStringLiteral("Add to the agent open above (Enter)")
                     : QStringLiteral("Send (Enter)"));
            chrome->composerSend->setAccessibleName(
                chat ? QStringLiteral("Add to agent") : QStringLiteral("Send"));
            setFlag(chrome->composerSend, "workspaceChat", chat);
            chrome->composerSend->setIcon(
                glyph(QStringLiteral("arrow-up"),
                      chat ? Theme::Role::OnAccent : Theme::Role::Fg, chat ? 14 : 16));
        }
    }

    if (window.m_agentListView != MainWindow::AgentListView::Full) {
        window.setAgentListView(MainWindow::AgentListView::Full);
        QSettings().setValue(QLatin1String(kListViewSetting),
                             chrome->classicListView);
    }
    applyRowHeight(table, kRowHeight);

    if (chrome->lastMode != window.m_viewMode) {
        const bool leaving = chrome->lastMode == AppViewMode::Workspace;
        chrome->lastMode = window.m_viewMode;
        if (workspace) {
            chrome->panelOpen = true;
            if (chrome->tab == TabChanges && !window.m_agentCodeMode)
                chrome->tab = TabTerminal;
        } else if (leaving) {
            chrome->panelOpen = window.m_agentCodeMode;
        }
    }
    if (workspace && !dock->isHidden())
        chrome->panelOpen = true;
    if (window.m_agentCodeMode) {
        chrome->panelOpen = true;
        chrome->tab = TabChanges;
    } else if (chrome->panelOpen && chrome->tab == TabChanges) {
        if (workspace)
            chrome->tab = TabTerminal;
        else
            chrome->panelOpen = false;
    }

    const bool wasHidden = dock->isHidden();
    dock->setVisible(chrome->panelOpen);
    chrome->panelStack->setCurrentWidget(chrome->hosts[chrome->tab]);
    const bool diffOpen = chrome->panelOpen && chrome->tab == TabChanges;
    for (int index = 0; index < TabCount; ++index) {
        chrome->tabs[index]->setChecked(chrome->tab == index);
        chrome->toggles[index]->setChecked(chrome->panelOpen &&
                                           chrome->tab == index);
    }
    const QString diffLabel =
        diffOpen ? QStringLiteral("Hide diff") : QStringLiteral("Show diff");
    if (chrome->toggles[TabChanges]->text() != diffLabel)
        chrome->toggles[TabChanges]->setText(diffLabel);
    chrome->panelNewShell->setVisible(chrome->tab == TabTerminal);
    if (chrome->needsSizing && split->isVisible() && split->width() >= 600) {
        chrome->needsSizing = false;
        const int total = split->width();
        const int pane =
            dock->isHidden() ? 0 : qBound(kPanelMinWidth, total * 42 / 100, 820);
        split->setSizes(
            {kSidebarWidth, qMax(240, total - kSidebarWidth - pane), pane});
    } else if (chrome->panelOpen && wasHidden) {
        QList<int> sizes = split->sizes();
        if (sizes.size() == 3 && sizes.at(2) < kPanelMinWidth) {
            const int total = qMax(split->width(), 960);
            const int pane = qBound(kPanelMinWidth, total * 42 / 100, 820);
            const int sidebar = qMax(260, qMin(sizes.at(0), kSidebarWidth));
            split->setSizes({sidebar, qMax(0, total - sidebar - pane), pane});
        }
    }
    if (chrome->panelOpen && wasHidden && chrome->tab == TabTerminal &&
        window.m_workspaceTerminal && !window.m_workspaceTerminal->isRunning())
        window.startWorkspaceTerminal();

    QString repository;
    QString repositoryName;
    if (window.m_repoDetailIndex >= 0 &&
        window.m_repoDetailIndex < window.m_repositories.size()) {
        const auto &record = window.m_repositories.at(window.m_repoDetailIndex);
        repository = QStringLiteral("%1/%2").arg(record.owner, record.name);
        repositoryName = record.name;
    }
    const QString brand =
        repositoryName.isEmpty() ? QStringLiteral("Sessions") : repositoryName;
    if (chrome->sidebarBrand->text() != brand ||
        chrome->brandLetter != brand.left(1)) {
        chrome->sidebarBrand->setText(brand);
        chrome->sidebarBrand->setToolTip(repository);
        chrome->brandLetter = brand.left(1);
        chrome->sidebarBadge->setPixmap(
            badgePixmap(brand, Theme::color(Theme::Role::Accent), 22));
    }

    const AgentSession *session =
        window.findAgentSession(window.m_selectedAgentSessionId);
    chrome->title->setParts(session ? repositoryName : QString(),
                            session ? sessionTitle(*session)
                                    : QStringLiteral("New session"));
    chrome->status->setVisible(session != nullptr &&
                               chrome->header->width() >= kHeaderStatusFloor);
    ChangeStats stats;
    if (session) {
        const StatusInfo info = statusInfo(*session);
        const QString model = agentModelShortLabel(session->model);
        const QString text = model.isEmpty()
                                 ? info.text
                                 : QStringLiteral("%1 · %2").arg(model, info.text);
        if (chrome->status->text() != text ||
            chrome->status->property("workspaceTone").toInt() !=
                int(info.tone)) {
            chrome->status->setText(text);
            chrome->status->setProperty("workspaceTone", int(info.tone));
            chrome->status->setIcon(QIcon(paintedGlyph(
                QStringLiteral("dot"), Theme::color(info.tone), 10)));
        }
        stats.branch = session->branchName;
        for (int row = 0; row < table->rowCount(); ++row) {
            const QTableWidgetItem *lead = table->item(row, kAgentIdColumn);
            if (!lead || lead->data(Qt::UserRole).toInt() != session->id)
                continue;
            stats.files = qMax(0, lead->data(kAgentBranchFilesRole).toInt());
            stats.added = qMax(0, lead->data(kAgentAddedRole).toInt());
            stats.removed = qMax(0, lead->data(kAgentRemovedRole).toInt());
            break;
        }
    }
    chrome->stop->setVisible(session && window.m_agentStopButton &&
                             window.m_agentStopButton->isEnabled());
    chrome->resume->setVisible(session && window.m_agentStartButton &&
                               !window.m_agentStartButton->isHidden());
    chrome->more->setEnabled(session != nullptr);
    chrome->toggles[TabChanges]->setEnabled(session != nullptr);
    chrome->tabs[TabChanges]->setEnabled(session != nullptr);
    chrome->empty->setVisible(!session);
    if (chrome->outputPage)
        chrome->outputPage->setVisible(session != nullptr);
    if (!session) {
        chrome->emptyBody->setText(
            repository.isEmpty()
                ? QStringLiteral("Type a prompt below to start a session, or "
                                 "pick one from the list.")
                : QStringLiteral("Type a prompt below to start a session in "
                                 "%1, or pick one from the list.")
                      .arg(repository));
    }

    const bool showCard = session && stats.any() && !diffOpen;
    chrome->changesBar->setVisible(showCard);
    if (showCard) {
        const QString html =
            QStringLiteral("<span style='font-weight:600;color:%1'>%2 changed "
                           "file%3</span>&nbsp;&nbsp;&nbsp;%4"
                           "&nbsp;&nbsp;&nbsp;<span style='color:%5'>%6</span>")
                .arg(Theme::hex(Theme::Role::Fg))
                .arg(stats.files)
                .arg(stats.files == 1 ? QString() : QStringLiteral("s"))
                .arg(statsHtml(stats), Theme::hex(Theme::Role::FgSubtle),
                     stats.branch.toHtmlEscaped());
        if (chrome->changesHtml != html) {
            chrome->changesHtml = html;
            chrome->changesText->setText(html);
        }
    }
    const bool onDiff = chrome->tab == TabChanges;
    const QString branchText = onDiff ? stats.branch : QString();
    if (chrome->panelBranch->text() != branchText)
        chrome->panelBranch->setText(branchText);
    chrome->panelStats->setVisible(onDiff && stats.any());
    if (onDiff && stats.any())
        chrome->panelStats->setText(statsHtml(stats));

    const int rows = table->rowCount();
    chrome->sidebarCount->setText(rows > 0 ? QString::number(rows) : QString());
    const QString queue =
        window.m_agentQueueButton ? window.m_agentQueueButton->text() : QString();
    const QStringList parts = queue.split(QLatin1Char('/'));
    const QString running =
        parts.size() == 2
            ? QStringLiteral("%1 of %2 agents running")
                  .arg(parts.at(0).trimmed(), parts.at(1).trimmed())
            : queue;
    chrome->sidebarQueue->setVisible(!running.isEmpty());
    if (chrome->sidebarQueue->text() != running)
        chrome->sidebarQueue->setText(running);
    fitColumn(chrome);
}

void NextAccess::workspaceCommand(MainWindow &window, int command)
{
    Chrome *chrome = chromeOf(window);
    if (!chrome || !chrome->applied)
        return;
    const auto openTab = [&window, chrome](int tab, bool toggle) {
        if (toggle && chrome->panelOpen && chrome->tab == tab) {
            chrome->panelOpen = false;
            window.m_workspaceTerminalDock->hide();
            if (window.m_agentCodeMode)
                window.setAgentCodeMode(false);
            syncWorkspace(window);
            return;
        }
        chrome->panelOpen = true;
        chrome->tab = tab;
        if (tab != TabChanges && window.m_agentCodeMode)
            window.setAgentCodeMode(false);
        if (tab == TabBrowser && !chrome->browser) {
            chrome->browser = new WorkspaceBrowserPane;
            chrome->browser->setInstallHandler(
                [w = &window] { w->installEmbeddedBrowser(); });
            chrome->hosts[TabBrowser]->layout()->addWidget(chrome->browser);
            chrome->iconsReady = false;
        }
        if (tab == TabChanges) {
            window.m_workspaceTerminalDock->show();
            chrome->panelStack->setCurrentWidget(chrome->hosts[TabChanges]);
            if (!window.m_agentCodeMode)
                window.setAgentCodeMode(true);
            else
                window.refreshAgentFilesPanel(window.m_selectedAgentSessionId);
        }
        syncWorkspace(window);
        if (tab == TabTerminal && window.m_workspaceTerminal) {
            if (!window.m_workspaceTerminal->isRunning())
                window.startWorkspaceTerminal();
            window.m_workspaceTerminal->setFocus(Qt::OtherFocusReason);
        }
        if (tab == TabBrowser && chrome->browser &&
            !chrome->browser->currentUrl().isValid())
            chrome->browser->focusAddress();
    };

    switch (command) {
    case CmdNewSession: {
        {
            const QSignalBlocker blocker(window.m_agentTable);
            window.m_agentTable->clearSelection();
            window.m_agentTable->setCurrentItem(nullptr);
        }
        window.showAgentSession(-1);
        if (window.m_promptOverlayCollapsed)
            window.setPromptOverlayCollapsed(false);
        if (window.m_issueQuickAdd)
            window.m_issueQuickAdd->setFocus(Qt::OtherFocusReason);
        syncWorkspace(window);
        break;
    }
    case CmdToggleChanges:
        openTab(TabChanges, true);
        break;
    case CmdToggleTerminal:
        openTab(TabTerminal, true);
        break;
    case CmdToggleBrowser:
        openTab(TabBrowser, true);
        break;
    case CmdShowChanges:
        openTab(TabChanges, false);
        break;
    case CmdShowTerminal:
        openTab(TabTerminal, false);
        break;
    case CmdShowBrowser:
        openTab(TabBrowser, false);
        break;
    case CmdClosePanel:
        chrome->panelOpen = false;
        window.m_workspaceTerminalDock->hide();
        if (window.m_agentCodeMode)
            window.setAgentCodeMode(false);
        syncWorkspace(window);
        break;
    case CmdNewShell:
        window.startWorkspaceTerminal();
        break;
    case CmdDetails: {
        QFrame *popup = window.m_agentMetaPopup;
        if (!popup)
            break;
        if (popup->isVisible()) {
            popup->hide();
            break;
        }
        popup->adjustSize();
        popup->move(chrome->status->mapToGlobal(
            QPoint(0, chrome->status->height() + 6)));
        popup->show();
        break;
    }
    case CmdQueue: {
        window.showAgentQueueLimitMenu();
        const QList<QMenu *> menus = window.findChildren<QMenu *>(
            QStringLiteral("agentQueueLimitMenu"));
        if (!menus.isEmpty()) {
            QMenu *menu = menus.last();
            menu->move(chrome->sidebarQueue->mapToGlobal(
                QPoint(0, -menu->sizeHint().height() - 4)));
        }
        break;
    }
    case CmdNextSession:
    case CmdPreviousSession: {
        QTableWidget *table = window.m_agentTable;
        const int rows = table->rowCount();
        const int step = command == CmdNextSession ? 1 : -1;
        int row = table->currentRow();
        for (int tries = 0; tries < rows; ++tries) {
            row = row < 0 ? (step > 0 ? 0 : rows - 1) : (row + step + rows) % rows;
            if (!table->isRowHidden(row))
                break;
        }
        if (row < 0 || row >= rows || table->isRowHidden(row) ||
            row == table->currentRow())
            break;
        table->setCurrentCell(row, kAgentIdColumn);
        if (QTableWidgetItem *item = table->item(row, kAgentIdColumn))
            table->scrollToItem(item);
        break;
    }
    case CmdModeMenu:
    case CmdModelMenu:
    case CmdEffortMenu: {
        QComboBox *picker = command == CmdModeMenu    ? window.m_quickAddModeSelector
                            : command == CmdModelMenu ? window.m_quickAddAgentModelSelector
                                                      : window.m_quickAddSpeedSelector;
        if (!picker || !picker->isVisible() || !picker->isEnabled())
            break;
        picker->setFocus(Qt::ShortcutFocusReason);
        picker->showPopup();
        break;
    }
    case CmdShortcuts:
        showWorkspaceShortcuts(&window);
        break;
    case CmdSend: {
        QPushButton *target =
            !window.promptChatRoom().isEmpty() && window.m_quickAddSendToChannelButton
                ? window.m_quickAddSendToChannelButton
            : window.m_quickAddSendToAgentButton &&
                    window.m_quickAddSendToAgentButton->property("enterTarget")
                        .toBool()
                ? window.m_quickAddSendToAgentButton
                : window.m_quickAddSendButton;
        if (target)
            target->click();
        break;
    }
    default:
        break;
    }
}

void NextAccess::fillWorkspaceMenu(MainWindow &window, QMenu *menu, int which)
{
    if (!menu)
        return;
    MainWindow *self = &window;
    if (which == MenuSidebar) {
        QAction *fresh =
            menu->addAction(glyph(QStringLiteral("plus"), Theme::Role::FgMuted),
                            QStringLiteral("New session"));
        QObject::connect(fresh, &QAction::triggered, self, [self] {
            NextAccess::workspaceCommand(*self, CmdNewSession);
        });
        menu->addSeparator();
        addProxy(menu, window.m_agentStartAllButton, QStringLiteral("rocket"));
        addProxy(menu, window.m_agentStopAllButton,
                 QStringLiteral("circle-slash"));
        addProxy(menu, window.m_agentUpdateAllButton, QStringLiteral("sync"));
        addProxy(menu, window.m_agentMoveToDesktopButton,
                 QStringLiteral("device-desktop"),
                 QStringLiteral("Move cloud runs to this desktop"));
        menu->addSeparator();
        addProxy(menu, window.m_agentDeleteSelectedButton,
                 QStringLiteral("trash"));
        addProxy(menu, window.m_agentDeleteMergedButton, QStringLiteral("trash"),
                 QStringLiteral("Delete merged"));
        menu->addSeparator();

        QMenu *filter =
            menu->addMenu(glyph(QStringLiteral("people"), Theme::Role::FgMuted),
                          QStringLiteral("Teammate sessions"));
        auto *filterGroup = new QActionGroup(filter);
        const struct {
            const char *label;
            MainWindow::AgentTeammateFilter value;
        } filters[] = {
            {"Hide teammate sessions",
             MainWindow::AgentTeammateFilter::WithoutTeammates},
            {"Only teammate sessions",
             MainWindow::AgentTeammateFilter::TeammatesOnly},
            {"All sessions", MainWindow::AgentTeammateFilter::All}};
        for (const auto &entry : filters) {
            QAction *action = filter->addAction(QString::fromLatin1(entry.label));
            action->setCheckable(true);
            action->setChecked(window.m_agentTeammateFilter == entry.value);
            filterGroup->addAction(action);
            const MainWindow::AgentTeammateFilter value = entry.value;
            QObject::connect(action, &QAction::triggered, self, [self, value] {
                self->setAgentTeammateFilter(value);
            });
        }
        if (window.m_agentSortButton && window.m_agentSortButton->menu()) {
            QAction *sort = menu->addMenu(window.m_agentSortButton->menu());
            sort->setText(QStringLiteral("Sort by"));
            sort->setIcon(glyph(QStringLiteral("sort-desc"), Theme::Role::FgMuted));
        }
        QMenu *layout = menu->addMenu(
            glyph(QStringLiteral("list-unordered"), Theme::Role::FgMuted),
            QStringLiteral("Layout"));
        auto *layoutGroup = new QActionGroup(layout);
        for (const bool classic : {false, true}) {
            QAction *action = layout->addAction(
                classic ? QStringLiteral("Classic") : QStringLiteral("New"));
            action->setCheckable(true);
            action->setChecked(classic == workspaceLayoutClassic());
            layoutGroup->addAction(action);
            QObject::connect(action, &QAction::triggered, self, [self, classic] {
                if (workspaceLayoutClassic() == classic)
                    return;
                QTimer::singleShot(0, self, [self, classic] {
                    setWorkspaceLayoutClassic(classic);
                    NextAccess::syncWorkspace(*self);
                });
            });
        }
        menu->addSeparator();
        addProxy(menu, window.m_agentEfficiencyButton, QStringLiteral("graph"));
        addProxy(menu, window.m_agentGuardrailsButton,
                 QStringLiteral("shield-check"));
        addProxy(menu, window.m_agentModelsButton, QStringLiteral("smart-toy"));
        addProxy(menu, window.m_agentAnnouncerButton,
                 QStringLiteral("broadcast"));
        QAction *keysAction = menu->addAction(
            glyph(QStringLiteral("list-unordered"), Theme::Role::FgMuted),
            QString::fromUtf8("Keyboard shortcuts\xE2\x80\xA6"));
        keysAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Slash));
        keysAction->setShortcutVisibleInContextMenu(true);
        QObject::connect(keysAction, &QAction::triggered, self, [self] {
            QTimer::singleShot(0, self, [self] { showWorkspaceShortcuts(self); });
        });
        menu->addSeparator();
        if (window.m_agentUsageButton) {
            QAction *usage = menu->addAction(
                glyph(QStringLiteral("terminal"), Theme::Role::FgMuted),
                QString::fromUtf8("Usage and terminals\xE2\x80\xA6"));
            usage->setToolTip(window.m_agentUsageButton->toolTip());
            QObject::connect(usage, &QAction::triggered, self, [self] {
                QTimer::singleShot(0, self, [self] {
                    self->showAgentUsageMenu(QCursor::pos());
                });
            });
        }
        return;
    }

    if (which == MenuComposer) {
        addProxy(menu, window.m_promptWorktreeCheck, QString(),
                 QStringLiteral("Own branch and worktree for new runs"), true);
        addProxy(menu, window.m_promptCloudButton, QString(),
                 QStringLiteral("Run new sessions in the cloud"), true);
        addProxy(menu, window.m_promptVibeCheck, QString(),
                 QStringLiteral("Merge and push when a new run finishes (vibe)"),
                 true);
        menu->addSeparator();
        addProxy(menu, window.m_quickAddSendButton,
                 QStringLiteral("paper-airplane"),
                 QStringLiteral("Start a new session with this prompt"), true);
        addProxy(menu, window.m_quickAddSendToChannelButton,
                 QStringLiteral("comment"),
                 window.m_quickAddSendToChannelButton
                     ? window.m_quickAddSendToChannelButton->accessibleName()
                     : QString(),
                 true);
        menu->addSeparator();
        addProxy(menu, window.m_promptClearButton, QStringLiteral("x"),
                 QStringLiteral("Clear the prompt"), true);
        return;
    }

    const AgentSession *session =
        window.findAgentSession(window.m_selectedAgentSessionId);
    if (!session)
        return;
    const bool branched = !session->branchName.isEmpty();
    addProxy(menu, window.m_agentStartButton, QStringLiteral("play"));
    if (window.m_agentStopButton && window.m_agentStopButton->isEnabled())
        addProxy(menu, window.m_agentStopButton, QStringLiteral("stop"));
    menu->addSeparator();
    addProxy(menu, window.m_agentViewTaskButton, QStringLiteral("checklist"));
    addProxy(menu, window.m_agentViewIssueButton,
             QStringLiteral("issue-opened"));
    addProxy(menu, window.m_agentCreateIssueButton,
             QStringLiteral("issue-opened"), QStringLiteral("Create issue"));
    addProxy(menu, window.m_agentViewPrButton,
             QStringLiteral("git-pull-request"));
    addProxy(menu, window.m_agentCreatePrButton,
             QStringLiteral("git-pull-request"),
             QStringLiteral("Create pull request"));
    menu->addSeparator();
    addProxy(menu, window.m_agentBranchButton, QStringLiteral("git-branch"),
             QStringLiteral("Open branch in Git"));
    addProxy(menu, window.m_agentWorktreeButton, QStringLiteral("worktree"),
             QStringLiteral("Open worktree"));
    QString folder = window.m_streamWorktree.value(session->id);
    if (folder.isEmpty()) {
        const int repoIndex = window.repoIndexFor(session->owner, session->name);
        if (repoIndex >= 0) {
            const QString repoLocal = window.m_repositories.at(repoIndex).localPath;
            if (branched)
                folder = window.cachedSessionWorktreeForUi(session->id, repoLocal,
                                                           session->branchName);
            if (folder.isEmpty() || !QDir(folder).exists())
                folder = repoLocal;
        }
    }
    addOpenInMenu(menu, self, folder, session->branchName);
    if (branched) {
        addProxy(menu, window.m_agentUpdateButton, QStringLiteral("sync"),
                 QStringLiteral("Update from the default branch"));
        addProxy(menu, window.m_agentMergeButton, QStringLiteral("git-merge"));
        addProxy(menu, window.m_agentMergeDeleteButton,
                 QStringLiteral("git-merge"));
    }
    menu->addSeparator();
    addProxy(menu, window.m_agentMoveToCloudButton, QStringLiteral("cloud"));
    if (window.m_agentMoveToCloudButton &&
        !window.m_agentMoveToCloudButton->isHidden()) {
        const int sessionId = window.m_selectedAgentSessionId;
        const bool movable = window.m_agentMoveToCloudButton->isEnabled();
        QMenu *toDesktop = menu->addMenu(
            glyph(QStringLiteral("device-desktop"), Theme::Role::FgMuted),
            QStringLiteral("Send to another desktop"));
        toDesktop->setObjectName(QStringLiteral("agentSendToDesktopMenu"));
        toDesktop->setToolTipsVisible(true);
        toDesktop->menuAction()->setEnabled(movable);
        toDesktop->menuAction()->setToolTip(
            movable ? QStringLiteral("Have one of this account's other "
                                     "desktops continue this session")
                    : window.m_agentMoveToCloudButton->toolTip());
        QObject::connect(toDesktop, &QMenu::aboutToShow, self,
                         [self, toDesktop, sessionId] {
            self->fillAgentHandoffDesktopMenu(toDesktop, sessionId);
        });
    }
    addProxy(menu, window.m_agentContinueHereButton,
             QStringLiteral("device-desktop"));
    addProxy(menu, window.m_agentRetryInCloudButton, QStringLiteral("sync"));
    addProxy(menu, window.m_agentSignInRunnerButton, QStringLiteral("key"));
    menu->addSeparator();
    addProxy(menu, window.m_agentOutputModeButton, QStringLiteral("terminal"),
             window.m_agentOutputModeButton &&
                     window.m_agentOutputModeButton->text() ==
                         QLatin1String("Raw")
                 ? QStringLiteral("Show raw output")
                 : QStringLiteral("Show transcript"));
    addProxy(menu, window.m_agentListenButton, QStringLiteral("unmute"));
    QAction *details =
        menu->addAction(glyph(QStringLiteral("info"), Theme::Role::FgMuted),
                        QStringLiteral("Session details"));
    QObject::connect(details, &QAction::triggered, self, [self] {
        QTimer::singleShot(0, self, [self] {
            NextAccess::workspaceCommand(*self, CmdDetails);
        });
    });
    menu->addSeparator();
    addProxy(menu, window.m_agentDeleteAllButton, QStringLiteral("trash"),
             QStringLiteral("Delete session"));
}

void NextAccess::placeWorkspaceComposer(MainWindow &window, bool overDetail)
{
    if (!window.m_footerDock || !window.m_promptOverlayHost)
        return;
    auto *row = qobject_cast<QHBoxLayout *>(window.m_footerDock->layout());
    if (!row)
        return;
    Chrome *chrome = chromeOf(window);
    const bool centred = chrome && chrome->applied && chrome->composerReady &&
                         overDetail && !window.m_promptOverlayCollapsed &&
                         !window.m_promptOverlayFloating &&
                         !window.m_promptOverlayDetached;
    if (chrome && chrome->composerReady && centred != chrome->composerDocked) {
        chrome->composerDocked = centred;
        if (centred) {
            if (chrome->sendColumn->parent() == chrome->composerRow) {
                const QList<QWidget *> parked =
                    layoutWidgets(chrome->sendColumn);
                chrome->composerRow->removeItem(chrome->sendColumn);
                for (QWidget *widget : parked)
                    widget->move(kParkedAt, kParkedAt);
            }
            chrome->composerBarRow->setContentsMargins(0, 0, 10, 8);
            chrome->composerBarRow->setSpacing(4);
        } else {
            if (!chrome->sendColumn->parent())
                chrome->composerRow->addLayout(chrome->sendColumn, 0);
            chrome->composerBarRow->setContentsMargins(0, 0, 0, 0);
            chrome->composerBarRow->setSpacing(0);
        }
        chrome->composerMenu->setVisible(centred);
        chrome->composerSend->setVisible(centred);
        window.m_promptDragHandle->setVisible(!centred);
        setFlag(window.m_promptWrapper, "workspaceDocked", centred);
        for (QWidget *child : window.m_promptWrapper->findChildren<QWidget *>())
            repolish(child);
        if (window.m_issueQuickAdd)
            window.m_issueQuickAdd->viewport()->setAutoFillBackground(
                centred ? false : chrome->classicEditFill);
    }
    const int preview = window.m_promptChatPreview &&
                                !window.m_promptChatPreview->isHidden()
                            ? window.m_promptChatPreview->sizeHint().height()
                            : 0;
    const int host =
        centred ? kComposerHeight + preview : window.m_promptAnchoredHeight;
    const int height = host + 8 + (centred ? kComposerBottomPad : 0);
    if (window.m_footerDock->minimumHeight() != height ||
        window.m_footerDock->maximumHeight() != height)
        window.m_footerDock->setFixedHeight(height);
    if (!centred)
        return;
    window.m_promptOverlayHost->setFixedHeight(host);
    if (window.m_issueQuickAdd)
        window.m_issueQuickAdd->setMaximumHeight(QWIDGETSIZE_MAX);
    const int available = window.m_agentDetail ? window.m_agentDetail->width() : 0;
    const int side = qMax(kComposerSidePad, (available - kComposerWidth) / 2);
    row->setContentsMargins(side, 8, side, kComposerBottomPad);
    row->invalidate();
}

}
