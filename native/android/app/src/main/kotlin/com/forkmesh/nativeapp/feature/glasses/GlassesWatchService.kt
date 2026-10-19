package com.forkmesh.nativeapp.feature.glasses

import android.Manifest
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothManager
import android.bluetooth.le.BluetoothLeScanner
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import com.forkmesh.nativeapp.R
import com.forkmesh.nativeapp.glasses.GlassesAdvert
import com.forkmesh.nativeapp.glasses.GlassesCensus
import com.forkmesh.nativeapp.glasses.GlassesHoming
import com.forkmesh.nativeapp.glasses.GlassesHomingReading
import com.forkmesh.nativeapp.glasses.GlassesMaker
import com.forkmesh.nativeapp.glasses.GlassesRange
import com.forkmesh.nativeapp.glasses.GlassesSighting
import com.forkmesh.nativeapp.glasses.GlassesSignatures
import com.forkmesh.nativeapp.glasses.GlassesTracker
import com.forkmesh.nativeapp.glasses.NearbyDevice
import com.forkmesh.nativeapp.glasses.NearbyKinds
import com.forkmesh.nativeapp.net.RadioPolicy
import com.forkmesh.nativeapp.net.ScanDuty
import com.forkmesh.nativeapp.platform.AppLifecycle
import com.forkmesh.nativeapp.platform.AudioActivityProbe
import com.forkmesh.nativeapp.platform.Prefs
import com.forkmesh.nativeapp.platform.scanMode
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

data class GlassesWatchState(
    val running: Boolean = false,
    val watching: Boolean = false,
    val ringing: Boolean = false,
    val nearby: List<GlassesSighting> = emptyList(),
    val problem: String? = null,
    val homing: GlassesHomingReading? = null,
    val others: List<NearbyDevice> = emptyList(),
    val listening: Boolean = false,
    val tracked: GlassesHomingReading? = null,
)

class GlassesWatchService : Service() {
    companion object {
        const val ACTION_START = "com.forkmesh.glasses.START"
        const val ACTION_STOP = "com.forkmesh.glasses.STOP"
        private const val STATUS_CHANNEL = "glasses_watch"
        private const val STATUS_NOTIFICATION_ID = 7470
        private const val REFRESH_MS = 5_000L
        private const val HOMING_MS = 500L
        private const val CENSUS_MS = 1_000L
        private const val PREFS = "glasses_alert"
        private const val KEY_RANGE = "range"
        private const val KEY_WATCHING = "watching"

        private val mutableState = MutableStateFlow(GlassesWatchState())
        val state: StateFlow<GlassesWatchState> = mutableState
        private val trackRequest = MutableStateFlow<NearbyDevice?>(null)

        fun track(device: NearbyDevice?) {
            trackRequest.value = device
            if (device == null) mutableState.update { it.copy(tracked = null) }
        }

        fun range(context: Context): GlassesRange = GlassesRange.of(Prefs(context, PREFS).getString(KEY_RANGE))

        fun setRange(context: Context, range: GlassesRange) = Prefs(context, PREFS).putString(KEY_RANGE, range.wire)

        fun wantsWatching(context: Context): Boolean = Prefs(context, PREFS).getBool(KEY_WATCHING, false)

        fun runtimePermissions(): Array<String> = buildList {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) add(Manifest.permission.BLUETOOTH_SCAN) else add(Manifest.permission.ACCESS_FINE_LOCATION)
            if (Build.VERSION.SDK_INT >= 33) add(Manifest.permission.POST_NOTIFICATIONS)
        }.toTypedArray()

        fun scanPermitted(context: Context): Boolean {
            val needed = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) Manifest.permission.BLUETOOTH_SCAN else Manifest.permission.ACCESS_FINE_LOCATION
            return context.checkSelfPermission(needed) == PackageManager.PERMISSION_GRANTED
        }

        fun start(context: Context) {
            Prefs(context, PREFS).putBool(KEY_WATCHING, true)
            context.startForegroundService(Intent(context, GlassesWatchService::class.java).setAction(ACTION_START))
        }

        fun stop(context: Context) {
            Prefs(context, PREFS).putBool(KEY_WATCHING, false)
            context.startService(Intent(context, GlassesWatchService::class.java).setAction(ACTION_STOP))
        }

        fun silence(context: Context) = GlassesAlarm.get(context).stop()

        fun test(context: Context) =
            GlassesAlarm.get(context).ring(GlassesSighting("test", "Test alarm", GlassesMaker.LUXOTTICA, true, -58, System.currentTimeMillis()))

        internal fun publishRinging(ringing: Boolean) = mutableState.update { it.copy(ringing = ringing) }

    }

    private val main = Handler(Looper.getMainLooper())
    private val tracker = GlassesTracker()
    private val homing = GlassesHoming()
    private val census = GlassesCensus()
    private val trackHoming = GlassesHoming()
    private var censusOn = false
    private var foreground = false
    private var foregroundJob: Job? = null
    private val alarm: GlassesAlarm get() = GlassesAlarm.get(this)
    private var scanning = false
    private var duty: ScanDuty? = null
    private var adapterReceiver: BroadcastReceiver? = null

    private val refresher = object : Runnable {
        override fun run() {
            retune()
            publishNearby()
            main.postDelayed(this, REFRESH_MS)
        }
    }

    private val homingTicker = object : Runnable {
        override fun run() {
            publishHoming()
            if (homing.target != null) main.postDelayed(this, HOMING_MS)
        }
    }

    private val censusTicker = object : Runnable {
        override fun run() {
            publishCensus()
            if (censusOn) main.postDelayed(this, CENSUS_MS)
        }
    }

    private val censusCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            main.post { hearOther(result) }
        }

        override fun onBatchScanResults(results: MutableList<ScanResult>) {
            main.post { results.forEach(::hearOther) }
        }

        override fun onScanFailed(errorCode: Int) {
            main.post {
                censusOn = false
                mutableState.update { it.copy(listening = false) }
            }
        }
    }

    private val callback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            main.post { handle(result) }
        }

        override fun onBatchScanResults(results: MutableList<ScanResult>) {
            main.post { results.forEach(::handle) }
        }

        override fun onScanFailed(errorCode: Int) {
            main.post {
                scanning = false
                mutableState.update {
                    it.copy(watching = false, problem = "Bluetooth scanning stopped (error $errorCode). Turn Bluetooth off and on, then start again.")
                }
            }
        }
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        val receiver = object : BroadcastReceiver() {
            override fun onReceive(context: Context, intent: Intent) {
                when (intent.getIntExtra(BluetoothAdapter.EXTRA_STATE, BluetoothAdapter.ERROR)) {
                    BluetoothAdapter.STATE_ON -> startScan()
                    BluetoothAdapter.STATE_TURNING_OFF, BluetoothAdapter.STATE_OFF -> {
                        stopScan()
                        mutableState.update { it.copy(problem = "Bluetooth is off. Watching starts again when you turn it on.") }
                    }
                }
            }
        }
        val filter = IntentFilter(BluetoothAdapter.ACTION_STATE_CHANGED)
        if (Build.VERSION.SDK_INT >= 33) {
            registerReceiver(receiver, filter, Context.RECEIVER_NOT_EXPORTED)
        } else {
            registerReceiver(receiver, filter)
        }
        adapterReceiver = receiver
        foregroundJob = AppLifecycle.scope.launch {
            AppLifecycle.foreground.collect {
                foreground = it
                syncCensus()
            }
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP || !scanPermitted(this)) {
            if (intent?.action != ACTION_STOP) {
                mutableState.update { it.copy(watching = false, problem = "Allow Nearby devices so ForkMesh can scan Bluetooth.") }
            }
            shutDown()
            return START_NOT_STICKY
        }
        promote()
        mutableState.update { it.copy(running = true) }
        startScan()
        return START_STICKY
    }

    override fun onDestroy() {
        adapterReceiver?.let { runCatching { unregisterReceiver(it) } }
        adapterReceiver = null
        foregroundJob?.cancel()
        foregroundJob = null
        stopScan()
        alarm.stop()
        main.removeCallbacksAndMessages(null)
        mutableState.update { it.copy(running = false, watching = false, nearby = emptyList(), homing = null, others = emptyList(), tracked = null) }
        super.onDestroy()
    }

    override fun onTimeout(startId: Int, fgsType: Int) {
        shutDown()
    }

    private fun shutDown() {
        stopScan()
        alarm.stop()
        stopForeground(STOP_FOREGROUND_REMOVE)
        stopSelf()
    }

    private fun promote() {
        ensureStatusChannel()
        val stopIntent = PendingIntent.getService(
            this, 2, Intent(this, GlassesWatchService::class.java).setAction(ACTION_STOP),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        val open = packageManager.getLaunchIntentForPackage(packageName)?.let {
            PendingIntent.getActivity(this, 3, it, PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
        }
        val notification = Notification.Builder(this, STATUS_CHANNEL)
            .setSmallIcon(R.drawable.ic_stat_ping)
            .setContentTitle("Glasses Alert is watching")
            .setContentText("Scanning Bluetooth for Meta smart glasses near you")
            .setOngoing(true)
            .setShowWhen(false)
            .setCategory(Notification.CATEGORY_SERVICE)
            .apply { open?.let(::setContentIntent) }
            .addAction(Notification.Action.Builder(null, "Stop watching", stopIntent).build())
            .build()
        if (Build.VERSION.SDK_INT >= 34) {
            startForeground(STATUS_NOTIFICATION_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE)
        } else {
            startForeground(STATUS_NOTIFICATION_ID, notification)
        }
    }

    private fun ensureStatusChannel() {
        val manager = getSystemService(NotificationManager::class.java) ?: return
        if (manager.getNotificationChannel(STATUS_CHANNEL) != null) return
        manager.createNotificationChannel(
            NotificationChannel(STATUS_CHANNEL, "Glasses Alert watching", NotificationManager.IMPORTANCE_LOW).apply {
                description = "Shown while Glasses Alert scans Bluetooth for smart glasses."
                setShowBadge(false)
            },
        )
    }

    private fun startScan() {
        if (scanning) return
        val adapter = getSystemService(BluetoothManager::class.java)?.adapter
        val scanner = adapter?.takeIf { it.isEnabled }?.bluetoothLeScanner
        if (scanner == null) {
            val problem = if (adapter == null) "This phone has no Bluetooth." else "Bluetooth is off. Watching starts again when you turn it on."
            mutableState.update { it.copy(watching = false, problem = problem) }
            if (adapter == null) shutDown()
            return
        }
        val wanted = RadioPolicy.glassesDuty(AudioActivityProbe.current(this))
        if (!launchScan(scanner, wanted)) {
            mutableState.update { it.copy(watching = false, problem = "Android refused the Bluetooth scan. Check Nearby devices permission.") }
            shutDown()
            return
        }
        scanning = true
        duty = wanted
        tracker.reset()
        homing.reset()
        mutableState.update { it.copy(watching = true, problem = null, nearby = emptyList(), homing = null) }
        main.removeCallbacks(refresher)
        main.postDelayed(refresher, REFRESH_MS)
        syncCensus()
    }

    private fun launchScan(scanner: BluetoothLeScanner, wanted: ScanDuty): Boolean {
        val filters = GlassesSignatures.COMPANY_IDS.map { id ->
            ScanFilter.Builder().setManufacturerData(id, ByteArray(0), ByteArray(0)).build()
        }
        val settings = ScanSettings.Builder()
            .setScanMode(wanted.scanMode)
            .setCallbackType(ScanSettings.CALLBACK_TYPE_ALL_MATCHES)
            .build()
        return try {
            scanner.startScan(filters, settings, callback)
            true
        } catch (_: SecurityException) {
            false
        } catch (_: IllegalStateException) {
            false
        }
    }

    private fun retune() {
        if (!scanning) return
        val wanted = RadioPolicy.glassesDuty(AudioActivityProbe.current(this))
        if (wanted != duty) {
            val scanner = getSystemService(BluetoothManager::class.java)?.adapter?.takeIf { it.isEnabled }?.bluetoothLeScanner ?: return
            runCatching { scanner.stopScan(callback) }
            if (!launchScan(scanner, wanted)) {
                mutableState.update { it.copy(watching = false, problem = "Android refused the Bluetooth scan. Check Nearby devices permission.") }
                shutDown()
                return
            }
            duty = wanted
        }
        syncCensus()
    }

    private fun syncCensus() {
        if (scanning && RadioPolicy.glassesCensus(AudioActivityProbe.current(this), foreground)) startCensus() else stopCensus()
    }

    private fun startCensus() {
        if (censusOn) return
        val scanner = getSystemService(BluetoothManager::class.java)?.adapter?.takeIf { it.isEnabled }?.bluetoothLeScanner ?: return
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_BALANCED)
            .setCallbackType(ScanSettings.CALLBACK_TYPE_ALL_MATCHES)
            .build()
        censusOn = runCatching { scanner.startScan(null, settings, censusCallback) }.isSuccess
        if (!censusOn) return
        census.reset()
        mutableState.update { it.copy(listening = true, others = emptyList()) }
        main.removeCallbacks(censusTicker)
        main.postDelayed(censusTicker, CENSUS_MS)
    }

    private fun stopCensus() {
        main.removeCallbacks(censusTicker)
        if (!censusOn) return
        censusOn = false
        runCatching { getSystemService(BluetoothManager::class.java)?.adapter?.bluetoothLeScanner?.stopScan(censusCallback) }
        census.reset()
        trackHoming.reset()
        mutableState.update { it.copy(listening = false, others = emptyList(), tracked = null) }
    }

    private fun stopScan() {
        main.removeCallbacks(refresher)
        main.removeCallbacks(homingTicker)
        stopCensus()
        if (!scanning) return
        scanning = false
        duty = null
        runCatching { getSystemService(BluetoothManager::class.java)?.adapter?.bluetoothLeScanner?.stopScan(callback) }
        homing.reset()
        mutableState.update { it.copy(watching = false, homing = null) }
    }

    private fun advert(result: ScanResult): GlassesAdvert? {
        val record = result.scanRecord
        val makers = record?.manufacturerSpecificData
        val ids = buildList { if (makers != null) for (i in 0 until makers.size()) add(makers.keyAt(i)) }
        val name = record?.deviceName ?: runCatching { result.device?.name }.getOrNull()
        val address = runCatching { result.device?.address }.getOrNull() ?: return null
        val services = (record?.serviceUuids.orEmpty() + record?.serviceData?.keys.orEmpty())
            .mapNotNull { NearbyKinds.shortUuid(it.uuid) }
        return GlassesAdvert(address, name, result.rssi, ids, services, NearbyKinds.appearance(record?.bytes))
    }

    private fun handle(result: ScanResult) {
        val advert = advert(result) ?: return
        handle(advert)
    }

    private fun handle(advert: GlassesAdvert) {
        val now = System.currentTimeMillis()
        val sighting = GlassesSignatures.match(advert, range(this), now)
        if (sighting != null) {
            if (tracker.observe(sighting)) alarm.ring(sighting)
            publishNearby()
        }
        homing.hear(advert.address, advert.rssi, GlassesCompass.heading.value, now)
    }

    private fun hearOther(result: ScanResult) {
        if (!scanning) return
        val advert = advert(result) ?: return
        trackHoming.hear(advert.address, advert.rssi, GlassesCompass.heading.value, System.currentTimeMillis())
        if (advert.companyIds.any { it in GlassesSignatures.COMPANY_IDS }) return
        if (GlassesSignatures.nameMatches(advert.name)) {
            handle(advert)
            return
        }
        NearbyKinds.device(advert, System.currentTimeMillis())?.let(census::observe)
    }

    private fun publishCensus() {
        val now = System.currentTimeMillis()
        val others = census.devices(now)
        val tracked = followTracked(others, now)
        mutableState.update { it.copy(others = others, tracked = tracked) }
    }

    private fun followTracked(others: List<NearbyDevice>, now: Long): GlassesHomingReading? {
        val wanted = trackRequest.value
        if (wanted == null) {
            trackHoming.reset()
            return null
        }
        if (trackHoming.target?.address != wanted.address) trackHoming.reset()
        val device = others.firstOrNull { it.address == wanted.address } ?: wanted
        trackHoming.follow(listOf(GlassesSighting(device.address, device.label, null, false, device.rssi, now)), now)
        return trackHoming.reading(now)
    }

    private fun publishNearby() {
        val now = System.currentTimeMillis()
        val nearby = tracker.nearby(now)
        val had = homing.target != null
        homing.follow(nearby, now)
        mutableState.update { it.copy(nearby = nearby) }
        if (!had && homing.target != null) {
            main.removeCallbacks(homingTicker)
            main.post(homingTicker)
        } else if (homing.target == null) {
            publishHoming()
        }
    }

    private fun publishHoming() {
        val reading = homing.reading(System.currentTimeMillis())
        mutableState.update { it.copy(homing = reading) }
    }
}
