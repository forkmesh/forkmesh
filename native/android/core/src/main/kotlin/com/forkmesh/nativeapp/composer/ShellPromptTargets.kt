package com.forkmesh.nativeapp.composer

import com.forkmesh.nativeapp.agents.PromptAttachment
import com.forkmesh.nativeapp.model.OrgApiException

class ShellPromptError(override val message: String) : RuntimeException(message)

class ShellPromptTarget(
    val id: String,
    val label: String,
    val icon: String,
    val hint: String,
    val sendDescription: String,
    val acceptsPhotos: Boolean = true,
    val submit: suspend (String, List<PromptAttachment>) -> String,
)

class ShellPromptTargets {
    private class Entry(val owner: Any, val target: ShellPromptTarget)

    private val entries = mutableListOf<Entry>()
    private val listeners = mutableListOf<() -> Unit>()

    var bypassed: Boolean = false
        private set

    val active: ShellPromptTarget? get() = synchronized(entries) { entries.lastOrNull()?.target }

    val routing: ShellPromptTarget? get() = if (bypassed) null else active

    fun register(owner: Any, target: ShellPromptTarget) {
        synchronized(entries) {
            entries.removeAll { it.owner === owner || it.target.id == target.id }
            entries.add(Entry(owner, target))
            bypassed = false
        }
        changed()
    }

    fun unregister(owner: Any) {
        val removed = synchronized(entries) {
            val any = entries.removeAll { it.owner === owner }
            if (entries.isEmpty()) bypassed = false
            any
        }
        if (removed) changed()
    }

    fun bypass(on: Boolean) {
        if (bypassed == on) return
        bypassed = on
        changed()
    }

    fun addListener(listener: () -> Unit): () -> Unit {
        synchronized(listeners) { listeners.add(listener) }
        return { synchronized(listeners) { listeners.remove(listener) } }
    }

    private fun changed() {
        val snapshot = synchronized(listeners) { listeners.toList() }
        snapshot.forEach { it() }
    }

    companion object {
        fun message(error: Throwable): String = when (error) {
            is ShellPromptError -> error.message
            is OrgApiException -> error.message
            else -> "That did not work."
        }
    }
}
