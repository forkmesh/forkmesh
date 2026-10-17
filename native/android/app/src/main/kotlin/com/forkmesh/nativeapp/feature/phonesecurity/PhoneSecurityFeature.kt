package com.forkmesh.nativeapp.feature.phonesecurity

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.provider.Settings
import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.navigation.Route
import com.forkmesh.nativeapp.phonesecurity.PhoneSecurityFix

object PhoneSecurityFeature {
    private const val OWNER = "existing"

    fun install() {
        NavigationFeature.register(Route.Kind.PhoneSecurity, OWNER) { PhoneSecurityScreen() }
    }

    fun intentFor(context: Context, fix: PhoneSecurityFix): Intent {
        val intent = if (fix.ownAppDetails) {
            Intent(fix.action, Uri.fromParts("package", context.packageName, null))
        } else {
            Intent(fix.action)
        }
        return intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
    }

    fun open(context: Context, fix: PhoneSecurityFix): Boolean {
        val opened = try {
            context.startActivity(intentFor(context, fix))
            true
        } catch (_: Exception) {
            false
        }
        if (opened) return true
        return try {
            context.startActivity(Intent(Settings.ACTION_SETTINGS).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
            true
        } catch (_: Exception) {
            false
        }
    }
}
