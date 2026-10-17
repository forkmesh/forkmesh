package com.forkmesh.nativeapp.market

import com.forkmesh.nativeapp.navigation.Route
import com.forkmesh.nativeapp.navigation.RouteParser

object PackageMenu {
    const val FEEDBACK = "Send feedback or report a bug"
    const val MORE = "Plugin options"
    const val SHARE = "Share…"
    const val COPY_LINK = "Copy link"
    const val SHARE_BASE = "https://forkmesh.com/market/"

    val UNTAGGED_KINDS: Set<Route.Kind> = setOf(
        Route.Kind.Agents, Route.Kind.Inbox, Route.Kind.Chat, Route.Kind.Compose,
        Route.Kind.Commit, Route.Kind.Commits,
    )

    fun packageFor(kind: Route.Kind): String? = PackageGate.ROUTE_KINDS[kind]?.takeIf { kind !in UNTAGGED_KINDS }

    fun name(id: String, known: String? = null): String {
        known?.trim()?.takeIf { it.isNotEmpty() }?.let { return it }
        val words = id.split('-', '_').filter { it.isNotEmpty() }.joinToString(" ")
        return words.replaceFirstChar { it.uppercase() }.ifEmpty { "this plugin" }
    }

    fun rateLabel(name: String, rated: Boolean): String = if (rated) "Edit your rating of $name" else "Rate $name"

    fun uninstallLabel(organization: String?): String = organization?.let { "Uninstall from $it" } ?: "Uninstall"

    fun shareUrl(packageId: String): String? = RouteParser.marketPackageId(packageId.trim().lowercase())?.let { SHARE_BASE + it }
}
