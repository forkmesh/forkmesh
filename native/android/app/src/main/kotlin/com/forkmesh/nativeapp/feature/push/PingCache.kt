package com.forkmesh.nativeapp.feature.push

import android.content.Context
import com.forkmesh.nativeapp.feature.account.PrefsStore
import com.forkmesh.nativeapp.inbox.PingSnapshots
import com.forkmesh.nativeapp.platform.Prefs

object PingCache {
    @Volatile
    private var shared: PingSnapshots? = null

    fun get(context: Context): PingSnapshots =
        shared ?: synchronized(this) { shared ?: PingSnapshots(PrefsStore(Prefs(context))).also { shared = it } }
}
