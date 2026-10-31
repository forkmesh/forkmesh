package com.forkmesh.nativeapp.feature.djstudio

import androidx.compose.runtime.Composable
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.djstudio.DjAutoDj
import com.forkmesh.nativeapp.djstudio.DjAutoDjCandidate
import com.forkmesh.nativeapp.djstudio.DjDeckSide
import com.forkmesh.nativeapp.djstudio.DjLikes
import com.forkmesh.nativeapp.djstudio.DjSmart
import com.forkmesh.nativeapp.djstudio.DjStudio
import com.forkmesh.nativeapp.djstudio.DjTrack
import com.forkmesh.nativeapp.net.HttpClient
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

fun DjStudioModel.autoDj() {
    val wasMixing = automixOn
    setAutoDj(!autoDjOn)
    show(
        when {
            !autoDjOn -> if (automixOn) "AUTO DJ off. AUTOMIX keeps playing your queue and library." else "AUTO DJ off."
            a.playing || b.playing -> "AUTO DJ on: when this song ends, a related song people liked plays on the next deck."
            wasMixing -> "AUTO DJ on: related songs people liked follow each song."
            else -> "AUTO DJ on: start a song and related songs people liked follow it."
        },
    )
}

fun DjStudioModel.setAutoDj(on: Boolean, refresh: Boolean = true) {
    if (on == autoDjOn) return
    if (!on) {
        val owned = autoDjOwnsAutomix
        stopAutoDj()
        if (owned && automixOn) setAutomix(false)
        save()
        return
    }
    autoDjOn = true
    autoDjRefresh = refresh
    if (!automixOn) {
        setAutomix(true)
        autoDjOwnsAutomix = true
    }
    primeAutoDj()
    save()
}

fun DjStudioModel.stopAutoDj() {
    autoDjJob?.cancel()
    autoDjJob = null
    autoDjOn = false
    autoDjFinding = false
    autoDjSeed = ""
    autoDjChoices = emptyList()
    autoDjOwnsAutomix = false
    autoDjRefresh = false
}

internal val DjStudioModel.autoDjSeedTrack: DjTrack?
    get() {
        val playing = listOf(a, b).filter { it.playing && it.loaded }
        val live = if (playing.size > 1) deck(if (crossfader <= 0) DjDeckSide.A else DjDeckSide.B) else playing.firstOrNull()
        if (live != null) return song(live.videoId)
        return history.firstOrNull()?.let { song(it.id) }
    }

internal fun DjStudioModel.primeAutoDj() {
    if (!autoDjOn) return
    val seed = autoDjSeedTrack ?: return
    if (seed.id == autoDjSeed) return
    findAutoDj(seed)
}

internal fun DjStudioModel.autoDjNext(): DjAutoDjCandidate? =
    DjAutoDj.next(autoDjChoices, listOf(a, b).filter { it.loaded }.map { it.videoId }.toSet(), unplayable.toSet(), autoDjPlayed)

internal fun DjStudioModel.takeAutoDj(): DjAutoDjCandidate? {
    val pick = autoDjNext() ?: return null
    autoDjPlayed = DjAutoDj.remember(pick.id, autoDjPlayed)
    return pick
}

private fun DjStudioModel.findAutoDj(seed: DjTrack) {
    autoDjJob?.cancel()
    autoDjSeed = seed.id
    autoDjChoices = emptyList()
    autoDjFinding = true
    val decks = listOf(a, b).filter { it.loaded }
    val avoid = (history.map { it.id } + decks.map { it.videoId } + autoDjPlayed).toSet() + unplayable
    val heard = history.map { it.title } + decks.map { it.title }
    autoDjJob = scope.launch {
        var instance: String? = null
        var fresh = emptyList<DjAutoDjCandidate>()
        invidiousData { DjStudio.similarUrl(it, seed.id) }?.let { (text, from) ->
            instance = from
            fresh = DjAutoDj.eligible(DjAutoDj.candidates(text), seed, avoid, heard)
        }
        val query = DjSmart.searchQuery(seed)
        if (fresh.isEmpty() && isActive && query.length >= 2) {
            invidiousData { DjStudio.searchUrl(it, query) }?.let { (text, from) ->
                instance = from
                fresh = DjAutoDj.eligible(DjAutoDj.candidates(text), seed, avoid, heard)
            }
        }
        val source = instance
        if (source != null && fresh.isNotEmpty() && isActive) {
            val top = fresh.take(DjAutoDj.CHECKED)
            val liked = coroutineScope {
                top.map { candidate -> async { djLikes(candidate.id, source)?.let { candidate.id to it } } }.awaitAll()
            }.filterNotNull().toMap()
            fresh = fresh.map { candidate ->
                val count = liked[candidate.id] ?: return@map candidate
                candidate.copy(likes = count.likes, views = if (count.views > 0) count.views else candidate.views)
            }
            fresh = DjAutoDj.ranked(fresh.take(DjAutoDj.CHECKED)) + DjAutoDj.ranked(fresh.drop(DjAutoDj.CHECKED))
        }
        if (!isActive || !autoDjOn || autoDjSeed != seed.id) return@launch
        autoDjChoices = fresh
        autoDjFinding = false
        if (fresh.isEmpty()) {
            show(
                if (source == null) "AUTO DJ can’t reach YouTube suggestions right now, so AUTOMIX plays your library."
                else "AUTO DJ found nothing new like this song, so AUTOMIX plays your library.",
            )
        }
        refreshAutoDj()
    }
}

private fun DjStudioModel.refreshAutoDj() {
    if (!autoDjRefresh) return
    autoDjRefresh = false
    if (!automixOn || automixing || shownQueue.isNotEmpty() || autoDjNext() == null) return
    val playing = listOf(a, b).filter { it.playing }
    val live = playing.singleOrNull() ?: return
    val idle = deck(live.side.other)
    if (idle.scratching || idle.keepsPlaying || idle.repeats) return
    preload(idle.side)
}

private suspend fun djLikes(id: String, instance: String): DjLikes? = withContext(Dispatchers.IO) {
    val url = DjAutoDj.likesUrl(instance, id) ?: return@withContext null
    runCatching {
        val response = HttpClient.Request.get(url).timeouts(6_000, 6_000).send()
        if (response.ok) DjAutoDj.likes(response.text()) else null
    }.getOrNull()
}

@Composable
internal fun DjAutoDjButton(model: DjStudioModel, u: Float) {
    val on = model.autoDjOn
    val label = "Auto DJ " + when {
        !on -> "off"
        model.autoDjFinding -> "on, finding the next song"
        else -> "on"
    }
    DjRoundButton((26 * u).dp, if (model.autoDjFinding) "more_horiz" else "auto_awesome", label, lit = on) { model.autoDj() }
}

@Composable
internal fun DjRepeatButton(model: DjStudioModel, deck: DjDeck, u: Float) {
    DjRoundButton((26 * u).dp, "repeat", "Repeat Deck ${deck.side.label} ${if (deck.repeats) "on" else "off"}", lit = deck.repeats) {
        model.toggleRepeat(deck.side)
    }
}
