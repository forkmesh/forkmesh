package com.forkmesh.nativeapp.platform

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

object AppLifecycle {
    private val state = MutableStateFlow(false)

    val foreground: StateFlow<Boolean> = state.asStateFlow()

    val scope: CoroutineScope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)

    fun setForeground(value: Boolean) {
        state.value = value
    }
}
