package com.forkmesh.nativeapp.djstudio

enum class DjSmartKind(val title: String) {
    LISTENING("Listening"), GENRE("Genres"), STYLE("Styles"), DECADE("Decades"), ARTIST("Artists")
}

data class DjSmartList(val id: String, val name: String, val icon: String, val kind: DjSmartKind, val songs: List<DjTrack>)

data class DjSimilarSong(val track: DjTrack, val score: Int, val reasons: List<String>) {
    val id: String get() = track.id
}

object DjSmart {
    const val MINIMUM_SONGS = 2
    const val LISTENING_LIMIT = 50
    const val ARTIST_LIMIT = 30
    const val REDISCOVER_AFTER = 30.0 * 24 * 60 * 60

    private class Rule(val id: String, val name: String, val icon: String, val words: List<String>)

    private val genres = listOf(
        Rule("house", "House", "house", listOf("house", "deep house", "tech house", "afro house", "progressive house")),
        Rule("techno", "Techno", "graphic_eq", listOf("techno", "hard techno", "minimal")),
        Rule("trance", "Trance", "auto_awesome", listOf("trance", "psytrance", "uplifting")),
        Rule("dnb", "Drum & Bass", "timer", listOf("drum & bass", "drum and bass", "drum n bass", "dnb", "d&b", "jungle")),
        Rule("dubstep", "Dubstep & Bass", "speaker", listOf("dubstep", "riddim", "bass music", "brostep", "future bass")),
        Rule("edm", "EDM & Electro", "bolt", listOf("edm", "big room", "electro", "electro house", "festival", "hardstyle")),
        Rule("garage", "UK Garage & Breaks", "layers", listOf("garage", "ukg", "2 step", "breakbeat", "breaks")),
        Rule("hiphop", "Hip-Hop & Rap", "mic", listOf("hip hop", "hiphop", "rap", "trap", "drill", "boom bap", "grime")),
        Rule("rnb", "R&B & Soul", "favorite", listOf("r&b", "rnb", "soul", "neo soul", "motown")),
        Rule("pop", "Pop", "star", listOf("pop", "synthpop", "dance pop", "europop")),
        Rule("kpop", "K-Pop", "stars", listOf("k pop", "kpop")),
        Rule("rock", "Rock & Metal", "electric_bolt", listOf("rock", "metal", "punk", "grunge", "emo", "alternative")),
        Rule("chill", "Chill & Lo-fi", "bedtime", listOf("lofi", "lo fi", "chill", "chillhop", "chillout", "ambient", "relax", "relaxing", "study", "sleep")),
        Rule("latin", "Latin", "wb_sunny", listOf("reggaeton", "latin", "bachata", "salsa", "cumbia", "dembow", "merengue", "latino")),
        Rule("afro", "Afrobeats & Amapiano", "public", listOf("afrobeats", "afrobeat", "amapiano", "afro")),
        Rule("reggae", "Reggae & Dancehall", "eco", listOf("reggae", "dancehall", "ska", "dub")),
        Rule("disco", "Disco & Funk", "blur_circular", listOf("disco", "nu disco", "funk", "funky", "boogie")),
        Rule("jazz", "Jazz & Blues", "music_note", listOf("jazz", "blues", "swing", "bossa nova")),
        Rule("classical", "Classical", "piano", listOf("classical", "symphony", "sonata", "concerto", "orchestra", "beethoven", "mozart", "bach", "chopin")),
        Rule("country", "Country & Folk", "mic_external_on", listOf("country", "folk", "bluegrass", "americana")),
    )

    private val styles = listOf(
        Rule("remix", "Remixes & Edits", "tune", listOf("remix", "edit", "bootleg", "flip", "vip", "rework", "mashup", "extended mix", "club mix")),
        Rule("sets", "DJ Sets & Mixes", "album", listOf("dj set", "live set", "boiler room", "full set", "mixtape", "megamix", "essential mix", "b2b")),
        Rule("live", "Live Performances", "mic", listOf("live", "concert", "tiny desk", "session", "unplugged")),
        Rule("acoustic", "Acoustic & Covers", "queue_music", listOf("acoustic", "cover", "piano version")),
        Rule("speed", "Sped Up & Slowed", "speed", listOf("sped up", "slowed", "nightcore", "reverb")),
        Rule("videos", "Official Videos", "smart_display", listOf("official video", "official music video", "music video")),
        Rule("lyrics", "Lyric Videos", "format_quote", listOf("lyrics", "lyric video")),
    )

    fun text(raw: String): String {
        val spaced = raw.lowercase().map { if (it.isLetterOrDigit() || it == '&') it else ' ' }.joinToString("")
        return " " + spaced.split(' ').filter { it.isNotEmpty() }.joinToString(" ") + " "
    }

    private fun haystack(track: DjTrack): String = text((listOf(track.title) + track.tags).joinToString(" "))

    private fun matches(rule: Rule, haystack: String): Boolean = rule.words.any { haystack.contains(" $it ") }

    fun artist(track: DjTrack): String {
        var name = track.author.trim()
        for (suffix in listOf(" - Topic", "VEVO", " Official", " Music")) {
            if (name.length > suffix.length && name.lowercase().endsWith(suffix.lowercase())) name = name.dropLast(suffix.length).trim()
        }
        if (name.isEmpty()) {
            val dash = track.title.indexOf(" - ")
            if (dash >= 0) {
                val lead = track.title.substring(0, dash).trim()
                if (lead.length <= 40) name = lead
            }
        }
        return name
    }

    fun artistKey(track: DjTrack): String = text(artist(track)).trim()

    fun decade(track: DjTrack): Int? {
        for (word in haystack(track).split(' ')) {
            if (word.isEmpty()) continue
            val year = if (word.length == 4) word.toIntOrNull() else null
            if (year != null && year in 1950..2029) return year / 10 * 10
            when (word) {
                "50s" -> return 1950
                "60s" -> return 1960
                "70s" -> return 1970
                "80s" -> return 1980
                "90s" -> return 1990
                "00s", "2000s" -> return 2000
                "2010s" -> return 2010
                "2020s" -> return 2020
            }
        }
        return null
    }

    fun categories(track: DjTrack): List<String> {
        val words = haystack(track)
        val found = genres.filter { matches(it, words) }.map { "genre:${it.id}" }.toMutableList()
        found += styles.filter { matches(it, words) }.map { "style:${it.id}" }
        decade(track)?.let { found.add("decade:$it") }
        return found
    }

    fun lists(all: List<DjTrack>, library: List<DjPlayedSong>, now: Double): List<DjSmartList> {
        val seen = mutableSetOf<String>()
        val songs = all.filter { DjStudio.isVideoId(it.id) && seen.add(it.id) }
        val known = songs.associateBy { it.id }
        val played = library.filter { known[it.id] != null }
        val plays = mutableMapOf<String, Int>()
        for (song in played) plays[song.id] = maxOf(plays[song.id] ?: 0, song.plays)
        val result = mutableListOf<DjSmartList>()

        fun add(id: String, name: String, icon: String, kind: DjSmartKind, tracks: List<DjTrack>) {
            if (tracks.size < MINIMUM_SONGS) return
            result.add(DjSmartList(id, name, icon, kind, tracks))
        }

        val most = played.withIndex().filter { it.value.plays > 1 }
            .sortedWith(compareByDescending<IndexedValue<DjPlayedSong>> { it.value.plays }.thenBy { it.index })
        add("listen:most", "Most Played", "local_fire_department", DjSmartKind.LISTENING, most.take(LISTENING_LIMIT).mapNotNull { known[it.value.id] })
        val recent = played.withIndex().filter { it.value.plays > 0 }
            .sortedWith(compareByDescending<IndexedValue<DjPlayedSong>> { it.value.last }.thenBy { it.index })
        add("listen:recent", "Recently Played", "schedule", DjSmartKind.LISTENING, recent.take(LISTENING_LIMIT).mapNotNull { known[it.value.id] })
        add("listen:new", "Never Played", "new_releases", DjSmartKind.LISTENING, songs.filter { (plays[it.id] ?: 0) == 0 })
        add("listen:once", "Played Once", "looks_one", DjSmartKind.LISTENING, songs.filter { plays[it.id] == 1 })
        val rediscover = played.filter { it.plays > 0 && it.last > 0 && now - it.last > REDISCOVER_AFTER }
        add("listen:rediscover", "Rediscover", "history", DjSmartKind.LISTENING, rediscover.mapNotNull { known[it.id] })

        val tagged = songs.map { it to categories(it).toSet() }
        for (rule in genres) add("genre:${rule.id}", rule.name, rule.icon, DjSmartKind.GENRE, tagged.filter { "genre:${rule.id}" in it.second }.map { it.first })
        for (rule in styles) add("style:${rule.id}", rule.name, rule.icon, DjSmartKind.STYLE, tagged.filter { "style:${rule.id}" in it.second }.map { it.first })
        val decades = tagged.flatMap { pair -> pair.second.filter { it.startsWith("decade:") } }.toSet().mapNotNull { it.drop(7).toIntOrNull() }.sorted()
        for (decade in decades) add("decade:$decade", "${decade}s", "calendar_month", DjSmartKind.DECADE, tagged.filter { "decade:$decade" in it.second }.map { it.first })

        val artists = linkedMapOf<String, MutableList<DjTrack>>()
        val names = mutableMapOf<String, MutableMap<String, Int>>()
        for (song in songs) {
            val key = artistKey(song)
            if (key.isEmpty()) continue
            artists.getOrPut(key) { mutableListOf() }.add(song)
            val spelled = names.getOrPut(key) { mutableMapOf() }
            val name = artist(song)
            spelled[name] = (spelled[name] ?: 0) + 1
        }
        val ranked = artists.keys.withIndex()
            .filter { (artists[it.value]?.size ?: 0) >= MINIMUM_SONGS }
            .sortedWith(compareByDescending<IndexedValue<String>> { artists[it.value]?.size ?: 0 }.thenBy { it.index })
            .take(ARTIST_LIMIT)
        for (entry in ranked) {
            val key = entry.value
            val spelled = names[key]?.entries?.maxWithOrNull(compareBy<Map.Entry<String, Int>> { it.value }.thenByDescending { it.key })?.key ?: key
            add("artist:$key", spelled, "person", DjSmartKind.ARTIST, artists[key] ?: emptyList())
        }
        return result
    }

    fun similar(seed: DjTrack, among: List<DjTrack>, playlists: List<DjPlaylist> = emptyList(), limit: Int = 25): List<DjSimilarSong> {
        val artist = artistKey(seed)
        val tags = seed.tags.toSet()
        val lists = playlists.filter { seed.id in it.songs }
        val kinds = categories(seed).toSet()
        val labels = (genres.map { "genre:${it.id}" to it.name } + styles.map { "style:${it.id}" to it.name }).toMap()
        val seen = mutableSetOf(seed.id)
        val found = mutableListOf<Pair<Int, DjSimilarSong>>()
        for ((offset, song) in among.withIndex()) {
            if (!seen.add(song.id)) continue
            var score = 0
            val reasons = mutableListOf<String>()
            if (artist.isNotEmpty() && artistKey(song) == artist) {
                score += 5
                reasons.add("Same artist")
            }
            for (tag in song.tags.filter { it in tags }) {
                score += 3
                reasons.add("#$tag")
            }
            for (list in lists) {
                if (song.id !in list.songs) continue
                score += 2
                reasons.add(list.name)
            }
            for (kind in categories(song)) {
                if (kind !in kinds) continue
                when {
                    kind.startsWith("genre:") -> {
                        score += 2
                        reasons.add(labels[kind] ?: kind)
                    }
                    kind.startsWith("style:") -> {
                        score += 1
                        reasons.add(labels[kind] ?: kind)
                    }
                    kind.startsWith("decade:") -> {
                        score += 1
                        reasons.add("${kind.drop(7)}s")
                    }
                }
            }
            if (score <= 0) continue
            found.add(offset to DjSimilarSong(song, score, reasons))
        }
        return found.sortedWith(compareByDescending<Pair<Int, DjSimilarSong>> { it.second.score }.thenBy { it.first }).take(limit).map { it.second }
    }

    fun searchQuery(seed: DjTrack): String {
        val artist = artist(seed)
        val genre = genres.firstOrNull { matches(it, haystack(seed)) }?.name
        if (artist.isNotEmpty()) return listOf(artist, genre ?: "mix").joinToString(" ")
        var title = seed.title
        for ((open, close) in listOf("(" to ")", "[" to "]")) {
            while (true) {
                val start = title.indexOf(open)
                if (start < 0) break
                val end = title.indexOf(close, start + 1)
                if (end < 0) break
                title = title.removeRange(start, end + 1)
            }
        }
        return title.splitWhere { it.isWhitespace() }.take(6).joinToString(" ")
    }
}
