#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "AppViewMode.h"
#include "ForkMeshPackageRegistry.h"
#include "PackageRailTile.h"

#include <QApplication>
#include <QDateTime>
#include <QLabel>
#include <QMenu>
#include <QPointer>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QTimer>

using namespace forkmesh::ui;
using forkmesh::market::NativePackageRailStep;
using forkmesh::market::NativePackageRailTile;
using forkmesh::market::PackageRailTileButton;
using forkmesh::market::packageRailNeedsMoreAttention;
using forkmesh::market::packageRailReminderBody;
using forkmesh::market::packageRailReminderTitle;
using forkmesh::market::packageRailStateKey;

namespace {

constexpr int kPackageRailWidth = 60;
constexpr int kPackageRailPrepareIntervalMs = 15 * 60 * 1000;
constexpr int kPackageRailReminderQuietMs = 60 * 60 * 1000;
const QString kPackageRailLinkKind = QStringLiteral("package-rail");
const QString kPackageRailHrefPrefix = QStringLiteral("fm:package-rail:");

QString railToneKey(forkmesh::market::NativePackageRailTone tone)
{
    return QString::number(static_cast<int>(tone));
}

} // namespace

bool MainWindow::packageRailEnabled() const
{
    return QSettings().value(kShowPackageRailSetting, true).toBool();
}

void MainWindow::setPackageRailEnabled(bool enabled)
{
    QSettings().setValue(kShowPackageRailSetting, enabled);
    if (m_packageRailCheck && m_packageRailCheck->isChecked() != enabled) {
        const QSignalBlocker blocker(m_packageRailCheck);
        m_packageRailCheck->setChecked(enabled);
    }
    if (enabled)
        preparePackageRails();
    applyPackageRailVisibility();
}

bool MainWindow::packageRailReminderEnabled() const
{
    return QSettings().value(kPackageRailReminderSetting, true).toBool();
}

void MainWindow::setPackageRailReminderEnabled(bool enabled)
{
    QSettings().setValue(kPackageRailReminderSetting, enabled);
    if (m_packageRailReminderCheck &&
        m_packageRailReminderCheck->isChecked() != enabled) {
        const QSignalBlocker blocker(m_packageRailReminderCheck);
        m_packageRailReminderCheck->setChecked(enabled);
    }
}

NotificationLink MainWindow::packageRailLinkForHref(const QString &href) const
{
    NotificationLink link;
    if (!href.startsWith(kPackageRailHrefPrefix))
        return link;
    const QString target = href.mid(kPackageRailHrefPrefix.size());
    const int split = target.indexOf(QLatin1Char(':'));
    if (split <= 0 || split == target.size() - 1)
        return link;
    link.kind = kPackageRailLinkKind;
    link.name = target.left(split);
    link.ref = target.mid(split + 1);
    return link;
}

bool MainWindow::openPackageRailLink(const NotificationLink &link)
{
    if (link.kind != kPackageRailLinkKind)
        return false;
    openPackageRailTile(link.name, link.ref);
    return true;
}

void MainWindow::checkPackageRailReminder()
{
    if (!packageRailEnabled() || !packageRailReminderEnabled() ||
        m_packageRailState.isEmpty())
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_packageRailChangedAtMs < kPackageRailReminderQuietMs)
        return;
    for (const AppNotification &item : std::as_const(m_notifications)) {
        if (item.link.kind == kPackageRailLinkKind && item.dismissOnly &&
            !item.acknowledged)
            return;
    }
    QString packageId;
    NativePackageRailTile worst;
    for (const NativePackagePage &page : std::as_const(m_nativePackagePages)) {
        if (!page.widget)
            continue;
        for (const NativePackageRailTile &tile : page.widget->railTiles()) {
            if (packageRailNeedsMoreAttention(tile, worst)) {
                worst = tile;
                packageId = page.id;
            }
        }
    }
    if (packageId.isEmpty())
        return;
    AppNotification ping;
    ping.title = packageRailReminderTitle(worst);
    ping.body = packageRailReminderBody(worst);
    ping.kind = QStringLiteral("channel_reminder");
    ping.link.kind = kPackageRailLinkKind;
    ping.link.name = packageId;
    ping.link.ref = worst.id;
    ping.dismissOnly = true;
    recordNotification(ping);
    postNotification(ping.title, ping.body, false,
                     QStringLiteral("appointment-soon"),
                     kPackageRailHrefPrefix + packageId + QLatin1Char(':') + worst.id,
                     true);
}

void MainWindow::applyPackageRailVisibility()
{
    if (!m_packageRail)
        return;
    const bool visible = packageRailEnabled() && m_packageRailHasTiles &&
                         !appViewModeHidesShellChrome(m_viewMode);
    if (visible == !m_packageRail->isHidden())
        return;
    m_packageRail->setVisible(visible);
}

QWidget *MainWindow::buildPackageRail()
{
    auto *content = new QWidget;
    content->setObjectName(QStringLiteral("packageRailContent"));
    content->setContextMenuPolicy(Qt::CustomContextMenu);
    m_packageRailLayout = new QVBoxLayout(content);
    m_packageRailLayout->setContentsMargins(0, kRepoTabRowTopInset + 4, 0, 8);
    m_packageRailLayout->setSpacing(8);

    auto *scroll = new QScrollArea;
    scroll->setObjectName(QStringLiteral("packageRail"));
    scroll->setAccessibleName(QStringLiteral("Package status"));
    scroll->setWidget(content);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setFixedWidth(kPackageRailWidth);
    scroll->setMinimumHeight(0);
    scroll->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Ignored);
    scroll->hide();
    m_packageRail = scroll;

    connect(content, &QWidget::customContextMenuRequested, this,
            [this, content](const QPoint &pos) {
                QMenu menu(this);
                QAction *hide = menu.addAction(QStringLiteral("Hide this rail"));
                hide->setObjectName(QStringLiteral("packageRailHideAction"));
                connect(hide, &QAction::triggered, this, [this] {
                    setPackageRailEnabled(false);
                    flashMessage(QStringLiteral(
                        "Channel status rail hidden. Turn it back on in Settings > "
                        "Appearance."));
                });
                menu.exec(content->mapToGlobal(pos));
            });

    m_packageRailRefreshTimer = new QTimer(this);
    m_packageRailRefreshTimer->setSingleShot(true);
    m_packageRailRefreshTimer->setInterval(150);
    connect(m_packageRailRefreshTimer, &QTimer::timeout, this,
            [this] { refreshPackageRail(); });
    m_packageRailClock = new QTimer(this);
    m_packageRailClock->setInterval(kPackageRailPrepareIntervalMs);
    connect(m_packageRailClock, &QTimer::timeout, this,
            [this] { preparePackageRails(); });
    m_packageRailClock->start();
    m_packageRailReminderTimer = new QTimer(this);
    m_packageRailReminderTimer->setInterval(kPackageRailReminderQuietMs);
    connect(m_packageRailReminderTimer, &QTimer::timeout, this,
            [this] { checkPackageRailReminder(); });

    refreshPackageRail();
    return scroll;
}

void MainWindow::schedulePackageRailRefresh()
{
    if (m_packageRailRefreshTimer && !m_packageRailRefreshTimer->isActive())
        m_packageRailRefreshTimer->start();
}

void MainWindow::preparePackageRails()
{
    if (!packageRailEnabled())
        return;
    for (const NativePackagePage &page : std::as_const(m_nativePackagePages))
        queuePackagePreparation(page.widget, false);
}

void MainWindow::preparePackagePage(forkmesh::market::NativePackageWidget *widget)
{
    if (!widget)
        return;
    widget->prepareNavigationBadge();
    if (packageRailEnabled())
        widget->prepareRail();
}

void MainWindow::preparePackagePages()
{
    for (const NativePackagePage &page : std::as_const(m_nativePackagePages))
        queuePackagePreparation(page.widget, true);
}

void MainWindow::queuePackagePreparation(
    forkmesh::market::NativePackageWidget *widget, bool wholePage)
{
    if (!widget)
        return;
    for (PendingPackagePreparation &pending : m_packagePreparationQueue) {
        if (pending.widget == widget) {
            pending.wholePage = pending.wholePage || wholePage;
            return;
        }
    }
    m_packagePreparationQueue.append({widget, wholePage});
    if (m_packagePreparationScheduled)
        return;
    m_packagePreparationScheduled = true;
    QTimer::singleShot(0, this, &MainWindow::drainPackagePreparation);
}

void MainWindow::drainPackagePreparation()
{
    m_packagePreparationScheduled = false;
    while (!m_packagePreparationQueue.isEmpty()) {
        const PendingPackagePreparation next =
            m_packagePreparationQueue.takeFirst();
        if (!next.widget)
            continue;
        if (next.wholePage)
            preparePackagePage(next.widget);
        else if (packageRailEnabled())
            next.widget->prepareRail();
        break;
    }
    if (m_packagePreparationQueue.isEmpty() || m_packagePreparationScheduled)
        return;
    m_packagePreparationScheduled = true;
    QTimer::singleShot(0, this, &MainWindow::drainPackagePreparation);
}

void MainWindow::refreshPackageRail()
{
    if (!m_packageRailLayout)
        return;
    if (QApplication::activePopupWidget()) {
        schedulePackageRailRefresh();
        return;
    }
    const bool dark = currentThemeIsDark();
    struct Group {
        QString packageId;
        QString title;
        QList<NativePackageRailTile> tiles;
    };
    QList<Group> groups;
    QStringList signature{dark ? QStringLiteral("dark") : QStringLiteral("light")};
    QStringList state;
    for (const NativePackagePage &page : std::as_const(m_nativePackagePages)) {
        if (!page.widget)
            continue;
        Group group{page.id, page.widget->railTitle(), page.widget->railTiles()};
        if (group.tiles.isEmpty())
            continue;
        signature << page.id << group.title;
        for (const NativePackageRailTile &tile : std::as_const(group.tiles)) {
            signature << tile.id << tile.label << tile.count << tile.countLabel
                      << railToneKey(tile.tone) << tile.stateLabel
                      << QString::number(tile.pending) << tile.details
                      << QString::number(tile.opensPage);
            for (const NativePackageRailStep &step : tile.steps)
                signature << step.id << step.text;
            state << page.id << packageRailStateKey(tile);
        }
        groups.append(group);
    }
    m_packageRailHasTiles = !groups.isEmpty();
    const QString stateJoined = state.join(QChar(0x1e));
    if (stateJoined != m_packageRailState) {
        m_packageRailState = stateJoined;
        m_packageRailChangedAtMs = QDateTime::currentMSecsSinceEpoch();
        if (m_packageRailReminderTimer)
            m_packageRailReminderTimer->start();
    }
    const QString joined = signature.join(QChar(0x1f));
    if (joined != m_packageRailSignature) {
        m_packageRailSignature = joined;
        while (QLayoutItem *item = m_packageRailLayout->takeAt(0)) {
            if (QWidget *widget = item->widget())
                widget->deleteLater();
            delete item;
        }
        for (const Group &group : std::as_const(groups)) {
            if (!group.title.isEmpty()) {
                auto *title = new QLabel(group.title);
                title->setObjectName(QStringLiteral("packageRailTitle"));
                title->setAlignment(Qt::AlignHCenter);
                m_packageRailLayout->addWidget(title, 0, Qt::AlignHCenter);
            }
            for (const NativePackageRailTile &tile : group.tiles) {
                auto *button = new PackageRailTileButton(tile, dark);
                button->setObjectName(QStringLiteral("packageRailTile-%1-%2")
                                          .arg(group.packageId, tile.id));
                button->setContextMenuPolicy(Qt::CustomContextMenu);
                const QString packageId = group.packageId;
                const QString tileId = tile.id;
                const bool opensPage = tile.opensPage;
                connect(button, &QAbstractButton::clicked, this,
                        [this, packageId, tileId, opensPage, button] {
                            if (opensPage)
                                openPackageRailTile(packageId, tileId);
                            else
                                showPackageRailMenu(packageId, tileId, button);
                        });
                connect(button, &QWidget::customContextMenuRequested, this,
                        [this, packageId, tileId, button](const QPoint &) {
                            showPackageRailMenu(packageId, tileId, button);
                        });
                m_packageRailLayout->addWidget(button, 0, Qt::AlignHCenter);
            }
        }
        m_packageRailLayout->addStretch(1);
    }
    applyPackageRailVisibility();
}

void MainWindow::openPackageRailTile(const QString &packageId,
                                     const QString &tileId)
{
    for (const NativePackagePage &page : std::as_const(m_nativePackagePages)) {
        if (page.id != packageId || !page.widget)
            continue;
        page.widget->openRailTile(tileId);
        showNativePackage(packageId);
        return;
    }
}

void MainWindow::showPackageRailMenu(const QString &packageId,
                                     const QString &tileId, QWidget *anchor)
{
    QPointer<forkmesh::market::NativePackageWidget> widget;
    for (const NativePackagePage &page : std::as_const(m_nativePackagePages)) {
        if (page.id == packageId)
            widget = page.widget;
    }
    if (!widget || !anchor)
        return;
    NativePackageRailTile tile;
    for (const NativePackageRailTile &candidate : widget->railTiles()) {
        if (candidate.id == tileId)
            tile = candidate;
    }
    if (tile.id.isEmpty())
        return;
    const forkmesh::market::PackageManifest *manifest =
        m_packageRegistry ? m_packageRegistry->package(packageId) : nullptr;
    const QString packageName =
        manifest && !manifest->name.isEmpty() ? manifest->name : packageId;

    QMenu menu(this);
    menu.setObjectName(QStringLiteral("packageRailMenu"));
    menu.setToolTipsVisible(true);
    const auto note = [&menu](const QString &text) {
        menu.addAction(text)->setEnabled(false);
    };
    note(tile.stateLabel.isEmpty()
             ? tile.label
             : tile.label + QString::fromUtf8(" \xC2\xB7 ") + tile.stateLabel);
    if (!tile.count.isEmpty())
        note(tile.countLabel.isEmpty() ? tile.count
                                       : tile.count + QLatin1Char(' ') + tile.countLabel);
    for (const QString &detail : std::as_const(tile.details))
        note(detail);
    if (!tile.steps.isEmpty()) {
        menu.addSection(QStringLiteral("Next steps"));
        for (const NativePackageRailStep &step : std::as_const(tile.steps)) {
            QAction *action = menu.addAction(step.text);
            action->setObjectName(QStringLiteral("packageRailStep-%1").arg(step.id));
            const QString stepId = step.id;
            connect(action, &QAction::triggered, this,
                    [this, widget, packageId, tileId, stepId] {
                        showNativePackage(packageId);
                        if (widget)
                            widget->activateRailStep(tileId, stepId);
                    });
        }
    }
    menu.addSeparator();
    if (tile.opensPage)
        menu.addAction(QStringLiteral("Open %1").arg(tile.label), this,
                       [this, packageId, tileId] {
                           openPackageRailTile(packageId, tileId);
                       });
    menu.addAction(QStringLiteral("Open %1").arg(packageName), this,
                   [this, packageId] { showNativePackage(packageId); });
    QAction *hide = menu.addAction(QStringLiteral("Hide this rail"));
    connect(hide, &QAction::triggered, this, [this] {
        setPackageRailEnabled(false);
        flashMessage(QStringLiteral(
            "Channel status rail hidden. Turn it back on in Settings > Appearance."));
    });
    menu.exec(anchor->mapToGlobal(QPoint(-menu.sizeHint().width() - 4, 0)));
}
