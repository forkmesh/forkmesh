package com.forkmesh.nativeapp.rssreader

import com.forkmesh.nativeapp.json.JsonMap
import com.forkmesh.nativeapp.json.bool
import com.forkmesh.nativeapp.json.int
import com.forkmesh.nativeapp.json.long
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.optBool
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.json.strings

data class RssFolder(val id: String, val name: String) {
    fun toJson(): Map<String, Any?> = linkedMapOf("id" to id, "name" to name)

    companion object {
        fun fromJson(json: JsonMap) = RssFolder(json.str("id"), json.str("name"))
    }
}

data class RssFeed(
    val id: String,
    val url: String,
    val title: String = "",
    val customTitle: String = "",
    val folderId: String = "",
    val siteUrl: String = "",
    val iconUrl: String = "",
    val description: String = "",
    val etag: String = "",
    val lastModified: String = "",
    val previousUrls: List<String> = emptyList(),
    val addedAt: Long = 0,
    val lastCheck: Long = 0,
    val lastSuccess: Long = 0,
    val nextCheck: Long = 0,
    val errorCount: Int = 0,
    val error: String = "",
    val errorAt: Long = 0,
    val viaHttps: Boolean = false,
) {
    val displayTitle: String get() = customTitle.ifEmpty { title }.ifEmpty { RssUrls.host(url).ifEmpty { url } }

    val homePage: String get() = siteUrl.takeIf { it.isNotEmpty() && it != url }.orEmpty()

    fun toJson(): Map<String, Any?> = linkedMapOf(
        "id" to id, "url" to url, "title" to title, "customTitle" to customTitle, "folderId" to folderId,
        "siteUrl" to siteUrl, "iconUrl" to iconUrl, "description" to description, "etag" to etag,
        "lastModified" to lastModified, "previousUrls" to previousUrls, "addedAt" to addedAt,
        "lastCheck" to lastCheck, "lastSuccess" to lastSuccess, "nextCheck" to nextCheck,
        "errorCount" to errorCount, "error" to error, "errorAt" to errorAt, "viaHttps" to viaHttps,
    )

    companion object {
        fun fromJson(json: JsonMap) = RssFeed(
            id = json.str("id"), url = json.str("url"), title = json.str("title"), customTitle = json.str("customTitle"),
            folderId = json.str("folderId"), siteUrl = json.str("siteUrl"), iconUrl = json.str("iconUrl"),
            description = json.str("description"), etag = json.str("etag"), lastModified = json.str("lastModified"),
            previousUrls = json.strings("previousUrls"), addedAt = json.long("addedAt"), lastCheck = json.long("lastCheck"),
            lastSuccess = json.long("lastSuccess"), nextCheck = json.long("nextCheck"), errorCount = json.int("errorCount"),
            error = json.str("error"), errorAt = json.long("errorAt"), viaHttps = json.bool("viaHttps"),
        )
    }
}

data class RssArticle(
    val id: String,
    val feedId: String,
    val guid: String,
    val title: String = "",
    val url: String = "",
    val externalUrl: String = "",
    val author: String = "",
    val summary: String = "",
    val contentHtml: String = "",
    val commentsUrl: String = "",
    val enclosures: List<RssEnclosure> = emptyList(),
    val image: String = "",
    val published: Long = 0,
    val updated: Long = 0,
    val arrived: Long = 0,
    val read: Boolean = false,
    val starred: Boolean = false,
    val readerHtml: String = "",
    val contentKey: String = "",
) {
    val link: String get() = url.ifEmpty { externalUrl }

    fun toJson(): Map<String, Any?> = linkedMapOf(
        "id" to id, "feedId" to feedId, "guid" to guid, "title" to title, "url" to url, "externalUrl" to externalUrl,
        "author" to author, "summary" to summary, "contentHtml" to contentHtml, "commentsUrl" to commentsUrl,
        "enclosures" to enclosures.map { linkedMapOf("url" to it.url, "type" to it.type, "length" to it.length) },
        "image" to image, "published" to published, "updated" to updated, "arrived" to arrived, "read" to read,
        "starred" to starred, "readerHtml" to readerHtml, "contentKey" to contentKey,
    )

    companion object {
        fun fromJson(json: JsonMap) = RssArticle(
            id = json.str("id"), feedId = json.str("feedId"), guid = json.str("guid"), title = json.str("title"),
            url = json.str("url"), externalUrl = json.str("externalUrl"), author = json.str("author"),
            summary = json.str("summary"), contentHtml = json.str("contentHtml"), commentsUrl = json.str("commentsUrl"),
            enclosures = json.maps("enclosures").map { RssEnclosure(it.str("url"), it.str("type"), it.long("length")) },
            image = json.str("image"), published = json.long("published"), updated = json.long("updated"),
            arrived = json.long("arrived"), read = json.bool("read"), starred = json.bool("starred"),
            readerHtml = json.str("readerHtml"), contentKey = json.str("contentKey"),
        )
    }
}

enum class RssTheme(val wire: String, val label: String) {
    DEFAULT("default", "Default"), SEPIA("sepia", "Sepia"), PAPER("paper", "Paper"), CONTRAST("contrast", "High contrast");

    companion object {
        fun of(wire: String?) = entries.firstOrNull { it.wire == wire } ?: DEFAULT
    }
}

enum class RssFont(val wire: String, val label: String) {
    SANS("sans", "Sans serif"), SERIF("serif", "Serif"), MONO("mono", "Monospace");

    companion object {
        fun of(wire: String?) = entries.firstOrNull { it.wire == wire } ?: SANS
    }
}

enum class RssAppearance(val wire: String, val label: String) {
    AUTOMATIC("auto", "Automatic"), LIGHT("light", "Light"), DARK("dark", "Dark");

    companion object {
        fun of(wire: String?) = entries.firstOrNull { it.wire == wire } ?: AUTOMATIC
    }
}

data class RssSettings(
    val sortOldestFirst: Boolean = false,
    val groupByFeed: Boolean = false,
    val refreshClearsRead: Boolean = false,
    val confirmMarkAllRead: Boolean = true,
    val timelineLines: Int = 2,
    val showFeedIcons: Boolean = false,
    val feedsUnreadOnly: Boolean = false,
    val theme: RssTheme = RssTheme.DEFAULT,
    val font: RssFont = RssFont.SANS,
    val textSize: Int = 17,
    val loadRemoteImages: Boolean = true,
    val appearance: RssAppearance = RssAppearance.AUTOMATIC,
    val collapsed: Set<String> = emptySet(),
    val blogFeedOffered: Boolean = false,
) {
    fun toJson(): Map<String, Any?> = linkedMapOf(
        "sortOldestFirst" to sortOldestFirst, "groupByFeed" to groupByFeed, "refreshClearsRead" to refreshClearsRead,
        "confirmMarkAllRead" to confirmMarkAllRead, "timelineLines" to timelineLines, "showFeedIcons" to showFeedIcons,
        "feedsUnreadOnly" to feedsUnreadOnly, "articleTheme" to theme.wire, "articleFont" to font.wire,
        "articleSize" to textSize, "loadRemoteImages" to loadRemoteImages, "appearance" to appearance.wire,
        "collapsed" to collapsed.sorted(), "blogFeedOffered" to blogFeedOffered,
    )

    companion object {
        const val MIN_TEXT = 12
        const val MAX_TEXT = 28

        fun fromJson(json: JsonMap) = RssSettings(
            sortOldestFirst = json.bool("sortOldestFirst"),
            groupByFeed = json.bool("groupByFeed"),
            refreshClearsRead = json.bool("refreshClearsRead"),
            confirmMarkAllRead = json.optBool("confirmMarkAllRead") ?: true,
            timelineLines = json.int("timelineLines", 2).coerceIn(0, 4),
            showFeedIcons = json.optBool("showFeedIcons") ?: false,
            feedsUnreadOnly = json.bool("feedsUnreadOnly"),
            theme = RssTheme.of(json.str("articleTheme")),
            font = RssFont.of(json.str("articleFont")),
            textSize = json.int("articleSize", 17).coerceIn(MIN_TEXT, MAX_TEXT),
            loadRemoteImages = json.optBool("loadRemoteImages") ?: true,
            appearance = RssAppearance.of(json.str("appearance")),
            collapsed = json.strings("collapsed").toSet(),
            blogFeedOffered = json.bool("blogFeedOffered"),
        )
    }
}

data class RssActivity(val at: Long, val kind: String, val text: String) {
    fun toJson(): Map<String, Any?> = linkedMapOf("at" to at, "kind" to kind, "text" to text)

    companion object {
        fun fromJson(json: JsonMap) = RssActivity(json.long("at"), json.str("kind"), json.str("text"))
    }
}

sealed class RssScope {
    data object Today : RssScope()
    data object Unread : RssScope()
    data object Starred : RssScope()
    data class Feed(val id: String) : RssScope()
    data class Folder(val id: String) : RssScope()

    val key: String
        get() = when (this) {
            Today -> "today"
            Unread -> "unread"
            Starred -> "starred"
            is Feed -> "feed:$id"
            is Folder -> "folder:$id"
        }

    companion object {
        fun of(key: String): RssScope? = when {
            key == "today" -> Today
            key == "unread" -> Unread
            key == "starred" -> Starred
            key.startsWith("feed:") -> Feed(key.removePrefix("feed:"))
            key.startsWith("folder:") -> Folder(key.removePrefix("folder:"))
            else -> null
        }
    }
}

data class RssCounts(
    val today: Int = 0,
    val unread: Int = 0,
    val starred: Int = 0,
    val byFeed: Map<String, Int> = emptyMap(),
    val byFolder: Map<String, Int> = emptyMap(),
) {
    fun of(scope: RssScope): Int = when (scope) {
        RssScope.Today -> today
        RssScope.Unread -> unread
        RssScope.Starred -> starred
        is RssScope.Feed -> byFeed[scope.id] ?: 0
        is RssScope.Folder -> byFolder[scope.id] ?: 0
    }
}

data class RssStats(
    val feeds: Int,
    val folders: Int,
    val articles: Int,
    val unread: Int,
    val starred: Int,
    val failingFeeds: Int,
    val oldestArticle: Long,
    val lastRefresh: Long,
)

sealed class RssSidebarEntry {
    abstract val name: String

    data class FolderEntry(val folder: RssFolder, val feeds: List<RssFeed>) : RssSidebarEntry() {
        override val name: String get() = folder.name
    }

    data class FeedEntry(val feed: RssFeed) : RssSidebarEntry() {
        override val name: String get() = feed.displayTitle
    }
}

data class RssLibraryData(
    val folders: List<RssFolder> = emptyList(),
    val feeds: List<RssFeed> = emptyList(),
    val settings: RssSettings = RssSettings(),
    val activity: List<RssActivity> = emptyList(),
    val lastRefresh: Long = 0,
) {
    fun toJson(): Map<String, Any?> = linkedMapOf(
        "version" to 1,
        "folders" to folders.map { it.toJson() },
        "feeds" to feeds.map { it.toJson() },
        "settings" to settings.toJson(),
        "activity" to activity.map { it.toJson() },
        "lastRefresh" to lastRefresh,
    )

    companion object {
        fun fromJson(json: JsonMap) = RssLibraryData(
            folders = json.maps("folders").map(RssFolder::fromJson).filter { it.id.isNotEmpty() },
            feeds = json.maps("feeds").map(RssFeed::fromJson).filter { it.id.isNotEmpty() && it.url.isNotEmpty() },
            settings = RssSettings.fromJson(json.obj("settings")),
            activity = json.maps("activity").map(RssActivity::fromJson),
            lastRefresh = json.long("lastRefresh"),
        )
    }
}
