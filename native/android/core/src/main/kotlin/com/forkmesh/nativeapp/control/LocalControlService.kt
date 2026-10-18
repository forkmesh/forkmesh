package com.forkmesh.nativeapp.control

import com.forkmesh.nativeapp.agents.AgentAttachmentError
import com.forkmesh.nativeapp.agents.AgentAttachmentMedia
import com.forkmesh.nativeapp.agents.AgentAttachmentMeta
import com.forkmesh.nativeapp.agents.AgentAttachmentRef
import com.forkmesh.nativeapp.agents.AgentTranscriptAttachments
import com.forkmesh.nativeapp.agents.AttachmentOrigin
import com.forkmesh.nativeapp.agents.PromptAttachment
import com.forkmesh.nativeapp.agents.promptOrPhotoNote
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.crypto.B64
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.JsonMap
import com.forkmesh.nativeapp.json.asJsonMap
import com.forkmesh.nativeapp.json.jsonText
import com.forkmesh.nativeapp.json.list
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.model.AgentSession
import com.forkmesh.nativeapp.security.DeviceIdentity
import java.io.File
import java.io.FileOutputStream
import java.util.Timer
import java.util.TimerTask
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.Executors

enum class LocalControlStatus { Idle, Connecting, AwaitingApproval, Connected, Failed }

data class PairedDesktop(val key: String, val name: String = "", val user: String = "", val pairedAtMs: Long = 0) {
    fun toJson(): Map<String, Any?> = linkedMapOf("key" to key, "name" to name, "user" to user, "pairedAtMs" to pairedAtMs)

    companion object {
        fun fromJson(json: Any?): PairedDesktop? {
            val map = json as? Map<*, *> ?: return null
            val key = jsonText(map["key"])
            if (controlFromB64(key, 32) == null) return null
            return PairedDesktop(key, jsonText(map["name"]), jsonText(map["user"]), (map["pairedAtMs"] as? Number)?.toLong() ?: 0)
        }
    }
}

data class MeshControlHost(
    val nodeId: String,
    val address: String,
    val port: Int,
    val name: String = "",
    val nodeName: String = "",
    val ownerUser: String = "",
    val platform: String = "",
) {
    val label: String
        get() = nodeName.ifEmpty { name.ifEmpty { nodeId.take(8) } }
}

fun automaticControlTarget(
    hosts: List<MeshControlHost>,
    paired: Set<String>,
    account: String,
    tried: Set<String>,
    accountDesktops: Set<String> = emptySet(),
): MeshControlHost? {
    hosts.firstOrNull { it.nodeId in paired }?.let { return it }
    hosts.firstOrNull { it.nodeId in accountDesktops }?.let { return it }
    val me = account.trim().lowercase()
    if (me.isEmpty()) return null
    return hosts.firstOrNull { it.nodeId !in tried && it.ownerUser.trim().lowercase() == me }
}

interface ControlHostSource {
    val displayName: String

    fun controlHosts(): List<MeshControlHost>

    fun addListener(listener: () -> Unit)

    fun removeListener(listener: () -> Unit)

    fun setRememberedTargets(provider: () -> List<String>) {}
}

data class LocalAgentRow(
    val owner: String,
    val name: String,
    val session: AgentSession,
    val prompt: String = "",
    val stoppable: Boolean = false,
    val unread: Boolean? = null,
) {
    val repoLabel: String get() = if (owner.isEmpty()) name else "$owner/$name"

    val title: String
        get() {
            if (session.issueTitle.isNotEmpty() || session.issueNumber > 0) return session.displayTitle
            val line = prompt.trim().split('\n').first().trim()
            if (line.isEmpty()) return session.displayTitle
            return if (line.length > 90) line.substring(0, 89) + "…" else line
        }

    val canContinue: Boolean get() = !session.isActive && !session.merged

    companion object {
        fun fromJson(json: JsonMap): LocalAgentRow = LocalAgentRow(
            owner = jsonText(json["repositoryOwner"]),
            name = jsonText(json["repositoryName"]),
            session = AgentSession.fromJson(json),
            prompt = jsonText(json["prompt"]),
            stoppable = json["stoppable"] == true,
            unread = json["unread"] as? Boolean,
        )
    }
}

data class LocalAgentProvider(val id: String, val label: String)

data class LocalAgentRepository(val owner: String, val name: String) {
    val label: String get() = "$owner/$name"
}

data class LocalOutsideAgent(
    val id: Int,
    val kind: String,
    val label: String,
    val provider: String = "",
    val model: String = "",
    val title: String = "",
    val cwd: String = "",
    val branchName: String = "",
    val owner: String = "",
    val name: String = "",
    val lastActivityMs: Long = 0,
    val steer: Boolean = false,
    val typeable: Boolean = false,
    val stoppable: Boolean = false,
    val otherUser: String = "",
    val terminal: String = "",
) {
    companion object {
        fun fromJson(json: JsonMap): LocalOutsideAgent = LocalOutsideAgent(
            id = (json["id"] as? Number)?.toInt() ?: 0,
            kind = jsonText(json["kind"]),
            label = jsonText(json["label"]).ifEmpty { jsonText(json["kind"]) },
            provider = jsonText(json["provider"]),
            model = jsonText(json["model"]),
            title = jsonText(json["title"]),
            cwd = jsonText(json["cwd"]),
            branchName = jsonText(json["branchName"]),
            owner = jsonText(json["repositoryOwner"]),
            name = jsonText(json["repositoryName"]),
            lastActivityMs = (json["lastActivityMs"] as? Number)?.toLong() ?: 0,
            steer = json["steer"] == true,
            typeable = json["typeable"] == true,
            stoppable = json["stoppable"] == true,
            otherUser = jsonText(json["otherUser"]),
            terminal = jsonText(json["terminal"]),
        )
    }
}

enum class QueueMove { Top, Up, Down, Bottom }

data class LocalAgentQueue(
    val running: Int = 0,
    val limit: Int = 1,
    val limitMax: Int = MAX_LIMIT,
    val order: List<Int> = emptyList(),
) {
    val waiting: Int get() = order.size

    val summary: String
        get() {
            val base = "$running of $limit running"
            return if (order.isEmpty()) base else "$base · $waiting waiting"
        }

    fun position(sessionId: Int): Int = order.indexOf(sessionId)

    fun target(sessionId: Int, move: QueueMove): Int? {
        val at = order.indexOf(sessionId)
        if (at < 0) return null
        val last = order.size - 1
        return when (move) {
            QueueMove.Top -> if (at == 0) null else 0
            QueueMove.Up -> if (at == 0) null else at - 1
            QueueMove.Down -> if (at == last) null else at + 1
            QueueMove.Bottom -> if (at == last) null else order.size
        }
    }

    companion object {
        const val MIN_LIMIT = 1
        const val MAX_LIMIT = 99

        fun fromJson(json: JsonMap): LocalAgentQueue {
            val limit = ((json["limit"] as? Number)?.toInt() ?: MIN_LIMIT).coerceIn(MIN_LIMIT, MAX_LIMIT)
            val limitMax = ((json["limitMax"] as? Number)?.toInt() ?: MAX_LIMIT).coerceIn(limit, MAX_LIMIT)
            return LocalAgentQueue(
                running = maxOf(0, (json["running"] as? Number)?.toInt() ?: 0),
                limit = limit,
                limitMax = limitMax,
                order = json.list("queue").mapNotNull { (it as? Number)?.toInt() }.filter { it > 0 }.distinct(),
            )
        }

        fun fromJsonOrNull(value: Any?): LocalAgentQueue? =
            if (value is Map<*, *>) fromJson(value.asJsonMap()) else null
    }
}

data class LocalAgentsSnapshot(
    val sessions: List<LocalAgentRow> = emptyList(),
    val outside: List<LocalOutsideAgent> = emptyList(),
    val repositories: List<LocalAgentRepository> = emptyList(),
    val providers: List<LocalAgentProvider> = emptyList(),
    val defaultProvider: String = "",
    val running: Int = 0,
    val queue: LocalAgentQueue? = null,
) {
    companion object {
        fun fromJson(json: JsonMap): LocalAgentsSnapshot = LocalAgentsSnapshot(
            sessions = json.maps("sessions").map(LocalAgentRow::fromJson),
            outside = json.maps("outside").map(LocalOutsideAgent::fromJson).filter { it.id != 0 },
            repositories = json.maps("repositories")
                .map { LocalAgentRepository(jsonText(it["owner"]), jsonText(it["name"])) }
                .filter { it.owner.isNotEmpty() && it.name.isNotEmpty() },
            providers = json.maps("providers")
                .map { LocalAgentProvider(jsonText(it["id"]), jsonText(it["label"])) }
                .filter { it.id.isNotEmpty() },
            defaultProvider = jsonText(json["defaultProvider"]),
            running = (json["running"] as? Number)?.toInt() ?: 0,
            queue = LocalAgentQueue.fromJsonOrNull(json["queue"]),
        )
    }
}

data class LocalAgentTranscript(val text: String, val attachments: List<AgentAttachmentRef>) {
    companion object {
        const val ID_PATTERN = "^[0-9a-f]{32}$"
        const val MAX_ATTACHMENT_BYTES = 3 * 1024 * 1024

        fun fromJson(json: JsonMap, sessionId: Int): LocalAgentTranscript {
            val text = jsonText(json["transcript"])
            val refs = if (json["attachments"] == null) {
                AgentTranscriptAttachments.references(text)
            } else {
                AgentAttachmentMeta.list(json["attachments"], ID_PATTERN).map {
                    AgentAttachmentRef(it, AttachmentOrigin.Desktop(sessionId))
                }
            }
            return LocalAgentTranscript(text, refs)
        }

        fun attachmentBytes(json: JsonMap): ByteArray {
            val encoded = jsonText(json["data"])
            if (encoded.isEmpty() || encoded.length > (MAX_ATTACHMENT_BYTES + 2) / 3 * 4 + 4) {
                throw AgentAttachmentError("The desktop sent no usable picture.")
            }
            val data = B64.decodeOrNull(encoded) ?: throw AgentAttachmentError("The desktop sent a damaged picture.")
            AgentAttachmentMedia.checkImage(data, MAX_ATTACHMENT_BYTES)
            return data
        }

        fun attachmentErrorText(code: String): String = when (code) {
            "not_connected", "closed" -> "Connect to the desktop on this network to open this attachment."
            "unknown_attachment", "unknown_session" -> "The desktop no longer has this attachment."
            "not_an_image" -> "The desktop can only show pictures here."
            "too_large" -> "This file is too large to show on the phone."
            "too_large_over_relay" -> "This file is too large to open through the relay. Open it on the desktop's network."
            "unsupported" -> "Update ForkMesh on the desktop to see attachments here."
            "timeout" -> "The desktop did not answer in time."
            else -> "The desktop could not send this attachment ($code)."
        }
    }
}

data class LocalReceivedFile(val name: String, val path: String, val size: Long, val receivedAtMs: Long)

object ControlErrorText {
    fun describe(error: Throwable): String =
        if (error is LocalControlException) withReason(describe(error.code), error) else (error.message ?: error.toString())

    fun withReason(text: String, error: LocalControlException): String =
        if (error.reason.isBlank()) text else "$text ${error.reason.trim()}"

    fun describe(code: String): String = when (code) {
        "not_connected", "closed" -> "The desktop is not connected."
        "timeout" -> "The desktop did not answer in time."
        "not_paired" -> "Approve this phone on the desktop first."
        "pair_on_lan" -> "Sign this phone and the desktop in to the same ForkMesh account, or approve this phone on the desktop while both are on the same network; after that it works from anywhere."
        "not_owner" -> "Only the account that owns this desktop can manage its hosts."
        "confirmation_required", "confirmation_mismatch" -> "That change was not confirmed. Try again."
        "confirmation_expired" -> "The confirmation expired. Try again."
        "name_mismatch" -> "The typed name does not match the host."
        "busy" -> "The desktop is already installing a host. Wait for it to finish."
        "already_running" -> "That action is already running."
        "unknown_host" -> "The desktop no longer has that host."
        "not_a_mirror_node" -> "Only mirror nodes run the ForkMesh service the desktop can control."
        "not_a_vultr_host" -> "Only a Vultr server the desktop created can be destroyed from here."
        "vultr_unreachable" -> "The desktop could not reach Vultr."
        "hosts_page_unavailable" -> "Open the desktop's Hosts page once, then try again."
        "lan_only" -> "Files move only on the same network."
        "relay_unavailable" -> "The relay does not carry desktop control yet, or the desktop is offline."
        "desktop_unreachable" -> "The desktop stopped answering through the relay."
        "channel_gone", "relay_reconnected", "idle" -> "The connection to the desktop was reset. Try again."
        "account_check_failed" -> "The desktop could not confirm this phone with your ForkMesh account. Try again in a moment."
        "revoked" -> "This phone is no longer on your ForkMesh account, so it cannot control the desktop."
        "unsupported" -> "This desktop's ForkMesh is too old for that. Update it."
        "wrong_desktop" -> "A different machine answered at that address. Try again in a moment."
        "bad_signature", "bad_welcome" -> "The desktop could not prove who it is."
        "pairing_denied" -> "The desktop declined this phone."
        "no_checkout" -> "The desktop has no local checkout of that repository."
        "not_running" -> "That agent is not running."
        "not_continuable" -> "That agent is still running."
        "unknown_session" -> "The desktop no longer has that agent."
        "bad_task" -> "Write what the agent should do (8000 characters at most)."
        "not_started" -> "The desktop could not start the agent."
        "images_unsupported" -> "Update ForkMesh on the desktop to send it photos."
        "too_many_images", "bad_images" -> "A prompt can carry 5 photos at most."
        "bad_image" -> "The desktop takes JPEG, PNG, GIF or WebP photos of 5 MB at most."
        "bad_chunk", "unknown_image", "image_incomplete" -> "A photo did not reach the desktop whole. Send it again."
        "cannot_save_image" -> "The desktop could not save the photo."
        "not_steerable" ->
            "That Claude Code session no longer takes messages. Steer it in the terminal that started it."
        "outside_cli_unsupported" ->
            "Only Claude Code started outside ForkMesh can be steered from here. Steer this agent in the terminal that started it."
        "outside_stop_unsupported" ->
            "An agent started outside ForkMesh cannot be stopped from here. Ask it to stop in a message, or stop it in the terminal that started it."
        "outside_other_user" ->
            "That agent runs in another login on the desktop. Open ForkMesh in that login to follow and steer it."
        "outside_images_unsupported" -> "Photos cannot be sent to an agent started outside ForkMesh."
        "outside_refuses_messages" ->
            "That Claude Code session refuses messages from other sessions (crossSessionInbound is \"refuse\"). Steer it in the terminal that started it."
        "outside_held_no_terminal" ->
            "That Claude Code session would hold the message for approval, but it has no terminal to approve it in. Turn on Accept messages from ForkMesh (\"crossSessionInbound\": \"accept\" in its settings.json) to steer it."
        "held_for_approval" ->
            "Sent. Claude Code is holding it for approval: approve it in the terminal that started it, or turn on Accept messages from ForkMesh (\"crossSessionInbound\": \"accept\" in that Claude Code's settings.json) to steer without approving."
        "typing_rate_limited" -> "Too many prompts from this phone in the last minute. Wait a moment and try again."
        "terminal_user_typing" -> "Someone is typing in that terminal right now. Try again in a moment."
        "terminal_remote_off" ->
            "Phone typing is turned off in that terminal. Press Ctrl-^ there (or restart it without --no-remote) to turn it back on."
        "not_typeable" -> "That terminal can no longer be typed into. It may have closed."
        "bad_limit" -> "Choose between 1 and 99 agents at once."
        "not_queued" -> "That agent is no longer waiting in the desktop's queue."
        "unknown_repository" -> "The desktop does not have that repository open."
        "unknown_terminal" -> "That terminal has ended on the desktop."
        "unsupported_provider" -> "The desktop can start only Claude Code or Codex in a terminal."
        "cli_missing" -> "That agent's command-line tool is not installed on the desktop."
        "guardrails_on" ->
            "That repository's agent guardrails do not allow a free-form terminal. Start a guarded agent instead, or turn the guardrails off on the desktop."
        "terminal_vm" -> "This desktop runs agents in its virtual machine, so it cannot open an agent terminal."
        "terminal_limit" -> "Six terminals (agents and SSH sessions) already run on the desktop. End one first."
        "ssh_missing" -> "The desktop has no ssh (or sshpass, for a password host) installed to open the session."
        "ssh_unavailable" -> "The desktop could not use this host's saved SSH key or address. Check the host on the desktop."
        "terminal_failed" -> "The desktop could not open a terminal."
        "no_worktree" -> "That agent has no folder left on the desktop to open a terminal in."
        "cloud_session" -> "That agent runs on another node, so its conversation is not on the desktop."
        "terminal_open" ->
            "That agent is open in a terminal on the desktop. Continue it there, or end the terminal first."
        "bad_input" -> "The desktop refused that input."
        else -> code.replace('_', ' ')
    }
}

fun interface LocalControlConnector {
    fun open(host: MeshControlHost, expectedKey: String?): LocalControlConnection
}

class LocalControlService(
    private val identity: DeviceIdentity,
    private val prefs: PrefStore,
    private val mesh: ControlHostSource? = null,
    scopePrefix: String = "",
    private val userName: () -> String = { "" },
    private val filesDirectory: () -> File,
    private val platform: String = "android",
    connector: LocalControlConnector? = null,
) {
    companion object {
        const val PAIRED_KEY = "localControl/pairedDesktops"
        const val LAN_ADDRESSES_KEY = "localControl/lanAddresses"
        const val LAN_PROBE_WINDOW_MS = 1_500L

        fun safeName(name: String): String {
            var base = name.replace('\\', '/').split('/').last()
            base = base.replace(Regex("[^A-Za-z0-9._\\-() ]"), "_").trim()
            while (base.startsWith(".")) base = base.substring(1)
            if (base.length > 120) base = base.substring(base.length - 120)
            return base.ifEmpty { "file" }
        }
    }

    private val pairedPrefsKey = scopePrefix + PAIRED_KEY
    private val lanAddressesPrefsKey = scopePrefix + LAN_ADDRESSES_KEY
    private val lock = Any()
    private val listeners = CopyOnWriteArrayList<() -> Unit>()
    private val tapListeners = CopyOnWriteArrayList<Pair<Any, (ControlEvent) -> Unit>>()
    private val worker = Executors.newSingleThreadExecutor { Thread(it, "LocalControl").apply { isDaemon = true } }
    private val timer = Timer("LocalControl-retry", true)
    private val connector: LocalControlConnector = connector ?: LocalControlConnector { host, expected ->
        LocalControlConnection.open(host.address, host.port, identity, mesh?.displayName ?: "", userName(), platform, expected)
    }
    private val meshListener: () -> Unit = { onMeshChanged() }

    @Volatile
    var paired: List<PairedDesktop> = emptyList()
        private set

    @Volatile
    var status: LocalControlStatus = LocalControlStatus.Idle
        private set

    @Volatile
    var pairingCode: String = ""
        private set

    @Volatile
    var error: String = ""
        private set

    @Volatile
    var desktopKey: String = ""
        private set

    @Volatile
    var desktopName: String = ""
        private set

    @Volatile
    var desktopUser: String = ""
        private set

    @Volatile
    var host: MeshControlHost? = null
        private set

    @Volatile
    var agents: LocalAgentsSnapshot? = null
        private set

    @Volatile
    var agentsLoading: Boolean = false
        private set

    @Volatile
    var uploadName: String = ""
        private set

    @Volatile
    var uploadProgress: Double? = null
        private set

    private val received = CopyOnWriteArrayList<LocalReceivedFile>()
    private var connection: LocalControlConnection? = null
    private var connectionTap: Any? = null
    private var autoConnectHeld = false
    private var dialQueued = false
    private val accountDesktopsTried = HashSet<String>()

    @Volatile
    private var accountDesktopKeys: Set<String> = emptySet()
    private var retryTask: TimerTask? = null
    private var failures = 0
    private var disposed = false
    private var agentsInFlight = false
    private var agentsAgain = false
    private val incoming = HashMap<String, Incoming>()

    @Volatile
    private var lanProbeMs = 0L

    private class Incoming(val name: String, val size: Long, val path: File, val part: File, val out: FileOutputStream) {
        var receivedBytes = 0L
    }

    val connected: Boolean get() = status == LocalControlStatus.Connected

    val lanPending: Boolean
        get() {
            if (connected || autoConnectHeld) return false
            if (synchronized(lock) { dialQueued } || status == LocalControlStatus.Connecting) return true
            return System.currentTimeMillis() - lanProbeMs < LAN_PROBE_WINDOW_MS
        }

    private fun rememberedAddresses(): Map<String, String> = try {
        (Json.parse(prefs.getString(lanAddressesPrefsKey) ?: "") as? Map<*, *>)
            ?.mapNotNull { (key, value) -> if (key is String && value is String) key to value else null }
            ?.toMap()
    } catch (_: Exception) {
        null
    } ?: emptyMap()

    private fun rememberAddress(key: String, address: String) {
        if (key.isEmpty() || address.isEmpty()) return
        val map = rememberedAddresses()
        if (map[key] == address) return
        prefs.putString(lanAddressesPrefsKey, Json.stringify(map + (key to address)))
    }

    private fun lanProbeTargets(): List<String> {
        if (disposed || connected || autoConnectHeld) return emptyList()
        val heard = hosts.map { it.nodeId }.toSet()
        val targets = rememberedAddresses().filter { (key, _) -> isKnown(key) && key !in heard }.values.toList()
        if (targets.isNotEmpty()) lanProbeMs = System.currentTimeMillis()
        return targets
    }
    val receivedFiles: List<LocalReceivedFile> get() = received.toList()
    val hosts: List<MeshControlHost> get() = mesh?.controlHosts() ?: emptyList()
    val currentConnection: LocalControlConnection? get() = synchronized(lock) { connection }

    fun isPaired(key: String): Boolean = paired.any { it.key == key }

    private fun isKnown(key: String): Boolean = isPaired(key) || key in accountDesktopKeys

    fun setAccountDesktops(keys: Set<String>) {
        if (keys == accountDesktopKeys) return
        accountDesktopKeys = keys
        onMeshChanged()
    }

    init {
        paired = try {
            (Json.parse(prefs.getString(pairedPrefsKey) ?: "") as? List<*>)?.mapNotNull(PairedDesktop::fromJson)
        } catch (_: Exception) {
            null
        } ?: emptyList()
        mesh?.addListener(meshListener)
        mesh?.setRememberedTargets { lanProbeTargets() }
        worker.execute { onMeshChanged() }
    }

    fun addListener(listener: () -> Unit) = listeners.add(listener)

    fun removeListener(listener: () -> Unit) = listeners.remove(listener)

    private fun notifyListeners() {
        if (disposed) return
        for (listener in listeners) runCatching { listener() }
    }

    fun addEventTap(tap: (ControlEvent) -> Unit): Any {
        val token = Any()
        tapListeners.add(token to tap)
        return token
    }

    fun removeEventTap(token: Any) {
        tapListeners.removeAll { it.first === token }
    }

    private fun savePaired() {
        prefs.putString(pairedPrefsKey, Json.stringify(paired.map { it.toJson() }))
    }

    private fun setStatus(next: LocalControlStatus, message: String = "") {
        status = next
        error = message
        notifyListeners()
    }

    private fun onMeshChanged() {
        if (disposed) return
        notifyListeners()
        val pairedKeys = paired.map { it.key }.toSet()
        val candidate = synchronized(lock) {
            if (autoConnectHeld || dialQueued || connection != null || status == LocalControlStatus.Connecting || retryTask != null) return
            val target = automaticControlTarget(hosts, pairedKeys, userName(), accountDesktopsTried, accountDesktopKeys) ?: return
            if (!isKnown(target.nodeId)) accountDesktopsTried.add(target.nodeId)
            dialQueued = true
            target
        }
        worker.execute { connectNow(candidate, automatic = true) }
    }

    fun connect(target: MeshControlHost) {
        worker.execute { connectNow(target, automatic = false) }
    }

    private fun connectNow(target: MeshControlHost, automatic: Boolean) {
        synchronized(lock) {
            if (automatic) dialQueued = false else autoConnectHeld = false
            retryTask?.cancel()
            retryTask = null
        }
        if (currentConnection != null) disconnectNow(hold = false)
        host = target
        desktopKey = target.nodeId
        desktopName = target.label
        desktopUser = target.ownerUser
        pairingCode = ""
        setStatus(LocalControlStatus.Connecting)
        val opened = try {
            connector.open(target, target.nodeId.ifEmpty { null })
        } catch (error: Throwable) {
            failed(ControlErrorText.describe(error), target)
            return
        }
        if (disposed) {
            opened.close()
            return
        }
        synchronized(lock) {
            connection = opened
            failures = 0
        }
        if (opened.serverName.isNotEmpty()) desktopName = opened.serverName
        if (opened.serverUser.isNotEmpty()) desktopUser = opened.serverUser
        desktopKey = opened.serverKey
        connectionTap = opened.addEventTap { event -> onEvent(opened, event) }
        opened.onClose { worker.execute { onClosed(opened) } }
    }

    private fun failed(message: String, target: MeshControlHost) {
        synchronized(lock) {
            connection = null
            failures++
        }
        setStatus(LocalControlStatus.Failed, message)
        if (isKnown(target.nodeId) && !autoConnectHeld) {
            val seconds = (5 * failures).coerceIn(5, 60)
            scheduleRetry(seconds * 1000L)
        }
    }

    private fun scheduleRetry(delayMs: Long) {
        synchronized(lock) {
            if (disposed) return
            retryTask?.cancel()
            val task = object : TimerTask() {
                override fun run() {
                    synchronized(lock) { retryTask = null }
                    worker.execute { onMeshChanged() }
                }
            }
            retryTask = task
            timer.schedule(task, delayMs)
        }
    }

    private fun onClosed(closed: LocalControlConnection) {
        synchronized(lock) {
            if (connection !== closed) return
            connection = null
        }
        for (item in synchronized(incoming) { incoming.values.toList().also { incoming.clear() } }) discard(item)
        if (status == LocalControlStatus.Failed) return
        val wasConnected = status == LocalControlStatus.Connected
        setStatus(
            if (wasConnected) LocalControlStatus.Idle else LocalControlStatus.Failed,
            if (wasConnected) "" else "The desktop closed the connection.",
        )
        val current = host
        if (!autoConnectHeld && current != null && isKnown(current.nodeId)) scheduleRetry(3000)
    }

    private fun onEvent(source: LocalControlConnection, event: ControlEvent) {
        val data = event.data
        when (event.event) {
            "session" -> {
                jsonText(data["name"]).takeIf { it.isNotEmpty() }?.let { desktopName = it }
                jsonText(data["user"]).takeIf { it.isNotEmpty() }?.let { desktopUser = it }
                if (data["paired"] == true) {
                    pairingCode = ""
                    rememberPaired()
                    host?.let { rememberAddress(desktopKey, it.address) }
                    setStatus(LocalControlStatus.Connected)
                    worker.execute { refreshAgentsNow() }
                } else {
                    pairingCode = jsonText(data["code"]).ifEmpty { source.pairingCode }
                    setStatus(LocalControlStatus.AwaitingApproval)
                }
            }
            "pairing-denied" -> {
                autoConnectHeld = true
                setStatus(LocalControlStatus.Failed, "The desktop declined this phone.")
                source.close()
            }
            "agents.changed" -> worker.execute { refreshAgentsNow() }
            "file.begin" -> beginIncoming(data)
            "file.chunk" -> chunkIncoming(data)
            "file.end" -> endIncoming(data, complete = true)
            "file.cancel" -> endIncoming(data, complete = false)
        }
        for ((_, tap) in tapListeners) runCatching { tap(event) }
    }

    private fun rememberPaired() {
        val key = desktopKey
        if (key.isEmpty()) return
        val existing = paired.firstOrNull { it.key == key }
        val entry = PairedDesktop(key, desktopName, desktopUser, existing?.pairedAtMs ?: System.currentTimeMillis())
        paired = paired.filter { it.key != key } + entry
        savePaired()
    }

    fun disconnect(hold: Boolean = true) {
        worker.execute { disconnectNow(hold) }
    }

    private fun disconnectNow(hold: Boolean) {
        val current: LocalControlConnection?
        synchronized(lock) {
            if (hold) autoConnectHeld = true
            retryTask?.cancel()
            retryTask = null
            current = connection
            connection = null
        }
        connectionTap?.let { current?.removeEventTap(it) }
        current?.close()
        agents = null
        pairingCode = ""
        setStatus(LocalControlStatus.Idle)
    }

    fun forget(key: String) {
        paired = paired.filter { it.key != key }
        savePaired()
        if (desktopKey == key) disconnect()
        notifyListeners()
    }

    fun request(method: String, params: Map<String, Any?> = emptyMap(), timeoutMs: Long = 20_000): Map<String, Any?> {
        val current = currentConnection
        if (current == null || !connected) throw LocalControlException("not_connected")
        return current.request(method, params, timeoutMs)
    }

    fun refreshAgents() {
        worker.execute { refreshAgentsNow() }
    }

    private fun refreshAgentsNow() {
        synchronized(lock) {
            if (agentsInFlight) {
                agentsAgain = true
                return
            }
            agentsInFlight = true
        }
        agentsLoading = true
        notifyListeners()
        try {
            do {
                synchronized(lock) { agentsAgain = false }
                agents = LocalAgentsSnapshot.fromJson(request("agents.list"))
                notifyListeners()
            } while (synchronized(lock) { agentsAgain } && connected)
        } catch (_: Exception) {
        } finally {
            synchronized(lock) { agentsInFlight = false }
            agentsLoading = false
            notifyListeners()
        }
    }

    fun startAgent(owner: String, name: String, task: String, provider: String = ""): Int {
        val params = linkedMapOf<String, Any?>("owner" to owner, "name" to name, "task" to task)
        if (provider.isNotEmpty()) params["provider"] = provider
        return (request("agents.start", params)["id"] as? Number)?.toInt() ?: 0
    }

    fun sendPrompt(sessionId: Int, text: String, images: List<PromptAttachment> = emptyList()) {
        val link = LanControlLink(this)
        val params = DesktopPromptImages.params(mapOf("id" to sessionId, "text" to promptOrPhotoNote(text, images)), images, link)
        request("agents.prompt", params, 30_000)
    }

    fun stopAgent(sessionId: Int) {
        request("agents.stop", mapOf("id" to sessionId))
    }

    fun continueAgent(sessionId: Int) {
        request("agents.continue", mapOf("id" to sessionId))
    }

    fun transcript(sessionId: Int): String = jsonText(request("agents.transcript", mapOf("id" to sessionId))["transcript"])

    fun transcriptDetail(sessionId: Int): LocalAgentTranscript =
        LocalAgentTranscript.fromJson(request("agents.transcript", mapOf("id" to sessionId)), sessionId)

    fun attachment(sessionId: Int, attachmentId: String): ByteArray = try {
        LocalAgentTranscript.attachmentBytes(request("agents.attachment", mapOf("id" to sessionId, "attachment" to attachmentId)))
    } catch (error: LocalControlException) {
        throw AgentAttachmentError(LocalAgentTranscript.attachmentErrorText(error.code))
    }

    fun sendFile(file: File, name: String = file.name): String {
        val size = file.length()
        uploadName = name
        uploadProgress = 0.0
        notifyListeners()
        var transfer: String? = null
        try {
            transfer = jsonText(request("files.begin", mapOf("name" to name, "size" to size))["transfer"])
            var sent = 0L
            file.inputStream().use { input ->
                val buffer = ByteArray(CONTROL_FILE_CHUNK_BYTES)
                while (true) {
                    var filled = 0
                    while (filled < buffer.size) {
                        val n = input.read(buffer, filled, buffer.size - filled)
                        if (n < 0) break
                        filled += n
                    }
                    if (filled == 0) break
                    request("files.chunk", mapOf("transfer" to transfer, "data" to B64.encode(buffer.copyOf(filled))))
                    sent += filled
                    uploadProgress = if (size == 0L) 1.0 else sent.toDouble() / size
                    notifyListeners()
                    if (filled < buffer.size) break
                }
            }
            val end = request("files.end", mapOf("transfer" to transfer))
            transfer = null
            return jsonText(end["name"]).ifEmpty { name }
        } finally {
            val open = transfer
            if (!open.isNullOrEmpty() && connected) runCatching { request("files.cancel", mapOf("transfer" to open)) }
            uploadProgress = null
            uploadName = ""
            notifyListeners()
        }
    }

    private fun beginIncoming(data: JsonMap) {
        val transfer = jsonText(data["transfer"])
        val size = (data["size"] as? Number)?.toLong() ?: return
        if (transfer.isEmpty() || size < 0) return
        val name = safeName(jsonText(data["name"]))
        try {
            val dir = filesDirectory().apply { mkdirs() }
            var path = File(dir, name)
            var n = 2
            while (path.exists()) {
                val dot = name.lastIndexOf('.')
                path = if (dot > 0) File(dir, "${name.substring(0, dot)} ($n)${name.substring(dot)}") else File(dir, "$name ($n)")
                n++
            }
            val part = File(path.path + ".part")
            synchronized(incoming) { incoming[transfer] = Incoming(name, size, path, part, FileOutputStream(part)) }
        } catch (_: Exception) {
        }
    }

    private fun chunkIncoming(data: JsonMap) {
        val item = synchronized(incoming) { incoming[jsonText(data["transfer"])] } ?: return
        val bytes = B64.decodeOrNull(jsonText(data["data"])) ?: return
        item.receivedBytes += bytes.size
        runCatching { item.out.write(bytes) }
    }

    private fun endIncoming(data: JsonMap, complete: Boolean) {
        val item = synchronized(incoming) { incoming.remove(jsonText(data["transfer"])) } ?: return
        if (!complete || item.receivedBytes != item.size) {
            discard(item)
            return
        }
        try {
            item.out.close()
            if (item.part.renameTo(item.path)) {
                received.add(0, LocalReceivedFile(item.path.name, item.path.path, item.size, System.currentTimeMillis()))
                notifyListeners()
            }
        } catch (_: Exception) {
        }
    }

    private fun discard(item: Incoming) {
        runCatching { item.out.close() }
        runCatching { item.part.delete() }
    }

    fun dispose() {
        synchronized(lock) {
            disposed = true
            retryTask?.cancel()
            timer.cancel()
        }
        mesh?.removeListener(meshListener)
        currentConnection?.close()
        worker.shutdown()
    }
}
