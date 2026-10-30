package com.forkmesh.nativeapp.navigation

data class ShellPaneMetrics(val wide: Boolean, val list: Double) {
    companion object {
        val Phone = ShellPaneMetrics(wide = false, list = 0.0)
    }
}

object ShellPanes {
    const val WIDE_WIDTH = 600.0
    const val WIDE_HEIGHT = 480.0
    const val LIST_MIN = 280.0
    const val LIST_MAX = 380.0
    const val LIST_SHARE = 0.4

    fun metrics(windowWidth: Double, windowHeight: Double, content: Double = windowWidth): ShellPaneMetrics {
        if (windowWidth < WIDE_WIDTH || windowHeight < WIDE_HEIGHT) return ShellPaneMetrics.Phone
        return ShellPaneMetrics(wide = true, list = listWidth(content))
    }

    fun listWidth(content: Double): Double = Math.round(content * LIST_SHARE).toDouble().coerceIn(LIST_MIN, LIST_MAX)

    fun splits(tab: AppTab, metrics: ShellPaneMetrics): Boolean = metrics.wide && tab != AppTab.Chat
}
