#pragma once

#include "DesktopUi.h"
#include "NextShellHooks.h"
#include "Theme.h"

#include <QFont>
#include <QFontMetrics>
#include <QIcon>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QTimer>

namespace forkmesh::ui {

class VerticalIconButton : public QPushButton
{
public:
    enum Form { Action, Tab, Bare };
    explicit VerticalIconButton(const QString &text, Form form,
                                QWidget *parent = nullptr)
        : QPushButton(text, parent), m_form(form)
    {
        setCursor(Qt::PointingHandCursor);
        setFlat(true);
        setAttribute(Qt::WA_Hover, true);
    }

    QSize sizeHint() const override
    {
        const QSize hooked = tabButtonSizeHook(this, paintState());
        if (hooked.isValid())
            return hooked;
        QFont f = font();
        f.setPixelSize(10);
        f.setWeight(QFont::DemiBold);
        const int textW = QFontMetrics(f).horizontalAdvance(text());
        const int badgesW = badgeStripWidth() + kIconPx / 2;
        return QSize(qMax(44, qMax(qMax(kIconPx, textW), badgesW) + 16),
                     tileHeight());
    }
    QSize minimumSizeHint() const override { return sizeHint(); }

    void setBadgeAboveIcon(bool above)
    {
        if (m_badgeAbove == above)
            return;
        m_badgeAbove = above;
        updateGeometry();
        update();
    }

    void setBadgeCount(qint64 count)
    {
        if (m_badge == count)
            return;
        m_badge = count;
        updateGeometry();
        update();
    }
    qint64 badgeCount() const { return m_badge; }

    void setAlertBadgeCount(qint64 count)
    {
        count = qMax<qint64>(0, count);
        if (m_alertBadge == count)
            return;
        m_alertBadge = count;
        updateGeometry();
        update();
    }
    qint64 alertBadgeCount() const { return m_alertBadge; }

    void setAttentionLight(bool active)
    {
        if (m_attentionLight == active)
            return;
        m_attentionLight = active;
        m_attentionLightOn = active;
        if (active) {
            if (!m_attentionTimer) {
                m_attentionTimer = new QTimer(this);
                m_attentionTimer->setInterval(650);
                QObject::connect(m_attentionTimer, &QTimer::timeout, this,
                                 [this] {
                                     m_attentionLightOn = !m_attentionLightOn;
                                     update();
                                 });
            }
            m_attentionTimer->start();
        } else if (m_attentionTimer) {
            m_attentionTimer->stop();
        }
        update();
    }
    bool hasAttentionLight() const { return m_attentionLight; }

    void setOcticonName(const QString &name)
    {
        if (m_iconName == name)
            return;
        m_iconName = name;
        update();
    }

    void setAccentColor(const QColor &accent)
    {
        if (m_accent == accent)
            return;
        m_accent = accent;
        update();
    }
    QColor accentColor() const { return m_accent; }

    void setInRail(bool inRail)
    {
        if (m_inRail == inRail)
            return;
        m_inRail = inRail;
        update();
    }
    bool inRail() const { return m_inRail; }

    void setFooterWidget(QWidget *footer)
    {
        if (m_footer == footer)
            return;
        if (m_footer)
            m_footer->deleteLater();
        m_footer = footer;
        if (footer) {
            footer->setParent(this);
            footer->show();
        }
        updateGeometry();
        placeFooter();
    }
    QWidget *footerWidget() const { return m_footer; }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QPushButton::resizeEvent(event);
        placeFooter();
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        if (paintTabButtonHook(this, p, paintState()))
            return;
        const bool dark = currentThemeIsDark();
        const bool hovered = isEnabled() && underMouse();
        QColor fg;
        if (m_form == Action)
            fg = dark ? Theme::remapColor(QColor("#e6edf3")) : Theme::remapColor(QColor("#1f2328"));
        else
            fg = dark ? QColor(isChecked() || hovered ? "#e6edf3" : "#8b949e")
                      : QColor(isChecked() || hovered ? "#1f2328" : "#656d76");
        if (!isEnabled())
            fg = Theme::remapColor(QColor("#6e7681"));
        const bool accented = m_accent.isValid() && isEnabled();
        const QColor glyphColor = accented ? m_accent : fg;
        const QColor captionColor =
            accented && (isChecked() || hovered) ? m_accent : fg;

        if (m_form == Action) {
            if (hovered) {
                p.setPen(Qt::NoPen);
                p.setBrush(Theme::remapColor(QColor(dark ? "#21262d" : "#eaeef2")));
                p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5),
                                  6, 6);
            }
        } else if (m_form == Tab && isChecked()) {
            const QColor bar = accented ? m_accent
                                        : QColor(dark ? "#2ea043" : "#1f883d");
            if (m_inRail)
                p.fillRect(QRectF(0, 4, 2, height() - 8), bar);
            else
                p.fillRect(QRect(0, height() - 2, width(), 2), bar);
        }

        const QRect iconRect((width() - kIconPx) / 2, iconTop(), kIconPx,
                             kIconPx);
        if (m_iconName.isEmpty())
            icon().paint(&p, iconRect, Qt::AlignCenter,
                         isEnabled() ? QIcon::Normal : QIcon::Disabled);
        else
            p.drawPixmap(iconRect.topLeft(),
                         tintedOcticonPixmap(m_iconName, glyphColor, kIconPx));

        if (m_attentionLight) {
            QColor light(QStringLiteral("#58a6ff"));
            light.setAlpha(m_attentionLightOn ? 255 : 85);
            p.setPen(Qt::NoPen);
            p.setBrush(light);
            p.drawEllipse(QRectF(iconRect.right() - 2, iconRect.top() - 2,
                                 6, 6));
        }

        QFont f = font();
        f.setPixelSize(10);
        f.setWeight(QFont::DemiBold);
        p.setFont(f);
        p.setPen(captionColor);
        p.drawText(QRect(2, iconRect.bottom() + 2, width() - 4, 14),
                   Qt::AlignHCenter | Qt::AlignTop,
                   QFontMetrics(f).elidedText(text(), Qt::ElideRight,
                                              width() - 4));

        double rightEdge = m_badgeAbove
                               ? (width() + badgeStripWidth()) / 2.0
                               : iconRect.right() + kBadgeH / 2.0 + 2;
        const auto paintBadge = [&](qint64 count, const QColor &fill) {
            if (count <= 0)
                return;
            const QString badgeText = formatCount(count);
            QFont bf = font();
            bf.setPixelSize(9);
            bf.setBold(true);
            p.setFont(bf);
            const int h = kBadgeH;
            const int w = badgeWidth(count);
            const QRectF badge(qMax(0.0, rightEdge - w),
                               m_badgeAbove
                                   ? 0.0
                                   : qMax(0.0, double(iconRect.top() - 5)),
                               w, h);
            p.setPen(Qt::NoPen);
            p.setBrush(fill);
            p.drawRoundedRect(badge, h / 2.0, h / 2.0);
            p.setPen(fill.lightness() > 155 ? Theme::remapColor(QColor("#0d1117"))
                                            : QColor("#ffffff"));
            p.drawText(badge, Qt::AlignCenter, badgeText);
            rightEdge = badge.left() - 2;
        };
        paintBadge(m_badge, accented ? m_accent : QColor("#1f6feb"));
        paintBadge(m_alertBadge, QColor(dark ? "#da3633" : "#cf222e"));
    }

private:
    TabButtonPaintState paintState() const
    {
        TabButtonPaintState state;
        state.form = int(m_form);
        state.badge = m_badge;
        state.alertBadge = m_alertBadge;
        state.badgeAbove = m_badgeAbove;
        state.attentionLight = m_attentionLight;
        state.attentionLightOn = m_attentionLightOn;
        state.inRail = m_inRail;
        state.iconName = m_iconName;
        state.accent = m_accent;
        return state;
    }

    int badgeWidth(qint64 count) const
    {
        if (count <= 0)
            return 0;
        QFont bf = font();
        bf.setPixelSize(9);
        bf.setBold(true);
        return qMax(kBadgeH,
                    QFontMetrics(bf).horizontalAdvance(formatCount(count)) + 8);
    }
    int badgeStripWidth() const
    {
        const int blue = badgeWidth(m_badge);
        const int red = badgeWidth(m_alertBadge);
        return blue + red + (blue > 0 && red > 0 ? 2 : 0);
    }
    int iconTop() const { return m_badgeAbove ? kBadgeH + 1 : 6; }
    int bodyHeight() const { return m_badgeAbove ? kHeight + kBadgeH - 8 : kHeight; }
    int footerHeight() const
    {
        return m_footer ? m_footer->sizeHint().height() : 0;
    }
    int tileHeight() const { return bodyHeight() + footerHeight(); }
    void placeFooter()
    {
        if (!m_footer)
            return;
        const QSize size = m_footer->sizeHint();
        m_footer->setGeometry((width() - size.width()) / 2, bodyHeight() - 2,
                              size.width(), size.height());
    }

    static constexpr int kIconPx = 16;
    static constexpr int kHeight = 44;
    static constexpr int kBadgeH = 14;
    Form m_form;
    qint64 m_badge = 0;
    qint64 m_alertBadge = 0;
    bool m_badgeAbove = false;
    bool m_attentionLight = false;
    bool m_attentionLightOn = false;
    bool m_inRail = false;
    QTimer *m_attentionTimer = nullptr;
    QPointer<QWidget> m_footer;
    QString m_iconName;
    QColor m_accent;
};

}
