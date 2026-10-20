package com.forkmesh.nativeapp.agents

class AgentListRings {
    private var epoch = 0
    private var loading = false
    private var rang = false

    val isLoading: Boolean @Synchronized get() = loading

    @Synchronized
    fun begin(): Int {
        epoch++
        loading = true
        rang = false
        return epoch
    }

    @Synchronized
    fun ring(): Boolean {
        if (!loading) return true
        rang = true
        return false
    }

    @Synchronized
    fun readsSent(token: Int) {
        if (token == epoch) rang = false
    }

    @Synchronized
    fun finish(token: Int): Boolean {
        if (token != epoch) return false
        loading = false
        val again = rang
        rang = false
        return again
    }

    @Synchronized
    fun abandon(token: Int) {
        if (token != epoch) return
        loading = false
        rang = false
    }
}
