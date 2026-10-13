package com.forkmesh.nativeapp.tools

import java.util.concurrent.CopyOnWriteArrayList

open class ChangeNotifier {
    private val listeners = CopyOnWriteArrayList<() -> Unit>()

    fun addListener(listener: () -> Unit): () -> Unit {
        listeners.add(listener)
        return { listeners.remove(listener) }
    }

    fun removeListener(listener: () -> Unit) {
        listeners.remove(listener)
    }

    val hasListeners: Boolean get() = listeners.isNotEmpty()

    protected fun notifyListeners() {
        for (listener in listeners) listener()
    }
}
