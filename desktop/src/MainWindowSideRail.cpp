#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "RepoWorkflow.h"
#include "RepoWorkflowRunner.h"
#include "SideRail.h"
#include "TeammateStore.h"
#include "WorkspaceBrowserPane.h"

#include <QAbstractButton>
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <functional>

using namespace forkmesh::ui;
namespace rw = forkmesh::repoworkflows;
namespace sr = forkmesh::siderail;

namespace {

constexpr int kSideRailWidth = 52;
constexpr int kSideRailIcon = 28;
constexpr int kSideRailButton = 38;
constexpr int kSideRailMinWindowWidth = 1000;

QJsonObject itemObject(const sr::Item &item)
{
    QJsonObject object{{QStringLiteral("kind"), item.kind}};
    if (!item.repo.isEmpty())
        object.insert(QStringLiteral("repo"), item.repo);
    if (!item.id.isEmpty())
        object.insert(QStringLiteral("id"), item.id);
    if (!item.url.isEmpty())
        object.insert(QStringLiteral("url"), item.url);
    if (!item.title.isEmpty())
        object.insert(QStringLiteral("title"), item.title);
    return object;
}

class SideRailItemButton final : public QToolButton
{
public:
    SideRailItemButton(const sr::Item &item, QWidget *parent = nullptr)
        : QToolButton(parent), m_item(item)
    {
        setObjectName(QStringLiteral("sideRailItem"));
        setProperty("sideRailKey", item.key());
        setProperty("sideRailKind", item.kind);
        setAutoRaise(true);
        setCursor(Qt::PointingHandCursor);
        setFixedSize(kSideRailButton, kSideRailButton);
        setIconSize(QSize(kSideRailIcon, kSideRailIcon));
        setFocusPolicy(Qt::TabFocus);
        setContextMenuPolicy(Qt::CustomContextMenu);
    }

    void setStatus(const QString &status)
    {
        m_status = status;
        setProperty("status", status);
        update();
    }

    const sr::Item &item() const { return m_item; }

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton)
            m_pressAt = event->position().toPoint();
        QToolButton::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!(event->buttons() & Qt::LeftButton) || m_pressAt.isNull() ||
            (event->position().toPoint() - m_pressAt).manhattanLength() <
                QApplication::startDragDistance()) {
            QToolButton::mouseMoveEvent(event);
            return;
        }
        m_pressAt = QPoint();
        setDown(false);
        auto *mime = new QMimeData;
        mime->setData(QLatin1String(sr::kItemMime),
                      QJsonDocument(itemObject(m_item)).toJson(QJsonDocument::Compact));
        if (m_item.kind == sr::kLinkKind)
            mime->setUrls({QUrl(m_item.url)});
        if (m_item.kind == sr::kWorkflowKind)
            mime->setData(QLatin1String(sr::kWorkflowPinMime),
                          sr::encodeWorkflowPin(m_item.repo, m_item.id));
        auto *drag = new QDrag(this);
        drag->setMimeData(mime);
        drag->setPixmap(grab());
        drag->setHotSpot(QPoint(width() / 2, height() / 2));
        if (QWidget *rail = window()->findChild<QWidget *>(QStringLiteral("sideRail")))
            rail->setProperty("dragging", true);
        drag->exec(Qt::MoveAction | Qt::CopyAction);
        if (QWidget *rail = window()->findChild<QWidget *>(QStringLiteral("sideRail")))
            rail->setProperty("dragging", false);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        m_pressAt = QPoint();
        QToolButton::mouseReleaseEvent(event);
    }

    void paintEvent(QPaintEvent *event) override
    {
        QToolButton::paintEvent(event);
        QColor ring;
        if (m_status == rw::kStatusRunning)
            ring = Theme::color(Theme::Role::Accent);
        else if (m_status == rw::kStatusError)
            ring = Theme::color(Theme::Role::Danger);
        if (!ring.isValid())
            return;
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(ring, 2));
        painter.setBrush(Qt::NoBrush);
        const QRectF target((width() - kSideRailIcon) / 2.0, (height() - kSideRailIcon) / 2.0,
                            kSideRailIcon, kSideRailIcon);
        painter.drawEllipse(target.adjusted(-1.5, -1.5, 1.5, 1.5));
    }

private:
    sr::Item m_item;
    QString m_status;
    QPoint m_pressAt;
};

class SideRailDropArea final : public QWidget
{
public:
    std::function<bool(const QMimeData *)> accepts;
    std::function<bool(const QMimeData *, const QString &)> drop;

    explicit SideRailDropArea(QWidget *parent = nullptr) : QWidget(parent)
    {
        setObjectName(QStringLiteral("sideRailContent"));
        setAcceptDrops(true);
    }

protected:
    void dragEnterEvent(QDragEnterEvent *event) override
    {
        if (!accepts || !accepts(event->mimeData()))
            return;
        event->acceptProposedAction();
        m_indicatorY = indicatorFor(event->position().toPoint().y(), nullptr);
        update();
    }

    void dragMoveEvent(QDragMoveEvent *event) override
    {
        if (!accepts || !accepts(event->mimeData()))
            return;
        event->acceptProposedAction();
        m_indicatorY = indicatorFor(event->position().toPoint().y(), nullptr);
        update();
    }

    void dragLeaveEvent(QDragLeaveEvent *event) override
    {
        m_indicatorY = -1;
        update();
        QWidget::dragLeaveEvent(event);
    }

    void dropEvent(QDropEvent *event) override
    {
        QString before;
        indicatorFor(event->position().toPoint().y(), &before);
        m_indicatorY = -1;
        update();
        if (drop && drop(event->mimeData(), before))
            event->acceptProposedAction();
        else
            event->ignore();
    }

    void paintEvent(QPaintEvent *event) override
    {
        QWidget::paintEvent(event);
        if (m_indicatorY < 0)
            return;
        QPainter painter(this);
        painter.setPen(QPen(Theme::color(Theme::Role::Accent), 2));
        painter.drawLine(QPoint(8, m_indicatorY), QPoint(width() - 8, m_indicatorY));
    }

private:
    int indicatorFor(int y, QString *beforeKey) const
    {
        QList<SideRailItemButton *> ordered;
        for (QObject *child : children())
            if (auto *button = dynamic_cast<SideRailItemButton *>(child);
                button && button->isVisible())
                ordered.append(button);
        std::sort(ordered.begin(), ordered.end(),
                  [](const QWidget *a, const QWidget *b) { return a->y() < b->y(); });
        for (SideRailItemButton *button : std::as_const(ordered)) {
            if (y < button->geometry().center().y()) {
                if (beforeKey)
                    *beforeKey = button->item().key();
                return qMax(1, button->y() - 1);
            }
        }
        if (beforeKey)
            beforeKey->clear();
        if (ordered.isEmpty())
            return 4;
        return ordered.constLast()->geometry().bottom() + 2;
    }

    int m_indicatorY = -1;
};

class BrowserResizeHandle final : public QWidget
{
public:
    std::function<void(int)> resized;
    std::function<void()> finished;

    explicit BrowserResizeHandle(QWidget *panel) : QWidget(panel), m_panel(panel)
    {
        setObjectName(QStringLiteral("sideRailBrowserHandle"));
        setFixedWidth(5);
        setCursor(Qt::SizeHorCursor);
    }

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton)
            return;
        m_startX = event->globalPosition().toPoint().x();
        m_startWidth = m_panel->width();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (m_startX < 0 || !resized)
            return;
        resized(m_startWidth + m_startX - event->globalPosition().toPoint().x());
    }

    void mouseReleaseEvent(QMouseEvent *) override
    {
        if (m_startX >= 0 && finished)
            finished();
        m_startX = -1;
    }

private:
    QWidget *m_panel = nullptr;
    int m_startX = -1;
    int m_startWidth = 0;
};

QPushButton *railToolButton(const char *name, const QString &tip)
{
    auto *button = new QPushButton;
    button->setObjectName(QString::fromLatin1(name));
    button->setProperty("sideRailTool", true);
    button->setCursor(Qt::PointingHandCursor);
    button->setFocusPolicy(Qt::TabFocus);
    button->setFixedSize(kSideRailButton, kSideRailButton);
    button->setIconSize(QSize(18, 18));
    button->setToolTip(tip);
    button->setAccessibleName(tip);
    return button;
}

} // namespace

bool MainWindow::sideRailEnabled() const
{
    const QSettings settings;
    return settings.value(sr::kShowSetting, settings.value(sr::kLegacyShowSetting, true))
        .toBool();
}

void MainWindow::setSideRailEnabled(bool enabled)
{
    QSettings().setValue(sr::kShowSetting, enabled);
    if (m_sideRailCheck && m_sideRailCheck->isChecked() != enabled) {
        const QSignalBlocker blocker(m_sideRailCheck);
        m_sideRailCheck->setChecked(enabled);
    }
    applySideRailVisibility();
}

void MainWindow::applySideRailVisibility()
{
    if (!m_sideRail)
        return;
    const bool visible = sideRailEnabled() && !appViewModeHidesShellChrome(m_viewMode) &&
                         width() >= kSideRailMinWindowWidth;
    if (visible == !m_sideRail->isHidden())
        return;
    m_sideRail->setVisible(visible);
    if (visible)
        scheduleSideRailRefresh();
}

QWidget *MainWindow::buildSideRail()
{
    auto *rail = new QWidget;
    rail->setObjectName(QStringLiteral("sideRail"));
    rail->setAccessibleName(QStringLiteral("Side rail"));
    rail->setAttribute(Qt::WA_StyledBackground, true);
    rail->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Ignored);
    auto *row = new QHBoxLayout(rail);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);

    auto *panel = new QWidget;
    panel->setObjectName(QStringLiteral("sideRailBrowserPanel"));
    panel->setAttribute(Qt::WA_StyledBackground, true);
    panel->setFixedWidth(sr::clampBrowserWidth(QSettings().value(sr::kBrowserWidthSetting).toInt()));
    auto *panelRow = new QHBoxLayout(panel);
    panelRow->setContentsMargins(0, kRepoTabRowTopInset, 4, 8);
    panelRow->setSpacing(0);
    auto *handle = new BrowserResizeHandle(panel);
    handle->resized = [panel](int width) { panel->setFixedWidth(sr::clampBrowserWidth(width)); };
    handle->finished = [panel] { QSettings().setValue(sr::kBrowserWidthSetting, panel->width()); };
    panelRow->addWidget(handle);
    auto *panelColumn = new QVBoxLayout;
    panelColumn->setContentsMargins(0, 0, 0, 0);
    panelColumn->setSpacing(4);
    auto *panelHeader = new QHBoxLayout;
    panelHeader->setContentsMargins(4, 0, 0, 0);
    auto *panelTitle = new QLabel(QStringLiteral("Browser"));
    panelTitle->setObjectName(QStringLiteral("sideRailBrowserTitle"));
    panelHeader->addWidget(panelTitle);
    panelHeader->addStretch(1);
    auto *panelClose = new QPushButton;
    panelClose->setObjectName(QStringLiteral("windowChromeButton"));
    panelClose->setCursor(Qt::PointingHandCursor);
    panelClose->setFocusPolicy(Qt::NoFocus);
    panelClose->setFixedSize(26, 24);
    panelClose->setToolTip(QStringLiteral("Close the side browser"));
    setOcticon(panelClose, QStringLiteral("x"), 12);
    connect(panelClose, &QPushButton::clicked, this, [this] { setSideRailBrowserOpen(false); });
    panelHeader->addWidget(panelClose);
    panelColumn->addLayout(panelHeader);
    auto *browser = new WorkspaceBrowserPane;
    browser->setObjectName(QStringLiteral("sideRailBrowser"));
    WorkspaceBrowserPane::Icons icons;
    const QColor muted = Theme::color(Theme::Role::FgMuted);
    icons.back = QIcon(tintedOcticonPixmap(QStringLiteral("chevron-left"), muted, 14));
    icons.forward = QIcon(tintedOcticonPixmap(QStringLiteral("chevron-right"), muted, 14));
    icons.reload = QIcon(tintedOcticonPixmap(QStringLiteral("sync"), muted, 14));
    icons.external = QIcon(tintedOcticonPixmap(QStringLiteral("link-external"), muted, 14));
    browser->setIcons(icons);
    m_sideRailBrowser = browser;
    panelColumn->addWidget(browser, 1);
    panelRow->addLayout(panelColumn, 1);
    panel->hide();
    m_sideRailBrowserPanel = panel;
    row->addWidget(panel);

    auto *column = new QWidget;
    column->setObjectName(QStringLiteral("sideRailColumn"));
    column->setFixedWidth(kSideRailWidth);
    auto *columnLayout = new QVBoxLayout(column);
    columnLayout->setContentsMargins(0, kRepoTabRowTopInset + 4, 0, 8);
    columnLayout->setSpacing(2);

    QPushButton *terminal = railToolButton("sideRailTerminalButton",
                                           QStringLiteral("Open a terminal tab"));
    setOcticon(terminal, QStringLiteral("terminal"), 18);
    connect(terminal, &QPushButton::clicked, this, [this] { showTerminalLauncher(); });
    columnLayout->addWidget(terminal, 0, Qt::AlignHCenter);
    QPushButton *browse = railToolButton("sideRailBrowserButton",
                                         QStringLiteral("Show the side browser"));
    browse->setCheckable(true);
    setOcticon(browse, QStringLiteral("device-desktop"), 18);
    connect(browse, &QPushButton::toggled, this, [this](bool on) { setSideRailBrowserOpen(on); });
    m_sideRailBrowserButton = browse;
    columnLayout->addWidget(browse, 0, Qt::AlignHCenter);
    auto *line = new QFrame;
    line->setObjectName(QStringLiteral("sideRailSeparator"));
    line->setFrameShape(QFrame::HLine);
    line->setFixedWidth(24);
    columnLayout->addSpacing(4);
    columnLayout->addWidget(line, 0, Qt::AlignHCenter);
    columnLayout->addSpacing(4);

    auto *content = new SideRailDropArea;
    content->setContextMenuPolicy(Qt::CustomContextMenu);
    content->accepts = [](const QMimeData *mime) {
        return !sr::itemsFromMime(mime, kRailItemMimeType).isEmpty();
    };
    content->drop = [this](const QMimeData *mime, const QString &before) {
        const QList<sr::Item> current = sideRailItems();
        const int at = before.isEmpty() ? -1 : sr::indexOfKey(current, before);
        return dropOnSideRail(mime, at);
    };
    connect(content, &QWidget::customContextMenuRequested, this,
            [this, content](const QPoint &pos) { showSideRailMenu(content->mapToGlobal(pos)); });
    m_sideRailItemsLayout = new QVBoxLayout(content);
    m_sideRailItemsLayout->setContentsMargins(0, 2, 0, 2);
    m_sideRailItemsLayout->setSpacing(4);
    m_sideRailItemsLayout->setAlignment(Qt::AlignHCenter | Qt::AlignTop);

    auto *scroll = new QScrollArea;
    scroll->setObjectName(QStringLiteral("sideRailScroll"));
    scroll->setWidget(content);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setMinimumHeight(0);
    scroll->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Ignored);
    scroll->setFixedWidth(kSideRailWidth);
    columnLayout->addWidget(scroll, 1);
    row->addWidget(column);

    m_sideRail = rail;
    m_sideRailRefreshTimer = new QTimer(this);
    m_sideRailRefreshTimer->setSingleShot(true);
    m_sideRailRefreshTimer->setInterval(150);
    connect(m_sideRailRefreshTimer, &QTimer::timeout, this, [this] { refreshSideRail(); });

    applySideRailVisibility();
    if (QSettings().value(sr::kBrowserOpenSetting, false).toBool())
        setSideRailBrowserOpen(true);
    refreshSideRail();
    return rail;
}

void MainWindow::scheduleSideRailRefresh()
{
    if (!m_sideRailRefreshTimer || !m_sideRail || m_sideRail->isHidden())
        return;
    if (!m_sideRailRefreshTimer->isActive())
        m_sideRailRefreshTimer->start();
}

QList<sr::Item> MainWindow::sideRailItems() const
{
    QList<sr::WorkflowRef> right;
    for (const rw::Pin &pin : workflowPins())
        if (pin.rail == rw::kRailRight)
            right.append({pin.repo, pin.workflowId});
    return sr::withWorkflows(sr::parseItems(QSettings().value(sr::kItemsSetting).toString()),
                             right);
}

void MainWindow::saveSideRailItems(const QList<sr::Item> &items)
{
    QSettings().setValue(sr::kItemsSetting, sr::serializeItems(items));
    if (m_sideRailRefreshTimer)
        m_sideRailRefreshTimer->start();
}

#ifdef FORKMESH_WINDOW_TESTS
QStringList MainWindow::testSideRailKeys() const
{
    QStringList keys;
    for (const sr::Item &item : sideRailItems())
        keys << item.key();
    return keys;
}
#endif

bool MainWindow::dropOnSideRail(const QMimeData *mime, int index)
{
    const QList<sr::Item> dropped = sr::itemsFromMime(mime, kRailItemMimeType);
    if (dropped.isEmpty())
        return false;
    QList<sr::Item> items = sideRailItems();
    int at = index < 0 || index > items.size() ? int(items.size()) : index;
    bool added = false;
    for (sr::Item item : dropped) {
        if (item.kind == sr::kWorkflowKind) {
            const int repoIndex = repoIndexForKey(item.repo);
            rw::Workflow workflow;
            const bool pinned = !rw::pinnedRail(workflowPins(), item.repo, item.id).isEmpty();
            if (!pinned && (repoIndex < 0 || !findRepoWorkflow(repoIndex, item.id, &workflow))) {
                flashMessage(QStringLiteral("Open %1 on this desktop to put its workflow on the "
                                            "side rail.")
                                 .arg(item.repo),
                             true);
                continue;
            }
            if (rw::pinnedRail(workflowPins(), item.repo, item.id) != rw::kRailRight)
                pinWorkflowToRail(item.repo, item.id, rw::kRailRight);
        } else if (item.kind == sr::kPageKind) {
            QWidget *page = railWidgetForKey(item.id);
            if (!page)
                continue;
            if (item.title.isEmpty()) {
                QString title = page->accessibleName();
                if (auto *button = qobject_cast<QAbstractButton *>(page);
                    button && title.isEmpty())
                    title = button->text();
                item = sr::pageItem(item.id, title.isEmpty() ? page->toolTip() : title);
            }
        }
        items = sr::insertItem(items, item, at);
        at = sr::indexOfKey(items, item.key()) + 1;
        added = true;
    }
    if (!added)
        return false;
    saveSideRailItems(items);
    refreshSideRail();
    return true;
}

void MainWindow::refreshSideRail()
{
    if (!m_sideRailItemsLayout)
        return;
    if (QApplication::activePopupWidget() ||
        (m_sideRail && m_sideRail->property("dragging").toBool())) {
        if (m_sideRailRefreshTimer)
            m_sideRailRefreshTimer->start(400);
        return;
    }
    while (QLayoutItem *item = m_sideRailItemsLayout->takeAt(0)) {
        if (QWidget *widget = item->widget())
            widget->deleteLater();
        delete item;
    }
    QWidget *host = m_sideRailItemsLayout->parentWidget();
    const QColor muted = Theme::color(Theme::Role::FgMuted);
    QHash<QString, QList<rw::Listing>> listings;
    int shown = 0;
    for (const sr::Item &item : sideRailItems()) {
        auto *button = new SideRailItemButton(item, host);
        if (item.kind == sr::kWorkflowKind) {
            const int repoIndex = repoIndexForKey(item.repo);
            if (repoIndex < 0) {
                delete button;
                continue;
            }
            if (!listings.contains(item.repo))
                listings.insert(item.repo,
                                rw::listRepositoryWorkflows(m_repositories.at(repoIndex).localPath));
            rw::Listing found;
            for (const rw::Listing &listing : listings.value(item.repo))
                if (listing.workflow.id == item.id)
                    found = listing;
            if (found.workflow.id.isEmpty()) {
                delete button;
                continue;
            }
            const bool running = repoWorkflowRunner()->isRunning(item.repo, item.id);
            const QList<rw::Run> runs = rw::loadRuns(repoWorkflowRunsFile(repoIndex, item.id));
            QString status = running ? rw::kStatusRunning
                                     : (runs.isEmpty() ? QString() : runs.first().status);
            QStringList tip{QStringLiteral("%1 · %2").arg(found.workflow.name, item.repo),
                            found.workflow.triggerSummary()};
            if (!runs.isEmpty() && !running)
                tip << QStringLiteral("Last run %1: %2")
                           .arg(forkmesh::teammates::humanLastActive(runs.first().startedAtMs),
                                rw::runSummaryText(runs.first()));
            if (!found.error.isEmpty()) {
                status = rw::kStatusError;
                tip << found.error;
            }
            tip << (running ? QStringLiteral("Running · click to open it in Workflows")
                            : QStringLiteral("Click to run · right-click for more · drag "
                                             "to reorder"));
            button->setIcon(QIcon(repoWorkflowIcon(found.workflow, kSideRailIcon)));
            button->setToolTip(tip.join(QLatin1Char('\n')));
            button->setAccessibleName(QStringLiteral("Run %1").arg(found.workflow.name));
            button->setStatus(status);
        } else if (item.kind == sr::kPageKind) {
            QWidget *page = railWidgetForKey(item.id);
            if (!page) {
                delete button;
                continue;
            }
            auto *pageButton = qobject_cast<QAbstractButton *>(page);
            QIcon icon = pageButton ? pageButton->icon() : QIcon();
            if (icon.isNull())
                icon = QIcon(tintedOcticonPixmap(QStringLiteral("file"), muted, 18));
            button->setIcon(icon);
            button->setIconSize(QSize(18, 18));
            const QString title = item.title.isEmpty() ? item.id : item.title;
            button->setToolTip(QStringLiteral("%1\nClick to open · right-click for more").arg(title));
            button->setAccessibleName(QStringLiteral("Open %1").arg(title));
        } else {
            button->setIcon(QIcon(tintedOcticonPixmap(QStringLiteral("link"), muted, 18)));
            button->setIconSize(QSize(18, 18));
            button->setToolTip(QStringLiteral("%1\n%2\nClick to open in the side browser")
                                   .arg(item.title, item.url));
            button->setAccessibleName(QStringLiteral("Open %1").arg(item.title));
        }
        connect(button, &QToolButton::clicked, this, [this, item] { activateSideRailItem(item); });
        connect(button, &QWidget::customContextMenuRequested, this,
                [this, button, item](const QPoint &point) {
                    showSideRailItemMenu(item, button->mapToGlobal(point));
                });
        m_sideRailItemsLayout->addWidget(button, 0, Qt::AlignHCenter);
        ++shown;
    }
    auto *add = new QPushButton(host);
    add->setObjectName(QStringLiteral("sideRailAddButton"));
    add->setCursor(Qt::PointingHandCursor);
    add->setFixedSize(kSideRailButton, kSideRailButton);
    setOcticon(add, QStringLiteral("plus"), 16);
    add->setToolTip(shown == 0 ? QStringLiteral("Drag workflows, special agents, pages from the "
                                                "left rail, or links here. Click to add one.")
                               : QStringLiteral("Add to the side rail"));
    add->setAccessibleName(QStringLiteral("Add to the side rail"));
    connect(add, &QPushButton::clicked, this,
            [this, add] { showSideRailMenu(add->mapToGlobal(QPoint(0, add->height()))); });
    m_sideRailItemsLayout->addWidget(add, 0, Qt::AlignHCenter);
    m_sideRailItemsLayout->addStretch(1);
}

void MainWindow::activateSideRailItem(const sr::Item &item)
{
    if (item.kind == sr::kWorkflowKind) {
        runPinnedWorkflow(item.repo, item.id, rw::kRailRight);
        return;
    }
    if (item.kind == sr::kPageKind) {
        if (auto *button = qobject_cast<QAbstractButton *>(railWidgetForKey(item.id)))
            button->click();
        return;
    }
    openInSideRailBrowser(item.url);
}

void MainWindow::showSideRailItemMenu(const sr::Item &item, const QPoint &globalPos)
{
    if (item.kind == sr::kWorkflowKind) {
        showWorkflowPinMenu(item.repo, item.id, globalPos);
        return;
    }
    QMenu menu(this);
    connect(menu.addAction(QStringLiteral("Open")), &QAction::triggered, this,
            [this, item] { activateSideRailItem(item); });
    if (item.kind == sr::kLinkKind) {
        const QUrl url(item.url);
        connect(menu.addAction(QStringLiteral("Open in the system browser")), &QAction::triggered,
                this, [url] { QDesktopServices::openUrl(url); });
        connect(menu.addAction(QStringLiteral("Copy link")), &QAction::triggered, this,
                [url] { QApplication::clipboard()->setText(url.toString()); });
    }
    menu.addSeparator();
    const QList<sr::Item> items = sideRailItems();
    const int at = sr::indexOfKey(items, item.key());
    QAction *up = menu.addAction(QStringLiteral("Move up"));
    up->setEnabled(at > 0);
    connect(up, &QAction::triggered, this, [this, item, at] {
        saveSideRailItems(sr::insertItem(sideRailItems(), item, at - 1));
    });
    QAction *down = menu.addAction(QStringLiteral("Move down"));
    down->setEnabled(at >= 0 && at < items.size() - 1);
    connect(down, &QAction::triggered, this, [this, item, at] {
        saveSideRailItems(sr::insertItem(sideRailItems(), item, at + 2));
    });
    QAction *remove = menu.addAction(QStringLiteral("Remove from the side rail"));
    remove->setObjectName(QStringLiteral("sideRailRemoveAction"));
    connect(remove, &QAction::triggered, this, [this, key = item.key()] {
        saveSideRailItems(sr::removeItem(sideRailItems(), key));
    });
    menu.exec(globalPos);
}

void MainWindow::showSideRailMenu(const QPoint &globalPos)
{
    QMenu menu(this);
    connect(menu.addAction(QStringLiteral("Open a terminal tab…")), &QAction::triggered, this,
            [this] { QTimer::singleShot(0, this, [this] { showTerminalLauncher(); }); });
    connect(menu.addAction(sideRailBrowserOpen() ? QStringLiteral("Hide the side browser")
                                                 : QStringLiteral("Show the side browser")),
            &QAction::triggered, this, [this] { setSideRailBrowserOpen(!sideRailBrowserOpen()); });
    menu.addSeparator();
    const int repoIndex = activeTeammateRepoIndex();
    QMenu *workflows = menu.addMenu(QStringLiteral("Add a workflow"));
    const QString repo = repoIndex >= 0 ? shortcutRepoKey(repoIndex) : QString();
    const QList<sr::Item> current = sideRailItems();
    if (repoIndex >= 0 && repoIndex < m_repositories.size()) {
        for (const rw::Listing &listing :
             rw::listRepositoryWorkflows(m_repositories.at(repoIndex).localPath)) {
            if (listing.workflow.id.isEmpty())
                continue;
            const sr::Item item = sr::workflowItem(repo, listing.workflow.id);
            QAction *action = workflows->addAction(
                QIcon(repoWorkflowIcon(listing.workflow, 16)), listing.workflow.name);
            action->setEnabled(sr::indexOfKey(current, item.key()) < 0);
            connect(action, &QAction::triggered, this, [this, item] {
                auto mime = std::make_unique<QMimeData>();
                mime->setData(QLatin1String(sr::kWorkflowMime), sr::encodeWorkflow(item.repo, item.id));
                dropOnSideRail(mime.get(), -1);
            });
        }
    }
    if (workflows->isEmpty())
        workflows->addAction(QStringLiteral("No workflows in this repository"))->setEnabled(false);
    QMenu *pages = menu.addMenu(QStringLiteral("Add a page"));
    for (QWidget *widget : railItemWidgets()) {
        auto *button = qobject_cast<QAbstractButton *>(widget);
        const QString key = widget->property("forkmeshRailKey").toString();
        if (!button || key.isEmpty() || button->isHidden())
            continue;
        const QString title = button->accessibleName().isEmpty() ? button->text()
                                                                 : button->accessibleName();
        if (title.trimmed().isEmpty())
            continue;
        QAction *action = pages->addAction(button->icon(), title);
        action->setEnabled(sr::indexOfKey(current, sr::pageItem(key).key()) < 0);
        connect(action, &QAction::triggered, this, [this, key] {
            auto mime = std::make_unique<QMimeData>();
            mime->setData(kRailItemMimeType, key.toUtf8());
            dropOnSideRail(mime.get(), -1);
        });
    }
    if (pages->isEmpty())
        pages->addAction(QStringLiteral("No pages"))->setEnabled(false);
    connect(menu.addAction(QStringLiteral("Add a link…")), &QAction::triggered, this, [this] {
        QTimer::singleShot(0, this, [this] {
            bool ok = false;
            const QString text = QInputDialog::getText(
                this, QStringLiteral("Add a link"), QStringLiteral("Address (http or https)"),
                QLineEdit::Normal, QString(), &ok);
            if (!ok || text.trimmed().isEmpty())
                return;
            auto mime = std::make_unique<QMimeData>();
            mime->setText(WorkspaceBrowserPane::urlFromInput(text).toString());
            if (!dropOnSideRail(mime.get(), -1))
                flashMessage(QStringLiteral("Only http and https links fit on the side rail."),
                             true);
        });
    });
    menu.addSeparator();
    connect(menu.addAction(QStringLiteral("Open Workflows")), &QAction::triggered, this,
            [this] { openWorkflowInPage(QString(), QString()); });
    QAction *hide = menu.addAction(QStringLiteral("Hide the side rail"));
    hide->setObjectName(QStringLiteral("sideRailHideAction"));
    connect(hide, &QAction::triggered, this, [this] {
        setSideRailEnabled(false);
        flashMessage(QStringLiteral("Side rail hidden. Turn it back on from the top bar or in "
                                    "Settings > Appearance."));
    });
    menu.exec(globalPos);
}

bool MainWindow::sideRailBrowserOpen() const
{
    return m_sideRailBrowserPanel && !m_sideRailBrowserPanel->isHidden();
}

void MainWindow::setSideRailBrowserOpen(bool open)
{
    if (!m_sideRailBrowserPanel)
        return;
    QSettings().setValue(sr::kBrowserOpenSetting, open);
    if (m_sideRailBrowserButton && m_sideRailBrowserButton->isChecked() != open) {
        const QSignalBlocker blocker(m_sideRailBrowserButton);
        m_sideRailBrowserButton->setChecked(open);
    }
    if (m_sideRailBrowserButton)
        m_sideRailBrowserButton->setToolTip(open ? QStringLiteral("Hide the side browser")
                                                 : QStringLiteral("Show the side browser"));
    if (open == sideRailBrowserOpen())
        return;
    m_sideRailBrowserPanel->setVisible(open);
    if (open && m_sideRailBrowser && m_sideRailBrowser->currentUrl().isEmpty()) {
        const QString last = QSettings().value(sr::kBrowserUrlSetting).toString();
        if (!last.isEmpty())
            m_sideRailBrowser->navigate(last);
        else
            m_sideRailBrowser->focusAddress();
    }
}

void MainWindow::openInSideRailBrowser(const QString &address)
{
    if (!m_sideRailBrowser)
        return;
    if (!sideRailEnabled())
        setSideRailEnabled(true);
    setSideRailBrowserOpen(true);
    m_sideRailBrowser->navigate(address);
    const QUrl url = m_sideRailBrowser->currentUrl();
    if (url.isValid() && !url.isEmpty())
        QSettings().setValue(sr::kBrowserUrlSetting, url.toString());
}
