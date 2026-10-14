package com.forkmesh.nativeapp.feature.facechanger

import android.content.ClipData
import android.content.Context
import android.content.Intent
import android.os.Handler
import android.os.Looper
import android.webkit.JavascriptInterface
import androidx.core.content.FileProvider
import com.forkmesh.nativeapp.feature.composer.PhotoPicker
import com.forkmesh.nativeapp.market.FaceChangerPage
import com.forkmesh.nativeapp.market.ShareStep
import com.forkmesh.nativeapp.market.ShareTransfer
import java.io.File
import java.io.OutputStream

internal class FaceChangerShare(
    private val context: Context,
    private val origin: String,
    private val onProblem: (String) -> Unit,
) {
    @Volatile
    var pageUrl: String? = null

    @Volatile
    private var closed = false

    private val main = Handler(Looper.getMainLooper())
    private val transfer = ShareTransfer()
    private var file: File? = null
    private var out: OutputStream? = null

    @JavascriptInterface
    fun post(text: String) {
        synchronized(this) {
            if (closed || !FaceChangerPage.isPageHost(pageUrl, origin)) {
                transfer.cancel()
                discard()
                return
            }
            when (val step = transfer.accept(text)) {
                is ShareStep.Begin -> begin(step)
                is ShareStep.Append -> append(step.bytes)
                is ShareStep.Finish -> finish(step)
                is ShareStep.Reject -> {
                    discard()
                    problem("The clip could not be shared.")
                }
                ShareStep.Ignore -> Unit
            }
        }
    }

    fun close() {
        synchronized(this) {
            closed = true
            discard()
        }
    }

    private fun begin(step: ShareStep.Begin) {
        discard()
        val opened = runCatching {
            val folder = File(context.cacheDir, SHARE_DIR).apply { mkdirs() }
            folder.listFiles()?.forEach { if (it.isFile) it.delete() }
            val target = File(folder, step.name)
            file = target
            out = target.outputStream().buffered()
        }
        if (opened.isFailure) {
            transfer.cancel()
            discard()
            problem("There is no room to save the clip on this phone.")
        }
    }

    private fun append(bytes: ByteArray) {
        val stream = out ?: return
        if (runCatching { stream.write(bytes) }.isFailure) {
            transfer.cancel()
            discard()
            problem("There is no room to save the clip on this phone.")
        }
    }

    private fun finish(step: ShareStep.Finish) {
        val target = file
        val written = runCatching { out?.close() }.isSuccess
        out = null
        file = null
        if (target == null || !written) {
            target?.delete()
            problem("The clip could not be shared.")
            return
        }
        main.post { if (!closed) share(target, step) }
    }

    private fun share(target: File, step: ShareStep.Finish) {
        val started = runCatching {
            val uri = FileProvider.getUriForFile(context, PhotoPicker.authority(context), target)
            val send = Intent(Intent.ACTION_SEND)
                .setType(step.mime)
                .putExtra(Intent.EXTRA_STREAM, uri)
                .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
            send.clipData = ClipData.newRawUri(step.name, uri)
            context.startActivity(Intent.createChooser(send, step.title.ifEmpty { "Share" }))
        }
        if (started.isFailure) onProblem("No app on this phone can take the clip.")
    }

    private fun discard() {
        runCatching { out?.close() }
        out = null
        file?.delete()
        file = null
    }

    private fun problem(message: String) {
        main.post { if (!closed) onProblem(message) }
    }

    companion object {
        const val BRIDGE = "FmShareBridge"
        const val SHARE_DIR = "face-changer-share"
    }
}
