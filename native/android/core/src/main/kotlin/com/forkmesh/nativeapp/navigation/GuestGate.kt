package com.forkmesh.nativeapp.navigation

object GuestGate {
    private val detailActions: Map<Route.Kind, String> = mapOf(
        Route.Kind.Note to "use notes",
        Route.Kind.Task to "use tasks",
        Route.Kind.Host to "manage your hosts",
        Route.Kind.Contact to "use the CRM",
        Route.Kind.CrmOrg to "use the CRM",
        Route.Kind.Cove to "open your coves",
        Route.Kind.Approval to "answer agent approvals",
        Route.Kind.InstalledApps to "manage your apps",
    )

    fun action(route: Route): String? = HubCatalog.guestAction(route) ?: detailActions[route.kind]

    fun action(route: Route, guest: Boolean): String? = if (guest) action(route) else null

    val gatedKinds: Set<Route.Kind>
        get() = HubCatalog.routes.filter { HubCatalog.guestAction(it) != null }.map { it.kind }.toSet() + detailActions.keys
}
