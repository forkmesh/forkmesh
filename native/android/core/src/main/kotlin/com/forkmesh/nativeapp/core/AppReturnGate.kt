package com.forkmesh.nativeapp.core

enum class AppVisibility {
    RESUMED,
    INACTIVE,
    HIDDEN,
    ;

    val visible: Boolean get() = this != HIDDEN
}

class AppReturnGate(private var away: Boolean = true) {
    fun isReturn(state: AppVisibility): Boolean = when (state) {
        AppVisibility.HIDDEN -> {
            away = true
            false
        }
        AppVisibility.INACTIVE -> false
        AppVisibility.RESUMED -> away.also { away = false }
    }
}
