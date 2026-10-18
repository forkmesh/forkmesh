package com.forkmesh.nativeapp.feature.push

import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.projects.ProjectsService
import com.forkmesh.nativeapp.model.ForkNotification
import com.forkmesh.nativeapp.platform.AppLifecycle
import com.forkmesh.nativeapp.push.PushScreen
import com.forkmesh.nativeapp.push.pingOnScreen

object PushScreens {
    @Volatile
    private var shown: PushScreen? = null

    fun publish(screen: PushScreen?) {
        shown = screen
    }

    fun shows(projectId: String, ping: ForkNotification): Boolean {
        val screen = shown ?: return false
        if (!AppLifecycle.foreground.value) return false
        if (Services.get<ProjectsService>()?.directory?.activeId != projectId) return false
        return pingOnScreen(ping, screen)
    }
}
