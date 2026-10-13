package com.forkmesh.nativeapp.platform

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import java.util.concurrent.CopyOnWriteArrayList

object ProjectLifecycle {
    @Volatile
    var scope: CoroutineScope = newScope()
        private set

    private val hooks = CopyOnWriteArrayList<suspend () -> Unit>()

    fun onClose(hook: suspend () -> Unit) {
        hooks.add(hook)
    }

    suspend fun close() {
        val pending = hooks.toList().asReversed()
        hooks.clear()
        for (hook in pending) runCatching { hook() }
        scope.cancel()
        scope = newScope()
    }

    private fun newScope(): CoroutineScope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
}
