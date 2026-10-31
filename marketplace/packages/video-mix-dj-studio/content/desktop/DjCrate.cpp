#include "DjCrate.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QMap>
#include <QStringDecoder>
#include <QTextBoundaryFinder>
#include <QUuid>

#include <algorithm>
#include <initializer_list>

namespace forkmesh::djstudio {

namespace {

bool isNewline(QChar c)
{
    const ushort u = c.unicode();
    return (u >= 0x0A && u <= 0x0D) || u == 0x85 || u == 0x2028 || u == 0x2029;
}

bool isWhitespace(QChar c) { return c.isSpace() || isNewline(c); }

bool isTagSeparator(QChar c) { return isWhitespace(c) || c == QLatin1Char(',') || c == QLatin1Char('#'); }

QStringList splitWhere(const QString &text, bool (*separator)(QChar))
{
    QStringList parts;
    QString part;
    for (const QChar c : text) {
        if (separator(c)) {
            if (!part.isEmpty()) {
                parts.append(part);
                part.clear();
            }
        } else {
            part.append(c);
        }
    }
    if (!part.isEmpty())
        parts.append(part);
    return parts;
}

QString newId() { return QUuid::createUuid().toString(QUuid::WithoutBraces).toUpper(); }

QJsonValue firstPresent(const QJsonObject &object, std::initializer_list<const char *> keys)
{
    for (const char *key : keys) {
        const QString name = QString::fromLatin1(key);
        if (object.contains(name))
            return object.value(name);
    }
    return QJsonValue(QJsonValue::Undefined);
}

QString stringOr(const QJsonValue &value) { return value.isString() ? value.toString() : QString(); }

std::optional<QStringList> strings(const QJsonValue &value)
{
    if (!value.isArray())
        return std::nullopt;
    QStringList out;
    for (const QJsonValue &item : value.toArray()) {
        if (!item.isString())
            return std::nullopt;
        out.append(item.toString());
    }
    return out;
}

std::optional<bool> boolean(const QJsonValue &value)
{
    if (value.isBool())
        return value.toBool();
    if (value.isDouble() && (value.toDouble() == 0 || value.toDouble() == 1))
        return value.toDouble() == 1;
    return std::nullopt;
}

QJsonArray arrayOf(const QJsonValue &value) { return value.isArray() ? value.toArray() : QJsonArray(); }

std::optional<DjTrack> importedSong(const QJsonValue &value)
{
    if (value.isString()) {
        const QString id = studio::videoId(value.toString());
        if (id.isEmpty())
            return std::nullopt;
        return crate::makeTrack(id);
    }
    if (!value.isObject())
        return std::nullopt;
    const QJsonObject object = value.toObject();
    QString id;
    for (const char *key : {"videoId", "id", "url", "link", "href"}) {
        const QJsonValue link = object.value(QString::fromLatin1(key));
        if (!link.isString())
            continue;
        id = studio::videoId(link.toString());
        if (!id.isEmpty())
            break;
    }
    if (id.isEmpty())
        return std::nullopt;
    const QJsonValue rawTags = object.value(QStringLiteral("tags"));
    QStringList tags;
    if (rawTags.isArray()) {
        for (const QJsonValue &tag : rawTags.toArray()) {
            if (tag.isString())
                tags.append(tag.toString());
        }
    } else if (rawTags.isString()) {
        tags = crate::tagNames(rawTags.toString());
    }
    return crate::makeTrack(id, stringOr(firstPresent(object, {"title", "name"})),
                            stringOr(firstPresent(object, {"author", "artist", "channel"})), tags,
                            boolean(object.value(QStringLiteral("noShuffle"))).value_or(false));
}

QList<DjTrack> importedSongs(const QJsonValue &value)
{
    QList<DjTrack> out;
    for (const QJsonValue &item : arrayOf(value)) {
        if (const auto song = importedSong(item))
            out.append(*song);
    }
    return out;
}

QStringList idsOf(const QList<DjTrack> &tracks)
{
    QStringList ids;
    ids.reserve(tracks.size());
    for (const DjTrack &track : tracks)
        ids.append(track.id);
    return ids;
}

std::optional<DjCrateImport> built(const QList<DjTrack> &found, const QList<DjImportedList> &lists)
{
    QStringList order;
    QHash<QString, DjTrack> merged;
    for (const DjTrack &track : found) {
        auto known = merged.find(track.id);
        if (known == merged.end()) {
            order.append(track.id);
            merged.insert(track.id, track);
            continue;
        }
        if (known->title.isEmpty())
            known->title = track.title;
        if (known->author.isEmpty())
            known->author = track.author;
        known->tags = crate::tagList(known->tags + track.tags);
        known->noShuffle = known->noShuffle || track.noShuffle;
    }
    if (order.isEmpty())
        return std::nullopt;
    DjCrateImport result;
    for (const QString &id : order.mid(0, crate::trackLimit))
        result.tracks.append(merged.value(id));
    result.playlists = lists.mid(0, crate::playlistLimit);
    return result;
}

std::optional<DjCrateImport> parseJson(const QJsonDocument &document)
{
    if (document.isArray())
        return built(importedSongs(document.array()), {});
    if (!document.isObject())
        return std::nullopt;
    const QJsonObject object = document.object();
    QList<DjTrack> tracks = importedSongs(firstPresent(object, {"tracks", "songs", "videos", "items"}));
    QList<DjImportedList> lists;
    for (const QJsonValue &value : arrayOf(object.value(QStringLiteral("playlists")))) {
        if (!value.isObject())
            continue;
        const QJsonObject list = value.toObject();
        const QList<DjTrack> songs = importedSongs(firstPresent(list, {"songs", "tracks", "videos", "items"}));
        tracks += songs;
        lists.append(crate::importedList(stringOr(firstPresent(list, {"name", "title"})), idsOf(songs)));
    }
    const QJsonValue name = firstPresent(object, {"name", "title"});
    const QJsonValue format = object.value(QStringLiteral("format"));
    const bool ours = format.isString() && format.toString() == QLatin1String(crate::format);
    if (!object.contains(QStringLiteral("playlists")) && !ours && name.isString() && !tracks.isEmpty())
        lists.append(crate::importedList(name.toString(), idsOf(tracks)));
    return built(tracks, lists);
}

}

DjFocus DjFocus::fromKey(const QString &key)
{
    if (key.startsWith(QLatin1String("smart:")))
        return smart(key.mid(6));
    if (key.startsWith(QLatin1String("playlist:")))
        return playlist(key.mid(9));
    if (key.startsWith(QLatin1String("tag:"))) {
        const QString name = crate::tagName(key.mid(4));
        return name.isEmpty() ? everything() : tag(name);
    }
    return everything();
}

QString DjFocus::key() const
{
    switch (kind) {
    case Kind::Everything:
        return QString();
    case Kind::Playlist:
        return QStringLiteral("playlist:") + value;
    case Kind::Tag:
        return QStringLiteral("tag:") + value;
    case Kind::Smart:
        return QStringLiteral("smart:") + value;
    }
    return QString();
}

QString DjMergeReport::summary() const
{
    const QString count = songs == 1 ? QStringLiteral("1 new song") : QStringLiteral("%1 new songs").arg(songs);
    const QString lists = playlists == 0   ? QString()
                          : playlists == 1 ? QStringLiteral(" into 1 playlist")
                                           : QStringLiteral(" into %1 playlists").arg(playlists);
    return QStringLiteral("Imported ") + count + lists + QLatin1Char('.') +
           (full ? QStringLiteral(" Some were left out: the library is full.") : QString());
}

namespace crate {

int length(const QString &text)
{
    if (text.isEmpty())
        return 0;
    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, text);
    int count = 0;
    while (finder.toNextBoundary() != -1)
        ++count;
    return count;
}

QString prefix(const QString &text, int limit)
{
    if (limit <= 0)
        return QString();
    if (text.size() <= limit)
        return text;
    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, text);
    qsizetype position = 0;
    for (int count = 0; count < limit; ++count) {
        const qsizetype next = finder.toNextBoundary();
        if (next == -1)
            break;
        position = next;
    }
    return text.left(position);
}

QString line(const QString &text, int limit)
{
    return prefix(splitWhere(text, isNewline).join(QLatin1Char(' ')), limit);
}

QString tagName(const QString &raw)
{
    return prefix(splitWhere(raw.toLower(), isTagSeparator).join(QLatin1Char(' ')), tagLength).trimmed();
}

QStringList tagNames(const QString &raw) { return tagList(raw.split(QLatin1Char(','))); }

QStringList tagList(const QStringList &names)
{
    QSet<QString> seen;
    QStringList out;
    for (const QString &raw : names) {
        if (out.size() >= tagLimit)
            break;
        const QString name = tagName(raw);
        if (name.isEmpty() || seen.contains(name))
            continue;
        seen.insert(name);
        out.append(name);
    }
    return out;
}

QString playlistName(const QString &raw)
{
    return prefix(splitWhere(raw, isWhitespace).join(QLatin1Char(' ')), nameLength);
}

DjTrack makeTrack(const QString &id, const QString &title, const QString &author, const QStringList &tags,
                  bool noShuffle)
{
    DjTrack track;
    track.id = id;
    track.title = line(title, 300);
    track.author = line(author, 200);
    track.tags = tagList(tags);
    track.noShuffle = noShuffle;
    return track;
}

DjPlaylist makePlaylist(const QString &id, const QString &name, const QStringList &songs)
{
    DjPlaylist playlist;
    playlist.id = prefix(id, 64);
    playlist.name = playlistName(name);
    QSet<QString> seen;
    for (const QString &song : songs) {
        if (!studio::isVideoId(song) || seen.contains(song))
            continue;
        seen.insert(song);
        if (playlist.songs.size() < playlistSongLimit)
            playlist.songs.append(song);
    }
    return playlist;
}

DjImportedList importedList(const QString &name, const QStringList &songs)
{
    return DjImportedList{playlistName(name), songs};
}

QJsonObject trackJson(const DjTrack &track)
{
    QJsonObject object{{QStringLiteral("id"), track.id},
                       {QStringLiteral("title"), track.title},
                       {QStringLiteral("author"), track.author},
                       {QStringLiteral("tags"), QJsonArray::fromStringList(track.tags)}};
    if (track.noShuffle)
        object.insert(QStringLiteral("noShuffle"), true);
    return object;
}

std::optional<DjTrack> trackFromJson(const QJsonObject &object)
{
    const QJsonValue id = object.value(QStringLiteral("id"));
    if (!id.isString() || !studio::isVideoId(id.toString()))
        return std::nullopt;
    const QJsonValue noShuffle = object.value(QStringLiteral("noShuffle"));
    return makeTrack(id.toString(), stringOr(object.value(QStringLiteral("title"))),
                     stringOr(object.value(QStringLiteral("author"))),
                     strings(object.value(QStringLiteral("tags"))).value_or(QStringList()),
                     noShuffle.isBool() && noShuffle.toBool());
}

QJsonObject playlistJson(const DjPlaylist &playlist)
{
    return {{QStringLiteral("id"), playlist.id},
            {QStringLiteral("name"), playlist.name},
            {QStringLiteral("songs"), QJsonArray::fromStringList(playlist.songs)}};
}

std::optional<DjPlaylist> playlistFromJson(const QJsonObject &object)
{
    const QString id = stringOr(object.value(QStringLiteral("id")));
    return makePlaylist(id.isEmpty() ? newId() : id, stringOr(object.value(QStringLiteral("name"))),
                        strings(object.value(QStringLiteral("songs"))).value_or(QStringList()));
}

}

QJsonObject DjCrate::toJson() const
{
    QJsonArray trackRows;
    for (const DjTrack &item : tracks)
        trackRows.append(crate::trackJson(item));
    QJsonArray playlistRows;
    for (const DjPlaylist &item : playlists)
        playlistRows.append(crate::playlistJson(item));
    return {{QStringLiteral("tracks"), trackRows},
            {QStringLiteral("playlists"), playlistRows},
            {QStringLiteral("focus"), focus.key()},
            {QStringLiteral("shuffle"), shuffle},
            {QStringLiteral("cursor"), cursor}};
}

DjCrate DjCrate::fromJson(const QJsonObject &object)
{
    DjCrate result;
    QSet<QString> seen;
    for (const QJsonValue &value : arrayOf(object.value(QStringLiteral("tracks")))) {
        if (!value.isObject())
            continue;
        const auto item = crate::trackFromJson(value.toObject());
        if (!item || seen.contains(item->id))
            continue;
        seen.insert(item->id);
        if (result.tracks.size() < crate::trackLimit)
            result.tracks.append(*item);
    }
    QSet<QString> ids;
    for (const QJsonValue &value : arrayOf(object.value(QStringLiteral("playlists")))) {
        if (!value.isObject())
            continue;
        const auto item = crate::playlistFromJson(value.toObject());
        if (!item || ids.contains(item->id))
            continue;
        ids.insert(item->id);
        if (result.playlists.size() < crate::playlistLimit)
            result.playlists.append(*item);
    }
    for (DjPlaylist &item : result.playlists)
        item.songs.removeIf([&](const QString &song) { return !seen.contains(song); });
    const QJsonValue shuffle = object.value(QStringLiteral("shuffle"));
    result.shuffle = shuffle.isBool() ? shuffle.toBool() : true;
    result.cursor = stringOr(object.value(QStringLiteral("cursor")));
    result.focus = DjFocus::fromKey(stringOr(object.value(QStringLiteral("focus"))));
    if (!result.focus.isSmart() && !result.knows(result.focus))
        result.focus = DjFocus::everything();
    return result;
}

QByteArray DjCrate::encoded() const { return QJsonDocument(toJson()).toJson(QJsonDocument::Compact); }

DjCrate DjCrate::decode(const QByteArray &data)
{
    const QJsonDocument document = QJsonDocument::fromJson(data);
    if (!document.isObject())
        return DjCrate();
    return fromJson(document.object());
}

int DjCrate::trackIndex(const QString &id) const
{
    for (int i = 0; i < tracks.size(); ++i) {
        if (tracks.at(i).id == id)
            return i;
    }
    return -1;
}

int DjCrate::playlistIndex(const QString &id) const
{
    for (int i = 0; i < playlists.size(); ++i) {
        if (playlists.at(i).id == id)
            return i;
    }
    return -1;
}

std::optional<DjTrack> DjCrate::track(const QString &id) const
{
    const int index = trackIndex(id);
    if (index < 0)
        return std::nullopt;
    return tracks.at(index);
}

std::optional<DjPlaylist> DjCrate::playlist(const QString &id) const
{
    const int index = playlistIndex(id);
    if (index < 0)
        return std::nullopt;
    return playlists.at(index);
}

std::optional<DjSmartList> DjCrate::smartList(const QString &id) const
{
    for (const DjSmartList &list : smart) {
        if (list.id == id)
            return list;
    }
    return std::nullopt;
}

bool DjCrate::shuffles(const QString &id) const
{
    const int index = trackIndex(id);
    return index < 0 || !tracks.at(index).noShuffle;
}

QSet<QString> DjCrate::unshuffled() const
{
    QSet<QString> out;
    for (const DjTrack &item : tracks) {
        if (item.noShuffle)
            out.insert(item.id);
    }
    return out;
}

void DjCrate::categorize(const QList<DjTrack> &songs, const QList<DjPlayedSong> &library, double now)
{
    setSmart(smart::lists(songs, library, now));
}

void DjCrate::setSmart(const QList<DjSmartList> &lists)
{
    smart = lists;
    if (focus.isSmart() && !knows(focus))
        setFocus(DjFocus::everything());
}

bool DjCrate::setShuffles(const QString &id, bool on, const QString &title, const QString &author)
{
    if (on) {
        const int index = trackIndex(id);
        if (index >= 0)
            tracks[index].noShuffle = false;
        return true;
    }
    if (!keep(id, title, author))
        return false;
    const int index = trackIndex(id);
    if (index < 0)
        return false;
    tracks[index].noShuffle = true;
    return true;
}

bool DjCrate::knows(const DjFocus &target) const
{
    switch (target.kind) {
    case DjFocus::Kind::Everything:
        return true;
    case DjFocus::Kind::Playlist:
        return playlistIndex(target.value) >= 0;
    case DjFocus::Kind::Tag:
        return std::any_of(tracks.begin(), tracks.end(),
                           [&](const DjTrack &item) { return item.tags.contains(target.value); });
    case DjFocus::Kind::Smart:
        return smartList(target.value).has_value();
    }
    return false;
}

QList<DjTagCount> DjCrate::tagCounts() const
{
    QMap<QString, int> counts;
    for (const DjTrack &item : tracks) {
        for (const QString &name : item.tags)
            counts[name] += 1;
    }
    QList<DjTagCount> out;
    for (auto it = counts.cbegin(); it != counts.cend(); ++it)
        out.append(DjTagCount{it.key(), it.value()});
    return out;
}

QString DjCrate::label(const DjFocus &target) const
{
    switch (target.kind) {
    case DjFocus::Kind::Everything:
        return QStringLiteral("Everything");
    case DjFocus::Kind::Playlist: {
        const auto list = playlist(target.value);
        return list ? list->name : QStringLiteral("Playlist");
    }
    case DjFocus::Kind::Tag:
        return QStringLiteral("#") + target.value;
    case DjFocus::Kind::Smart: {
        const auto list = smartList(target.value);
        return list ? list->name : QStringLiteral("Smart Playlist");
    }
    }
    return QString();
}

QList<DjTrack> DjCrate::songs(const DjFocus &target) const
{
    switch (target.kind) {
    case DjFocus::Kind::Everything:
        return tracks;
    case DjFocus::Kind::Tag: {
        QList<DjTrack> out;
        for (const DjTrack &item : tracks) {
            if (item.tags.contains(target.value))
                out.append(item);
        }
        return out;
    }
    case DjFocus::Kind::Playlist:
    case DjFocus::Kind::Smart:
        break;
    }
    QHash<QString, int> known;
    for (int i = 0; i < tracks.size(); ++i) {
        if (!known.contains(tracks.at(i).id))
            known.insert(tracks.at(i).id, i);
    }
    QList<DjTrack> out;
    if (target.kind == DjFocus::Kind::Playlist) {
        const auto list = playlist(target.value);
        if (!list)
            return out;
        for (const QString &id : list->songs) {
            const auto found = known.constFind(id);
            if (found != known.cend())
                out.append(tracks.at(*found));
        }
        return out;
    }
    const auto list = smartList(target.value);
    if (!list)
        return out;
    for (const DjTrack &song : list->songs) {
        const auto found = known.constFind(song.id);
        out.append(found != known.cend() ? tracks.at(*found) : song);
    }
    return out;
}

bool DjCrate::keep(const QString &id, const QString &title, const QString &author)
{
    if (!studio::isVideoId(id))
        return false;
    const int index = trackIndex(id);
    if (index >= 0) {
        if (!title.isEmpty())
            tracks[index].title = crate::line(title, 300);
        if (!author.isEmpty())
            tracks[index].author = crate::line(author, 200);
        return true;
    }
    if (tracks.size() >= crate::trackLimit)
        return false;
    tracks.append(crate::makeTrack(id, title, author));
    return true;
}

bool DjCrate::tag(const QString &id, const QString &raw, const QString &title, const QString &author)
{
    const QStringList names = crate::tagNames(raw);
    if (names.isEmpty() || !keep(id, title, author))
        return false;
    const int index = trackIndex(id);
    if (index < 0)
        return false;
    tracks[index].tags = crate::tagList(tracks[index].tags + names);
    return true;
}

void DjCrate::untag(const QString &id, const QString &name)
{
    const int index = trackIndex(id);
    if (index < 0)
        return;
    tracks[index].tags.removeAll(name);
    if (focus == DjFocus::tag(name) && !knows(focus))
        setFocus(DjFocus::everything());
}

bool DjCrate::renameTag(const QString &old, const QString &raw)
{
    const QString name = crate::tagName(raw);
    if (name.isEmpty())
        return false;
    if (name == old)
        return true;
    for (DjTrack &item : tracks) {
        if (!item.tags.contains(old))
            continue;
        QStringList renamed = item.tags;
        for (QString &value : renamed) {
            if (value == old)
                value = name;
        }
        item.tags = crate::tagList(renamed);
    }
    if (focus == DjFocus::tag(old))
        focus = DjFocus::tag(name);
    return true;
}

void DjCrate::deleteTag(const QString &name)
{
    for (DjTrack &item : tracks)
        item.tags.removeAll(name);
    if (focus == DjFocus::tag(name))
        setFocus(DjFocus::everything());
}

QString DjCrate::uniqueName(const QString &raw, const std::optional<QString> &except) const
{
    const QString cleaned = crate::playlistName(raw);
    const QString base = cleaned.isEmpty() ? QStringLiteral("New Playlist") : cleaned;
    QSet<QString> taken;
    for (const DjPlaylist &list : playlists) {
        if (!except || list.id != *except)
            taken.insert(list.name.toLower());
    }
    if (!taken.contains(base.toLower()))
        return base;
    int number = 2;
    while (taken.contains((base + QLatin1Char(' ') + QString::number(number)).toLower()))
        ++number;
    return base + QLatin1Char(' ') + QString::number(number);
}

std::optional<QString> DjCrate::createPlaylist(const QString &raw, const QString &id)
{
    if (playlists.size() >= crate::playlistLimit)
        return std::nullopt;
    const QString chosen = id.isEmpty() ? newId() : id;
    playlists.append(crate::makePlaylist(chosen, uniqueName(raw)));
    return chosen;
}

void DjCrate::renamePlaylist(const QString &id, const QString &raw)
{
    const int index = playlistIndex(id);
    if (index < 0 || crate::playlistName(raw).isEmpty())
        return;
    playlists[index].name = uniqueName(raw, id);
}

void DjCrate::deletePlaylist(const QString &id)
{
    playlists.removeIf([&](const DjPlaylist &list) { return list.id == id; });
    if (focus == DjFocus::playlist(id))
        setFocus(DjFocus::everything());
}

int DjCrate::add(const QList<DjTrack> &songs, const QString &id)
{
    const int index = playlistIndex(id);
    if (index < 0)
        return 0;
    int added = 0;
    for (const DjTrack &song : songs) {
        if (playlists[index].songs.size() >= crate::playlistSongLimit || playlists[index].songs.contains(song.id))
            continue;
        if (!keep(song.id, song.title, song.author))
            continue;
        playlists[index].songs.append(song.id);
        ++added;
    }
    return added;
}

void DjCrate::remove(const QString &song, const QString &id)
{
    const int index = playlistIndex(id);
    if (index >= 0)
        playlists[index].songs.removeAll(song);
}

void DjCrate::removeSongs(const QList<int> &offsets, const QString &id)
{
    const int index = playlistIndex(id);
    if (index < 0)
        return;
    const QSet<int> drop(offsets.begin(), offsets.end());
    QStringList kept;
    const QStringList &current = playlists[index].songs;
    for (int i = 0; i < current.size(); ++i) {
        if (!drop.contains(i))
            kept.append(current.at(i));
    }
    playlists[index].songs = kept;
}

void DjCrate::moveSongs(const QString &id, const QList<int> &offsets, int destination)
{
    const int index = playlistIndex(id);
    if (index < 0)
        return;
    QList<int> sorted = offsets;
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    const QSet<int> picked(sorted.begin(), sorted.end());
    const QStringList songs = playlists[index].songs;
    QStringList moving;
    for (const int offset : sorted) {
        if (offset >= 0 && offset < songs.size())
            moving.append(songs.at(offset));
    }
    QStringList rest;
    for (int i = 0; i < songs.size(); ++i) {
        if (!picked.contains(i))
            rest.append(songs.at(i));
    }
    const int before = int(std::count_if(sorted.begin(), sorted.end(), [&](int offset) { return offset < destination; }));
    const int place = std::min(int(rest.size()), std::max(0, destination - before));
    for (int i = 0; i < moving.size(); ++i)
        rest.insert(place + i, moving.at(i));
    playlists[index].songs = rest;
}

void DjCrate::removeTrack(const QString &id)
{
    tracks.removeIf([&](const DjTrack &item) { return item.id == id; });
    for (DjPlaylist &list : playlists)
        list.songs.removeAll(id);
    if (cursor == id)
        cursor.clear();
    if (!knows(focus))
        setFocus(DjFocus::everything());
}

DjMove DjCrate::move(const QString &id, const DjFocus &target, const QString &title, const QString &author)
{
    switch (target.kind) {
    case DjFocus::Kind::Playlist: {
        const auto list = playlist(target.value);
        const bool already = list && list->songs.contains(id);
        if (!already && add({crate::makeTrack(id, title, author)}, target.value) == 0)
            return DjMove::Full;
        break;
    }
    case DjFocus::Kind::Tag:
        if (!tag(id, target.value, title, author))
            return DjMove::Full;
        break;
    case DjFocus::Kind::Everything:
    case DjFocus::Kind::Smart:
        break;
    }
    const DjFocus from = focus;
    if (from == target || !from.isFiled())
        return DjMove::Added;
    bool left = false;
    if (from.kind == DjFocus::Kind::Playlist) {
        const auto list = playlist(from.value);
        left = list && list->songs.contains(id);
        remove(id, from.value);
    } else if (from.kind == DjFocus::Kind::Tag) {
        const auto item = track(id);
        left = item && item->tags.contains(from.value);
        untag(id, from.value);
    }
    if (target.isEverything())
        return DjMove::Removed;
    return left ? DjMove::Moved : DjMove::Added;
}

void DjCrate::setFocus(const DjFocus &target)
{
    focus = knows(target) ? target : DjFocus::everything();
    cursor.clear();
}

QList<DjPlayedSong> DjCrate::pool(const QList<DjPlayedSong> &library, const QSet<QString> &skip) const
{
    QHash<QString, int> played;
    for (int i = 0; i < library.size(); ++i) {
        if (!played.contains(library.at(i).id))
            played.insert(library.at(i).id, i);
    }
    const auto fresh = [](const DjTrack &item) {
        DjPlayedSong song;
        song.id = item.id;
        song.title = item.title;
        song.author = item.author;
        song.plays = 0;
        return song;
    };
    QList<DjPlayedSong> candidates;
    if (focus.isEverything()) {
        candidates = library;
        for (const DjTrack &item : tracks) {
            if (!played.contains(item.id))
                candidates.append(fresh(item));
        }
    } else {
        for (const DjTrack &item : songs(focus)) {
            const auto found = played.constFind(item.id);
            if (found == played.cend()) {
                candidates.append(fresh(item));
                continue;
            }
            DjPlayedSong song = library.at(*found);
            if (song.title.isEmpty())
                song.title = item.title;
            if (song.author.isEmpty())
                song.author = item.author;
            candidates.append(song);
        }
    }
    const QSet<QString> held = focus.isEverything() || shuffle ? unshuffled() : QSet<QString>();
    QList<DjPlayedSong> out;
    for (const DjPlayedSong &song : candidates) {
        if (!skip.contains(song.id) && !held.contains(song.id))
            out.append(song);
    }
    return out;
}

std::optional<DjPlayedSong> DjCrate::next(const QList<DjPlayedSong> &library, const QSet<QString> &loaded,
                                          const QSet<QString> &playing, quint64 seed, const QSet<QString> &skip)
{
    const QList<DjPlayedSong> candidates = pool(library, skip);
    if (candidates.isEmpty())
        return std::nullopt;
    std::optional<DjPlayedSong> pick;
    if (focus.isEverything() || shuffle) {
        pick = automix::next(candidates, loaded, playing, seed);
    } else {
        int start = 0;
        for (int i = 0; i < candidates.size(); ++i) {
            if (candidates.at(i).id == cursor) {
                start = i + 1;
                break;
            }
        }
        const int count = int(candidates.size());
        for (int i = 0; i < count && !pick; ++i) {
            const DjPlayedSong &song = candidates.at((start + i) % count);
            if (!loaded.contains(song.id) && !playing.contains(song.id))
                pick = song;
        }
        for (int i = 0; i < count && !pick; ++i) {
            const DjPlayedSong &song = candidates.at((start + i) % count);
            if (!playing.contains(song.id))
                pick = song;
        }
    }
    if (pick)
        cursor = pick->id;
    return pick;
}

QByteArray DjCrate::exported(const DjFocus &scope, const QDateTime &date) const
{
    QList<DjPlaylist> lists;
    switch (scope.kind) {
    case DjFocus::Kind::Everything:
        lists = playlists;
        break;
    case DjFocus::Kind::Playlist:
        if (const auto list = playlist(scope.value))
            lists.append(*list);
        break;
    case DjFocus::Kind::Tag:
        break;
    case DjFocus::Kind::Smart:
        if (const auto list = smartList(scope.value))
            lists.append(crate::makePlaylist(list->id, list->name, idsOf(list->songs)));
        break;
    }
    QJsonArray trackRows;
    for (const DjTrack &item : songs(scope))
        trackRows.append(crate::trackJson(item));
    QJsonArray listRows;
    for (const DjPlaylist &list : lists) {
        listRows.append(QJsonObject{{QStringLiteral("name"), list.name},
                                    {QStringLiteral("songs"), QJsonArray::fromStringList(list.songs)}});
    }
    const QJsonObject file{{QStringLiteral("format"), QLatin1String(crate::format)},
                           {QStringLiteral("version"), 1},
                           {QStringLiteral("exported"), date.toUTC().toString(Qt::ISODate)},
                           {QStringLiteral("tracks"), trackRows},
                           {QStringLiteral("playlists"), listRows}};
    return QJsonDocument(file).toJson(QJsonDocument::Indented);
}

QByteArray DjCrate::m3u(const DjFocus &scope) const
{
    QStringList lines{QStringLiteral("#EXTM3U")};
    for (const DjTrack &item : songs(scope)) {
        lines.append(QStringLiteral("#EXTINF:-1,") + item.shownTitle());
        lines.append(studio::watchUrl(item.id).toString());
    }
    return (lines.join(QLatin1Char('\n')) + QLatin1Char('\n')).toUtf8();
}

QString DjCrate::fileName(const DjFocus &scope) const
{
    QString name;
    switch (scope.kind) {
    case DjFocus::Kind::Everything:
        name = QStringLiteral("DJ Playlists");
        break;
    case DjFocus::Kind::Playlist: {
        const auto list = playlist(scope.value);
        name = list ? list->name : QStringLiteral("Playlist");
        break;
    }
    case DjFocus::Kind::Tag:
        name = QStringLiteral("Tag ") + scope.value;
        break;
    case DjFocus::Kind::Smart: {
        const auto list = smartList(scope.value);
        name = list ? list->name : QStringLiteral("Smart Playlist");
        break;
    }
    }
    QString safe;
    for (const char32_t u : name.toUcs4()) {
        const bool keepIt = QChar::isLetterOrNumber(u) || QChar::isMark(u) || u == U' ' || u == U'-' || u == U'_';
        if (keepIt)
            safe.append(QString::fromUcs4(&u, 1));
        else
            safe.append(QLatin1Char('-'));
    }
    const QString trimmed = safe.trimmed();
    return trimmed.isEmpty() ? QStringLiteral("DJ Playlist") : trimmed;
}

std::optional<DjCrateImport> DjCrate::parse(const QByteArray &data)
{
    if (data.isEmpty() || data.size() > crate::importLimit)
        return std::nullopt;
    const QByteArray bom("\xEF\xBB\xBF");
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(data.startsWith(bom) ? data.mid(3) : data, &error);
    if (error.error == QJsonParseError::NoError && (document.isArray() || document.isObject()))
        return parseJson(document);
    QStringDecoder decoder(QStringDecoder::Utf8);
    const QString text = decoder.decode(data);
    if (decoder.hasError())
        return std::nullopt;
    return parseText(text);
}

std::optional<DjCrateImport> DjCrate::parseText(const QString &text)
{
    QList<DjTrack> found;
    QString title;
    for (const QString &raw : splitWhere(text, isNewline)) {
        const QString row = raw.trimmed();
        if (row.toUpper().startsWith(QLatin1String("#EXTINF"))) {
            const qsizetype comma = row.indexOf(QLatin1Char(','));
            title = comma < 0 ? QString() : row.mid(comma + 1).trimmed();
            continue;
        }
        if (row.startsWith(QLatin1Char('#')))
            continue;
        const QString id = studio::videoId(row);
        if (!id.isEmpty())
            found.append(crate::makeTrack(id, title));
        title.clear();
    }
    return built(found, {});
}

DjMergeReport DjCrate::merge(const DjCrateImport &incoming, const QString &fallback)
{
    DjMergeReport report;
    for (const DjTrack &item : incoming.tracks) {
        const int index = trackIndex(item.id);
        if (index >= 0) {
            DjTrack &known = tracks[index];
            if (known.title.isEmpty())
                known.title = item.title;
            if (known.author.isEmpty())
                known.author = item.author;
            known.tags = crate::tagList(known.tags + item.tags);
            if (item.noShuffle)
                known.noShuffle = true;
        } else if (tracks.size() < crate::trackLimit) {
            tracks.append(item);
            report.songs += 1;
        } else {
            report.full = true;
        }
    }
    QList<DjImportedList> lists = incoming.playlists;
    const bool tagged = std::any_of(incoming.tracks.begin(), incoming.tracks.end(),
                                    [](const DjTrack &item) { return !item.tags.isEmpty(); });
    if (lists.isEmpty() && !tagged)
        lists = {crate::importedList(fallback, idsOf(incoming.tracks))};
    for (const DjImportedList &list : lists) {
        const QString name = list.name.isEmpty() ? crate::playlistName(fallback) : list.name;
        QList<DjTrack> members;
        for (const QString &id : list.songs) {
            if (const auto item = track(id))
                members.append(*item);
        }
        std::optional<QString> id;
        for (const DjPlaylist &existing : playlists) {
            if (existing.name.toLower() == name.toLower()) {
                id = existing.id;
                break;
            }
        }
        if (!id)
            id = createPlaylist(name);
        if (!id) {
            report.full = true;
            continue;
        }
        add(members, *id);
        report.playlists += 1;
    }
    return report;
}

}
