#pragma once

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QCursor>
#include <QEvent>
#include <QFont>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QObject>
#include <QPainter>
#include <QPalette>
#include <QPoint>
#include <QPointer>
#include <QRect>
#include <QScreen>
#include <QString>
#include <QStringList>
#include <QTextDocument>
#include <QTextOption>
#include <QTimer>
#include <QWidget>
#include <QWindow>

#include <cmath>

namespace Tooltips {

// A bounded rich-text rendering of `text`: the first `maxLines` lines and at
// most `maxChars` characters, pre-wrapped in a fixed-pitch block, followed by
// a note saying how much was left out. Escapes the shown part, so callers hand
// in raw text. Short text comes back complete with no note.
inline QString preview(const QString &raw, int maxLines = 12, int maxChars = 1000)
{
    QString text = raw;
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    while (!text.isEmpty() && text.back().isSpace())
        text.chop(1);
    if (text.isEmpty())
        return QString(); // no tooltip at all, rather than an empty box

    const int totalLines = text.isEmpty() ? 0 : int(text.count(QLatin1Char('\n'))) + 1;
    qsizetype cut = text.size();
    if (maxLines > 0 && totalLines > maxLines) {
        qsizetype at = -1;
        for (int i = 0; i < maxLines; ++i)
            at = text.indexOf(QLatin1Char('\n'), at + 1);
        cut = qMin(cut, at);
    }
    if (maxChars > 0 && text.size() > maxChars) {
        qsizetype limit = maxChars;
        if (text.at(limit - 1).isHighSurrogate())
            --limit;
        cut = qMin(cut, limit);
    }

    QString shown = text.left(cut);
    while (!shown.isEmpty() && shown.back().isSpace())
        shown.chop(1);
    const bool truncated = cut < text.size();

    QString html = QStringLiteral(
        "<pre style=\"white-space:pre-wrap;margin:0;\">%1</pre>")
        .arg(shown.toHtmlEscaped());
    if (truncated) {
        const int shownLines = shown.isEmpty() ? 0 : int(shown.count(QLatin1Char('\n'))) + 1;
        const int moreLines = totalLines - shownLines;
        const qsizetype moreChars = text.size() - shown.size();
        const QString note = moreLines > 0
            ? QStringLiteral("… %1 more line%2").arg(moreLines).arg(moreLines == 1 ? QString() : QStringLiteral("s"))
            : QStringLiteral("… %1 more character%2").arg(moreChars).arg(moreChars == 1 ? QString() : QStringLiteral("s"));
        html += QStringLiteral("<p style=\"margin:4px 0 0 0;\"><i>%1</i></p>").arg(note);
    }
    return html;
}

// Where a tooltip of size `tip.size()` should sit so that it does not cover
// `pointer`: below, above, right of, or left of it, in that order, inside
// `screen`. When nothing fits, the returned rect is the largest strip below
// or above the pointer and is shorter than the tip; the caller trims the text
// to that height. `cursor` is the logical cursor size the tip must clear.
inline QRect placeBeside(const QRect &tip, const QPoint &pointer,
                         const QRect &screen, int cursor = 32)
{
    const int w = qMin(tip.width(), screen.width());
    const int h = tip.height();
    auto clampX = [&](int x) {
        return qBound(screen.left(), x, screen.right() - w + 1);
    };
    auto clampY = [&](int y, int height) {
        return qBound(screen.top(), y, screen.bottom() - height + 1);
    };

    const int belowTop = pointer.y() + cursor;
    const int belowRoom = screen.bottom() - belowTop + 1;
    if (h <= belowRoom)
        return QRect(clampX(pointer.x() + 2), belowTop, w, h);

    const int aboveBottom = pointer.y() - 8;
    const int aboveRoom = aboveBottom - screen.top() + 1;
    if (h <= aboveRoom)
        return QRect(clampX(pointer.x() + 2), aboveBottom - h + 1, w, h);

    const int fullHeight = qMin(h, screen.height());
    const int rightLeft = pointer.x() + cursor;
    if (rightLeft + w - 1 <= screen.right())
        return QRect(rightLeft, clampY(pointer.y() - fullHeight / 2, fullHeight),
                     w, fullHeight);
    const int leftRight = pointer.x() - 8;
    if (leftRight - w + 1 >= screen.left())
        return QRect(leftRight - w + 1,
                     clampY(pointer.y() - fullHeight / 2, fullHeight), w,
                     fullHeight);

    if (belowRoom >= aboveRoom)
        return QRect(clampX(pointer.x() + 2), belowTop, w, qMax(1, belowRoom));
    return QRect(clampX(pointer.x() + 2), screen.top(), w, qMax(1, aboveRoom));
}

class Window : public QWidget
{
public:
    static constexpr int kMaxWidth = 440;
    static constexpr int kFontPixels = 13;
    static constexpr int kPadX = 9;
    static constexpr int kPadY = 6;

    explicit Window(QWidget *host)
        : QWidget(host, Qt::ToolTip | Qt::FramelessWindowHint
                            | Qt::WindowTransparentForInput
                            | Qt::BypassGraphicsProxyWidget)
    {
        setObjectName(QStringLiteral("forkmeshTooltip"));
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_OpaquePaintEvent);
        setFocusPolicy(Qt::NoFocus);
        m_document.setDocumentMargin(0);
        QTextOption option = m_document.defaultTextOption();
        option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        m_document.setDefaultTextOption(option);
    }

    QString text() const { return m_text; }
    QColor background() const { return m_background; }

    QSize setContent(const QString &text, int maxWidth, int maxHeight)
    {
        const QPalette palette = QGuiApplication::palette();
        m_background = palette.color(QPalette::ToolTipBase);
        m_background.setAlpha(255);
        m_foreground = palette.color(QPalette::ToolTipText);
        m_border = QColor::fromRgbF(
            m_background.redF() * 0.7f + m_foreground.redF() * 0.3f,
            m_background.greenF() * 0.7f + m_foreground.greenF() * 0.3f,
            m_background.blueF() * 0.7f + m_foreground.blueF() * 0.3f);
        m_link = palette.color(QPalette::Link);

        m_text = text;
        QFont font = QApplication::font();
        font.setPixelSize(kFontPixels);
        m_document.setDefaultFont(font);
        if (Qt::mightBeRichText(text))
            m_document.setHtml(text);
        else
            m_document.setPlainText(text);

        const int chrome = 2 * (kPadX + 1);
        const int inner = qMax(60, qMin(kMaxWidth, maxWidth) - chrome);
        m_document.setTextWidth(-1);
        const qreal natural = m_document.idealWidth();
        m_document.setTextWidth(natural > inner ? inner : std::ceil(natural));
        const QSizeF content = m_document.size();
        return QSize(int(std::ceil(content.width())) + chrome,
                     qMin(int(std::ceil(content.height())) + 2 * (kPadY + 1),
                          qMax(1, maxHeight)));
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), m_background);
        painter.setPen(m_border);
        painter.drawRect(rect().adjusted(0, 0, -1, -1));
        const QRectF clip(0, 0, width() - 2 * (kPadX + 1),
                          height() - 2 * (kPadY + 1));
        painter.translate(kPadX + 1, kPadY + 1);
        painter.setClipRect(clip);
        QAbstractTextDocumentLayout::PaintContext context;
        context.palette = QGuiApplication::palette();
        context.palette.setColor(QPalette::Text, m_foreground);
        context.palette.setColor(QPalette::Link, m_link);
        context.clip = clip;
        m_document.documentLayout()->draw(&painter, context);
    }

private:
    QTextDocument m_document;
    QString m_text;
    QColor m_background = Qt::white;
    QColor m_foreground = Qt::black;
    QColor m_border = Qt::gray;
    QColor m_link = Qt::blue;
};

class Presenter : public QObject
{
public:
    explicit Presenter(QObject *parent = nullptr) : QObject(parent)
    {
        m_sync.setSingleShot(true);
        m_sync.setInterval(0);
        connect(&m_sync, &QTimer::timeout, this, [this] { sync(); });
        m_poll.setInterval(100);
        connect(&m_poll, &QTimer::timeout, this, [this] { sync(); });
        m_grace.setSingleShot(true);
        m_grace.setInterval(150);
        connect(&m_grace, &QTimer::timeout, this, [this] { hideWindow(); });
    }

    ~Presenter() override { delete m_window.data(); }

    static bool isQtTooltipWindow(const QObject *object)
    {
        if (!object || !object->isWidgetType())
            return false;
        const auto *widget = static_cast<const QWidget *>(object);
        if (!widget->isWindow() || widget->windowType() != Qt::ToolTip)
            return false;
        return qstrcmp(object->metaObject()->className(), "QTipLabel") == 0;
    }

    Window *visibleWindow() const
    {
        return m_window && m_window->isVisible() ? m_window.data() : nullptr;
    }
    int placements() const { return m_placements; }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        switch (event->type()) {
        case QEvent::Polish:
            if (isQtTooltipWindow(watched))
                static_cast<QWidget *>(watched)->setAttribute(Qt::WA_DontShowOnScreen);
            break;
        case QEvent::Show:
            if (isQtTooltipWindow(watched))
                adopt(static_cast<QWidget *>(watched));
            break;
        case QEvent::Hide:
        case QEvent::Move:
        case QEvent::Resize:
            if (watched == m_qtTip)
                m_sync.start();
            break;
        case QEvent::Leave:
            if (m_qtTip && leaveEndsTip(watched))
                forward(watched, event);
            break;
        case QEvent::WindowDeactivate:
            if (m_qtTip && m_qtTip->parentWidget()
                && watched == m_qtTip->parentWidget()->window()) {
                hideWindow();
                forward(watched, event);
            }
            break;
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
        case QEvent::MouseButtonDblClick:
        case QEvent::Wheel:
            if (visibleWindow())
                hideWindow();
            forward(watched, event);
            break;
        case QEvent::KeyPress: {
            const int key = static_cast<QKeyEvent *>(event)->key();
            if ((key < Qt::Key_Shift || key > Qt::Key_ScrollLock) && visibleWindow()) {
                hideWindow();
                if (m_qtTip)
                    m_qtTip->hide();
            }
            break;
        }
        case QEvent::MouseMove:
            forward(watched, event);
            break;
        default:
            break;
        }
        return false;
    }

private:
    void adopt(QWidget *tip)
    {
        tip->setAttribute(Qt::WA_DontShowOnScreen);
        if (m_qtTip != tip) {
            m_qtTip = tip;
            qApp->removeEventFilter(tip);
            connect(tip, &QObject::destroyed, this, [this] { m_sync.start(); });
        }
        m_sync.start();
    }

    void forward(QObject *watched, QEvent *event)
    {
        if (QObject *tip = m_qtTip.data())
            tip->eventFilter(watched, event);
    }

    static bool pointerOver(const QWidget *widget)
    {
        return widget->isVisible()
            && widget->rect().contains(widget->mapFromGlobal(QCursor::pos()));
    }

    bool leaveEndsTip(QObject *watched) const
    {
        QWidget *owner = m_qtTip->parentWidget();
        if (!owner)
            return true;
        QWidget *ownerWindow = owner->window();
        if (auto *window = qobject_cast<QWindow *>(watched))
            return window == ownerWindow->windowHandle();
        if (!watched->isWidgetType())
            return false;
        return watched == ownerWindow || !pointerOver(owner);
    }

    void sync()
    {
        auto *label = qobject_cast<QLabel *>(m_qtTip.data());
        const QString text = label && label->isVisible() ? label->text() : QString();
        if (text.isEmpty()) {
            if (visibleWindow()) {
                if (!m_grace.isActive())
                    m_grace.start();
            } else {
                m_poll.stop();
            }
            return;
        }
        m_grace.stop();
        QWidget *owner = label->parentWidget();
        if (visibleWindow() && m_window->text() == text && m_owner == owner)
            return;
        present(text, owner);
    }

    void present(const QString &text, QWidget *owner)
    {
        const QPoint pointer = QCursor::pos();
        QWidget *host = owner ? owner->window() : QApplication::activeWindow();
        if (m_window && m_window->parentWidget() != host) {
            m_window->hide();
            m_window->deleteLater();
            m_window = nullptr;
        }
        if (!m_window)
            m_window = new Window(host);

        QScreen *screen = QGuiApplication::screenAt(pointer);
        if (!screen && host)
            screen = host->screen();
        if (!screen)
            screen = QGuiApplication::primaryScreen();
        if (!screen)
            return;
        const QRect area = screen->availableGeometry();
        const QSize size = m_window->setContent(text, area.width() - 16, area.height());
        m_window->setGeometry(placeBeside(QRect(QPoint(), size), pointer, area));
        m_window->update();
        m_window->show();
        m_owner = owner;
        ++m_placements;
        m_poll.start();
    }

    void hideWindow()
    {
        m_grace.stop();
        m_poll.stop();
        if (m_window)
            m_window->hide();
        m_owner = nullptr;
    }

    QPointer<QWidget> m_qtTip;
    QPointer<QWidget> m_owner;
    QPointer<Window> m_window;
    QTimer m_sync;
    QTimer m_poll;
    QTimer m_grace;
    int m_placements = 0;
};

inline Presenter *install()
{
    static Presenter *presenter = nullptr;
    if (presenter || !qApp)
        return presenter;
    QApplication::setEffectEnabled(Qt::UI_FadeTooltip, false);
    QApplication::setEffectEnabled(Qt::UI_AnimateTooltip, false);
    presenter = new Presenter(qApp);
    qApp->installEventFilter(presenter);
    return presenter;
}

} // namespace Tooltips
