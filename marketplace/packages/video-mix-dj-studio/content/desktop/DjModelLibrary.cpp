#include "DjModel.h"

#include <QDateTime>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRandomGenerator>
#include <QTimer>
#include <QUuid>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <memory>

namespace forkmesh::djstudio {

void DjModel::fetchUrl(const QUrl &url, int timeout, std::function<void(bool, const QByteArray &)> done)
{
    if (!m_host.network || !url.isValid()) {
        done(false, {});
        return;
    }
    QNetworkRequest request(url);
    request.setTransferTimeout(timeout);
    request.setRawHeader(QByteArrayLiteral("Accept"), QByteArrayLiteral("application/json, image/*"));
    QNetworkReply *reply = m_host.network->get(request);
    QPointer<DjModel> self(this);
    connect(reply, &QNetworkReply::finished, this, [reply, done, self]() {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool ok = reply->error() == QNetworkReply::NoError && status == 200;
        const QByteArray data = ok ? reply->readAll() : QByteArray();
        reply->deleteLater();
        if (self)
            done(ok, data);
    });
}

void DjModel::fetchInstances(std::function<void()> done)
{
    if (!m_instances.isEmpty()) {
        done();
        return;
    }
    fetchUrl(QUrl(QStringLiteral("https://api.invidious.io/instances.json?sort_by=health")), 8000,
             [this, done](bool ok, const QByteArray &data) {
                 if (ok)
                     m_instances = studio::searchInstances(data);
                 done();
             });
}

void DjModel::fetchInvidious(std::function<QUrl(const QUrl &)> address,
                             std::function<void(std::optional<QPair<QByteArray, QUrl>>)> done, int attempt)
{
    if (attempt == 0) {
        fetchInstances([this, address, done]() {
            if (m_instances.isEmpty()) {
                done(std::nullopt);
                return;
            }
            fetchInvidious(address, done, 1);
        });
        return;
    }
    if (attempt > m_instances.size()) {
        m_instances.clear();
        done(std::nullopt);
        return;
    }
    const QUrl instance = m_instances.at(attempt - 1);
    const QUrl url = address(instance);
    if (!url.isValid()) {
        fetchInvidious(address, done, attempt + 1);
        return;
    }
    fetchUrl(url, 8000, [this, address, done, attempt, instance](bool ok, const QByteArray &data) {
        if (ok) {
            m_instances.removeAll(instance);
            m_instances.prepend(instance);
            done(QPair<QByteArray, QUrl>{data, instance});
            return;
        }
        fetchInvidious(address, done, attempt + 1);
    });
}

void DjModel::search(const QString &raw)
{
    const QString query = raw.trimmed();
    if (query.size() < 2) {
        m_searchProblem = QStringLiteral("Enter at least two characters, or paste a YouTube link.");
        if (searchChanged)
            searchChanged();
        return;
    }
    const int token = ++m_searchGeneration;
    m_searching = true;
    m_searchProblem.clear();
    if (searchChanged)
        searchChanged();
    fetchInvidious([query](const QUrl &instance) { return studio::searchUrl(instance, query); },
                   [this, token](std::optional<QPair<QByteArray, QUrl>> found) {
                       if (token != m_searchGeneration)
                           return;
                       m_searching = false;
                       if (!found) {
                           m_searchProblem = QStringLiteral("YouTube search is unavailable right now. Paste a YouTube link to load it directly.");
                       } else {
                           m_searchResults = studio::searchResults(found->first);
                           if (m_searchResults.isEmpty())
                               m_searchProblem = QStringLiteral("No YouTube videos matched that search.");
                       }
                       if (searchChanged)
                           searchChanged();
                   });
}

void DjModel::findSimilar(const QString &id)
{
    if (!studio::isVideoId(id))
        return;
    if (m_similarSeed == id && (!m_similarResults.isEmpty() || m_similarLoading))
        return;
    const DjTrack seed = song(id);
    const int token = ++m_similarGeneration;
    m_similarSeed = id;
    m_similarResults.clear();
    m_similarProblem.clear();
    m_similarLoading = true;
    if (similarChanged)
        similarChanged();
    auto finish = [this, token, id](std::optional<QList<DjSearchResult>> found) {
        if (token != m_similarGeneration)
            return;
        m_similarLoading = false;
        QList<DjSearchResult> results;
        QSet<QString> loaded;
        for (const DjDeck &deck : m_decks)
            loaded.insert(deck.videoId);
        for (const DjSearchResult &result : found.value_or(QList<DjSearchResult>())) {
            if (result.id != id && !loaded.contains(result.id))
                results.append(result);
        }
        m_similarResults = results;
        m_similarProblem = !found ? QStringLiteral("YouTube suggestions are unavailable right now.")
                           : results.isEmpty() ? QStringLiteral("YouTube has no suggestions for this song.")
                                               : QString();
        if (similarChanged)
            similarChanged();
    };
    fetchInvidious([id](const QUrl &instance) { return studio::similarUrl(instance, id); },
                   [this, seed, finish](std::optional<QPair<QByteArray, QUrl>> found) {
                       QList<DjSearchResult> results = found ? studio::similarResults(found->first) : QList<DjSearchResult>();
                       const QString query = smart::searchQuery(seed);
                       if (results.isEmpty() && query.size() >= 2) {
                           fetchInvidious([query](const QUrl &instance) { return studio::searchUrl(instance, query); },
                                          [finish, found](std::optional<QPair<QByteArray, QUrl>> more) {
                                              if (more)
                                                  finish(studio::searchResults(more->first));
                                              else if (found)
                                                  finish(QList<DjSearchResult>());
                                              else
                                                  finish(std::nullopt);
                                          });
                           return;
                       }
                       if (found)
                           finish(results);
                       else
                           finish(std::nullopt);
                   });
}

QList<DjSimilarSong> DjModel::similarSongs(const QString &id) const
{
    if (!studio::isVideoId(id))
        return {};
    QSet<QString> loaded;
    for (const DjDeck &deck : m_decks)
        loaded.insert(deck.videoId);
    QList<DjTrack> among;
    for (const DjTrack &track : librarySongs()) {
        if (!loaded.contains(track.id) && track.id != id)
            among.append(track);
    }
    return smart::similar(song(id), among, m_crate.playlists);
}

QImage DjModel::artwork(const QString &id)
{
    if (!studio::isVideoId(id))
        return {};
    const auto found = m_art.constFind(id);
    if (found != m_art.constEnd())
        return *found;
    if (m_artLoading.contains(id) || m_artLoading.size() > 12)
        return {};
    m_artLoading.insert(id);
    fetchUrl(studio::thumbnailUrl(id, QStringLiteral("mqdefault")), 10000, [this, id](bool ok, const QByteArray &data) {
        m_artLoading.remove(id);
        QImage image;
        if (ok)
            image.loadFromData(data);
        if (image.isNull())
            image = QImage(1, 1, QImage::Format_ARGB32_Premultiplied);
        if (image.width() > 1) {
            const int side = std::min(image.width(), image.height());
            image = image.copy((image.width() - side) / 2, (image.height() - side) / 2, side, side)
                        .scaled(256, 256, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                        .convertToFormat(QImage::Format_ARGB32_Premultiplied);
        }
        if (m_art.size() > 600)
            m_art.clear();
        m_art.insert(id, image);
        for (int i = 0; i < 4; ++i) {
            if (m_decks[i].videoId == id)
                emitDeck(i);
        }
        emitChanged();
    });
    return {};
}

DjTrack DjModel::song(const QString &id) const
{
    if (const auto track = m_crate.track(id))
        return *track;
    for (const DjPlayedSong &played : m_library) {
        if (played.id == id)
            return crate::makeTrack(id, played.title, played.author);
    }
    for (const DjHistoryItem &item : m_history) {
        if (item.id == id)
            return crate::makeTrack(id, item.title, item.author);
    }
    for (const DjDeck &deck : m_decks) {
        if (deck.videoId == id)
            return crate::makeTrack(id, deck.title, deck.author);
    }
    for (const DjQueueItem &item : shownQueue()) {
        if (item.videoId == id)
            return crate::makeTrack(id, item.title, item.author);
    }
    for (const DjSearchResult &result : m_searchResults) {
        if (result.id == id)
            return crate::makeTrack(id, result.title, result.author);
    }
    for (const DjSearchResult &result : m_similarResults) {
        if (result.id == id)
            return crate::makeTrack(id, result.title, result.author);
    }
    return crate::makeTrack(id);
}

QList<DjTrack> DjModel::librarySongs() const
{
    QHash<QString, DjTrack> kept;
    for (const DjTrack &track : m_crate.tracks)
        kept.insert(track.id, track);
    QSet<QString> played;
    QList<DjTrack> out;
    for (const DjPlayedSong &song : m_library) {
        played.insert(song.id);
        out.append(kept.contains(song.id) ? kept.value(song.id) : crate::makeTrack(song.id, song.title, song.author));
    }
    for (auto it = m_crate.tracks.crbegin(); it != m_crate.tracks.crend(); ++it) {
        if (!played.contains(it->id))
            out.append(*it);
    }
    return out;
}

int DjModel::plays(const QString &id) const
{
    for (const DjPlayedSong &song : m_library) {
        if (song.id == id)
            return song.plays;
    }
    return 0;
}

double DjModel::lastPlayed(const QString &id) const
{
    for (const DjPlayedSong &song : m_library) {
        if (song.id == id)
            return song.last;
    }
    return 0;
}

void DjModel::forgetPlayed(const QString &id)
{
    m_history.erase(std::remove_if(m_history.begin(), m_history.end(), [&](const DjHistoryItem &item) { return item.id == id; }), m_history.end());
    m_library.erase(std::remove_if(m_library.begin(), m_library.end(), [&](const DjPlayedSong &song) { return song.id == id; }), m_library.end());
    refreshSmart();
    save();
    emitLibrary();
}

void DjModel::clearHistory()
{
    m_history.clear();
    m_library.clear();
    refreshSmart();
    save();
    emitLibrary();
}

void DjModel::refreshSmartNow()
{
    m_crate.categorize(librarySongs(), m_library, QDateTime::currentMSecsSinceEpoch() / 1000.0);
}

void DjModel::refreshSmart()
{
    static int generation = 0;
    const int token = ++generation;
    const QList<DjTrack> songs = librarySongs();
    const QList<DjPlayedSong> library = m_library;
    const double time = QDateTime::currentMSecsSinceEpoch() / 1000.0;
    QtConcurrent::run([songs, library, time]() { return smart::lists(songs, library, time); })
        .then(this, [this, token](const QList<DjSmartList> &lists) {
            if (token != generation || m_crate.smart == lists)
                return;
            m_crate.setSmart(lists);
            emitLibrary();
        });
}

void DjModel::toggleShuffle(const QString &id)
{
    const DjTrack track = song(id);
    const bool on = !m_crate.shuffles(id);
    if (!m_crate.setShuffles(id, on, track.title, track.author)) {
        show(QStringLiteral("The library is full: remove a song first."));
        return;
    }
    saveCrate();
    const QString name = track.title.isEmpty() ? QStringLiteral("This song") : track.title;
    show(on ? QStringLiteral("%1 is back in the shuffle.").arg(name) : QStringLiteral("%1 will not come up in shuffle.").arg(name));
}

void DjModel::addTags(const QString &raw, const QString &id)
{
    const DjTrack known = song(id);
    if (crate::tagNames(raw).isEmpty())
        return;
    if (!m_crate.tag(id, raw, known.title, known.author)) {
        show(QStringLiteral("The library is full: remove a song to tag another."));
        return;
    }
    saveCrate();
}

void DjModel::toggleTag(const QString &name, const QString &id)
{
    const auto track = m_crate.track(id);
    if (track && track->tags.contains(name)) {
        m_crate.untag(id, name);
        saveCrate();
    } else {
        addTags(name, id);
    }
}

void DjModel::renameTag(const QString &old, const QString &raw)
{
    if (m_crate.renameTag(old, raw))
        saveCrate();
}

void DjModel::deleteTag(const QString &name)
{
    m_crate.deleteTag(name);
    saveCrate();
    show(QStringLiteral("Removed the #%1 tag.").arg(name));
}

bool DjModel::inPlaylist(const QString &id, const QString &playlist) const
{
    const auto list = m_crate.playlist(playlist);
    return list && list->songs.contains(id);
}

void DjModel::toggleInPlaylist(const QString &id, const QString &playlist)
{
    const auto list = m_crate.playlist(playlist);
    if (!list)
        return;
    if (list->songs.contains(id)) {
        m_crate.remove(id, playlist);
        show(QStringLiteral("Removed from %1.").arg(list->name));
    } else if (m_crate.add({song(id)}, playlist) > 0) {
        show(QStringLiteral("Added to %1.").arg(list->name));
    } else {
        show(list->songs.size() >= crate::playlistSongLimit ? QStringLiteral("%1 is full.").arg(list->name)
                                                           : QStringLiteral("The library is full: remove a song first."));
    }
    saveCrate();
}

std::optional<QString> DjModel::newPlaylist(const QString &name, const QStringList &ids)
{
    const auto id = m_crate.createPlaylist(name);
    if (!id) {
        show(QStringLiteral("You already have %1 playlists.").arg(crate::playlistLimit));
        return std::nullopt;
    }
    QList<DjTrack> songs;
    for (const QString &song : ids)
        songs.append(this->song(song));
    m_crate.add(songs, *id);
    saveCrate();
    show(QStringLiteral("Created %1.").arg(m_crate.label(DjFocus::playlist(*id))));
    return id;
}

void DjModel::renamePlaylist(const QString &id, const QString &name)
{
    m_crate.renamePlaylist(id, name);
    saveCrate();
}

void DjModel::deletePlaylist(const QString &id)
{
    const QString name = m_crate.label(DjFocus::playlist(id));
    m_crate.deletePlaylist(id);
    saveCrate();
    show(QStringLiteral("Deleted %1.").arg(name));
}

void DjModel::removeFromPlaylist(const QString &songId, const QString &playlist)
{
    m_crate.remove(songId, playlist);
    saveCrate();
}

void DjModel::movePlaylistSong(const QString &playlist, int from, int to)
{
    m_crate.moveSongs(playlist, {from}, to);
    saveCrate();
}

void DjModel::removeSong(const QString &id)
{
    m_crate.removeTrack(id);
    saveCrate();
    forgetPlayed(id);
}

void DjModel::fileDeck(int index, const DjFocus &target)
{
    const DjDeck &deck = m_decks[index];
    if (!deck.loaded())
        return;
    const QString id = deck.videoId;
    const DjTrack track = song(id);
    const DjFocus from = m_crate.focus;
    const QString fromLabel = m_crate.label(from);
    const DjMove result = m_crate.move(id, target, track.title, track.author);
    if (result == DjMove::Full) {
        show(QStringLiteral("%1 or the library is full.").arg(m_crate.label(target)));
        return;
    }
    saveCrate();
    QString done;
    switch (result) {
    case DjMove::Removed:
        done = QStringLiteral("Took it out of %1").arg(fromLabel);
        break;
    case DjMove::Moved:
        done = QStringLiteral("Moved from %1 to %2").arg(fromLabel, m_crate.label(target));
        break;
    default:
        done = QStringLiteral("Added to %1").arg(m_crate.label(target));
    }
    bool stillIn = false;
    for (const DjTrack &t : m_crate.songs(from))
        stillIn = stillIn || t.id == id;
    if (!m_automixOn || !(result == DjMove::Moved || result == DjMove::Removed) || stillIn) {
        show(done + QLatin1Char('.'));
        return;
    }
    if (skipOut(index))
        show(done + QStringLiteral(". Skipping to the next song."));
    else
        show(done + QStringLiteral(". Nothing else in %1 to play.").arg(fromLabel));
}

void DjModel::setShuffle(bool on)
{
    m_crate.shuffle = on;
    saveCrate();
}

void DjModel::focusAutomix(const DjFocus &focus, bool startMix)
{
    m_crate.setFocus(focus);
    saveCrate();
    if (startMix && !m_automixOn)
        setAutomix(true);
    refocusAutomix();
    const QString label = m_crate.focus.isEverything()
                              ? QStringLiteral("everything you have played and saved")
                              : m_crate.label(m_crate.focus) + (m_crate.shuffle ? QStringLiteral(", shuffled") : QStringLiteral(" in order"));
    show(m_automixOn ? QStringLiteral("AUTOMIX plays %1.").arg(label) : QStringLiteral("AUTOMIX will play %1.").arg(label));
    emitChanged();
}

void DjModel::queueAll(const DjFocus &focus, bool shuffled)
{
    QList<DjTrack> songs = m_crate.songs(focus);
    if (shuffled)
        std::shuffle(songs.begin(), songs.end(), *QRandomGenerator::global());
    enqueueAll(songs);
}

void DjModel::saveSmart(const QString &id)
{
    const auto list = m_crate.smartList(id);
    if (!list)
        return;
    QStringList ids;
    for (const DjTrack &track : list->songs)
        ids.append(track.id);
    newPlaylist(list->name, ids);
}

QString DjModel::importData(const QByteArray &data, const QString &name)
{
    const auto found = DjCrate::parse(data);
    if (!found) {
        show(QStringLiteral("No YouTube songs were found to import."));
        return QStringLiteral("No YouTube songs were found to import.");
    }
    const DjMergeReport report = m_crate.merge(*found, name.isEmpty() ? QStringLiteral("Imported") : name);
    saveCrate();
    show(report.summary());
    return report.summary();
}

QList<DjQueueItem> DjModel::shownQueue() const
{
    if (m_jam && m_jam->joined())
        return m_jam->queue;
    return m_queue;
}

bool DjModel::playNow(const QString &id, const QString &title, const QString &author)
{
    if (m_automixing || m_decks[0].scratching || m_decks[1].scratching || m_decks[0].keepsPlaying || m_decks[1].keepsPlaying)
        return false;
    for (int i = 2; i < deckCount(); ++i) {
        if (m_decks[i].keepsPlaying)
            return false;
    }
    bool playing = false;
    const DjSide side = automix::playNow(snapshot(0), snapshot(1), m_crossfader, &playing);
    if (playing)
        return false;
    const int index = side == DjSide::A ? 0 : 1;
    load(index, id, title, author);
    play(index);
    return true;
}

void DjModel::enqueue(const QString &id, const QString &title, const QString &author)
{
    if (!studio::isVideoId(id))
        return;
    if (playNow(id, title, author))
        return;
    if (m_jam && m_jam->joined()) {
        show(title.isEmpty() ? QStringLiteral("Added to the jam queue.") : QStringLiteral("Queued for the jam: %1").arg(title));
        jamRequest("POST", QStringLiteral("/jams/%1/queue").arg(m_jam->id),
                   {{QStringLiteral("videoId"), id}, {QStringLiteral("title"), title}, {QStringLiteral("author"), author}},
                   [this](std::optional<QJsonObject> reply) { receive(reply); });
        return;
    }
    if (m_queue.size() >= studio::queueLimit) {
        show(QStringLiteral("The queue is full. Play or remove a video first."));
        return;
    }
    m_queue.append({QUuid::createUuid().toString(QUuid::WithoutBraces), id, title.left(300), author.left(200), QString(),
                    QDateTime::currentMSecsSinceEpoch()});
    show(title.isEmpty() ? QStringLiteral("Added to the queue.") : QStringLiteral("Queued: %1").arg(title));
    save();
    emitLibrary();
}

void DjModel::enqueueAll(const QList<DjTrack> &songs)
{
    if (songs.isEmpty()) {
        show(QStringLiteral("There are no songs to queue."));
        return;
    }
    const DjTrack first = songs.first();
    if (playNow(first.id, first.title, first.author)) {
        if (songs.size() > 1)
            enqueueAll(songs.mid(1));
        return;
    }
    if (m_jam && m_jam->joined()) {
        const int room = std::max(0, int(studio::queueLimit - m_jam->queue.size()));
        const QList<DjTrack> picked = songs.mid(0, room);
        show(picked.isEmpty() ? QStringLiteral("The jam queue is full.") : QStringLiteral("Queueing %1 songs for the jam.").arg(picked.size()));
        auto rest = std::make_shared<QList<DjTrack>>(picked);
        auto step = std::make_shared<std::function<void()>>();
        const QString jamId = m_jam->id;
        *step = [this, rest, step, jamId]() {
            if (rest->isEmpty())
                return;
            const DjTrack song = rest->takeFirst();
            jamRequest("POST", QStringLiteral("/jams/%1/queue").arg(jamId),
                       {{QStringLiteral("videoId"), song.id}, {QStringLiteral("title"), song.title}, {QStringLiteral("author"), song.author}},
                       [this, step](std::optional<QJsonObject> reply) {
                           receive(reply);
                           if (reply)
                               (*step)();
                       });
        };
        (*step)();
        return;
    }
    const int room = std::max(0, int(studio::queueLimit - m_queue.size()));
    const qint64 at = QDateTime::currentMSecsSinceEpoch();
    int added = 0;
    for (const DjTrack &song : songs.mid(0, room)) {
        m_queue.append({QUuid::createUuid().toString(QUuid::WithoutBraces), song.id, song.title, song.author, QString(), at});
        ++added;
    }
    show(added == 0 ? QStringLiteral("The queue is full. Play or remove a video first.")
         : added < songs.size() ? QStringLiteral("Queued %1 songs: the queue is full.").arg(added)
         : added == 1 ? QStringLiteral("Queued 1 song.") : QStringLiteral("Queued %1 songs.").arg(added));
    save();
    emitLibrary();
}

void DjModel::playFromQueue(const DjQueueItem &item, int index)
{
    load(index, item.videoId, item.title, item.author);
    removeFromQueue(item);
}

void DjModel::removeFromQueue(const DjQueueItem &item)
{
    if (m_jam && m_jam->joined()) {
        m_jam->queue.erase(std::remove_if(m_jam->queue.begin(), m_jam->queue.end(), [&](const DjQueueItem &q) { return q.id == item.id; }), m_jam->queue.end());
        jamRequest("DELETE", QStringLiteral("/jams/%1/queue/%2").arg(m_jam->id, item.id), {},
                   [this](std::optional<QJsonObject> reply) { receive(reply); });
        emitLibrary();
        return;
    }
    m_queue.erase(std::remove_if(m_queue.begin(), m_queue.end(), [&](const DjQueueItem &q) { return q.id == item.id; }), m_queue.end());
    save();
    emitLibrary();
}

void DjModel::moveQueueItem(int from, int to)
{
    if (inJam() || from < 0 || from >= m_queue.size())
        return;
    to = std::clamp(to, 0, int(m_queue.size()) - 1);
    m_queue.move(from, to);
    save();
    emitLibrary();
}

void DjModel::clearQueue()
{
    if (inJam()) {
        for (const DjQueueItem &item : shownQueue())
            removeFromQueue(item);
        return;
    }
    m_queue.clear();
    save();
    emitLibrary();
}

void DjModel::toggleAutoDj()
{
    const bool wasMixing = m_automixOn;
    setAutoDj(!m_autoDjOn);
    if (!m_autoDjOn)
        show(m_automixOn ? QStringLiteral("AUTO DJ off. AUTOMIX keeps playing your queue and library.") : QStringLiteral("AUTO DJ off."));
    else if (m_decks[0].playing || m_decks[1].playing)
        show(QStringLiteral("AUTO DJ on: when this song ends, a related song people liked plays on the next deck."));
    else if (wasMixing)
        show(QStringLiteral("AUTO DJ on: related songs people liked follow each song."));
    else
        show(QStringLiteral("AUTO DJ on: start a song and related songs people liked follow it."));
}

void DjModel::setAutoDj(bool on, bool refresh)
{
    if (on == m_autoDjOn)
        return;
    if (!on) {
        const bool owned = m_autoDjOwnsAutomix;
        stopAutoDj();
        if (owned && m_automixOn)
            setAutomix(false);
        save();
        emitChanged();
        return;
    }
    m_autoDjOn = true;
    m_autoDjRefresh = refresh;
    if (!m_automixOn) {
        setAutomix(true);
        m_autoDjOwnsAutomix = true;
    }
    primeAutoDj();
    save();
    emitChanged();
}

void DjModel::stopAutoDj()
{
    ++m_autoDjGeneration;
    m_autoDjOn = false;
    m_autoDjFinding = false;
    m_autoDjSeed.clear();
    m_autoDjChoices.clear();
    m_autoDjOwnsAutomix = false;
    m_autoDjRefresh = false;
}

std::optional<DjTrack> DjModel::autoDjSeedTrack() const
{
    QList<int> playing;
    for (int i = 0; i < 2; ++i) {
        if (m_decks[i].playing && m_decks[i].loaded())
            playing.append(i);
    }
    if (!playing.isEmpty()) {
        const int live = playing.size() > 1 ? (m_crossfader <= 0 ? 0 : 1) : playing.first();
        return song(m_decks[live].videoId);
    }
    if (!m_history.isEmpty())
        return song(m_history.first().id);
    return std::nullopt;
}

void DjModel::primeAutoDj()
{
    if (!m_autoDjOn)
        return;
    const auto seed = autoDjSeedTrack();
    if (!seed || seed->id == m_autoDjSeed)
        return;
    findAutoDj(*seed);
}

std::optional<DjAutoDjCandidate> DjModel::autoDjNext() const
{
    QSet<QString> loaded;
    for (const DjDeck &deck : m_decks) {
        if (deck.loaded())
            loaded.insert(deck.videoId);
    }
    return autodj::next(m_autoDjChoices, loaded, m_unplayable, m_autoDjPlayed);
}

std::optional<DjAutoDjCandidate> DjModel::takeAutoDj()
{
    const auto pick = autoDjNext();
    if (!pick)
        return std::nullopt;
    m_autoDjPlayed = autodj::remember(pick->id, m_autoDjPlayed);
    return pick;
}

void DjModel::findAutoDj(const DjTrack &seed)
{
    const int token = ++m_autoDjGeneration;
    m_autoDjSeed = seed.id;
    m_autoDjChoices.clear();
    m_autoDjFinding = true;
    QSet<QString> avoid(m_unplayable);
    QStringList heard;
    for (const DjHistoryItem &item : m_history) {
        avoid.insert(item.id);
        heard.append(item.title);
    }
    for (const DjDeck &deck : m_decks) {
        if (deck.loaded()) {
            avoid.insert(deck.videoId);
            heard.append(deck.title);
        }
    }
    for (const QString &id : m_autoDjPlayed)
        avoid.insert(id);
    emitChanged();
    auto rank = [this, token](QList<DjAutoDjCandidate> fresh, std::optional<QUrl> instance) {
        auto done = [this, token, instance](QList<DjAutoDjCandidate> choices) {
            if (token != m_autoDjGeneration || !m_autoDjOn)
                return;
            m_autoDjChoices = choices;
            m_autoDjFinding = false;
            if (choices.isEmpty())
                show(!instance ? QStringLiteral("AUTO DJ can't reach YouTube suggestions right now, so AUTOMIX plays your library.")
                               : QStringLiteral("AUTO DJ found nothing new like this song, so AUTOMIX plays your library."));
            refreshAutoDj();
            emitChanged();
        };
        if (!instance || fresh.isEmpty()) {
            done(fresh);
            return;
        }
        auto state = std::make_shared<QList<DjAutoDjCandidate>>(fresh);
        auto left = std::make_shared<int>(std::min<int>(autodj::checked, fresh.size()));
        for (int i = 0; i < *left; ++i) {
            const QString id = fresh.at(i).id;
            fetchUrl(autodj::likesUrl(*instance, id), 6000, [state, left, id, done](bool ok, const QByteArray &data) {
                if (ok) {
                    if (const auto counts = autodj::likes(data)) {
                        for (DjAutoDjCandidate &candidate : *state) {
                            if (candidate.id != id)
                                continue;
                            candidate.likes = counts->likes;
                            if (counts->views > 0)
                                candidate.views = counts->views;
                        }
                    }
                }
                if (--*left > 0)
                    return;
                const QList<DjAutoDjCandidate> all = *state;
                done(autodj::ranked(all.mid(0, autodj::checked)) + autodj::ranked(all.mid(autodj::checked)));
            });
        }
    };
    fetchInvidious([id = seed.id](const QUrl &instance) { return studio::similarUrl(instance, id); },
                   [this, token, seed, avoid, heard, rank](std::optional<QPair<QByteArray, QUrl>> found) {
                       if (token != m_autoDjGeneration)
                           return;
                       QList<DjAutoDjCandidate> fresh;
                       std::optional<QUrl> instance;
                       if (found) {
                           instance = found->second;
                           fresh = autodj::eligible(autodj::candidates(found->first), seed, avoid, heard);
                       }
                       const QString query = smart::searchQuery(seed);
                       if (fresh.isEmpty() && query.size() >= 2) {
                           fetchInvidious([query](const QUrl &url) { return studio::searchUrl(url, query); },
                                          [this, token, seed, avoid, heard, rank, instance](std::optional<QPair<QByteArray, QUrl>> more) {
                                              if (token != m_autoDjGeneration)
                                                  return;
                                              if (!more) {
                                                  rank({}, instance);
                                                  return;
                                              }
                                              rank(autodj::eligible(autodj::candidates(more->first), seed, avoid, heard), more->second);
                                          });
                           return;
                       }
                       rank(fresh, instance);
                   });
}

void DjModel::refreshAutoDj()
{
    if (!m_autoDjRefresh)
        return;
    m_autoDjRefresh = false;
    if (!m_automixOn || m_automixing || !shownQueue().isEmpty() || !autoDjNext())
        return;
    QList<int> playing;
    for (int i = 0; i < 2; ++i) {
        if (m_decks[i].playing)
            playing.append(i);
    }
    if (playing.size() != 1)
        return;
    const int idle = 1 - playing.first();
    if (m_decks[idle].scratching || m_decks[idle].keepsPlaying)
        return;
    preload(idle);
}

}
