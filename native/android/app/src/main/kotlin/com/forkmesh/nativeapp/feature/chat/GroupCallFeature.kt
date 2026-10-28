package com.forkmesh.nativeapp.feature.chat

import android.content.Context
import android.webkit.WebView
import com.forkmesh.nativeapp.chat.CallQualityHistory
import com.forkmesh.nativeapp.chat.DirectCallKind
import com.forkmesh.nativeapp.chat.GroupCallFrameBuffer
import com.forkmesh.nativeapp.chat.GroupCallIdentity
import com.forkmesh.nativeapp.chat.GroupCallLook
import com.forkmesh.nativeapp.chat.GroupCallPageEvent
import com.forkmesh.nativeapp.chat.GroupCallRoster
import com.forkmesh.nativeapp.chat.GroupCallView
import com.forkmesh.nativeapp.chat.OfficeChannelService
import com.forkmesh.nativeapp.feature.facechanger.FaceChangerFeature
import com.forkmesh.nativeapp.json.asJsonMap
import com.forkmesh.nativeapp.json.jsonText
import com.forkmesh.nativeapp.platform.ProjectLifecycle
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update

object GroupCallFeature {
    private val worker: ExecutorService = Executors.newSingleThreadExecutor { Thread(it, "group-call").apply { isDaemon = true } }
    private val network: ExecutorService = Executors.newSingleThreadExecutor { Thread(it, "group-call-net").apply { isDaemon = true } }
    private val events: ExecutorService = Executors.newSingleThreadExecutor { Thread(it, "group-call-events").apply { isDaemon = true } }
    private val viewState = MutableStateFlow(GroupCallView())
    private val webState = MutableStateFlow<WebView?>(null)
    private val rosterRevision = MutableStateFlow(0)
    private val qualityState = MutableStateFlow(CallQualityHistory())
    private val roster = GroupCallRoster()
    private val recent = GroupCallFrameBuffer()

    val view: StateFlow<GroupCallView> = viewState.asStateFlow()
    val web: StateFlow<WebView?> = webState.asStateFlow()
    val revision: StateFlow<Int> = rosterRevision.asStateFlow()
    val quality: StateFlow<CallQualityHistory> = qualityState.asStateFlow()

    @Volatile
    private var office: OfficeChannelService? = null

    @Volatile
    private var appContext: Context? = null

    @Volatile
    private var accountName: () -> String = { "" }

    @Volatile
    private var page: GroupCallPage? = null

    @Volatile
    private var identity: GroupCallIdentity? = null

    val active: Boolean get() = viewState.value.active

    fun install(context: Context, service: OfficeChannelService, account: () -> String) {
        appContext = context.applicationContext
        office = service
        accountName = account
        service.onLiveFrame = { plain, conversation -> handleFrame(plain, conversation) }
        service.onRoomOpened = { conversation, roomName -> roomOpened(conversation, roomName) }
        ProjectLifecycle.onClose { uninstall(service) }
    }

    private fun uninstall(service: OfficeChannelService) {
        if (office !== service) return
        service.onLiveFrame = null
        service.onRoomOpened = null
        office = null
        worker.execute { end("") }
        recent.clear()
        rosterRevision.update { it + 1 }
    }

    fun canCall(conversation: String): Boolean = office?.room(conversation)?.let { it.isPrivate && !it.readOnly } == true

    fun inCall(conversation: String): Int = roster.roster(conversation).size

    private fun handleFrame(plain: Map<String, Any?>, conversation: String) {
        recent.add(plain, conversation)
        if (roster.handle(plain, conversation, selfPeer = "") != null) rosterRevision.update { it + 1 }
        val current = page
        if (current != null && viewState.value.conversation == conversation) current.post("frame", listOf(plain, conversation))
    }

    private fun roomOpened(conversation: String, roomName: String) {
        val current = page
        if (current == null || viewState.value.conversation != conversation) return
        val fresh = office?.callIdentity(conversation)
        identity = fresh
        current.post("rekeyed", listOf(conversation, roomName, fresh?.page()))
    }

    private fun sign(call: String, message: ByteArray): ByteArray? = identity?.sign(call, message)

    private fun chatRoomName(conversation: String): String =
        office?.room(conversation)?.let { "chat-channel:${it.id}:v${it.keyVersion}" } ?: ""

    fun join(conversation: String, media: DirectCallKind, accent: String): String? {
        if (active) return "Leave the current call first."
        if (!canCall(conversation)) return "Group calls need a private channel."
        if (DirectCallFeature.busy) return "A call is already in progress."
        val context = appContext ?: return "Calls are not available right now."
        viewState.value = GroupCallView(conversation = conversation, media = media, state = "joining")
        qualityState.value = CallQualityHistory()
        worker.execute { start(context, conversation, media, accent) }
        return null
    }

    private fun start(context: Context, conversation: String, media: DirectCallKind, accent: String) {
        val made = GroupCallPage(context, events, ::onPageEvent, { event -> qualityState.update { it.add(event) } }, ::sign)
        page = made
        try {
            webState.value = made.load()
            if (!made.awaitAttached()) error("The call screen did not open.")
            if (page !== made || viewState.value.conversation != conversation) return
            for (frame in recent.recent(conversation)) made.post("frame", listOf(frame, conversation))
            val service = office
            if (service != null && service.open(conversation)) identity = service.callIdentity(conversation)
            val look = GroupCallLook.page(FaceChangerFeature.look, video = media == DirectCallKind.VIDEO)
            val joining = listOf(conversation, media.wire, accountName(), accent, chatRoomName(conversation), identity?.page(), look)
            val reply = made.request("join", joining, timeoutMs = 60_000L).asJsonMap()
            val problem = jsonText(reply["error"])
            if (problem.isNotEmpty()) end(problem)
        } catch (error: Exception) {
            end(error.message ?: "Group calls are not available right now.")
        }
    }

    private fun onPageEvent(event: GroupCallPageEvent) {
        when (event) {
            is GroupCallPageEvent.Send -> network.execute { runCatching { office?.sendLiveFrame(event.conversation, event.type, event.body) } }
            is GroupCallPageEvent.Ticket -> network.execute {
                val current = page ?: return@execute
                val answer = runCatching { office?.callTicket(event.conversation, event.body) }
                val value = answer.getOrNull()
                if (value != null) current.answerTicket(event.id, true, value) else current.answerTicket(event.id, false, answer.exceptionOrNull()?.message ?: "calls_unavailable")
            }
            is GroupCallPageEvent.State -> {
                val view = viewState.value
                if (!view.active) return
                if (!event.active && view.state != "joining") {
                    end(event.error)
                    return
                }
                if (event.active) {
                    viewState.value = view.copy(
                        state = event.state,
                        others = event.others,
                        muted = event.muted,
                        cameraOff = event.cameraOff,
                        engine = event.engine,
                        verify = event.verify,
                    )
                }
            }
            else -> Unit
        }
    }

    fun leave() = worker.execute { end("") }

    fun toggleMute() = worker.execute { page?.post("mute", emptyList()) }

    fun toggleCamera() = worker.execute { page?.post("camera", emptyList()) }

    fun confirmCode(match: Boolean) = worker.execute { page?.post("confirm", listOf(match)) }

    private fun end(problem: String) {
        val current = page
        page = null
        identity = null
        webState.value = null
        current?.close()
        val was = viewState.value
        viewState.value = GroupCallView()
        qualityState.value = CallQualityHistory()
        if (was.active && problem.isNotEmpty()) ChatDepth.post(problem)
    }
}
