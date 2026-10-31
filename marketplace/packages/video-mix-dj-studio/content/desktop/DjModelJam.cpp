#include "DjModel.h"

#include <QJsonArray>
#include <QPointer>
#include <QTimer>

#include <algorithm>
#include <cmath>

namespace forkmesh::djstudio {

namespace {

const QString apiPrefix = QStringLiteral("/api/video-mix-dj-studio");

}

bool DjModel::inJam() const { return m_jam && m_jam->joined(); }

int DjModel::pendingInvites() const
{
    return int(std::count_if(m_jams.begin(), m_jams.end(), [](const DjJamSummary &jam) { return jam.invited(); }));
}

void DjModel::jamRequest(const QByteArray &method, const QString &path, const QJsonObject &body,
                         std::function<void(std::optional<QJsonObject>)> done)
{
    if (!m_host.request) {
        m_jamProblem = QStringLiteral("Sign in to ForkMesh to jam with other people.");
        done(std::nullopt);
        return;
    }
    QPointer<DjModel> self(this);
    m_host.request(method, apiPrefix + path, body,
                   [self, path, done](bool ok, const QJsonObject &payload, const QString &error) {
                       if (!self)
                           return;
                       if (ok) {
                           self->m_jamProblem.clear();
                           done(payload);
                           return;
                       }
                       const QString code = payload.value(QStringLiteral("error")).toString();
                       if (code == QLatin1String("jam_not_found")) {
                           if (self->m_jam && path.startsWith(QStringLiteral("/jams/") + self->m_jam->id)) {
                               self->clearJam();
                               self->show(QStringLiteral("That jam has ended."));
                           }
                           self->m_jamProblem = QStringLiteral("That jam has ended or you were removed from it.");
                       } else if (code == QLatin1String("too_many_jams")) {
                           self->m_jamProblem = QStringLiteral("You already have 20 jams. End one first.");
                       } else if (code == QLatin1String("queue_full")) {
                           self->m_jamProblem = QStringLiteral("The jam queue is full.");
                       } else if (code == QLatin1String("too_many_reactions")) {
                           self->m_jamProblem = QStringLiteral("Slow down a little: reactions are limited to eight every 12 seconds.");
                       } else if (code == QLatin1String("jam_not_joined")) {
                           self->m_jamProblem = QStringLiteral("Join the jam first.");
                       } else if (code == QLatin1String("invalid_session")) {
                           self->m_jamProblem = QStringLiteral("Sign in to ForkMesh to jam with other people.");
                       } else {
                           self->m_jamProblem = error.isEmpty() ? QStringLiteral("ForkMesh could not be reached. Check your connection.") : error;
                       }
                       if (self->jamChanged)
                           self->jamChanged();
                       done(std::nullopt);
                   });
}

DjJamDeck DjModel::deckState(int index) const
{
    const DjDeck &deck = m_decks[index];
    DjJamDeck state;
    state.videoId = deck.videoId;
    state.title = deck.title;
    state.author = deck.author;
    state.playing = deck.playing;
    state.position = deck.position(now());
    state.rate = studio::clampRate(deck.rate);
    return state;
}

void DjModel::share(int index)
{
    if (index > 1 || m_applyingRemote || !inJam())
        return;
    QTimer *timer = m_shareTimers.value(index);
    if (!timer) {
        timer = new QTimer(this);
        timer->setSingleShot(true);
        timer->setInterval(250);
        connect(timer, &QTimer::timeout, this, [this, index]() {
            if (!inJam())
                return;
            const QString side = index == 0 ? QStringLiteral("a") : QStringLiteral("b");
            m_sharing.insert(index);
            jamRequest("PUT", QStringLiteral("/jams/%1/decks/%2").arg(m_jam->id, side), deckState(index).body(),
                       [this, index](std::optional<QJsonObject> reply) {
                           m_sharing.remove(index);
                           if (reply) {
                               if (const auto fresh = DjJam::parse(*reply)) {
                                   const DjSide side = index == 0 ? DjSide::A : DjSide::B;
                                   if (fresh->decks.contains(side))
                                       m_applied[index] = std::max(m_applied.value(index), fresh->decks.value(side).at);
                               }
                           }
                           receive(reply);
                       });
        });
        m_shareTimers.insert(index, timer);
    }
    timer->start();
}

void DjModel::refreshJams()
{
    jamRequest("GET", QStringLiteral("/jams"), {}, [this](std::optional<QJsonObject> reply) {
        if (!reply)
            return;
        m_jams = DjJamSummary::list(*reply);
        if (jamChanged)
            jamChanged();
    });
}

void DjModel::resumeJam()
{
    refreshJams();
    if (m_jam || m_resumeJam.isEmpty()) {
        if (inJam())
            poll();
        return;
    }
    openJam(m_resumeJam);
}

void DjModel::openJam(const QString &id)
{
    m_jamBusy = true;
    jamRequest("GET", QStringLiteral("/jams/%1").arg(id), {}, [this](std::optional<QJsonObject> reply) {
        m_jamBusy = false;
        receive(reply);
        if (jamChanged)
            jamChanged();
    });
}

void DjModel::startJam()
{
    m_jamBusy = true;
    QJsonObject decks;
    for (int i = 0; i < 2; ++i) {
        if (m_decks[i].loaded())
            decks.insert(i == 0 ? QStringLiteral("a") : QStringLiteral("b"), deckState(i).body());
    }
    if (jamChanged)
        jamChanged();
    jamRequest("POST", QStringLiteral("/jams"), {{QStringLiteral("decks"), decks}}, [this](std::optional<QJsonObject> reply) {
        m_jamBusy = false;
        receive(reply);
        if (inJam())
            show(QStringLiteral("Jam started. Invite people to share the decks and the queue."));
        if (jamChanged)
            jamChanged();
    });
}

void DjModel::joinJam(const QString &id)
{
    m_jamBusy = true;
    m_applied.clear();
    jamRequest("POST", QStringLiteral("/jams/%1/join").arg(id), {}, [this](std::optional<QJsonObject> reply) {
        m_jamBusy = false;
        receive(reply);
        if (inJam())
            show(QStringLiteral("You joined the jam. Decks 1 and 2 now follow the session."));
        refreshJams();
        if (jamChanged)
            jamChanged();
    });
}

void DjModel::leaveJam()
{
    if (!m_jam)
        return;
    const DjJam current = *m_jam;
    m_jamBusy = true;
    jamRequest(current.isOwner() ? QByteArray("DELETE") : QByteArray("POST"),
               current.isOwner() ? QStringLiteral("/jams/%1").arg(current.id) : QStringLiteral("/jams/%1/leave").arg(current.id), {},
               [this, current](std::optional<QJsonObject> reply) {
                   m_jamBusy = false;
                   if (!reply) {
                       if (jamChanged)
                           jamChanged();
                       return;
                   }
                   clearJam();
                   show(current.isOwner() ? QStringLiteral("Jam ended.") : QStringLiteral("You left the jam."));
                   refreshJams();
               });
}

void DjModel::invite(const QStringList &names)
{
    if (!m_jam || names.isEmpty())
        return;
    m_jamBusy = true;
    QJsonArray list;
    for (const QString &name : names)
        list.append(name.trimmed().toLower().remove(QLatin1Char('@')));
    jamRequest("POST", QStringLiteral("/jams/%1/members").arg(m_jam->id), {{QStringLiteral("names"), list}},
               [this](std::optional<QJsonObject> reply) {
                   m_jamBusy = false;
                   receive(reply);
                   if (reply) {
                       QStringList invited;
                       QStringList skipped;
                       for (const QJsonValue &value : reply->value(QStringLiteral("invited")).toArray())
                           invited.append(value.toString());
                       for (const QJsonValue &value : reply->value(QStringLiteral("skipped")).toArray())
                           skipped.append(value.toString());
                       if (!skipped.isEmpty())
                           m_jamProblem = QStringLiteral("No ForkMesh account for %1.").arg(skipped.join(QStringLiteral(", ")));
                       else if (!invited.isEmpty())
                           show(QStringLiteral("Invited %1.").arg(invited.join(QStringLiteral(", "))));
                   }
                   if (jamChanged)
                       jamChanged();
               });
}

void DjModel::removeMember(const QString &name)
{
    if (!m_jam)
        return;
    jamRequest("DELETE", QStringLiteral("/jams/%1/members/%2").arg(m_jam->id, name), {},
               [this](std::optional<QJsonObject> reply) { receive(reply); });
}

void DjModel::receive(const std::optional<QJsonObject> &reply, double latency)
{
    if (!reply)
        return;
    if (const auto fresh = DjJam::parse(*reply))
        adopt(*fresh, latency);
    else if (reply->contains(QStringLiteral("jam")) && reply->value(QStringLiteral("jam")).isNull())
        clearJam();
}

void DjModel::clearJam()
{
    for (QTimer *timer : std::as_const(m_shareTimers))
        timer->stop();
    m_sharing.clear();
    m_applied.clear();
    m_jam.reset();
    m_crowd.clear();
    m_seenReactions.clear();
    m_resumeJam.clear();
    m_polling = false;
    m_jamTimer->stop();
    for (int i = 0; i < 2; ++i)
        m_decks[i].pitchRange = std::max(m_decks[i].pitchRange, 0.04);
    save();
    if (jamChanged)
        jamChanged();
    emitLibrary();
}

void DjModel::adopt(const DjJam &fresh, double latency)
{
    const bool continuing = m_jam && m_jam->id == fresh.id && m_jam->joined();
    if (!continuing) {
        m_applied.clear();
        m_seenReactions.clear();
        for (const DjJamReaction &reaction : fresh.reactions)
            m_seenReactions.append(reaction.id);
    }
    const bool sharedChanged = !m_jam || m_jam->shared() != fresh.shared();
    const bool queueChanged = !m_jam || m_jam->queue != fresh.queue;
    if (sharedChanged)
        m_jam = fresh;
    else
        m_jam->now = fresh.now;
    const bool crowdChanged = m_crowd != fresh.presence;
    m_crowd = fresh.presence;
    if (m_resumeJam != fresh.id) {
        m_resumeJam = fresh.id;
        save();
    }
    if (sharedChanged || crowdChanged) {
        if (jamChanged)
            jamChanged();
    }
    if (queueChanged)
        emitLibrary();
    if (!fresh.joined())
        return;
    for (int i = 0; i < 2; ++i)
        m_decks[i].pitchRange = std::min(m_decks[i].pitchRange, studio::pitchRange);
    if (continuing)
        hear(fresh.reactions, fresh.me);
    DjJam timed = fresh;
    timed.now += qint64(std::llround(latency * 1000));
    const bool wasPlaying = m_decks[0].playing || m_decks[1].playing;
    follow(DjSide::A, timed);
    follow(DjSide::B, timed);
    if (wasPlaying)
        holdAutomix();
    if (!m_polling) {
        m_polling = true;
        m_jamTimer->start(m_onScreen ? 1500 : 2500);
    }
}

void DjModel::hear(const QList<DjJamReaction> &reactions, const QString &me)
{
    for (const DjJamReaction &reaction : reactions) {
        if (m_seenReactions.contains(reaction.id))
            continue;
        m_seenReactions.append(reaction.id);
        const DjReaction *kind = DjReaction::find(reaction.kind);
        if (reaction.name == me || reaction.ago >= 8000 || !m_onScreen || !kind)
            continue;
        spawn(*kind, reaction.name);
    }
    while (m_seenReactions.size() > 160)
        m_seenReactions.removeFirst();
}

void DjModel::react(const DjReaction &reaction)
{
    spawn(reaction, inJam() ? m_jam->me : QString());
    if (!inJam())
        return;
    const double at = now();
    m_sentReactions.erase(std::remove_if(m_sentReactions.begin(), m_sentReactions.end(), [at](double sent) { return at - sent > 12; }),
                          m_sentReactions.end());
    if (m_sentReactions.size() >= 8)
        return;
    m_sentReactions.append(at);
    jamRequest("POST", QStringLiteral("/jams/%1/reactions").arg(m_jam->id), {{QStringLiteral("kind"), reaction.id}},
               [this](std::optional<QJsonObject> reply) { receive(reply); });
}

void DjModel::spawn(const DjReaction &reaction, const QString &from)
{
    while (m_effects.size() >= 10)
        m_effects.removeFirst();
    DjEffect effect;
    effect.id = ++m_effectId;
    effect.reaction = reaction;
    effect.sender = from;
    effect.born = now();
    m_effects.append(effect);
    const int id = effect.id;
    QTimer::singleShot(int(reaction.seconds() * 1000) + 50, this, [this, id]() {
        m_effects.erase(std::remove_if(m_effects.begin(), m_effects.end(), [id](const DjEffect &e) { return e.id == id; }), m_effects.end());
        if (effectsChanged)
            effectsChanged();
    });
    if (effectsChanged)
        effectsChanged();
}

DjPersonStatus DjModel::status(const DjJamMember &member) const
{
    if (!m_jam || member.name != m_jam->me) {
        std::optional<DjJamPresence> seen;
        for (const DjJamPresence &presence : m_crowd) {
            if (presence.name == member.name)
                seen = presence;
        }
        return DjJam::status(member, seen);
    }
    if (m_onScreen)
        return DjPersonStatus::Studio;
    return m_decks[0].playing || m_decks[1].playing ? DjPersonStatus::Listening : DjPersonStatus::Idle;
}

void DjModel::follow(DjSide side, const DjJam &jam)
{
    const int index = side == DjSide::A ? 0 : 1;
    if (!jam.decks.contains(side))
        return;
    const DjJamDeck remote = jam.decks.value(side);
    if (remote.at <= m_applied.value(index))
        return;
    m_applied[index] = remote.at;
    if (remote.by == jam.me)
        return;
    DjDeck &deck = m_decks[index];
    if (deck.scratching || m_sharing.contains(index) || (m_shareTimers.value(index) && m_shareTimers.value(index)->isActive()))
        return;
    m_applyingRemote = true;
    if (remote.videoId.isEmpty()) {
        if (deck.loaded())
            eject(index);
        m_applyingRemote = false;
        return;
    }
    const double target = remote.expectedPosition(jam.now);
    if (deck.videoId != remote.videoId)
        prepare(index, remote.videoId, remote.title, remote.author, target, false);
    if (std::abs(deck.rate - remote.rate) > 0.0005)
        setRate(index, remote.rate);
    if (DjJam::needsSeek(deck.position(now()), target, remote.playing)) {
        m_player->seek(index, target);
        deck.time = target;
        deck.tickAt = now();
        settle(index, deck.tickAt);
    }
    if (remote.playing && !deck.playing)
        play(index);
    else if (!remote.playing && deck.playing)
        pause(index);
    m_applyingRemote = false;
}

void DjModel::poll()
{
    if (!inJam()) {
        m_polling = false;
        return;
    }
    m_polling = true;
    const bool playing = m_decks[0].playing || m_decks[1].playing;
    if (!m_onScreen && !playing) {
        m_jamTimer->start(2500);
        return;
    }
    const DjPresenceState state = m_onScreen ? DjPresenceState::Studio : playing ? DjPresenceState::Listening : DjPresenceState::Idle;
    const double began = now();
    const QString id = m_jam->id;
    jamRequest("POST", QStringLiteral("/jams/%1/presence").arg(id), DjJamPresence::body(state, playing),
               [this, began, id](std::optional<QJsonObject> reply) {
                   if (!m_jam || m_jam->id != id)
                       return;
                   receive(reply, (now() - began) / 2);
                   if (inJam())
                       m_jamTimer->start(m_onScreen ? 1500 : 2500);
                   else
                       m_polling = false;
               });
}

}
