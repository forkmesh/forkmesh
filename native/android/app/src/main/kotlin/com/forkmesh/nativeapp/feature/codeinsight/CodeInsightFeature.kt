package com.forkmesh.nativeapp.feature.codeinsight

import com.forkmesh.nativeapp.App
import com.forkmesh.nativeapp.feature.market.PackageGateFeature
import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.navigation.Route
import com.forkmesh.nativeapp.ui.navigation.Screen

object CodeInsightFeature {
    private const val OWNER = "existing"

    fun install(app: App) {
        CodeInsightSession.install(app.cacheDir)
        NavigationFeature.register(Route.Kind.Ember, OWNER) { route ->
            CodeInsightScreen(CodeInsightPlugin.Ember, (route as Route.Ember).repo) { context, navigator -> EmberPage(context, navigator) }
        }
        NavigationFeature.register(Route.Kind.Strata, OWNER) { route ->
            CodeInsightScreen(CodeInsightPlugin.Strata, (route as Route.Strata).repo) { context, navigator -> StrataPage(context, navigator) }
        }
        NavigationFeature.register(Route.Kind.Synapse, OWNER) { route ->
            CodeInsightScreen(CodeInsightPlugin.Synapse, (route as Route.Synapse).repo) { context, navigator -> SynapsePage(context, navigator) }
        }
    }

    fun installed(plugin: CodeInsightPlugin): Boolean = PackageGateFeature.surfaces.value.shows(plugin.packageId)

    fun installedPlugins(): List<CodeInsightPlugin> = CodeInsightPlugin.entries.filter(::installed)

    fun localScreen(plugin: CodeInsightPlugin, localName: String): Screen {
        val screen = NavigationFeature.registeredScreen(plugin.route(null))
        return (screen as? CodeInsightScreen)?.withLocalCopy(localName) ?: screen
    }
}
