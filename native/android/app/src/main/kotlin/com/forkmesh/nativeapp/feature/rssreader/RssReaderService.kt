package com.forkmesh.nativeapp.feature.rssreader

import android.content.Context
import android.graphics.BitmapFactory
import android.security.NetworkSecurityPolicy
import android.util.Base64
import android.util.LruCache
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import com.forkmesh.nativeapp.rssreader.RssErrors
import com.forkmesh.nativeapp.rssreader.RssFeed
import com.forkmesh.nativeapp.rssreader.RssFetchException
import com.forkmesh.nativeapp.rssreader.RssFetchOutcome
import com.forkmesh.nativeapp.rssreader.RssFormat
import com.forkmesh.nativeapp.rssreader.RssFetcher
import com.forkmesh.nativeapp.rssreader.RssImportResult
import com.forkmesh.nativeapp.rssreader.RssLibrary
import com.forkmesh.nativeapp.rssreader.RssLibraryException
import com.forkmesh.nativeapp.rssreader.RssReadable
import com.forkmesh.nativeapp.rssreader.RssRefresher
import com.forkmesh.nativeapp.rssreader.RssStore
import com.forkmesh.nativeapp.rssreader.RssUrlConnectionHttp
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.Semaphore
import kotlinx.coroutines.sync.withPermit
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import java.io.File
import java.util.concurrent.ConcurrentHashMap

class RssReaderService(context: Context) {
    private val appContext = context.applicationContext
    private val store = RssStore(File(appContext.filesDir, "rss-reader"))
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val loadLock = Mutex()
    private var loadedLibrary: RssLibrary? = null
    private var saveJob: Job? = null
    private var refreshJob: Job? = null
    private val iconAttempts = ConcurrentHashMap.newKeySet<String>()
    private val iconBytes = ConcurrentHashMap<String, ByteArray>()
    private val thumbnails = LruCache<String, ImageBitmap>(MAX_THUMBNAILS)
    private val thumbnailFailures = ConcurrentHashMap.newKeySet<String>()
    private val thumbnailGate = Semaphore(4)

    companion object {
        const val MAX_THUMBNAILS = 300
        const val MAX_THUMBNAIL_BYTES = 4 * 1024 * 1024
    }

    private val versionState = MutableStateFlow(-1L)
    val version: StateFlow<Long> = versionState.asStateFlow()

    private val refreshingState = MutableStateFlow(false)
    val refreshing: StateFlow<Boolean> = refreshingState.asStateFlow()

    private val iconState = MutableStateFlow<Map<String, ImageBitmap>>(emptyMap())
    val icons: StateFlow<Map<String, ImageBitmap>> = iconState.asStateFlow()

    private fun cleartextAllowed(host: String): Boolean =
        runCatching { NetworkSecurityPolicy.getInstance().isCleartextTrafficPermitted(host) }.getOrDefault(false)

    val fetcher = RssFetcher(RssUrlConnectionHttp("ForkMesh-Reader/1.0 (Android)", ::cleartextAllowed), ::cleartextAllowed)

    val library: RssLibrary? get() = loadedLibrary

    suspend fun load(): RssLibrary {
        loadedLibrary?.let { return it }
        return loadLock.withLock {
            loadedLibrary ?: withContext(Dispatchers.IO) {
                val loaded = store.load()
                loadedLibrary = loaded
                val decoded = HashMap<String, ImageBitmap>()
                for (feed in loaded.feeds()) {
                    val bytes = store.readIcon(feed.id) ?: continue
                    iconBytes[feed.id] = bytes
                    decode(bytes)?.let { decoded[feed.id] = it }
                }
                iconState.value = decoded
                versionState.value = loaded.version
                loaded
            }
        }
    }

    private fun decode(bytes: ByteArray): ImageBitmap? =
        runCatching { BitmapFactory.decodeByteArray(bytes, 0, bytes.size)?.asImageBitmap() }.getOrNull()

    fun changed() {
        val current = loadedLibrary ?: return
        versionState.value = current.version
        saveJob?.cancel()
        saveJob = scope.launch {
            delay(400)
            runCatching { store.save(current) }
        }
    }

    fun mutate(block: (RssLibrary) -> Unit) {
        val current = loadedLibrary ?: return
        block(current)
        changed()
    }

    fun open() {
        scope.launch {
            load()
            refresh(force = false)
        }
    }

    fun refresh(force: Boolean): Job? {
        if (refreshJob?.isActive == true) return refreshJob
        val job = scope.launch {
            val current = load()
            refreshingState.value = true
            try {
                val due = current.due(System.currentTimeMillis(), force)
                if (due.isNotEmpty()) {
                    val summary = RssRefresher(current, fetcher).refresh(due, System::currentTimeMillis) { versionState.value = current.version }
                    val text = buildString {
                        append("Checked ${summary.checked} feed${if (summary.checked == 1) "" else "s"}")
                        if (summary.added > 0) append(", ${summary.added} new article${if (summary.added == 1) "" else "s"}")
                        if (summary.failed > 0) append(", ${summary.failed} failed")
                    }
                    current.log("refresh", text, System.currentTimeMillis())
                }
                current.markRefreshed(System.currentTimeMillis())
                changed()
                fetchIcons(current)
            } finally {
                refreshingState.value = false
            }
        }
        refreshJob = job
        return job
    }

    private fun fetchIcons(current: RssLibrary) {
        val missing = current.feeds().filter { !iconBytes.containsKey(it.id) && iconAttempts.add(it.id) }
        if (missing.isEmpty()) return
        val decoded = HashMap(iconState.value)
        for (feed in missing) {
            val bytes = runCatching { fetcher.icon(feed) }.getOrNull() ?: continue
            val bitmap = decode(bytes) ?: continue
            iconBytes[feed.id] = bytes
            runCatching { store.writeIcon(feed.id, bytes) }
            decoded[feed.id] = bitmap
        }
        iconState.value = decoded
    }

    fun cachedThumbnail(url: String): ImageBitmap? = thumbnails.get(url)

    suspend fun thumbnail(url: String, sizePx: Int): ImageBitmap? {
        thumbnails.get(url)?.let { return it }
        if (url in thumbnailFailures) return null
        return thumbnailGate.withPermit {
            withContext(Dispatchers.IO) {
                thumbnails.get(url) ?: runCatching {
                    val response = fetcher.request(url, mapOf("Accept" to "image/*"), MAX_THUMBNAIL_BYTES).response
                    val bytes = response.body.takeIf { response.status in 200..299 && it.isNotEmpty() } ?: return@runCatching null
                    val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
                    BitmapFactory.decodeByteArray(bytes, 0, bytes.size, bounds)
                    if (bounds.outWidth <= 0 || bounds.outHeight <= 0) return@runCatching null
                    var sample = 1
                    while (minOf(bounds.outWidth, bounds.outHeight) / (sample * 2) >= sizePx) sample *= 2
                    BitmapFactory.decodeByteArray(bytes, 0, bytes.size, BitmapFactory.Options().apply { inSampleSize = sample })?.asImageBitmap()
                }.getOrNull().also { image ->
                    if (image != null) thumbnails.put(url, image) else thumbnailFailures.add(url)
                }
            }
        }
    }

    suspend fun checkNow(feedId: String): String = withContext(Dispatchers.IO) {
        val current = load()
        val feed = current.feed(feedId) ?: throw RssLibraryException("feed_not_found")
        val outcome = fetcher.fetchNow(feed)
        val now = System.currentTimeMillis()
        current.applyFetch(feedId, outcome, now)
        changed()
        scope.launch { fetchIcons(current) }
        when (outcome) {
            is RssFetchOutcome.Fetched -> "${outcome.feed.format} · ${RssFormat.plural(outcome.feed.items.size, "item")} · checked ${RssFormat.time(now)}"
            is RssFetchOutcome.NotModified -> "Not modified · checked ${RssFormat.time(now)}"
            is RssFetchOutcome.Failed -> RssErrors.describe(outcome.error)
        }
    }

    suspend fun changeFeedUrl(feedId: String, input: String): String {
        val current = load()
        val before = current.feed(feedId)?.url
        val feed = current.changeFeedUrl(feedId, input)
        if (feed.url == before) return ""
        changed()
        return checkNow(feedId)
    }

    suspend fun importOpmlUrl(url: String): RssImportResult {
        val text = withContext(Dispatchers.IO) {
            try {
                fetcher.opml(url)
            } catch (error: Exception) {
                throw RssFetchException(fetcher.errorCode(error))
            }
        }
        return importOpml(text)
    }

    fun iconDataUri(feedId: String): String {
        val bytes = iconBytes[feedId] ?: return ""
        if (bytes.size > 96 * 1024) return ""
        val type = when {
            bytes.size > 4 && bytes[0] == 0x89.toByte() && bytes[1] == 'P'.code.toByte() -> "image/png"
            bytes.size > 3 && bytes[0] == 0xFF.toByte() && bytes[1] == 0xD8.toByte() -> "image/jpeg"
            bytes.size > 3 && bytes[0] == 'G'.code.toByte() && bytes[1] == 'I'.code.toByte() -> "image/gif"
            bytes.size > 12 && bytes[8] == 'W'.code.toByte() && bytes[9] == 'E'.code.toByte() -> "image/webp"
            bytes.size > 4 && bytes[0] == 0.toByte() && bytes[2] == 1.toByte() -> "image/x-icon"
            else -> return ""
        }
        return "data:$type;base64," + Base64.encodeToString(bytes, Base64.NO_WRAP)
    }

    suspend fun addFeed(input: String, folderId: String, title: String): RssFeed = withContext(Dispatchers.IO) {
        val current = load()
        val now = System.currentTimeMillis()
        current.findFeed(com.forkmesh.nativeapp.rssreader.RssUrls.normalizeFeedInput(input))?.let { throw RssLibraryException("feed_exists") }
        val found = try {
            fetcher.discover(input)
        } catch (error: Exception) {
            throw RssFetchException(fetcher.errorCode(error))
        }
        val feed = current.addFetched(found.outcome, title, folderId, now)
        current.log("add", "Subscribed to ${feed.displayTitle}", now)
        changed()
        scope.launch { fetchIcons(current) }
        feed
    }

    suspend fun importOpml(text: String): RssImportResult = withContext(Dispatchers.IO) {
        val current = load()
        val result = current.importOpml(text, System.currentTimeMillis())
        changed()
        if (result.imported > 0) refresh(force = false)
        result
    }

    suspend fun exportOpml(): String = withContext(Dispatchers.IO) { load().exportOpml(System.currentTimeMillis()) }

    suspend fun readerView(articleId: String): RssReadable = withContext(Dispatchers.IO) {
        val current = load()
        val article = current.article(articleId) ?: throw RssFetchException("no_link")
        val readable = fetcher.readerView(article)
        if (readable.contentHtml.isNotEmpty()) {
            current.setReaderHtml(articleId, readable.contentHtml)
            changed()
        }
        readable
    }

    fun errorText(error: Throwable): String = com.forkmesh.nativeapp.rssreader.RssErrors.describe(
        when (error) {
            is RssLibraryException -> error.code
            else -> fetcher.errorCode(error)
        },
    )
}
