#include "MainWindow.h"
#include "MainWindowInternal.h"

#include <QAbstractButton>
#include <QApplication>
#include <QButtonGroup>
#include <QDrag>
#include <QDropEvent>
#include <QFrame>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPushButton>
#include <QSettings>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

using namespace forkmesh::ui;

namespace {

const QString kRailShortcutsSetting = QStringLiteral("ui/railShortcuts");
const QString kKnownPageButtonsSetting = QStringLiteral("ui/pageButtons");
const QString kPageButtonMimeType =
    QStringLiteral("application/x-forkmesh-page-button");
constexpr qint64 kPageProbeWindowMs = 1500;

QString escapeSegmentName(QString name)
{
    return name.replace(QLatin1Char('%'), QStringLiteral("%25"))
        .replace(QLatin1Char('/'), QStringLiteral("%2F"));
}

QString unescapeSegmentName(QString name)
{
    return name.replace(QStringLiteral("%2F"), QStringLiteral("/"))
        .replace(QStringLiteral("%25"), QStringLiteral("%"));
}

QList<QWidget *> childWidgets(QWidget *parent)
{
    QList<QWidget *> widgets;
    for (QObject *object : parent->children())
        if (auto *widget = qobject_cast<QWidget *>(object))
            widgets.append(widget);
    return widgets;
}

QString pathSegment(QWidget *parent, QWidget *child)
{
    if (auto *stack = qobject_cast<QStackedWidget *>(parent)) {
        const int index = stack->indexOf(child);
        if (index >= 0)
            return QStringLiteral("@%1").arg(index);
    }
    const QString name = child->objectName();
    const QByteArray className = child->metaObject()->className();
    int sameName = 0;
    int classIndex = 0;
    bool reached = false;
    for (QWidget *sibling : childWidgets(parent)) {
        if (!name.isEmpty() && sibling->objectName() == name)
            ++sameName;
        if (sibling == child)
            reached = true;
        else if (!reached && className == sibling->metaObject()->className())
            ++classIndex;
    }
    if (!name.isEmpty() && sameName == 1)
        return QStringLiteral("n:") + escapeSegmentName(name);
    return QStringLiteral("c:%1#%2")
        .arg(QString::fromLatin1(className))
        .arg(classIndex);
}

QWidget *childAtSegment(QWidget *parent, const QString &segment)
{
    if (!parent)
        return nullptr;
    if (segment.startsWith(QLatin1Char('@'))) {
        auto *stack = qobject_cast<QStackedWidget *>(parent);
        bool ok = false;
        const int index = segment.mid(1).toInt(&ok);
        return stack && ok ? stack->widget(index) : nullptr;
    }
    if (segment.startsWith(QLatin1String("n:"))) {
        const QString name = unescapeSegmentName(segment.mid(2));
        for (QWidget *child : childWidgets(parent))
            if (child->objectName() == name)
                return child;
        return nullptr;
    }
    if (!segment.startsWith(QLatin1String("c:")))
        return nullptr;
    const int hash = segment.lastIndexOf(QLatin1Char('#'));
    if (hash < 0)
        return nullptr;
    const QByteArray className = segment.mid(2, hash - 2).toLatin1();
    bool ok = false;
    const int index = segment.mid(hash + 1).toInt(&ok);
    if (!ok)
        return nullptr;
    int seen = 0;
    for (QWidget *child : childWidgets(parent)) {
        if (className != child->metaObject()->className())
            continue;
        if (seen++ == index)
            return child;
    }
    return nullptr;
}

QString pageButtonLabel(const QAbstractButton *button)
{
    for (const QString &candidate :
         {button->text(), button->accessibleName(), button->toolTip(),
          button->objectName()}) {
        const QString label = RailShortcuts::normalizeLabel(candidate);
        if (!label.isEmpty())
            return label;
    }
    return QString();
}

int stackIndexInPath(const QString &path, const QString &stackPath)
{
    if (path.isEmpty() || stackPath.isEmpty() ||
        !path.startsWith(stackPath + QStringLiteral("/@")))
        return -1;
    const QString rest = path.mid(stackPath.size() + 2);
    bool ok = false;
    const int index = rest.section(QLatin1Char('/'), 0, 0).toInt(&ok);
    return ok ? index : -1;
}

bool largeEnoughForPage(const QWidget *stack, const QWidget *window)
{
    const qint64 stackArea = qint64(stack->width()) * stack->height();
    const qint64 windowArea = qint64(window->width()) * window->height();
    return windowArea > 0 && stackArea * 5 >= windowArea;
}

}

RailShortcuts::State &MainWindow::railShortcutState()
{
    if (!m_railShortcutStateLoaded) {
        m_railShortcutStateLoaded = true;
        m_railShortcutState = RailShortcuts::parse(
            QSettings().value(kRailShortcutsSetting).toString());
    }
    return m_railShortcutState;
}

void MainWindow::saveRailShortcutState()
{
    QSettings settings;
    if (railShortcutState().isEmpty())
        settings.remove(kRailShortcutsSetting);
    else
        settings.setValue(kRailShortcutsSetting,
                          RailShortcuts::serialize(railShortcutState()));
}

QStringList &MainWindow::knownPageButtons()
{
    if (!m_knownPageButtonsLoaded) {
        m_knownPageButtonsLoaded = true;
        m_knownPageButtons =
            QSettings().value(kKnownPageButtonsSetting).toStringList();
    }
    return m_knownPageButtons;
}

void MainWindow::syncRailShortcutTiles()
{
    if (!m_appNavigationRailLayout)
        return;
    QWidget *railContent = m_appNavigationRailLayout->parentWidget();
    QSet<QString> live;
    for (const RailShortcuts::Shortcut &shortcut : railShortcutState()) {
        live.insert(shortcut.id);
        ActivityRailButton *tile = m_railShortcutButtons.value(shortcut.id);
        if (tile) {
            tile->setGlyph(shortcut.icon, shortcut.label);
            continue;
        }
        tile = new ActivityRailButton(shortcut.icon, shortcut.label, railContent);
        tile->setObjectName(QStringLiteral("railShortcutButton"));
        tile->setCheckable(false);
        tile->setProperty("forkmeshRailShortcut", shortcut.id);
        tile->setToolTip(QStringLiteral("%1\nOpens the page its button opens. "
                                        "Right-click to remove it from the rail.")
                             .arg(shortcut.label));
        tile->followRailCaptionPreference();
        const QString id = shortcut.id;
        connect(tile, &QPushButton::clicked, this, [this, id] {
            QTimer::singleShot(0, this, [this, id] { activateRailShortcut(id); });
        });
        m_railShortcutButtons.insert(shortcut.id, tile);
        m_appNavigationRailLayout->addWidget(tile, 0, Qt::AlignLeft);
    }
    refreshRailShortcutBusy();
    for (auto it = m_railShortcutButtons.begin(); it != m_railShortcutButtons.end();) {
        if (live.contains(it.key()) && it.value()) {
            ++it;
            continue;
        }
        if (QWidget *tile = it.value()) {
            tile->hide();
            m_appNavigationRailLayout->removeWidget(tile);
            tile->setParent(nullptr);
            tile->deleteLater();
        }
        it = m_railShortcutButtons.erase(it);
    }
}

void MainWindow::removeRailShortcut(const QString &id)
{
    if (!RailShortcuts::remove(railShortcutState(), id))
        return;
    const QString key = RailShortcuts::railKey(id);
    if (RailFolders::move(railFolderState(), key, QString()))
        saveRailFolderState();
    QSettings settings;
    QStringList order = settings.value(kRailOrderSetting).toStringList();
    if (order.removeAll(key) > 0)
        settings.setValue(kRailOrderSetting, order);
    saveRailShortcutState();
    applyRailOrder();
}

QString MainWindow::pageButtonPath(QWidget *button) const
{
    if (!button)
        return QString();
    QStringList segments;
    QWidget *widget = button;
    while (widget && widget != this) {
        bool poppedOut = false;
        for (const PoppedOutPage &out : m_poppedOutPages) {
            if (out.page == widget && out.stack && out.placeholder) {
                segments.prepend(
                    QStringLiteral("@%1").arg(out.stack->indexOf(out.placeholder)));
                widget = out.stack;
                poppedOut = true;
                break;
            }
        }
        if (poppedOut)
            continue;
        QWidget *parent = widget->parentWidget();
        if (!parent)
            return QString();
        segments.prepend(pathSegment(parent, widget));
        widget = parent;
    }
    return widget == this ? segments.join(QLatin1Char('/')) : QString();
}

QAbstractButton *MainWindow::pageButtonAtPath(const QString &path) const
{
    if (path.isEmpty())
        return nullptr;
    QWidget *widget = const_cast<MainWindow *>(this);
    for (const QString &segment : path.split(QLatin1Char('/'))) {
        QWidget *next = childAtSegment(widget, segment);
        for (const PoppedOutPage &out : m_poppedOutPages) {
            if (next && out.placeholder == next && out.page) {
                next = out.page;
                break;
            }
        }
        if (!next)
            return nullptr;
        widget = next;
    }
    return qobject_cast<QAbstractButton *>(widget);
}

QAbstractButton *MainWindow::pageButtonForShortcut(
    const RailShortcuts::Shortcut &shortcut) const
{
    if (QAbstractButton *button = pageButtonAtPath(shortcut.path))
        return button;
    if (shortcut.section < 0 || !m_sectionStack)
        return nullptr;
    QWidget *scope = m_sectionStack->widget(shortcut.section);
    for (const PoppedOutPage &out : m_poppedOutPages) {
        if (scope && out.placeholder == scope && out.page) {
            scope = out.page;
            break;
        }
    }
    if (!scope)
        return nullptr;
    for (QAbstractButton *button : scope->findChildren<QAbstractButton *>()) {
        if (pageButtonLabel(button) == shortcut.label &&
            isPageButtonCandidate(button))
            return button;
    }
    return nullptr;
}

bool MainWindow::isPageButtonCandidate(QAbstractButton *button) const
{
    if (!button)
        return false;
    auto *push = qobject_cast<QPushButton *>(button);
    auto *tool = qobject_cast<QToolButton *>(button);
    if ((!push && !tool) || (push && push->menu()) || (tool && tool->menu()))
        return false;
    if (button == m_navBackButton || button == m_navForwardButton ||
        button->property("forkmeshRailShortcut").isValid() ||
        !button->property("forkmeshRailFolderId").toString().isEmpty() ||
        isInRail(button))
        return false;
    QWidget *top = button->window();
    if (top != this &&
        top->objectName() != QLatin1String("poppedOutPageWindow"))
        return false;
    QWidget *railContent = m_appNavigationRailLayout
                               ? m_appNavigationRailLayout->parentWidget()
                               : nullptr;
    for (QWidget *widget = button->parentWidget(); widget && widget != top;
         widget = widget->parentWidget()) {
        if (widget == railContent || widget == m_appNavigationRail ||
            widget->objectName() == QLatin1String("railFolderBody") ||
            widget->objectName() == QLatin1String("poppedOutPagePlaceholder"))
            return false;
    }
    return true;
}

bool MainWindow::isDraggablePageButton(QAbstractButton *button)
{
    if (!isPageButtonCandidate(button))
        return false;
    if (!button->property("forkmeshButtonKey").toString().isEmpty())
        return true;
    if (button->isCheckable() && button->group() && button->group()->exclusive())
        return true;
    return knownPageButtons().contains(pageButtonPath(button));
}

void MainWindow::notePageButtonNavigated()
{
    QAbstractButton *button = m_pageProbeButton;
    if (!button || !m_pageProbeClock.isValid() ||
        m_pageProbeClock.elapsed() > kPageProbeWindowMs)
        return;
    m_pageProbeButton.clear();
    if (!isPageButtonCandidate(button))
        return;
    if (RailShortcuts::rememberPageButton(knownPageButtons(),
                                          pageButtonPath(button)))
        QSettings().setValue(kKnownPageButtonsSetting, knownPageButtons());
}

bool MainWindow::handlePageButtonEvent(QObject *watched, QEvent *event)
{
    const QEvent::Type type = event->type();
    if (type == QEvent::ShowToParent) {
        if (!m_pageProbeButton)
            return false;
        auto *page = qobject_cast<QWidget *>(watched);
        auto *stack = page ? qobject_cast<QStackedWidget *>(page->parentWidget())
                           : nullptr;
        if (!stack || stack == m_stack)
            return false;
        QWidget *top = stack->window();
        if ((top == this ||
             top->objectName() == QLatin1String("poppedOutPageWindow")) &&
            largeEnoughForPage(stack, top))
            notePageButtonNavigated();
        return false;
    }
    if (type == QEvent::KeyPress) {
        m_pageProbeButton.clear();
        return false;
    }
    if (type == QEvent::MouseMove) {
        if (!m_pageDragSource || watched != m_pageDragSource)
            return false;
    } else if (type == QEvent::MouseButtonPress ||
               type == QEvent::MouseButtonRelease) {
        m_pageProbeButton.clear();
        if (!watched->isWidgetType())
            return false;
    } else {
        return false;
    }
    auto *button = qobject_cast<QAbstractButton *>(watched);
    if (!button)
        return false;
    auto *mouse = static_cast<QMouseEvent *>(event);
    const QPoint position = mouse->position().toPoint();
    if (type == QEvent::MouseButtonPress) {
        m_pageDragSource.clear();
        if (mouse->button() == Qt::LeftButton && isDraggablePageButton(button)) {
            m_pageDragSource = button;
            m_pageDragStart = position;
        }
        return false;
    }
    if (type == QEvent::MouseButtonRelease) {
        if (mouse->button() != Qt::LeftButton)
            return false;
        m_pageDragSource.clear();
        if (button->rect().contains(position) && isPageButtonCandidate(button)) {
            m_pageProbeButton = button;
            m_pageProbeClock.start();
        }
        return false;
    }
    if (!(mouse->buttons() & Qt::LeftButton) ||
        (position - m_pageDragStart).manhattanLength() <
            QApplication::startDragDistance())
        return false;
    m_pageDragSource.clear();
    auto *mime = new QMimeData;
    mime->setData(kPageButtonMimeType, pageButtonPath(button).toUtf8());
    auto *drag = new QDrag(button);
    drag->setMimeData(mime);
    drag->setPixmap(button->grab());
    drag->setHotSpot(m_pageDragStart);
    QPointer<QAbstractButton> source(button);
    m_pageDragButton = button;
    drag->exec(Qt::CopyAction | Qt::MoveAction, Qt::CopyAction);
    m_pageDragButton.clear();
    hideRailDropIndicator();
    if (source)
        source->setDown(false);
    return true;
}

bool MainWindow::handleRailPageButtonDrag(QEvent *event)
{
    const QEvent::Type type = event->type();
    if (type == QEvent::DragLeave) {
        hideRailDropIndicator();
        return false;
    }
    if (type != QEvent::DragEnter && type != QEvent::DragMove &&
        type != QEvent::Drop)
        return false;
    auto *drop = static_cast<QDropEvent *>(event);
    if (!drop->mimeData() || !drop->mimeData()->hasFormat(kPageButtonMimeType) ||
        !m_pageDragButton)
        return false;
    const int y = drop->position().toPoint().y();
    drop->setDropAction(Qt::CopyAction);
    drop->accept();
    if (type != QEvent::Drop) {
        showRailDropIndicator(y);
        return true;
    }
    hideRailDropIndicator();
    QPointer<QAbstractButton> button = m_pageDragButton;
    QTimer::singleShot(0, this, [this, button, y] {
        if (button)
            dropPageButtonOnRail(button, y);
    });
    return true;
}

int MainWindow::railInsertIndexAt(int y) const
{
    if (!m_appNavigationRailLayout)
        return 0;
    int target = 0;
    for (int i = 0; i < m_appNavigationRailLayout->count(); ++i) {
        QWidget *widget = m_appNavigationRailLayout->itemAt(i)->widget();
        if (widget && !widget->isHidden() && widget->geometry().center().y() < y)
            target = i + 1;
    }
    return target;
}

void MainWindow::showRailDropIndicator(int y)
{
    QWidget *content = m_appNavigationRailLayout
                           ? m_appNavigationRailLayout->parentWidget()
                           : nullptr;
    if (!content)
        return;
    if (!m_railDropIndicator) {
        auto *line = new QFrame(content);
        line->setObjectName(QStringLiteral("railDropIndicator"));
        line->setAttribute(Qt::WA_TransparentForMouseEvents);
        line->setStyleSheet(Theme::remapSheet(QStringLiteral("background: #2ea043; border: none;")));
        m_railDropIndicator = line;
    }
    const int at = railInsertIndexAt(y);
    int lineY = 0;
    for (int i = 0; i < at && i < m_appNavigationRailLayout->count(); ++i) {
        QWidget *widget = m_appNavigationRailLayout->itemAt(i)->widget();
        if (widget && !widget->isHidden())
            lineY = widget->geometry().bottom() + 1;
    }
    m_railDropIndicator->setGeometry(2, qMax(0, lineY - 1),
                                     qMax(8, content->width() - 4), 2);
    m_railDropIndicator->raise();
    m_railDropIndicator->show();
}

void MainWindow::hideRailDropIndicator()
{
    if (m_railDropIndicator)
        m_railDropIndicator->hide();
}

QString MainWindow::dropPageButtonOnRail(QAbstractButton *button, int y)
{
    if (!button || !m_appNavigationRailLayout || !isDraggablePageButton(button))
        return QString();
    const int at = railInsertIndexAt(y);
    QStringList before;
    QStringList after;
    for (int i = 0; i < m_appNavigationRailLayout->count(); ++i) {
        QWidget *widget = m_appNavigationRailLayout->itemAt(i)->widget();
        const QString key =
            widget ? widget->property("forkmeshRailKey").toString() : QString();
        if (!key.isEmpty())
            (i < at ? before : after).append(key);
    }

    QString id = button->property("forkmeshButtonKey").toString();
    QString key;
    if (!id.isEmpty()) {
        storeRailItemHidden(id, false);
        moveButtonToPanel(id, kButtonPanelRail);
        if (!isInRail(button))
            return QString();
        key = button->property("forkmeshRailKey").toString();
        if (key.isEmpty())
            key = id;
    } else {
        RailShortcuts::Shortcut shortcut;
        shortcut.path = pageButtonPath(button);
        shortcut.label = pageButtonLabel(button);
        shortcut.icon = button->property("forkmeshOcticon").toString();
        shortcut.section =
            stackIndexInPath(shortcut.path, pageButtonPath(m_sectionStack));
        if (shortcut.section == 0)
            shortcut.repoTab =
                stackIndexInPath(shortcut.path, pageButtonPath(m_repoDetailStack));
        id = RailShortcuts::add(railShortcutState(), shortcut);
        if (id.isEmpty()) {
            showPageNotice(QStringLiteral("The left rail holds at most %1 shortcuts. "
                                          "Remove one to add another.")
                               .arg(RailShortcuts::kMaxShortcuts),
                           false);
            return QString();
        }
        saveRailShortcutState();
        applyRailOrder();
        applyRailCaptionPreference();
        key = RailShortcuts::railKey(id);
    }
    before.removeAll(key);
    after.removeAll(key);
    saveRailOrder(before + QStringList{key} + after);
    return id;
}

void MainWindow::activateRailShortcut(const QString &id)
{
    const int index = RailShortcuts::indexOf(railShortcutState(), id);
    if (index < 0)
        return;
    const RailShortcuts::Shortcut shortcut = railShortcutState().at(index);
    if (shortcut.repoTab >= 0 && m_repoDetailIndex < 0) {
        showPageNotice(QStringLiteral("Open a repository first: %1 is on a "
                                      "repository page.")
                           .arg(shortcut.label),
                       false);
        return;
    }
    if (shortcut.section >= 0 && m_sectionStack &&
        m_sectionStack->currentIndex() != shortcut.section)
        showSection(shortcut.section);
    if (shortcut.repoTab >= 0 && m_repoDetailTabs && m_repoDetailStack &&
        m_repoDetailStack->currentIndex() != shortcut.repoTab) {
        if (QAbstractButton *tab = m_repoDetailTabs->button(shortcut.repoTab))
            tab->click();
    }
    if (QAbstractButton *button = pageButtonForShortcut(shortcut)) {
        clickPageButton(button);
        return;
    }
    QTimer::singleShot(0, this, [this, shortcut] {
        if (QAbstractButton *button = pageButtonForShortcut(shortcut))
            clickPageButton(button);
        else
            showPageNotice(QStringLiteral("%1 is not on its page any more. "
                                          "Right-click the shortcut to remove it.")
                               .arg(shortcut.label),
                           false);
    });
}

void MainWindow::clickPageButton(QAbstractButton *button)
{
    if (!button)
        return;
    QWidget *top = button->window();
    if (!button->isVisibleTo(top)) {
        QWidget *child = button;
        for (QWidget *parent = button->parentWidget(); parent && parent != top;
             child = parent, parent = parent->parentWidget()) {
            auto *stack = qobject_cast<QStackedWidget *>(parent);
            if (!stack || stack == m_sectionStack || stack == m_repoDetailStack ||
                stack == m_stack || stack->currentWidget() == child)
                continue;
            if (auto *tabs = qobject_cast<QTabWidget *>(stack->parentWidget()))
                tabs->setCurrentWidget(child);
            else
                stack->setCurrentWidget(child);
        }
    }
    if (top != this) {
        top->setWindowState(top->windowState() & ~Qt::WindowMinimized);
        top->show();
        top->raise();
        top->activateWindow();
    }
    forkmesh::ui::busyring::watchClick(button);
    button->click();
}

void MainWindow::refreshRailShortcutBusy()
{
    for (const RailShortcuts::Shortcut &shortcut : railShortcutState()) {
        ActivityRailButton *tile = m_railShortcutButtons.value(shortcut.id);
        if (tile)
            forkmesh::ui::busyring::setBusy(
                tile, forkmesh::ui::busyring::isBusy(pageButtonForShortcut(shortcut)));
    }
}

#ifdef FORKMESH_WINDOW_TESTS
void MainWindow::testClickPageButton(QAbstractButton *button)
{
    m_pageProbeButton = button;
    m_pageProbeClock.start();
    button->click();
}

QWidget *MainWindow::testRailShortcutTile(const QString &id) const
{
    return m_railShortcutButtons.value(id);
}
#endif
