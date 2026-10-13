package com.forkmesh.nativeapp.feature.planning

import android.app.AlarmManager
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.os.Build
import com.forkmesh.nativeapp.App
import com.forkmesh.nativeapp.R
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.PrefsStore
import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.feature.push.PingNotifier
import com.forkmesh.nativeapp.navigation.RepoRef
import com.forkmesh.nativeapp.navigation.Route
import com.forkmesh.nativeapp.planning.CalendarMode
import com.forkmesh.nativeapp.planning.PlanningRepo
import com.forkmesh.nativeapp.planning.PlanningTimer
import com.forkmesh.nativeapp.planning.PlanningTimerStore
import com.forkmesh.nativeapp.planning.TaskListFilter
import com.forkmesh.nativeapp.planning.TimerPings
import com.forkmesh.nativeapp.platform.Prefs
import com.forkmesh.nativeapp.platform.ProjectLifecycle
import com.forkmesh.nativeapp.ui.navigation.Screen
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

object PlanningFeature {
    private const val OWNER = "WS15"
    private var store: PlanningTimerStore? = null
    private val timersState = MutableStateFlow<List<PlanningTimer>>(emptyList())

    val timers: StateFlow<List<PlanningTimer>> = timersState.asStateFlow()

    fun timerStore(context: Context): PlanningTimerStore = store ?: PlanningTimerStore(PrefsStore(Prefs(context, TimerAlarms.PREFS))).also { created ->
        store = created
        timersState.value = created.timers
        created.onChange = { list ->
            timersState.value = list
            TimerAlarms.schedule(context.applicationContext, created)
        }
    }

    fun install(app: App) {
        val timers = timerStore(app)
        TimerAlarms.schedule(app, timers)
        forgetPlanningModels()
        ProjectLifecycle.onClose { forgetPlanningModels() }
        ProjectLifecycle.scope.launch { settleTimers(app) }
        register()
    }

    suspend fun settleTimers(context: Context) {
        val timers = timerStore(context)
        val rung = timers.settle()
        if (rung.isEmpty()) return
        rung.forEach { TimerAlarms.notify(context, it) }
        val api = Services.get<ApiService>() ?: return
        withContext(Dispatchers.IO) { TimerPings.post(rung, api, System.currentTimeMillis()) }
    }

    private fun register() {
        NavigationFeature.register(Route.Kind.Calendar, OWNER) { route ->
            route as Route.Calendar
            calendarScreen(route.date, CalendarMode.of(route.mode.raw) ?: CalendarMode.MONTH)
        }
        NavigationFeature.register(Route.Kind.Event, OWNER) { route -> eventScreen((route as Route.Event).id) }
        NavigationFeature.register(Route.Kind.Timers, OWNER) { timersScreen() }
        NavigationFeature.register(Route.Kind.Project, OWNER) { route ->
            route as Route.Project
            projectsScreen(route.repo, route.id.toIntOrNull())
        }
        NavigationFeature.register(Route.Kind.Milestones, OWNER) { route -> milestonesScreen((route as Route.Milestones).repo) }
        NavigationFeature.register(Route.Kind.Tasks, OWNER) { route -> tasksScreen(TaskListFilter.of((route as Route.Tasks).filter)) }
        NavigationFeature.register(Route.Kind.Task, OWNER) { route -> taskScreen((route as Route.Task).id) }
    }

    fun calendarScreen(date: String? = null, mode: CalendarMode = CalendarMode.MONTH): Screen = CalendarScreen(date, mode)

    fun eventScreen(id: String): Screen = EventScreen(id)

    fun timersScreen(): Screen = TimersScreen()

    fun projectsScreen(repo: RepoRef, number: Int? = null): Screen =
        if (number != null && number > 0) ProjectDetailScreen(PlanningRepo(repo.owner, repo.name), number) else ProjectsScreen(PlanningRepo(repo.owner, repo.name))

    fun milestonesScreen(repo: RepoRef): Screen = MilestonesScreen(PlanningRepo(repo.owner, repo.name))

    fun tasksScreen(filter: TaskListFilter = TaskListFilter.MINE): Screen = com.forkmesh.nativeapp.feature.tools.TasksScreen(filter.ordinal)

    fun taskScreen(id: String): Screen = TaskToolsScreen(id)
}

object TimerAlarms {
    const val PREFS = "forkmesh-timers"
    const val CHANNEL = "timers"
    private const val EXTRA_ID = "timer"
    private const val SCHEDULED = "timers/scheduled"

    fun canBeExact(context: Context): Boolean {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) return true
        return context.getSystemService(AlarmManager::class.java)?.canScheduleExactAlarms() == true
    }

    private fun intent(context: Context, id: String): PendingIntent = PendingIntent.getBroadcast(
        context,
        id.hashCode(),
        Intent(context, TimerAlarmReceiver::class.java).putExtra(EXTRA_ID, id),
        PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
    )

    fun schedule(context: Context, store: PlanningTimerStore) {
        val manager = context.getSystemService(AlarmManager::class.java) ?: return
        val prefs = Prefs(context, PREFS)
        val plan = store.notificationPlan()
        val wanted = plan.map { it.id.removePrefix("timer-") }.toSet()
        for (old in prefs.getStringList(SCHEDULED).orEmpty()) if (old !in wanted) manager.cancel(intent(context, old))
        val exact = canBeExact(context)
        for (entry in plan) {
            val id = entry.id.removePrefix("timer-")
            val pending = intent(context, id)
            try {
                if (exact) {
                    manager.setExactAndAllowWhileIdle(AlarmManager.RTC_WAKEUP, entry.fireAtMs, pending)
                } else {
                    manager.setAndAllowWhileIdle(AlarmManager.RTC_WAKEUP, entry.fireAtMs, pending)
                }
            } catch (_: SecurityException) {
                manager.setAndAllowWhileIdle(AlarmManager.RTC_WAKEUP, entry.fireAtMs, pending)
            }
        }
        prefs.putStringList(SCHEDULED, wanted.toList())
    }

    fun channel(context: Context): NotificationManager? {
        val manager = context.getSystemService(NotificationManager::class.java) ?: return null
        if (manager.getNotificationChannel(CHANNEL) == null) {
            manager.createNotificationChannel(
                NotificationChannel(CHANNEL, "Timers and alarms", NotificationManager.IMPORTANCE_HIGH).apply {
                    description = "Rings when a countdown finishes or an alarm is due."
                },
            )
        }
        return manager
    }

    fun notify(context: Context, done: com.forkmesh.nativeapp.planning.PlanningTimer) {
        if (!PingNotifier.allowed(context)) return
        val manager = channel(context) ?: return
        val id = done.id
        val launch = context.packageManager.getLaunchIntentForPackage(context.packageName)
        val open = launch?.let { PendingIntent.getActivity(context, id.hashCode(), it, PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE) }
        val body = if (done.kind == com.forkmesh.nativeapp.planning.PlanningTimerKind.ALARM) "Alarm" else "Countdown finished"
        val notification = Notification.Builder(context, CHANNEL)
            .setSmallIcon(R.drawable.ic_stat_ping)
            .setContentTitle(done.label)
            .setContentText(body)
            .setCategory(Notification.CATEGORY_ALARM)
            .setAutoCancel(true)
            .apply { if (open != null) setContentIntent(open) }
            .build()
        manager.notify("timer-$id", id.hashCode(), notification)
    }

    internal fun ring(context: Context, id: String) {
        val store = PlanningFeature.timerStore(context)
        val timer = store.timers.firstOrNull { it.id == id }
        val rung = store.settle()
        val done = rung.firstOrNull { it.id == id } ?: timer ?: return
        notify(context, done)
        val api = Services.get<ApiService>()
        if (api != null && rung.isNotEmpty()) Thread { TimerPings.post(rung, api, System.currentTimeMillis()) }.start()
    }
}

class TimerAlarmReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        when (intent.action) {
            Intent.ACTION_BOOT_COMPLETED, Intent.ACTION_MY_PACKAGE_REPLACED ->
                TimerAlarms.schedule(context, PlanningFeature.timerStore(context))
            else -> intent.getStringExtra("timer")?.let { TimerAlarms.ring(context, it) }
        }
    }
}
