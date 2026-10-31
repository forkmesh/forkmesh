#include "MastodonWidgets.h"
#include "MastodonClient.h"
#include "MastodonCore.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QDateTime>
#include <QFile>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtMath>

namespace forkmesh::mastodon {

namespace {

QColor mix(const QColor &a, const QColor &b, qreal amount)
{
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * amount,
                            a.greenF() + (b.greenF() - a.greenF()) * amount,
                            a.blueF() + (b.blueF() - a.blueF()) * amount);
}

QString rgba(const QColor &color)
{
    return QStringLiteral("rgba(%1,%2,%3,%4)")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(QString::number(color.alphaF(), 'f', 3));
}

QPixmap drawnPixmap(const QString &name, const QColor &color, int side, qreal ratio)
{
    QPixmap pixmap(qRound(side * ratio), qRound(side * ratio));
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.scale(side / 16.0, side / 16.0);
    QPen pen(color, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    QPainterPath path;
    if (name == QLatin1String("reply")) {
        path.moveTo(6, 3.5);
        path.lineTo(2.5, 7);
        path.lineTo(6, 10.5);
        path.moveTo(2.5, 7);
        path.lineTo(9.5, 7);
        path.cubicTo(12.5, 7, 13.5, 9, 13.5, 12.5);
        painter.drawPath(path);
    } else if (name == QLatin1String("boost")) {
        path.moveTo(2.5, 8.5);
        path.lineTo(2.5, 5);
        path.lineTo(12.5, 5);
        path.moveTo(10.5, 3);
        path.lineTo(12.5, 5);
        path.lineTo(10.5, 7);
        path.moveTo(13.5, 7.5);
        path.lineTo(13.5, 11);
        path.lineTo(3.5, 11);
        path.moveTo(5.5, 9);
        path.lineTo(3.5, 11);
        path.lineTo(5.5, 13);
        painter.drawPath(path);
    } else if (name == QLatin1String("bookmark") || name == QLatin1String("bookmark-fill")) {
        path.moveTo(4, 2.5);
        path.lineTo(12, 2.5);
        path.lineTo(12, 13.5);
        path.lineTo(8, 10.5);
        path.lineTo(4, 13.5);
        path.closeSubpath();
        if (name == QLatin1String("bookmark-fill"))
            painter.setBrush(color);
        painter.drawPath(path);
    } else if (name == QLatin1String("star-fill")) {
        QPolygonF star;
        for (int point = 0; point < 10; ++point) {
            const qreal radius = point % 2 ? 2.9 : 6.6;
            const qreal angle = qDegreesToRadians(-90.0 + point * 36.0);
            star << QPointF(8 + radius * qCos(angle), 8.4 + radius * qSin(angle));
        }
        painter.setBrush(color);
        painter.drawPolygon(star);
    } else if (name == QLatin1String("globe")) {
        painter.drawEllipse(QRectF(2, 2, 12, 12));
        painter.drawEllipse(QRectF(5.3, 2, 5.4, 12));
        painter.drawLine(QPointF(2.3, 8), QPointF(13.7, 8));
    } else if (name == QLatin1String("at") || name == QLatin1String("hash")) {
        QFont font = QApplication::font();
        font.setPixelSize(14);
        font.setBold(true);
        painter.setFont(font);
        painter.drawText(QRectF(0, -1, 16, 17), Qt::AlignCenter,
                         name == QLatin1String("at") ? QStringLiteral("@") : QStringLiteral("#"));
    } else if (name == QLatin1String("pin")) {
        path.moveTo(5.5, 2.5);
        path.lineTo(10.5, 2.5);
        path.lineTo(9.8, 5);
        path.lineTo(12, 9);
        path.lineTo(4, 9);
        path.lineTo(6.2, 5);
        path.closeSubpath();
        path.moveTo(8, 9);
        path.lineTo(8, 14);
        painter.drawPath(path);
    } else if (name == QLatin1String("quote")) {
        path.moveTo(3, 4);
        path.lineTo(13, 4);
        path.moveTo(3, 8);
        path.lineTo(13, 8);
        path.moveTo(3, 12);
        path.lineTo(9, 12);
        painter.drawPath(path);
    } else {
        return {};
    }
    return pixmap;
}

QPixmap svgPixmap(const QString &name, const QColor &color, int side, qreal ratio)
{
    QString path = QStringLiteral(":/icons/octicons/%1.svg").arg(name);
    if (!QFile::exists(path))
        path = QStringLiteral(":/icons/codicons/%1.svg").arg(name);
    if (!QFile::exists(path))
        return {};
    QPixmap pixmap = QIcon(path).pixmap(QSize(side, side), ratio);
    if (pixmap.isNull())
        return pixmap;
    QPainter painter(&pixmap);
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(QRect(QPoint(0, 0), pixmap.size()), color);
    return pixmap;
}

QString cssColor(const QColor &color)
{
    return color.name(QColor::HexArgb);
}

QString durationLeft(const QDateTime &end)
{
    const qint64 seconds = QDateTime::currentDateTimeUtc().secsTo(end);
    if (seconds <= 0)
        return QStringLiteral("Closed");
    if (seconds < 3600)
        return QStringLiteral("%1 min left").arg(qMax<qint64>(1, seconds / 60));
    if (seconds < 86400)
        return QStringLiteral("%1 h left").arg(seconds / 3600);
    return QStringLiteral("%1 d left").arg(seconds / 86400);
}

}

QIcon icon(const QString &name, const QColor &color, int side)
{
    QIcon result;
    for (const qreal ratio : {1.0, 2.0}) {
        QPixmap pixmap = drawnPixmap(name, color, side, ratio);
        if (pixmap.isNull())
            pixmap = svgPixmap(name, color, side, ratio);
        if (!pixmap.isNull())
            result.addPixmap(pixmap);
    }
    return result;
}

QPixmap roundedPixmap(const QImage &image, int side, qreal dpr, bool circle)
{
    const int pixels = qRound(side * dpr);
    QPixmap pixmap(pixels, pixels);
    pixmap.fill(Qt::transparent);
    if (image.isNull())
        return pixmap;
    const QImage scaled = image.scaled(pixels, pixels, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    QPainterPath clip;
    if (circle)
        clip.addEllipse(QRectF(0, 0, pixels, pixels));
    else
        clip.addRoundedRect(QRectF(0, 0, pixels, pixels), pixels * 0.18, pixels * 0.18);
    painter.setClipPath(clip);
    painter.drawImage(QPointF((pixels - scaled.width()) / 2.0, (pixels - scaled.height()) / 2.0), scaled);
    pixmap.setDevicePixelRatio(dpr);
    return pixmap;
}

QPixmap letterAvatar(const QString &name, int side, qreal dpr)
{
    const int pixels = qRound(side * dpr);
    QPixmap pixmap(pixels, pixels);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    const uint hash = qHash(name);
    const QColor fill = QColor::fromHsl(static_cast<int>(hash % 360), 110, 120);
    painter.setPen(Qt::NoPen);
    painter.setBrush(fill);
    painter.drawRoundedRect(QRectF(0, 0, pixels, pixels), pixels * 0.18, pixels * 0.18);
    QFont font = QApplication::font();
    font.setPixelSize(qMax(8, pixels * 9 / 20));
    font.setBold(true);
    painter.setFont(font);
    painter.setPen(Qt::white);
    const QString letter = name.trimmed().isEmpty() ? QStringLiteral("@") : name.trimmed().left(1).toUpper();
    painter.drawText(QRect(0, 0, pixels, pixels), Qt::AlignCenter, letter);
    pixmap.setDevicePixelRatio(dpr);
    return pixmap;
}

QColor mutedColor(const QWidget *widget)
{
    const QPalette palette = widget ? widget->palette() : QApplication::palette();
    return mix(palette.color(QPalette::WindowText), palette.color(QPalette::Window), 0.42);
}

QColor accentColor(const QWidget *widget)
{
    const QPalette palette = widget ? widget->palette() : QApplication::palette();
    return palette.color(QPalette::Link);
}

QColor favouriteColor()
{
    return QColor(0xe3, 0xa0, 0x1b);
}

QColor boostColor()
{
    return QColor(0x2f, 0xa6, 0x6a);
}

QToolButton *iconButton(const QString &iconName, const QString &tip, QWidget *parent)
{
    auto *button = new QToolButton(parent);
    button->setObjectName(QStringLiteral("mastodonIconButton"));
    button->setProperty("mastodonIcon", iconName);
    button->setIcon(icon(iconName, mutedColor(parent)));
    button->setIconSize(QSize(16, 16));
    button->setToolTip(tip);
    button->setAccessibleName(tip);
    button->setAutoRaise(true);
    button->setCursor(Qt::PointingHandCursor);
    button->setFixedSize(30, 30);
    return button;
}

QLabel *mutedLabel(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("mutedLabel"));
    label->setStyleSheet(QStringLiteral("color: %1; font-size: 12px; background: transparent;")
                             .arg(cssColor(mutedColor(parent))));
    return label;
}

QLabel *richLabel(const QString &html, QWidget *parent)
{
    auto *label = new QLabel(parent);
    label->setTextFormat(Qt::RichText);
    label->setText(html);
    label->setWordWrap(true);
    label->setOpenExternalLinks(false);
    label->setTextInteractionFlags(Qt::TextBrowserInteraction);
    label->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    return label;
}

AvatarLabel::AvatarLabel(int side, QWidget *parent)
    : QLabel(parent), m_side(side)
{
    setFixedSize(side, side);
    setStyleSheet(QStringLiteral("background: transparent;"));
}

void AvatarLabel::setAccount(Client *client, const QJsonObject &account)
{
    QString url = account.value(QStringLiteral("avatar_static")).toString();
    if (url.isEmpty())
        url = account.value(QStringLiteral("avatar")).toString();
    setSource(client, url, displayName(account));
}

void AvatarLabel::setSource(Client *client, const QString &url, const QString &name)
{
    m_url = url;
    const qreal dpr = devicePixelRatioF();
    setPixmap(letterAvatar(name, m_side, dpr));
    if (!client || url.isEmpty())
        return;
    QPointer<AvatarLabel> guard(this);
    client->image(url, [guard, url](const QImage &image) {
        if (guard && guard->m_url == url)
            guard->setPixmap(roundedPixmap(image, guard->m_side, guard->devicePixelRatioF(), false));
    });
}

void AvatarLabel::mouseReleaseEvent(QMouseEvent *event)
{
    if (clicked && event->button() == Qt::LeftButton && rect().contains(event->position().toPoint())) {
        clicked();
        return;
    }
    QLabel::mouseReleaseEvent(event);
}

MediaTile::MediaTile(const QJsonObject &attachment, bool sensitive, QWidget *parent)
    : QWidget(parent), m_attachment(attachment), m_hidden(sensitive)
{
    setCursor(Qt::PointingHandCursor);
    setMinimumWidth(80);
    const QString description = attachment.value(QStringLiteral("description")).toString();
    setToolTip(description.isEmpty() ? QStringLiteral("No image description") : description);
    setAccessibleName(description.isEmpty() ? QStringLiteral("Attachment") : description);
}

void MediaTile::setImage(const QImage &image)
{
    m_image = image;
    update();
}

void MediaTile::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    QPainterPath clip;
    clip.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);
    painter.setClipPath(clip);
    const QColor base = palette().color(QPalette::Window);
    painter.fillRect(rect(), mix(base, palette().color(QPalette::WindowText), 0.08));
    if (!m_image.isNull()) {
        const QSize target = m_image.size().scaled(size(), Qt::KeepAspectRatioByExpanding);
        const QRect area(QPoint((width() - target.width()) / 2, (height() - target.height()) / 2), target);
        if (m_hidden) {
            const QImage tiny = m_image.scaled(12, 12, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            painter.drawImage(area, tiny);
        } else {
            painter.drawImage(area, m_image);
        }
    }
    const QString type = m_attachment.value(QStringLiteral("type")).toString();
    QFont font = painter.font();
    font.setPixelSize(12);
    font.setBold(true);
    painter.setFont(font);
    if (m_hidden) {
        painter.fillRect(rect(), QColor(0, 0, 0, 120));
        painter.setPen(Qt::white);
        painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("Sensitive content\nClick to show"));
        return;
    }
    if (type == QLatin1String("video") || type == QLatin1String("gifv") || type == QLatin1String("audio")) {
        const QRectF badge(width() / 2.0 - 20, height() / 2.0 - 20, 40, 40);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 150));
        painter.drawEllipse(badge);
        QPolygonF play;
        play << QPointF(badge.center().x() - 6, badge.center().y() - 9)
             << QPointF(badge.center().x() + 10, badge.center().y())
             << QPointF(badge.center().x() - 6, badge.center().y() + 9);
        painter.setBrush(Qt::white);
        painter.drawPolygon(play);
        if (type == QLatin1String("gifv")) {
            painter.setPen(Qt::white);
            painter.drawText(rect().adjusted(8, 6, -8, -6), Qt::AlignLeft | Qt::AlignBottom, QStringLiteral("GIF"));
        }
    }
    if (!m_attachment.value(QStringLiteral("description")).toString().isEmpty()) {
        const QRectF badge(8, height() - 26, 30, 18);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 170));
        painter.drawRoundedRect(badge, 4, 4);
        painter.setPen(Qt::white);
        font.setPixelSize(10);
        painter.setFont(font);
        painter.drawText(badge, Qt::AlignCenter, QStringLiteral("ALT"));
    }
}

void MediaTile::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    if (m_hidden) {
        m_hidden = false;
        update();
        return;
    }
    if (clicked)
        clicked();
}

PostCard::PostCard(const QJsonObject &status, CardContextPtr context, bool focused, QWidget *parent)
    : QFrame(parent), m_status(status), m_context(std::move(context)), m_focused(focused)
{
    setObjectName(QStringLiteral("mastodonPost"));
    setAttribute(Qt::WA_Hover);
    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(0);
    build();
}

QJsonObject PostCard::shown() const
{
    return shownStatus(m_status);
}

QString PostCard::shownId() const
{
    return shown().value(QStringLiteral("id")).toString();
}

void PostCard::setStatus(const QJsonObject &status)
{
    if (m_status.value(QStringLiteral("reblog")).isObject() &&
        status.value(QStringLiteral("id")).toString() == shownId())
        m_status.insert(QStringLiteral("reblog"), status);
    else
        m_status = status;
    build();
}

void PostCard::mouseReleaseEvent(QMouseEvent *event)
{
    const auto *label = qobject_cast<QLabel *>(childAt(event->position().toPoint()));
    if (label && label->hasSelectedText()) {
        QFrame::mouseReleaseEvent(event);
        return;
    }
    if (event->button() == Qt::LeftButton && !m_focused && m_context->openThread)
        m_context->openThread(shownId());
    QFrame::mouseReleaseEvent(event);
}

QToolButton *PostCard::actionButton(const QString &iconName, const QString &tip, const QString &count,
                                    bool active, const QColor &activeColor)
{
    auto *button = new QToolButton;
    button->setObjectName(QStringLiteral("mastodonAction"));
    button->setAutoRaise(true);
    button->setCursor(Qt::PointingHandCursor);
    button->setToolTip(tip);
    button->setAccessibleName(tip);
    button->setIconSize(QSize(16, 16));
    const QColor color = active ? activeColor : mutedColor(this);
    button->setIcon(icon(iconName, color));
    if (!count.isEmpty()) {
        button->setText(count);
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    }
    button->setStyleSheet(QStringLiteral("QToolButton { color: %1; font-size: 12px; padding: 3px 6px; border-radius: 6px; }")
                              .arg(cssColor(color)));
    button->setMinimumWidth(36);
    return button;
}

void PostCard::build()
{
    while (QLayoutItem *item = m_layout->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
    const QPalette colors = palette();
    const QColor separator = mix(colors.color(QPalette::Window), colors.color(QPalette::WindowText), 0.12);
    QColor hover = colors.color(QPalette::WindowText);
    hover.setAlphaF(0.035);
    QColor focusFill = accentColor(this);
    focusFill.setAlphaF(0.06);
    setStyleSheet(QStringLiteral("QFrame#mastodonPost { background: %1; border: none; border-bottom: 1px solid %2; }"
                                 "QFrame#mastodonPost:hover { background: %3; }")
                      .arg(m_focused ? rgba(focusFill) : QStringLiteral("transparent"), cssColor(separator),
                           m_focused ? rgba(focusFill) : rgba(hover)));

    auto *body = new QWidget;
    body->setStyleSheet(QStringLiteral("background: transparent;"));
    m_layout->addWidget(body);
    auto *outer = new QVBoxLayout(body);
    outer->setContentsMargins(16, 12, 16, 8);
    outer->setSpacing(6);

    const QJsonObject post = shown();
    const QJsonObject account = post.value(QStringLiteral("account")).toObject();
    const QColor muted = mutedColor(this);
    const QString mutedCss = cssColor(muted);

    bool hide = false;
    const QString filtered = filterHideReason(post, m_context->filterContext, &hide);
    if (!filtered.isEmpty() && !m_filterRevealed) {
        auto *row = new QHBoxLayout;
        row->addWidget(mutedLabel(QStringLiteral("Filtered: %1").arg(filtered), this), 1);
        auto *show = new QPushButton(QStringLiteral("Show anyway"));
        show->setObjectName(QStringLiteral("ghostButton"));
        connect(show, &QPushButton::clicked, this, [this] {
            m_filterRevealed = true;
            build();
        });
        row->addWidget(show);
        outer->addLayout(row);
        return;
    }

    const QJsonObject booster = m_status.value(QStringLiteral("reblog")).isObject()
                                    ? m_status.value(QStringLiteral("account")).toObject()
                                    : QJsonObject();
    if (!booster.isEmpty() || m_status.value(QStringLiteral("pinned")).toBool()) {
        auto *row = new QHBoxLayout;
        row->setContentsMargins(30, 0, 0, 0);
        row->setSpacing(6);
        auto *mark = new QLabel;
        mark->setPixmap(icon(booster.isEmpty() ? QStringLiteral("pin") : QStringLiteral("boost"), muted, 14).pixmap(14, 14));
        mark->setStyleSheet(QStringLiteral("background: transparent;"));
        row->addWidget(mark);
        auto *text = richLabel(booster.isEmpty()
                                   ? QStringLiteral("Pinned")
                                   : QStringLiteral("<a href=\"%1:%2\" style=\"color:%3; text-decoration:none;\">%4</a> boosted")
                                         .arg(kAccountScheme, booster.value(QStringLiteral("id")).toString(), mutedCss,
                                              escapeText(displayName(booster))));
        text->setStyleSheet(QStringLiteral("color: %1; font-size: 12px; background: transparent;").arg(mutedCss));
        connect(text, &QLabel::linkActivated, this, [this, booster](const QString &) {
            if (m_context->openAccount)
                m_context->openAccount(booster);
        });
        row->addWidget(text, 1);
        outer->addLayout(row);
    }

    auto *columns = new QHBoxLayout;
    columns->setSpacing(12);
    outer->addLayout(columns);
    auto *avatar = new AvatarLabel(m_focused ? 48 : 44);
    avatar->setCursor(Qt::PointingHandCursor);
    avatar->setToolTip(fullHandle(account, m_context->instance));
    avatar->setAccount(m_context->client, account);
    avatar->clicked = [this, account] {
        if (m_context->openAccount)
            m_context->openAccount(account);
    };
    auto *avatarColumn = new QVBoxLayout;
    avatarColumn->addWidget(avatar);
    avatarColumn->addStretch(1);
    columns->addLayout(avatarColumn);

    auto *content = new QVBoxLayout;
    content->setSpacing(6);
    columns->addLayout(content, 1);

    auto *header = new QHBoxLayout;
    header->setSpacing(6);
    const QString handle = fullHandle(account, m_context->instance);
    auto *name = richLabel(QStringLiteral("<a href=\"%1:%2\" style=\"color:%3; text-decoration:none;\"><b>%4</b></a>"
                                          " <span style=\"color:%5;\">%6</span>%7")
                               .arg(kAccountScheme, account.value(QStringLiteral("id")).toString(),
                                    cssColor(colors.color(QPalette::WindowText)),
                                    escapeText(displayName(account)), mutedCss, escapeText(handle),
                                    account.value(QStringLiteral("bot")).toBool()
                                        ? QStringLiteral(" <span style=\"color:%1;\">&middot; bot</span>").arg(mutedCss)
                                        : QString()));
    name->setWordWrap(false);
    name->setStyleSheet(QStringLiteral("font-size: 14px; background: transparent;"));
    name->setMinimumWidth(40);
    name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    connect(name, &QLabel::linkActivated, this, [this, account](const QString &) {
        if (m_context->openAccount)
            m_context->openAccount(account);
    });
    header->addWidget(name, 1);
    const QString visibility = post.value(QStringLiteral("visibility")).toString();
    auto *visibilityMark = new QLabel;
    visibilityMark->setPixmap(icon(visibilityIcon(visibility), muted, 13).pixmap(13, 13));
    visibilityMark->setToolTip(visibilityLabel(visibility));
    visibilityMark->setStyleSheet(QStringLiteral("background: transparent;"));
    header->addWidget(visibilityMark);
    const QDateTime created = QDateTime::fromString(post.value(QStringLiteral("created_at")).toString(), Qt::ISODateWithMs);
    QString when = relativeTime(created, QDateTime::currentDateTimeUtc());
    if (!post.value(QStringLiteral("edited_at")).toString().isEmpty())
        when += QStringLiteral(" · edited");
    auto *time = richLabel(QStringLiteral("<a href=\"fm-thread:%1\" style=\"color:%2; text-decoration:none;\">%3</a>")
                               .arg(escapeText(post.value(QStringLiteral("id")).toString()), mutedCss, escapeText(when)));
    time->setWordWrap(false);
    time->setToolTip(QLocale().toString(created.toLocalTime(), QLocale::LongFormat));
    time->setStyleSheet(QStringLiteral("font-size: 12px; background: transparent;"));
    connect(time, &QLabel::linkActivated, this, [this](const QString &) {
        if (m_context->openThread)
            m_context->openThread(shownId());
    });
    header->addWidget(time);
    content->addLayout(header);

    const RichText text = renderContent(post.value(QStringLiteral("content")).toString(),
                                        post.value(QStringLiteral("mentions")).toArray(),
                                        post.value(QStringLiteral("tags")).toArray());
    const QString spoiler = post.value(QStringLiteral("spoiler_text")).toString();
    QWidget *hidden = nullptr;
    if (!spoiler.isEmpty()) {
        auto *warning = new QFrame;
        warning->setStyleSheet(QStringLiteral("QFrame { background: %1; border-radius: 6px; }")
                                   .arg(rgba([&] { QColor c = favouriteColor(); c.setAlphaF(0.14); return c; }())));
        auto *warningRow = new QHBoxLayout(warning);
        warningRow->setContentsMargins(10, 6, 6, 6);
        auto *warningText = new QLabel(spoiler);
        warningText->setWordWrap(true);
        warningText->setStyleSheet(QStringLiteral("font-size: 13px; font-weight: 600; background: transparent;"));
        warningRow->addWidget(warningText, 1);
        auto *toggle = new QPushButton(m_revealed ? QStringLiteral("Show less") : QStringLiteral("Show more"));
        toggle->setObjectName(QStringLiteral("ghostButton"));
        toggle->setCursor(Qt::PointingHandCursor);
        connect(toggle, &QPushButton::clicked, this, [this] {
            m_revealed = !m_revealed;
            build();
        });
        warningRow->addWidget(toggle);
        content->addWidget(warning);
    }
    const bool showBody = spoiler.isEmpty() || m_revealed;
    if (showBody) {
        hidden = new QWidget;
        hidden->setStyleSheet(QStringLiteral("background: transparent;"));
        auto *bodyLayout = new QVBoxLayout(hidden);
        bodyLayout->setContentsMargins(0, 0, 0, 0);
        bodyLayout->setSpacing(8);
        if (!text.html.isEmpty()) {
            auto *label = richLabel(text.html);
            label->setStyleSheet(QStringLiteral("font-size: %1px; background: transparent;").arg(m_focused ? 16 : 14));
            connect(label, &QLabel::linkActivated, this, [this](const QString &link) {
                if (m_context->openLink)
                    m_context->openLink(link);
            });
            bodyLayout->addWidget(label);
        }
        const QJsonArray media = post.value(QStringLiteral("media_attachments")).toArray();
        if (!media.isEmpty())
            bodyLayout->addWidget(buildMedia(media, post.value(QStringLiteral("sensitive")).toBool()));
        const QJsonObject poll = post.value(QStringLiteral("poll")).toObject();
        if (!poll.isEmpty())
            bodyLayout->addWidget(buildPoll(poll));
        const QJsonObject card = post.value(QStringLiteral("card")).toObject();
        if (media.isEmpty() && poll.isEmpty() && !card.isEmpty() &&
            safeWebUrl(QUrl(card.value(QStringLiteral("url")).toString())))
            bodyLayout->addWidget(buildCardPreview(card));
        content->addWidget(hidden);
    }

    if (m_focused) {
        QStringList facts;
        facts << QLocale().toString(created.toLocalTime(), QLocale::LongFormat);
        const QString application = post.value(QStringLiteral("application")).toObject().value(QStringLiteral("name")).toString();
        if (!application.isEmpty())
            facts << application;
        facts << visibilityLabel(visibility);
        content->addWidget(mutedLabel(facts.join(QStringLiteral(" · ")), this));
    }

    auto *actions = new QHBoxLayout;
    actions->setContentsMargins(0, 2, 0, 0);
    actions->setSpacing(18);
    const auto count = [&](const char *key) {
        const qint64 value = post.value(QLatin1String(key)).toInteger();
        return value > 0 ? countText(value) : QString();
    };
    auto *replyButton = actionButton(QStringLiteral("reply"), QStringLiteral("Reply"), count("replies_count"), false, muted);
    connect(replyButton, &QToolButton::clicked, this, [this] {
        if (m_context->reply)
            m_context->reply(shown());
    });
    actions->addWidget(replyButton);
    const bool reblogged = post.value(QStringLiteral("reblogged")).toBool();
    const bool own = account.value(QStringLiteral("id")).toString() == m_context->selfId;
    const bool boostable = visibility == QLatin1String("public") || visibility == QLatin1String("unlisted") ||
                           (own && visibility == QLatin1String("private"));
    auto *boostButton = actionButton(QStringLiteral("boost"), reblogged ? QStringLiteral("Undo boost") : QStringLiteral("Boost"),
                                     count("reblogs_count"), reblogged, boostColor());
    boostButton->setEnabled(boostable);
    if (!boostable)
        boostButton->setToolTip(QStringLiteral("This post cannot be boosted"));
    connect(boostButton, &QToolButton::clicked, this, [this, reblogged] {
        if (m_context->act)
            m_context->act(shown(), reblogged ? QStringLiteral("unreblog") : QStringLiteral("reblog"));
    });
    actions->addWidget(boostButton);
    const bool favourited = post.value(QStringLiteral("favourited")).toBool();
    auto *favouriteButton = actionButton(favourited ? QStringLiteral("star-fill") : QStringLiteral("star"),
                                         favourited ? QStringLiteral("Remove favourite") : QStringLiteral("Favourite"),
                                         count("favourites_count"), favourited, favouriteColor());
    connect(favouriteButton, &QToolButton::clicked, this, [this, favourited] {
        if (m_context->act)
            m_context->act(shown(), favourited ? QStringLiteral("unfavourite") : QStringLiteral("favourite"));
    });
    actions->addWidget(favouriteButton);
    const bool bookmarked = post.value(QStringLiteral("bookmarked")).toBool();
    auto *bookmarkButton = actionButton(bookmarked ? QStringLiteral("bookmark-fill") : QStringLiteral("bookmark"),
                                        bookmarked ? QStringLiteral("Remove bookmark") : QStringLiteral("Bookmark"),
                                        QString(), bookmarked, accentColor(this));
    connect(bookmarkButton, &QToolButton::clicked, this, [this, bookmarked] {
        if (m_context->act)
            m_context->act(shown(), bookmarked ? QStringLiteral("unbookmark") : QStringLiteral("bookmark"));
    });
    actions->addWidget(bookmarkButton);
    auto *moreButton = actionButton(QStringLiteral("kebab-horizontal"), QStringLiteral("More"), QString(), false, muted);
    connect(moreButton, &QToolButton::clicked, this, [this, moreButton] {
        if (m_context->more)
            m_context->more(shown(), moreButton);
    });
    actions->addWidget(moreButton);
    actions->addStretch(1);
    content->addLayout(actions);
}

QWidget *PostCard::buildMedia(const QJsonArray &media, bool sensitive)
{
    auto *grid = new QWidget;
    grid->setStyleSheet(QStringLiteral("background: transparent;"));
    auto *layout = new QGridLayout(grid);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    const int count = qMin(4, static_cast<int>(media.size()));
    for (int index = 0; index < count; ++index) {
        const QJsonObject attachment = media.at(index).toObject();
        auto *tile = new MediaTile(attachment, sensitive, grid);
        int height = 120;
        if (count == 1)
            height = 280;
        else if (count == 2)
            height = 200;
        else if (count == 3 && index == 0)
            height = 244;
        tile->setFixedHeight(height);
        tile->clicked = [this, media, index] {
            if (m_context->openMedia)
                m_context->openMedia(media, index);
        };
        if (count == 3 && index == 0)
            layout->addWidget(tile, 0, 0, 2, 1);
        else if (count == 3)
            layout->addWidget(tile, index - 1, 1);
        else
            layout->addWidget(tile, index / 2, index % 2);
        QString preview = attachment.value(QStringLiteral("preview_url")).toString();
        if (preview.isEmpty())
            preview = attachment.value(QStringLiteral("url")).toString();
        if (m_context->client && !preview.isEmpty()) {
            QPointer<MediaTile> guard(tile);
            m_context->client->image(preview, [guard](const QImage &image) {
                if (guard)
                    guard->setImage(image);
            });
        }
    }
    return grid;
}

QWidget *PostCard::buildPoll(const QJsonObject &poll)
{
    auto *box = new QWidget;
    box->setStyleSheet(QStringLiteral("background: transparent;"));
    auto *layout = new QVBoxLayout(box);
    layout->setContentsMargins(0, 2, 0, 2);
    layout->setSpacing(6);
    const QJsonArray options = poll.value(QStringLiteral("options")).toArray();
    const bool expired = poll.value(QStringLiteral("expired")).toBool();
    const bool voted = poll.value(QStringLiteral("voted")).toBool();
    const bool multiple = poll.value(QStringLiteral("multiple")).toBool();
    const bool own = shown().value(QStringLiteral("account")).toObject().value(QStringLiteral("id")).toString() == m_context->selfId;
    const qint64 votes = poll.value(QStringLiteral("votes_count")).toInteger();
    const qint64 voters = poll.value(QStringLiteral("voters_count")).toInteger();
    QList<int> ownVotes;
    for (const QJsonValue &value : poll.value(QStringLiteral("own_votes")).toArray())
        ownVotes.append(value.toInt());
    if (!expired && !voted && !own) {
        auto *group = new QButtonGroup(box);
        group->setExclusive(!multiple);
        for (int index = 0; index < options.size(); ++index) {
            const QString title = options.at(index).toObject().value(QStringLiteral("title")).toString();
            QAbstractButton *choice = multiple ? static_cast<QAbstractButton *>(new QCheckBox(title))
                                               : static_cast<QAbstractButton *>(new QRadioButton(title));
            group->addButton(choice, index);
            layout->addWidget(choice);
        }
        auto *vote = new QPushButton(QStringLiteral("Vote"));
        vote->setObjectName(QStringLiteral("ghostButton"));
        vote->setCursor(Qt::PointingHandCursor);
        connect(vote, &QPushButton::clicked, this, [this, group, poll] {
            QList<int> choices;
            for (QAbstractButton *button : group->buttons()) {
                if (button->isChecked())
                    choices.append(group->id(button));
            }
            if (!choices.isEmpty() && m_context->vote)
                m_context->vote(poll, choices);
        });
        auto *row = new QHBoxLayout;
        row->addWidget(vote);
        row->addStretch(1);
        layout->addLayout(row);
    } else {
        const qint64 total = multiple && voters > 0 ? voters : votes;
        for (int index = 0; index < options.size(); ++index) {
            const QJsonObject option = options.at(index).toObject();
            const qint64 count = option.value(QStringLiteral("votes_count")).toInteger();
            const int percent = total > 0 ? qRound(100.0 * count / total) : 0;
            auto *line = new QLabel(QStringLiteral("<b>%1%</b>&nbsp;&nbsp;%2%3")
                                        .arg(percent)
                                        .arg(escapeText(option.value(QStringLiteral("title")).toString()),
                                             ownVotes.contains(index) ? QStringLiteral(" &#10003;") : QString()));
            line->setTextFormat(Qt::RichText);
            line->setWordWrap(true);
            line->setStyleSheet(QStringLiteral("font-size: 13px; background: transparent;"));
            layout->addWidget(line);
            auto *bar = new QProgressBar;
            bar->setRange(0, 100);
            bar->setValue(percent);
            bar->setTextVisible(false);
            bar->setFixedHeight(5);
            const QColor fill = ownVotes.contains(index) ? accentColor(this) : mutedColor(this);
            bar->setStyleSheet(QStringLiteral("QProgressBar { border: none; border-radius: 2px; background: %1; }"
                                              "QProgressBar::chunk { border-radius: 2px; background: %2; }")
                                   .arg(rgba([&] { QColor c = mutedColor(this); c.setAlphaF(0.18); return c; }()),
                                        cssColor(fill)));
            layout->addWidget(bar);
        }
    }
    QStringList facts;
    facts << (voters == 1 ? QStringLiteral("1 person") : QStringLiteral("%1 people").arg(countText(voters)));
    const QDateTime end = QDateTime::fromString(poll.value(QStringLiteral("expires_at")).toString(), Qt::ISODateWithMs);
    facts << (expired ? QStringLiteral("Closed") : (end.isValid() ? durationLeft(end) : QStringLiteral("Open")));
    layout->addWidget(mutedLabel(facts.join(QStringLiteral(" · ")), this));
    return box;
}

QWidget *PostCard::buildCardPreview(const QJsonObject &card)
{
    auto *frame = new QFrame;
    frame->setObjectName(QStringLiteral("mastodonLinkCard"));
    frame->setCursor(Qt::PointingHandCursor);
    const QColor border = mix(palette().color(QPalette::Window), palette().color(QPalette::WindowText), 0.16);
    frame->setStyleSheet(QStringLiteral("QFrame#mastodonLinkCard { border: 1px solid %1; border-radius: 8px; background: transparent; }")
                             .arg(cssColor(border)));
    auto *row = new QHBoxLayout(frame);
    row->setContentsMargins(0, 0, 10, 0);
    row->setSpacing(10);
    const QString image = card.value(QStringLiteral("image")).toString();
    if (!image.isEmpty()) {
        auto *tile = new MediaTile(QJsonObject{{QStringLiteral("type"), QStringLiteral("image")}}, false, frame);
        tile->setFixedSize(88, 88);
        const QString url = card.value(QStringLiteral("url")).toString();
        tile->clicked = [this, url] {
            if (m_context->openLink)
                m_context->openLink(url);
        };
        if (m_context->client) {
            QPointer<MediaTile> guard(tile);
            m_context->client->image(image, [guard](const QImage &loaded) {
                if (guard)
                    guard->setImage(loaded);
            });
        }
        row->addWidget(tile);
    } else {
        row->addSpacing(10);
    }
    auto *texts = new QVBoxLayout;
    texts->setContentsMargins(0, 8, 0, 8);
    texts->setSpacing(2);
    QString provider = card.value(QStringLiteral("provider_name")).toString();
    if (provider.isEmpty())
        provider = QUrl(card.value(QStringLiteral("url")).toString()).host();
    texts->addWidget(mutedLabel(provider, frame));
    const QString url = card.value(QStringLiteral("url")).toString();
    auto *title = richLabel(QStringLiteral("<a href=\"%1\" style=\"text-decoration:none;\"><b>%2</b></a>")
                                .arg(url.toHtmlEscaped(), escapeText(elide(card.value(QStringLiteral("title")).toString(), 140))));
    title->setStyleSheet(QStringLiteral("font-size: 13px; background: transparent; border: none;"));
    connect(title, &QLabel::linkActivated, this, [this](const QString &link) {
        if (m_context->openLink)
            m_context->openLink(link);
    });
    texts->addWidget(title);
    const QString description = card.value(QStringLiteral("description")).toString();
    if (!description.isEmpty()) {
        auto *summary = new QLabel(elide(description, 160));
        summary->setWordWrap(true);
        summary->setStyleSheet(QStringLiteral("color: %1; font-size: 12px; background: transparent; border: none;")
                                   .arg(cssColor(mutedColor(this))));
        texts->addWidget(summary);
    }
    row->addLayout(texts, 1);
    return frame;
}

AccountRow::AccountRow(const QJsonObject &account, CardContextPtr context, bool showNote, QWidget *parent)
    : QFrame(parent), m_account(account), m_context(std::move(context))
{
    setObjectName(QStringLiteral("mastodonAccountRow"));
    setAttribute(Qt::WA_Hover);
    setCursor(Qt::PointingHandCursor);
    const QColor separator = mix(palette().color(QPalette::Window), palette().color(QPalette::WindowText), 0.12);
    QColor hover = palette().color(QPalette::WindowText);
    hover.setAlphaF(0.035);
    setStyleSheet(QStringLiteral("QFrame#mastodonAccountRow { background: transparent; border: none; border-bottom: 1px solid %1; }"
                                 "QFrame#mastodonAccountRow:hover { background: %2; }")
                      .arg(cssColor(separator), rgba(hover)));
    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(16, 10, 16, 10);
    row->setSpacing(12);
    auto *avatar = new AvatarLabel(40);
    avatar->setAccount(m_context->client, account);
    row->addWidget(avatar, 0, Qt::AlignTop);
    auto *texts = new QVBoxLayout;
    texts->setSpacing(1);
    auto *name = new QLabel(QStringLiteral("<b>%1</b>%2").arg(
        escapeText(displayName(account)),
        account.value(QStringLiteral("locked")).toBool() ? QStringLiteral(" &#128274;") : QString()));
    name->setTextFormat(Qt::RichText);
    name->setStyleSheet(QStringLiteral("font-size: 14px; background: transparent; border: none;"));
    texts->addWidget(name);
    auto *handle = mutedLabel(fullHandle(account, m_context->instance), this);
    handle->setStyleSheet(handle->styleSheet() + QStringLiteral(" border: none;"));
    texts->addWidget(handle);
    if (showNote) {
        const QString note = elide(plainText(account.value(QStringLiteral("note")).toString()), 150);
        if (!note.isEmpty()) {
            auto *summary = new QLabel(note);
            summary->setWordWrap(true);
            summary->setStyleSheet(QStringLiteral("font-size: 12px; background: transparent; border: none;"));
            texts->addWidget(summary);
        }
    }
    row->addLayout(texts, 1);
    auto *trailing = new QHBoxLayout;
    trailing->setSpacing(6);
    row->addLayout(trailing);
    m_trailing = trailing;
}

void AccountRow::addTrailing(QWidget *widget)
{
    m_trailing->addWidget(widget);
}

void AccountRow::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_context->openAccount)
        m_context->openAccount(m_account);
    QFrame::mouseReleaseEvent(event);
}

}
