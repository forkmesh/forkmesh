#include "DjCore.h"

#include <QJsonDocument>
#include <QJsonValue>
#include <QRegularExpression>
#include <QUrlQuery>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace forkmesh::djstudio {

namespace {

double finiteOr(double value, double fallback) { return std::isfinite(value) ? value : fallback; }

}

QJsonObject DjQueueItem::toJson() const
{
    QJsonObject object{{QStringLiteral("id"), id},
                       {QStringLiteral("videoId"), videoId},
                       {QStringLiteral("title"), title},
                       {QStringLiteral("author"), author}};
    if (!addedBy.isEmpty())
        object.insert(QStringLiteral("addedBy"), addedBy);
    if (addedAt > 0)
        object.insert(QStringLiteral("addedAt"), double(addedAt));
    return object;
}

std::optional<DjQueueItem> DjQueueItem::fromJson(const QJsonObject &object)
{
    const QString video = object.value(QStringLiteral("videoId")).toString();
    if (!studio::isVideoId(video))
        return std::nullopt;
    DjQueueItem item;
    item.id = object.value(QStringLiteral("id")).toString().left(64);
    if (item.id.isEmpty())
        item.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    item.videoId = video;
    item.title = object.value(QStringLiteral("title")).toString().left(300);
    item.author = object.value(QStringLiteral("author")).toString().left(200);
    item.addedBy = object.value(QStringLiteral("addedBy")).toString().left(64);
    item.addedAt = std::max<qint64>(0, qint64(object.value(QStringLiteral("addedAt")).toDouble()));
    return item;
}

quint64 DjRandom::next()
{
    state += 0x9E3779B97F4A7C15ULL;
    quint64 value = state;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31);
}

double DjRandom::unit() { return double(next() >> 11) / double(1ULL << 53); }

namespace studio {

bool isVideoId(const QString &value)
{
    if (value.size() != 11)
        return false;
    for (const QChar c : value) {
        const ushort u = c.unicode();
        const bool ok = (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') ||
                        u == '_' || u == '-';
        if (!ok)
            return false;
    }
    return true;
}

QString videoId(const QString &value)
{
    QString raw = value.trimmed();
    if (raw.startsWith(QLatin1Char('<')))
        raw.remove(0, 1);
    if (raw.endsWith(QLatin1Char('>')))
        raw.chop(1);
    if (isVideoId(raw))
        return raw;
    QString text = raw;
    static const QRegularExpression bare(
        QStringLiteral("^(?:www\\.|m\\.|music\\.)?(?:youtube(?:-nocookie)?\\.com|youtu\\.be)/"),
        QRegularExpression::CaseInsensitiveOption);
    if (bare.match(text).hasMatch())
        text = QStringLiteral("https://") + text;
    const QUrl url(text, QUrl::StrictMode);
    if (url.isValid() && !url.host().isEmpty()) {
        static const QRegularExpression prefix(QStringLiteral("^(?:www\\.|m\\.|music\\.)"));
        QString host = url.host().toLower();
        host.remove(prefix);
        const QUrlQuery query(url);
        const auto item = [&](const QString &name) { return query.queryItemValue(name, QUrl::FullyDecoded); };
        const QStringList segments = url.path().split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (host == QLatin1String("youtu.be")) {
            const QString id = segments.isEmpty() ? item(QStringLiteral("v")) : segments.first();
            return isVideoId(id) ? id : QString();
        }
        if (host == QLatin1String("youtube.com") || host == QLatin1String("youtube-nocookie.com")) {
            QStringList candidates{item(QStringLiteral("v")), item(QStringLiteral("vi")),
                                   item(QStringLiteral("video_id"))};
            if (url.path() == QLatin1String("/attribution_link")) {
                QString inner = item(QStringLiteral("u"));
                if (!inner.isEmpty()) {
                    if (!inner.startsWith(QLatin1Char('/')))
                        inner.prepend(QLatin1Char('/'));
                    const QUrl nested(QStringLiteral("https://www.youtube.com") + inner);
                    candidates.append(QUrlQuery(nested).queryItemValue(QStringLiteral("v")));
                }
            }
            static const QStringList kinds{QStringLiteral("embed"), QStringLiteral("shorts"),
                                           QStringLiteral("live"), QStringLiteral("v"),
                                           QStringLiteral("e"), QStringLiteral("watch")};
            for (int i = 0; i < segments.size(); ++i) {
                if (kinds.contains(segments.at(i))) {
                    if (i + 1 < segments.size())
                        candidates.append(segments.at(i + 1));
                    break;
                }
            }
            static const QRegularExpression fragment(QStringLiteral("[?&]v=([A-Za-z0-9_-]{11})"));
            const auto match = fragment.match(url.fragment());
            if (match.hasMatch())
                candidates.append(match.captured(1));
            for (const QString &candidate : candidates) {
                if (isVideoId(candidate))
                    return candidate;
            }
            return QString();
        }
    }
    static const QRegularExpression loose(
        QStringLiteral("(?:youtu\\.be/|youtube(?:-nocookie)?\\.com/(?:watch\\?(?:[^#\\s]*&)?v=|(?:embed|shorts|live|v|e)/))([A-Za-z0-9_-]{11})"),
        QRegularExpression::CaseInsensitiveOption);
    const auto match = loose.match(raw);
    return match.hasMatch() ? match.captured(1) : QString();
}

QStringList sharedVideoIds(const QStringList &urls, const QString &text)
{
    const QString typed = text.trimmed();
    QStringList links = urls;
    static const QRegularExpression space(QStringLiteral("\\s"));
    if (links.isEmpty() && !typed.isEmpty() && !isVideoId(typed) && !typed.contains(space))
        links = {typed};
    QStringList ids;
    for (const QString &link : links) {
        const QString id = videoId(link);
        if (id.isEmpty())
            return {};
        if (!ids.contains(id))
            ids.append(id);
    }
    return ids;
}

QUrl watchUrl(const QString &id)
{
    QUrl url(QStringLiteral("https://www.youtube.com/watch"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("v"), id);
    url.setQuery(query);
    return url;
}

QUrl thumbnailUrl(const QString &id, const QString &quality)
{
    return QUrl(QStringLiteral("https://i.ytimg.com/vi/%1/%2.jpg").arg(id, quality));
}

QPair<double, double> gains(double crossfader)
{
    const double position = (std::clamp(finiteOr(crossfader, 0), -1.0, 1.0) + 1) / 2;
    double a = std::cos(position * M_PI / 2);
    double b = std::sin(position * M_PI / 2);
    if (std::abs(a) < 1e-9)
        a = 0;
    if (std::abs(b) < 1e-9)
        b = 0;
    return {a, b};
}

QString clock(double seconds)
{
    const int total = std::isfinite(seconds) ? std::max(0, int(seconds)) : 0;
    const int hours = total / 3600;
    const int minutes = total % 3600 / 60;
    const int secs = total % 60;
    if (hours > 0)
        return QStringLiteral("%1:%2:%3").arg(hours).arg(minutes, 2, 10, QLatin1Char('0')).arg(secs, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1:%2").arg(minutes, 2, 10, QLatin1Char('0')).arg(secs, 2, 10, QLatin1Char('0'));
}

double clampRate(double rate, double range)
{
    if (!std::isfinite(rate))
        return 1;
    return std::clamp(rate, 1 - range, 1 + range);
}

double syncedRate(double ownBpm, double otherBpm, double otherRate, double range)
{
    if (ownBpm > 0 && otherBpm > 0)
        return clampRate(otherBpm * otherRate / ownBpm, range);
    return clampRate(otherRate, range);
}

double scratchSeconds(double radians) { return radians / (2 * M_PI) * secondsPerTurn; }

double scratchSpeed(double previous, double seconds, double elapsed)
{
    if (!std::isfinite(seconds) || !std::isfinite(elapsed) || elapsed <= 0)
        return previous;
    const double instant = std::clamp(seconds / std::max(0.008, elapsed), -8.0, 8.0);
    return previous * 0.4 + instant * 0.6;
}

bool isScratch(double dx, double dy, double threshold)
{
    if (!std::isfinite(dx) || !std::isfinite(dy))
        return false;
    return std::sqrt(dx * dx + dy * dy) >= threshold;
}

std::optional<DjSection> sampleSection(int deck, double start, double end)
{
    if (!std::isfinite(start) || !std::isfinite(end) || end - start < shortestSample)
        return std::nullopt;
    DjSection section;
    section.deck = deck;
    section.start = std::max(0.0, start);
    section.duration = std::clamp(end - start, 1.0, 30.0);
    return section;
}

double trackedPosition(double predicted, double reported, bool continuous)
{
    if (!std::isfinite(reported))
        return predicted;
    if (!continuous || !std::isfinite(predicted))
        return reported;
    const double error = reported - predicted;
    return std::abs(error) > 0.3 ? reported : predicted + error * 0.35;
}

DjWaveBar amplitude(const QVector<DjWaveBar> &bars, double time, double duration)
{
    if (bars.isEmpty() || duration <= 0 || !std::isfinite(time) || time < 0 || time > duration)
        return {};
    const int count = bars.size();
    const double place = time / duration * count - 0.5;
    const int lower = std::clamp(int(std::floor(place)), 0, count - 1);
    const int upper = std::min(count - 1, lower + 1);
    const double blend = std::clamp(place - lower, 0.0, 1.0);
    const DjWaveBar &a = bars.at(lower);
    const DjWaveBar &b = bars.at(upper);
    DjWaveBar out;
    out.level = a.level * (1 - blend) + b.level * blend;
    out.low = a.low * (1 - blend) + b.low * blend;
    out.mid = a.mid * (1 - blend) + b.mid * blend;
    out.high = a.high * (1 - blend) + b.high * blend;
    return out;
}

QList<DjHistoryItem> remember(const QList<DjHistoryItem> &history, const QString &id,
                              const QString &title, const QString &author, int limit)
{
    if (!isVideoId(id) || limit <= 0)
        return history;
    const auto known = std::find_if(history.begin(), history.end(), [&](const DjHistoryItem &h) { return h.id == id; });
    DjHistoryItem item{id, (title.isEmpty() && known != history.end() ? known->title : title).left(300),
                       (author.isEmpty() && known != history.end() ? known->author : author).left(200)};
    QList<DjHistoryItem> out{item};
    for (const DjHistoryItem &h : history) {
        if (h.id != id)
            out.append(h);
        if (out.size() >= limit)
            break;
    }
    return out.mid(0, limit);
}

QList<DjPlayedSong> played(const QList<DjPlayedSong> &library, const QString &id,
                           const QString &title, const QString &author, double now, int limit)
{
    if (!isVideoId(id) || limit <= 0)
        return library;
    const auto known = std::find_if(library.begin(), library.end(), [&](const DjPlayedSong &s) { return s.id == id; });
    const bool has = known != library.end();
    DjPlayedSong song;
    song.id = id;
    song.title = (title.isEmpty() && has ? known->title : title).left(300);
    song.author = (author.isEmpty() && has ? known->author : author).left(200);
    song.plays = (has ? known->plays : 0) + 1;
    song.last = std::isfinite(now) ? now : (has ? known->last : 0);
    QList<DjPlayedSong> out{song};
    for (const DjPlayedSong &s : library) {
        if (s.id != id)
            out.append(s);
        if (out.size() >= limit)
            break;
    }
    return out.mid(0, limit);
}

QList<DjPlayedSong> retitled(const QList<DjPlayedSong> &library, const QString &id,
                             const QString &title, const QString &author)
{
    QList<DjPlayedSong> out = library;
    for (DjPlayedSong &song : out) {
        if (song.id != id)
            continue;
        if (!title.isEmpty())
            song.title = title.left(300);
        if (!author.isEmpty())
            song.author = author.left(200);
    }
    return out;
}

QString playsLabel(int plays)
{
    if (plays <= 0)
        return QString();
    return plays == 1 ? QStringLiteral("1 play") : QStringLiteral("%1 plays").arg(plays);
}

QList<DjPlayedSong> mostPlayed(const QList<DjPlayedSong> &library, int limit)
{
    QVector<int> order;
    for (int i = 0; i < library.size(); ++i) {
        if (library.at(i).plays > 1)
            order.append(i);
    }
    std::stable_sort(order.begin(), order.end(), [&](int l, int r) { return library.at(l).plays > library.at(r).plays; });
    QList<DjPlayedSong> out;
    for (int index : order) {
        if (out.size() >= limit)
            break;
        out.append(library.at(index));
    }
    return out;
}

double turnDegrees(double seconds) { return std::fmod(seconds / secondsPerTurn * 360, 360); }

QList<QUrl> searchInstances(const QByteArray &data, int limit)
{
    const QJsonArray rows = QJsonDocument::fromJson(data).array();
    QList<QUrl> found;
    for (const QJsonValue &row : rows) {
        const QJsonArray pair = row.toArray();
        if (pair.size() < 2)
            continue;
        const QJsonObject details = pair.at(1).toObject();
        if (!details.value(QStringLiteral("api")).toBool())
            continue;
        QString uri = details.value(QStringLiteral("uri")).toString();
        if (!uri.startsWith(QLatin1String("https://")))
            continue;
        if (uri.endsWith(QLatin1Char('/')))
            uri.chop(1);
        found.append(QUrl(uri));
        if (found.size() >= limit)
            break;
    }
    return found;
}

QUrl searchUrl(const QUrl &instance, const QString &query)
{
    QUrl url = instance;
    url.setPath(instance.path() + QStringLiteral("/api/v1/search"));
    QUrlQuery items;
    items.addQueryItem(QStringLiteral("q"), QString::fromLatin1(QUrl::toPercentEncoding(query)));
    items.addQueryItem(QStringLiteral("type"), QStringLiteral("video"));
    url.setQuery(items.query(QUrl::FullyEncoded), QUrl::StrictMode);
    return url;
}

QUrl similarUrl(const QUrl &instance, const QString &id)
{
    if (!isVideoId(id))
        return QUrl();
    QUrl url = instance;
    url.setPath(instance.path() + QStringLiteral("/api/v1/videos/") + id);
    url.setQuery(QStringLiteral("fields=recommendedVideos"));
    return url;
}

QList<DjSearchResult> results(const QJsonArray &rows)
{
    QSet<QString> seen;
    QList<DjSearchResult> out;
    for (const QJsonValue &value : rows) {
        const QJsonObject row = value.toObject();
        QString link = row.value(QStringLiteral("url")).toString();
        if (link.startsWith(QLatin1Char('/')))
            link.prepend(QStringLiteral("https://www.youtube.com"));
        const QString raw = row.value(QStringLiteral("videoId")).toString();
        const QString id = videoId(raw.isEmpty() ? link : raw);
        if (id.isEmpty() || seen.contains(id))
            continue;
        seen.insert(id);
        DjSearchResult result;
        result.id = id;
        result.title = row.value(QStringLiteral("title")).toString();
        if (result.title.isEmpty())
            result.title = QStringLiteral("Untitled video");
        result.author = row.value(QStringLiteral("author")).toString();
        result.seconds = row.value(QStringLiteral("lengthSeconds")).toDouble();
        out.append(result);
    }
    return out;
}

QList<DjSearchResult> similarResults(const QByteArray &data)
{
    const QJsonObject object = QJsonDocument::fromJson(data).object();
    return results(object.value(QStringLiteral("recommendedVideos")).toArray());
}

QList<DjSearchResult> searchResults(const QByteArray &data)
{
    return results(QJsonDocument::fromJson(data).array());
}

QJsonObject playedJson(const DjPlayedSong &song)
{
    return {{QStringLiteral("id"), song.id},
            {QStringLiteral("title"), song.title},
            {QStringLiteral("author"), song.author},
            {QStringLiteral("plays"), song.plays},
            {QStringLiteral("last"), song.last}};
}

std::optional<DjPlayedSong> playedFromJson(const QJsonObject &object)
{
    const QString id = object.value(QStringLiteral("id")).toString();
    if (!isVideoId(id))
        return std::nullopt;
    DjPlayedSong song;
    song.id = id;
    song.title = object.value(QStringLiteral("title")).toString().left(300);
    song.author = object.value(QStringLiteral("author")).toString().left(200);
    song.plays = std::clamp(object.value(QStringLiteral("plays")).toInt(1), 0, 1000000);
    const double last = object.value(QStringLiteral("last")).toDouble();
    song.last = std::isfinite(last) ? std::max(0.0, last) : 0;
    return song;
}

QJsonObject historyJson(const DjHistoryItem &item)
{
    return {{QStringLiteral("id"), item.id},
            {QStringLiteral("title"), item.title},
            {QStringLiteral("author"), item.author}};
}

std::optional<DjHistoryItem> historyFromJson(const QJsonObject &object)
{
    const QString id = object.value(QStringLiteral("id")).toString();
    if (!isVideoId(id))
        return std::nullopt;
    return DjHistoryItem{id, object.value(QStringLiteral("title")).toString().left(300),
                         object.value(QStringLiteral("author")).toString().left(200)};
}

}

std::optional<double> DjTapTempo::tap(double time)
{
    if (!m_taps.isEmpty() && (time - m_taps.last() > 2 || time <= m_taps.last()))
        m_taps.clear();
    m_taps.append(time);
    if (m_taps.size() > 8)
        m_taps.remove(0, m_taps.size() - 8);
    if (m_taps.size() < 3)
        return std::nullopt;
    const double bpm = 60 / ((m_taps.last() - m_taps.first()) / (m_taps.size() - 1));
    if (!std::isfinite(bpm) || bpm < 40 || bpm > 250)
        return std::nullopt;
    return std::round(bpm * 10) / 10;
}

void DjSpin::hold(double target, double now)
{
    if (!std::isfinite(target))
        return;
    if (std::abs(target - m_position) <= jump)
        m_turn += target - m_position;
    m_position = target;
    m_speed = 0;
    m_motor = 0;
    if (std::isfinite(now))
        m_stamp = now;
}

double DjSpin::step(double target, double goalSpeed, std::optional<double> motorDrive, double now)
{
    if (!std::isfinite(target))
        return m_position;
    const double goal = std::isfinite(goalSpeed) ? goalSpeed : 0;
    const double drive = motorDrive ? (std::isfinite(*motorDrive) ? *motorDrive : 0) : goal;
    if (!std::isfinite(now) || !m_stamp || std::abs(target - m_position) > jump) {
        m_position = target;
        m_speed = goal;
        m_motor = drive;
        if (std::isfinite(now))
            m_stamp = now;
        return m_position;
    }
    const double last = *m_stamp;
    if (now < last)
        return m_position - m_speed * std::min(last - now, gap);
    m_stamp = now;
    const double elapsed = now - last;
    if (elapsed > gap) {
        if (elapsed <= hitch)
            m_turn += m_motor * elapsed;
        m_position = target;
        return m_position;
    }
    if (elapsed <= 0)
        return m_position;
    m_speed += (goal - m_speed) * (1 - std::exp(-elapsed / (goal > m_speed ? spinUp : spinDown)));
    m_motor += (drive - m_motor) * (1 - std::exp(-elapsed / (drive > m_motor ? spinUp : spinDown)));
    m_turn += m_motor * elapsed;
    m_position += m_speed * elapsed;
    m_position += (target - m_position) * (1 - std::exp(-elapsed / follow));
    return m_position;
}

std::optional<double> DjAutomixDeck::remaining() const
{
    if (duration <= 0 || !std::isfinite(position))
        return std::nullopt;
    return std::max(0.0, duration - position) / std::max(0.25, rate);
}

bool DjAutomixDeck::fresh() const
{
    return loaded && !playing && (duration <= 0 || position < duration - studio::automixSeconds - 2);
}

namespace automix {

DjAutomixStep step(const DjAutomixDeck &a, const DjAutomixDeck &b, DjSide favoured, bool queued)
{
    const auto deck = [&](DjSide side) -> const DjAutomixDeck & { return side == DjSide::A ? a : b; };
    QList<DjSide> playing;
    if (a.playing)
        playing.append(DjSide::A);
    if (b.playing)
        playing.append(DjSide::B);
    if (playing.isEmpty()) {
        for (DjSide side : {favoured, otherSide(favoured)}) {
            if (deck(side).fresh())
                return {DjAutomixStep::Kind::Start, side};
        }
        return queued ? DjAutomixStep{DjAutomixStep::Kind::Preload, favoured} : DjAutomixStep{};
    }
    const DjSide live = playing.size() > 1 ? favoured : playing.first();
    const DjSide other = otherSide(live);
    const DjAutomixDeck &incoming = deck(other);
    const DjAutomixDeck &outgoing = deck(live);
    if (incoming.playing || outgoing.repeats)
        return {};
    if (!incoming.fresh())
        return queued ? DjAutomixStep{DjAutomixStep::Kind::Preload, other} : DjAutomixStep{};
    const auto remaining = outgoing.remaining();
    if (!remaining || *remaining > studio::automixSeconds + lead)
        return {};
    DjAutomixStep mix{DjAutomixStep::Kind::Mix, live, other};
    mix.seconds = std::max(2.0, std::min(studio::automixSeconds, *remaining));
    return mix;
}

DjSide playNow(const DjAutomixDeck &a, const DjAutomixDeck &b, double crossfader, bool *none)
{
    if (none)
        *none = a.playing || b.playing;
    const DjSide favoured = crossfader <= 0 ? DjSide::A : DjSide::B;
    const DjAutomixDeck &f = favoured == DjSide::A ? a : b;
    const DjAutomixDeck &o = favoured == DjSide::A ? b : a;
    const bool open = f.loaded && !o.loaded;
    return std::abs(crossfader) < 0.5 && open ? otherSide(favoured) : favoured;
}

std::optional<DjPlayedSong> next(const QList<DjPlayedSong> &library, const QSet<QString> &loaded,
                                 const QSet<QString> &playing, quint64 seed)
{
    QList<QPair<int, DjPlayedSong>> pool;
    for (int i = 0; i < library.size(); ++i) {
        const DjPlayedSong &song = library.at(i);
        if (!loaded.contains(song.id) && !playing.contains(song.id))
            pool.append({i, song});
    }
    if (pool.isEmpty()) {
        for (int i = 0; i < library.size(); ++i) {
            if (!playing.contains(library.at(i).id))
                pool.append({i, library.at(i)});
        }
    }
    if (pool.isEmpty())
        return std::nullopt;
    std::sort(pool.begin(), pool.end(), [](const auto &l, const auto &r) {
        if (l.second.last != r.second.last)
            return l.second.last < r.second.last;
        return l.first > r.first;
    });
    const int choices = std::max(1, int((pool.size() + 1) / 2));
    DjRandom random(seed);
    return pool.at(int(random.next() % quint64(choices))).second;
}

}

namespace end {

DjEndReport judge(double position, double duration, std::optional<double> doubtedSince,
                  bool intended, double now)
{
    if (!(duration > 0) || !std::isfinite(position) || position >= duration - slack)
        return DjEndReport::Ended;
    if (!intended || !doubtedSince || now - *doubtedSince < patience)
        return DjEndReport::Doubted;
    return DjEndReport::Failed;
}

}

namespace repeat {

std::optional<DjRepeatSpan> span(const QVector<DjWaveBar> &bars, double duration)
{
    if (!std::isfinite(duration) || duration <= tail + shortest)
        return std::nullopt;
    const DjRepeatSpan whole{0, duration - tail};
    int first = -1;
    int last = -1;
    for (int i = 0; i < bars.size(); ++i) {
        if (bars.at(i).level > quiet) {
            if (first < 0)
                first = i;
            last = i;
        }
    }
    if (first < 0)
        return whole;
    const double step = duration / bars.size();
    const double start = first * step;
    const double stop = std::min(whole.end, (last + 1) * step);
    if (stop - start < shortest)
        return whole;
    return DjRepeatSpan{start, stop};
}

DjRepeatStep step(double position, const DjRepeatSpan &span, double rate)
{
    if (!std::isfinite(position) || !std::isfinite(rate) || rate <= 0.01)
        return {};
    const double left = (span.end - position) / rate;
    if (left <= 0)
        return {DjRepeatStep::Kind::Wrap, 0};
    if (left < ahead)
        return {DjRepeatStep::Kind::After, left};
    return {};
}

}

double DjReaction::seconds() const
{
    switch (style) {
    case DjReactionStyle::Zoom:
    case DjReactionStyle::Flash:
        return 1.6;
    case DjReactionStyle::Spin:
    case DjReactionStyle::Orbit:
    case DjReactionStyle::Burst:
        return 2.2;
    default:
        return 3;
    }
}

const QList<DjReaction> &DjReaction::all()
{
    using S = DjReactionStyle;
    static const QList<DjReaction> list{
        {QStringLiteral("hearts"), QStringLiteral("❤️"), QStringLiteral("Hearts"), S::Rise, 0.98},
        {QStringLiteral("fire"), QStringLiteral("🔥"), QStringLiteral("Fire"), S::Rise, 0.06},
        {QStringLiteral("clap"), QStringLiteral("👏"), QStringLiteral("Applause"), S::Burst, 0.12},
        {QStringLiteral("confetti"), QStringLiteral("🎉"), QStringLiteral("Confetti"), S::Confetti, 0.9},
        {QStringLiteral("fireworks"), QStringLiteral("🎆"), QStringLiteral("Fireworks"), S::Fireworks, 0.62},
        {QStringLiteral("hundred"), QStringLiteral("💯"), QStringLiteral("100"), S::Zoom, 0.0},
        {QStringLiteral("laugh"), QStringLiteral("😂"), QStringLiteral("LOL"), S::Rise, 0.14},
        {QStringLiteral("starstruck"), QStringLiteral("🤩"), QStringLiteral("Star-struck"), S::Burst, 0.15},
        {QStringLiteral("mindblown"), QStringLiteral("🤯"), QStringLiteral("Mind Blown"), S::Zoom, 0.05},
        {QStringLiteral("rocket"), QStringLiteral("🚀"), QStringLiteral("Lift Off"), S::Rise, 0.58},
        {QStringLiteral("sparkles"), QStringLiteral("✨"), QStringLiteral("Sparkles"), S::Rain, 0.15},
        {QStringLiteral("notes"), QStringLiteral("🎵"), QStringLiteral("Music"), S::Orbit, 0.75},
        {QStringLiteral("headphones"), QStringLiteral("🎧"), QStringLiteral("Headphones"), S::Spin, 0.55},
        {QStringLiteral("dancer"), QStringLiteral("💃"), QStringLiteral("Dance"), S::Wave, 0.95},
        {QStringLiteral("groove"), QStringLiteral("🕺"), QStringLiteral("Groove"), S::Wave, 0.8},
        {QStringLiteral("hands"), QStringLiteral("🙌"), QStringLiteral("Hands Up"), S::Burst, 0.1},
        {QStringLiteral("thumbs"), QStringLiteral("👍"), QStringLiteral("Thumbs Up"), S::Zoom, 0.12},
        {QStringLiteral("cool"), QStringLiteral("😎"), QStringLiteral("Cool"), S::Zoom, 0.6},
        {QStringLiteral("lightning"), QStringLiteral("⚡"), QStringLiteral("Lightning"), S::Flash, 0.15},
        {QStringLiteral("disco"), QStringLiteral("🪩"), QStringLiteral("Disco"), S::Spin, 0.7},
        {QStringLiteral("rainbow"), QStringLiteral("🌈"), QStringLiteral("Rainbow"), S::Wave, 0.33},
        {QStringLiteral("balloons"), QStringLiteral("🎈"), QStringLiteral("Balloons"), S::Rise, 0.0},
        {QStringLiteral("snow"), QStringLiteral("❄️"), QStringLiteral("Snow"), S::Rain, 0.55},
        {QStringLiteral("money"), QStringLiteral("💸"), QStringLiteral("Make It Rain"), S::Rain, 0.33},
        {QStringLiteral("tears"), QStringLiteral("🥹"), QStringLiteral("Feels"), S::Rain, 0.58},
        {QStringLiteral("eyes"), QStringLiteral("👀"), QStringLiteral("Eyes"), S::Zoom, 0.1},
        {QStringLiteral("skull"), QStringLiteral("💀"), QStringLiteral("Dead"), S::Burst, 0.0},
        {QStringLiteral("bomb"), QStringLiteral("💣"), QStringLiteral("Drop"), S::Flash, 0.03},
        {QStringLiteral("rock"), QStringLiteral("🤘"), QStringLiteral("Rock On"), S::Burst, 0.85},
        {QStringLiteral("loveeyes"), QStringLiteral("😍"), QStringLiteral("Love It"), S::Orbit, 0.95},
    };
    return list;
}

const DjReaction *DjReaction::find(const QString &id)
{
    for (const DjReaction &reaction : all()) {
        if (reaction.id == id)
            return &reaction;
    }
    return nullptr;
}

QList<DjDrumPad> drumPads()
{
    return {DjDrumPad::Kick, DjDrumPad::Snare, DjDrumPad::Hat, DjDrumPad::Clap,
            DjDrumPad::Tom, DjDrumPad::Perc, DjDrumPad::Riser, DjDrumPad::Stab};
}

QString drumPadKey(DjDrumPad pad)
{
    static const QStringList keys{QStringLiteral("kick"), QStringLiteral("snare"), QStringLiteral("hat"),
                                  QStringLiteral("clap"), QStringLiteral("tom"), QStringLiteral("perc"),
                                  QStringLiteral("riser"), QStringLiteral("stab")};
    return keys.at(int(pad));
}

QString drumPadLabel(DjDrumPad pad)
{
    QString key = drumPadKey(pad);
    key[0] = key.at(0).toUpper();
    return key;
}

const QList<DjSoundEffect> &soundEffects()
{
    static const QList<DjSoundEffect> list{
        {QStringLiteral("airhorn"), QStringLiteral("Air Horn"), 1.2},
        {QStringLiteral("siren"), QStringLiteral("Siren"), 2.4},
        {QStringLiteral("explosion"), QStringLiteral("Explosion"), 2.2},
        {QStringLiteral("applause"), QStringLiteral("Applause"), 3.5},
        {QStringLiteral("laugh"), QStringLiteral("Laugh"), 1.6},
        {QStringLiteral("rewind"), QStringLiteral("Rewind"), 1.4},
        {QStringLiteral("laser"), QStringLiteral("Laser"), 0.8},
        {QStringLiteral("scratch"), QStringLiteral("Scratch"), 0.6},
    };
    return list;
}

}
