#include "DjDialogs.h"
#include "DjModel.h"
#include "DjWidgets.h"

#include <QCheckBox>
#include <QCompleter>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QStringListModel>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace forkmesh::djstudio {

namespace {

QString dialogStyle()
{
    return QStringLiteral(
        "QDialog{background:#1b1c1f;color:#e6e8eb;}QLabel{color:#e6e8eb;}"
        "QListWidget{background:#111214;color:#e6e8eb;border:1px solid #3c3e43;}"
        "QListWidget::item{padding:4px;}QListWidget::item:selected{background:#1d4f78;}"
        "QLineEdit{background:#111214;color:#e6e8eb;border:1px solid #3c3e43;border-radius:3px;padding:4px;}"
        "QPushButton{background:#31333a;color:#e6e8eb;border:1px solid #3c3e43;border-radius:3px;padding:5px 10px;}"
        "QPushButton:hover{background:#3a3d44;}QPushButton:disabled{color:#5a5e66;}");
}

QColor nameColor(const QString &name)
{
    const uint hash = qHash(name);
    return QColor::fromHsv(int(hash % 360), 150, 220);
}

QColor statusColor(DjPersonStatus status)
{
    switch (status) {
    case DjPersonStatus::Studio:
        return QColor(0x3d, 0xd6, 0x5c);
    case DjPersonStatus::Listening:
        return QColor(0x23, 0xa6, 0xff);
    case DjPersonStatus::Idle:
        return QColor(0x8c, 0x91, 0x99);
    case DjPersonStatus::Away:
        return QColor(0x55, 0x58, 0x5e);
    case DjPersonStatus::Invited:
        return QColor(0xff, 0xc4, 0x1c);
    }
    return QColor(0x8c, 0x91, 0x99);
}

}

DjReactionOverlay::DjReactionOverlay(DjModel *model, QWidget *parent) : QWidget(parent), m_model(model)
{
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
}

void DjReactionOverlay::paintEvent(QPaintEvent *)
{
    const auto &effects = m_model->effects();
    if (effects.isEmpty())
        return;
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const double at = DjModel::now();
    const double w = width();
    const double h = height();
    for (const DjEffect &effect : effects) {
        const DjReaction &r = effect.reaction;
        const double total = r.seconds();
        const double t = std::clamp((at - effect.born) / total, 0.0, 1.0);
        const double fade = t < 0.12 ? t / 0.12 : t > 0.8 ? (1 - t) / 0.2 : 1;
        DjRandom random(quint64(effect.id) * 7919 + 17);
        const QColor tint = QColor::fromHsvF(r.hue, 0.7, 1.0);
        auto emoji = [&](double x, double y, double size, double alpha, double angle = 0) {
            painter.save();
            painter.setOpacity(std::clamp(alpha, 0.0, 1.0));
            painter.translate(x, y);
            painter.rotate(angle);
            QFont f = painter.font();
            f.setPixelSize(int(std::max(8.0, size)));
            painter.setFont(f);
            painter.drawText(QRectF(-size, -size, size * 2, size * 2), Qt::AlignCenter, r.emoji);
            painter.restore();
        };
        switch (r.style) {
        case DjReactionStyle::Rise:
            for (int i = 0; i < 14; ++i) {
                const double x = random.unit() * w;
                const double delay = random.unit() * 0.4;
                const double local = std::clamp((t - delay) / (1 - delay), 0.0, 1.0);
                emoji(x + std::sin(local * 9 + i) * 24, h - local * (h + 60), 28 + random.unit() * 22, fade * (local > 0 ? 1 : 0));
            }
            break;
        case DjReactionStyle::Rain:
            for (int i = 0; i < 18; ++i) {
                const double x = random.unit() * w;
                const double delay = random.unit() * 0.5;
                const double local = std::clamp((t - delay) / (1 - delay), 0.0, 1.0);
                emoji(x, -40 + local * (h + 80), 20 + random.unit() * 18, fade * (local > 0 ? 1 : 0), local * 180 * (i % 2 ? 1 : -1));
            }
            break;
        case DjReactionStyle::Burst:
            for (int i = 0; i < 16; ++i) {
                const double angle = i / 16.0 * 2 * M_PI + random.unit() * 0.3;
                const double reach = t * std::min(w, h) * 0.45;
                emoji(w / 2 + std::cos(angle) * reach, h / 2 + std::sin(angle) * reach, 26 + random.unit() * 14, fade);
            }
            break;
        case DjReactionStyle::Zoom:
            emoji(w / 2, h / 2, 40 + t * std::min(w, h) * 0.35, fade);
            break;
        case DjReactionStyle::Spin:
            for (int i = 0; i < 8; ++i) {
                const double angle = i / 8.0 * 2 * M_PI + t * 2 * M_PI;
                const double reach = std::min(w, h) * 0.22;
                emoji(w / 2 + std::cos(angle) * reach, h / 2 + std::sin(angle) * reach, 34, fade, t * 720);
            }
            break;
        case DjReactionStyle::Orbit:
            for (int i = 0; i < 10; ++i) {
                const double angle = i / 10.0 * 2 * M_PI + t * 3 * M_PI;
                const double reach = std::min(w, h) * (0.15 + 0.2 * t);
                emoji(w / 2 + std::cos(angle) * reach * 1.6, h / 2 + std::sin(angle) * reach, 28, fade);
            }
            break;
        case DjReactionStyle::Wave:
            for (int i = 0; i < 12; ++i) {
                const double x = (i + 0.5) / 12 * w;
                emoji(x, h * 0.6 + std::sin(t * 12 + i * 0.7) * 40, 32, fade);
            }
            break;
        case DjReactionStyle::Confetti:
            for (int i = 0; i < 70; ++i) {
                const double x = random.unit() * w;
                const double delay = random.unit() * 0.4;
                const double local = std::clamp((t - delay) / (1 - delay), 0.0, 1.0);
                painter.save();
                painter.setOpacity(fade);
                painter.translate(x + std::sin(local * 8 + i) * 30, -20 + local * (h + 40));
                painter.rotate(local * 540 + i * 33);
                painter.fillRect(QRectF(-4, -2, 8, 4), QColor::fromHsvF(random.unit(), 0.8, 1));
                painter.restore();
            }
            emoji(w / 2, h * 0.35, 60, fade);
            break;
        case DjReactionStyle::Fireworks:
            for (int burst = 0; burst < 4; ++burst) {
                const double cx = (0.2 + random.unit() * 0.6) * w;
                const double cy = (0.15 + random.unit() * 0.4) * h;
                const double start = burst * 0.18;
                const double local = std::clamp((t - start) / 0.5, 0.0, 1.0);
                if (local <= 0)
                    continue;
                const QColor c = QColor::fromHsvF(random.unit(), 0.8, 1);
                for (int i = 0; i < 24; ++i) {
                    const double angle = i / 24.0 * 2 * M_PI;
                    const double reach = local * 90;
                    painter.setOpacity((1 - local) * fade);
                    painter.setPen(Qt::NoPen);
                    painter.setBrush(c);
                    painter.drawEllipse(QPointF(cx + std::cos(angle) * reach, cy + std::sin(angle) * reach + local * local * 30), 3, 3);
                }
            }
            painter.setOpacity(1);
            emoji(w / 2, h * 0.7, 46, fade);
            break;
        case DjReactionStyle::Flash: {
            const double pulse = 0.5 + 0.5 * std::sin(t * 2 * M_PI * 2.5);
            QColor c = tint;
            c.setAlphaF(0.18 * pulse * fade);
            painter.fillRect(rect(), c);
            emoji(w / 2, h / 2, 80, fade);
            break;
        }
        }
        if (!effect.sender.isEmpty()) {
            painter.setOpacity(fade);
            painter.setPen(Qt::white);
            painter.setFont(theme::font(14, true));
            painter.drawText(QRectF(0, 8, w, 24), Qt::AlignHCenter, QStringLiteral("%1 %2").arg(effect.sender, r.emoji));
        }
        painter.setOpacity(1);
    }
}

DjPeopleStrip::DjPeopleStrip(DjModel *model, QWidget *parent) : QWidget(parent), m_model(model)
{
    setFixedHeight(24);
    setCursor(Qt::PointingHandCursor);
    setToolTip(QStringLiteral("Jam session: share decks 1 and 2 and the queue with other ForkMesh users"));
    refresh();
}

void DjPeopleStrip::refresh()
{
    const auto &jam = m_model->jam();
    const int count = jam ? int(jam->members.size()) : 0;
    setFixedWidth(std::max(64, 26 + count * 22 + 30));
    QStringList tip;
    if (jam) {
        tip << QStringLiteral("%1 (%2)").arg(jam->title, jam->joined() ? QStringLiteral("joined") : QStringLiteral("invited"));
        for (const DjJamMember &member : jam->members)
            tip << QStringLiteral("%1: %2%3").arg(member.name, statusLabel(m_model->status(member)),
                                                  member.name == jam->owner ? QStringLiteral(" (host)") : QString());
    } else {
        tip << QStringLiteral("Start or join a jam session");
        if (m_model->pendingInvites() > 0)
            tip << QStringLiteral("%1 invitation(s) waiting").arg(m_model->pendingInvites());
    }
    setToolTip(tip.join(QLatin1Char('\n')));
    update();
}

void DjPeopleStrip::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const auto &jam = m_model->jam();
    painter.setPen(QPen(jam && jam->joined() ? QColor(0x3d, 0xd6, 0x5c) : theme::line(), 1));
    painter.setBrush(theme::raised());
    painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 11, 11);
    painter.setPen(theme::text());
    painter.setFont(theme::font(9, true));
    painter.drawText(QRectF(8, 0, 30, height()), Qt::AlignVCenter | Qt::AlignLeft, QStringLiteral("JAM"));
    double x = 36;
    if (jam) {
        for (const DjJamMember &member : jam->members) {
            const DjPersonStatus status = m_model->status(member);
            const QRectF circle(x, 3, 18, 18);
            painter.setPen(QPen(statusColor(status), 2, status == DjPersonStatus::Invited ? Qt::DashLine : Qt::SolidLine));
            painter.setBrush(nameColor(member.name).darker(status == DjPersonStatus::Away ? 220 : 130));
            painter.drawEllipse(circle);
            painter.setPen(Qt::white);
            painter.drawText(circle, Qt::AlignCenter, member.name.left(1).toUpper());
            if (member.name == jam->owner) {
                painter.setPen(QColor(0xff, 0xc4, 0x1c));
                painter.drawText(QRectF(x + 11, -2, 10, 10), Qt::AlignCenter, QStringLiteral("♛"));
            }
            x += 22;
        }
    }
    if (m_model->pendingInvites() > 0) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0xff, 0x3d, 0x3d));
        painter.drawEllipse(QPointF(width() - 8, 6), 4, 4);
    }
}

void DjPeopleStrip::mouseReleaseEvent(QMouseEvent *)
{
    if (clicked)
        clicked();
}

DjJamDialog::DjJamDialog(DjModel *model, std::function<QStringList()> directory, QWidget *parent)
    : QDialog(parent), m_model(model), m_directory(std::move(directory))
{
    setWindowTitle(QStringLiteral("Jam Session"));
    setStyleSheet(dialogStyle());
    resize(440, 460);
    auto *column = new QVBoxLayout(this);
    m_heading = new QLabel(this);
    m_heading->setWordWrap(true);
    m_heading->setStyleSheet(QStringLiteral("font-size:15px;font-weight:bold;"));
    column->addWidget(m_heading);
    auto *explain = new QLabel(QStringLiteral("Everyone in a jam shares decks 1 and 2 and the queue. Each computer or phone plays "
                                              "its own copy of the songs; volume, EQ and the crossfader stay your own."),
                               this);
    explain->setWordWrap(true);
    explain->setStyleSheet(QStringLiteral("color:#8c9199;"));
    column->addWidget(explain);

    m_outside = new QWidget(this);
    auto *outside = new QVBoxLayout(m_outside);
    outside->setContentsMargins(0, 0, 0, 0);
    outside->addWidget(new QLabel(QStringLiteral("Your jams and invitations"), m_outside));
    m_jams = new QListWidget(m_outside);
    outside->addWidget(m_jams, 1);
    auto *outsideButtons = new QHBoxLayout;
    m_join = new QPushButton(QStringLiteral("Join or Open"), m_outside);
    m_start = new QPushButton(QStringLiteral("Start a Jam Session"), m_outside);
    auto *reload = new QPushButton(QStringLiteral("Refresh"), m_outside);
    outsideButtons->addWidget(m_join);
    outsideButtons->addWidget(reload);
    outsideButtons->addStretch();
    outsideButtons->addWidget(m_start);
    outside->addLayout(outsideButtons);
    column->addWidget(m_outside, 1);

    m_inside = new QWidget(this);
    auto *inside = new QVBoxLayout(m_inside);
    inside->setContentsMargins(0, 0, 0, 0);
    inside->addWidget(new QLabel(QStringLiteral("People"), m_inside));
    m_people = new QListWidget(m_inside);
    inside->addWidget(m_people, 1);
    auto *inviteRow = new QHBoxLayout;
    m_invite = new QLineEdit(m_inside);
    m_invite->setPlaceholderText(QStringLiteral("Invite by ForkMesh name, separated by commas"));
    auto *completer = new QCompleter(m_invite);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    completer->setFilterMode(Qt::MatchContains);
    completer->setModel(new QStringListModel(m_directory ? m_directory() : QStringList(), completer));
    m_invite->setCompleter(completer);
    m_inviteButton = new QPushButton(QStringLiteral("Invite"), m_inside);
    inviteRow->addWidget(m_invite, 1);
    inviteRow->addWidget(m_inviteButton);
    inside->addLayout(inviteRow);
    auto *insideButtons = new QHBoxLayout;
    m_remove = new QPushButton(QStringLiteral("Remove Person"), m_inside);
    m_leave = new QPushButton(QStringLiteral("Leave"), m_inside);
    insideButtons->addWidget(m_remove);
    insideButtons->addStretch();
    insideButtons->addWidget(m_leave);
    inside->addLayout(insideButtons);
    column->addWidget(m_inside, 1);

    m_problem = new QLabel(this);
    m_problem->setWordWrap(true);
    m_problem->setStyleSheet(QStringLiteral("color:#ff6b6b;"));
    column->addWidget(m_problem);
    auto *close = new QPushButton(QStringLiteral("Close"), this);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    column->addWidget(close, 0, Qt::AlignRight);

    connect(m_start, &QPushButton::clicked, this, [this]() { m_model->startJam(); });
    connect(reload, &QPushButton::clicked, this, [this]() { m_model->refreshJams(); });
    auto join = [this]() {
        QListWidgetItem *item = m_jams->currentItem();
        if (!item)
            return;
        const QString id = item->data(Qt::UserRole).toString();
        if (item->data(Qt::UserRole + 1).toBool())
            m_model->joinJam(id);
        else
            m_model->openJam(id);
    };
    connect(m_join, &QPushButton::clicked, this, join);
    connect(m_jams, &QListWidget::itemDoubleClicked, this, join);
    auto invite = [this]() {
        QStringList names;
        for (const QString &part : m_invite->text().split(QRegularExpression(QStringLiteral("[,\\s]+")), Qt::SkipEmptyParts))
            names << part;
        if (names.isEmpty())
            return;
        m_model->invite(names);
        m_invite->clear();
    };
    connect(m_inviteButton, &QPushButton::clicked, this, invite);
    connect(m_invite, &QLineEdit::returnPressed, this, invite);
    connect(m_remove, &QPushButton::clicked, this, [this]() {
        QListWidgetItem *item = m_people->currentItem();
        if (item)
            m_model->removeMember(item->data(Qt::UserRole).toString());
    });
    connect(m_leave, &QPushButton::clicked, this, [this]() { m_model->leaveJam(); });
    refresh();
}

void DjJamDialog::refresh()
{
    const auto &jam = m_model->jam();
    const bool joined = jam && jam->joined();
    m_outside->setVisible(!joined);
    m_inside->setVisible(joined);
    m_problem->setText(m_model->jamProblem());
    m_start->setEnabled(!m_model->jamBusy());
    if (!joined) {
        m_heading->setText(jam && !jam->joined() ? QStringLiteral("%1 invited you to %2").arg(jam->owner, jam->title)
                                                 : QStringLiteral("Jam with friends"));
        m_jams->clear();
        for (const DjJamSummary &summary : m_model->jams()) {
            auto *item = new QListWidgetItem(
                summary.invited() ? QStringLiteral("● %1 — invited by %2").arg(summary.title, summary.owner)
                                  : QStringLiteral("%1 — %2, %3 listening").arg(summary.title, summary.owner).arg(summary.members),
                m_jams);
            item->setData(Qt::UserRole, summary.id);
            item->setData(Qt::UserRole + 1, summary.invited());
            if (summary.invited())
                item->setForeground(QColor(0xff, 0xc4, 0x1c));
        }
        if (m_model->jams().isEmpty())
            new QListWidgetItem(QStringLiteral("No jams yet. Start one and invite people."), m_jams);
        return;
    }
    m_heading->setText(QStringLiteral("%1%2").arg(jam->title, jam->isOwner() ? QStringLiteral(" (you host)") : QString()));
    m_leave->setText(jam->isOwner() ? QStringLiteral("End the Jam for Everyone") : QStringLiteral("Leave the Jam"));
    m_remove->setVisible(jam->isOwner());
    const QString current = m_people->currentItem() ? m_people->currentItem()->data(Qt::UserRole).toString() : QString();
    m_people->clear();
    for (const DjJamMember &member : jam->members) {
        const DjPersonStatus status = m_model->status(member);
        auto *item = new QListWidgetItem(QStringLiteral("● %1%2 — %3")
                                             .arg(member.name, member.name == jam->owner ? QStringLiteral(" ♛") : QString(), statusLabel(status)),
                                         m_people);
        item->setForeground(statusColor(status));
        item->setData(Qt::UserRole, member.name);
        if (member.name == current)
            m_people->setCurrentItem(item);
    }
}

DjSongDialog::DjSongDialog(DjModel *model, const QString &id, QWidget *parent) : QDialog(parent), m_model(model), m_id(id)
{
    setWindowTitle(QStringLiteral("Tags and Playlists"));
    setStyleSheet(dialogStyle());
    resize(380, 480);
    auto *column = new QVBoxLayout(this);
    m_heading = new QLabel(this);
    m_heading->setWordWrap(true);
    m_heading->setStyleSheet(QStringLiteral("font-size:14px;font-weight:bold;"));
    column->addWidget(m_heading);
    column->addWidget(new QLabel(QStringLiteral("Tags"), this));
    m_tags = new QListWidget(this);
    column->addWidget(m_tags, 1);
    m_newTags = new QLineEdit(this);
    m_newTags->setPlaceholderText(QStringLiteral("Add tags, separated by commas (chill, warm up)"));
    column->addWidget(m_newTags);
    column->addWidget(new QLabel(QStringLiteral("Playlists"), this));
    m_playlists = new QListWidget(this);
    column->addWidget(m_playlists, 1);
    auto *shuffle = new QCheckBox(QStringLiteral("Leave out of shuffle"), this);
    shuffle->setStyleSheet(QStringLiteral("color:#e6e8eb;"));
    shuffle->setChecked(!m_model->crate().shuffles(id));
    column->addWidget(shuffle);
    auto *close = new QPushButton(QStringLiteral("Done"), this);
    column->addWidget(close, 0, Qt::AlignRight);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    connect(shuffle, &QCheckBox::toggled, this, [this](bool) { m_model->toggleShuffle(m_id); });
    connect(m_newTags, &QLineEdit::returnPressed, this, [this]() {
        m_model->addTags(m_newTags->text(), m_id);
        m_newTags->clear();
        refresh();
    });
    connect(m_tags, &QListWidget::itemChanged, this, [this](QListWidgetItem *item) {
        if (!m_building)
            m_model->toggleTag(item->data(Qt::UserRole).toString(), m_id);
    });
    connect(m_playlists, &QListWidget::itemChanged, this, [this](QListWidgetItem *item) {
        if (m_building)
            return;
        const DjTrack track = m_model->song(m_id);
        m_model->crate().keep(m_id, track.title, track.author);
        m_model->toggleInPlaylist(m_id, item->data(Qt::UserRole).toString());
    });
    refresh();
}

void DjSongDialog::refresh()
{
    m_building = true;
    const DjTrack track = m_model->song(m_id);
    m_heading->setText(track.shownTitle() + (track.author.isEmpty() ? QString() : QStringLiteral("\n") + track.author));
    m_tags->clear();
    QStringList names;
    for (const DjTagCount &tag : m_model->crate().tagCounts())
        names << tag.name;
    for (const QString &tag : track.tags) {
        if (!names.contains(tag))
            names << tag;
    }
    for (const QString &name : names) {
        auto *item = new QListWidgetItem(QStringLiteral("#") + name, m_tags);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(track.tags.contains(name) ? Qt::Checked : Qt::Unchecked);
        item->setData(Qt::UserRole, name);
    }
    m_playlists->clear();
    for (const DjPlaylist &playlist : m_model->crate().playlists) {
        auto *item = new QListWidgetItem(playlist.name, m_playlists);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(playlist.songs.contains(m_id) ? Qt::Checked : Qt::Unchecked);
        item->setData(Qt::UserRole, playlist.id);
    }
    m_building = false;
}

}
