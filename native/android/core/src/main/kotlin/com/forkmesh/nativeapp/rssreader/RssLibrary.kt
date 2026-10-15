package com.forkmesh.nativeapp.rssreader

import java.security.MessageDigest
import java.util.UUID

sealed class RssFetchOutcome {
    data class Fetched(
        val url: String,
        val permanent: Boolean,
        val feed: RssParsedFeed,
        val etag: String = "",
        val lastModified: String = "",
        val viaHttps: Boolean = false,
    ) : RssFetchOutcome()

    data class NotModified(val viaHttps: Boolean = false) : RssFetchOutcome()

    data class Failed(val error: String) : RssFetchOutcome()
}

data class RssImportResult(val imported: Int, val skipped: Int, val folders: Int)

class RssLibraryException(val code: String) : Exception(code)

class RssLibrary(data: RssLibraryData = RssLibraryData(), articles: Map<String, List<RssArticle>> = emptyMap()) {
    private val folders = data.folders.toMutableList()
    private val feeds = data.feeds.toMutableList()
    private var settings = data.settings
    private val activity = data.activity.toMutableList()
    private var lastRefresh = data.lastRefresh
    private val articles = HashMap<String, MutableList<RssArticle>>().apply {
        for (feed in data.feeds) put(feed.id, articles[feed.id].orEmpty().toMutableList())
    }

    private val dirtyFeeds = HashSet<String>()
    private val removedFeeds = HashSet<String>()
    private var dirtyMeta = false

    @Volatile
    var version: Long = 0
        private set

    companion object {
        const val MAX_FEEDS = 500
        const val MAX_FOLDERS = 100
        const val MAX_ARTICLES_PER_FEED = 400
        const val MAX_ACTIVITY = 200
        const val FETCH_INTERVAL_MS = 30 * 60 * 1000L
        const val MAX_BACKOFF_MS = 24 * 60 * 60 * 1000L
        const val TODAY_MS = 24 * 60 * 60 * 1000L
        const val FUTURE_SLACK_MS = 24 * 60 * 60 * 1000L
        const val FORKMESH_BLOG_FEED = "https://forkmesh.com/blog/rss.xml"

        fun backoff(errors: Int): Long = minOf(MAX_BACKOFF_MS, FETCH_INTERVAL_MS * (1L shl (maxOf(0, minOf(errors, 10) - 1))))

        fun articleId(feedId: String, guid: String): String {
            val digest = MessageDigest.getInstance("SHA-256").digest(guid.toByteArray(Charsets.UTF_8))
            return feedId + "-" + digest.take(10).joinToString("") { "%02x".format(it.toInt() and 0xff) }
        }

        fun contentKey(title: String, url: String, content: String): String {
            val digest = MessageDigest.getInstance("SHA-256").digest("$title\u0000$url\u0000$content".toByteArray(Charsets.UTF_8))
            return digest.take(12).joinToString("") { "%02x".format(it.toInt() and 0xff) }
        }

        fun newId(): String = UUID.randomUUID().toString().replace("-", "")
    }

    private fun touch(meta: Boolean = true, feedId: String? = null) {
        if (meta) dirtyMeta = true
        if (feedId != null) dirtyFeeds.add(feedId)
        version++
    }

    @Synchronized
    fun snapshotData(): RssLibraryData = RssLibraryData(folders.toList(), feeds.toList(), settings, activity.toList(), lastRefresh)

    @Synchronized
    fun takeChanges(): Triple<RssLibraryData?, Map<String, List<RssArticle>>, Set<String>> {
        val meta = if (dirtyMeta) snapshotData() else null
        val changed = dirtyFeeds.filter { it in articles }.associateWith { articles[it].orEmpty().toList() }
        val removed = removedFeeds.toSet()
        dirtyMeta = false
        dirtyFeeds.clear()
        removedFeeds.clear()
        return Triple(meta, changed, removed)
    }

    @Synchronized
    fun settings(): RssSettings = settings

    @Synchronized
    fun updateSettings(change: (RssSettings) -> RssSettings) {
        val next = change(settings).let { it.copy(timelineLines = it.timelineLines.coerceIn(0, 4), textSize = it.textSize.coerceIn(RssSettings.MIN_TEXT, RssSettings.MAX_TEXT)) }
        if (next != settings) {
            settings = next
            touch()
        }
    }

    @Synchronized
    fun lastRefresh(): Long = lastRefresh

    @Synchronized
    fun folders(): List<RssFolder> = folders.sortedWith(compareBy(String.CASE_INSENSITIVE_ORDER) { it.name })

    @Synchronized
    fun feeds(): List<RssFeed> = feeds.sortedWith(compareBy(String.CASE_INSENSITIVE_ORDER) { it.displayTitle })

    @Synchronized
    fun feed(id: String): RssFeed? = feeds.firstOrNull { it.id == id }

    @Synchronized
    fun folder(id: String): RssFolder? = folders.firstOrNull { it.id == id }

    @Synchronized
    fun article(id: String): RssArticle? {
        val feedId = id.substringBeforeLast('-')
        return articles[feedId]?.firstOrNull { it.id == id }
    }

    @Synchronized
    fun activity(): List<RssActivity> = activity.toList()

    @Synchronized
    fun log(kind: String, text: String, now: Long) {
        activity.add(0, RssActivity(now, kind, text))
        while (activity.size > MAX_ACTIVITY) activity.removeAt(activity.lastIndex)
        touch()
    }

    @Synchronized
    fun findFeed(url: String): RssFeed? {
        val key = RssUrls.identity(url)
        return feeds.firstOrNull { feed -> RssUrls.identity(feed.url) == key || feed.previousUrls.any { RssUrls.identity(it) == key } }
    }

    @Synchronized
    fun addFolder(name: String): RssFolder {
        val clean = RssText.clean(name, 120)
        if (clean.isEmpty()) throw RssLibraryException("folder_name_required")
        folders.firstOrNull { it.name.equals(clean, ignoreCase = true) }?.let { return it }
        if (folders.size >= MAX_FOLDERS) throw RssLibraryException("too_many_folders")
        val folder = RssFolder(newId(), clean)
        folders.add(folder)
        touch()
        return folder
    }

    @Synchronized
    fun renameFolder(id: String, name: String) {
        val clean = RssText.clean(name, 120)
        if (clean.isEmpty()) throw RssLibraryException("folder_name_required")
        val index = folders.indexOfFirst { it.id == id }
        if (index < 0) throw RssLibraryException("folder_not_found")
        if (folders.any { it.id != id && it.name.equals(clean, ignoreCase = true) }) throw RssLibraryException("folder_exists")
        folders[index] = folders[index].copy(name = clean)
        touch()
    }

    @Synchronized
    fun deleteFolder(id: String) {
        if (!folders.removeAll { it.id == id }) return
        for (i in feeds.indices) if (feeds[i].folderId == id) feeds[i] = feeds[i].copy(folderId = "")
        settings = settings.copy(collapsed = settings.collapsed - "folder:$id")
        touch()
    }

    @Synchronized
    fun subscribe(url: String, title: String = "", folderId: String = "", siteUrl: String = "", now: Long): RssFeed {
        val clean = RssUrls.normalizeFeedInput(url)
        if (clean.isEmpty()) throw RssLibraryException("invalid_url")
        if (findFeed(clean) != null) throw RssLibraryException("feed_exists")
        if (feeds.size >= MAX_FEEDS) throw RssLibraryException("too_many_feeds")
        val folder = folderId.takeIf { id -> folders.any { it.id == id } }.orEmpty()
        val feed = RssFeed(
            id = newId(), url = clean, title = RssText.clean(title, 200), folderId = folder, siteUrl = siteUrl,
            iconUrl = RssUrls.favicon(siteUrl.ifEmpty { clean }), addedAt = now, nextCheck = 0,
        )
        feeds.add(feed)
        articles[feed.id] = ArrayList()
        touch(feedId = feed.id)
        return feed
    }

    @Synchronized
    fun addFetched(outcome: RssFetchOutcome.Fetched, title: String = "", folderId: String = "", now: Long): RssFeed {
        if (findFeed(outcome.url) != null) throw RssLibraryException("feed_exists")
        val feed = subscribe(outcome.url, "", folderId, outcome.feed.siteUrl, now)
        val custom = RssText.clean(title, 200)
        replaceFeed(feed.copy(customTitle = custom))
        applyFetch(feed.id, outcome, now)
        return feed(feed.id)!!
    }

    private fun replaceFeed(feed: RssFeed) {
        val index = feeds.indexOfFirst { it.id == feed.id }
        if (index >= 0) feeds[index] = feed
    }

    @Synchronized
    fun renameFeed(id: String, title: String) {
        val feed = feed(id) ?: throw RssLibraryException("feed_not_found")
        val clean = RssText.clean(title, 200)
        replaceFeed(feed.copy(customTitle = if (clean == feed.title) "" else clean))
        touch()
    }

    @Synchronized
    fun moveFeed(id: String, folderId: String) {
        val feed = feed(id) ?: throw RssLibraryException("feed_not_found")
        val target = folderId.takeIf { fid -> folders.any { it.id == fid } }.orEmpty()
        replaceFeed(feed.copy(folderId = target))
        touch()
    }

    @Synchronized
    fun unsubscribe(id: String, now: Long) {
        val feed = feed(id) ?: return
        feeds.removeAll { it.id == id }
        articles.remove(id)
        removedFeeds.add(id)
        settings = settings.copy(collapsed = settings.collapsed - "feed:$id")
        log("unsubscribe", "Unsubscribed from ${feed.displayTitle}", now)
        touch()
    }

    @Synchronized
    fun due(now: Long, force: Boolean = false): List<RssFeed> = feeds.filter { force || it.nextCheck <= now }

    @Synchronized
    fun checkSoon(id: String) {
        val feed = feed(id) ?: return
        replaceFeed(feed.copy(nextCheck = 0))
        touch()
    }

    @Synchronized
    fun markRefreshed(now: Long) {
        lastRefresh = now
        touch()
    }

    @Synchronized
    fun applyFetch(feedId: String, outcome: RssFetchOutcome, now: Long): Int {
        val feed = feed(feedId) ?: return 0
        when (outcome) {
            is RssFetchOutcome.Failed -> {
                val errors = feed.errorCount + 1
                replaceFeed(feed.copy(lastCheck = now, nextCheck = now + backoff(errors), errorCount = errors, error = outcome.error, errorAt = now))
                touch()
                return 0
            }
            is RssFetchOutcome.NotModified -> {
                replaceFeed(feed.copy(lastCheck = now, lastSuccess = now, nextCheck = now + FETCH_INTERVAL_MS, errorCount = 0, error = "", errorAt = 0, viaHttps = outcome.viaHttps))
                touch()
                return 0
            }
            is RssFetchOutcome.Fetched -> {
                val parsed = outcome.feed
                var url = feed.url
                var previous = feed.previousUrls
                if (outcome.permanent && outcome.url.isNotEmpty() && outcome.url != feed.url) {
                    val clash = feeds.any { it.id != feed.id && RssUrls.identity(it.url) == RssUrls.identity(outcome.url) }
                    if (!clash) {
                        previous = (previous + feed.url).distinct().takeLast(10)
                        url = outcome.url
                    }
                }
                val site = parsed.siteUrl.ifEmpty { feed.siteUrl }
                replaceFeed(
                    feed.copy(
                        url = url,
                        previousUrls = previous,
                        title = parsed.title.ifEmpty { feed.title },
                        siteUrl = site,
                        description = parsed.description.ifEmpty { feed.description },
                        iconUrl = parsed.iconUrl.ifEmpty { feed.iconUrl }.ifEmpty { RssUrls.favicon(site.ifEmpty { url }) },
                        etag = outcome.etag.take(200),
                        lastModified = outcome.lastModified.take(200),
                        lastCheck = now,
                        lastSuccess = now,
                        nextCheck = now + FETCH_INTERVAL_MS,
                        errorCount = 0,
                        error = "",
                        errorAt = 0,
                        viaHttps = outcome.viaHttps,
                    ),
                )
                val added = storeItems(feedId, url, parsed.items, now)
                touch(feedId = feedId)
                return added
            }
        }
    }

    private fun storeItems(feedId: String, feedUrl: String, items: List<RssParsedItem>, now: Long): Int {
        val list = articles.getOrPut(feedId) { ArrayList() }
        val index = HashMap<String, Int>()
        list.forEachIndexed { i, article -> index[article.guid] = i }
        var added = 0
        for (item in items) {
            val guid = RssGuid.of(item)
            if (guid.isEmpty()) continue
            val content = RssSanitizer.sanitize(item.contentHtml, item.url.ifEmpty { feedUrl })
            val summarySource = item.summaryHtml.ifEmpty { item.contentHtml }
            var published = if (item.published > 0) item.published else item.updated
            if (published <= 0 || published > now + FUTURE_SLACK_MS) published = now
            val title = RssText.clean(item.title, 1000).ifEmpty { RssSanitizer.plainText(summarySource, 120) }
            val key = contentKey(title, item.url, content)
            val existing = index[guid]
            if (existing != null) {
                val old = list[existing]
                if (old.contentKey != key) {
                    list[existing] = old.copy(
                        title = title, url = item.url, externalUrl = item.externalUrl, author = RssText.clean(item.author, 200),
                        summary = RssSanitizer.plainText(summarySource, RssSanitizer.MAX_SUMMARY_CHARS), contentHtml = content,
                        commentsUrl = item.commentsUrl, enclosures = item.enclosures, image = RssSanitizer.firstImage(content),
                        updated = item.updated, contentKey = key, readerHtml = "",
                    )
                }
                continue
            }
            val article = RssArticle(
                id = articleId(feedId, guid), feedId = feedId, guid = guid, title = title, url = item.url,
                externalUrl = item.externalUrl, author = RssText.clean(item.author, 200),
                summary = RssSanitizer.plainText(summarySource, RssSanitizer.MAX_SUMMARY_CHARS), contentHtml = content,
                commentsUrl = item.commentsUrl, enclosures = item.enclosures, image = RssSanitizer.firstImage(content),
                published = published, updated = item.updated, arrived = now, contentKey = key,
            )
            index[guid] = list.size
            list.add(article)
            added++
        }
        prune(list)
        return added
    }

    private fun prune(list: MutableList<RssArticle>) {
        if (list.size <= MAX_ARTICLES_PER_FEED) return
        val keep = list.sortedWith(compareByDescending<RssArticle> { it.published }.thenByDescending { it.id }).take(MAX_ARTICLES_PER_FEED).map { it.id }.toHashSet()
        list.removeAll { it.id !in keep && !it.starred }
    }

    @Synchronized
    fun setRead(ids: Collection<String>, read: Boolean): Int = update(ids) { if (it.read == read) null else it.copy(read = read) }

    @Synchronized
    fun setStarred(ids: Collection<String>, starred: Boolean): Int = update(ids) { if (it.starred == starred) null else it.copy(starred = starred) }

    @Synchronized
    fun setReaderHtml(id: String, html: String) {
        update(listOf(id)) { it.copy(readerHtml = html) }
    }

    private fun update(ids: Collection<String>, change: (RssArticle) -> RssArticle?): Int {
        val wanted = ids.toHashSet()
        var count = 0
        for (feedId in wanted.map { it.substringBeforeLast('-') }.toSet()) {
            val list = articles[feedId] ?: continue
            for (i in list.indices) {
                if (list[i].id !in wanted) continue
                val next = change(list[i]) ?: continue
                list[i] = next
                count++
                dirtyFeeds.add(feedId)
            }
        }
        if (count > 0) version++
        return count
    }

    @Synchronized
    fun feedIds(scope: RssScope): Set<String>? = when (scope) {
        is RssScope.Feed -> setOf(scope.id)
        is RssScope.Folder -> feeds.filter { it.folderId == scope.id }.map { it.id }.toSet()
        else -> null
    }

    @Synchronized
    fun articles(scope: RssScope, now: Long): List<RssArticle> {
        val ids = feedIds(scope)
        val source = if (ids == null) articles.values.asSequence().flatten() else ids.asSequence().flatMap { articles[it].orEmpty() }
        return when (scope) {
            RssScope.Today -> source.filter { it.published >= now - TODAY_MS }
            RssScope.Unread -> source.filter { !it.read }
            RssScope.Starred -> source.filter { it.starred }
            else -> source
        }.toList()
    }

    @Synchronized
    fun timeline(scope: RssScope, now: Long, query: String = "", keep: Set<String> = emptySet()): List<RssArticle> {
        var list = articles(scope, now)
        if (keep.isNotEmpty() && scope == RssScope.Unread) {
            val present = list.map { it.id }.toHashSet()
            list = list + articles.values.flatten().filter { it.id in keep && it.id !in present }
        }
        val needle = query.trim().lowercase()
        if (needle.isNotEmpty()) {
            list = list.filter { article ->
                article.title.lowercase().contains(needle) || article.summary.lowercase().contains(needle) ||
                    article.author.lowercase().contains(needle) || (feed(article.feedId)?.displayTitle?.lowercase()?.contains(needle) == true)
            }
        }
        return sort(list, scope)
    }

    private fun sort(list: List<RssArticle>, scope: RssScope): List<RssArticle> {
        val oldest = settings.sortOldestFirst
        val byDate: Comparator<RssArticle> = if (oldest) compareBy<RssArticle> { it.published }.thenBy { it.id } else compareByDescending<RssArticle> { it.published }.thenByDescending { it.id }
        if (!settings.groupByFeed || scope is RssScope.Feed) return list.sortedWith(byDate)
        val titles = feeds.associate { it.id to it.displayTitle.lowercase() }
        return list.sortedWith(compareBy<RssArticle> { titles[it.feedId].orEmpty() }.thenBy { it.feedId }.then(byDate))
    }

    @Synchronized
    fun counts(now: Long): RssCounts {
        var today = 0
        var unread = 0
        var starred = 0
        val byFeed = HashMap<String, Int>()
        for ((feedId, list) in articles) {
            var feedUnread = 0
            for (article in list) {
                if (article.starred) starred++
                if (!article.read) {
                    unread++
                    feedUnread++
                    if (article.published >= now - TODAY_MS) today++
                }
            }
            byFeed[feedId] = feedUnread
        }
        val byFolder = HashMap<String, Int>()
        for (feed in feeds) if (feed.folderId.isNotEmpty()) byFolder[feed.folderId] = (byFolder[feed.folderId] ?: 0) + (byFeed[feed.id] ?: 0)
        return RssCounts(today, unread, starred, byFeed, byFolder)
    }

    @Synchronized
    fun sidebar(unreadOnly: Boolean, counts: RssCounts, current: String = ""): List<RssSidebarEntry> {
        val shows = { feed: RssFeed -> !unreadOnly || (counts.byFeed[feed.id] ?: 0) > 0 || current == "feed:${feed.id}" }
        val known = folders.map { it.id }.toSet()
        val entries = ArrayList<RssSidebarEntry>()
        for (folder in folders) {
            val children = feeds.filter { it.folderId == folder.id && shows(it) }.sortedWith(compareBy(String.CASE_INSENSITIVE_ORDER) { it.displayTitle })
            if (unreadOnly && children.isEmpty() && current != "folder:${folder.id}") continue
            entries.add(RssSidebarEntry.FolderEntry(folder, children))
        }
        for (feed in feeds) if ((feed.folderId.isEmpty() || feed.folderId !in known) && shows(feed)) entries.add(RssSidebarEntry.FeedEntry(feed))
        return entries.sortedWith(compareBy(String.CASE_INSENSITIVE_ORDER) { it.name })
    }

    @Synchronized
    fun feedOrder(): List<String> {
        val counts = counts(0)
        return sidebar(false, counts).flatMap { entry ->
            when (entry) {
                is RssSidebarEntry.FeedEntry -> listOf(entry.feed.id)
                is RssSidebarEntry.FolderEntry -> entry.feeds.map { it.id }
            }
        }
    }

    @Synchronized
    fun nextUnread(scope: RssScope, list: List<RssArticle>, currentId: String?, now: Long): Pair<RssScope, String>? {
        val unread = list.map { article(it.id) ?: it }
        val index = unread.indexOfFirst { it.id == currentId }
        val after = unread.drop(index + 1).firstOrNull { !it.read } ?: unread.take(maxOf(0, index)).firstOrNull { !it.read }
        if (after != null) return scope to after.id
        val order = feedOrder()
        val start = if (scope is RssScope.Feed) order.indexOf(scope.id) else -1
        val candidates = order.drop(start + 1) + order.take(maxOf(0, start))
        for (feedId in candidates) {
            val target = RssScope.Feed(feedId)
            val first = timeline(target, now).firstOrNull { !it.read } ?: continue
            return target to first.id
        }
        return null
    }

    @Synchronized
    fun importOpml(document: String, now: Long): RssImportResult {
        val entries = RssOpml.parse(document)
        var imported = 0
        var skipped = 0
        val before = folders.size
        for (entry in entries) {
            if (feeds.size >= MAX_FEEDS || findFeed(entry.url) != null) {
                skipped++
                continue
            }
            val folderId = if (entry.folder.isNotEmpty()) runCatching { addFolder(entry.folder).id }.getOrDefault("") else ""
            subscribe(entry.url, entry.title, folderId, entry.siteUrl, now)
            imported++
        }
        val created = folders.size - before
        log("import", "Imported $imported subscription${if (imported == 1) "" else "s"}" + if (skipped > 0) ", skipped $skipped" else "", now)
        return RssImportResult(imported, skipped, created)
    }

    @Synchronized
    fun exportOpml(now: Long): String = RssOpml.build(folders.toList(), feeds.toList(), now)

    @Synchronized
    fun failingFeeds(): List<RssFeed> = feeds.filter { it.errorCount > 0 }.sortedByDescending { it.errorAt }

    @Synchronized
    fun stats(): RssStats {
        val all = articles.values.flatten()
        return RssStats(
            feeds = feeds.size,
            folders = folders.size,
            articles = all.size,
            unread = all.count { !it.read },
            starred = all.count { it.starred },
            failingFeeds = feeds.count { it.errorCount > 0 },
            oldestArticle = all.minOfOrNull { it.published } ?: 0,
            lastRefresh = maxOf(lastRefresh, feeds.maxOfOrNull { it.lastCheck } ?: 0),
        )
    }
}
