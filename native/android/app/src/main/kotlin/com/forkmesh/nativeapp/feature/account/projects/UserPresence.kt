package com.forkmesh.nativeapp.feature.account.projects

import android.app.KeyguardManager
import android.content.Context
import android.hardware.biometrics.BiometricManager
import android.hardware.biometrics.BiometricPrompt
import android.os.Build
import android.os.CancellationSignal
import androidx.annotation.RequiresApi
import com.forkmesh.nativeapp.ui.kit.FmMessenger
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlin.coroutines.resume

object UserPresence {
    @Volatile
    var override: (suspend (String) -> Boolean)? = null

    suspend fun confirm(context: Context, messenger: FmMessenger, reason: String): Boolean {
        override?.let { return it(reason) }
        val clean = reason.trim().ifEmpty { "Confirm it's you to continue." }
        val platform = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) platformConfirm(context, clean) else null
        if (platform != null) return platform
        return messenger.showConfirmDialog(title = "Confirm it's you", body = clean, confirmLabel = "Confirm")
    }

    @RequiresApi(Build.VERSION_CODES.Q)
    private suspend fun platformConfirm(context: Context, reason: String): Boolean? {
        val keyguard = context.getSystemService(KeyguardManager::class.java) ?: return null
        if (!keyguard.isDeviceSecure) return null
        return try {
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
                            if (!continuation.isActive) return
                            val refused = errorCode == BiometricPrompt.BIOMETRIC_ERROR_USER_CANCELED ||
                                errorCode == BiometricPrompt.BIOMETRIC_ERROR_CANCELED ||
                                errorCode == BiometricPrompt.BIOMETRIC_ERROR_LOCKOUT ||
                                errorCode == BiometricPrompt.BIOMETRIC_ERROR_LOCKOUT_PERMANENT
                            continuation.resume(if (refused) false else null)
                        }
                    },
                )
                continuation.invokeOnCancellation { signal.cancel() }
            }
        } catch (_: Exception) {
            null
        }
    }
}
