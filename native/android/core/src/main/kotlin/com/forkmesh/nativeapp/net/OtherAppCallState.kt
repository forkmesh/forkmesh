package com.forkmesh.nativeapp.net

class OtherAppCallState {
    var active: Boolean = false
        private set

    fun update(audioMode: Int, ownCall: Boolean): Boolean {
        val now = ownCall || RadioPolicy.activity(audioMode, musicActive = false) == AudioActivity.CALL
        if (now == active) return false
        active = now
        return true
    }
}
