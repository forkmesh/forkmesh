package com.forkmesh.nativeapp.feature.genie

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.graphics.BitmapFactory
import android.net.Uri
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import com.forkmesh.nativeapp.App
import com.forkmesh.nativeapp.agents.AgentProviderKind
import com.forkmesh.nativeapp.agents.AnthropicProvider
import com.forkmesh.nativeapp.api.HttpTransport
import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AuthService
import com.forkmesh.nativeapp.feature.account.PrefsStore
import com.forkmesh.nativeapp.feature.agents.AgentsFeature
import com.forkmesh.nativeapp.feature.inbox.InboxFeature
import com.forkmesh.nativeapp.feature.market.MarketFeature
import com.forkmesh.nativeapp.genie.GenieCrm
import com.forkmesh.nativeapp.genie.GenieDesktopWriter
import com.forkmesh.nativeapp.genie.GenieOutreach
import com.forkmesh.nativeapp.genie.GenieOutreachPick
import com.forkmesh.nativeapp.genie.GenieOutreachWriter
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.asJsonMap
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.platform.Prefs
import java.net.HttpURLConnection
import java.net.URL

object GenieFeature {
    const val IMAGE_BYTES = 2_000_000
    const val PICTURE_PIXELS = 256
    private var app: Context? = null

    var prefs: PrefStore = MemoryPrefStore()
        private set

    fun install(app: App) {
        this.app = app.applicationContext
        prefs = PrefsStore(Prefs(app))
        GenieSpiderState.load(prefs)
        ShellSuggestions.start(prefs)
        ShellAutoSend.start(prefs)
        GenieOutreachCoach.start(prefs)
    }

    fun signedIn(): Boolean = Services.get<AuthService>()?.session?.value != null

    fun crm(): GenieCrm? {
        if (!signedIn()) return null
        val client = MarketFeature.client() ?: return null
        return GenieCrm(client) { Services.get<AuthService>()?.session?.value?.nodeName.orEmpty() }
    }

    fun cloudWriter(timeout: Double = GenieOutreach.CLOUD_TIMEOUT, chat: Boolean = false): GenieOutreachWriter? {
        val saved = runCatching { AgentsFeature.credentials.read() }.getOrNull() ?: return null
        val model = if (saved.kind == AgentProviderKind.Anthropic) AnthropicProvider.DEFAULT_MODEL else "gpt-5.3"
        val label = saved.kind.models.firstOrNull { it.id == model }?.label ?: model
        val chosen = com.forkmesh.nativeapp.agents.AgentCredentials(saved.kind, saved.apiKey, model)
        return GenieOutreachWriter(label, if (chat) 30.0 else timeout) { messages ->
            val provider = chosen.build()
            val system = messages.filter { it.role == "system" }.joinToString("\n\n") { it.content }
            val history = mutableListOf<Any>()
            for (message in messages.filter { it.role != "system" }) {
                if (message.role == "user") {
                    provider.appendUser(history, message.content)
                } else {
                    history.add(linkedMapOf("role" to "assistant", "content" to message.content))
                }
            }
            provider.send(system, history, emptyList(), HttpTransport.DEFAULT).text
        }
    }

    fun outreachWriters(): List<GenieOutreachWriter> = listOfNotNull(cloudWriter(), desktopWriter())

    fun desktopWriter(): GenieOutreachWriter? {
        val hub = runCatching { AgentsFeature.hub }.getOrNull() ?: return null
        val desktop = hub.desktop ?: return null
        return GenieDesktopWriter.make(desktop.name) { method, params, timeoutMs -> hub.request(method, params, timeoutMs) }
    }

    fun ping(title: String, message: String) {
        runCatching { InboxFeature.recordLocalFailure("genie", title, message) }
    }

    fun picture(pick: GenieOutreachPick): ImageBitmap? {
        val urls = pick.images.toMutableList()
        pick.handle?.lookupUrl?.let { lookup ->
            val account = fetch(lookup)?.let { runCatching { Json.parse(String(it)).asJsonMap() }.getOrNull() }
            val avatar = account.str("avatar_static").ifEmpty { account.str("avatar") }
            if (avatar.startsWith("https://") && !avatar.contains("/missing.png")) urls.add(0, avatar)
        }
        for (url in urls.take(4)) {
            val data = fetch(url) ?: continue
            val bitmap = runCatching { decodePicture(data) }.getOrNull() ?: continue
            return bitmap.asImageBitmap()
        }
        return null
    }

    private fun fetch(url: String): ByteArray? = runCatching {
        val connection = URL(url).openConnection() as HttpURLConnection
        connection.connectTimeout = 8000
        connection.readTimeout = 8000
        connection.setRequestProperty("User-Agent", "ForkMesh-Android")
        try {
            if (connection.responseCode != 200) return@runCatching null
            val bytes = connection.inputStream.use { readBounded(it, IMAGE_BYTES + 1) }
            if (bytes.size > IMAGE_BYTES) null else bytes
        } finally {
            connection.disconnect()
        }
    }.getOrNull()

    private fun readBounded(input: java.io.InputStream, limit: Int): ByteArray {
        val out = java.io.ByteArrayOutputStream()
        val buffer = ByteArray(8192)
        while (out.size() < limit) {
            val read = input.read(buffer, 0, minOf(buffer.size, limit - out.size()))
            if (read < 0) break
            out.write(buffer, 0, read)
        }
        return out.toByteArray()
    }

    private fun decodePicture(data: ByteArray): android.graphics.Bitmap? {
        val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        BitmapFactory.decodeByteArray(data, 0, data.size, bounds)
        if (bounds.outWidth <= 0 || bounds.outHeight <= 0) return null
        var sample = 1
        while (maxOf(bounds.outWidth, bounds.outHeight) / (sample * 2) >= PICTURE_PIXELS) sample *= 2
        return BitmapFactory.decodeByteArray(data, 0, data.size, BitmapFactory.Options().apply { inSampleSize = sample })
    }

    fun openUrl(url: String): Boolean {
        val context = app ?: return false
        return runCatching {
            context.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url)).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
            true
        }.getOrDefault(false)
    }

    fun share(text: String) {
        val context = app ?: return
        val send = Intent(Intent.ACTION_SEND).setType("text/plain").putExtra(Intent.EXTRA_TEXT, text)
        runCatching { context.startActivity(Intent.createChooser(send, null).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) }
    }

    fun copy(text: String) {
        val context = app ?: return
        val clipboard = context.getSystemService(ClipboardManager::class.java) ?: return
        clipboard.setPrimaryClip(ClipData.newPlainText("Toot", text))
    }
}
