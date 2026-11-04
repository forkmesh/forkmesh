package com.forkmesh.nativeapp.feature.account.projects

import android.app.Activity
import android.app.KeyguardManager
import android.content.Context
import android.content.Intent
import android.os.Bundle
import kotlinx.coroutines.CompletableDeferred

class UserPresenceActivity : Activity() {
    private var answered = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        if (savedInstanceState != null) return
        val reason = intent.getStringExtra(EXTRA_REASON).orEmpty()
        @Suppress("DEPRECATION")
        val confirm = getSystemService(KeyguardManager::class.java)?.createConfirmDeviceCredentialIntent("Confirm it's you", reason)
        if (confirm == null) answer(false) else startActivityForResult(confirm, REQUEST)
    }

    @Deprecated("Deprecated in Java")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode == REQUEST) answer(resultCode == RESULT_OK)
    }

    override fun onDestroy() {
        if (isFinishing) answer(false)
        super.onDestroy()
    }

    private fun answer(confirmed: Boolean) {
        if (answered) return
        answered = true
        pending?.complete(confirmed)
        pending = null
        finish()
    }

    companion object {
        private const val EXTRA_REASON = "reason"
        private const val REQUEST = 1

        @Volatile
        private var pending: CompletableDeferred<Boolean>? = null

        suspend fun confirm(context: Context, reason: String): Boolean {
            val answer = CompletableDeferred<Boolean>()
            pending?.complete(false)
            pending = answer
            val intent = Intent(context, UserPresenceActivity::class.java)
                .putExtra(EXTRA_REASON, reason)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            val started = runCatching { context.startActivity(intent) }.isSuccess
            return if (started) answer.await() else false
        }
    }
}
