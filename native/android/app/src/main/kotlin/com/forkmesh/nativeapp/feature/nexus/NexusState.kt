package com.forkmesh.nativeapp.feature.nexus

import android.content.ClipData
import android.content.Context
import android.content.Intent
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.os.Build
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.core.content.FileProvider
import com.forkmesh.nativeapp.feature.composer.PhotoPicker
import com.forkmesh.nativeapp.nexus.AuthorizationAttestation
import com.forkmesh.nativeapp.nexus.ConnectionFailure
import com.forkmesh.nativeapp.nexus.ConnectivityResult
import com.forkmesh.nativeapp.nexus.Device
import com.forkmesh.nativeapp.nexus.DeviceInventory
import com.forkmesh.nativeapp.nexus.DiscoveryOptions
import com.forkmesh.nativeapp.nexus.DiscoverySession
import com.forkmesh.nativeapp.nexus.DnsLookup
import com.forkmesh.nativeapp.nexus.DnsRecordType
import com.forkmesh.nativeapp.nexus.HttpInspection
import com.forkmesh.nativeapp.nexus.IPAddr
import com.forkmesh.nativeapp.nexus.LocalNetwork
import com.forkmesh.nativeapp.nexus.NetService
import com.forkmesh.nativeapp.nexus.NexusCancel
import com.forkmesh.nativeapp.nexus.NexusClient
import com.forkmesh.nativeapp.nexus.NexusConfiguration
import com.forkmesh.nativeapp.nexus.NexusHistory
import com.forkmesh.nativeapp.nexus.NexusReports
import com.forkmesh.nativeapp.nexus.ObservedChange
import com.forkmesh.nativeapp.nexus.PortList
import com.forkmesh.nativeapp.nexus.ScanAuthorizationPolicy
import com.forkmesh.nativeapp.nexus.ScanOptions
import com.forkmesh.nativeapp.nexus.ScanSession
import com.forkmesh.nativeapp.nexus.ServiceBrowsing
import com.forkmesh.nativeapp.platform.Prefs
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit

class NsdBrowser(context: Context) : ServiceBrowsing {
    private val nsd = context.applicationContext.getSystemService(Context.NSD_SERVICE) as NsdManager
    private val executor = Executors.newSingleThreadExecutor { Thread(it, "nexus-nsd").apply { isDaemon = true } }

    override fun browse(type: String, durationMs: Long, cancel: NexusCancel): List<NetService> {
        val found = LinkedHashMap<String, NsdServiceInfo>()
        var failure: Int? = null
        val stopped = CountDownLatch(1)
        val listener = object : NsdManager.DiscoveryListener {
            override fun onDiscoveryStarted(serviceType: String) {}
            override fun onDiscoveryStopped(serviceType: String) = stopped.countDown()
            override fun onStartDiscoveryFailed(serviceType: String, errorCode: Int) {
                failure = errorCode
                stopped.countDown()
            }
            override fun onStopDiscoveryFailed(serviceType: String, errorCode: Int) = stopped.countDown()
            override fun onServiceFound(info: NsdServiceInfo) {
                synchronized(found) { found[info.serviceName] = info }
            }
            override fun onServiceLost(info: NsdServiceInfo) {
                synchronized(found) { found.remove(info.serviceName) }
            }
        }
        nsd.discoverServices(type, NsdManager.PROTOCOL_DNS_SD, listener)
        val deadline = System.currentTimeMillis() + durationMs
        while (System.currentTimeMillis() < deadline && !cancel.isCancelled && failure == null) Thread.sleep(100)
        if (failure == null) {
            runCatching { nsd.stopServiceDiscovery(listener) }
            stopped.await(2, TimeUnit.SECONDS)
        }
        failure?.let { throw ConnectionFailure.System("Android could not browse $type (error $it).") }
        cancel.check()
        val services = synchronized(found) { found.values.toList() }
        return services.map { resolve(it, cancel) ?: describe(it, type) }.sortedBy { it.id }
    }

    @Suppress("DEPRECATION")
    private fun resolve(info: NsdServiceInfo, cancel: NexusCancel): NetService? {
        if (cancel.isCancelled) return null
        var resolved: NsdServiceInfo? = null
        val done = CountDownLatch(1)
        val callback = object : NsdManager.ResolveListener {
            override fun onResolveFailed(serviceInfo: NsdServiceInfo, errorCode: Int) = done.countDown()
            override fun onServiceResolved(serviceInfo: NsdServiceInfo) {
                resolved = serviceInfo
                done.countDown()
            }
        }
        runCatching {
            if (Build.VERSION.SDK_INT >= 34) nsd.resolveService(info, executor, callback) else nsd.resolveService(info, callback)
        }.onFailure { return null }
        if (!done.await(3, TimeUnit.SECONDS)) {
            if (Build.VERSION.SDK_INT >= 34) runCatching { nsd.stopServiceResolution(callback) }
            return null
        }
        return resolved?.let { describe(it, info.serviceType) }
    }

    @Suppress("DEPRECATION")
    private fun describe(info: NsdServiceInfo, browsedType: String): NetService {
        val hosts = if (Build.VERSION.SDK_INT >= 34) info.hostAddresses else listOfNotNull(info.host)
        val addresses = hosts.mapNotNull { IPAddr.parse(it.hostAddress?.substringBefore('%') ?: "") }.distinct().sorted()
        val type = info.serviceType.orEmpty().trim('.').ifEmpty { browsedType }.removeSuffix(".local")
        val txt = runCatching { info.attributes.mapValues { (_, value) -> value?.toString(Charsets.UTF_8) ?: "" } }.getOrDefault(emptyMap())
        val network = if (Build.VERSION.SDK_INT >= 33) runCatching { info.network?.toString() }.getOrNull() else null
        return NetService(
            name = info.serviceName.orEmpty(), type = type, port = info.port.takeIf { it > 0 },
            hostName = hosts.firstOrNull()?.hostName?.takeIf { IPAddr.parse(it) == null },
            addresses = addresses, txt = txt, interfaceName = network,
        )
    }
}

class NexusState private constructor(context: Context) {
    private val app = context.applicationContext
    private val prefs = Prefs(app, "nexus")
    private val historyFile = File(app.filesDir, "nexus/history.json")
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private val browser = NsdBrowser(app)

    var devices by mutableStateOf<List<Device>>(emptyList())
    var scans by mutableStateOf<List<ScanSession>>(emptyList())
    var discoveries by mutableStateOf<List<DiscoverySession>>(emptyList())
    var changes by mutableStateOf<List<ObservedChange>>(emptyList())
    var progress by mutableStateOf(0.0)
    var activity by mutableStateOf("Ready")
    var running by mutableStateOf(false)
    var error by mutableStateOf<String?>(null)
    var attestationPrompt by mutableStateOf<String?>(null)

    var allowPublicTargets by mutableStateOf(prefs.getBool(KEY_PUBLIC, false))
        private set
    var timeoutSeconds by mutableStateOf(prefs.getDouble(KEY_TIMEOUT, 1.5))
        private set
    var concurrency by mutableStateOf(prefs.getInt(KEY_CONCURRENCY, 48))
        private set
    var keepHistory by mutableStateOf(prefs.getBool(KEY_HISTORY, true))
        private set

    private var job: Job? = null
    private var cancel: NexusCancel? = null
    private var pendingScan: Pair<String, PortList>? = null

    val client: NexusClient
        get() = NexusClient(
            NexusConfiguration(ScanAuthorizationPolicy(allowPublicTargets = allowPublicTargets), ScanOptions(timeoutSeconds = timeoutSeconds, maximumConcurrency = concurrency)),
            browser = browser,
        )

    init {
        scope.launch {
            val snapshot = withContext(Dispatchers.IO) { runCatching { NexusHistory.decode(historyFile.readText()) }.getOrNull() }
            if (snapshot != null && keepHistory) {
                scans = snapshot.scans
                discoveries = snapshot.discoveries
                devices = snapshot.devices
            }
        }
    }

    fun updateAllowPublic(value: Boolean) {
        allowPublicTargets = value
        prefs.putBool(KEY_PUBLIC, value)
    }

    fun updateTimeout(value: Double) {
        timeoutSeconds = (Math.round(value.coerceIn(0.2, 30.0) * 10) / 10.0)
        prefs.putDouble(KEY_TIMEOUT, timeoutSeconds)
    }

    fun updateConcurrency(value: Int) {
        concurrency = value.coerceIn(1, 128)
        prefs.putInt(KEY_CONCURRENCY, concurrency)
    }

    fun updateKeepHistory(value: Boolean) {
        keepHistory = value
        prefs.putBool(KEY_HISTORY, value)
        save()
    }

    fun clearHistory() {
        scans = emptyList()
        discoveries = emptyList()
        devices = emptyList()
        changes = emptyList()
        save()
    }

    fun stop() {
        cancel?.cancel()
        activity = "Stopping"
    }

    fun discover() {
        if (running) return
        val token = NexusCancel()
        cancel = token
        running = true
        progress = 0.0
        activity = "Discovering services"
        val previous = devices
        job = scope.launch {
            val session = withContext(Dispatchers.IO) {
                client.discover(DiscoveryOptions(), token) { update ->
                    scope.launch {
                        devices = update.devices
                        progress = update.fraction
                        activity = "Discovering ${update.serviceType}"
                    }
                }
            }
            devices = session.devices
            changes = DeviceInventory(session.devices).changes(previous)
            discoveries = listOf(session) + discoveries
            activity = when (val status = session.status) {
                is com.forkmesh.nativeapp.nexus.SessionStatus.Failed -> "Discovery stopped".also { error = status.reason }
                com.forkmesh.nativeapp.nexus.SessionStatus.Cancelled -> "Discovery stopped"
                else -> "Discovery complete"
            }
            progress = 1.0
            running = false
            save()
        }
    }

    fun scan(target: String, ports: PortList, attestation: AuthorizationAttestation? = null) {
        if (running) return
        val token = NexusCancel()
        cancel = token
        running = true
        progress = 0.0
        activity = "Preparing scan"
        job = scope.launch {
            val outcome = withContext(Dispatchers.IO) {
                runCatching {
                    client.scan(target, ports, attestation = attestation, cancel = token) { update ->
                        scope.launch {
                            progress = update.fraction
                            activity = "Scanning ${update.completed} of ${update.total} ports"
                        }
                    }
                }
            }
            running = false
            outcome.onSuccess { session ->
                scans = listOf(session) + scans
                progress = 1.0
                activity = if (session.status == com.forkmesh.nativeapp.nexus.SessionStatus.Cancelled) "Scan stopped" else "Scan complete"
                save()
            }.onFailure { failure ->
                activity = "Scan stopped"
                progress = 0.0
                if (failure is ConnectionFailure.AttestationRequired) {
                    pendingScan = target to ports
                    attestationPrompt = failure.detail
                } else {
                    error = failure.message ?: "The scan could not start."
                }
            }
        }
    }

    fun confirmAttestation() {
        val pending = pendingScan ?: return
        attestationPrompt = null
        pendingScan = null
        scan(pending.first, pending.second, AuthorizationAttestation())
    }

    fun dismissAttestation() {
        attestationPrompt = null
        pendingScan = null
    }

    suspend fun lookup(name: String, type: DnsRecordType): DnsLookup = withContext(Dispatchers.IO) { client.dnsLookup(name, type) }

    suspend fun inspect(url: String): HttpInspection = withContext(Dispatchers.IO) { client.inspectWeb(url) }

    suspend fun reach(host: String, port: Int): ConnectivityResult = withContext(Dispatchers.IO) { client.checkConnectivity(host.trim(), port) }

    fun localNetwork(): String? = LocalNetwork.current()?.toString()

    fun share(context: Context, session: ScanSession, csv: Boolean) {
        val extension = if (csv) "csv" else "json"
        val text = if (csv) NexusReports.csv(session) else NexusReports.scanJson(session)
        runCatching {
            val folder = File(context.cacheDir, "nexus-share").apply { mkdirs() }
            val file = File(folder, NexusReports.fileName(session, extension))
            file.writeText(text)
            val uri = FileProvider.getUriForFile(context, PhotoPicker.authority(context), file)
            val send = Intent(Intent.ACTION_SEND)
                .setType(if (csv) "text/csv" else "application/json")
                .putExtra(Intent.EXTRA_STREAM, uri)
                .putExtra(Intent.EXTRA_SUBJECT, "NEXUS scan of ${session.target}")
                .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
            send.clipData = ClipData.newRawUri(file.name, uri)
            context.startActivity(Intent.createChooser(send, "Share scan report").addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
        }.onFailure { error = "The report could not be shared." }
    }

    private fun save() {
        val snapshot = if (keepHistory) NexusHistory.Snapshot(scans, discoveries, devices) else NexusHistory.Snapshot()
        scope.launch(Dispatchers.IO) {
            runCatching {
                historyFile.parentFile?.mkdirs()
                historyFile.writeText(NexusHistory.encode(snapshot))
            }
        }
    }

    companion object {
        private const val KEY_PUBLIC = "nexus.allowPublicTargets"
        private const val KEY_TIMEOUT = "nexus.timeout"
        private const val KEY_CONCURRENCY = "nexus.concurrency"
        private const val KEY_HISTORY = "nexus.keepHistory"

        @Volatile
        private var instance: NexusState? = null

        fun shared(context: Context): NexusState = instance ?: synchronized(this) { instance ?: NexusState(context).also { instance = it } }
    }
}
