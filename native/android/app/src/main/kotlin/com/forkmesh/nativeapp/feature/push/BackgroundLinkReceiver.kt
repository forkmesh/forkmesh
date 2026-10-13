package com.forkmesh.nativeapp.feature.push

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent

class BackgroundLinkReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        when (intent.action) {
            Intent.ACTION_BOOT_COMPLETED, Intent.ACTION_MY_PACKAGE_REPLACED -> {
                val pending = goAsync()
                Thread({
                    try {
                        BackgroundLink.syncNow(context.applicationContext)
                    } finally {
                        pending.finish()
                    }
                }, "BackgroundLinkBoot").start()
            }
        }
    }
}
