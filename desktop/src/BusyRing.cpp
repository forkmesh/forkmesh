#include "BusyRing.h"

#include "Theme.h"

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QEvent>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QTimer>
#include <QToolButton>
#include <QVector>
#include <QWidget>

#include <algorithm>
#include <utility>

namespace forkmesh::ui::busyring {

namespace {

constexpr int kAutoBusyCapMs = 60000;
const char *const kOverlayName = "fmBusyRing";
const char *const kAutoReleaseName = "fmBusyAutoRelease";

struct Observer {
    QPointer<QObject> context;
    std::function<void()> changed;
};

QVector<Observer> &observers()
{
    static QVector<Observer> list;
    return list;
}

bool &notifyQueued()
{
    static bool queued = false;
    return queued;
}

void notifyObservers()
{
    if (notifyQueued() || !qApp)
        return;
    notifyQueued() = true;
    QTimer::singleShot(0, qApp, [] {
        notifyQueued() = false;
        QVector<Observer> &list = observers();
        list.erase(std::remove_if(list.begin(), list.end(),
                                  [](const Observer &o) { return o.context.isNull(); }),
                   list.end());
        const QVector<Observer> snapshot = list;
        for (const Observer &o : snapshot)
            if (o.context && o.changed)
                o.changed();
    });
}

class Overlay : public QWidget
{
public:
    Overlay(QWidget *host, QWidget *anchor)
        : QWidget(host), m_host(host), m_anchor(anchor)
    {
        setObjectName(QLatin1String(kOverlayName));
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setAttribute(Qt::WA_TranslucentBackground);
        setFocusPolicy(Qt::NoFocus);
        setGeometry(host->rect());
        host->installEventFilter(this);
        m_timer.setInterval(kFrameMs);
        connect(&m_timer, &QTimer::timeout, this, [this] {
            if (isVisible() && !window()->isMinimized())
                update();
        });
        raise();
        show();
    }

    ~Overlay() override { notifyObservers(); }

    void setAnchor(QWidget *anchor)
    {
        m_anchor = anchor;
        update();
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == m_host) {
            if (event->type() == QEvent::Resize)
                setGeometry(m_host->rect());
            else if (event->type() == QEvent::ChildAdded)
                QTimer::singleShot(0, this, [this] { raise(); });
        }
        return QWidget::eventFilter(watched, event);
    }

    void showEvent(QShowEvent *event) override
    {
        m_timer.start();
        QWidget::showEvent(event);
    }

    void hideEvent(QHideEvent *event) override
    {
        m_timer.stop();
        QWidget::hideEvent(event);
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        const qreal turn = phase();
        if (m_anchor && m_anchor->isVisible()) {
            const QRectF at(m_anchor->mapTo(m_host, QPoint(0, 0)), m_anchor->size());
            const qreal side = qMin(at.width(), at.height());
            paint(p, aroundIcon(QRectF(at.center().x() - side / 2, at.center().y() - side / 2,
                                       side, side)),
                  accent(), turn);
            return;
        }
        const QRectF icon = iconRect(qobject_cast<QAbstractButton *>(m_host.data()));
        if (!icon.isEmpty()) {
            paint(p, aroundIcon(icon), accent(), turn);
            return;
        }
        paint(p, QRectF(rect()).adjusted(1, 1, -1, -1), accent(), turn, 6);
    }

private:
    QPointer<QWidget> m_host;
    QPointer<QWidget> m_anchor;
    QTimer m_timer;
};

QPointer<QWidget> &pressedEditor()
{
    static QPointer<QWidget> editor;
    return editor;
}

bool editorText(const QWidget *widget, bool *empty)
{
    if (const auto *line = qobject_cast<const QLineEdit *>(widget))
        *empty = line->text().isEmpty();
    else if (const auto *plain = qobject_cast<const QPlainTextEdit *>(widget))
        *empty = plain->document()->isEmpty();
    else if (const auto *rich = qobject_cast<const QTextEdit *>(widget))
        *empty = rich->document()->isEmpty();
    else
        return false;
    return true;
}

class AutoRelease : public QObject
{
public:
    AutoRelease(QWidget *host, QWidget *editor) : QObject(host), m_host(host)
    {
        setObjectName(QLatin1String(kAutoReleaseName));
        host->installEventFilter(this);
        QTimer::singleShot(kAutoBusyCapMs, this, [this] { release(); });
        const auto releaseWhenEmpty = [this, editor] {
            bool empty = false;
            if (editorText(editor, &empty) && empty)
                release();
        };
        if (auto *line = qobject_cast<QLineEdit *>(editor))
            connect(line, &QLineEdit::textChanged, this, releaseWhenEmpty);
        else if (auto *plain = qobject_cast<QPlainTextEdit *>(editor))
            connect(plain, &QPlainTextEdit::textChanged, this, releaseWhenEmpty);
        else if (auto *rich = qobject_cast<QTextEdit *>(editor))
            connect(rich, &QTextEdit::textChanged, this, releaseWhenEmpty);
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == m_host && event->type() == QEvent::EnabledChange &&
            m_host->isEnabled())
            QTimer::singleShot(0, this, [this] { release(); });
        return QObject::eventFilter(watched, event);
    }

private:
    void release()
    {
        if (m_host)
            setBusy(m_host, false);
    }

    QPointer<QWidget> m_host;
};

bool wantsAutoBusy(const QAbstractButton *button)
{
    if (!button || qobject_cast<const QCheckBox *>(button) ||
        qobject_cast<const QRadioButton *>(button) || button->isCheckable() ||
        button->property("fmNoBusy").toBool() || button->property("fmSpinning").toBool())
        return false;
    if (const auto *push = qobject_cast<const QPushButton *>(button))
        if (push->menu())
            return false;
    if (const auto *tool = qobject_cast<const QToolButton *>(button))
        if (tool->menu() || tool->popupMode() == QToolButton::InstantPopup)
            return false;
    return true;
}

class ClickWatcher : public QObject
{
public:
    explicit ClickWatcher(QObject *parent) : QObject(parent)
    {
        connect(qApp, &QApplication::focusChanged, this, [this](QWidget *old, QWidget *) {
            bool empty = true;
            m_lastEditor = editorText(old, &empty) && !empty ? old : nullptr;
        });
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        const QEvent::Type type = event->type();
        if (type == QEvent::MouseButtonPress && qobject_cast<QAbstractButton *>(watched)) {
            QWidget *focus = QApplication::focusWidget();
            bool empty = true;
            if (editorText(focus, &empty))
                pressedEditor() = empty ? nullptr : focus;
            else
                pressedEditor() = focus == watched ? m_lastEditor.data() : nullptr;
            return false;
        }
        if (type != QEvent::MouseButtonRelease && type != QEvent::KeyRelease)
            return false;
        auto *button = qobject_cast<QAbstractButton *>(watched);
        if (!button || !button->isEnabled() || !wantsAutoBusy(button))
            return false;
        if (type == QEvent::MouseButtonRelease) {
            auto *mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() != Qt::LeftButton ||
                !button->rect().contains(mouse->position().toPoint()))
                return false;
        } else if (static_cast<QKeyEvent *>(event)->key() != Qt::Key_Space) {
            return false;
        }
        watchClick(button);
        return false;
    }

private:
    QPointer<QWidget> m_lastEditor;
};

}

QColor accent()
{
    return QColor(Theme::kRunning);
}

QRectF iconRect(const QAbstractButton *button)
{
    if (!button || button->icon().isNull())
        return QRectF();
    const QSize icon = button->iconSize();
    const QRectF area(button->rect());
    const QString text = button->text();
    const qreal textWidth =
        text.isEmpty() ? 0 : QFontMetrics(button->font()).horizontalAdvance(text);
    const qreal gap = text.isEmpty() ? 0 : 4;
    if (const auto *tool = qobject_cast<const QToolButton *>(button)) {
        const Qt::ToolButtonStyle style = tool->toolButtonStyle();
        if (style == Qt::ToolButtonIconOnly || text.isEmpty())
            return QRectF(area.center().x() - icon.width() / 2.0,
                          area.center().y() - icon.height() / 2.0, icon.width(),
                          icon.height());
        if (style == Qt::ToolButtonTextUnderIcon) {
            const qreal block = icon.height() + 2 + QFontMetrics(button->font()).height();
            return QRectF(area.center().x() - icon.width() / 2.0,
                          area.center().y() - block / 2.0, icon.width(),
                          icon.height());
        }
        if (style == Qt::ToolButtonTextOnly)
            return QRectF();
    }
    const qreal block = icon.width() + gap + textWidth;
    const qreal left = qMax<qreal>(2, area.center().x() - block / 2.0);
    return QRectF(left, area.center().y() - icon.height() / 2.0, icon.width(),
                  icon.height());
}

void setBusy(QWidget *host, bool busy, QWidget *anchor)
{
    if (!host)
        return;
    const bool was = host->property("fmBusy").toBool();
    if (!busy) {
        if (QObject *release = host->findChild<QObject *>(QLatin1String(kAutoReleaseName),
                                                         Qt::FindDirectChildrenOnly))
            release->deleteLater();
    }
    if (was != busy)
        host->setProperty("fmBusy", busy);
    if (!host->property("fmBusyPaintsSelf").toBool()) {
        auto *overlay = static_cast<Overlay *>(host->findChild<QWidget *>(
            QLatin1String(kOverlayName), Qt::FindDirectChildrenOnly));
        if (busy && !overlay)
            new Overlay(host, anchor);
        else if (busy && overlay)
            overlay->setAnchor(anchor);
        else if (!busy && overlay)
            delete overlay;
    }
    if (was != busy)
        notifyObservers();
}

bool isBusy(const QWidget *host)
{
    return host && host->property("fmBusy").toBool();
}

void observe(QObject *context, std::function<void()> changed)
{
    if (!context || !changed)
        return;
    observers().append({QPointer<QObject>(context), std::move(changed)});
}

void watchClick(QAbstractButton *button)
{
    if (!wantsAutoBusy(button))
        return;
    QPointer<QAbstractButton> clicked(button);
    QPointer<QWidget> editor = std::exchange(pressedEditor(), nullptr);
    QTimer::singleShot(0, button, [clicked, editor] {
        if (!clicked)
            return;
        QTimer::singleShot(0, clicked, [clicked, editor] {
            bool cleared = false;
            if (editor && editorText(editor, &cleared) && cleared)
                return;
            if (clicked && !clicked->isEnabled() && clicked->isVisible() &&
                !isBusy(clicked)) {
                setBusy(clicked, true);
                new AutoRelease(clicked, editor);
            }
        });
    });
}

void installClickWatcher()
{
    static QPointer<ClickWatcher> watcher;
    if (watcher || !qApp)
        return;
    watcher = new ClickWatcher(qApp);
    qApp->installEventFilter(watcher);
}

}
