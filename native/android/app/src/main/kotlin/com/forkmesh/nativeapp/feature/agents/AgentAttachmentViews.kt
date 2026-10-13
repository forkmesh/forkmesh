package com.forkmesh.nativeapp.feature.agents

import android.graphics.BitmapFactory
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import com.forkmesh.nativeapp.agents.AgentAttachmentRef
import com.forkmesh.nativeapp.agents.AttachmentOrigin
import com.forkmesh.nativeapp.agents.PromptAttachment
import com.forkmesh.nativeapp.control.ControlTransport
import com.forkmesh.nativeapp.control.attachment
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmInlineStatusLine
import com.forkmesh.nativeapp.ui.kit.FmSoundOnShow
import com.forkmesh.nativeapp.ui.kit.fmTapSounds
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import java.util.concurrent.ConcurrentHashMap
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

object AgentPhotoPicker {
    @Volatile
    var pick: (suspend () -> List<PromptAttachment>)? = null
}

object AgentAttachmentCache {
    private val bytes = ConcurrentHashMap<String, ByteArray>()
    private const val LIMIT = 48

    fun put(ref: AgentAttachmentRef, data: ByteArray) {
        if (bytes.size >= LIMIT) bytes.keys.firstOrNull()?.let { bytes.remove(it) }
        bytes[ref.cacheKey] = data
    }

    fun get(ref: AgentAttachmentRef): ByteArray? = bytes[ref.cacheKey]

    fun clear() = bytes.clear()

    fun local(photo: PromptAttachment): AgentAttachmentRef {
        val ref = AgentAttachmentRef.local(photo)
        bytes[ref.cacheKey] = photo.bytes
        return ref
    }

    fun fetch(ref: AgentAttachmentRef): ByteArray {
        get(ref)?.let { return it }
        val data = when (val origin = ref.origin) {
            is AttachmentOrigin.Relay ->
                AgentsFeature.queue.attachment(origin.owner, origin.repo, origin.sessionId, ref.meta.id, origin.orgScope)
            is AttachmentOrigin.Desktop -> return desktop(ref, origin.sessionId)
            AttachmentOrigin.Elsewhere -> throw com.forkmesh.nativeapp.agents.AgentAttachmentError("This file is on another machine.")
            AttachmentOrigin.Local -> throw com.forkmesh.nativeapp.agents.AgentAttachmentError("This photo is no longer on this phone.")
        }
        put(ref, data)
        return data
    }

    private fun desktop(ref: AgentAttachmentRef, sessionId: Int): ByteArray {
        val hub = AgentsFeature.hub
        val previewKey = ref.cacheKey + "#preview"
        if (hub.transport != ControlTransport.Lan) bytes[previewKey]?.let { return it }
        val fetched = hub.attachment(sessionId, ref.meta.id)
        if (fetched.preview) {
            if (bytes.size >= LIMIT) bytes.keys.firstOrNull()?.let { bytes.remove(it) }
            bytes[previewKey] = fetched.data
        } else {
            bytes.remove(previewKey)
            put(ref, fetched.data)
        }
        return fetched.data
    }
}

private fun decode(data: ByteArray, maxSide: Int): ImageBitmap? {
    val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
    BitmapFactory.decodeByteArray(data, 0, data.size, bounds)
    var sample = 1
    while (bounds.outWidth / (sample * 2) >= maxSide && bounds.outHeight / (sample * 2) >= maxSide) sample *= 2
    val options = BitmapFactory.Options().apply { inSampleSize = sample }
    return BitmapFactory.decodeByteArray(data, 0, data.size, options)?.asImageBitmap()
}

@Composable
internal fun AgentAttachmentStrip(refs: List<AgentAttachmentRef>) {
    if (refs.isEmpty()) return
    val thumbs = refs.filter { it.showsThumbnail }
    val rows = refs.filter { !it.showsThumbnail }
    Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
        if (thumbs.isNotEmpty()) {
            Row(Modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                for (ref in thumbs) AttachmentThumbnail(ref)
            }
        }
        for (ref in rows) AttachmentRow(ref)
    }
}

@Composable
private fun AttachmentThumbnail(ref: AgentAttachmentRef) {
    val c = Fm.colors
    var image by remember(ref.cacheKey) { mutableStateOf<ImageBitmap?>(null) }
    var failure by remember(ref.cacheKey) { mutableStateOf<String?>(null) }
    var open by remember(ref.cacheKey) { mutableStateOf(false) }
    LaunchedEffect(ref.cacheKey) {
        try {
            image = withContext(Dispatchers.IO) { decode(AgentAttachmentCache.fetch(ref), 240) }
            if (image == null) failure = "The attachment could not be decoded."
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (error: Exception) {
            failure = error.message ?: error.toString()
        }
    }
    Box(
        Modifier
            .size(88.dp)
            .clip(RoundedCornerShape(FmRadius.md))
            .background(c.bgOverlay)
            .clickable(enabled = image != null) { open = true }
            .semantics { contentDescription = ref.name },
        contentAlignment = Alignment.Center,
    ) {
        val current = image
        when {
            current != null -> Image(current, ref.name, Modifier.fillMaxSize(), contentScale = ContentScale.Crop)
            failure != null -> FmIcon("broken_image_outlined", size = 24.dp, tint = c.textTertiary)
            else -> FmCircularProgressIndicator(Modifier.size(18.dp), strokeWidth = 2.dp)
        }
    }
    if (open) AttachmentViewer(ref) { open = false }
}

@Composable
private fun AttachmentRow(ref: AgentAttachmentRef) {
    val c = Fm.colors
    Row(verticalAlignment = Alignment.CenterVertically) {
        FmIcon(if (ref.isImage) "photo_outlined" else "insert_drive_file_outlined", size = 18.dp, tint = c.textTertiary)
        Spacer(Modifier.width(FmSpace.x2))
        Column(Modifier.weight(1f)) {
            Text(ref.name, maxLines = 1, overflow = TextOverflow.Ellipsis, style = Fm.text.bodySmall.copy(color = c.textPrimary))
            if (ref.rowDetail.isNotEmpty()) Text(ref.rowDetail, style = Fm.text.labelSmall.copy(color = c.textTertiary))
        }
    }
}

@Composable
private fun AttachmentViewer(ref: AgentAttachmentRef, onClose: () -> Unit) {
    var image by remember(ref.cacheKey) { mutableStateOf<ImageBitmap?>(null) }
    var failure by remember(ref.cacheKey) { mutableStateOf<String?>(null) }
    val scope = rememberCoroutineScope()
    LaunchedEffect(ref.cacheKey) {
        scope.launch {
            try {
                image = withContext(Dispatchers.IO) { decode(AgentAttachmentCache.fetch(ref), 2048) }
                if (image == null) failure = "The attachment could not be decoded."
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (error: Exception) {
                failure = error.message ?: error.toString()
            }
        }
    }
    FmSoundOnShow()
    Dialog(onDismissRequest = onClose, properties = DialogProperties(usePlatformDefaultWidth = false)) {
        Box(
            Modifier
                .fmTapSounds()
                .fillMaxSize()
                .background(androidx.compose.ui.graphics.Color.Black),
            contentAlignment = Alignment.Center,
        ) {
            val current = image
            val failed = failure
            if (current != null) {
                Image(current, ref.name, Modifier.fillMaxWidth(), contentScale = ContentScale.Fit)
            } else if (failed != null) {
                FmInlineStatusLine(failed, isError = true, modifier = Modifier.padding(FmSpace.x5))
            } else {
                FmCircularProgressIndicator(Modifier.size(24.dp))
            }
            Box(Modifier.align(Alignment.TopEnd).padding(FmSpace.x3)) {
                FmIconButton("close", "Close", onPressed = onClose, color = androidx.compose.ui.graphics.Color.White)
            }
            Text(
                ref.name,
                Modifier.align(Alignment.BottomCenter).padding(FmSpace.x4),
                style = Fm.text.bodySmall.copy(color = androidx.compose.ui.graphics.Color.White),
            )
        }
    }
}

@Composable
internal fun PendingPhotoStrip(photos: List<PromptAttachment>, onRemove: (PromptAttachment) -> Unit) {
    if (photos.isEmpty()) return
    val c = Fm.colors
    Row(Modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
        for (photo in photos) {
            val image = remember(photo.id) { decode(photo.bytes, 160) }
            Box(Modifier.size(64.dp).clip(RoundedCornerShape(FmRadius.md)).background(c.bgOverlay)) {
                if (image != null) Image(image, photo.name, Modifier.fillMaxSize(), contentScale = ContentScale.Crop)
                Box(Modifier.align(Alignment.TopEnd)) {
                    FmIconButton("close", "Remove ${photo.name}", onPressed = { onRemove(photo) }, color = c.textPrimary, iconSize = 14.dp)
                }
            }
        }
    }
}
