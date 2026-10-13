#pragma once

// A user-chosen background colour or image laid over the whole main window.
//
// Almost every panel paints its own opaque theme background, so a picture
// placed *under* the widgets would never be seen. The backdrop instead sits
// on top of every sibling as a translucent, mouse-transparent veil (the same
// trick editor "background image" extensions use): strength is the veil's
// opacity, so a low value tints the interface and a high one lets the picture
// dominate. It stays raised above children added later and follows the
// window's size through an event filter, so the owner only has to call
// setBackdrop() whenever the setting changes.

#include <QColor>
#include <QEvent>
#include <QImageReader>
#include <QPaintEvent>
#include <QPainter>
#include <QPixmap>
#include <QTimer>
#include <QWidget>

#include <algorithm>

namespace forkmesh::ui {

class WindowBackdrop : public QWidget
{
public:
    enum class Mode { None, Color, Image };

    explicit WindowBackdrop(QWidget *host) : QWidget(host)
    {
        setObjectName(QStringLiteral("windowBackdrop"));
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setAttribute(Qt::WA_TranslucentBackground);
        setFocusPolicy(Qt::NoFocus);
        host->installEventFilter(this);
        setGeometry(host->rect());
        hide();
    }

    // strength is a percentage (0-100) of the veil's opacity.
    void setBackdrop(Mode mode, const QColor &color, const QString &imagePath,
                     int strength)
    {
        if (imagePath != m_imagePath) {
            m_imagePath = imagePath;
            m_source = QPixmap();
            m_scaled = QPixmap();
            if (!imagePath.isEmpty()) {
                QImageReader reader(imagePath);
                reader.setAutoTransform(true);
                // A camera photo can be 50+ megapixels; the veil never needs
                // more than a large screen's worth.
                const QSize size = reader.size();
                if (size.isValid() &&
                    (size.width() > 3840 || size.height() > 3840))
                    reader.setScaledSize(
                        size.scaled(3840, 3840, Qt::KeepAspectRatio));
                m_source = QPixmap::fromImage(reader.read());
            }
        }
        m_mode = mode;
        m_color = color;
        m_strength = std::clamp(strength, 0, 100);
        const bool visible = m_strength > 0 &&
                             ((m_mode == Mode::Color && m_color.isValid()) ||
                              (m_mode == Mode::Image && !m_source.isNull()));
        setVisible(visible);
        if (visible)
            raise();
        update();
    }

    Mode mode() const { return m_mode; }
    int strength() const { return m_strength; }
    bool hasImage() const { return !m_source.isNull(); }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == parent()) {
            switch (event->type()) {
            case QEvent::Resize:
                setGeometry(parentWidget()->rect());
                break;
            case QEvent::ChildAdded:
                // Siblings created later stack above us; re-raise once they
                // are in place rather than inside their construction.
                if (isVisible() && !m_raisePending) {
                    m_raisePending = true;
                    QTimer::singleShot(0, this, [this] {
                        m_raisePending = false;
                        if (isVisible())
                            raise();
                    });
                }
                break;
            default:
                break;
            }
        }
        return QWidget::eventFilter(watched, event);
    }

    void paintEvent(QPaintEvent *event) override
    {
        QPainter painter(this);
        painter.setClipRegion(event->region());
        painter.setOpacity(static_cast<qreal>(m_strength) / 100.0);
        if (m_mode == Mode::Color) {
            painter.fillRect(rect(), m_color);
            return;
        }
        if (m_source.isNull())
            return;
        const qreal dpr = devicePixelRatioF();
        const QSize target = (QSizeF(size()) * dpr).toSize();
        if (m_scaled.isNull() || m_scaledFor != target) {
            // Cover the window, cropping the overflow evenly from both sides.
            const QPixmap cover = m_source.scaled(
                target, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
            m_scaled = cover.copy((cover.width() - target.width()) / 2,
                                  (cover.height() - target.height()) / 2,
                                  target.width(), target.height());
            m_scaled.setDevicePixelRatio(dpr);
            m_scaledFor = target;
        }
        painter.drawPixmap(0, 0, m_scaled);
    }

private:
    Mode m_mode = Mode::None;
    QColor m_color;
    QString m_imagePath;
    QPixmap m_source;
    QPixmap m_scaled;
    QSize m_scaledFor;
    int m_strength = 0;
    bool m_raisePending = false;
};

} // namespace forkmesh::ui
