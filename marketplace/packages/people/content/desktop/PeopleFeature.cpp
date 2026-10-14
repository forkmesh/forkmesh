#include "PeopleFeature.h"

#include <QDateTime>
#include <QDialog>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRadialGradient>
#include <QScrollArea>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace forkmesh::people {
namespace {

const QColor kNight(0x12, 0x0d, 0x14);
const QColor kHearth(0x3d, 0x1c, 0x0d);
constexpr int kMaxSide = 560;
constexpr int kListAvatar = 36;

QColor senderColor(const QString &name)
{
    uint hash = 0;
    for (const QChar ch : name)
        hash = hash * 31 + ch.unicode();
    return QColor::fromHsl(int(hash % 360), 140, 105);
}

QPixmap circleAvatar(const QString &name, const QString &base64, int size, const QColor &ring, qreal ratio)
{
    QPixmap out(QSize(size, size) * ratio);
    out.setDevicePixelRatio(ratio);
    out.fill(Qt::transparent);
    QPainter painter(&out);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    const QRectF box(0, 0, size, size);
    QPainterPath clip;
    clip.addEllipse(box);
    painter.setClipPath(clip);
    QImage image;
    if (!base64.isEmpty())
        image.loadFromData(QByteArray::fromBase64(base64.toLatin1()));
    if (!image.isNull()) {
        const QImage scaled = image.scaled(QSize(size, size) * ratio, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        painter.drawImage(box, scaled, QRectF(QPointF((scaled.width() - size * ratio) / 2, (scaled.height() - size * ratio) / 2), QSizeF(size, size) * ratio));
    } else {
        painter.fillRect(box, senderColor(name.toLower()));
        QFont font = painter.font();
        font.setBold(true);
        font.setPixelSize(std::max(9, int(size * 0.44)));
        painter.setFont(font);
        painter.setPen(Qt::white);
        painter.drawText(box, Qt::AlignCenter, name.left(1).toUpper());
    }
    painter.setClipping(false);
    if (ring.isValid()) {
        painter.setPen(QPen(ring, std::max(2.0, size / 18.0)));
        painter.setBrush(Qt::NoBrush);
        const double inset = std::max(1.0, size / 36.0);
        painter.drawEllipse(box.adjusted(inset, inset, -inset, -inset));
    }
    return out;
}

QPixmap ringBadge(Ring ring, int size, qreal ratio)
{
    QPixmap out(QSize(size, size) * ratio);
    out.setDevicePixelRatio(ratio);
    out.fill(Qt::transparent);
    QPainter painter(&out);
    painter.setRenderHint(QPainter::Antialiasing);
    QColor pale = ringColor(ring);
    pale.setAlphaF(0.22);
    painter.setPen(Qt::NoPen);
    painter.setBrush(pale);
    painter.drawEllipse(QRectF(0, 0, size, size));
    painter.setBrush(ringColor(ring));
    const double dot = size * 0.36;
    painter.drawEllipse(QRectF((size - dot) / 2, (size - dot) / 2, dot, dot));
    return out;
}

QString pillStyle(const QColor &color, bool filled)
{
    QColor soft = color;
    soft.setAlphaF(0.16);
    return QStringLiteral("QLabel { padding: 2px 10px; border-radius: 10px; font-weight: 600; color: %1; background: %2; }")
        .arg(filled ? kNight.name() : color.name(), filled ? color.name() : soft.name(QColor::HexArgb));
}

QString subtitle(const Member &member)
{
    QStringList parts;
    if (!member.statusEmoji.isEmpty())
        parts << member.statusEmoji;
    if (!member.statusText.isEmpty())
        parts << member.statusText;
    return parts.isEmpty() ? lastActive(member.activityBucket) : parts.join(QLatin1Char(' '));
}

class Hero final : public QWidget
{
public:
    explicit Hero(Ring ring, QWidget *parent = nullptr) : QWidget(parent), m_color(ringColor(ring)) {}

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        QLinearGradient gradient(0, 0, 0, height());
        QColor top = m_color;
        top.setAlphaF(0.9);
        gradient.setColorAt(0, top.darker(160));
        gradient.setColorAt(0.55, kHearth);
        gradient.setColorAt(1, kNight);
        painter.fillRect(rect(), gradient);
    }

private:
    QColor m_color;
};

class ProfileView final : public QDialog
{
public:
    ProfileView(const forkmesh::market::NativePackageHost &host, const Ranked &person, const QString &viewer, QWidget *parent)
        : QDialog(parent), m_host(host), m_person(person), m_viewer(viewer)
    {
        setObjectName(QStringLiteral("peopleProfileDialog"));
        setWindowTitle(QStringLiteral("@%1").arg(person.member.name));
        setAttribute(Qt::WA_DeleteOnClose);
        setMinimumWidth(420);
        setMaximumWidth(480);
        const QColor color = ringColor(person.ring);
        auto *root = new QVBoxLayout(this);
        root->setContentsMargins(0, 0, 0, 0);
        root->setSpacing(0);

        auto *hero = new Hero(person.ring);
        auto *heroLayout = new QVBoxLayout(hero);
        heroLayout->setContentsMargins(24, 28, 24, 18);
        heroLayout->setSpacing(8);
        heroLayout->setAlignment(Qt::AlignHCenter);
        m_avatar = new QLabel;
        m_avatar->setAlignment(Qt::AlignCenter);
        m_avatar->setPixmap(circleAvatar(person.member.name, person.member.avatar, 88, color, devicePixelRatioF()));
        heroLayout->addWidget(m_avatar, 0, Qt::AlignHCenter);
        auto *name = new QLabel(QStringLiteral("@%1").arg(person.member.name));
        name->setStyleSheet(QStringLiteral("QLabel { color: white; font-size: 20px; font-weight: 700; }"));
        name->setAlignment(Qt::AlignCenter);
        heroLayout->addWidget(name);
        auto *badge = new QLabel(QStringLiteral("%1 ring").arg(ringTitle(person.ring)));
        badge->setStyleSheet(pillStyle(color, true));
        badge->setAccessibleName(QStringLiteral("%1 ring").arg(ringTitle(person.ring)));
        heroLayout->addWidget(badge, 0, Qt::AlignHCenter);
        auto *seen = new QLabel(lastActive(person.member.activityBucket));
        seen->setStyleSheet(QStringLiteral("QLabel { color: rgba(255,255,255,0.75); }"));
        seen->setAlignment(Qt::AlignCenter);
        heroLayout->addWidget(seen);
        root->addWidget(hero);

        auto *body = new QVBoxLayout;
        body->setContentsMargins(20, 16, 20, 16);
        body->setSpacing(12);
        const bool self = !viewer.isEmpty() && viewer == person.member.id();
        if (self) {
            auto *you = new QLabel(QStringLiteral("This is you."));
            you->setObjectName(QStringLiteral("mutedLabel"));
            body->addWidget(you);
        } else {
            auto *actions = new QHBoxLayout;
            actions->setSpacing(10);
            m_follow = new QPushButton(QStringLiteral("Follow"));
            m_follow->setObjectName(QStringLiteral("primaryButton"));
            m_follow->setMinimumHeight(40);
            m_follow->setCursor(Qt::PointingHandCursor);
            m_follow->setEnabled(false);
            actions->addWidget(m_follow, 1);
            auto *message = new QPushButton(QStringLiteral("Message"));
            message->setObjectName(QStringLiteral("ghostButton"));
            message->setMinimumHeight(40);
            message->setCursor(Qt::PointingHandCursor);
            message->setToolTip(QStringLiteral("Open a private conversation with @%1").arg(person.member.name));
            message->setEnabled(bool(host.openDirectMessage));
            actions->addWidget(message, 1);
            body->addLayout(actions);
            connect(m_follow, &QPushButton::clicked, this, [this] { toggleFollow(); });
            connect(message, &QPushButton::clicked, this, [this] {
                const QString target = m_person.member.name;
                const auto open = m_host.openDirectMessage;
                close();
                if (open)
                    open(target);
            });
        }
        m_error = new QLabel;
        m_error->setWordWrap(true);
        m_error->setStyleSheet(QStringLiteral("QLabel { color: #e5484d; }"));
        m_error->hide();
        body->addWidget(m_error);

        auto *stats = new QFrame;
        stats->setObjectName(QStringLiteral("peopleStats"));
        stats->setStyleSheet(QStringLiteral("QFrame#peopleStats { border: 1px solid rgba(128,128,128,0.35); border-radius: 10px; }"));
        auto *statsLayout = new QHBoxLayout(stats);
        statsLayout->setContentsMargins(8, 10, 8, 10);
        const auto stat = [&](const QString &label) {
            auto *column = new QVBoxLayout;
            auto *value = new QLabel(QStringLiteral("–"));
            value->setAlignment(Qt::AlignCenter);
            value->setStyleSheet(QStringLiteral("QLabel { font-size: 17px; font-weight: 700; }"));
            auto *caption = new QLabel(label);
            caption->setObjectName(QStringLiteral("mutedLabel"));
            caption->setAlignment(Qt::AlignCenter);
            column->addWidget(value);
            column->addWidget(caption);
            statsLayout->addLayout(column, 1);
            return value;
        };
        m_followers = stat(QStringLiteral("Followers"));
        m_following = stat(QStringLiteral("Following"));
        m_joined = stat(QStringLiteral("Joined"));
        body->addWidget(stats);

        m_about = new QLabel(QStringLiteral("Loading profile…"));
        m_about->setWordWrap(true);
        m_about->setTextFormat(Qt::PlainText);
        body->addWidget(m_about);
        m_location = new QLabel;
        m_location->setObjectName(QStringLiteral("mutedLabel"));
        m_location->hide();
        body->addWidget(m_location);

        auto *footer = new QHBoxLayout;
        auto *full = new QPushButton(QStringLiteral("View full profile"));
        full->setObjectName(QStringLiteral("ghostButton"));
        full->setCursor(Qt::PointingHandCursor);
        full->setEnabled(bool(host.openUserProfile));
        footer->addWidget(full);
        footer->addStretch(1);
        auto *closeButton = new QPushButton(QStringLiteral("Close"));
        closeButton->setObjectName(QStringLiteral("ghostButton"));
        footer->addWidget(closeButton);
        body->addLayout(footer);
        root->addLayout(body);
        connect(closeButton, &QPushButton::clicked, this, &QDialog::close);
        connect(full, &QPushButton::clicked, this, [this] {
            const QString target = m_person.member.name;
            const auto open = m_host.openUserProfile;
            close();
            if (open)
                open(target);
        });
        load();
    }

private:
    void load()
    {
        if (!m_host.request || !validName(m_person.member.id()))
            return;
        QPointer<ProfileView> self(this);
        m_host.request(QByteArrayLiteral("GET"), profilePath(m_person.member.id(), m_viewer), {},
                       [self](bool ok, const QJsonObject &payload, const QString &) {
                           if (!self)
                               return;
                           if (!ok) {
                               self->m_about->setText(QStringLiteral("Could not load this profile."));
                               return;
                           }
                           self->m_profile = parseProfile(payload, self->m_person.member.id());
                           self->fill(*self->m_profile);
                       });
    }

    void fill(const Profile &profile)
    {
        if (!profile.avatar.isEmpty())
            m_avatar->setPixmap(circleAvatar(m_person.member.name, profile.avatar, 88, ringColor(m_person.ring), devicePixelRatioF()));
        if (profile.isPrivate) {
            m_about->setText(QStringLiteral("This profile is private."));
            return;
        }
        m_followers->setText(QLocale().toString(profile.followers));
        m_following->setText(QLocale().toString(profile.following));
        m_joined->setText(profile.createdAt > 0
                              ? QLocale().toString(QDateTime::fromMSecsSinceEpoch(profile.createdAt).date(), QStringLiteral("MMM yyyy"))
                              : QStringLiteral("–"));
        QStringList about;
        const QString status = QStringList{profile.statusEmoji, profile.statusText}.join(QLatin1Char(' ')).trimmed();
        if (!status.isEmpty())
            about << status;
        if (!profile.bio.isEmpty())
            about << profile.bio;
        m_about->setText(about.join(QStringLiteral("\n\n")));
        m_about->setVisible(!about.isEmpty());
        m_location->setText(QStringLiteral("Location · %1").arg(profile.location));
        m_location->setVisible(!profile.location.isEmpty());
        updateFollow();
    }

    void updateFollow()
    {
        if (!m_follow || !m_profile)
            return;
        const bool following = m_profile->isFollowing;
        m_follow->setText(m_busy ? QStringLiteral("…") : following ? QStringLiteral("Following ✓") : QStringLiteral("Follow"));
        m_follow->setObjectName(following ? QStringLiteral("ghostButton") : QStringLiteral("primaryButton"));
        m_follow->style()->unpolish(m_follow);
        m_follow->style()->polish(m_follow);
        m_follow->setAccessibleName(QStringLiteral("%1 @%2").arg(following ? QStringLiteral("Unfollow") : QStringLiteral("Follow"), m_person.member.name));
        m_follow->setEnabled(!m_busy && !m_viewer.isEmpty() && bool(m_host.request));
        if (m_viewer.isEmpty())
            m_follow->setToolTip(QStringLiteral("Sign in to your ForkMesh account to follow people."));
    }

    void toggleFollow()
    {
        if (!m_profile || m_busy || !m_host.request)
            return;
        const bool follow = !m_profile->isFollowing;
        m_busy = true;
        m_error->hide();
        updateFollow();
        QPointer<ProfileView> self(this);
        m_host.request(follow ? QByteArrayLiteral("POST") : QByteArrayLiteral("DELETE"), followPath(m_person.member.id()), {},
                       [self, follow](bool ok, const QJsonObject &payload, const QString &error) {
                           if (!self || !self->m_profile)
                               return;
                           self->m_busy = false;
                           if (ok) {
                               self->m_profile->isFollowing = payload.contains(QStringLiteral("isFollowing"))
                                   ? payload.value(QStringLiteral("isFollowing")).toBool() : follow;
                               self->m_profile->followers = payload.contains(QStringLiteral("followers"))
                                   ? payload.value(QStringLiteral("followers")).toInt()
                                   : std::max(0, self->m_profile->followers + (follow ? 1 : -1));
                               self->m_followers->setText(QLocale().toString(self->m_profile->followers));
                           } else {
                               self->m_error->setText(followError(payload, error, follow));
                               self->m_error->show();
                           }
                           self->updateFollow();
                       });
    }

    forkmesh::market::NativePackageHost m_host;
    Ranked m_person;
    QString m_viewer;
    std::optional<Profile> m_profile;
    bool m_busy = false;
    QLabel *m_avatar = nullptr;
    QPushButton *m_follow = nullptr;
    QLabel *m_error = nullptr;
    QLabel *m_followers = nullptr;
    QLabel *m_following = nullptr;
    QLabel *m_joined = nullptr;
    QLabel *m_about = nullptr;
    QLabel *m_location = nullptr;
};

} // namespace

CampfireView::CampfireView(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("peopleCampfire"));
    setMouseTracking(true);
    setMinimumSize(300, 300);
    setMaximumWidth(kMaxSide);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    setAccessibleName(QStringLiteral("Campfire of people, from the most active in the centre to the quietest at the edge"));
    m_flicker = new QTimer(this);
    m_flicker->setInterval(60);
    connect(m_flicker, &QTimer::timeout, this, [this] {
        m_phase += 0.06;
        const QRectF box = square();
        const double s = side() * 0.24;
        update(QRectF(box.center() - QPointF(s, s), QSizeF(2 * s, 2 * s)).toAlignedRect());
    });
}

QSize CampfireView::sizeHint() const
{
    return {480, 480};
}

int CampfireView::heightForWidth(int width) const
{
    return std::min(width, kMaxSide);
}

void CampfireView::setPeople(const QList<Section> &all, std::optional<Ring> focus, const QString &viewer)
{
    m_sections = all;
    m_focus = focus;
    m_viewer = viewer;
    m_hover = -1;
    layoutHits();
    update();
}

QRectF CampfireView::square() const
{
    const double s = side();
    return QRectF((width() - s) / 2, (height() - s) / 2, s, s);
}

double CampfireView::side() const
{
    return std::min({double(width()), double(height()), double(kMaxSide)});
}

double CampfireView::seatSize() const
{
    return std::clamp(side() * 0.066, 22.0, 36.0);
}

double CampfireView::labelWidth() const
{
    return side() * 0.2;
}

void CampfireView::layoutHits()
{
    m_hits.clear();
    const double s = side();
    if (s <= 0)
        return;
    const QPointF origin = square().topLeft();
    const double seat = seatSize();
    m_seats = seats(m_sections, s, seat, labelWidth());
    for (const Seat &placed : m_seats) {
        Hit hit;
        hit.rect = QRectF(origin + placed.center - QPointF(seat / 2, seat / 2), QSizeF(seat, seat));
        hit.person = placed.person;
        hit.ring = placed.ring;
        m_hits.append(hit);
    }
    for (Ring each : allRings()) {
        Hit hit;
        const double r = radius(ringOrder(each), s);
        hit.rect = QRectF(origin + QPointF(s / 2 - labelWidth() / 2, s / 2 - r - 10), QSizeF(labelWidth(), 20));
        hit.ring = each;
        hit.label = true;
        m_hits.append(hit);
    }
}

const CampfireView::Hit *CampfireView::hitAt(const QPointF &point) const
{
    for (int i = int(m_hits.size()) - 1; i >= 0; --i) {
        if (m_hits.at(i).rect.contains(point))
            return &m_hits.at(i);
    }
    return nullptr;
}

QPixmap CampfireView::avatar(const Member &member, int size)
{
    const QString key = QStringLiteral("%1:%2").arg(member.id()).arg(size);
    auto found = m_avatars.constFind(key);
    if (found != m_avatars.constEnd())
        return *found;
    const QPixmap made = circleAvatar(member.name, member.avatar, size, QColor(), devicePixelRatioF());
    m_avatars.insert(key, made);
    return made;
}

void CampfireView::paintFire(QPainter &painter, const QRectF &box)
{
    const double s = box.width();
    const QPointF base(box.center().x(), box.top() + s * 0.95);
    QRadialGradient glow(box.center(), s * (0.68 + 0.04 * std::sin(m_phase * 2.1)));
    glow.setColorAt(0, QColor(255, 138, 43, 140));
    glow.setColorAt(1, QColor(255, 138, 43, 0));
    painter.setPen(Qt::NoPen);
    painter.setBrush(glow);
    painter.drawEllipse(box.center(), s * 0.8, s * 0.8);
    const auto at = [&](double x, double y) { return QPointF(box.left() + s * x, box.top() + s * y); };
    QPainterPath flame;
    flame.moveTo(at(.5, .05));
    flame.cubicTo(at(.66, .3), at(.9, .42), at(.84, .7));
    flame.cubicTo(at(.8, .88), at(.66, .95), at(.5, .95));
    flame.cubicTo(at(.32, .95), at(.16, .86), at(.16, .68));
    flame.cubicTo(at(.16, .52), at(.28, .44), at(.34, .3));
    flame.cubicTo(at(.37, .44), at(.43, .47), at(.5, .5));
    flame.cubicTo(at(.5, .32), at(.44, .18), at(.5, .05));
    QPainterPath core;
    core.moveTo(at(.5, .45));
    core.cubicTo(at(.6, .6), at(.72, .68), at(.69, .8));
    core.cubicTo(at(.67, .9), at(.6, .94), at(.5, .94));
    core.cubicTo(at(.4, .94), at(.32, .88), at(.32, .8));
    core.cubicTo(at(.32, .68), at(.42, .62), at(.5, .45));
    painter.save();
    painter.translate(base);
    painter.scale(1 + 0.04 * std::sin(m_phase * 5.3), 1 + 0.07 * std::sin(m_phase * 7.1));
    painter.translate(-base);
    QLinearGradient fill(box.topLeft(), box.bottomLeft());
    fill.setColorAt(0, QColor(0xff, 0xd8, 0x5a));
    fill.setColorAt(0.6, QColor(0xff, 0x7a, 0x1f));
    fill.setColorAt(1, QColor(0xe2, 0x43, 0x0f));
    painter.setBrush(fill);
    painter.drawPath(flame);
    painter.restore();
    painter.save();
    painter.translate(base);
    painter.scale(1, 1 + 0.1 * std::sin(m_phase * 9.7));
    painter.translate(-base);
    painter.setBrush(QColor(0xff, 0xf3, 0xb0));
    painter.drawPath(core);
    painter.restore();
}

void CampfireView::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF box = square();
    const double s = box.width();
    if (s <= 0)
        return;
    QPainterPath frame;
    frame.addRoundedRect(box, 22, 22);
    painter.setClipPath(frame);
    QRadialGradient sky(box.center(), s * 0.62);
    sky.setColorAt(0, kHearth);
    sky.setColorAt(0.45, QColor(0x1f, 0x12, 0x14));
    sky.setColorAt(1, kNight);
    painter.fillRect(box, sky);

    for (Ring each : allRings()) {
        const double r = radius(ringOrder(each), s);
        const bool dim = m_focus && *m_focus != each;
        QColor band = ringColor(each);
        band.setAlphaF(dim ? 0.02 : 0.08);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(band, s * 0.06));
        painter.drawEllipse(box.center(), r, r);
        QColor line = ringColor(each);
        line.setAlphaF(dim ? 0.12 : 0.55);
        QPen pen(line, m_focus == each ? 3 : 1.5);
        if (each == Ring::Embers)
            pen.setDashPattern({3, 4});
        painter.setPen(pen);
        painter.drawEllipse(box.center(), r, r);
    }

    const double fire = s * 0.2;
    paintFire(painter, QRectF(box.center() - QPointF(fire / 2, fire / 2), QSizeF(fire, fire)));

    const double seat = seatSize();
    for (int i = 0; i < m_hits.size(); ++i) {
        const Hit &hit = m_hits.at(i);
        if (hit.label)
            continue;
        const QColor color = ringColor(hit.ring);
        const bool dim = m_focus && *m_focus != hit.ring;
        if (hit.person) {
            painter.drawPixmap(hit.rect.topLeft(), avatar(hit.person->member, int(seat)));
            const bool mine = hit.person->member.id() == m_viewer;
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(mine ? QColor(Qt::white) : color, 2.5));
            painter.drawEllipse(hit.rect.adjusted(1, 1, -1, -1));
        } else {
            painter.setPen(Qt::NoPen);
            painter.setBrush(color);
            painter.drawEllipse(hit.rect);
            QFont font = painter.font();
            font.setBold(true);
            font.setPixelSize(int(seat * 0.32));
            painter.setFont(font);
            painter.setPen(kNight);
            painter.drawText(hit.rect, Qt::AlignCenter, QStringLiteral("+%1").arg(m_seats.value(i).overflow));
        }
        if (dim) {
            QColor veil = kNight;
            veil.setAlphaF(0.72);
            painter.setPen(Qt::NoPen);
            painter.setBrush(veil);
            painter.drawEllipse(hit.rect);
        }
        if (i == m_hover) {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(Qt::white, 2));
            painter.drawEllipse(hit.rect.adjusted(-3, -3, 3, 3));
        }
    }

    QFont labelFont = font();
    labelFont.setPixelSize(11);
    labelFont.setBold(true);
    painter.setFont(labelFont);
    const QFontMetricsF metrics(labelFont);
    for (const Hit &hit : m_hits) {
        if (!hit.label)
            continue;
        const QColor color = ringColor(hit.ring);
        const bool on = m_focus == hit.ring;
        int total = 0;
        for (const Section &section : m_sections) {
            if (section.ring == hit.ring)
                total = int(section.people.size());
        }
        const QString text = QStringLiteral("%1  %2").arg(ringTitle(hit.ring)).arg(total);
        const double w = std::min(hit.rect.width(), metrics.horizontalAdvance(text) + 16);
        const QRectF pill(hit.rect.center().x() - w / 2, hit.rect.top() + 1, w, 18);
        QColor fill = on ? color : kNight;
        fill.setAlphaF(on ? 1.0 : 0.88);
        QColor edge = color;
        edge.setAlphaF(0.75);
        painter.setBrush(fill);
        painter.setPen(QPen(edge, 1));
        painter.drawRoundedRect(pill, 9, 9);
        painter.setPen(on ? kNight : color);
        painter.drawText(pill, Qt::AlignCenter, text);
    }
}

void CampfireView::mouseMoveEvent(QMouseEvent *event)
{
    const Hit *hit = hitAt(event->position());
    const int index = hit ? int(hit - m_hits.constData()) : -1;
    setCursor(hit ? Qt::PointingHandCursor : Qt::ArrowCursor);
    if (index != m_hover) {
        m_hover = hit && !hit->label ? index : -1;
        update();
    }
    if (hit && hit->person) {
        QToolTip::showText(event->globalPosition().toPoint(),
                           QStringLiteral("@%1 · %2\n%3").arg(hit->person->member.name, ringTitle(hit->ring), subtitle(hit->person->member)), this);
    } else if (hit) {
        QToolTip::showText(event->globalPosition().toPoint(), QStringLiteral("%1: %2").arg(ringTitle(hit->ring), ringBlurb(hit->ring)), this);
    } else {
        QToolTip::hideText();
    }
}

void CampfireView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    const Hit *hit = hitAt(event->position());
    if (!hit)
        return;
    if (hit->person && onPerson)
        onPerson(*hit->person);
    else if (!hit->person && onRing)
        onRing(hit->ring);
}

void CampfireView::leaveEvent(QEvent *)
{
    m_hover = -1;
    update();
}

void CampfireView::showEvent(QShowEvent *)
{
    layoutHits();
    m_flicker->start();
}

void CampfireView::hideEvent(QHideEvent *)
{
    m_flicker->stop();
}

PeopleFeature::PeopleFeature(const forkmesh::market::NativePackageHost &host, QWidget *parent)
    : NativePackageWidget(parent), m_host(host)
{
    setObjectName(QStringLiteral("peoplePage"));
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(24, 20, 24, 20);
    root->setSpacing(14);

    auto *header = new QHBoxLayout;
    auto *titles = new QVBoxLayout;
    titles->setSpacing(4);
    auto *title = new QLabel(QStringLiteral("People"));
    title->setObjectName(QStringLiteral("pageTitle"));
    titles->addWidget(title);
    auto *lead = new QLabel(QStringLiteral("Everyone around the fire. The most active sit closest to the flames; quieter members sit on the outer rings. Pick someone to see their profile, follow them or send a message."));
    lead->setObjectName(QStringLiteral("mutedLabel"));
    lead->setWordWrap(true);
    titles->addWidget(lead);
    header->addLayout(titles, 1);
    m_search = new QLineEdit;
    m_search->setPlaceholderText(QStringLiteral("Find a person"));
    m_search->setClearButtonEnabled(true);
    m_search->setMaximumWidth(280);
    m_search->setAccessibleName(QStringLiteral("Find a person"));
    header->addWidget(m_search, 0, Qt::AlignBottom);
    root->addLayout(header);

    m_status = new QLabel(QStringLiteral("Gathering everyone around the fire…"));
    m_status->setObjectName(QStringLiteral("mutedLabel"));
    m_status->setWordWrap(true);
    root->addWidget(m_status);

    auto *body = new QHBoxLayout;
    body->setSpacing(20);
    auto *left = new QVBoxLayout;
    left->setSpacing(12);
    m_fire = new CampfireView;
    m_fire->onPerson = [this](const Ranked &person) { openPerson(person); };
    m_fire->onRing = [this](Ring ring) { toggleRing(ring); };
    left->addWidget(m_fire, 0, Qt::AlignHCenter);
    m_legend = new QWidget;
    auto *legend = new QGridLayout(m_legend);
    legend->setContentsMargins(0, 0, 0, 0);
    legend->setSpacing(8);
    left->addWidget(m_legend);
    left->addStretch(1);
    body->addLayout(left, 0);

    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *list = new QWidget;
    m_sections = new QVBoxLayout(list);
    m_sections->setContentsMargins(0, 0, 4, 0);
    m_sections->setSpacing(14);
    scroll->setWidget(list);
    body->addWidget(scroll, 1);
    root->addLayout(body, 1);

    connect(m_search, &QLineEdit::textChanged, this, [this](const QString &text) {
        m_query = text;
        render();
    });
}

QString PeopleFeature::viewer() const
{
    return m_host.accountName ? m_host.accountName().trimmed().toLower() : QString();
}

void PeopleFeature::refresh()
{
    if (m_loading || !m_host.request)
        return;
    m_loading = true;
    if (!m_loaded)
        m_status->setText(QStringLiteral("Gathering everyone around the fire…"));
    QPointer<PeopleFeature> self(this);
    m_host.request(QByteArrayLiteral("GET"), directoryPath(), {}, [self](bool ok, const QJsonObject &payload, const QString &error) {
        if (!self)
            return;
        self->m_loading = false;
        if (!ok) {
            self->m_status->setText(QStringLiteral("Could not load people: %1").arg(error.isEmpty() ? QStringLiteral("try again later.") : error));
            self->m_status->show();
            return;
        }
        self->m_members = parseDirectory(payload);
        self->m_loaded = true;
        self->m_status->hide();
        self->render();
        if (self->m_host.searchIndexChanged)
            self->m_host.searchIndexChanged();
    });
}

void PeopleFeature::render()
{
    const QList<Section> all = sections(m_members, QDateTime::currentMSecsSinceEpoch());
    m_fire->setPeople(all, m_focus, viewer());
    renderLegend(all);
    renderSections(filtered(all, m_query, m_focus));
}

void PeopleFeature::renderLegend(const QList<Section> &all)
{
    auto *grid = static_cast<QGridLayout *>(m_legend->layout());
    while (QLayoutItem *item = grid->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    int index = 0;
    for (const Section &section : all) {
        const QColor color = ringColor(section.ring);
        const bool on = m_focus == section.ring;
        auto *chip = new QPushButton(QStringLiteral("%1  %2").arg(ringTitle(section.ring)).arg(section.people.size()));
        chip->setIcon(QIcon(ringBadge(section.ring, 14, devicePixelRatioF())));
        chip->setCheckable(true);
        chip->setChecked(on);
        chip->setCursor(Qt::PointingHandCursor);
        chip->setMinimumHeight(34);
        chip->setToolTip(ringBlurb(section.ring));
        chip->setAccessibleName(QStringLiteral("%1, %2").arg(ringTitle(section.ring)).arg(section.people.size()));
        chip->setStyleSheet(QStringLiteral(
            "QPushButton { border-radius: 17px; padding: 0 14px; font-weight: 600; border: 1px solid %1; background: transparent; }"
            "QPushButton:checked { background: %1; color: %2; }")
                                .arg(color.name(), kNight.name()));
        const Ring ring = section.ring;
        connect(chip, &QPushButton::clicked, this, [this, ring] { toggleRing(ring); });
        grid->addWidget(chip, index / 3, index % 3);
        ++index;
    }
}

void PeopleFeature::renderSections(const QList<Section> &visible)
{
    while (QLayoutItem *item = m_sections->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    const QString mine = viewer();
    for (const Section &section : visible) {
        const QColor color = ringColor(section.ring);
        auto *card = new QFrame;
        card->setObjectName(QStringLiteral("peopleSection"));
        QColor soft = color;
        soft.setAlphaF(0.08);
        card->setStyleSheet(QStringLiteral("QFrame#peopleSection { border: 1px solid rgba(128,128,128,0.35); border-left: 4px solid %1; border-radius: 10px; background: %2; }")
                                .arg(color.name(), soft.name(QColor::HexArgb)));
        auto *layout = new QVBoxLayout(card);
        layout->setContentsMargins(14, 12, 14, 12);
        layout->setSpacing(8);
        auto *head = new QHBoxLayout;
        head->setSpacing(10);
        auto *badge = new QLabel;
        badge->setPixmap(ringBadge(section.ring, 30, devicePixelRatioF()));
        head->addWidget(badge);
        auto *texts = new QVBoxLayout;
        texts->setSpacing(0);
        auto *name = new QLabel(ringTitle(section.ring));
        name->setObjectName(QStringLiteral("sectionTitle"));
        name->setAccessibleName(QStringLiteral("%1 ring").arg(ringTitle(section.ring)));
        texts->addWidget(name);
        auto *blurb = new QLabel(ringBlurb(section.ring));
        blurb->setObjectName(QStringLiteral("mutedLabel"));
        texts->addWidget(blurb);
        head->addLayout(texts, 1);
        auto *count = new QLabel(QString::number(section.people.size()));
        count->setStyleSheet(pillStyle(color, false));
        head->addWidget(count, 0, Qt::AlignVCenter);
        layout->addLayout(head);
        if (section.people.isEmpty()) {
            auto *empty = new QLabel(m_query.trimmed().isEmpty() ? QStringLiteral("No one in this ring right now.") : QStringLiteral("No one in this ring matches."));
            empty->setObjectName(QStringLiteral("mutedLabel"));
            layout->addWidget(empty);
        } else {
            auto *grid = new QGridLayout;
            grid->setHorizontalSpacing(6);
            grid->setVerticalSpacing(2);
            int index = 0;
            for (const Ranked &person : section.people) {
                auto *button = new QToolButton;
                button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
                button->setAutoRaise(true);
                button->setCursor(Qt::PointingHandCursor);
                button->setIcon(QIcon(circleAvatar(person.member.name, person.member.avatar, kListAvatar, color, devicePixelRatioF())));
                button->setIconSize(QSize(kListAvatar, kListAvatar));
                const QString label = person.member.id() == mine ? QStringLiteral("@%1 (you)").arg(person.member.name) : QStringLiteral("@%1").arg(person.member.name);
                button->setText(QStringLiteral("%1\n%2").arg(label, subtitle(person.member)));
                button->setToolTip(QStringLiteral("Open @%1's profile").arg(person.member.name));
                button->setAccessibleName(QStringLiteral("@%1, %2").arg(person.member.name, ringTitle(section.ring)));
                button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
                button->setMinimumHeight(48);
                button->setStyleSheet(QStringLiteral("QToolButton { text-align: left; padding: 4px 8px; border-radius: 8px; }"));
                connect(button, &QToolButton::clicked, this, [this, person] { openPerson(person); });
                grid->addWidget(button, index / 2, index % 2);
                ++index;
            }
            layout->addLayout(grid);
        }
        m_sections->addWidget(card);
    }
    m_sections->addStretch(1);
}

void PeopleFeature::toggleRing(Ring ring)
{
    m_focus = m_focus == ring ? std::nullopt : std::optional<Ring>(ring);
    render();
}

void PeopleFeature::openPerson(const Ranked &person)
{
    if (m_dialog)
        m_dialog->close();
    auto *dialog = new ProfileView(m_host, person, viewer(), this);
    m_dialog = dialog;
    dialog->open();
}

QList<forkmesh::market::NativePackageSearchHit> PeopleFeature::searchHits(const QString &query, int limit) const
{
    QList<forkmesh::market::NativePackageSearchHit> hits;
    QString needle = query.trimmed().toLower();
    while (needle.startsWith(QLatin1Char('@')))
        needle.remove(0, 1);
    if (needle.isEmpty())
        return hits;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (const Member &member : m_members) {
        if (hits.size() >= limit)
            break;
        if (!member.id().contains(needle))
            continue;
        hits.append({member.id(), QStringLiteral("@%1").arg(member.name),
                     QStringLiteral("%1 ring · %2").arg(ringTitle(ring(member, now)), lastActive(member.activityBucket))});
    }
    return hits;
}

void PeopleFeature::activateSearchHit(const QString &id)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (const Member &member : m_members) {
        if (member.id() == id) {
            openPerson({member, ring(member, now), score(member)});
            return;
        }
    }
}

void PeopleFeature::setHostSearchQuery(const QString &query)
{
    if (m_search->text() != query)
        m_search->setText(query);
}

} // namespace forkmesh::people

namespace {
const forkmesh::market::NativePackageRegistration peopleRegistration({
    QStringLiteral("builtin:people"),
    {QStringLiteral("extension.native")},
    QStringLiteral("People"),
    QStringLiteral("people"),
    QStringLiteral("/api/accounts"),
    QStringLiteral("forkmesh-people-request-v1"),
    [](const forkmesh::market::NativePackageHost &host, QWidget *parent) {
        return new forkmesh::people::PeopleFeature(host, parent);
    },
});
} // namespace
