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
    private val filters = data.filters.toMutableList()
    private val smartFeeds = data.smartFeeds.toMutableList()
    private val matchers = HashMap<String, RssMatcher>()
    private var hiddenCache: Set<String>? = null
    private val textCache = object : LinkedHashMap<String, String>(256, 0.75f, true) {
        override fun removeEldestEntry(eldest: MutableMap.MutableEntry<String, String>?): Boolean = size > 4000
    }
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
        const val MAX_FILTERS = 100
        const val MAX_SMART_FEEDS = 50
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
    fun snapshotData(): RssLibraryData =
        RssLibraryData(folders.toList(), feeds.toList(), settings, activity.toList(), lastRefresh, filters.toList(), smartFeeds.toList())

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
        filters.removeAll { it.feedId == id }
        hiddenCache = null
        settings = settings.copy(collapsed = settings.collapsed - "feed:$id")
        log("unsubscribe", "Unsubscribed from ${feed.displayTitle}", now)
        touch()
    }

    @Synchronized
    fun due(now: Long, force: Boolean = false): List<RssFeed> = feeds.filter { !it.paused && (force || it.nextCheck <= now) }

    @Synchronized
    fun setFeedSwitches(id: String, paused: Boolean, excludeFromToday: Boolean, excludeFromUnread: Boolean) {
        val feed = feed(id) ?: throw RssLibraryException("feed_not_found")
        val next = feed.copy(paused = paused, excludeFromToday = excludeFromToday, excludeFromUnread = excludeFromUnread)
        if (next == feed) return
        replaceFeed(next)
        touch()
    }

    @Synchronized
    fun changeFeedUrl(id: String, input: String): RssFeed {
        val feed = feed(id) ?: throw RssLibraryException("feed_not_found")
        val clean = RssUrls.normalizeFeedInput(input)
        if (clean.isEmpty() || clean.any { it.isWhitespace() } || RssUrls.host(clean).isEmpty()) throw RssLibraryException("invalid_url")
        if (clean == feed.url) return feed
        val key = RssUrls.identity(clean)
        val clash = feeds.any { other -> other.id != id && (RssUrls.identity(other.url) == key || other.previousUrls.any { RssUrls.identity(it) == key }) }
        if (clash) throw RssLibraryException("feed_exists")
        val next = feed.copy(
            url = clean,
            previousUrls = (feed.previousUrls + feed.url).filter { it != clean }.distinct().takeLast(10),
            etag = "",
            lastModified = "",
            error = "",
            errorCount = 0,
            errorAt = 0,
            nextCheck = 0,
            viaHttps = false,
        )
        replaceFeed(next)
        touch()
        return next
    }

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
                        language = parsed.language.ifEmpty { feed.language },
                    ),
                )
                val added = storeItems(feedId, url, parsed.items, now, parsed.language.ifEmpty { feed.language })
                hiddenCache = null
                touch(feedId = feedId)
                return added
            }
        }
    }

    private fun itemLanguage(item: RssParsedItem, feedLanguage: String): String =
        item.language.takeIf { it.isNotEmpty() && !it.equals(feedLanguage, ignoreCase = true) }.orEmpty()

    private fun itemImage(item: RssParsedItem, content: String): String =
        item.image.ifEmpty { item.images.firstOrNull().orEmpty() }.ifEmpty { RssSanitizer.firstImage(content) }

    private fun arrivalRead(article: RssArticle, links: Set<String>?): Boolean {
        if (article.starred) return false
        val rules = filters.filter { it.action == RssFilterAction.MARK_READ && it.pattern.isNotEmpty() }
        if (rules.any { RssRules.filterMatches(it, matcher(it.pattern, it.regex), article) }) return true
        if (links != null) {
            val identity = RssRules.linkIdentity(article.link)
            if (identity.isNotEmpty() && identity in links) return true
        }
        return false
    }

    private fun storeItems(feedId: String, feedUrl: String, items: List<RssParsedItem>, now: Long, feedLanguage: String = ""): Int {
        val list = articles.getOrPut(feedId) { ArrayList() }
        val index = HashMap<String, Int>()
        list.forEachIndexed { i, article -> index[article.guid] = i }
        var added = 0
        var links: Set<String>? = null
        val linksLoaded = { ->
            links ?: articles.filterKeys { it != feedId }.values.asSequence().flatten()
                .map { RssRules.linkIdentity(it.link) }.filter { it.isNotEmpty() }.toHashSet().also { links = it }
        }
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
                        commentsUrl = item.commentsUrl, enclosures = item.enclosures, image = itemImage(item, content),
                        updated = item.updated, contentKey = key, readerHtml = "", images = item.images,
                        language = itemLanguage(item, feedLanguage),
                    )
                }
                continue
            }
            var article = RssArticle(
                id = articleId(feedId, guid), feedId = feedId, guid = guid, title = title, url = item.url,
                externalUrl = item.externalUrl, author = RssText.clean(item.author, 200),
                summary = RssSanitizer.plainText(summarySource, RssSanitizer.MAX_SUMMARY_CHARS), contentHtml = content,
                commentsUrl = item.commentsUrl, enclosures = item.enclosures, image = itemImage(item, content),
                published = published, updated = item.updated, arrived = now, contentKey = key, images = item.images,
                language = itemLanguage(item, feedLanguage),
            )
            if (arrivalRead(article, if (settings.foldDuplicates) linksLoaded() else null)) article = article.copy(read = true)
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
        list.removeAll { it.id !in keep && !it.starred && !it.readLater }
    }

    @Synchronized
    fun setRead(ids: Collection<String>, read: Boolean): Int = update(ids) { if (it.read == read) null else it.copy(read = read) }

    @Synchronized
    fun setStarred(ids: Collection<String>, starred: Boolean): Int = update(ids) { if (it.starred == starred) null else it.copy(starred = starred) }

    @Synchronized
    fun setReadLater(ids: Collection<String>, readLater: Boolean): Int = update(ids) { if (it.readLater == readLater) null else it.copy(readLater = readLater) }

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

    private fun matcher(pattern: String, regex: Boolean): RssMatcher =
        matchers.getOrPut((if (regex) "r:" else "p:") + pattern) { RssMatcher(pattern, regex) }

    private fun cachedText(article: RssArticle): String {
        val key = article.id + "|" + article.contentKey
        return textCache[key] ?: RssRules.text(article).also { textCache[key] = it }
    }

    private fun hiddenIds(): Set<String> {
        hiddenCache?.let { return it }
        val rules = filters.filter { it.action == RssFilterAction.HIDE && it.pattern.isNotEmpty() }
        val result = HashSet<String>()
        if (rules.isNotEmpty()) {
            for (list in articles.values) for (article in list) {
                if (rules.any { RssRules.filterMatches(it, matcher(it.pattern, it.regex), article) { cachedText(article) } }) result.add(article.id)
            }
        }
        hiddenCache = result
        return result
    }

    private fun isHidden(article: RssArticle, hidden: Set<String>): Boolean = !article.starred && article.id in hidden

    @Synchronized
    fun hiddenByFilter(id: String): Boolean = article(id)?.let { isHidden(it, hiddenIds()) } == true

    private fun smartMatch(smart: RssSmartFeed, article: RssArticle, folders: Map<String, String>, now: Long): Boolean =
        RssRules.smartMatches(smart, matcher(smart.words, smart.regex), article, folders[article.feedId].orEmpty(), now) { cachedText(article) }

    @Synchronized
    fun feedIds(scope: RssScope): Set<String>? = when (scope) {
        is RssScope.Feed -> setOf(scope.id)
        is RssScope.Folder -> feeds.filter { it.folderId == scope.id }.map { it.id }.toSet()
        RssScope.Today -> feeds.filter { !it.excludeFromToday && !it.excludeFromUnread }.map { it.id }.toSet()
        RssScope.Unread -> feeds.filter { !it.excludeFromUnread }.map { it.id }.toSet()
        else -> null
    }

    private fun pool(scope: RssScope, now: Long): List<RssArticle> {
        val ids = feedIds(scope)
        val hidden = hiddenIds()
        val source = (if (ids == null) articles.values.asSequence().flatten() else ids.asSequence().flatMap { articles[it].orEmpty() })
            .filter { !isHidden(it, hidden) }
        return when (scope) {
            RssScope.Today -> source.filter { it.published >= now - TODAY_MS }
            RssScope.Starred -> source.filter { it.starred }
            RssScope.ReadLater -> source.filter { it.readLater }
            is RssScope.Smart -> {
                val smart = smartFeeds.firstOrNull { it.id == scope.id }
                if (smart == null) emptySequence() else {
                    val folderOf = feeds.associate { it.id to it.folderId }
                    source.filter { smartMatch(smart, it, folderOf, now) }
                }
            }
            else -> source
        }.toList()
    }

    @Synchronized
    fun articles(scope: RssScope, now: Long): List<RssArticle> {
        val list = pool(scope, now)
        return if (scope == RssScope.Unread) list.filter { !it.read } else list
    }

    @Synchronized
    fun hidesRead(scope: RssScope): Boolean = scope != RssScope.Starred && (scope == RssScope.Unread || settings.hideReadArticles)

    @Synchronized
    fun timeline(scope: RssScope, now: Long, query: String = "", keep: Set<String> = emptySet()): List<RssArticle> {
        var list = pool(scope, now)
        if (hidesRead(scope)) list = list.filter { !it.read || it.id in keep }
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

    private fun markable(article: RssArticle): Boolean = !article.read && !(settings.markAllKeepsStarred && article.starred)

    @Synchronized
    fun markAllRead(list: List<RssArticle>): Int = setRead(list.mapNotNull { listed -> article(listed.id)?.takeIf(::markable)?.id }, true)

    @Synchronized
    fun catchUpIds(scope: RssScope?, cutoff: Long, now: Long): List<String> {
        val source = if (scope == null) {
            val hidden = hiddenIds()
            articles.values.flatten().filter { !isHidden(it, hidden) }
        } else {
            articles(scope, now)
        }
        return source.filter { markable(it) && RssRules.articleDate(it) < cutoff }.map { it.id }
    }

    @Synchronized
    fun catchUp(scope: RssScope?, cutoff: Long, now: Long): Int = setRead(catchUpIds(scope, cutoff, now), true)

    @Synchronized
    fun counts(now: Long): RssCounts {
        var today = 0
        var unread = 0
        var starred = 0
        var readLater = 0
        var readLaterTotal = 0
        val hidden = hiddenIds()
        val byFeed = HashMap<String, Int>()
        val counted = HashSet<String>()
        for (feed in feeds) {
            var feedUnread = 0
            for (article in articles[feed.id].orEmpty()) {
                if (isHidden(article, hidden)) continue
                if (article.starred) starred++
                if (article.readLater) {
                    readLaterTotal++
                    if (!article.read && !feed.excludeFromUnread) readLater++
                }
                if (!article.read) {
                    feedUnread++
                    if (!feed.excludeFromUnread) {
                        unread++
                        counted.add(article.id)
                        if (!feed.excludeFromToday && article.published >= now - TODAY_MS) today++
                    }
                }
            }
            byFeed[feed.id] = feedUnread
        }
        val byFolder = HashMap<String, Int>()
        for (feed in feeds) {
            if (feed.folderId.isNotEmpty() && !feed.excludeFromUnread) byFolder[feed.folderId] = (byFolder[feed.folderId] ?: 0) + (byFeed[feed.id] ?: 0)
        }
        val bySmart = HashMap<String, Int>()
        if (smartFeeds.isNotEmpty()) {
            val folderOf = feeds.associate { it.id to it.folderId }
            val candidates = feeds.flatMap { articles[it.id].orEmpty() }.filter { it.id in counted }
            for (smart in smartFeeds) bySmart[smart.id] = candidates.count { smartMatch(smart, it, folderOf, now) }
        }
        return RssCounts(today, unread, starred, byFeed, byFolder, readLater, readLaterTotal, bySmart)
    }

    private fun feedMatches(feed: RssFeed, needle: String): Boolean =
        needle.isEmpty() || feed.displayTitle.lowercase().contains(needle) || RssUrls.host(feed.url).contains(needle) || feed.url.lowercase().contains(needle)

    @Synchronized
    fun sidebar(unreadOnly: Boolean, counts: RssCounts, current: String = "", search: String = ""): List<RssSidebarEntry> {
        val needle = search.trim().lowercase()
        val searching = needle.isNotEmpty()
        val shows = { feed: RssFeed ->
            if (searching) feedMatches(feed, needle) else !unreadOnly || (counts.byFeed[feed.id] ?: 0) > 0 || current == "feed:${feed.id}"
        }
        val byName = compareBy<RssSidebarEntry, String>(String.CASE_INSENSITIVE_ORDER) { it.name }
        val feedOrder: Comparator<RssFeed> = if (settings.sortFeedsByUnread) {
            compareByDescending<RssFeed> { counts.byFeed[it.id] ?: 0 }.thenBy(String.CASE_INSENSITIVE_ORDER) { it.displayTitle }
        } else {
            compareBy(String.CASE_INSENSITIVE_ORDER) { it.displayTitle }
        }
        val known = folders.map { it.id }.toSet()
        val entries = ArrayList<RssSidebarEntry>()
        for (folder in folders) {
            val children = feeds.filter { it.folderId == folder.id && shows(it) }.sortedWith(feedOrder)
            if (searching && children.isEmpty()) continue
            if (!searching && unreadOnly && children.isEmpty() && current != "folder:${folder.id}") continue
            entries.add(RssSidebarEntry.FolderEntry(folder, children))
        }
        for (feed in feeds) if ((feed.folderId.isEmpty() || feed.folderId !in known) && shows(feed)) entries.add(RssSidebarEntry.FeedEntry(feed))
        if (!settings.sortFeedsByUnread) return entries.sortedWith(byName)
        val unreadOf = { entry: RssSidebarEntry ->
            when (entry) {
                is RssSidebarEntry.FeedEntry -> counts.byFeed[entry.feed.id] ?: 0
                is RssSidebarEntry.FolderEntry -> counts.byFolder[entry.folder.id] ?: 0
            }
        }
        return entries.sortedWith(compareByDescending(unreadOf).then(byName))
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
    fun filters(): List<RssFilter> = filters.toList()

    @Synchronized
    fun saveFilter(filter: RssFilter): RssFilter {
        val pattern = filter.pattern.trim().take(500)
        if (pattern.isEmpty()) throw RssLibraryException("pattern_required")
        if (!RssMatcher.valid(pattern, filter.regex)) throw RssLibraryException("invalid_regex")
        val feedId = filter.feedId.takeIf { id -> feeds.any { it.id == id } }.orEmpty()
        val index = filters.indexOfFirst { it.id == filter.id }
        val saved = filter.copy(id = filter.id.ifEmpty { newId() }, pattern = pattern, feedId = feedId)
        if (index >= 0) {
            filters[index] = saved
        } else {
            if (filters.size >= MAX_FILTERS) throw RssLibraryException("too_many_filters")
            filters.add(saved)
        }
        hiddenCache = null
        touch()
        return saved
    }

    @Synchronized
    fun deleteFilter(id: String) {
        if (!filters.removeAll { it.id == id }) return
        hiddenCache = null
        touch()
    }

    @Synchronized
    fun smartFeeds(): List<RssSmartFeed> = smartFeeds.sortedWith(compareBy(String.CASE_INSENSITIVE_ORDER) { it.name })

    @Synchronized
    fun smartFeed(id: String): RssSmartFeed? = smartFeeds.firstOrNull { it.id == id }

    @Synchronized
    fun saveSmartFeed(smart: RssSmartFeed): RssSmartFeed {
        val name = RssText.clean(smart.name, 120)
        if (name.isEmpty()) throw RssLibraryException("smart_feed_name_required")
        val words = smart.words.trim().take(500)
        if (!RssMatcher.valid(words, smart.regex)) throw RssLibraryException("invalid_regex")
        val scope = when {
            smart.scope.startsWith("feed:") && feeds.any { "feed:${it.id}" == smart.scope } -> smart.scope
            smart.scope.startsWith("folder:") && folders.any { "folder:${it.id}" == smart.scope } -> smart.scope
            else -> ""
        }
        val saved = smart.copy(id = smart.id.ifEmpty { newId() }, name = name, words = words, scope = scope, maxAgeDays = maxOf(0, smart.maxAgeDays))
        val index = smartFeeds.indexOfFirst { it.id == smart.id }
        if (index >= 0) {
            smartFeeds[index] = saved
        } else {
            if (smartFeeds.size >= MAX_SMART_FEEDS) throw RssLibraryException("too_many_smart_feeds")
            smartFeeds.add(saved)
        }
        touch()
        return saved
    }

    @Synchronized
    fun deleteSmartFeed(id: String) {
        if (smartFeeds.removeAll { it.id == id }) touch()
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
