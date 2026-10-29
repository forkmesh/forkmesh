package com.forkmesh.nativeapp.feature.djstudio

import android.os.SystemClock
import com.forkmesh.nativeapp.djstudio.DjDeckSide
import com.forkmesh.nativeapp.djstudio.DjJam
import com.forkmesh.nativeapp.djstudio.DjJamApi
import com.forkmesh.nativeapp.djstudio.DjJamDeck
import com.forkmesh.nativeapp.djstudio.DjJamMember
import com.forkmesh.nativeapp.djstudio.DjJamPresence
import com.forkmesh.nativeapp.djstudio.DjJamReaction
import com.forkmesh.nativeapp.djstudio.DjJamSummary
import com.forkmesh.nativeapp.djstudio.DjPersonStatus
import com.forkmesh.nativeapp.djstudio.DjPresenceState
import com.forkmesh.nativeapp.djstudio.DjQueueItem
import com.forkmesh.nativeapp.djstudio.DjReaction
import com.forkmesh.nativeapp.djstudio.DjStudio
import com.forkmesh.nativeapp.djstudio.DjTrack
import com.forkmesh.nativeapp.feature.market.MarketFeature
import com.forkmesh.nativeapp.json.JsonMap
import com.forkmesh.nativeapp.market.PackagesException
import com.forkmesh.nativeapp.platform.AppLifecycle
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlin.math.abs
import kotlin.math.max
import kotlin.math.roundToLong
import com.forkmesh.nativeapp.djstudio.DjAutomix

val DjStudioModel.shownQueue: List<DjQueueItem>
    get() = jam?.takeIf { it.joined }?.queue ?: queue

val DjStudioModel.inJam: Boolean get() = jam?.joined == true

val DjStudioModel.pendingInvites: Int get() = jams.count { it.invited }

internal suspend fun DjStudioModel.jamCall(block: (DjJamApi) -> JsonMap): JsonMap? {
    val client = MarketFeature.client()
    if (client == null) {
        jamProblem = DjJamApi.problem("invalid_session")
        return null
    }
    val result = withContext(Dispatchers.IO) { runCatching { block(DjJamApi(client)) } }
    return result.fold(
        onSuccess = {
            jamProblem = ""
            it
        },
        onFailure = { error ->
            val code = (error as? PackagesException)?.code ?: "network"
            if (code == "jam_not_found" && jam != null) {
                clearJam()
                show("That jam has ended.")
            }
            jamProblem = DjJamApi.problem(code)
            null
        },
    )
}

fun DjStudioModel.enqueue(id: String, title: String, author: String) {
    if (playNow(id, title, author)) return
    val current = jam
    if (current != null && current.joined) {
        show(if (title.isEmpty()) "Added to the jam queue." else "Queued for the jam: $title")
        scope.launch { receive(jamCall { it.enqueue(current.id, id, title, author) }) }
        return
    }
    enqueueLocal(id, title, author)
}

fun DjStudioModel.enqueueAll(songs: List<DjTrack>) {
    val first = songs.firstOrNull()
    if (first == null) {
        show("There are no songs to queue.")
        return
    }
    if (playNow(first.id, first.title, first.author)) {
        if (songs.size > 1) enqueueAll(songs.drop(1))
        return
    }
    val current = jam
    if (current != null && current.joined) {
        val picked = songs.take(max(0, DjStudio.QUEUE_LIMIT - current.queue.size))
        show(if (picked.isEmpty()) "The jam queue is full." else "Queueing ${picked.size} songs for the jam.")
        scope.launch {
            for (song in picked) {
                val reply = jamCall { it.enqueue(current.id, song.id, song.title, song.author) }
                receive(reply)
                if (reply == null) break
            }
        }
        return
    }
    val room = max(0, DjStudio.QUEUE_LIMIT - queue.size)
    val now = System.currentTimeMillis()
    val added = songs.take(room).map { DjQueueItem.of(java.util.UUID.randomUUID().toString(), it.id, it.title, it.author, addedAt = now) }
    queue = queue + added
    show(
        when {
            added.isEmpty() -> "The queue is full. Play or remove a video first."
            added.size < songs.size -> "Queued ${added.size} songs: the queue is full."
            added.size == 1 -> "Queued 1 song."
            else -> "Queued ${added.size} songs."
        },
    )
    save()
}

private fun DjStudioModel.playNow(id: String, title: String, author: String): Boolean {
    if (automixing || a.scratching || b.scratching || a.keepsPlaying || b.keepsPlaying) return false
    val side = DjAutomix.playNow(snapshot(a), snapshot(b), crossfader) ?: return false
    load(side, id, title, author)
    play(deck(side))
    return true
}

fun DjStudioModel.playFromQueue(item: DjQueueItem, side: DjDeckSide) {
    load(side, item.videoId, item.title, item.author)
    removeFromQueue(item)
}

fun DjStudioModel.removeFromQueue(item: DjQueueItem) {
    val current = jam
    if (current != null && current.joined) {
        jam = current.copy(queue = current.queue.filter { it.id != item.id })
        scope.launch { receive(jamCall { it.dequeue(current.id, item.id) }) }
        return
    }
    queue = queue.filter { it.id != item.id }
    save()
}

internal fun DjStudioModel.share(side: DjDeckSide) {
    val current = jam
    if (applyingRemote || current == null || !current.joined) return
    shareJobs[side]?.cancel()
    shareJobs[side] = scope.launch {
        delay(250)
        val reply = jamCall { it.deck(current.id, side, deckState(side)) }
        reply?.let { DjJam.parse(it) }?.decks?.get(side)?.let { mine -> applied[side] = max(applied[side] ?: 0, mine.at) }
        shareJobs.remove(side)
        receive(reply)
    }
}

internal fun DjStudioModel.deckState(side: DjDeckSide): DjJamDeck {
    val deck = deck(side)
    return DjJamDeck(deck.videoId, deck.title, deck.author, deck.playing, deck.position(), deck.rate)
}

fun DjStudioModel.refreshJams() {
    scope.launch { jamCall { api -> mapOf("jams" to api.list()) }?.let { reply -> jams = (reply["jams"] as? List<*>).orEmpty().filterIsInstance<DjJamSummary>() } }
}

fun DjStudioModel.resumeJam() {
    refreshJams()
    val id = prefs?.getString(DjStudioModel.JAM_KEY).orEmpty()
    if (jam != null || id.isEmpty()) {
        if (inJam) startPolling()
        return
    }
    openJam(id)
}

fun DjStudioModel.openJam(id: String) {
    scope.launch {
        jamBusy = true
        receive(jamCall { it.open(id) })
        jamBusy = false
    }
}

fun DjStudioModel.startJam() {
    scope.launch {
        jamBusy = true
        val decks = LinkedHashMap<String, Any?>()
        for (side in DjDeckSide.entries) if (deck(side).loaded) decks[side.key] = deckState(side).body
        receive(jamCall { it.create(decks) })
        jamBusy = false
        if (inJam) show("Jam started. Invite people to share the decks and the queue.")
    }
}

fun DjStudioModel.joinJam(id: String) {
    scope.launch {
        jamBusy = true
        applied.clear()
        receive(jamCall { it.join(id) })
        jamBusy = false
        if (inJam) show("You joined the jam. The decks now follow the session.")
        refreshJams()
    }
}

fun DjStudioModel.leaveJam() {
    val current = jam ?: return
    scope.launch {
        jamBusy = true
        val reply = jamCall { it.leave(current.id, current.isOwner) }
        jamBusy = false
        if (reply == null) return@launch
        clearJam()
        show(if (current.isOwner) "Jam ended." else "You left the jam.")
        refreshJams()
    }
}

fun DjStudioModel.invite(names: List<String>) {
    val current = jam ?: return
    if (names.isEmpty()) return
    scope.launch {
        jamBusy = true
        val reply = jamCall { it.invite(current.id, names) }
        jamBusy = false
        receive(reply)
        val invited = (reply?.get("invited") as? List<*>).orEmpty().filterIsInstance<String>()
        val skipped = (reply?.get("skipped") as? List<*>).orEmpty().filterIsInstance<String>()
        if (skipped.isNotEmpty()) {
            jamProblem = "No ForkMesh account for ${skipped.joinToString(", ")}."
        } else if (invited.isNotEmpty()) {
            inviteText = ""
            inviteSuggestions = emptyList()
            show("Invited ${invited.joinToString(", ")}.")
        }
    }
}

fun DjStudioModel.removeMember(name: String) {
    val current = jam ?: return
    scope.launch { receive(jamCall { it.remove(current.id, name) }) }
}

fun DjStudioModel.suggestPeople(text: String) {
    suggestJob?.cancel()
    val query = text.trim().lowercase().replace("@", "")
    if (query.length < 2) {
        inviteSuggestions = emptyList()
        return
    }
    suggestJob = scope.launch {
        delay(250)
        val reply = jamCall { it.people(query) } ?: return@launch
        val taken = (jam?.members.orEmpty().map { it.name } + listOfNotNull(jam?.me)).toSet()
        inviteSuggestions = DjJam.userNames(reply, taken).take(8)
    }
}

internal fun DjStudioModel.receive(reply: JsonMap?, latency: Double = 0.0) {
    if (reply == null) return
    val fresh = DjJam.parse(reply)
    if (fresh != null) adopt(fresh, latency) else if (reply.containsKey("jam") && reply["jam"] == null) clearJam()
}

fun DjStudioModel.clearJam() {
    jamJob?.cancel()
    jamJob = null
    shareJobs.values.forEach { it.cancel() }
    shareJobs.clear()
    applied.clear()
    jam = null
    crowd = emptyList()
    seenReactions.clear()
    motion.stop()
    prefs?.remove(DjStudioModel.JAM_KEY)
}

private fun DjStudioModel.adopt(fresh: DjJam, latency: Double) {
    val continuing = jam?.id == fresh.id && jam?.joined == true
    if (!continuing) {
        applied.clear()
        seenReactions = fresh.reactions.map { it.id }.toMutableList()
    }
    if (jam?.shared != fresh.shared) jam = fresh
    if (crowd != fresh.presence) crowd = fresh.presence
    prefs?.putString(DjStudioModel.JAM_KEY, fresh.id)
    if (!fresh.joined) return
    if (continuing) hear(fresh.reactions, fresh.me)
    val timed = fresh.copy(now = fresh.now + (latency * 1000).roundToLong())
    val wasPlaying = anyPlaying
    for (side in DjDeckSide.entries) follow(side, timed)
    if (wasPlaying) holdAutomix()
    startPolling()
}

private fun DjStudioModel.hear(reactions: List<DjJamReaction>, me: String) {
    for (reaction in reactions) {
        if (reaction.id in seenReactions) continue
        seenReactions.add(reaction.id)
        if (reaction.name == me || reaction.ago >= 8000 || !onScreen) continue
        DjReaction.find(reaction.kind)?.let { spawn(it, reaction.name) }
    }
    while (seenReactions.size > 160) seenReactions.removeAt(0)
}

fun DjStudioModel.react(reaction: DjReaction) {
    spawn(reaction, if (inJam) jam?.me.orEmpty() else "")
    val current = jam
    if (current == null || !current.joined) return
    val now = SystemClock.elapsedRealtime()
    sentReactions.removeAll { now - it > 12_000 }
    if (sentReactions.size >= 8) return
    sentReactions.add(now)
    scope.launch { receive(jamCall { it.react(current.id, reaction.id) }) }
}

fun DjStudioModel.spawn(reaction: DjReaction, sender: String) {
    while (effects.size >= 10) effects.removeAt(0)
    val effect = DjEffect(reaction, sender)
    effects.add(effect)
    scope.launch {
        delay((reaction.seconds * 1000).toLong())
        effects.remove(effect)
    }
}

fun DjStudioModel.setShareMotion(on: Boolean) {
    shareMotion = on
    prefs?.putBool(DjStudioModel.MOTION_KEY, on)
    if (!on) motion.stop()
}

fun DjStudioModel.status(member: DjJamMember): DjPersonStatus {
    if (member.name != jam?.me) return DjJam.status(member, crowd.firstOrNull { it.name == member.name })
    if (onScreen && AppLifecycle.foreground.value) return DjPersonStatus.STUDIO
    return if (anyPlaying) DjPersonStatus.LISTENING else DjPersonStatus.IDLE
}

private fun DjStudioModel.presenceBody(): Map<String, Any?> {
    val visible = onScreen && AppLifecycle.foreground.value
    val state = if (visible) DjPresenceState.STUDIO else if (anyPlaying) DjPresenceState.LISTENING else DjPresenceState.IDLE
    return DjJamPresence.body(state, anyPlaying, if (shareMotion) motion.drain() else emptyList())
}

private fun DjStudioModel.follow(side: DjDeckSide, jam: DjJam) {
    val remote = jam.decks[side] ?: return
    if (remote.at <= (applied[side] ?: 0)) return
    applied[side] = remote.at
    if (remote.by == jam.me) return
    val deck = deck(side)
    if (deck.scratching || shareJobs[side] != null) return
    applyingRemote = true
    try {
        if (remote.videoId.isEmpty()) {
            if (deck.loaded) eject(side)
            return
        }
        val target = remote.expectedPosition(jam.now)
        if (deck.videoId != remote.videoId) prepare(deck, remote.videoId, remote.title, remote.author, target, false)
        if (abs(deck.rate - remote.rate) > 0.0005) setRate(side, remote.rate)
        if (DjJam.needsSeek(deck.position(), target, remote.playing)) {
            deck.voice.seek(target)
            deck.stoppedAt = target
        }
        if (remote.playing && !deck.playing) play(deck) else if (!remote.playing && deck.playing) pause(deck)
    } finally {
        applyingRemote = false
    }
}

private fun DjStudioModel.startPolling() {
    if (jamJob != null) return
    jamJob = scope.launch {
        while (isActive) {
            delay(if (onScreen) 1500 else 2500)
            val current = jam
            if (current == null || !current.joined) {
                motion.stop()
                jamJob = null
                return@launch
            }
            if (!onScreen && !anyPlaying) {
                motion.stop()
                continue
            }
            if (shareMotion) motion.start()
            val began = SystemClock.elapsedRealtime()
            val body = presenceBody()
            val reply = jamCall { it.presence(current.id, body) }
            receive(reply, (SystemClock.elapsedRealtime() - began) / 2000.0)
        }
    }
}
