package com.forkmesh.nativeapp

import android.content.Intent
import android.os.Bundle
import android.view.KeyEvent
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.layout.Box
import androidx.compose.ui.Modifier
import com.forkmesh.nativeapp.feature.Features
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.SideEffect
import androidx.compose.runtime.getValue
import androidx.core.view.WindowCompat
import com.forkmesh.nativeapp.platform.AppLifecycle
import com.forkmesh.nativeapp.platform.AppearanceStore
import com.forkmesh.nativeapp.shell.HomeShell
import com.forkmesh.nativeapp.ui.kit.FmUserActivity
import com.forkmesh.nativeapp.ui.kit.KitGalleryScreen
import com.forkmesh.nativeapp.ui.kit.fmTapSounds
import com.forkmesh.nativeapp.ui.navigation.AppNavigation
import com.forkmesh.nativeapp.ui.theme.ForkMeshTheme

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        enableEdgeToEdge()
        super.onCreate(savedInstanceState)
        AppearanceStore.init(this)
        FmUserActivity.startSync()
        val navigator = AppNavigation.init { HomeShell() }
        val kitGallery = KitGalleryScreen.fromIntent(this)
        setContent {
            val appearance by AppearanceStore.appearance.collectAsState()
            val dark = appearance.isDark(isSystemInDarkTheme())
            SideEffect {
                WindowCompat.getInsetsController(window, window.decorView).apply {
                    isAppearanceLightStatusBars = !dark
                    isAppearanceLightNavigationBars = !dark
                }
            }
            ForkMeshTheme(dark = dark, accent = appearance.accent) {
                Box(Modifier.fmTapSounds(), propagateMinConstraints = true) {
                    if (kitGallery != null) kitGallery.Content(navigator) else Features.Root(navigator, onExit = { moveTaskToBack(true) })
                }
            }
        }
        Features.onIntent(navigator, intent)
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        Features.onIntent(AppNavigation.navigator, intent)
    }

    override fun onResume() {
        super.onResume()
        AppLifecycle.setForeground(true)
    }

    override fun onPause() {
        AppLifecycle.setForeground(false)
        FmUserActivity.flush()
        super.onPause()
    }

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        FmUserActivity.key(event)
        return super.dispatchKeyEvent(event)
    }
}
