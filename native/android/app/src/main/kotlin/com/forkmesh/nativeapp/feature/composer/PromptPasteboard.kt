package com.forkmesh.nativeapp.feature.composer

import android.content.ClipData
import android.content.ClipDescription
import android.content.ClipboardManager
import android.content.Context
import android.net.Uri
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.platform.TextToolbar
import androidx.compose.ui.platform.TextToolbarStatus
import com.forkmesh.nativeapp.composer.PasteboardItem
import com.forkmesh.nativeapp.composer.PromptPaste
import com.forkmesh.nativeapp.composer.PromptPasteRules
import java.io.ByteArrayOutputStream
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

object PromptPasteboard {
    private const val MAX_ITEM_BYTES = 64 * 1024 * 1024

    private fun clipboard(context: Context): ClipboardManager? =
        context.getSystemService(Context.CLIPBOARD_SERVICE) as? ClipboardManager

    fun hasImages(context: Context): Boolean {
        val manager = clipboard(context) ?: return false
        val description = manager.primaryClipDescription ?: return false
        for (index in 0 until description.mimeTypeCount) {
            if (PromptPasteRules.isImageType(description.getMimeType(index))) return true
        }
        if (!description.hasMimeType(ClipDescription.MIMETYPE_TEXT_HTML)) return false
        val clip = try {
            manager.primaryClip
        } catch (error: SecurityException) {
            null
        } ?: return false
        for (index in 0 until clip.itemCount) {
            if (clip.getItemAt(index).htmlText?.contains("data:image/", ignoreCase = true) == true) return true
        }
        return false
    }

    suspend fun read(context: Context): PromptPaste {
        val clip = try {
            clipboard(context)?.primaryClip
        } catch (error: SecurityException) {
            null
        } ?: return PromptPaste(emptyList(), "")
        return read(context, clip)
    }

    suspend fun read(context: Context, clip: ClipData): PromptPaste = withContext(Dispatchers.IO) {
        val items = ArrayList<PasteboardItem>()
        for (index in 0 until clip.itemCount) {
            val item = clip.getItemAt(index)
            val representations = LinkedHashMap<String, ByteArray>()
            val uri = item.uri
            if (uri != null) {
                val type = context.contentResolver.getType(uri) ?: ""
                if (PromptPasteRules.isImageType(type)) readBytes(context, uri)?.let { representations[type.lowercase()] = it }
            }
            item.htmlText?.let { representations[PromptPasteRules.HTML_TYPE] = it.toByteArray() }
            item.text?.toString()?.let { representations["text/plain"] = it.toByteArray() }
            items.add(PasteboardItem(representations))
        }
        PromptPasteRules.paste(items)
    }

    private fun readBytes(context: Context, uri: Uri): ByteArray? = try {
        context.contentResolver.openInputStream(uri)?.use { input ->
            val out = ByteArrayOutputStream()
            val buffer = ByteArray(64 * 1024)
            var total = 0
            while (true) {
                val read = input.read(buffer)
                if (read < 0) break
                total += read
                if (total > MAX_ITEM_BYTES) return null
                out.write(buffer, 0, read)
            }
            out.toByteArray()
        }
    } catch (error: Exception) {
        null
    }
}

class ImagePasteTextToolbar(
    private val base: TextToolbar,
    private val hasImages: () -> Boolean,
    private val pasteImages: (fallback: (() -> Unit)?) -> Unit,
) : TextToolbar {
    override val status: TextToolbarStatus get() = base.status

    override fun hide() = base.hide()

    override fun showMenu(
        rect: Rect,
        onCopyRequested: (() -> Unit)?,
        onPasteRequested: (() -> Unit)?,
        onCutRequested: (() -> Unit)?,
        onSelectAllRequested: (() -> Unit)?,
    ) {
        val paste = if (hasImages()) ({ pasteImages(onPasteRequested) }) else onPasteRequested
        base.showMenu(rect, onCopyRequested, paste, onCutRequested, onSelectAllRequested)
    }

    override fun showMenu(
        rect: Rect,
        onCopyRequested: (() -> Unit)?,
        onPasteRequested: (() -> Unit)?,
        onCutRequested: (() -> Unit)?,
        onSelectAllRequested: (() -> Unit)?,
        onAutofillRequested: (() -> Unit)?,
    ) {
        val paste = if (hasImages()) ({ pasteImages(onPasteRequested) }) else onPasteRequested
        base.showMenu(rect, onCopyRequested, paste, onCutRequested, onSelectAllRequested, onAutofillRequested)
    }
}
