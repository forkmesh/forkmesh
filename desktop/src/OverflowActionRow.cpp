#include "OverflowActionRow.h"

#include <QAbstractButton>
#include <QAction>
#include <QComboBox>
#include <QEvent>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QResizeEvent>
#include <QSettings>
#include <QTextDocumentFragment>

#include <algorithm>

namespace forkmesh::ui {

namespace {

constexpr int kParkedAt = -20000;

QString plainText(const QString &text)
{
    return Qt::mightBeRichText(text)
               ? QTextDocumentFragment::fromHtml(text).toPlainText().simplified()
               : text.simplified();
}

}

OverflowActionRow::OverflowActionRow(const QString &usageKey, QWidget *parent)
    : QWidget(parent), m_usageKey(usageKey)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_pending.setSingleShot(true);
    m_pending.setInterval(0);
    connect(&m_pending, &QTimer::timeout, this, [this] { relayout(); });
    auto *more = new QPushButton(QString::fromUtf8("\xE2\x8B\xAF"));
    more->setObjectName(QStringLiteral("overflowMoreButton"));
    more->setFlat(true);
    more->setCursor(Qt::PointingHandCursor);
    more->setFixedSize(28, 28);
    setMoreButton(more);
}

void OverflowActionRow::setMoreButton(QAbstractButton *button)
{
    if (!button || button == m_more)
        return;
    if (m_more)
        m_more->deleteLater();
    m_more = button;
    button->setParent(this);
    button->setToolTip(QStringLiteral("More actions"));
    button->setAccessibleName(QStringLiteral("More actions"));
    if (auto *push = qobject_cast<QPushButton *>(button))
        push->setCheckable(false);
    button->hide();
    connect(button, &QAbstractButton::clicked, this, [this] { showMenu(); });
    m_pending.start();
}

void OverflowActionRow::setIconProvider(std::function<QIcon(const QString &)> provider)
{
    m_iconProvider = std::move(provider);
}

void OverflowActionRow::addItem(QWidget *widget, int rank, const QString &icon,
                                const QString &menuLabel)
{
    if (!widget)
        return;
    const bool explicitlyHidden =
        widget->isHidden() && widget->testAttribute(Qt::WA_WState_ExplicitShowHide);
    widget->setParent(this);
    widget->setVisible(!explicitlyHidden);
    widget->installEventFilter(this);
    Item item;
    item.widget = widget;
    item.rank = rank;
    item.right = m_stretchAdded;
    item.icon = icon;
    item.label = menuLabel;
    QString name = widget->objectName();
    if (name.isEmpty())
        if (auto *button = qobject_cast<QAbstractButton *>(widget))
            name = button->text();
    if (name.isEmpty())
        name = menuLabel;
    item.key = name;
    item.uses = settingKey(item).isEmpty()
                    ? 0
                    : QSettings().value(settingKey(item), 0).toInt();
    if (auto *button = qobject_cast<QAbstractButton *>(widget)) {
        const QPointer<QWidget> watched(widget);
        connect(button, &QAbstractButton::clicked, this, [this, watched] {
            if (watched)
                noteUse(watched);
        });
    }
    m_items.append(item);
    updateGeometry();
    m_pending.start();
}

void OverflowActionRow::addPinned(QWidget *widget)
{
    addItem(widget, -1);
    if (!m_items.isEmpty() && m_items.last().widget == widget)
        m_items.last().pinned = true;
}

void OverflowActionRow::addStretch()
{
    m_stretchAdded = true;
}

void OverflowActionRow::setInlineLimit(int limit)
{
    m_limit = qMax(0, limit);
    updateGeometry();
    m_pending.start();
}

void OverflowActionRow::setSpacing(int spacing)
{
    m_spacing = qMax(0, spacing);
    updateGeometry();
    m_pending.start();
}

QString OverflowActionRow::settingKey(const Item &item) const
{
    if (m_usageKey.isEmpty() || item.key.isEmpty())
        return QString();
    QString key = item.key;
    key.replace(QLatin1Char('/'), QLatin1Char('_'));
    return m_usageKey + QLatin1Char('/') + key;
}

void OverflowActionRow::noteUse(QWidget *widget)
{
    for (Item &item : m_items) {
        if (item.widget != widget)
            continue;
        ++item.uses;
        const QString key = settingKey(item);
        if (!key.isEmpty())
            QSettings().setValue(key, item.uses);
        break;
    }
    m_pending.start();
}

int OverflowActionRow::usesOf(const QWidget *widget) const
{
    for (const Item &item : m_items)
        if (item.widget == widget)
            return item.uses;
    return 0;
}

QSize OverflowActionRow::itemSize(const Item &item) const
{
    if (!item.widget)
        return QSize();
    QSize size = item.widget->sizeHint();
    size = size.expandedTo(item.widget->minimumSize());
    size = size.boundedTo(item.widget->maximumSize());
    if (size.width() < 0 || size.height() < 0)
        size = item.widget->size();
    return size;
}

QList<int> OverflowActionRow::chooseInline(int width, QList<int> *overflow) const
{
    QList<int> pinned;
    QList<int> ranked;
    for (int index = 0; index < m_items.size(); ++index) {
        const Item &item = m_items.at(index);
        if (!item.widget || item.widget->isHidden())
            continue;
        (item.pinned ? pinned : ranked).append(index);
    }
    std::stable_sort(ranked.begin(), ranked.end(), [this](int a, int b) {
        const Item &left = m_items.at(a);
        const Item &right = m_items.at(b);
        if (left.uses != right.uses)
            return left.uses > right.uses;
        return left.rank < right.rank;
    });
    QList<int> chosen = ranked.mid(0, m_limit);
    QList<int> spilled = ranked.mid(qMin<int>(m_limit, ranked.size()));
    const int moreWidth = m_more ? m_more->sizeHint().expandedTo(
                                       m_more->minimumSize()).boundedTo(
                                       m_more->maximumSize()).width()
                                 : 0;
    const auto total = [&] {
        int sum = 0;
        int count = 0;
        for (int index : pinned) {
            sum += itemSize(m_items.at(index)).width();
            ++count;
        }
        for (int index : chosen) {
            sum += itemSize(m_items.at(index)).width();
            ++count;
        }
        if (!spilled.isEmpty()) {
            sum += moreWidth;
            ++count;
        }
        return sum + qMax(0, count - 1) * m_spacing;
    };
    if (width >= 0) {
        while (!chosen.isEmpty() && total() > width)
            spilled.prepend(chosen.takeLast());
    }
    if (overflow) {
        std::sort(spilled.begin(), spilled.end());
        *overflow = spilled;
    }
    QList<int> shown = pinned + chosen;
    std::sort(shown.begin(), shown.end());
    return shown;
}

void OverflowActionRow::relayout()
{
    if (m_placing)
        return;
    m_placing = true;
    const QRect area = contentsRect();
    QList<int> spilled;
    const QList<int> shown = chooseInline(area.width(), &spilled);
    const auto place = [area](QWidget *widget, int x, const QSize &size) {
        const QRect target(x,
                           area.top() + qMax(0, (area.height() - size.height()) / 2),
                           size.width(), size.height());
        if (widget->geometry() != target)
            widget->setGeometry(target);
    };
    int left = area.left();
    QList<int> rightSide;
    for (int index : shown) {
        const Item &item = m_items.at(index);
        if (m_stretchAdded && !item.right) {
            const QSize size = itemSize(item);
            place(item.widget, left, size);
            left += size.width() + m_spacing;
        } else {
            rightSide.append(index);
        }
    }
    int right = area.right() + 1;
    if (m_more) {
        m_more->setVisible(!spilled.isEmpty());
        if (!spilled.isEmpty()) {
            const QSize size = m_more->sizeHint()
                                   .expandedTo(m_more->minimumSize())
                                   .boundedTo(m_more->maximumSize());
            right -= size.width();
            place(m_more, right, size);
            right -= m_spacing;
        }
    }
    for (auto it = rightSide.crbegin(); it != rightSide.crend(); ++it) {
        const Item &item = m_items.at(*it);
        const QSize size = itemSize(item);
        right -= size.width();
        place(item.widget, right, size);
        right -= m_spacing;
    }
    for (int index : spilled) {
        QWidget *widget = m_items.at(index).widget;
        if (widget->pos() != QPoint(kParkedAt, kParkedAt))
            widget->move(kParkedAt, kParkedAt);
    }
    m_placing = false;
}

QList<QWidget *> OverflowActionRow::inlineWidgets() const
{
    QList<QWidget *> widgets;
    for (int index : chooseInline(contentsRect().width(), nullptr))
        widgets.append(m_items.at(index).widget);
    return widgets;
}

QList<QWidget *> OverflowActionRow::overflowWidgets() const
{
    QList<int> spilled;
    chooseInline(contentsRect().width(), &spilled);
    QList<QWidget *> widgets;
    for (int index : spilled)
        widgets.append(m_items.at(index).widget);
    return widgets;
}

bool OverflowActionRow::isInline(const QWidget *widget) const
{
    for (int index : chooseInline(contentsRect().width(), nullptr))
        if (m_items.at(index).widget == widget)
            return true;
    return false;
}

void OverflowActionRow::fillMenu(QMenu *menu) const
{
    if (!menu)
        return;
    QList<int> spilled;
    chooseInline(contentsRect().width(), &spilled);
    for (int index : spilled) {
        const Item &item = m_items.at(index);
        QWidget *widget = item.widget;
        if (!widget)
            continue;
        const QIcon icon = m_iconProvider && !item.icon.isEmpty()
                               ? m_iconProvider(item.icon)
                               : QIcon();
        if (auto *combo = qobject_cast<QComboBox *>(widget)) {
            QMenu *sub = menu->addMenu(
                icon, item.label.isEmpty() ? combo->currentText() : item.label);
            sub->setEnabled(combo->isEnabled());
            const QPointer<QComboBox> target(combo);
            for (int row = 0; row < combo->count(); ++row) {
                QAction *action = sub->addAction(combo->itemText(row));
                action->setCheckable(true);
                action->setChecked(row == combo->currentIndex());
                connect(action, &QAction::triggered, combo, [target, row] {
                    if (target)
                        target->setCurrentIndex(row);
                });
            }
            continue;
        }
        if (auto *label = qobject_cast<QLabel *>(widget)) {
            const QString text = plainText(label->text());
            if (text.isEmpty())
                continue;
            QAction *action = menu->addAction(icon, text);
            action->setEnabled(false);
            continue;
        }
        auto *button = qobject_cast<QAbstractButton *>(widget);
        if (!button)
            continue;
        QString text = item.label.isEmpty() ? button->text() : item.label;
        if (text.isEmpty())
            text = button->accessibleName();
        if (text.isEmpty())
            continue;
        auto *push = qobject_cast<QPushButton *>(button);
        if (push && push->menu()) {
            QAction *sub = menu->addMenu(push->menu());
            sub->setText(text);
            sub->setIcon(icon);
            continue;
        }
        QAction *action = menu->addAction(icon, text);
        action->setEnabled(button->isEnabled());
        action->setToolTip(button->toolTip());
        if (button->isCheckable()) {
            action->setCheckable(true);
            action->setChecked(button->isChecked());
        }
        const QPointer<QAbstractButton> target(button);
        connect(action, &QAction::triggered, button, [target] {
            if (target)
                target->click();
        });
    }
}

void OverflowActionRow::showMenu()
{
    if (!m_more)
        return;
    QMenu menu(this);
    menu.setObjectName(QStringLiteral("overflowActionMenu"));
    menu.setToolTipsVisible(true);
    fillMenu(&menu);
    if (menu.isEmpty())
        return;
    const QSize hint = menu.sizeHint();
    menu.exec(m_more->mapToGlobal(
        QPoint(m_more->width() - hint.width(), m_more->height() + 2)));
}

QSize OverflowActionRow::sizeHint() const
{
    QList<int> spilled;
    const QList<int> shown = chooseInline(-1, &spilled);
    int width = 0;
    int height = m_more ? m_more->sizeHint().height() : 0;
    for (int index : shown) {
        const QSize size = itemSize(m_items.at(index));
        width += size.width();
        height = qMax(height, size.height());
    }
    int count = shown.size();
    if (!spilled.isEmpty() && m_more) {
        width += m_more->sizeHint().expandedTo(m_more->minimumSize()).width();
        ++count;
    }
    const QMargins margins = contentsMargins();
    return QSize(width + qMax(0, count - 1) * m_spacing + margins.left() +
                     margins.right(),
                 height + margins.top() + margins.bottom());
}

QSize OverflowActionRow::minimumSizeHint() const
{
    int width = 0;
    int count = 0;
    bool anyRanked = false;
    int height = m_more ? m_more->sizeHint().height() : 0;
    for (const Item &item : m_items) {
        if (!item.widget || item.widget->isHidden())
            continue;
        const QSize size = itemSize(item);
        height = qMax(height, size.height());
        if (!item.pinned) {
            anyRanked = true;
            continue;
        }
        width += size.width();
        ++count;
    }
    if (anyRanked && m_more) {
        width += m_more->sizeHint().expandedTo(m_more->minimumSize()).width();
        ++count;
    }
    const QMargins margins = contentsMargins();
    return QSize(width + qMax(0, count - 1) * m_spacing + margins.left() +
                     margins.right(),
                 height + margins.top() + margins.bottom());
}

bool OverflowActionRow::event(QEvent *event)
{
    if (event->type() == QEvent::LayoutRequest) {
        updateGeometry();
        m_pending.start();
    }
    return QWidget::event(event);
}

bool OverflowActionRow::eventFilter(QObject *watched, QEvent *event)
{
    switch (event->type()) {
    case QEvent::ShowToParent:
    case QEvent::HideToParent:
        updateGeometry();
        m_pending.start();
        break;
    default:
        break;
    }
    return QWidget::eventFilter(watched, event);
}

void OverflowActionRow::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    m_pending.start();
}

void OverflowActionRow::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    relayout();
}

}
