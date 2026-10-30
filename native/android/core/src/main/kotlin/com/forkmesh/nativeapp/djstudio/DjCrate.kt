package com.forkmesh.nativeapp.djstudio

import com.forkmesh.nativeapp.json.Json
import java.time.Instant
import java.time.temporal.ChronoUnit
import java.util.UUID

data class DjTrack(
    val id: String,
    val title: String = "",
    val author: String = "",
    val tags: List<String> = emptyList(),
    val noShuffle: Boolean = false,
) {
    val thumbnail: String get() = DjStudio.thumbnailUrl(id, "mqdefault")
    val shownTitle: String get() = title.ifEmpty { "YouTube video" }

    fun toJson(): Map<String, Any?> {
        val out = linkedMapOf<String, Any?>("author" to author, "id" to id)
        if (noShuffle) out["noShuffle"] = true
        out["tags"] = tags
        out["title"] = title
        return out
    }

    companion object {
        fun of(id: String, title: String = "", author: String = "", tags: List<String> = emptyList(), noShuffle: Boolean = false) =
            DjTrack(id, DjCrate.line(title, 300), DjCrate.line(author, 200), DjCrate.tagList(tags), noShuffle)

        fun parse(value: Any?): DjTrack? {
            val row = value as? Map<*, *> ?: return null
            val id = row["id"] as? String ?: return null
            if (!DjStudio.isVideoId(id)) return null
            val tags = (row["tags"] as? List<*>)?.let { list -> if (list.all { it is String }) list.map { it as String } else emptyList() } ?: emptyList()
            return of(id, row["title"] as? String ?: "", row["author"] as? String ?: "", tags, row["noShuffle"] == true)
        }
    }
}

data class DjPlaylist(val id: String, val name: String, val songs: List<String> = emptyList()) {
    fun toJson(): Map<String, Any?> = linkedMapOf("id" to id, "name" to name, "songs" to songs)

    companion object {
        fun of(id: String, name: String, songs: List<String> = emptyList()): DjPlaylist {
            val seen = mutableSetOf<String>()
            return DjPlaylist(
                id.take(64), DjCrate.playlistName(name),
                songs.filter { DjStudio.isVideoId(it) && seen.add(it) }.take(DjCrate.PLAYLIST_SONG_LIMIT),
            )
        }

        fun parse(value: Any?): DjPlaylist? {
            val row = value as? Map<*, *> ?: return null
            val id = row["id"] as? String ?: ""
            val songs = (row["songs"] as? List<*>)?.let { list -> if (list.all { it is String }) list.map { it as String } else emptyList() } ?: emptyList()
            return of(id.ifEmpty { UUID.randomUUID().toString() }, row["name"] as? String ?: "", songs)
        }
    }
}

sealed interface DjFocus {
    data object Everything : DjFocus
    data class Playlist(val id: String) : DjFocus
    data class Tag(val name: String) : DjFocus
    data class Smart(val id: String) : DjFocus

    val key: String
        get() = when (this) {
            Everything -> ""
            is Playlist -> "playlist:$id"
            is Tag -> "tag:$name"
            is Smart -> "smart:$id"
        }

    val isSmart: Boolean get() = this is Smart
    val isFiled: Boolean get() = this is Playlist || this is Tag

    companion object {
        fun of(key: String): DjFocus = when {
            key.startsWith("smart:") -> Smart(key.drop(6))
            key.startsWith("playlist:") -> Playlist(key.drop(9))
            key.startsWith("tag:") -> DjCrate.tagName(key.drop(4)).let { if (it.isEmpty()) Everything else Tag(it) }
            else -> Everything
        }
    }
}

data class DjTagCount(val name: String, val count: Int)

data class DjImportedList(val name: String, val songs: List<String>) {
    companion object {
        fun of(name: String, songs: List<String>) = DjImportedList(DjCrate.playlistName(name), songs)
    }
}

data class DjCrateImport(val tracks: List<DjTrack>, val playlists: List<DjImportedList> = emptyList())

data class DjMergeReport(val songs: Int = 0, val playlists: Int = 0, val full: Boolean = false) {
    val summary: String
        get() {
            val songs = if (songs == 1) "1 new song" else "$songs new songs"
            val lists = if (playlists == 0) "" else if (playlists == 1) " into 1 playlist" else " into $playlists playlists"
            return "Imported $songs$lists." + if (full) " Some were left out: the library is full." else ""
        }
}

enum class DjMove { FULL, ADDED, MOVED, REMOVED }

class DjCrate {
    companion object {
        const val STORAGE_KEY = "fm.videoMixDjStudio.crate.v1"
        const val FORMAT = "forkmesh-dj-playlists"
        const val TRACK_LIMIT = 2000
        const val PLAYLIST_LIMIT = 100
        const val PLAYLIST_SONG_LIMIT = 1000
        const val TAG_LIMIT = 20
        const val TAG_LENGTH = 32
        const val NAME_LENGTH = 80
        const val IMPORT_LIMIT = 4_000_000
        private val NEWLINES = setOf(0x0A, 0x0B, 0x0C, 0x0D, 0x85, 0x2028, 0x2029)

        fun line(text: String, limit: Int): String =
            text.splitWhere { it.code in NEWLINES }.joinToString(" ").take(limit)

        fun tagName(raw: String): String {
            val words = raw.lowercase().splitWhere { it.isWhitespace() || it == ',' || it == '#' }
            return words.joinToString(" ").take(TAG_LENGTH).trim()
        }

        fun tagNames(raw: String): List<String> = tagList(raw.split(','))

        fun tagList(names: List<String>): List<String> {
            val seen = mutableSetOf<String>()
            return names.map(::tagName).filter { it.isNotEmpty() && seen.add(it) }.take(TAG_LIMIT)
        }

        fun playlistName(raw: String): String = raw.splitWhere { it.isWhitespace() }.joinToString(" ").take(NAME_LENGTH)

        fun decode(text: String?): DjCrate {
            val crate = DjCrate()
            if (text.isNullOrEmpty()) return crate
            val root = runCatching { Json.parse(text) }.getOrNull() as? Map<*, *> ?: return crate
            val seen = mutableSetOf<String>()
            crate.tracks = (root["tracks"] as? List<*> ?: emptyList<Any?>()).mapNotNull(DjTrack::parse).filter { seen.add(it.id) }.take(TRACK_LIMIT)
            val ids = mutableSetOf<String>()
            crate.playlists = (root["playlists"] as? List<*> ?: emptyList<Any?>()).mapNotNull(DjPlaylist::parse)
                .filter { ids.add(it.id) }.take(PLAYLIST_LIMIT).map { list -> list.copy(songs = list.songs.filter { it in seen }) }
            crate.shuffle = root["shuffle"] as? Boolean ?: true
            crate.cursor = root["cursor"] as? String ?: ""
            crate.focus = DjFocus.of(root["focus"] as? String ?: "")
            if (!crate.focus.isSmart && !crate.knows(crate.focus)) crate.focus = DjFocus.Everything
            return crate
        }

        fun parse(data: ByteArray): DjCrateImport? {
            if (data.isEmpty() || data.size > IMPORT_LIMIT) return null
            val text = runCatching { String(data, Charsets.UTF_8) }.getOrNull() ?: return null
            val json = runCatching { Json.parse(text.trim()) }.getOrNull()
            if (json != null && json !is String) return parseJson(json)
            return parseText(text)
        }

        fun parseText(text: String): DjCrateImport? {
            val tracks = mutableListOf<DjTrack>()
            var title = ""
            for (raw in text.split('\n', '\r')) {
                if (raw.isEmpty()) continue
                val line = raw.trim()
                if (line.uppercase().startsWith("#EXTINF")) {
                    val comma = line.indexOf(',')
                    title = if (comma >= 0) line.substring(comma + 1).trim() else ""
                    continue
                }
                if (line.startsWith("#")) continue
                val id = DjStudio.videoId(line)
                if (id.isNotEmpty()) tracks.add(DjTrack.of(id, title))
                title = ""
            }
            return built(tracks, emptyList())
        }

        private fun parseJson(json: Any): DjCrateImport? {
            if (json is List<*>) return built(json.mapNotNull(::song), emptyList())
            val root = json as? Map<*, *> ?: return null
            val songsOf = { row: Map<*, *> ->
                ((row["tracks"] ?: row["songs"] ?: row["videos"] ?: row["items"]) as? List<*> ?: emptyList<Any?>()).mapNotNull(::song)
            }
            val tracks = songsOf(root).toMutableList()
            val lists = mutableListOf<DjImportedList>()
            for (entry in root["playlists"] as? List<*> ?: emptyList<Any?>()) {
                val list = entry as? Map<*, *> ?: continue
                val songs = ((list["songs"] ?: list["tracks"] ?: list["videos"] ?: list["items"]) as? List<*> ?: emptyList<Any?>()).mapNotNull(::song)
                tracks += songs
                lists.add(DjImportedList.of((list["name"] ?: list["title"]) as? String ?: "", songs.map { it.id }))
            }
            val name = (root["name"] ?: root["title"]) as? String
            if (root["playlists"] == null && root["format"] as? String != FORMAT && name != null && tracks.isNotEmpty()) {
                lists.add(DjImportedList.of(name, tracks.map { it.id }))
            }
            return built(tracks, lists)
        }

        private fun song(value: Any?): DjTrack? {
            if (value is String) {
                val id = DjStudio.videoId(value)
                return if (id.isEmpty()) null else DjTrack.of(id)
            }
            val row = value as? Map<*, *> ?: return null
            val id = listOf("videoId", "id", "url", "link", "href").asSequence()
                .mapNotNull { row[it] as? String }.map(DjStudio::videoId).firstOrNull { it.isNotEmpty() } ?: return null
            val tags = (row["tags"] as? List<*>)?.mapNotNull { it as? String } ?: (row["tags"] as? String)?.let(::tagNames) ?: emptyList()
            return DjTrack.of(
                id, (row["title"] ?: row["name"]) as? String ?: "",
                (row["author"] ?: row["artist"] ?: row["channel"]) as? String ?: "", tags, row["noShuffle"] as? Boolean ?: false,
            )
        }

        private fun built(found: List<DjTrack>, lists: List<DjImportedList>): DjCrateImport? {
            val order = mutableListOf<String>()
            val merged = mutableMapOf<String, DjTrack>()
            for (track in found) {
                val known = merged[track.id]
                if (known == null) {
                    order.add(track.id)
                    merged[track.id] = track
                    continue
                }
                merged[track.id] = known.copy(
                    title = known.title.ifEmpty { track.title },
                    author = known.author.ifEmpty { track.author },
                    tags = tagList(known.tags + track.tags),
                    noShuffle = known.noShuffle || track.noShuffle,
                )
            }
            if (order.isEmpty()) return null
            return DjCrateImport(order.take(TRACK_LIMIT).mapNotNull { merged[it] }, lists.take(PLAYLIST_LIMIT))
        }
    }

    var tracks: List<DjTrack> = emptyList()
    var playlists: List<DjPlaylist> = emptyList()
    var focus: DjFocus = DjFocus.Everything
    var shuffle = true
    var cursor = ""
    var smart: List<DjSmartList> = emptyList()

    override fun equals(other: Any?): Boolean = other is DjCrate && tracks == other.tracks && playlists == other.playlists &&
        focus == other.focus && shuffle == other.shuffle && cursor == other.cursor && smart == other.smart

    override fun hashCode(): Int = listOf(tracks, playlists, focus, shuffle, cursor, smart).hashCode()

    fun copy(): DjCrate = DjCrate().also {
        it.tracks = tracks
        it.playlists = playlists
        it.focus = focus
        it.shuffle = shuffle
        it.cursor = cursor
        it.smart = smart
    }

    fun encoded(): String = Json.stringify(
        linkedMapOf(
            "tracks" to tracks.map { it.toJson() }, "playlists" to playlists.map { it.toJson() },
            "focus" to focus.key, "shuffle" to shuffle, "cursor" to cursor,
        ),
    )

    fun track(id: String): DjTrack? = tracks.firstOrNull { it.id == id }

    fun playlist(id: String): DjPlaylist? = playlists.firstOrNull { it.id == id }

    fun smartList(id: String): DjSmartList? = smart.firstOrNull { it.id == id }

    fun shuffles(id: String): Boolean = track(id)?.noShuffle != true

    val unshuffled: Set<String> get() = tracks.filter { it.noShuffle }.map { it.id }.toSet()

    fun categorize(songs: List<DjTrack>, library: List<DjPlayedSong>, now: Double) {
        smart = DjSmart.lists(songs, library, now)
        if (focus.isSmart && !knows(focus)) focusOn(DjFocus.Everything)
    }

    private fun update(id: String, change: (DjTrack) -> DjTrack) {
        tracks = tracks.map { if (it.id == id) change(it) else it }
    }

    fun setShuffles(id: String, on: Boolean, title: String = "", author: String = ""): Boolean {
        if (on) {
            if (track(id) == null) return true
            update(id) { it.copy(noShuffle = false) }
            return true
        }
        if (!keep(id, title, author) || track(id) == null) return false
        update(id) { it.copy(noShuffle = true) }
        return true
    }

    fun knows(focus: DjFocus): Boolean = when (focus) {
        DjFocus.Everything -> true
        is DjFocus.Playlist -> playlist(focus.id) != null
        is DjFocus.Tag -> tracks.any { focus.name in it.tags }
        is DjFocus.Smart -> smartList(focus.id) != null
    }

    val tagCounts: List<DjTagCount>
        get() {
            val counts = mutableMapOf<String, Int>()
            for (track in tracks) for (tag in track.tags) counts[tag] = (counts[tag] ?: 0) + 1
            return counts.map { DjTagCount(it.key, it.value) }.sortedBy { it.name }
        }

    fun label(focus: DjFocus): String = when (focus) {
        DjFocus.Everything -> "Everything"
        is DjFocus.Playlist -> playlist(focus.id)?.name ?: "Playlist"
        is DjFocus.Tag -> "#${focus.name}"
        is DjFocus.Smart -> smartList(focus.id)?.name ?: "Smart Playlist"
    }

    fun songs(focus: DjFocus): List<DjTrack> = when (focus) {
        DjFocus.Everything -> tracks
        is DjFocus.Playlist -> {
            val known = tracks.associateBy { it.id }
            (playlist(focus.id)?.songs ?: emptyList()).mapNotNull { known[it] }
        }
        is DjFocus.Tag -> tracks.filter { focus.name in it.tags }
        is DjFocus.Smart -> {
            val known = tracks.associateBy { it.id }
            (smartList(focus.id)?.songs ?: emptyList()).map { known[it.id] ?: it }
        }
    }

    fun keep(id: String, title: String = "", author: String = ""): Boolean {
        if (!DjStudio.isVideoId(id)) return false
        if (track(id) != null) {
            update(id) {
                it.copy(
                    title = if (title.isNotEmpty()) line(title, 300) else it.title,
                    author = if (author.isNotEmpty()) line(author, 200) else it.author,
                )
            }
            return true
        }
        if (tracks.size >= TRACK_LIMIT) return false
        tracks = tracks + DjTrack.of(id, title, author)
        return true
    }

    fun tag(id: String, raw: String, title: String = "", author: String = ""): Boolean {
        val names = tagNames(raw)
        if (names.isEmpty() || !keep(id, title, author) || track(id) == null) return false
        update(id) { it.copy(tags = tagList(it.tags + names)) }
        return true
    }

    fun untag(id: String, name: String) {
        if (track(id) == null) return
        update(id) { it.copy(tags = it.tags.filter { tag -> tag != name }) }
        if (focus == DjFocus.Tag(name) && !knows(focus)) focusOn(DjFocus.Everything)
    }

    fun renameTag(old: String, raw: String): Boolean {
        val name = tagName(raw)
        if (name.isEmpty()) return false
        if (name == old) return true
        tracks = tracks.map { track -> if (old in track.tags) track.copy(tags = tagList(track.tags.map { if (it == old) name else it })) else track }
        if (focus == DjFocus.Tag(old)) focus = DjFocus.Tag(name)
        return true
    }

    fun deleteTag(name: String) {
        tracks = tracks.map { track -> track.copy(tags = track.tags.filter { it != name }) }
        if (focus == DjFocus.Tag(name)) focusOn(DjFocus.Everything)
    }

    fun uniqueName(raw: String, except: String? = null): String {
        val base = playlistName(raw).ifEmpty { "New Playlist" }
        val taken = playlists.filter { it.id != except }.map { it.name.lowercase() }.toSet()
        if (base.lowercase() !in taken) return base
        var number = 2
        while ("$base $number".lowercase() in taken) number += 1
        return "$base $number"
    }

    fun createPlaylist(raw: String, id: String = UUID.randomUUID().toString()): String? {
        if (playlists.size >= PLAYLIST_LIMIT) return null
        playlists = playlists + DjPlaylist.of(id, uniqueName(raw))
        return id
    }

    fun renamePlaylist(id: String, raw: String) {
        if (playlist(id) == null || playlistName(raw).isEmpty()) return
        val name = uniqueName(raw, id)
        playlists = playlists.map { if (it.id == id) it.copy(name = name) else it }
    }

    fun deletePlaylist(id: String) {
        playlists = playlists.filter { it.id != id }
        if (focus == DjFocus.Playlist(id)) focusOn(DjFocus.Everything)
    }

    private fun updateList(id: String, change: (List<String>) -> List<String>) {
        playlists = playlists.map { if (it.id == id) it.copy(songs = change(it.songs)) else it }
    }

    fun add(songs: List<DjTrack>, to: String): Int {
        if (playlist(to) == null) return 0
        var added = 0
        for (song in songs) {
            val current = playlist(to)?.songs ?: return added
            if (current.size >= PLAYLIST_SONG_LIMIT || song.id in current) continue
            if (!keep(song.id, song.title, song.author)) continue
            updateList(to) { it + song.id }
            added += 1
        }
        return added
    }

    fun remove(song: String, from: String) {
        if (playlist(from) == null) return
        updateList(from) { list -> list.filter { it != song } }
    }

    fun removeSongs(offsets: Set<Int>, from: String) {
        if (playlist(from) == null) return
        updateList(from) { list -> list.filterIndexed { index, _ -> index !in offsets } }
    }

    fun moveSongs(id: String, from: Set<Int>, to: Int) {
        val songs = playlist(id)?.songs ?: return
        val moving = from.filter { it in songs.indices }.sorted().map { songs[it] }
        val rest = songs.filterIndexed { index, _ -> index !in from }.toMutableList()
        val place = to - from.count { it < to }
        rest.addAll(place.coerceIn(0, rest.size), moving)
        updateList(id) { rest }
    }

    fun removeTrack(id: String) {
        tracks = tracks.filter { it.id != id }
        playlists = playlists.map { list -> list.copy(songs = list.songs.filter { it != id }) }
        if (cursor == id) cursor = ""
        if (!knows(focus)) focusOn(DjFocus.Everything)
    }

    fun move(id: String, title: String = "", author: String = "", to: DjFocus): DjMove {
        when (to) {
            is DjFocus.Playlist -> if (playlist(to.id)?.songs?.contains(id) != true && add(listOf(DjTrack.of(id, title, author)), to.id) == 0) return DjMove.FULL
            is DjFocus.Tag -> if (!tag(id, to.name, title, author)) return DjMove.FULL
            else -> Unit
        }
        val from = focus
        if (from == to || !from.isFiled) return DjMove.ADDED
        var left = false
        when (from) {
            is DjFocus.Playlist -> {
                left = playlist(from.id)?.songs?.contains(id) == true
                remove(id, from.id)
            }
            is DjFocus.Tag -> {
                left = track(id)?.tags?.contains(from.name) == true
                untag(id, from.name)
            }
            else -> Unit
        }
        return if (to == DjFocus.Everything) DjMove.REMOVED else if (left) DjMove.MOVED else DjMove.ADDED
    }

    fun focusOn(focus: DjFocus) {
        this.focus = if (knows(focus)) focus else DjFocus.Everything
        cursor = ""
    }

    fun pool(library: List<DjPlayedSong>, skip: Set<String> = emptySet()): List<DjPlayedSong> {
        val played = library.associateBy { it.id }
        val songs = when (focus) {
            DjFocus.Everything -> library + tracks.filter { played[it.id] == null }.map { DjPlayedSong.of(it.id, it.title, it.author, 0) }
            else -> songs(focus).map { track ->
                val song = played[track.id] ?: return@map DjPlayedSong.of(track.id, track.title, track.author, 0)
                song.copy(title = song.title.ifEmpty { track.title }, author = song.author.ifEmpty { track.author })
            }
        }
        val held = if (focus == DjFocus.Everything || shuffle) unshuffled else emptySet()
        return songs.filter { it.id !in skip && it.id !in held }
    }

    fun next(library: List<DjPlayedSong>, loaded: Set<String>, playing: Set<String>, skip: Set<String> = emptySet(), seed: Long): DjPlayedSong? {
        val pool = pool(library, skip)
        if (pool.isEmpty()) return null
        val pick = if (focus == DjFocus.Everything || shuffle) {
            DjAutomix.next(pool, loaded, playing, seed)
        } else {
            val found = pool.indexOfFirst { it.id == cursor }
            val start = if (found >= 0) found + 1 else 0
            val order = pool.indices.map { pool[(start + it) % pool.size] }
            order.firstOrNull { it.id !in loaded && it.id !in playing } ?: order.firstOrNull { it.id !in playing }
        }
        if (pick != null) cursor = pick.id
        return pick
    }

    fun exported(scope: DjFocus, at: Instant = Instant.now()): String {
        val lists = when (scope) {
            DjFocus.Everything -> playlists
            is DjFocus.Playlist -> listOfNotNull(playlist(scope.id))
            is DjFocus.Tag -> emptyList()
            is DjFocus.Smart -> listOfNotNull(smartList(scope.id)?.let { DjPlaylist.of(it.id, it.name, it.songs.map { song -> song.id }) })
        }
        val file = linkedMapOf<String, Any?>(
            "exported" to at.truncatedTo(ChronoUnit.SECONDS).toString(),
            "format" to FORMAT,
            "playlists" to lists.map { linkedMapOf("name" to it.name, "songs" to it.songs) },
            "tracks" to songs(scope).map { it.toJson() },
            "version" to 1,
        )
        return Json.stringify(file)
    }

    fun m3u(scope: DjFocus): String =
        (listOf("#EXTM3U") + songs(scope).flatMap { listOf("#EXTINF:-1,${it.shownTitle}", DjStudio.watchUrl(it.id)) }).joinToString("\n") + "\n"

    fun fileName(scope: DjFocus): String {
        val label = when (scope) {
            DjFocus.Everything -> "DJ Playlists"
            is DjFocus.Playlist -> playlist(scope.id)?.name ?: "Playlist"
            is DjFocus.Tag -> "Tag ${scope.name}"
            is DjFocus.Smart -> smartList(scope.id)?.name ?: "Smart Playlist"
        }
        val safe = label.map { if (it.isLetterOrDigit() || it in " -_") it else '-' }.joinToString("").trim()
        return safe.ifEmpty { "DJ Playlist" }
    }

    fun merge(incoming: DjCrateImport, fallback: String): DjMergeReport {
        var songs = 0
        var lists = 0
        var full = false
        for (track in incoming.tracks) {
            val known = track(track.id)
            if (known != null) {
                update(track.id) {
                    it.copy(
                        title = it.title.ifEmpty { track.title },
                        author = it.author.ifEmpty { track.author },
                        tags = tagList(it.tags + track.tags),
                        noShuffle = it.noShuffle || track.noShuffle,
                    )
                }
            } else if (tracks.size < TRACK_LIMIT) {
                tracks = tracks + track
                songs += 1
            } else {
                full = true
            }
        }
        var imported = incoming.playlists
        if (imported.isEmpty() && incoming.tracks.none { it.tags.isNotEmpty() }) {
            imported = listOf(DjImportedList.of(fallback, incoming.tracks.map { it.id }))
        }
        for (list in imported) {
            val name = list.name.ifEmpty { playlistName(fallback) }
            val found = list.songs.mapNotNull { track(it) }
            val id = playlists.firstOrNull { it.name.lowercase() == name.lowercase() }?.id ?: createPlaylist(name)
            if (id == null) {
                full = true
                continue
            }
            add(found, id)
            lists += 1
        }
        return DjMergeReport(songs, lists, full)
    }
}
