package com.forkmesh.nativeapp.feature.djstudio

import com.forkmesh.nativeapp.djstudio.DjCrate
import com.forkmesh.nativeapp.djstudio.DjDeckSide
import com.forkmesh.nativeapp.djstudio.DjFocus
import com.forkmesh.nativeapp.djstudio.DjMove
import com.forkmesh.nativeapp.djstudio.DjSimilarSong
import com.forkmesh.nativeapp.djstudio.DjSmart
import com.forkmesh.nativeapp.djstudio.DjStudio
import com.forkmesh.nativeapp.djstudio.DjTrack

fun DjStudioModel.song(id: String): DjTrack {
    crate.track(id)?.let { return it }
    library.firstOrNull { it.id == id }?.let { return DjTrack.of(id, it.title, it.author) }
    history.firstOrNull { it.id == id }?.let { return DjTrack.of(id, it.title, it.author) }
    listOf(a, b).firstOrNull { it.videoId == id }?.let { return DjTrack.of(id, it.title, it.author) }
    shownQueue.firstOrNull { it.videoId == id }?.let { return DjTrack.of(id, it.title, it.author) }
    searchResults.firstOrNull { it.id == id }?.let { return DjTrack.of(id, it.title, it.author) }
    similarResults.firstOrNull { it.id == id }?.let { return DjTrack.of(id, it.title, it.author) }
    return DjTrack.of(id)
}

val DjStudioModel.librarySongs: List<DjTrack>
    get() {
        val kept = crate.tracks.associateBy { it.id }
        val played = library.map { it.id }.toSet()
        return library.map { kept[it.id] ?: DjTrack.of(it.id, it.title, it.author) } + crate.tracks.filter { it.id !in played }.reversed()
    }

fun DjStudioModel.similarSongs(side: DjDeckSide): List<DjSimilarSong> {
    val deck = deck(side)
    if (!deck.loaded) return emptyList()
    val loaded = setOf(a.videoId, b.videoId)
    return DjSmart.similar(song(deck.videoId), librarySongs.filter { it.id !in loaded }, crate.playlists)
}

fun DjStudioModel.toggleShuffle(id: String) {
    val track = song(id)
    val on = !crate.shuffles(id)
    var ok = true
    updateCrate { ok = it.setShuffles(id, on, track.title, track.author) }
    if (!ok) {
        show("The library is full: remove a song first.")
        return
    }
    val name = track.title.ifEmpty { "This song" }
    show(if (on) "$name is back in the shuffle." else "$name will not come up in shuffle.")
}

fun DjStudioModel.saveSmart(id: String) {
    val list = crate.smartList(id) ?: return
    newPlaylist(list.name, list.songs.map { it.id })
}

fun DjStudioModel.editSong(id: String) {
    if (DjStudio.isVideoId(id)) sheet = DjSheet.Song(id)
}

fun DjStudioModel.addTags(raw: String, id: String) {
    val known = song(id)
    if (DjCrate.tagNames(raw).isEmpty()) return
    var ok = true
    updateCrate { ok = it.tag(id, raw, known.title, known.author) }
    if (!ok) show("The library is full: remove a song to tag another.")
}

fun DjStudioModel.toggleTag(name: String, id: String) {
    if (crate.track(id)?.tags?.contains(name) == true) updateCrate { it.untag(id, name) } else addTags(name, id)
}

fun DjStudioModel.renameTag(old: String, raw: String) {
    updateCrate { it.renameTag(old, raw) }
}

fun DjStudioModel.deleteTag(name: String) {
    updateCrate { it.deleteTag(name) }
    show("Removed the #$name tag.")
}

fun DjStudioModel.inPlaylist(id: String, playlist: String): Boolean = crate.playlist(playlist)?.songs?.contains(id) == true

fun DjStudioModel.toggleInPlaylist(id: String, playlist: String) {
    val list = crate.playlist(playlist) ?: return
    if (id in list.songs) {
        updateCrate { it.remove(id, playlist) }
        show("Removed from ${list.name}.")
        return
    }
    var added = 0
    updateCrate { added = it.add(listOf(song(id)), playlist) }
    show(if (added > 0) "Added to ${list.name}." else if (list.songs.size >= DjCrate.PLAYLIST_SONG_LIMIT) "${list.name} is full." else "The library is full: remove a song first.")
}

fun DjStudioModel.newPlaylist(name: String, ids: List<String> = emptyList()): String? {
    var id: String? = null
    val songs = ids.map { song(it) }
    updateCrate {
        id = it.createPlaylist(name)
        id?.let { made -> it.add(songs, made) }
    }
    val made = id
    if (made == null) {
        show("You already have ${DjCrate.PLAYLIST_LIMIT} playlists.")
        return null
    }
    show("Created ${crate.label(DjFocus.Playlist(made))}.")
    return made
}

fun DjStudioModel.renamePlaylist(id: String, name: String) = updateCrate { it.renamePlaylist(id, name) }

fun DjStudioModel.deletePlaylist(id: String) {
    val name = crate.label(DjFocus.Playlist(id))
    updateCrate { it.deletePlaylist(id) }
    show("Deleted $name.")
}

fun DjStudioModel.removeFromPlaylist(index: Int, playlist: String) = updateCrate { it.removeSongs(setOf(index), playlist) }

fun DjStudioModel.moveInPlaylist(playlist: String, from: Int, to: Int) = updateCrate { it.moveSongs(playlist, setOf(from), to) }

fun DjStudioModel.removeSong(id: String) {
    updateCrate { it.removeTrack(id) }
    forgetPlayed(id)
}

fun DjStudioModel.addLink(text: String, playlist: String): Boolean {
    val id = DjStudio.videoId(text)
    if (id.isEmpty()) return false
    if (!inPlaylist(id, playlist)) toggleInPlaylist(id, playlist)
    return true
}

fun DjStudioModel.move(side: DjDeckSide, target: DjFocus) {
    val deck = deck(side)
    if (!deck.loaded) return
    val id = deck.videoId
    val track = song(id)
    val from = crate.focus
    val fromLabel = crate.label(from)
    var result = DjMove.ADDED
    updateCrate(saving = false) { result = it.move(id, track.title, track.author, target) }
    if (result == DjMove.FULL) {
        show("${crate.label(target)} or the library is full.")
        return
    }
    saveCrate()
    val done = when (result) {
        DjMove.REMOVED -> "Took it out of $fromLabel"
        DjMove.MOVED -> "Moved from $fromLabel to ${crate.label(target)}"
        else -> "Added to ${crate.label(target)}"
    }
    if (!automixOn || (result != DjMove.MOVED && result != DjMove.REMOVED) || crate.songs(from).any { it.id == id }) {
        show("$done.")
        return
    }
    show(if (skipOut(deck)) "$done. Skipping to the next song." else "$done. Nothing else in $fromLabel to play.")
}

fun DjStudioModel.setShuffle(on: Boolean) = updateCrate { it.shuffle = on }

fun DjStudioModel.focusAutomix(focus: DjFocus, start: Boolean) {
    updateCrate { it.focusOn(focus) }
    if (start && !automixOn) setAutomix(true)
    refocusAutomix()
    val label = if (crate.focus == DjFocus.Everything) "everything you have played and saved" else crate.label(crate.focus) + if (crate.shuffle) ", shuffled" else " in order"
    show(if (automixOn) "AUTOMIX plays $label." else "AUTOMIX will play $label.")
}

fun DjStudioModel.queueAll(focus: DjFocus, shuffled: Boolean) {
    val songs = crate.songs(focus).let { if (shuffled) it.shuffled() else it }
    enqueueAll(songs)
}

fun DjStudioModel.loadTrack(track: DjTrack, side: DjDeckSide) {
    load(side, track.id, track.title, track.author)
    sheet = null
}

fun DjStudioModel.importData(data: ByteArray, name: String) {
    val found = DjCrate.parse(data)
    if (found == null) {
        show("No YouTube songs were found to import.")
        return
    }
    var summary = ""
    updateCrate { summary = it.merge(found, name.ifEmpty { "Imported" }).summary }
    show(summary)
}
