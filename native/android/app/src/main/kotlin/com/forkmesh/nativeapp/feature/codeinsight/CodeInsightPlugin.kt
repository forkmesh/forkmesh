package com.forkmesh.nativeapp.feature.codeinsight

import com.forkmesh.nativeapp.navigation.RepoRef
import com.forkmesh.nativeapp.navigation.Route

enum class CodeInsightPlugin(val title: String, val packageId: String, val icon: String, val kind: Route.Kind) {
    Ember("Ember", "ember", "local_fire_department_rounded", Route.Kind.Ember),
    Strata("Strata", "strata", "layers", Route.Kind.Strata),
    Synapse("Synapse", "synapse", "polyline", Route.Kind.Synapse),
    ;

    fun route(repo: RepoRef?): Route = when (this) {
        Ember -> Route.Ember(repo)
        Strata -> Route.Strata(repo)
        Synapse -> Route.Synapse(repo)
    }

    companion object {
        fun of(kind: Route.Kind): CodeInsightPlugin? = entries.firstOrNull { it.kind == kind }
    }
}
