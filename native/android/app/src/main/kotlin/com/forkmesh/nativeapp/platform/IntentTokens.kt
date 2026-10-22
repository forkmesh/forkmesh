package com.forkmesh.nativeapp.platform

import android.content.Context
import android.content.Intent
import com.forkmesh.nativeapp.feature.account.PrefsStore
import com.forkmesh.nativeapp.security.IntentToken

object IntentTokens {
    private const val PREFS_NAME = "forkmesh.intents"

    @Volatile
    private var token: IntentToken? = null

    fun of(context: Context): IntentToken = token ?: synchronized(this) {
        token ?: IntentToken(PrefsStore(Prefs(context, PREFS_NAME))).also { token = it }
    }

    fun stamp(context: Context, intent: Intent): Intent = intent.putExtra(IntentToken.EXTRA, of(context).value())

    fun carries(context: Context?, intent: Intent): Boolean {
        if (context == null) return false
        val value = runCatching { intent.getStringExtra(IntentToken.EXTRA) }.getOrNull()
        return of(context).accepts(value)
    }

    fun strip(intent: Intent) {
        runCatching { intent.removeExtra(IntentToken.EXTRA) }
    }
}
