package com.forkmesh.nativeapp

import android.content.Intent
import android.os.Bundle
import android.view.KeyEvent
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.layout.Box
import androidx.compose.ui.Modifier
import com.forkmesh.nativeapp.core.AppVisibility
import com.forkmesh.nativeapp.feature.Features
import com.forkmesh.nativeapp.feature.chat.ChatPrivacy
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.SideEffect
import androidx.compose.runtime.getValue
import androidx.core.view.WindowCompat
import com.forkmesh.nativeapp.platform.AppLifecycle
import com.forkmesh.nativeapp.platform.AppearanceStore
import com.forkmesh.nativeapp.shell.HomeShell
import com.forkmesh.nativeapp.ui.kit.FmUserActivity
import com.forkmesh.nativeapp.ui.kit.FmWallpaperLayer
import com.forkmesh.nativeapp.ui.kit.KitGalleryScreen
import com.forkmesh.nativeapp.ui.kit.fmTapSounds
import com.forkmesh.nativeapp.ui.navigation.AppNavigation
import com.forkmesh.nativeapp.ui.theme.ForkMeshTheme

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        enableEdgeToEdge()
        super.onCreate(savedInstanceState)
        AppearanceStore.init(this)
        ChatPrivacy.init(this)
        ChatPrivacy.applyWindow(this, ChatPrivacy.hideInSwitcher.value)
        FmUserActivity.startSync()
        val navigator = AppNavigation.init { HomeShell() }
        val kitGallery = KitGalleryScreen.fromIntent(this)
        setContent {
            val appearance by AppearanceStore.appearance.collectAsState()
            val hideInSwitcher by ChatPrivacy.hideInSwitcher.collectAsState()
            SideEffect { ChatPrivacy.applyWindow(this@MainActivity, hideInSwitcher) }
            val dark = appearance.isDark(isSystemInDarkTheme())
            SideEffect {
                WindowCompat.getInsetsController(window, window.decorView).apply {
                    isAppearanceLightStatusBars = !dark
                    isAppearanceLightNavigationBars = !dark
                }
            }
            ForkMeshTheme(dark = dark, theme = appearance.effectiveTheme, wallpaper = appearance.wallpaper.active) {
                Box(Modifier.fmTapSounds(), propagateMinConstraints = true) {
                    FmWallpaperLayer(Modifier.matchParentSize())
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

    override fun onStart() {
        super.onStart()
        AppLifecycle.setVisibility(AppVisibility.INACTIVE)
    }

    override fun onResume() {
        super.onResume()
        AppLifecycle.setVisibility(AppVisibility.RESUMED)
        AppLifecycle.setForeground(true)
    }

    override fun onPause() {
        AppLifecycle.setForeground(false)
        AppLifecycle.setVisibility(AppVisibility.INACTIVE)
        FmUserActivity.flush()
        com.forkmesh.nativeapp.feature.account.settings.DataFolderFeature.syncSoon()
        super.onPause()
    }

    override fun onStop() {
        AppLifecycle.setVisibility(AppVisibility.HIDDEN)
        super.onStop()
    }

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        FmUserActivity.key(event)
        return super.dispatchKeyEvent(event)
    }
}
