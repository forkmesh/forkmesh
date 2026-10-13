package com.forkmesh.nativeapp.feature.push

import com.forkmesh.nativeapp.push.DIRECT_MESSAGES_TOPIC
import android.app.NotificationManager
import android.app.Service
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.ServiceInfo
import android.net.ConnectivityManager
import android.net.Network
import android.os.Build
import android.os.IBinder
import android.os.PowerManager
import com.forkmesh.nativeapp.api.AccountEventLoop
import com.forkmesh.nativeapp.push.LinkPlan
import com.forkmesh.nativeapp.push.LinkSupervisor
import com.forkmesh.nativeapp.push.ReconnectBackoff
import com.forkmesh.nativeapp.push.diffLinks
import java.util.concurrent.Executors

class BackgroundLinkService : Service() {
    private class Link(val account: LinkAccount, val api: com.forkmesh.nativeapp.api.ApiService, val loop: AccountEventLoop)

    private val links = LinkedHashMap<String, Link>()
    private val supervisor = LinkSupervisor()
    private val worker = Executors.newSingleThreadExecutor { Thread(it, "BackgroundLink").apply { isDaemon = true } }
    private var plan: LinkPlan = LinkPlan.OFF
    private var live = false
    private var callback: ConnectivityManager.NetworkCallback? = null
    private var idleReceiver: BroadcastReceiver? = null

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        instance = this
        watchNetwork()
        watchIdle()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        promote(plan.statusText(supervisor.connected()).ifEmpty { "Connecting" })
        apply()
        return START_STICKY
    }

    override fun onDestroy() {
        instance = null
        callback?.let { runCatching { getSystemService(ConnectivityManager::class.java)?.unregisterNetworkCallback(it) } }
        idleReceiver?.let { runCatching { unregisterReceiver(it) } }
        worker.execute { closeAll() }
        worker.shutdown()
        super.onDestroy()
    }

    override fun onTimeout(startId: Int, fgsType: Int) {
        stopSelf()
    }

    private fun promote(text: String) {
        val notification = PingNotifier.linkNotification(this, text, plan.title)
        if (Build.VERSION.SDK_INT >= 34) {
            startForeground(PingNotifier.LINK_NOTIFICATION_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_REMOTE_MESSAGING)
        } else {
            startForeground(PingNotifier.LINK_NOTIFICATION_ID, notification)
        }
    }

    fun apply() = worker.execute { applyNow() }

    fun stopLinks() = worker.execute { closeAll() }

    private fun applyNow() {
        val accounts = BackgroundLink.accounts()
        val next = BackgroundLink.plan(this, accounts)
        plan = next
        if (!next.runService) {
            closeAll()
            stopSelf()
            return
        }
        val diff = diffLinks(links.keys, next)
        for (id in diff.close) close(id)
        val byId = accounts.associateBy { it.id }
        for (id in diff.keep) {
            val account = byId[id] ?: continue
            if (links[id]?.account?.session?.sessionToken != account.session.sessionToken) {
                close(id)
                open(account)
            }
        }
        for (id in diff.open) byId[id]?.let(::open)
        setLive(next.live)
        refreshStatus()
    }

    private fun open(account: LinkAccount) {
        val api = account.api()
        val loop = AccountEventLoop(api, sessionToken = { account.session.sessionToken }, backoff = ReconnectBackoff())
        val link = Link(account, api, loop)
        loop.onTopic = { topic ->
            when (topic) {
                "pings" -> worker.execute { deliver(link) }
                DIRECT_MESSAGES_TOPIC -> worker.execute { deliverDirectMessage(link) }
            }
        }
        loop.onConnected = {
            supervisor.onConnected(account.id)
            worker.execute {
                deliver(link)
                refreshStatus()
            }
        }
        loop.onStateChanged = {
            if (!loop.connected) supervisor.onDropped(account.id)
            worker.execute { refreshStatus() }
        }
        loop.onSignedOut = {
            supervisor.onSignedOut(account.id)
            worker.execute {
                close(account.id)
                refreshStatus()
            }
        }
        links[account.id] = link
        loop.setForeground(live)
        loop.start()
    }

    private fun deliver(link: Link) {
        val account = link.account
        val many = links.size > 1 || BackgroundLink.accounts().size > 1
        if (PingAlerts.fetchAndDeliver(this, account.id, account.label, many, link.api, account.node)) {
            supervisor.markCaughtUp(account.id)
        }
    }

    private fun deliverDirectMessage(link: Link) {
        val account = link.account
        val many = links.size > 1 || BackgroundLink.accounts().size > 1
        PingAlerts.deliverDirectMessage(this, account.id, account.label, many, link.api, account.node)
    }

    private fun close(id: String) {
        val link = links.remove(id) ?: return
        supervisor.forget(id)
        runCatching {
            link.loop.stop()
            link.loop.dispose()
        }
    }

    private fun closeAll() {
        for (id in links.keys.toList()) close(id)
    }

    private fun setLive(value: Boolean) {
        val changed = value != live
        live = value
        for (link in links.values) link.loop.setForeground(value)
        if (value && changed) {
            for (link in links.values) if (supervisor.catchUpDue(link.account.id)) deliver(link)
        }
    }

    private fun refreshStatus() {
        if (!plan.runService) return
        val manager = getSystemService(NotificationManager::class.java) ?: return
        runCatching {
            manager.notify(PingNotifier.LINK_NOTIFICATION_ID, PingNotifier.linkNotification(this, plan.statusText(supervisor.connected()), plan.title))
        }
    }

    private fun conditionsChanged() = worker.execute {
        plan = BackgroundLink.plan(this)
        setLive(plan.live)
        refreshStatus()
    }

    private fun watchNetwork() {
        val manager = getSystemService(ConnectivityManager::class.java) ?: return
        val cb = object : ConnectivityManager.NetworkCallback() {
            override fun onAvailable(network: Network) = conditionsChanged()

            override fun onLost(network: Network) = conditionsChanged()
        }
        runCatching { manager.registerDefaultNetworkCallback(cb) }.onSuccess { callback = cb }
    }

    private fun watchIdle() {
        val receiver = object : BroadcastReceiver() {
            override fun onReceive(context: Context, intent: Intent) = conditionsChanged()
        }
        val filter = IntentFilter(PowerManager.ACTION_DEVICE_IDLE_MODE_CHANGED)
        if (Build.VERSION.SDK_INT >= 33) {
            registerReceiver(receiver, filter, Context.RECEIVER_NOT_EXPORTED)
        } else {
            registerReceiver(receiver, filter)
        }
        idleReceiver = receiver
    }

    fun catchUpDue(id: String): Boolean = supervisor.catchUpDue(id)

    companion object {
        @Volatile
        var instance: BackgroundLinkService? = null
            private set
    }
}
