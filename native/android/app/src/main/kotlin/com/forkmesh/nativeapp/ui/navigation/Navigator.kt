package com.forkmesh.nativeapp.ui.navigation

import androidx.activity.compose.BackHandler
import androidx.compose.animation.AnimatedContent
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

    open val hidesShellChrome: Boolean get() = false

    open val extendsUnderTabBar: Boolean get() = false

    var pluginPackage: String? = null

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

val LocalScreen = staticCompositionLocalOf<Screen?> { null }

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
    AnimatedContent(
        targetState = navigator.top,
        transitionSpec = {
            val from = navigator.stack.indexOf(initialState)
            val to = navigator.stack.indexOf(targetState)
            val forward = from >= 0 && (to > from || to < 0)
            if (forward) {
                (slideInHorizontally(FmMotion.push()) { it } togetherWith slideOutHorizontally(FmMotion.push()) { -it * 3 / 10 })
                    .apply { targetContentZIndex = 1f }
            } else {
                (slideInHorizontally(FmMotion.push()) { -it * 3 / 10 } togetherWith slideOutHorizontally(FmMotion.push()) { it })
                    .apply { targetContentZIndex = -1f }
            }
        },
        label = "navigator",
    ) { screen ->
        holder.SaveableStateProvider(screen.key) {
            CompositionLocalProvider(LocalNavigator provides navigator, LocalScreen provides screen) {
                screen.Content(navigator)
            }
        }
    }
}
