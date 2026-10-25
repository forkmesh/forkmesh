#pragma once

#include "DjCore.h"
#include "DjSmart.h"

#include <QByteArray>
#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

#include <optional>

namespace forkmesh::djstudio {

struct DjPlaylist {
    QString id;
    QString name;
    QStringList songs;
    bool operator==(const DjPlaylist &) const = default;
};

struct DjFocus {
    enum class Kind { Everything, Playlist, Tag, Smart };
    Kind kind = Kind::Everything;
    QString value;

    static DjFocus everything() { return {}; }
    static DjFocus playlist(const QString &id) { return {Kind::Playlist, id}; }
    static DjFocus tag(const QString &name) { return {Kind::Tag, name}; }
    static DjFocus smart(const QString &id) { return {Kind::Smart, id}; }
    static DjFocus fromKey(const QString &key);
    QString key() const;
    bool isEverything() const { return kind == Kind::Everything; }
    bool isSmart() const { return kind == Kind::Smart; }
    bool isFiled() const { return kind == Kind::Playlist || kind == Kind::Tag; }
    bool operator==(const DjFocus &) const = default;
};

struct DjTagCount {
    QString name;
    int count = 0;
    bool operator==(const DjTagCount &) const = default;
};

struct DjImportedList {
    QString name;
    QStringList songs;
    bool operator==(const DjImportedList &) const = default;
};

struct DjCrateImport {
    QList<DjTrack> tracks;
    QList<DjImportedList> playlists;
    bool operator==(const DjCrateImport &) const = default;
};

struct DjMergeReport {
    int songs = 0;
    int playlists = 0;
    bool full = false;
    QString summary() const;
    bool operator==(const DjMergeReport &) const = default;
};

enum class DjMove { Full, Added, Moved, Removed };

namespace crate {

inline constexpr char storageKey[] = "fm.videoMixDjStudio.crate.v1";
inline constexpr char format[] = "forkmesh-dj-playlists";
inline constexpr int trackLimit = 2000;
inline constexpr int playlistLimit = 100;
inline constexpr int playlistSongLimit = 1000;
inline constexpr int tagLimit = 20;
inline constexpr int tagLength = 32;
inline constexpr int nameLength = 80;
inline constexpr int importLimit = 4000000;

int length(const QString &text);
QString prefix(const QString &text, int limit);
QString line(const QString &text, int limit);
QString tagName(const QString &raw);
QStringList tagNames(const QString &raw);
QStringList tagList(const QStringList &names);
QString playlistName(const QString &raw);
DjTrack makeTrack(const QString &id, const QString &title = QString(), const QString &author = QString(),
                  const QStringList &tags = QStringList(), bool noShuffle = false);
DjPlaylist makePlaylist(const QString &id, const QString &name, const QStringList &songs = QStringList());
DjImportedList importedList(const QString &name, const QStringList &songs);
QJsonObject trackJson(const DjTrack &track);
std::optional<DjTrack> trackFromJson(const QJsonObject &object);
QJsonObject playlistJson(const DjPlaylist &playlist);
std::optional<DjPlaylist> playlistFromJson(const QJsonObject &object);

}

class DjCrate
{
public:
    QList<DjTrack> tracks;
    QList<DjPlaylist> playlists;
    DjFocus focus;
    bool shuffle = true;
    QString cursor;
    QList<DjSmartList> smart;

    bool operator==(const DjCrate &) const = default;

    QJsonObject toJson() const;
    static DjCrate fromJson(const QJsonObject &object);
    QByteArray encoded() const;
    static DjCrate decode(const QByteArray &data);

    std::optional<DjTrack> track(const QString &id) const;
    std::optional<DjPlaylist> playlist(const QString &id) const;
    std::optional<DjSmartList> smartList(const QString &id) const;
    bool shuffles(const QString &id) const;
    QSet<QString> unshuffled() const;

    void categorize(const QList<DjTrack> &songs, const QList<DjPlayedSong> &library, double now);
    void setSmart(const QList<DjSmartList> &lists);
    bool setShuffles(const QString &id, bool on, const QString &title = QString(),
                     const QString &author = QString());

    bool knows(const DjFocus &focus) const;
    QList<DjTagCount> tagCounts() const;
    QString label(const DjFocus &focus) const;
    QList<DjTrack> songs(const DjFocus &focus) const;

    bool keep(const QString &id, const QString &title = QString(), const QString &author = QString());
    bool tag(const QString &id, const QString &raw, const QString &title = QString(),
             const QString &author = QString());
    void untag(const QString &id, const QString &name);
    bool renameTag(const QString &old, const QString &raw);
    void deleteTag(const QString &name);

    QString uniqueName(const QString &raw, const std::optional<QString> &except = std::nullopt) const;
    std::optional<QString> createPlaylist(const QString &raw, const QString &id = QString());
    void renamePlaylist(const QString &id, const QString &raw);
    void deletePlaylist(const QString &id);
    int add(const QList<DjTrack> &songs, const QString &id);
    void remove(const QString &song, const QString &id);
    void removeSongs(const QList<int> &offsets, const QString &id);
    void moveSongs(const QString &id, const QList<int> &offsets, int destination);
    void removeTrack(const QString &id);
    DjMove move(const QString &id, const DjFocus &target, const QString &title = QString(),
                const QString &author = QString());

    void setFocus(const DjFocus &focus);
    QList<DjPlayedSong> pool(const QList<DjPlayedSong> &library, const QSet<QString> &skip = {}) const;
    std::optional<DjPlayedSong> next(const QList<DjPlayedSong> &library, const QSet<QString> &loaded,
                                     const QSet<QString> &playing, quint64 seed,
                                     const QSet<QString> &skip = {});

    QByteArray exported(const DjFocus &scope, const QDateTime &date = QDateTime::currentDateTimeUtc()) const;
    QByteArray m3u(const DjFocus &scope) const;
    QString fileName(const DjFocus &scope) const;
    static std::optional<DjCrateImport> parse(const QByteArray &data);
    static std::optional<DjCrateImport> parseText(const QString &text);
    DjMergeReport merge(const DjCrateImport &incoming, const QString &fallback);

private:
    int trackIndex(const QString &id) const;
    int playlistIndex(const QString &id) const;
};

}
