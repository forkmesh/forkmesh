package com.forkmesh.nativeapp.feature.surfaces

import android.app.PendingIntent
import android.appwidget.AppWidgetManager
import android.appwidget.AppWidgetProvider
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.service.quicksettings.TileService
import android.view.View
import android.widget.RemoteViews
import com.forkmesh.nativeapp.R
import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.navigation.AgentFilter
import com.forkmesh.nativeapp.navigation.ComposeTarget
import com.forkmesh.nativeapp.navigation.InboxFilter
import com.forkmesh.nativeapp.navigation.Route

private val rowIds = listOf(
    Triple(R.id.widget_row_0, R.id.widget_row_0_title, R.id.widget_row_0_detail),
    Triple(R.id.widget_row_1, R.id.widget_row_1_title, R.id.widget_row_1_detail),
    Triple(R.id.widget_row_2, R.id.widget_row_2_title, R.id.widget_row_2_detail),
)

private fun routeIntent(context: Context, route: Route, code: Int): PendingIntent {
    val intent = NavigationFeature.routeIntent(context, route)
        ?: Intent(context, com.forkmesh.nativeapp.MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
    return PendingIntent.getActivity(context, code, intent, PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
}

class AgentsWidgetProvider : AppWidgetProvider() {
    override fun onUpdate(context: Context, manager: AppWidgetManager, ids: IntArray) {
        ids.forEach { manager.updateAppWidget(it, views(context)) }
    }

    companion object {
        fun updateAll(context: Context) {
            val manager = AppWidgetManager.getInstance(context) ?: return
            val ids = manager.getAppWidgetIds(ComponentName(context, AgentsWidgetProvider::class.java))
            if (ids.isNotEmpty()) ids.forEach { manager.updateAppWidget(it, views(context)) }
        }

        private fun views(context: Context): RemoteViews {
            val views = RemoteViews(context.packageName, R.layout.widget_surface)
            val snapshot = SurfacesFeature.readSnapshot(context)
            views.setTextViewText(R.id.widget_title, "Agents")
            views.setOnClickPendingIntent(R.id.widget_root, routeIntent(context, Route.Agents(AgentFilter.All), 9100))
            views.setOnClickPendingIntent(R.id.widget_action, routeIntent(context, Route.Compose(ComposeTarget()), 9101))
            views.setViewVisibility(R.id.widget_action, View.VISIBLE)
            if (snapshot == null || !snapshot.signedIn) {
                views.setTextViewText(R.id.widget_summary, "Open ForkMesh to sign in")
                rowIds.forEach { views.setViewVisibility(it.first, View.GONE) }
                return views
            }
            views.setTextViewText(R.id.widget_summary, snapshot.agents.summaryLine)
            rowIds.forEachIndexed { index, (row, title, detail) ->
                val card = snapshot.agents.runs.getOrNull(index)
                if (card == null) {
                    views.setViewVisibility(row, View.GONE)
                } else {
                    views.setViewVisibility(row, View.VISIBLE)
                    views.setTextViewText(title, card.title)
                    views.setTextViewText(detail, listOf(card.state.label, card.repo, card.detail).filter { it.isNotEmpty() }.joinToString(" · "))
                    views.setOnClickPendingIntent(row, routeIntent(context, card.route, 9110 + index))
                }
            }
            return views
        }
    }
}

class InboxWidgetProvider : AppWidgetProvider() {
    override fun onUpdate(context: Context, manager: AppWidgetManager, ids: IntArray) {
        ids.forEach { manager.updateAppWidget(it, views(context)) }
    }

    companion object {
        fun updateAll(context: Context) {
            val manager = AppWidgetManager.getInstance(context) ?: return
            val ids = manager.getAppWidgetIds(ComponentName(context, InboxWidgetProvider::class.java))
            if (ids.isNotEmpty()) ids.forEach { manager.updateAppWidget(it, views(context)) }
        }

        private fun views(context: Context): RemoteViews {
            val views = RemoteViews(context.packageName, R.layout.widget_surface)
            val snapshot = SurfacesFeature.readSnapshot(context)
            views.setTextViewText(R.id.widget_title, "Inbox")
            views.setViewVisibility(R.id.widget_action, View.GONE)
            views.setOnClickPendingIntent(R.id.widget_root, routeIntent(context, Route.Inbox(InboxFilter.All), 9200))
            if (snapshot == null || !snapshot.signedIn) {
                views.setTextViewText(R.id.widget_summary, "Open ForkMesh to sign in")
                rowIds.forEach { views.setViewVisibility(it.first, View.GONE) }
                return views
            }
            val unread = snapshot.inbox.unread
            views.setTextViewText(R.id.widget_summary, if (unread == 0) "Nothing needs you" else "$unread unread")
            rowIds.forEachIndexed { index, (row, title, detail) ->
                val item = snapshot.inbox.items.getOrNull(index)
                if (item == null) {
                    views.setViewVisibility(row, View.GONE)
                } else {
                    views.setViewVisibility(row, View.VISIBLE)
                    views.setTextViewText(title, item.title)
                    views.setTextViewText(detail, item.subtitle)
                    views.setOnClickPendingIntent(row, routeIntent(context, item.destination, 9210 + index))
                }
            }
            return views
        }
    }
}

class NewAgentTileService : TileService() {
    override fun onClick() {
        super.onClick()
        val intent = NavigationFeature.routeIntent(this, Route.Compose(ComposeTarget())) ?: return
        val pending = PendingIntent.getActivity(this, 9300, intent, PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
        if (android.os.Build.VERSION.SDK_INT >= 34) {
            startActivityAndCollapse(pending)
        } else {
            @Suppress("DEPRECATION", "StartActivityAndCollapseDeprecated")
            startActivityAndCollapse(intent)
        }
    }
}
