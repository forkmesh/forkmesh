package com.forkmesh.nativeapp.ui.navigation

import androidx.activity.compose.BackHandler
import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideOutHorizontally
import androidx.compose.animation.togetherWith
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.saveable.rememberSaveableStateHolder
import androidx.compose.runtime.staticCompositionLocalOf
import com.forkmesh.nativeapp.ui.SoundCue
import com.forkmesh.nativeapp.ui.kit.FmSoundCues
import com.forkmesh.nativeapp.ui.theme.FmMotion
import java.util.concurrent.atomic.AtomicLong

abstract class Screen {
    val key: String = "screen-${NEXT_KEY.incrementAndGet()}"

    open val title: String get() = javaClass.simpleName

    @Composable
    abstract fun Content(navigator: Navigator)

    open fun onBack(): Boolean = false

    open fun onDisposed() {}

    private companion object {
        val NEXT_KEY = AtomicLong()
    }
}

class Navigator(root: Screen) {
    val stack = mutableStateListOf(root)

    internal var onRemoved: (Screen) -> Unit = {}

    val top: Screen get() = stack.last()

    val depth: Int get() = stack.size

    fun push(screen: Screen) {
        FmSoundCues.play(SoundCue.NAVIGATE)
        stack.add(screen)
    }

    fun pop(): Boolean {
        if (stack.size <= 1) return false
        FmSoundCues.play(SoundCue.CLOSE)
        remove(stack.removeAt(stack.lastIndex))
        return true
    }

    fun popTo(screen: Screen) {
        if (screen !in stack) return
        while (top !== screen) {
            if (!pop()) break
        }
    }

    fun replace(screen: Screen) {
        val old = stack[stack.lastIndex]
        stack[stack.lastIndex] = screen
        remove(old)
    }

    fun setRoot(screen: Screen) {
        val old = stack.toList()
        stack.clear()
        stack.add(screen)
        old.asReversed().forEach(::remove)
    }

    fun popToRoot() {
        while (stack.size > 1) remove(stack.removeAt(stack.lastIndex))
    }

    fun handleBack(): Boolean = top.onBack() || pop()

    private fun remove(screen: Screen) {
        screen.onDisposed()
        onRemoved(screen)
    }
}

val LocalNavigator = staticCompositionLocalOf<Navigator> { error("no Navigator in composition") }

object AppNavigation {
    lateinit var navigator: Navigator
        private set

    fun init(root: () -> Screen): Navigator {
        if (!::navigator.isInitialized) navigator = Navigator(root())
        return navigator
    }
}

@Composable
fun NavigatorHost(navigator: Navigator, onExit: () -> Unit) {
    val holder = rememberSaveableStateHolder()
    DisposableEffect(navigator) {
        navigator.onRemoved = { holder.removeState(it.key) }
        onDispose { navigator.onRemoved = {} }
    }
    BackHandler {
        if (!navigator.handleBack()) onExit()
    }
    var lastDepth = navigator.depth
    AnimatedContent(
        targetState = navigator.top,
        transitionSpec = {
            val forward = navigator.depth >= lastDepth
            lastDepth = navigator.depth
            val duration = FmMotion.normal
            if (forward) {
                (slideInHorizontally(tween(duration)) { it / 4 } + fadeIn(tween(duration))) togetherWith
                    fadeOut(tween(duration))
            } else {
                fadeIn(tween(duration)) togetherWith
                    (slideOutHorizontally(tween(duration)) { it / 4 } + fadeOut(tween(duration)))
            }
        },
        label = "navigator",
    ) { screen ->
        holder.SaveableStateProvider(screen.key) {
            CompositionLocalProvider(LocalNavigator provides navigator) {
                screen.Content(navigator)
            }
        }
    }
}
