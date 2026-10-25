package com.forkmesh.nativeapp.platform

import com.forkmesh.nativeapp.core.AppVisibility
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

object AppLifecycle {
    private val state = MutableStateFlow(false)
    private val visibilityState = MutableStateFlow(AppVisibility.HIDDEN)
    private val visibleState = MutableStateFlow(false)

    val foreground: StateFlow<Boolean> = state.asStateFlow()

    val visibility: StateFlow<AppVisibility> = visibilityState.asStateFlow()

    val visible: StateFlow<Boolean> = visibleState.asStateFlow()

    val scope: CoroutineScope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)

    fun setForeground(value: Boolean) {
        state.value = value
    }

    fun setVisibility(value: AppVisibility) {
        visibilityState.value = value
        visibleState.value = value.visible
    }
}
