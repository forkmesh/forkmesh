#pragma once

#include "NativePackageExtension.h"
#include "RssReaderModel.h"

#include <QHash>
#include <QIcon>
#include <QImage>
#include <QPixmap>
#include <QPointer>
#include <QSet>
#include <QString>

#include <functional>
#include <memory>

class QDialog;
class QKeyEvent;
class QLabel;
class QLineEdit;
class QListView;
class QMenu;
class QSplitter;
class QStackedWidget;
class QTimer;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace forkmesh::rssreader {

class ArticleBrowser;
class FetchQueue;
struct FetchResult;
struct IconSearch;
class SourceDelegate;
class TimelineDelegate;
class TimelineModel;

struct ArticleColors {
    QString background;
    QString text;
    QString muted;
    QString link;
};

class RssReaderFeature final : public forkmesh::market::NativePackageWidget
{
public:
    explicit RssReaderFeature(const forkmesh::market::NativePackageHost &host,
                              QWidget *parent = nullptr);
    ~RssReaderFeature() override;

    void refresh() override;
    void setHostSearchQuery(const QString &query) override;
    QList<forkmesh::market::NativePackageSearchHit> searchHits(const QString &query,
                                                               int limit) const override;
    void activateSearchHit(const QString &id) override;
    void prepareSearch() override;
    bool exportData(forkmesh::market::NativePackageReplyHandler done) override;
    int navigationBadge() const override;
    void prepareNavigationBadge() override;

    const Store &store() const { return m_store; }
    QString dataDirectory() const;
    bool refreshing() const { return !m_inFlight.isEmpty(); }
    QString statusText() const;
    void checkFeeds(bool force);
    void subscribe(const QString &address, const QString &title, const QString &folder,
                   std::function<void(bool, const QString &)> done);
    void selectSource(const Source &source);
    void selectArticle(const ArticleRef &ref);
    Source currentSource() const { return m_source; }
    ArticleRef currentArticle() const { return m_article; }
    QString renderedArticleHtml() const { return m_renderedHtml; }
    int timelineRows() const;
    void nextUnread();
    void toggleRead();
    void toggleStar();
    void toggleReadLater();
    void markAllRead(bool ask);
    void catchUp(int days, bool everywhere, bool ask);
    void checkNow(const QString &feedId, std::function<void(const QString &)> done);
    QString changeFeedAddress(const QString &feedId, const QString &address);
    void setFeedFlags(const QString &feedId, bool paused, bool excludeFromToday, bool excludeFromUnread);
    void importOpmlFromUrl(const QString &address, std::function<void(bool, const QString &)> done);
    void setFeedSearch(const QString &text);
    QStringList visibleSourceIds() const;
    void updateSettings(const std::function<void(Settings &)> &change, bool timeline, bool article);
    void showReaderView();
    void saveNow();
    QList<forkmesh::market::NativePackageChatTarget> shareTargets() const;
    QString shareArticle(const ArticleRef &ref, const QString &target);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    QWidget *buildSourcesPane();
    QWidget *buildTimelinePane();
    QWidget *buildArticlePane();
    QToolButton *toolButton(const QString &icon, const QString &tooltip, bool checkable = false);
    void refreshIcons();
    void loadStore();
    QString dataPrefix() const;
    void persistExportKeys();
    void scheduleSave();
    void fetchFeed(const QString &feedId, bool unconditional = false,
                   std::function<void(const QString &)> done = {});
    QString feedFetched(const QString &feedId, const FetchResult &result);
    void finishRefresh();
    void updateStatus();
    void updateCounts();
    void rebuildSources();
    void rebuildTimeline();
    void updateTimelineHeader();
    void showArticle();
    void updateArticleToolbar();
    QString composeArticleHtml(const Article &article) const;
    QString prepareBody(const QString &html) const;
    ArticleColors articleColors() const;
    void applyArticleStyle();
    void markSelectedRead(const ArticleRef &ref);
    void moveSelection(int delta);
    void openInBrowser();
    void copyLink();
    void showShareDialog(const ArticleRef &ref);
    bool handleKey(QKeyEvent *event);
    void sourceContextMenu(const QPoint &position);
    void timelineContextMenu(const QPoint &position);
    void showAddFeed();
    void showSmartFeedEditor(const QString &id);
    void deleteSmartFeed(const QString &id);
    bool showFilterEditor(QWidget *parent, const QString &id);
    void filtersEdited();
    void addCatchUpMenu(QMenu *menu, bool everywhere);
    void showSourcesMenu();
    void importOpmlFromUrlDialog(QWidget *parent);
    QString importSummary(const OpmlImportResult &result) const;
    bool readHidden(const Source &source) const;
    QPixmap thumbnail(const QString &url);
    void fetchThumbnail(const QString &url);
    void showFeedInfo(const QString &feedId);
    void showSettings();
    void showLog(const QString &title, const QString &text);
    void unsubscribe(const QString &feedId);
    void renameFolder(const QString &name);
    void deleteFolder(const QString &name);
    void newFolder(QWidget *parent);
    void importOpml(QWidget *parent);
    void exportOpml(QWidget *parent);
    void addBlogFeed(QWidget *parent);
    void settingsChanged(bool timeline, bool article);
    QString sourceTitle(const Source &source) const;
    QStringList feedOrder() const;
    QPixmap feedIcon(const QString &feedId, int side) const;
    QPixmap letterTile(const QString &name, int side) const;
    void fetchMissingIcons();
    void fetchIcon(const std::shared_ptr<IconSearch> &search);
    void fetchIconPage(const std::shared_ptr<IconSearch> &search);
    void storeIcon(const QString &feedId, const QImage &image);
    QVariant articleResource(int type, const QUrl &url);
    void fetchImage(const QString &url);
    QString keptKey(const ArticleRef &ref) const;
    void announceChanges();

    forkmesh::market::NativePackageHost m_host;
    Store m_store;
    std::unique_ptr<DiskStore> m_disk;
    std::unique_ptr<FetchQueue> m_feedQueue;
    std::unique_ptr<FetchQueue> m_assetQueue;
    bool m_loaded = false;
    Source m_source{SourceKind::AllUnread, QString()};
    ArticleRef m_article;
    QString m_search;
    QString m_feedSearch;
    QSet<QString> m_keptRead;
    QSet<QString> m_inFlight;
    int m_refreshTotal = 0;
    int m_badgeHint = 0;
    int m_refreshNew = 0;
    int m_refreshFailed = 0;
    qint64 m_lastRefreshMs = 0;
    QHash<QString, int> m_counts;
    QSet<QString> m_collapsedSections;
    mutable QHash<QString, QPixmap> m_icons;
    mutable QSet<QString> m_iconMisses;
    mutable QHash<QString, QPixmap> m_tiles;
    mutable QHash<QString, QPixmap> m_scaledIcons;
    QHash<QString, QIcon> m_iconCache;
    QSet<QString> m_iconAttempts;
    QHash<QString, QImage> m_images;
    QSet<QString> m_imagePending;
    QHash<QString, QPixmap> m_thumbs;
    QSet<QString> m_thumbPending;
    QSet<QString> m_thumbFailed;
    QSet<QString> m_readerPending;
    QString m_readerError;
    bool m_readerMode = false;
    QString m_renderedHtml;
    QString m_shownKey;
    bool m_rebuilding = false;

    QSplitter *m_splitter = nullptr;
    QLabel *m_status = nullptr;
    QToolButton *m_refreshButton = nullptr;
    QToolButton *m_unreadFeedsButton = nullptr;
    QToolButton *m_settingsButton = nullptr;
    QToolButton *m_addButton = nullptr;
    QToolButton *m_sourcesMenuButton = nullptr;
    QLineEdit *m_feedSearchField = nullptr;
    QTreeWidget *m_sources = nullptr;
    SourceDelegate *m_sourceDelegate = nullptr;
    QLabel *m_timelineTitle = nullptr;
    QLabel *m_timelineCount = nullptr;
    QLineEdit *m_searchField = nullptr;
    QToolButton *m_markAllButton = nullptr;
    QToolButton *m_unreadTimelineButton = nullptr;
    QToolButton *m_catchUpButton = nullptr;
    QListView *m_timeline = nullptr;
    TimelineModel *m_timelineModel = nullptr;
    TimelineDelegate *m_timelineDelegate = nullptr;
    QStackedWidget *m_articleStack = nullptr;
    QLabel *m_articleEmpty = nullptr;
    ArticleBrowser *m_browser = nullptr;
    QToolButton *m_readButton = nullptr;
    QToolButton *m_starButton = nullptr;
    QToolButton *m_readLaterButton = nullptr;
    QToolButton *m_nextUnreadButton = nullptr;
    QToolButton *m_readerButton = nullptr;
    QToolButton *m_openButton = nullptr;
    QToolButton *m_copyButton = nullptr;
    QToolButton *m_shareButton = nullptr;
    QTimer *m_saveTimer = nullptr;
    QTimer *m_renderTimer = nullptr;
    QTimer *m_countsTimer = nullptr;
    QTimer *m_searchTimer = nullptr;
    QPointer<QDialog> m_settingsDialog;
};

}
