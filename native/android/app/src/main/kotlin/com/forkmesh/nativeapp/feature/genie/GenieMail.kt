package com.forkmesh.nativeapp.feature.genie

import androidx.compose.ui.graphics.ImageBitmap
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.genie.GenieAutoSendOutcome
import com.forkmesh.nativeapp.genie.GenieAutoSendPace
import com.forkmesh.nativeapp.genie.GenieAutoSendReceipt
import com.forkmesh.nativeapp.genie.GenieAutoSendStage
import com.forkmesh.nativeapp.genie.GenieCountdown
import com.forkmesh.nativeapp.genie.GenieOutreach
import com.forkmesh.nativeapp.genie.GenieOutreachChannel
import com.forkmesh.nativeapp.genie.GenieOutreachDraft
import com.forkmesh.nativeapp.genie.GenieOutreachLog
import com.forkmesh.nativeapp.genie.GenieOutreachPick
import com.forkmesh.nativeapp.genie.GenieOutreachProblem
import com.forkmesh.nativeapp.market.PackagesException
import com.forkmesh.nativeapp.platform.AppLifecycle
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

private fun nowSeconds(): Double = System.currentTimeMillis() / 1000.0

data class GenieMailNotice(val id: Int, val line: String, val isError: Boolean)

object GenieMailStatus {
    const val SENT_SECONDS = 3.0
    const val NOTICE_SECONDS = 8.0

    private val stageState = MutableStateFlow<GenieAutoSendStage?>(null)
    private val noticeState = MutableStateFlow<GenieMailNotice?>(null)
    val stage: StateFlow<GenieAutoSendStage?> = stageState.asStateFlow()
    val notice: StateFlow<GenieMailNotice?> = noticeState.asStateFlow()
    private var count = 0
    private var settle: Job? = null
    private var fade: Job? = null

    val busy: Boolean get() = stageState.value.let { it != null && it != GenieAutoSendStage.Sent }

    fun set(next: GenieAutoSendStage) {
        AppLifecycle.scope.launch {
            settle?.cancel()
            stageState.value = next
            if (next != GenieAutoSendStage.Sent) return@launch
            settle = AppLifecycle.scope.launch {
                delay((SENT_SECONDS * 1000).toLong())
                stageState.value = null
            }
        }
    }

    fun finish() {
        AppLifecycle.scope.launch {
            if (!busy) return@launch
            settle?.cancel()
            stageState.value = null
        }
    }

    fun say(line: String, isError: Boolean = false) {
        GenieFeature.ping(if (isError) "Mail needs attention" else "Mail update", line)
        AppLifecycle.scope.launch {
            count += 1
            val id = count
            noticeState.value = GenieMailNotice(id, line, isError)
            fade?.cancel()
            fade = AppLifecycle.scope.launch {
                delay(((NOTICE_SECONDS + line.length / 30.0) * 1000).toLong())
                if (noticeState.value?.id == id) noticeState.value = null
            }
        }
    }

    fun dismiss() {
        fade?.cancel()
        noticeState.value = null
    }
}

data class GenieSuggestion(val pick: GenieOutreachPick, val image: ImageBitmap?)

object ShellSuggestions {
    const val ON_KEY = "shell/suggestion_mode"
    const val REFRESH_EVERY = 10 * 60.0

    private var prefs: PrefStore? = null
    private val onState = MutableStateFlow(false)
    private val contactState = MutableStateFlow<GenieSuggestion?>(null)
    val on: StateFlow<Boolean> = onState.asStateFlow()
    val contact: StateFlow<GenieSuggestion?> = contactState.asStateFlow()
    private val passed = mutableSetOf<String>()
    private var loading = false
    private var again = false
    private var loop: Job? = null

    fun start(store: PrefStore) {
        prefs = store
        onState.value = store.getBool(ON_KEY) ?: false
        loop?.cancel()
        loop = AppLifecycle.scope.launch {
            while (isActive) {
                refresh()
                delay((REFRESH_EVERY * 1000).toLong())
            }
        }
    }

    fun setOn(value: Boolean) {
        prefs?.putBool(ON_KEY, value)
        onState.value = value
        if (value) {
            refreshNow()
        } else {
            contactState.value = null
            if (ShellAutoSend.on.value) ShellAutoSend.setOn(false)
        }
    }

    fun refreshNow() {
        AppLifecycle.scope.launch { refresh() }
    }

    suspend fun refresh() {
        if (!onState.value) return
        if (loading) {
            again = true
            return
        }
        loading = true
        try {
            do {
                again = false
                refreshContact()
            } while (again && onState.value)
        } finally {
            loading = false
        }
    }

    fun contactTapped() {
        val suggestion = contactState.value ?: return
        if (GenieMailStatus.busy) {
            GenieMailStatus.say("An email is still being written. Try again when the picture stops spinning.")
            return
        }
        passed.add(suggestion.pick.contact.id)
        GenieOutreachCoach.write(suggestion.pick)
        contactState.value = null
        refreshNow()
    }

    fun skipContact() {
        val suggestion = contactState.value ?: return
        passed.add(suggestion.pick.contact.id)
        contactState.value = null
        refreshNow()
    }

    private suspend fun refreshContact() {
        val crm = GenieFeature.crm()
        if (crm == null) {
            contactState.value = null
            return
        }
        val skip = passed + GenieOutreachCoach.log.draftedContacts + ShellAutoSend.skipped
        val auto = ShellAutoSend.on.value
        val next = withContext(Dispatchers.IO) {
            if (auto) crm.best(skip, GenieOutreachChannel.Email, zohoOnly = true) else crm.best(skip)
        }
        if (!onState.value) return
        val current = contactState.value
        if (next != null && next.contact.id == current?.pick?.contact?.id) {
            contactState.value = current.copy(pick = next)
            return
        }
        val image = next?.let { withContext(Dispatchers.IO) { GenieFeature.picture(it) } }
        if (!onState.value) return
        contactState.value = next?.let { GenieSuggestion(it, image) }
    }
}

object ShellAutoSend {
    const val TRIES = 5
    const val IDLE_CHECK = 60.0
    const val SOONEST = 30.0

    private var pace = GenieAutoSendPace(null)
    private val onState = MutableStateFlow(false)
    private val sentState = MutableStateFlow(0)
    private val sendingState = MutableStateFlow(false)
    private val pausedState = MutableStateFlow("")
    private val reviewState = MutableStateFlow<GenieAutoSendReceipt?>(null)
    private val waitingState = MutableStateFlow(0)
    val on: StateFlow<Boolean> = onState.asStateFlow()
    val sentToday: StateFlow<Int> = sentState.asStateFlow()
    val sending: StateFlow<Boolean> = sendingState.asStateFlow()
    val pausedFor: StateFlow<String> = pausedState.asStateFlow()
    val review: StateFlow<GenieAutoSendReceipt?> = reviewState.asStateFlow()
    val waiting: StateFlow<Int> = waitingState.asStateFlow()
    val skipped = mutableSetOf<String>()
    private val queue = ArrayDeque<GenieAutoSendReceipt>()
    private var loop: Job? = null

    fun start(store: PrefStore) {
        pace = GenieAutoSendPace(store)
        onState.value = pace.on
        sentState.value = pace.sentToday(nowSeconds())
        loop?.cancel()
        loop = AppLifecycle.scope.launch {
            while (isActive) {
                if (AppLifecycle.foreground.value) tick()
                val wait = if (onState.value) maxOf(SOONEST, pace.wait(nowSeconds())) else IDLE_CHECK
                delay((wait * 1000).toLong())
            }
        }
    }

    fun toggle() {
        setOn(!onState.value)
        if (onState.value) {
            GenieMailStatus.say(
                "Auto-send is on: sending the first email now, then up to ${GenieAutoSendPace.PER_DAY} a day through Zoho Mail, one about every ${Math.round(GenieAutoSendPace.EVERY / 60)} minutes while the app is open. Long-press the picture to stop.",
            )
        } else {
            val sent = sentState.value
            GenieMailStatus.say("Auto-send is off. ${if (sent == 1) "1 email" else "$sent emails"} sent today.")
        }
    }

    fun setOn(value: Boolean) {
        pace.setOn(value)
        onState.value = value
        pausedState.value = ""
        ShellSuggestions.refreshNow()
        if (value) AppLifecycle.scope.launch { tick(force = true) }
    }

    fun countdown(now: Double): GenieCountdown? = if (sendingState.value) null else pace.countdown(now)

    suspend fun tick(force: Boolean = false) {
        val now = nowSeconds()
        sentState.value = pace.sentToday(now)
        if (!onState.value || sendingState.value) return
        if (force) {
            if (sentState.value >= GenieAutoSendPace.PER_DAY) {
                pause("Today's ${GenieAutoSendPace.PER_DAY} emails are already sent. The next one goes out tomorrow.")
                return
            }
        } else if (!pace.due(now)) {
            return
        }
        val crm = GenieFeature.crm()
        if (crm == null) {
            if (force) pause("This phone is not connected to ForkMesh. Sign in and try again.")
            return
        }
        sendingState.value = true
        try {
            repeat(TRIES) {
                if (!onState.value) return
                GenieMailStatus.set(GenieAutoSendStage.Finding)
                val pick = withContext(Dispatchers.IO) { crm.best(skipped, GenieOutreachChannel.Email, zohoOnly = true) }
                if (pick == null) {
                    pause("Nobody is left to email in a CRM with Zoho Mail connected.")
                    return
                }
                val writers = GenieFeature.outreachWriters()
                val outcome = withContext(Dispatchers.IO) { crm.autoSend(pick, writers) { GenieMailStatus.set(it) } }
                when (outcome) {
                    is GenieAutoSendOutcome.Sent -> {
                        skipped.add(pick.contact.id)
                        pace.sent(nowSeconds())
                        sentState.value = pace.sentToday(nowSeconds())
                        pausedState.value = ""
                        show(outcome.receipt)
                        ShellSuggestions.refreshNow()
                        return
                    }
                    is GenieAutoSendOutcome.Skip -> skipped.add(pick.contact.id)
                    is GenieAutoSendOutcome.Pause -> {
                        pause(outcome.reason)
                        return
                    }
                }
            }
            if (force) {
                pause("The next $TRIES contacts were skipped: no email address, closed, or emailed recently.")
            } else {
                pace.held(nowSeconds())
            }
        } finally {
            sendingState.value = false
            GenieMailStatus.finish()
        }
    }

    fun next() {
        reviewState.value = queue.removeFirstOrNull()
        waitingState.value = queue.size
    }

    suspend fun rate(receipt: GenieAutoSendReceipt, rating: Int, note: String): String? {
        val crm = GenieFeature.crm() ?: return "Sign in to rate emails."
        return withContext(Dispatchers.IO) {
            try {
                crm.rate(receipt, rating, note)
                null
            } catch (error: Exception) {
                PackagesException.describe(error)
            }
        }
    }

    private fun show(receipt: GenieAutoSendReceipt) {
        GenieFeature.ping("Auto-sent to ${receipt.contactName}", "“${receipt.subject}” through ${receipt.viaLabel}")
        if (reviewState.value == null) {
            reviewState.value = receipt
        } else {
            queue.addLast(receipt)
            waitingState.value = queue.size
        }
    }

    private fun pause(reason: String) {
        pace.held(nowSeconds())
        if (pausedState.value == reason) return
        pausedState.value = reason
        GenieMailStatus.say("Auto-send is waiting: $reason", isError = true)
    }
}

enum class GenieOfferStage { Offered, Writing }

data class GenieOutreachOffer(val id: Int, val pick: GenieOutreachPick, val image: ImageBitmap?, val stage: GenieOfferStage)

object GenieOutreachCoach {
    const val CHECK_EVERY = 60.0
    const val SHOW_SECONDS = 30.0

    var log: GenieOutreachLog = GenieOutreachLog(null)
        private set
    private val offerState = MutableStateFlow<GenieOutreachOffer?>(null)
    private val offState = MutableStateFlow(false)
    val offer: StateFlow<GenieOutreachOffer?> = offerState.asStateFlow()
    val off: StateFlow<Boolean> = offState.asStateFlow()
    private var activeSince = nowSeconds()
    private var count = 0
    private var looking = false
    private var fade: Job? = null
    private var loop: Job? = null

    fun start(store: PrefStore) {
        log = GenieOutreachLog(store)
        offState.value = log.off
        loop?.cancel()
        loop = AppLifecycle.scope.launch {
            AppLifecycle.foreground.collect { if (it) activeSince = nowSeconds() }
        }
        AppLifecycle.scope.launch {
            while (isActive) {
                delay((CHECK_EVERY * 1000).toLong())
                consider(force = false)
            }
        }
    }

    private val quiet: Boolean get() = !AppLifecycle.foreground.value || GenieSpiderState.hidden.value || !GenieSpiderState.calm

    fun setOff(value: Boolean) {
        log.setOff(value)
        offState.value = value
        if (value) dismiss()
    }

    fun suggestNow() {
        AppLifecycle.scope.launch { consider(force = true) }
    }

    suspend fun consider(force: Boolean) {
        val now = nowSeconds()
        if (offerState.value != null || looking || offState.value) return
        if (!force && !log.due(now, activeSince)) return
        if (quiet && !force) return
        val crm = GenieFeature.crm() ?: return
        looking = true
        try {
            val pick = withContext(Dispatchers.IO) { crm.pick(log.skip(now)) }
            if (pick == null) {
                log.checked(now)
                if (force) GenieSpiderState.say("Nobody new to contact in your CRM right now.")
                return
            }
            val image = withContext(Dispatchers.IO) { GenieFeature.picture(pick) }
            if (offerState.value != null || offState.value) return
            log.showed(pick.contact.id, now)
            count += 1
            val id = count
            GenieSpiderState.react(GenieReaction.Surprise)
            offerState.value = GenieOutreachOffer(id, pick, image, GenieOfferStage.Offered)
            fade?.cancel()
            fade = AppLifecycle.scope.launch {
                delay((SHOW_SECONDS * 1000).toLong())
                val current = offerState.value
                if (current?.id == id && current.stage == GenieOfferStage.Offered) dismiss()
            }
        } finally {
            looking = false
        }
    }

    fun dismiss() {
        fade?.cancel()
        offerState.value = null
    }

    fun write(pick: GenieOutreachPick) {
        if (GenieMailStatus.busy) return
        GenieMailStatus.set(GenieAutoSendStage.Writing)
        AppLifecycle.scope.launch {
            if (compose(pick, null)) GenieMailStatus.set(GenieAutoSendStage.Sent) else GenieMailStatus.finish()
        }
    }

    fun write() {
        val current = offerState.value ?: return
        if (current.stage != GenieOfferStage.Offered) return
        fade?.cancel()
        GenieSpiderState.react(GenieReaction.Eureka)
        offerState.value = current.copy(stage = GenieOfferStage.Writing)
        AppLifecycle.scope.launch { compose(current.pick, current.id) }
    }

    private suspend fun compose(pick: GenieOutreachPick, card: Int?): Boolean {
        var deck = ""
        if (pick.needsDeck) {
            try {
                val crm = GenieFeature.crm() ?: throw GenieOutreachProblem("The CRM is not available.")
                deck = withContext(Dispatchers.IO) { crm.deck(pick) }
            } catch (error: Exception) {
                if (!holds(card)) return false
                close(card)
                say(
                    "No email to ${pick.contact.displayName}: investor emails carry their own deck link, and I could not make one. ${error.message.orEmpty()}",
                    card, isError = true,
                )
                return false
            }
        }
        if (!holds(card)) return false
        log.drafted(pick.contact.id)
        val writers = GenieFeature.outreachWriters()
        val draft = withContext(Dispatchers.IO) { GenieOutreach.write(pick, writers, deck) }
        if (!holds(card)) return false
        close(card)
        open(draft, card)
        return true
    }

    private fun holds(card: Int?): Boolean = card == null || offerState.value?.id == card

    private fun close(card: Int?) {
        if (card != null) dismiss()
    }

    private fun say(line: String, card: Int?, isError: Boolean = false) {
        if (card == null) GenieMailStatus.say(line, isError) else GenieSpiderState.say(line)
    }

    private fun open(draft: GenieOutreachDraft, card: Int?) {
        if (card != null) GenieSpiderState.react(GenieReaction.Cheer)
        when (draft.channel) {
            GenieOutreachChannel.Email -> {
                val url = draft.zohoUrl
                if (url == null || !GenieFeature.openUrl(url)) {
                    GenieFeature.share(draft.shareText)
                    return
                }
                val line = openedLine(draft)
                say(line, card)
                if (card != null) GenieFeature.ping("The spider said", line)
            }
            GenieOutreachChannel.Mastodon -> {
                GenieFeature.copy(draft.body)
                GenieFeature.share(draft.body)
                say("Toot copied. Pick Mastodon to post it.", card)
            }
        }
    }

    fun openedLine(draft: GenieOutreachDraft): String {
        var line = if (draft.byModel) "Opened in Zoho Mail, written by ${draft.writer}." else "Opened in Zoho Mail from a template."
        if (draft.deck.isNotEmpty()) line += " The deck link is theirs alone, so the CRM shows when they open it and what they read."
        if (draft.cc.isNotEmpty()) line += if (draft.cc.size == 1) " Your teammate is on cc." else " Your ${draft.cc.size} teammates are on cc."
        return "$line Check it before you send."
    }
}
