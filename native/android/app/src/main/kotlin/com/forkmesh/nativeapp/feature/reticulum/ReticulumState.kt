package com.forkmesh.nativeapp.feature.reticulum

import android.content.Context
import android.content.SharedPreferences
import android.net.wifi.WifiManager
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AuthService
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.asJsonMap
import com.forkmesh.nativeapp.platform.AppLifecycle
import com.forkmesh.nativeapp.platform.SecureStore
import com.forkmesh.nativeapp.reticulum.ChatMessage
import com.forkmesh.nativeapp.reticulum.Conversation
import com.forkmesh.nativeapp.reticulum.DestinationSort
import com.forkmesh.nativeapp.reticulum.ReticulumMessenger
import com.forkmesh.nativeapp.reticulum.InterfaceConfig
import com.forkmesh.nativeapp.reticulum.InterfaceKind
import com.forkmesh.nativeapp.reticulum.LinkState
import com.forkmesh.nativeapp.reticulum.MapGroup
import com.forkmesh.nativeapp.reticulum.ReticulumConfig
import com.forkmesh.nativeapp.reticulum.ReticulumNode
import com.forkmesh.nativeapp.reticulum.ReticulumSnapshot
import com.forkmesh.nativeapp.reticulum.Rns
import com.forkmesh.nativeapp.reticulum.RnsIdentity
import java.util.concurrent.atomic.AtomicBoolean
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

enum class ReticulumTab(val label: String) { Network("Network"), Messages("Messages"), Interfaces("Interfaces"), Destinations("Destinations"), Activity("Activity"), Identity("Identity") }

class ReticulumState {
    var snapshot by mutableStateOf(ReticulumSnapshot())
        private set
    var message by mutableStateOf("")
        private set
    var messageIsError by mutableStateOf(false)
        private set
    var tab by mutableStateOf(ReticulumTab.Network)
    var selected by mutableStateOf("")
    var selectedInterface by mutableStateOf("")
    var mapFilter by mutableStateOf("")
    var destinationFilter by mutableStateOf("")
    var kindFilter by mutableStateOf<MapGroup?>(null)
    var sort by mutableStateOf(DestinationSort.LastHeard)
    var lookupInput by mutableStateOf("")
    var lookupError by mutableStateOf("")
    var announceName by mutableStateOf("")
    var announceMinutes by mutableIntStateOf(0)
        private set
    var answerPaths by mutableStateOf(true)
        private set
    val pulses = mutableStateMapOf<String, Long>()
    var conversations by mutableStateOf<List<Conversation>>(emptyList())
        private set
    var chat by mutableStateOf<List<ChatMessage>>(emptyList())
        private set
    var unread by mutableIntStateOf(0)
        private set
    var activeLinks by mutableIntStateOf(0)
        private set
    var address by mutableStateOf(ByteArray(0))
        private set
    var currentPeer by mutableStateOf("")
        private set
    var composer by mutableStateOf("")

    private val node = ReticulumNode()
    private val messenger = ReticulumMessenger(node)
    private var appContext: Context? = null
    @Volatile private var watchingMessages = false
    private val worker = Dispatchers.IO.limitedParallelism(1)
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private val publishPending = AtomicBoolean(false)
    private var prefs: SharedPreferences? = null
    private var secrets: SecureStore? = null
    private var wifi: WifiManager? = null
    private var multicast: WifiManager.MulticastLock? = null
    private var started = false
    private var previousOnline = 0
    private var announceJob: Job? = null

    fun attach(context: Context) {
        if (prefs != null) return
        val app = context.applicationContext
        prefs = app.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        secrets = SecureStore(app, PREFS)
        wifi = app.getSystemService(WifiManager::class.java)
        appContext = app
        node.changedHandler = { schedulePublish() }
        node.announceHandler = { hash ->
            messenger.destinationAnnounced(hash)
            val key = Rns.hex(hash)
            scope.launch { pulses[key] = System.currentTimeMillis() }
        }
        node.packetHandler = { id, packet, raw, hops -> messenger.handlePacket(id, packet, raw, hops) }
        messenger.changedHandler = { schedulePublish() }
        messenger.saveHandler = { json -> prefs?.edit()?.putString(MESSAGES, json)?.apply() }
        messenger.incomingHandler = { message -> scope.launch { onIncoming(message) } }
        val store = prefs!!
        announceMinutes = store.getInt(ANNOUNCE_MINUTES, 0).takeIf { minutes -> minutes in setOf(0, 15, 30, 60, 180) } ?: 0
        answerPaths = store.getBoolean(ANSWER_PATHS, true)
        val storedName = store.getString(ANNOUNCE_NAME, "").orEmpty()
        val defaultName = storedName.ifEmpty { Services.get<AuthService>()?.session?.value?.nodeName.orEmpty() }
        announceName = defaultName.trim().take(64)
        val stored = store.getString(INTERFACES, "").orEmpty()
        val savedMessages = store.getString(MESSAGES, "").orEmpty()
        perform {
            val configs = if (stored.isEmpty()) ReticulumConfig.defaultInterfaces() else withPassphrases(parseInterfaces(stored))
            setIdentity(loadIdentity())
            setInterfaces(configs)
            setAnnounceName(announceName)
            setAnswerPathRequests(answerPaths)
            messenger.setDisplayName(announceName)
            messenger.load(savedMessages)
        }
        restartAnnounceTimer()
        scope.launch {
            AppLifecycle.foreground.collect { foreground -> onForeground(foreground) }
        }
    }

    private fun parseInterfaces(text: String): List<InterfaceConfig> =
        (runCatching { Json.parse(text) }.getOrNull() as? List<*>).orEmpty().mapNotNull { InterfaceConfig.fromJson(it.asJsonMap()) }

    private fun withPassphrases(configs: List<InterfaceConfig>): List<InterfaceConfig> {
        val sealed = secrets?.read(PASSPHRASES)?.let { runCatching { Json.parseObject(it) }.getOrNull() }.orEmpty()
        return configs.map { config -> (sealed[config.id] as? String)?.let { config.copy(passphrase = it) } ?: config }
    }

    private fun loadIdentity(): RnsIdentity {
        val store = secrets
        val saved = store?.read(IDENTITY)?.let(Rns::fromHex)?.let(RnsIdentity::fromPrivateKey)
        if (saved != null) return saved
        val fresh = RnsIdentity.generate()
        store?.writeChecked(IDENTITY, Rns.hex(fresh.privateKey))
        return fresh
    }

    fun refresh() {
        if (!started) {
            started = true
            val on = prefs?.getBoolean(NETWORK_ON, true) ?: true
            if (on) perform { start() }
        } else {
            perform {
                if (isRunning) {
                    for (config in interfaces()) {
                        if (config.enabled && status(config.id).state != LinkState.Online) reconnectInterface(config.id)
                    }
                }
            }
        }
    }

    private fun onForeground(foreground: Boolean) {
        if (!started || !foreground) return
        refresh()
    }

    private fun perform(block: ReticulumNode.() -> Unit) {
        scope.launch(worker) {
            node.block()
            publishNow()
        }
    }

    private fun schedulePublish() {
        if (!publishPending.compareAndSet(false, true)) return
        scope.launch(worker) {
            delay(PUBLISH_GAP_MS)
            publishPending.set(false)
            publishNow()
        }
    }

    private suspend fun publishNow() {
        val next = node.snapshot()
        val peer = Rns.fromHex(currentPeer) ?: ByteArray(0)
        val nextConversations = messenger.conversations()
        val nextChat = if (peer.size == Rns.HASH_LENGTH) messenger.messages(peer) else emptyList()
        val nextUnread = messenger.unreadCount()
        val nextLinks = messenger.activeLinks()
        val nextAddress = messenger.address()
        withContext(Dispatchers.Main) {
            snapshot = next
            conversations = nextConversations
            chat = nextChat
            unread = nextUnread
            activeLinks = nextLinks
            if (!nextAddress.contentEquals(address)) address = nextAddress
            val online = next.onlineCount
            if (online > 0 && previousOnline == 0 && announceMinutes > 0) {
                scope.launch {
                    delay(FIRST_ANNOUNCE_DELAY_MS)
                    perform { announce() }
                }
            }
            previousOnline = online
            updateMulticast(next)
            val now = System.currentTimeMillis()
            pulses.keys.filter { now - (pulses[it] ?: 0L) > PULSE_MS }.forEach { pulses.remove(it) }
        }
    }

    private fun updateMulticast(next: ReticulumSnapshot) {
        val wanted = next.running && next.interfaces.any { it.config.enabled && it.config.kind == InterfaceKind.Udp }
        val lock = multicast
        if (wanted && lock == null) {
            multicast = runCatching { wifi?.createMulticastLock("ForkMesh Reticulum")?.apply { setReferenceCounted(false); acquire() } }.getOrNull()
        } else if (!wanted && lock != null) {
            runCatching { lock.release() }
            multicast = null
        }
    }

    fun show(text: String, error: Boolean = false) {
        message = text
        messageIsError = error
    }

    private fun saveInterfaces(configs: List<InterfaceConfig>) {
        prefs?.edit()?.putString(INTERFACES, Json.stringify(configs.map { it.toJson(includePassphrase = false) }))?.apply()
        val sealed = configs.filter { it.passphrase.isNotEmpty() }.associate { it.id to it.passphrase }
        if (sealed.isEmpty()) secrets?.delete(PASSPHRASES) else secrets?.writeChecked(PASSPHRASES, Json.stringify(sealed))
    }

    fun setWatching(watching: Boolean) {
        watchingMessages = watching
        if (watching) markCurrentRead()
    }

    private fun markCurrentRead() {
        val peer = Rns.fromHex(currentPeer) ?: return
        if (peer.size != Rns.HASH_LENGTH || tab != ReticulumTab.Messages) return
        perform { messenger.markRead(peer) }
    }

    private fun onIncoming(message: ChatMessage) {
        val watching = watchingMessages && tab == ReticulumTab.Messages && currentPeer == message.peerHex && AppLifecycle.foreground.value
        if (watching) {
            perform { messenger.markRead(message.peer) }
            return
        }
        val context = appContext ?: return
        scope.launch(worker) {
            val name = messenger.peerName(message.peer)
            withContext(Dispatchers.Main) { ReticulumNotifier.post(context, name, message.content.take(200)) }
        }
    }

    fun openConversation(hex: String) {
        val peer = Rns.fromHex(hex) ?: return
        if (peer.size != Rns.HASH_LENGTH) return
        currentPeer = hex
        tab = ReticulumTab.Messages
        chat = emptyList()
        perform { messenger.markRead(peer) }
    }

    fun closeConversation() {
        currentPeer = ""
        chat = emptyList()
    }

    fun sendMessage(): Boolean {
        val text = composer
        val peer = Rns.fromHex(currentPeer) ?: return false
        if (text.isBlank()) return false
        if (text.toByteArray(Charsets.UTF_8).size > ReticulumMessenger.MAX_CONTENT_BYTES) {
            show("Messages can be up to ${ReticulumMessenger.MAX_CONTENT_BYTES / 1024} KB.", true)
            return false
        }
        composer = ""
        scope.launch(worker) {
            val id = messenger.send(peer, text)
            if (id.isEmpty()) withContext(Dispatchers.Main) {
                composer = text
                show("That message could not be queued.", true)
            }
            publishNow()
        }
        return true
    }

    fun retryMessage(id: String) = perform { messenger.retry(id) }

    fun deleteMessage(id: String) = perform { messenger.forget(id) }

    fun deleteConversation() {
        val peer = Rns.fromHex(currentPeer) ?: return
        closeConversation()
        perform { messenger.clearConversation(peer) }
    }

    fun peerCost(peer: ByteArray): Int = snapshot.destinations[Rns.hex(peer)]?.let { com.forkmesh.nativeapp.reticulum.Lxmf.stampCostFromAppData(it.appData) } ?: 0

    fun toggleNetwork() {
        val running = snapshot.running
        prefs?.edit()?.putBoolean(NETWORK_ON, !running)?.apply()
        if (running) {
            show("Disconnected from every interface. Nothing is sent until you connect again.")
            perform { stop() }
        } else {
            show("")
            perform { start() }
        }
    }

    fun announceNow() {
        val name = announceName.trim().take(64)
        prefs?.edit()?.putString(ANNOUNCE_NAME, name)?.apply()
        scope.launch(worker) {
            node.setAnnounceName(name)
            messenger.setDisplayName(name)
            val sent = node.announce()
            val online = node.onlineCount()
            val hash = node.destinationHash()
            withContext(Dispatchers.Main) {
                if (sent) show("Announced ${Rns.prettyHash(hash)} on $online interface${if (online == 1) "" else "s"}.")
                else show("Connect an interface first.", true)
            }
            publishNow()
        }
    }

    fun saveAnnounceName() {
        val name = announceName.trim().take(64)
        prefs?.edit()?.putString(ANNOUNCE_NAME, name)?.apply()
        perform {
            setAnnounceName(name)
            messenger.setDisplayName(name)
        }
    }

    fun changeAnnounceMinutes(minutes: Int) {
        announceMinutes = minutes
        prefs?.edit()?.putInt(ANNOUNCE_MINUTES, minutes)?.apply()
        restartAnnounceTimer()
    }

    private fun restartAnnounceTimer() {
        announceJob?.cancel()
        val minutes = announceMinutes
        if (minutes <= 0) return
        announceJob = scope.launch {
            while (true) {
                delay(minutes * 60_000L)
                perform { if (onlineCount() > 0) announce() }
            }
        }
    }

    fun changeAnswerPaths(on: Boolean) {
        answerPaths = on
        prefs?.edit()?.putBoolean(ANSWER_PATHS, on)?.apply()
        perform { setAnswerPathRequests(on) }
    }

    fun newIdentity() {
        val fresh = RnsIdentity.generate()
        if (secrets?.writeChecked(IDENTITY, Rns.hex(fresh.privateKey)) != true) {
            show("The new identity could not be stored in this phone's keystore, so it was not changed.", true)
            return
        }
        val hash = Rns.destinationHashForName(ReticulumNode.ASPECT, fresh.hash)
        perform { setIdentity(fresh) }
        show("New identity ${Rns.prettyHash(hash)}. Announce it so others can find this phone.")
    }

    fun requestPath(hash: ByteArray) {
        if (hash.size != Rns.HASH_LENGTH) return
        lookupInput = Rns.hex(hash)
        lookupError = ""
        scope.launch(worker) {
            val tag = node.requestPath(hash)
            withContext(Dispatchers.Main) { if (tag == null) show("Connect an interface first.", true) }
            publishNow()
        }
    }

    fun lookupTyped() {
        val hash = Rns.parseHash(lookupInput)
        if (hash == null) {
            lookupError = "A destination hash is 32 hexadecimal characters, like the ones in the list below."
            return
        }
        requestPath(hash)
    }

    fun select(hex: String, showMap: Boolean = false) {
        selected = hex
        if (showMap) tab = ReticulumTab.Network
    }

    fun forget(hex: String) {
        val hash = Rns.fromHex(hex) ?: return
        if (selected == hex) selected = ""
        perform { forgetDestination(hash) }
    }

    fun clearDestinations() {
        selected = ""
        perform { clearDestinations() }
    }

    fun addInterface(config: InterfaceConfig) {
        val existing = snapshot.interfaces.map { it.config }.firstOrNull { it.sameTarget(config) }
        if (existing != null) {
            show("${config.endpoint} is already in your interfaces as ${existing.name}.")
            perform {
                if (!existing.enabled) setInterfaceEnabled(existing.id, true)
                saveInterfaces(interfaces())
            }
            return
        }
        show("Added ${config.name} (${config.endpoint}).")
        prefs?.edit()?.putBoolean(NETWORK_ON, true)?.apply()
        perform {
            upsertInterface(config)
            if (!isRunning) start()
            saveInterfaces(interfaces())
        }
    }

    fun joinPublicNetwork() {
        val entry = ReticulumConfig.publicEntryPoints().first().copy(id = InterfaceConfig.newId())
        addInterface(entry)
        tab = ReticulumTab.Network
    }

    fun addPublicEntry(entry: InterfaceConfig) = addInterface(entry.copy(id = InterfaceConfig.newId()))

    fun updateInterface(config: InterfaceConfig) {
        perform {
            upsertInterface(config)
            saveInterfaces(interfaces())
        }
    }

    fun toggleInterface(id: String) {
        val config = snapshot.interfaces.firstOrNull { it.config.id == id }?.config ?: return
        perform {
            setInterfaceEnabled(id, !config.enabled)
            saveInterfaces(interfaces())
        }
    }

    fun reconnect(id: String) = perform { reconnectInterface(id) }

    fun removeInterface(id: String) {
        if (selectedInterface == id) selectedInterface = ""
        perform {
            removeInterface(id)
            saveInterfaces(interfaces())
        }
    }

    fun importConfig(text: String, source: String) {
        val skipped = ArrayList<String>()
        val found = ReticulumConfig.parse(text, skipped)
        scope.launch(worker) {
            var added = 0
            for (config in found) {
                if (node.interfaces().any { it.sameTarget(config) }) continue
                node.upsertInterface(config)
                added++
            }
            saveInterfaces(node.interfaces())
            var summary = "Imported $added interface${if (added == 1) "" else "s"} from $source."
            if (skipped.isNotEmpty()) summary += " Skipped (not supported here, or protected by a network name): ${skipped.joinToString(", ")}."
            withContext(Dispatchers.Main) { show(summary) }
            publishNow()
        }
    }

    companion object {
        const val PREFS = "market.packages.reticulum"
        const val INTERFACES = "interfaces"
        const val IDENTITY = "identity"
        const val ANNOUNCE_NAME = "announceName"
        const val ANNOUNCE_MINUTES = "announceMinutes"
        const val ANSWER_PATHS = "answerPathRequests"
        const val NETWORK_ON = "networkOn"
        const val MESSAGES = "messages"
        const val PASSPHRASES = "passphrases"
        const val PUBLISH_GAP_MS = 250L
        const val FIRST_ANNOUNCE_DELAY_MS = 3000L
        const val PULSE_MS = 1600L
        const val MAX_IMPORT_BYTES = 1024 * 1024

        val shared = ReticulumState()
    }
}
