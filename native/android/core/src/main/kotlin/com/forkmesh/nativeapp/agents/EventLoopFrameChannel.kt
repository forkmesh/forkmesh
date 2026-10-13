package com.forkmesh.nativeapp.agents

import com.forkmesh.nativeapp.api.AccountEventLoop
import com.forkmesh.nativeapp.api.ApiService

class EventLoopFrameChannel(api: ApiService, sessionToken: () -> String, nodeName: String) : RelayFrameChannel {
    private val loop = AccountEventLoop(api, sessionToken, nodeName = nodeName)

    override var onFrame: ((Map<String, Any?>) -> Unit)?
        get() = loop.onFrame
        set(value) {
            loop.onFrame = value
        }

    override var onConnected: (() -> Unit)?
        get() = loop.onConnected
        set(value) {
            loop.onConnected = value
        }

    override var onSignedOut: (() -> Unit)?
        get() = loop.onSignedOut
        set(value) {
            loop.onSignedOut = value
        }

    override fun start() = loop.start()

    override fun send(frame: Map<String, Any?>): Boolean = loop.send(frame)

    override fun dispose() = loop.dispose()
}
