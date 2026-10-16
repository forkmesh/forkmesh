#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrl>

namespace forkmesh::rssreader {

constexpr int kMaxFeeds = 500;
constexpr int kMaxFolders = 100;
constexpr int kMaxArticlesPerFeed = 400;
constexpr int kMaxItemsPerFetch = 200;
constexpr qint64 kMaxFetchBytes = qint64(2) * 1024 * 1024;
constexpr qint64 kCheckIntervalMs = qint64(30) * 60 * 1000;
constexpr qint64 kMaxBackoffMs = qint64(24) * 60 * 60 * 1000;
constexpr qint64 kFutureSlackMs = qint64(24) * 60 * 60 * 1000;
constexpr qint64 kTodayWindowMs = qint64(24) * 60 * 60 * 1000;
constexpr int kMaxGuidLength = 600;
constexpr int kMaxConcurrentFetches = 6;
constexpr int kMaxDiscoveryLinks = 3;
constexpr int kMaxLogEntries = 200;
constexpr int kMaxStarredHtml = 8 * 1024;
constexpr int kMaxStarredExport = 2 * 1024 * 1024;
constexpr int kMaxFilters = 100;
constexpr int kMaxSmartFeeds = 50;
constexpr int kMaxFilterText = 20000;
constexpr int kWordsPerMinute = 230;
constexpr int kCjkPerMinute = 500;
constexpr qint64 kDayMs = qint64(24) * 60 * 60 * 1000;

QString forkMeshBlogFeed();

struct Enclosure {
    QString url;
    QString type;
    qint64 length = 0;
};

struct ParsedItem {
    QString id;
    QString link;
    QString title;
    QString author;
    QString html;
    QString comments;
    QList<Enclosure> enclosures;
    qint64 publishedMs = -1;
    QString language;
    QString jsonImage;
    QString bannerImage;
    QStringList thumbnails;
    QStringList mediaImages;
    QStringList enclosureImages;
};

struct ParsedFeed {
    bool ok = false;
    QString error;
    QString format;
    QString title;
    QString homePage;
    QString iconUrl;
    QString language;
    QList<ParsedItem> items;
};

struct Article {
    QString feedId;
    QString guid;
    QString title;
    QString link;
    QString author;
    QString html;
    QString summary;
    QString comments;
    QString readerHtml;
    QList<Enclosure> enclosures;
    qint64 publishedMs = 0;
    qint64 firstSeenMs = 0;
    qint64 updatedMs = 0;
    bool datedByFeed = false;
    bool read = false;
    bool starred = false;
    bool readLater = false;
    QString image;
    QStringList images;
    QString language;
    mutable quint64 filterGeneration = 0;
    mutable bool filterHidden = false;
    mutable bool textCached = false;
    mutable QString textCache;
};

struct Feed {
    QString id;
    QString url;
    QString title;
    QString customTitle;
    QString homePage;
    QString iconUrl;
    QString folder;
    QString etag;
    QString lastModified;
    QString lastError;
    qint64 addedMs = 0;
    qint64 lastCheckedMs = 0;
    qint64 lastSuccessMs = 0;
    qint64 nextDueMs = 0;
    int failures = 0;
    bool paused = false;
    bool excludeFromToday = false;
    bool excludeFromUnread = false;
    QString language;
    QStringList previousUrls;
    QString displayName() const;
};

struct Folder {
    QString name;
    bool collapsed = false;
};

struct ContentFilter {
    QString id;
    QString feedId;
    QString field = QStringLiteral("title");
    QString pattern;
    bool regex = false;
    QString action = QStringLiteral("hide");
    QJsonObject toJson() const;
    static ContentFilter fromJson(const QJsonObject &object);
    bool operator==(const ContentFilter &other) const;
    bool operator!=(const ContentFilter &other) const { return !(*this == other); }
};

struct SmartFeed {
    QString id;
    QString name;
    QString words;
    bool regex = false;
    QString scope;
    bool unreadOnly = false;
    bool starredOnly = false;
    int maxAgeDays = 0;
    QJsonObject toJson() const;
    static SmartFeed fromJson(const QJsonObject &object);
    bool operator==(const SmartFeed &other) const;
    bool operator!=(const SmartFeed &other) const { return !(*this == other); }
};

class TextMatcher
{
public:
    TextMatcher() = default;
    TextMatcher(const QString &pattern, bool regex);
    bool isValid() const { return m_valid; }
    bool isEmpty() const { return m_pattern.isEmpty(); }
    bool matches(const QString &text) const;

private:
    QString m_pattern;
    bool m_regex = false;
    bool m_valid = false;
    QRegularExpression m_expression;
};

struct Settings {
    bool oldestFirst = false;
    bool groupByFeed = false;
    bool refreshClearsRead = false;
    bool confirmMarkAllRead = true;
    int summaryLines = 2;
    bool showFeedIcons = true;
    QString theme = QStringLiteral("default");
    QString fontFamily;
    int textSize = 16;
    bool loadRemoteImages = false;
    bool unreadOnlyFeeds = false;
    bool unreadOnlyTimeline = false;
    bool showThumbnails = false;
    bool foldDuplicates = false;
    bool markAllKeepsStarred = false;
    bool sortFeedsByUnread = false;
    QList<ContentFilter> filters;
    QList<SmartFeed> smartFeeds;
    QJsonObject toJson() const;
    static Settings fromJson(const QJsonObject &object);
};

struct LogEntry {
    qint64 atMs = 0;
    QString text;
};

enum class SourceKind { None, Today, AllUnread, Starred, Feed, Folder, ReadLater, Smart };

struct Source {
    SourceKind kind = SourceKind::None;
    QString id;
    bool operator==(const Source &other) const { return kind == other.kind && id == other.id; }
    bool operator!=(const Source &other) const { return !(*this == other); }
};

struct ArticleRef {
    QString feedId;
    QString guid;
    bool operator==(const ArticleRef &other) const
    {
        return feedId == other.feedId && guid == other.guid;
    }
    bool isNull() const { return feedId.isEmpty(); }
};

struct OpmlEntry {
    QString title;
    QString xmlUrl;
    QString htmlUrl;
    QString folder;
};

struct OpmlDocument {
    bool ok = false;
    QString error;
    QStringList folders;
    QList<OpmlEntry> feeds;
};

struct OpmlImportResult {
    int added = 0;
    int skipped = 0;
    int folders = 0;
    QStringList addedIds;
};

struct Readable {
    QString title;
    QString html;
};

struct DiscoveryRewrite {
    QUrl feed;
    bool channelLookup = false;
};

struct SourceEntry {
    QString name;
    QString id;
    bool folder = false;
    int unread = 0;
};

struct Stats {
    int feeds = 0;
    int folders = 0;
    int articles = 0;
    int unread = 0;
    int starred = 0;
    int failing = 0;
};

QString escapeHtml(const QString &text);
QString decodeEntities(const QString &text);
QString collapseWhitespace(const QString &text);
QByteArray prepareXml(const QByteArray &data);
qint64 parseDate(const QString &text);
QString makeGuid(const QString &id, const QString &link, const QString &title, qint64 publishedMs);
ParsedFeed parseFeed(const QByteArray &data, const QUrl &base);
bool looksLikeHtml(const QByteArray &data, const QString &contentType);
QList<QUrl> discoverFeedLinks(const QString &html, const QUrl &base);
QList<QUrl> fallbackFeedUrls(const QUrl &address);
QUrl normalizeAddress(const QString &input, bool allowFile = false);
QString feedKey(const QUrl &url);
QString linkKey(const QString &link);
QString articleKey(const ArticleRef &ref);
DiscoveryRewrite discoveryRewrite(const QUrl &address);
bool isYouTubeHost(const QString &host);
QString youtubeChannelId(const QString &html);
QUrl youtubeChannelFeed(const QString &channelId);
QString plainTextToHtml(const QString &text);
QStringList itemImages(const ParsedItem &item);
QString firstImageSource(const QString &html);
QString articleImage(const ParsedItem &item, const QString &sanitizedHtml);
QString leadImagesHtml(const QStringList &images, const QString &html);
QString addAltCaptions(const QString &html);
int readingMinutes(const QString &text);
QString textDirection(const QString &language);
QString filterPatternError(const QString &pattern, bool regex);
bool hideReadApplies(const Source &source);
QList<int> catchUpDays();
QString catchUpLabel(int days);
bool feedMatchesSearch(const Feed &feed, const QString &needle);
void sortSourceEntries(QList<SourceEntry> &entries, bool byUnread);
QString sanitizeHtml(const QString &html, const QUrl &base);
QString plainText(const QString &html, int maxLength = 600);
Readable extractReadable(const QString &html, const QUrl &base);
OpmlDocument parseOpml(const QByteArray &data);
QByteArray writeOpml(const QList<Feed> &feeds, const QList<Folder> &folders, qint64 nowMs);
qint64 backoffMs(int failures);
QString newId();

class Store
{
public:
    const QList<Feed> &feeds() const { return m_feeds; }
    const QList<Folder> &folders() const { return m_folders; }
    const Feed *feed(const QString &id) const;
    const Feed *feedByUrl(const QUrl &url) const;
    const Folder *folder(const QString &name) const;
    QStringList feedIdsIn(const Source &source) const;

    QString addFeed(const QUrl &url, const QString &title, const QString &folder, qint64 nowMs,
                    QString *error);
    bool removeFeed(const QString &id);
    bool renameFeed(const QString &id, const QString &title);
    bool moveFeed(const QString &id, const QString &folder);
    bool addFolder(const QString &name, QString *error);
    bool renameFolder(const QString &from, const QString &to, QString *error);
    bool deleteFolder(const QString &name);
    void setFolderCollapsed(const QString &name, bool collapsed);
    bool setFeedFlags(const QString &id, bool paused, bool excludeFromToday, bool excludeFromUnread);
    bool changeFeedAddress(const QString &id, const QString &input, qint64 nowMs, QString *error);

    const QList<Article> &articles(const QString &feedId) const;
    const Article *article(const ArticleRef &ref) const;
    int merge(const QString &feedId, const ParsedFeed &parsed, qint64 nowMs);
    bool setRead(const ArticleRef &ref, bool read);
    bool setStarred(const ArticleRef &ref, bool starred);
    bool setReadLater(const ArticleRef &ref, bool readLater);
    int readLaterCount() const;
    int markRead(const QList<ArticleRef> &refs);
    void setReaderHtml(const ArticleRef &ref, const QString &html);

    QList<ArticleRef> query(const Source &source, const QString &search, bool unreadOnly,
                            qint64 nowMs, const QSet<QString> &kept = {}) const;
    QList<ArticleRef> olderUnread(const Source &source, qint64 cutoffMs, bool keepStarred,
                                  qint64 nowMs) const;
    QList<ArticleRef> olderUnreadEverywhere(qint64 cutoffMs, bool keepStarred) const;
    bool isHidden(const Article &article) const;
    bool smartMatches(const SmartFeed &smart, const Article &article, qint64 nowMs) const;
    const SmartFeed *smartFeed(const QString &id) const;
    bool saveSmartFeed(const SmartFeed &smart, QString *error);
    bool removeSmartFeed(const QString &id);
    bool saveFilter(const ContentFilter &filter, QString *error);
    bool removeFilter(const QString &id);
    void filtersChanged();
    int unreadCount(const Source &source, qint64 nowMs) const;
    int feedUnread(const QString &feedId) const { return m_unread.value(feedId); }
    int totalUnread() const;

    QStringList dueFeeds(qint64 nowMs, bool force) const;
    void recordSuccess(const QString &feedId, qint64 nowMs, const QString &etag,
                       const QString &lastModified, const QUrl &movedTo);
    void recordFailure(const QString &feedId, const QString &error, qint64 nowMs);
    void updateFeedMeta(const QString &feedId, const QString &homePage);

    void logError(qint64 nowMs, const QString &text);
    void logActivity(qint64 nowMs, const QString &text);
    const QList<LogEntry> &errorLog() const { return m_errorLog; }
    const QList<LogEntry> &activityLog() const { return m_activityLog; }

    Settings &settings() { return m_settings; }
    const Settings &settings() const { return m_settings; }

    QString storeId() const { return m_storeId; }
    void setStoreId(const QString &id) { m_storeId = id; }
    QJsonObject stateJson() const;
    bool loadState(const QJsonObject &state);
    QJsonArray articlesJson(const QString &feedId) const;
    void loadArticles(const QString &feedId, const QJsonArray &array);
    QSet<QString> takeDirtyArticles();
    bool takeStateDirty();
    bool takeExportDirty();
    void markStateDirty() { m_stateDirty = true; }
    void markExportDirty() { m_exportDirty = true; m_stateDirty = true; }

    QJsonArray starredJson() const;
    int restoreStarred(const QJsonArray &array, qint64 nowMs);
    OpmlImportResult importOpml(const OpmlDocument &document, qint64 nowMs, bool allowFile = false);
    QJsonObject exportBundle(qint64 nowMs) const;
    bool hasData() const;
    Stats stats() const;
    void clear();

private:
    struct CompiledFilter {
        ContentFilter filter;
        TextMatcher matcher;
    };
    Feed *mutableFeed(const QString &id);
    const QList<CompiledFilter> &compiledFilters() const;
    bool filterMatches(const CompiledFilter &compiled, const Article &article) const;
    const QString &filterText(const Article &article) const;
    bool textMatches(const TextMatcher &matcher, const Article &article) const;
    bool inScope(const SmartFeed &smart, const QString &feedId) const;
    bool smartMatches(const SmartFeed &smart, const TextMatcher &words, const Article &article,
                      qint64 nowMs) const;
    bool countsTowardUnread(const QString &feedId) const;
    void ensureLinkIndex();
    Article *mutableArticle(const ArticleRef &ref);
    void recount(const QString &feedId);
    void prune(QList<Article> &list);
    bool matches(const Article &article, const QString &needle, const QString &feedName) const;

    QList<Feed> m_feeds;
    QList<Folder> m_folders;
    QHash<QString, QList<Article>> m_articles;
    QHash<QString, int> m_unread;
    QList<LogEntry> m_errorLog;
    QList<LogEntry> m_activityLog;
    Settings m_settings;
    QString m_storeId;
    QSet<QString> m_dirtyArticles;
    bool m_stateDirty = false;
    bool m_exportDirty = false;
    QHash<QString, QSet<QString>> m_linkIndex;
    bool m_linkIndexReady = false;
    mutable QList<ContentFilter> m_compiledSource;
    mutable QList<CompiledFilter> m_compiled;
    mutable quint64 m_filterGeneration = 1;
};

class DiskStore
{
public:
    explicit DiskStore(const QString &directory) : m_directory(directory) {}
    QString directory() const { return m_directory; }
    bool load(Store &store, const QString &expectedStoreId) const;
    bool saveState(const Store &store, QString *error = nullptr) const;
    bool saveArticles(const Store &store, const QString &feedId, QString *error = nullptr) const;
    void removeFeedFiles(const QString &feedId) const;
    QString iconPath(const QString &feedId) const;
    int storedUnread(const QString &expectedStoreId) const;
    qint64 sizeOnDisk() const;

private:
    QString m_directory;
};

}
