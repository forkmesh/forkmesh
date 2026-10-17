package com.forkmesh.nativeapp.feature.chat

import android.app.Activity
import android.app.KeyguardManager
import android.content.Context
import android.content.Intent
import android.hardware.biometrics.BiometricManager
import android.hardware.biometrics.BiometricPrompt
import android.os.Build
import android.os.CancellationSignal
import android.view.WindowManager
import com.forkmesh.nativeapp.platform.AppLifecycle
import com.forkmesh.nativeapp.platform.Prefs
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch

object ChatPrivacy {
    const val HIDE_IN_SWITCHER_TITLE = "Hide chats in the app switcher"
    const val HIDE_IN_SWITCHER_NOTE = "Covers ForkMesh in the recent apps view and blocks screenshots while it is on."
    const val LOCK_TITLE = "Lock chats with fingerprint"
    const val LOCK_NOTE = "Asks for your fingerprint, face or screen lock before Chat opens after ForkMesh was in the background."
    const val LOCK_UNAVAILABLE = "Set a screen lock on this phone to use this."
    const val LOCK_REASON = "Unlock your chats"

    private const val HIDE_KEY = "chat/privacy/hideInSwitcher"
    private const val LOCK_KEY = "chat/privacy/lock"

    private var prefs: Prefs? = null
    private val hide = MutableStateFlow(true)
    private val lock = MutableStateFlow(false)
    private val locked = MutableStateFlow(false)
    private var watching = false

    val hideInSwitcher: StateFlow<Boolean> = hide.asStateFlow()
    val lockEnabled: StateFlow<Boolean> = lock.asStateFlow()
    val chatLocked: StateFlow<Boolean> = locked.asStateFlow()

    fun init(context: Context) {
        val p = prefs ?: Prefs(context.applicationContext).also { prefs = it }
        hide.value = p.getBool(HIDE_KEY, true)
        lock.value = p.getBool(LOCK_KEY, false) && deviceSecure(context)
        if (!watching) {
            watching = true
            locked.value = lock.value
            AppLifecycle.scope.launch {
                AppLifecycle.foreground.collect { foreground -> if (!foreground && lock.value) locked.value = true }
            }
        }
    }

    fun setHideInSwitcher(value: Boolean) {
        prefs?.putBool(HIDE_KEY, value)
        hide.value = value
    }

    fun setLockEnabled(value: Boolean) {
        prefs?.putBool(LOCK_KEY, value)
        lock.value = value
        if (!value) locked.value = false
    }

    fun unlocked() {
        locked.value = false
    }

    fun applyWindow(activity: Activity, secure: Boolean) {
        if (secure) {
            activity.window.setFlags(WindowManager.LayoutParams.FLAG_SECURE, WindowManager.LayoutParams.FLAG_SECURE)
        } else {
            activity.window.clearFlags(WindowManager.LayoutParams.FLAG_SECURE)
        }
    }

    fun deviceSecure(context: Context): Boolean =
        (context.getSystemService(Context.KEYGUARD_SERVICE) as? KeyguardManager)?.isDeviceSecure == true

    fun credentialIntent(context: Context): Intent? {
        val keyguard = context.getSystemService(Context.KEYGUARD_SERVICE) as? KeyguardManager ?: return null
        @Suppress("DEPRECATION")
        return keyguard.createConfirmDeviceCredentialIntent("ForkMesh", LOCK_REASON)
    }

    fun usesPrompt(): Boolean = Build.VERSION.SDK_INT >= Build.VERSION_CODES.P

    fun prompt(activity: Activity, done: (Boolean) -> Unit) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.P) {
            done(false)
            return
        }
        val builder = BiometricPrompt.Builder(activity).setTitle("ForkMesh").setSubtitle(LOCK_REASON)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            builder.setAllowedAuthenticators(
                BiometricManager.Authenticators.BIOMETRIC_WEAK or BiometricManager.Authenticators.DEVICE_CREDENTIAL,
            )
        } else {
            @Suppress("DEPRECATION")
            builder.setDeviceCredentialAllowed(true)
        }
        try {
            builder.build().authenticate(CancellationSignal(), activity.mainExecutor, object : BiometricPrompt.AuthenticationCallback() {
                override fun onAuthenticationSucceeded(result: BiometricPrompt.AuthenticationResult?) {
                    done(true)
                }

                override fun onAuthenticationError(errorCode: Int, errString: CharSequence?) {
                    done(false)
                }
            })
        } catch (_: Exception) {
            done(false)
        }
    }
}
