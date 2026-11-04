package com.forkmesh.nativeapp.feature.account.projects

import android.app.KeyguardManager
import android.content.Context
import android.hardware.biometrics.BiometricManager
import android.hardware.biometrics.BiometricPrompt
import android.os.Build
import android.os.CancellationSignal
import androidx.annotation.RequiresApi
import com.forkmesh.nativeapp.security.UserPresenceMethod
import com.forkmesh.nativeapp.security.UserPresencePolicy
import com.forkmesh.nativeapp.ui.kit.FmMessenger
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlin.coroutines.resume

object UserPresence {
    @Volatile
    var override: (suspend (String) -> Boolean)? = null

    suspend fun confirm(context: Context, messenger: FmMessenger, reason: String): Boolean {
        override?.let { return it(reason) }
        val clean = reason.trim().ifEmpty { "Confirm it's you to continue." }
        val secure = context.getSystemService(KeyguardManager::class.java)?.isDeviceSecure != false
        return when (UserPresencePolicy.method(secure, Build.VERSION.SDK_INT)) {
            UserPresenceMethod.Dialog -> messenger.showConfirmDialog(title = "Confirm it's you", body = clean, confirmLabel = "Confirm")
            UserPresenceMethod.Prompt -> if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) prompt(context, clean) else false
            UserPresenceMethod.DeviceCredential -> UserPresenceActivity.confirm(context, clean)
        }
    }

    @RequiresApi(Build.VERSION_CODES.Q)
    private suspend fun prompt(context: Context, reason: String): Boolean = try {
        suspendCancellableCoroutine { continuation ->
            val signal = CancellationSignal()
            val builder = BiometricPrompt.Builder(context)
                .setTitle("Confirm it's you")
                .setSubtitle(reason)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                builder.setAllowedAuthenticators(
                    BiometricManager.Authenticators.BIOMETRIC_WEAK or BiometricManager.Authenticators.DEVICE_CREDENTIAL,
                )
            } else {
                @Suppress("DEPRECATION")
                builder.setDeviceCredentialAllowed(true)
            }
            builder.build().authenticate(
                signal,
                context.mainExecutor,
                object : BiometricPrompt.AuthenticationCallback() {
                    override fun onAuthenticationSucceeded(result: BiometricPrompt.AuthenticationResult?) {
                        if (continuation.isActive) continuation.resume(true)
                    }

                    override fun onAuthenticationError(errorCode: Int, errString: CharSequence?) {
                        if (continuation.isActive) continuation.resume(false)
                    }
                },
            )
            continuation.invokeOnCancellation { signal.cancel() }
        }
    } catch (_: Exception) {
        false
    }
}
