package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.JsonMap
import com.forkmesh.nativeapp.json.asJsonMap
import com.forkmesh.nativeapp.json.bool
import com.forkmesh.nativeapp.json.firstStr
import com.forkmesh.nativeapp.json.int
import com.forkmesh.nativeapp.json.jsonMaps
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.json.strings
import com.forkmesh.nativeapp.rssreader.RssCharsets
import java.io.File
import java.net.URI
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

sealed class ChatLinkPreviewState {
    data object Idle : ChatLinkPreviewState()
    data object Loading : ChatLinkPreviewState()
    data class Ready(val preview: ChatLinkPreview) : ChatLinkPreviewState()
    data object None : ChatLinkPreviewState()
}

object ChatLinkPreviewPolicy {
    val everyoneRoomNames = setOf("general", "random", "welcome")

    fun autoLoads(conversation: String, fromSelf: Boolean, room: OfficeRoom?, everyone: Boolean = room?.name in everyoneRoomNames): Boolean {
        if (fromSelf || DirectMessageFrames.isDirect(conversation)) return true
        if (!OfficeFrames.isOffice(conversation) || room == null) return false
        return room.isPrivate && !everyone
    }
}

private class ChatLinkPending<T> {
    private val latch = CountDownLatch(1)

    @Volatile
    private var value: T? = null

    fun complete(result: T?) {
        value = result
        latch.countDown()
    }

    fun await(): T? {
        latch.await(60, TimeUnit.SECONDS)
        return value
    }
}

class ChatLinkPreviewService(
    http: ChatLinkHttp,
    private val prefs: PrefStore? = null,
    private val fileSource: () -> File? = { null },
    private val now: () -> Long = System::currentTimeMillis,
    resolver: ChatHostResolver = ChatLinkAddresses.system,
    private val saveLater: (Runnable) -> Unit = { it.run() },
) {
    companion object {
        const val ENABLED_KEY = "chat/link-previews/enabled"
        const val MAX_ENTRIES = 400
        const val MAX_PAGE_BYTES = 2 * 1024 * 1024
        const val MAX_IMAGE_BYTES = 6 * 1024 * 1024
        const val MAX_IMAGE_CACHE_BYTES = 48 * 1024 * 1024
        const val MAX_JSON_BYTES = 512 * 1024
        const val MAX_REDIRECTS = 4
        const val TIMEOUT_MS = 8_000
        const val FRESH_MS = 3L * 24 * 3600 * 1000
        const val MISS_MS = 6L * 3600 * 1000
        const val FINAL_URL_HEADER = "x-forkmesh-final-url"
        const val USER_AGENT =
            "Mozilla/5.0 (compatible; ForkMeshBot/1.0; +https://forkmesh.com/bot) facebookexternalhit/1.1 Twitterbot/1.0"
        const val BROWSER_AGENT =
            "Mozilla/5.0 (Linux; Android 15; Pixel 9) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0.0.0 Mobile Safari/537.36"
        val refusals = setOf(401, 403, 429, 503)

        fun fileName(project: String): String =
            "chat-link-previews-${ChatHistoryCodec.scopeName(project, "link-previews")}.json"

        fun read(file: File): List<ChatLinkPreview> = try {
            if (!file.isFile) emptyList() else jsonMaps(Json.parse(file.readText(Charsets.UTF_8))).mapNotNull(ChatLinkPreview::fromJson)
        } catch (_: Exception) {
            emptyList()
        }

        fun write(previews: List<ChatLinkPreview>, file: File) {
            runCatching {
                file.parentFile?.mkdirs()
                val temp = File(file.parentFile, file.name + ".part")
                temp.writeText(Json.stringify(previews.map { it.toJson() }), Charsets.UTF_8)
                if (!temp.renameTo(file)) {
                    file.writeText(temp.readText(Charsets.UTF_8), Charsets.UTF_8)
                    temp.delete()
                }
            }
        }

        fun fetch(
            start: URI,
            http: ChatLinkHttp,
            accept: String,
            limit: Int,
            truncate: Boolean = false,
            timeoutMs: Int = TIMEOUT_MS,
            agent: String = USER_AGENT,
        ): ChatLinkResponse? {
            var url = start
            for (hop in 0..MAX_REDIRECTS) {
                if (ChatLinkPreviewParsing.previewable(url.toString()) == null) return null
                val headers = mapOf("Accept" to accept, "User-Agent" to agent, "Accept-Language" to "en")
                val response = http.get(ChatLinkRequest(url.toString(), headers, timeoutMs, limit)) ?: return null
                if (response.status in 300..399) {
                    val location = response.header("location") ?: return null
                    url = ChatLinkPreviewParsing.resolveUri(location.trim(), url) ?: return null
                    continue
                }
                var body = response.body
                if (response.overflow || body.size >= limit) {
                    if (!truncate) return null
                    if (body.size > limit) body = body.copyOf(limit)
                } else if (response.interrupted && (body.isEmpty() || !truncate)) {
                    return null
                }
                return response.with(FINAL_URL_HEADER, url.toString(), body)
            }
            return null
        }

        fun resolve(url: URI, http: ChatLinkHttp, nowMs: Long): ChatLinkPreview? {
            val site = ChatLinkSites.site(url)
            var preview: ChatLinkPreview? = null
            if (ChatLinkPreviewParsing.isImageLink(url)) {
                preview = ChatLinkPreview(
                    url = url.toString(),
                    title = ChatLinkPreviewParsing.lastPathComponent(url),
                    imageUrl = url.toString(),
                    kind = ChatLinkKind.Image,
                )
            } else if (site != null) {
                preview = special(site, url, http)
            }
            if (preview == null || preview.isEmpty) preview = page(url, http, preview)
            var found = preview?.takeIf { !it.isEmpty } ?: return null
            val landed = ChatLinkPreviewParsing.uri(found.url) ?: url
            found = found.copy(url = url.toString())
            val known = site ?: found.siteKey.takeIf { it.isNotEmpty() }?.let(ChatLinkSites::siteForKey)
            if (known != null) {
                val host = ChatLinkSites.bareHost(url.host.orEmpty())
                found = found.copy(
                    siteKey = known.key,
                    siteName = if (known.key == "stackoverflow" && host != "stackoverflow.com") host else known.name,
                    kind = if (found.kind == ChatLinkKind.Page) known.kind else found.kind,
                )
            }
            if (found.siteName.isEmpty()) found = found.copy(siteName = ChatLinkSites.bareHost(landed.host ?: url.host.orEmpty()))
            if (found.title.isEmpty() && found.detail.isNotEmpty()) {
                found = found.copy(title = ChatLinkPreviewParsing.truncate(found.detail, ChatLinkPreviewParsing.MAX_TITLE), detail = "")
            }
            return found.copy(fetchedAtMs = nowMs)
        }

        private fun page(url: URI, http: ChatLinkHttp, fallback: ChatLinkPreview?): ChatLinkPreview? {
            var best = fallback
            for (agent in listOf(USER_AGENT, BROWSER_AGENT)) {
                val response = fetch(
                    url, http, "text/html,application/xhtml+xml;q=0.9,*/*;q=0.5", MAX_PAGE_BYTES, truncate = true, agent = agent,
                ) ?: continue
                if (response.status in refusals) continue
                if (!(response.ok || response.status == 206 || (response.status in 100..199 && response.body.isNotEmpty()))) return fallback
                val found = parsePage(response, url, fallback)?.takeIf { !it.isEmpty } ?: continue
                if (best == null || best.isEmpty || found.imageUrl.isNotEmpty()) best = found
                if (found.imageUrl.isNotEmpty() || found.detail.isNotEmpty()) return found
            }
            return best
        }

        private fun parsePage(response: ChatLinkResponse, url: URI, fallback: ChatLinkPreview?): ChatLinkPreview? {
            val finalUrl = response.header(FINAL_URL_HEADER)?.let(ChatLinkPreviewParsing::uri) ?: url
            val type = response.header("content-type").orEmpty().lowercase()
            if (type.startsWith("image/")) {
                return ChatLinkPreview(
                    url = url.toString(),
                    title = ChatLinkPreviewParsing.lastPathComponent(url),
                    imageUrl = finalUrl.toString(),
                    kind = ChatLinkKind.Image,
                )
            }
            if (type.isNotEmpty() && "html" !in type && "xml" !in type) return fallback
            var parsed = ChatLinkPreviewParsing.parseHtml(RssCharsets.decode(response.body, type), finalUrl)
            if (fallback != null) {
                parsed = parsed.copy(
                    title = parsed.title.ifEmpty { fallback.title },
                    imageUrl = parsed.imageUrl.ifEmpty { fallback.imageUrl },
                    author = parsed.author.ifEmpty { fallback.author },
                    detail = parsed.detail.ifEmpty { fallback.detail },
                )
            }
            val finalSite = ChatLinkSites.site(finalUrl)
            if (finalSite != null && ChatLinkSites.site(url) == null) parsed = parsed.copy(siteKey = finalSite.key)
            return parsed
        }

        private fun json(raw: String, http: ChatLinkHttp): JsonMap? {
            val url = ChatLinkPreviewParsing.uri(raw) ?: return null
            val response = fetch(url, http, "application/json", MAX_JSON_BYTES) ?: return null
            if (!response.ok) return null
            val value = try {
                Json.parse(String(response.body, Charsets.UTF_8).trim())
            } catch (_: Exception) {
                null
            }
            return value.asJsonMap().ifEmpty { null }
        }

        fun encoded(url: String): String {
            val out = StringBuilder()
            for (byte in url.toByteArray(Charsets.UTF_8)) {
                val code = byte.toInt() and 0xff
                val character = code.toChar()
                if (character in 'a'..'z' || character in 'A'..'Z' || character in '0'..'9' || character in "-._~") {
                    out.append(character)
                } else {
                    out.append('%').append("0123456789ABCDEF"[code shr 4]).append("0123456789ABCDEF"[code and 0xf])
                }
            }
            return out.toString()
        }

        private fun clip(text: String, limit: Int): String = ChatLinkPreviewParsing.truncate(text, limit)

        private fun special(site: ChatLinkSite, url: URI, http: ChatLinkHttp): ChatLinkPreview? {
            val link = url.toString()
            val maxTitle = ChatLinkPreviewParsing.MAX_TITLE
            val maxDetail = ChatLinkPreviewParsing.MAX_DETAIL
            return when (val resolver = site.resolver) {
                ChatLinkResolver.Page -> null
                ChatLinkResolver.YouTube -> {
                    val id = ChatLinkPreviewParsing.youtubeId(url) ?: return null
                    val canonical = "https://www.youtube.com/watch?v=$id"
                    val found = json("https://www.youtube.com/oembed?format=json&url=${encoded(canonical)}", http)
                    ChatLinkPreview(
                        url = link,
                        title = clip(found.str("title"), maxTitle),
                        author = clip(found.str("author_name"), 80),
                        siteName = site.name,
                        siteKey = site.key,
                        imageUrl = "https://i.ytimg.com/vi/$id/hqdefault.jpg",
                        kind = ChatLinkKind.Video,
                    )
                }
                is ChatLinkResolver.OEmbed -> {
                    val found = json(resolver.endpoint + encoded(link), http) ?: return null
                    var image = found.firstStr("thumbnail_url", "url")
                    if (found.str("type") != "photo" && image == found.str("url") && found.str("thumbnail_url").isEmpty()) image = ""
                    ChatLinkPreview(
                        url = link,
                        title = clip(found.str("title"), maxTitle),
                        detail = clip(found.str("description"), maxDetail),
                        author = clip(found.str("author_name"), 80),
                        siteName = found.str("provider_name").ifEmpty { site.name },
                        siteKey = site.key,
                        imageUrl = ChatLinkPreviewParsing.resolve(image, url),
                        kind = if (found.str("type") == "photo") ChatLinkKind.Image else site.kind,
                    )
                }
                ChatLinkResolver.Tweet -> {
                    val path = url.rawPath.orEmpty()
                    if ("/status/" !in path) return null
                    val canonical = "https://twitter.com$path"
                    val found = json("https://publish.twitter.com/oembed?omit_script=1&dnt=true&url=${encoded(canonical)}", http) ?: return null
                    ChatLinkPreview(
                        url = link,
                        title = clip(ChatLinkPreviewParsing.tweetText(found.str("html")), maxDetail),
                        author = clip(found.str("author_name"), 80),
                        siteName = site.name,
                        siteKey = site.key,
                        kind = ChatLinkKind.Social,
                    )
                }
                ChatLinkResolver.Wikipedia -> {
                    val (host, title) = ChatLinkPreviewParsing.wikipediaPage(url) ?: return null
                    val found = json("https://$host/api/rest_v1/page/summary/$title", http) ?: return null
                    ChatLinkPreview(
                        url = link,
                        title = clip(found.str("title"), maxTitle),
                        detail = clip(found.str("extract"), maxDetail),
                        author = clip(found.str("description"), 80),
                        siteName = site.name,
                        siteKey = site.key,
                        imageUrl = ChatLinkPreviewParsing.resolve(found.obj("thumbnail").str("source"), url),
                        kind = ChatLinkKind.Reference,
                    )
                }
                ChatLinkResolver.HackerNews -> {
                    val id = ChatLinkPreviewParsing.hackerNewsId(url) ?: return null
                    val found = json("https://hacker-news.firebaseio.com/v0/item/$id.json", http) ?: return null
                    val points = found.int("score")
                    val comments = found.int("descendants")
                    val parts = ArrayList<String>()
                    if (points > 0) parts.add("$points ${if (points == 1) "point" else "points"}")
                    if (found.str("by").isNotEmpty()) parts.add("by ${found.str("by")}")
                    if (found.containsKey("descendants")) parts.add("$comments ${if (comments == 1) "comment" else "comments"}")
                    val text = found.str("title").ifEmpty { ChatLinkPreviewParsing.stripTags(found.str("text")) }
                    ChatLinkPreview(
                        url = link,
                        title = clip(text, maxTitle),
                        detail = parts.joinToString(" · "),
                        siteName = site.name,
                        siteKey = site.key,
                        kind = ChatLinkKind.Social,
                    )
                }
                ChatLinkResolver.Npm -> {
                    val name = ChatLinkPreviewParsing.packageName(url, "package") ?: return null
                    val found = json("https://registry.npmjs.org/${name.replace("/", "%2F")}/latest", http) ?: return null
                    ChatLinkPreview(
                        url = link,
                        title = "${found.str("name")} ${found.str("version")}".trim(),
                        detail = clip(found.str("description"), maxDetail),
                        siteName = site.name,
                        siteKey = site.key,
                        kind = ChatLinkKind.Package,
                    )
                }
                ChatLinkResolver.PyPI -> {
                    val name = ChatLinkPreviewParsing.packageName(url, "project") ?: return null
                    val found = json("https://pypi.org/pypi/$name/json", http) ?: return null
                    val info = found.obj("info")
                    ChatLinkPreview(
                        url = link,
                        title = "${info.str("name")} ${info.str("version")}".trim(),
                        detail = clip(info.str("summary"), maxDetail),
                        author = clip(info.str("author"), 80),
                        siteName = site.name,
                        siteKey = site.key,
                        kind = ChatLinkKind.Package,
                    )
                }
                ChatLinkResolver.Crates -> {
                    val name = ChatLinkPreviewParsing.packageName(url, "crates") ?: return null
                    val found = json("https://crates.io/api/v1/crates/$name", http) ?: return null
                    val crate = found.obj("crate")
                    ChatLinkPreview(
                        url = link,
                        title = "${crate.str("name")} ${crate.firstStr("max_stable_version", "max_version")}".trim(),
                        detail = clip(crate.str("description"), maxDetail),
                        siteName = site.name,
                        siteKey = site.key,
                        kind = ChatLinkKind.Package,
                    )
                }
                ChatLinkResolver.AppStore -> {
                    val id = ChatLinkPreviewParsing.appStoreId(url) ?: return null
                    val found = json("https://itunes.apple.com/lookup?id=$id", http) ?: return null
                    val result = found.maps("results").firstOrNull() ?: return null
                    ChatLinkPreview(
                        url = link,
                        title = clip(result.firstStr("trackName", "collectionName"), maxTitle),
                        detail = clip(result.str("description"), maxDetail),
                        author = clip(result.firstStr("sellerName", "artistName"), 80),
                        siteName = site.name,
                        siteKey = site.key,
                        imageUrl = ChatLinkPreviewParsing.resolve(result.firstStr("artworkUrl512", "artworkUrl100"), url),
                        kind = ChatLinkKind.App,
                    )
                }
                ChatLinkResolver.Steam -> {
                    val id = ChatLinkPreviewParsing.steamAppId(url) ?: return null
                    val found = json("https://store.steampowered.com/api/appdetails?appids=$id", http) ?: return null
                    val data = found.obj(id).obj("data")
                    if (data.isEmpty()) return null
                    ChatLinkPreview(
                        url = link,
                        title = clip(data.str("name"), maxTitle),
                        detail = clip(ChatLinkPreviewParsing.decodeEntities(data.str("short_description")), maxDetail),
                        author = clip(data.strings("developers").joinToString(", "), 80),
                        siteName = site.name,
                        siteKey = site.key,
                        imageUrl = ChatLinkPreviewParsing.resolve(data.str("header_image"), url),
                        kind = ChatLinkKind.App,
                    )
                }
                ChatLinkResolver.StackExchange -> {
                    val (siteName, id) = ChatLinkPreviewParsing.stackExchangeQuestion(url) ?: return null
                    val kind = if (url.path.orEmpty().startsWith("/a/")) "answers" else "questions"
                    val found = json("https://api.stackexchange.com/2.3/$kind/$id?site=$siteName&filter=default", http) ?: return null
                    val item = found.maps("items").firstOrNull() ?: return null
                    val parts = ArrayList<String>()
                    val score = item.int("score")
                    parts.add("$score ${if (score == 1) "vote" else "votes"}")
                    if (item.containsKey("answer_count")) {
                        val answers = item.int("answer_count")
                        parts.add("$answers ${if (answers == 1) "answer" else "answers"}")
                    }
                    if (item.bool("is_answered")) parts.add("answered")
                    ChatLinkPreview(
                        url = link,
                        title = clip(ChatLinkPreviewParsing.decodeEntities(item.str("title")), maxTitle),
                        detail = parts.joinToString(" · "),
                        author = clip(ChatLinkPreviewParsing.decodeEntities(item.obj("owner").str("display_name")), 80),
                        siteName = site.name,
                        siteKey = site.key,
                        kind = ChatLinkKind.Reference,
                    )
                }
                ChatLinkResolver.Xkcd -> {
                    val number = ChatLinkPreviewParsing.xkcdNumber(url) ?: return null
                    val found = json(if (number.isEmpty()) "https://xkcd.com/info.0.json" else "https://xkcd.com/$number/info.0.json", http) ?: return null
                    ChatLinkPreview(
                        url = link,
                        title = clip(found.firstStr("safe_title", "title"), maxTitle),
                        detail = clip(found.str("alt"), maxDetail),
                        siteName = site.name,
                        siteKey = site.key,
                        imageUrl = ChatLinkPreviewParsing.resolve(found.str("img"), url),
                        kind = ChatLinkKind.Image,
                    )
                }
            }
        }
    }

    private val http: ChatLinkHttp = ChatLinkGuardedHttp(http, resolver)
    private val lock = Any()
    private val listeners = CopyOnWriteArrayList<() -> Unit>()
    private val states = HashMap<String, ChatLinkPreviewState>()
    private val order = ArrayList<String>()
    private val misses = HashMap<String, Long>()
    private val inflight = HashMap<String, ChatLinkPending<ChatLinkPreview>>()
    private val images = LinkedHashMap<String, ByteArray>()
    private var imageBytes = 0L
    private val imageTasks = HashMap<String, ChatLinkPending<ByteArray>>()
    private var file: File? = null
    private var warmed = false
    private var saveQueued = false
    private var generation = 0

    @Volatile
    var enabled: Boolean = prefs?.getBool(ENABLED_KEY) ?: true
        private set

    fun addListener(listener: () -> Unit) {
        listeners.add(listener)
    }

    fun removeListener(listener: () -> Unit) {
        listeners.remove(listener)
    }

    private fun changed() {
        for (listener in listeners) runCatching { listener() }
    }

    fun setEnabled(on: Boolean) {
        enabled = on
        prefs?.putBool(ENABLED_KEY, on)
        changed()
    }

    fun state(url: String): ChatLinkPreviewState = synchronized(lock) { states[url] ?: ChatLinkPreviewState.Idle }

    fun state(url: URI): ChatLinkPreviewState = state(url.toString())

    fun preview(url: URI): ChatLinkPreview? = (state(url) as? ChatLinkPreviewState.Ready)?.preview

    fun load(url: URI) {
        if (!enabled) return
        warm()
        val key = url.toString()
        val pending: ChatLinkPending<ChatLinkPreview>
        val started: Int
        var waiting: ChatLinkPending<ChatLinkPreview>? = null
        var marked = false
        synchronized(lock) {
            val clock = now()
            when (val cached = states[key]) {
                is ChatLinkPreviewState.Ready -> if (clock - cached.preview.fetchedAtMs < FRESH_MS) return
                ChatLinkPreviewState.None -> if (clock - (misses[key] ?: Long.MIN_VALUE / 2) < MISS_MS) return
                else -> Unit
            }
            waiting = inflight[key]
            if (waiting == null) {
                if (states[key] == null) {
                    states[key] = ChatLinkPreviewState.Loading
                    marked = true
                }
                pending = ChatLinkPending()
                inflight[key] = pending
                started = generation
            } else {
                pending = ChatLinkPending()
                started = -1
            }
        }
        waiting?.let {
            it.await()
            return
        }
        if (marked) changed()
        val result = try {
            resolve(url, http, now())
        } catch (_: Exception) {
            null
        }
        pending.complete(result)
        var save = false
        synchronized(lock) {
            if (started != generation) return
            inflight.remove(key)
            if (result != null && !result.isEmpty) {
                remember(key, ChatLinkPreviewState.Ready(result))
                save = true
            } else if (states[key] !is ChatLinkPreviewState.Ready) {
                misses[key] = now()
                remember(key, ChatLinkPreviewState.None)
            }
        }
        if (save) scheduleSave()
        changed()
    }

    fun imageData(raw: String): ByteArray? {
        if (!enabled) return null
        val url = ChatLinkPreviewParsing.previewable(raw) ?: return null
        val key = url.toString()
        val pending = ChatLinkPending<ByteArray>()
        val started: Int
        val waiting: ChatLinkPending<ByteArray>?
        synchronized(lock) {
            images[key]?.let { return it }
            waiting = imageTasks[key]
            started = generation
            if (waiting == null) imageTasks[key] = pending
        }
        if (waiting != null) return waiting.await()
        val data = try {
            fetchImage(url)
        } catch (_: Exception) {
            null
        }
        pending.complete(data)
        synchronized(lock) {
            if (started != generation) return data
            imageTasks.remove(key)
            if (data != null) storeImage(data, key)
        }
        return data
    }

    private fun fetchImage(url: URI): ByteArray? {
        val response = fetch(url, http, "image/*", MAX_IMAGE_BYTES) ?: return null
        if (!response.ok) return null
        val type = response.header("content-type").orEmpty().lowercase()
        if (type.isNotEmpty() && !type.startsWith("image/") && !type.startsWith("application/octet-stream")) return null
        return response.body.takeIf { it.isNotEmpty() }
    }

    private fun storeImage(data: ByteArray, key: String) {
        images.remove(key)?.let { imageBytes -= it.size }
        images[key] = data
        imageBytes += data.size
        val iterator = images.entries.iterator()
        while (imageBytes > MAX_IMAGE_CACHE_BYTES && iterator.hasNext()) {
            imageBytes -= iterator.next().value.size
            iterator.remove()
        }
    }

    private fun remember(key: String, state: ChatLinkPreviewState) {
        states[key] = state
        order.remove(key)
        order.add(key)
        while (order.size > MAX_ENTRIES) states.remove(order.removeAt(0))
    }

    fun warm() {
        var loaded = false
        synchronized(lock) {
            if (warmed) return
            warmed = true
            val target = fileSource() ?: return
            file = target
            val clock = now()
            for (preview in read(target)) {
                if (states.containsKey(preview.url) || clock - preview.fetchedAtMs >= FRESH_MS) continue
                states[preview.url] = ChatLinkPreviewState.Ready(preview)
                order.add(preview.url)
                loaded = true
            }
        }
        if (loaded) changed()
    }

    private fun scheduleSave() {
        val started: Int
        synchronized(lock) {
            if (file == null || saveQueued) return
            saveQueued = true
            started = generation
        }
        saveLater(Runnable {
            synchronized(lock) { saveQueued = false }
            if (started == generation) flush()
        })
    }

    private fun snapshot(): List<ChatLinkPreview> =
        order.mapNotNull { (states[it] as? ChatLinkPreviewState.Ready)?.preview }

    fun flush() {
        val target: File
        val previews: List<ChatLinkPreview>
        synchronized(lock) {
            target = file ?: return
            previews = snapshot()
        }
        write(previews, target)
    }

    fun leaveProject() {
        var target: File? = null
        var previews: List<ChatLinkPreview> = emptyList()
        synchronized(lock) {
            if (warmed) {
                target = file
                previews = snapshot()
            }
            generation++
            states.clear()
            order.clear()
            misses.clear()
            inflight.clear()
            images.clear()
            imageBytes = 0
            imageTasks.clear()
            warmed = false
            saveQueued = false
            file = null
        }
        target?.let { write(previews, it) }
        changed()
    }
}
