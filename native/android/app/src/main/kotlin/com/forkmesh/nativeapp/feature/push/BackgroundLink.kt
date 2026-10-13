package com.forkmesh.nativeapp.feature.push

import android.app.job.JobInfo
import android.app.job.JobScheduler
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.net.ConnectivityManager
import android.net.NetworkCapabilities
import android.os.PowerManager
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.api.ClientSettings
import com.forkmesh.nativeapp.account.projects.Project
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.projects.ProjectsService
import com.forkmesh.nativeapp.model.auth.AuthSession
import com.forkmesh.nativeapp.platform.Prefs
import com.forkmesh.nativeapp.push.BackgroundLinkKeys
import com.forkmesh.nativeapp.push.LinkConditions
import com.forkmesh.nativeapp.push.LinkPlan
import com.forkmesh.nativeapp.push.LinkProject

class LinkAccount(val project: Project, val session: AuthSession) {
    val id: String get() = project.id
    val label: String get() = project.label.ifEmpty { project.id }
    val node: String get() = session.nodeName

    fun api(): ApiService {
        val server = project.serverUrl
        val api = ApiService(
            object : ClientSettings {
                override val serverUrl: String = server
                override val room: String = ""
            },
        )
        val token = session.sessionToken
        api.sessionTokenProvider = { token }
        return api
    }
}

object BackgroundLink {
    const val JOB_ID = 7302
    private const val JOB_PERIOD_MS = 15 * 60_000L

    @Volatile
    var projects: ProjectsService? = null

    fun projectsService(): ProjectsService? = projects ?: Services.get<ProjectsService>()?.also { projects = it }

    fun isEnabled(context: Context, projectId: String): Boolean =
        Prefs(context).getBool(BackgroundLinkKeys.enabled(projectId), BackgroundLinkKeys.DEFAULT_ENABLED)

    fun setEnabled(context: Context, projectId: String, enabled: Boolean) {
        Prefs(context).putBool(BackgroundLinkKeys.enabled(projectId), enabled)
        if (enabled) PingAlerts.prime(context, projectId, restart = true)
        sync(context)
    }

    fun accounts(): List<LinkAccount> {
        val service = projectsService() ?: return emptyList()
        return service.directory.projects.mapNotNull { project ->
            if (!project.hasInstance) return@mapNotNull null
            val session = service.sessionFor(project.id) ?: return@mapNotNull null
            if (session.sessionToken.isEmpty() || session.nodeName.isEmpty()) null else LinkAccount(project, session)
        }
    }

    fun networkAvailable(context: Context): Boolean {
        val manager = context.getSystemService(ConnectivityManager::class.java) ?: return true
        val caps = manager.getNetworkCapabilities(manager.activeNetwork ?: return false) ?: return false
        return caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
    }

    fun deviceIdle(context: Context): Boolean =
        context.getSystemService(PowerManager::class.java)?.isDeviceIdleMode == true

    fun plan(context: Context, accounts: List<LinkAccount> = accounts()): LinkPlan {
        val prefs = Prefs(context)
        val projects = accounts.map {
            LinkProject(it.id, it.label, signedIn = true, enabled = prefs.getBool(BackgroundLinkKeys.enabled(it.id), BackgroundLinkKeys.DEFAULT_ENABLED))
        }
        return LinkPlan.of(
            projects,
            LinkConditions(
                notificationsAllowed = PingNotifier.allowed(context),
                networkAvailable = networkAvailable(context),
                deviceIdle = deviceIdle(context),
            ),
        )
    }

    private val syncer = java.util.concurrent.Executors.newSingleThreadExecutor { Thread(it, "BackgroundLinkSync").apply { isDaemon = true } }

    fun sync(context: Context) {
        val app = context.applicationContext
        syncer.execute { runCatching { syncNow(app) } }
    }

    fun syncNow(context: Context) {
        val app = context.applicationContext
        val accounts = accounts()
        val plan = plan(app, accounts)
        for (account in accounts) PingAlerts.prime(app, account.id, restart = false)
        scheduleRevive(app, plan.runService)
        val running = BackgroundLinkService.instance
        if (!plan.runService) {
            running?.stopLinks()
            if (running != null) app.stopService(Intent(app, BackgroundLinkService::class.java))
            return
        }
        if (running != null) {
            running.apply()
            return
        }
        start(app)
    }

    fun start(context: Context): Boolean = try {
        context.startForegroundService(Intent(context, BackgroundLinkService::class.java))
        true
    } catch (_: IllegalStateException) {
        false
    } catch (_: SecurityException) {
        false
    }

    private fun scheduleRevive(context: Context, wanted: Boolean) {
        val scheduler = context.getSystemService(JobScheduler::class.java) ?: return
        if (!wanted) {
            scheduler.cancel(JOB_ID)
            return
        }
        if (scheduler.getPendingJob(JOB_ID) != null) return
        val job = JobInfo.Builder(JOB_ID, ComponentName(context, BackgroundLinkJob::class.java))
            .setPeriodic(JOB_PERIOD_MS)
            .setRequiredNetworkType(JobInfo.NETWORK_TYPE_ANY)
            .setPersisted(true)
            .build()
        runCatching { scheduler.schedule(job) }
    }
}
