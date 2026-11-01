package com.forkmesh.nativeapp.feature.mastodon

import android.content.Context
import android.graphics.BitmapFactory
import android.net.Uri
import android.provider.OpenableColumns
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.PickVisualMediaRequest
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
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
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.mastodon.MastodonCompose
import com.forkmesh.nativeapp.mastodon.MastodonStatus
import com.forkmesh.nativeapp.mastodon.MastodonText
import com.forkmesh.nativeapp.mastodon.MastodonUnauthorized
import com.forkmesh.nativeapp.mastodon.MastodonUpload
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmModalBottomSheet
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.util.UUID

internal class MastodonPicked(val upload: MastodonUpload, val preview: ImageBitmap?)

internal class MastodonComposer(val replyTo: MastodonStatus?, me: String) {
    var text by mutableStateOf(replyTo?.let { MastodonCompose.replyText(it, me) }.orEmpty())
    var spoiler by mutableStateOf("")
    var warning by mutableStateOf(false)
    var visibility by mutableStateOf(replyTo?.let(MastodonCompose::replyVisibility) ?: "public")
    val media = mutableStateListOf<MastodonPicked>()
    var posting by mutableStateOf(false)
    var error by mutableStateOf("")
    val idempotencyKey: String = UUID.randomUUID().toString()

    fun remaining(limit: Int): Int = MastodonText.remaining(limit, text, spoiler, warning)
}

private const val MAX_UPLOAD_BYTES = 16 * 1024 * 1024

private fun readPicked(context: Context, uri: Uri): MastodonPicked? {
    val resolver = context.contentResolver
    val type = resolver.getType(uri)?.takeIf { it.startsWith("image/") } ?: "image/jpeg"
    val name = runCatching {
        resolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { cursor ->
            if (cursor.moveToFirst()) cursor.getString(0) else null
        }
    }.getOrNull()?.takeIf { it.isNotBlank() } ?: "image.${type.substringAfter('/').substringBefore('+')}"
    val bytes = runCatching {
        resolver.openInputStream(uri)?.use { input ->
            val out = java.io.ByteArrayOutputStream()
            val buffer = ByteArray(64 * 1024)
            while (true) {
                val read = input.read(buffer)
                if (read < 0) break
                out.write(buffer, 0, read)
                if (out.size() > MAX_UPLOAD_BYTES) return null
            }
            out.toByteArray()
        }
    }.getOrNull() ?: return null
    val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
    BitmapFactory.decodeByteArray(bytes, 0, bytes.size, bounds)
    var sample = 1
    while (bounds.outWidth / (sample * 2) >= 240) sample *= 2
    val preview = BitmapFactory.decodeByteArray(bytes, 0, bytes.size, BitmapFactory.Options().apply { inSampleSize = sample })?.asImageBitmap()
    return MastodonPicked(MastodonUpload(bytes, name, type), preview)
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun MastodonComposeSheet(composer: MastodonComposer, onDismiss: () -> Unit, onPosted: (MastodonStatus) -> Unit) {
    val c = Fm.colors
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val limits = MastodonState.limits
    val remaining = composer.remaining(limits.maxCharacters)
    val picker = rememberLauncherForActivityResult(ActivityResultContracts.PickVisualMedia()) { uri ->
        if (uri == null) return@rememberLauncherForActivityResult
        scope.launch {
            val picked = withContext(Dispatchers.IO) { readPicked(context, uri) }
            if (picked == null) composer.error = "Could not read that picture. Pictures can be up to 16 MB." else composer.media += picked
        }
    }
    val canPost = !composer.posting && remaining >= 0 && (composer.text.isNotBlank() || composer.media.isNotEmpty())
    FmModalBottomSheet(onDismissRequest = { if (!composer.posting) onDismiss() }, showDragHandle = true, isScrollControlled = true) {
        Column(
            Modifier
                .fillMaxWidth()
                .verticalScroll(rememberScrollState())
                .padding(horizontal = FmSpace.x4, vertical = FmSpace.x2),
            verticalArrangement = Arrangement.spacedBy(FmSpace.x3),
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                FmTextButton(onClick = onDismiss, enabled = !composer.posting) { Text("Cancel") }
                Spacer(Modifier.weight(1f))
                Text(if (composer.replyTo != null) "Reply" else "New post", style = Fm.text.titleSmall.copy(color = c.textPrimary))
                Spacer(Modifier.weight(1f))
                FmToneButton(
                    if (composer.posting) "Posting…" else "Post",
                    onPressed = if (canPost) ({ submit(scope, composer, onPosted) }) else null,
                    icon = "send",
                    tone = FmTone.Success,
                    filled = true,
                    busy = composer.posting,
                )
            }
            composer.replyTo?.let { parent ->
                Text(
                    "Replying to @${parent.account.acct}",
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    style = Fm.text.bodySmall.copy(color = c.textSecondary),
                )
            }
            if (composer.error.isNotEmpty()) MastodonErrorLine(composer.error)
            if (composer.warning) {
                FmTextField(composer.spoiler, { composer.spoiler = it }, hint = "Content warning")
            }
            FmTextField(
                composer.text,
                { composer.text = it },
                hint = if (composer.replyTo != null) "Write your reply" else "What's on your mind?",
                minLines = 4,
                maxLines = 12,
                autofocus = true,
            )
            if (composer.media.isNotEmpty()) {
                Row(Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                    composer.media.toList().forEach { picked ->
                        Box(Modifier.size(84.dp).clip(RoundedCornerShape(FmRadius.md)).background(c.bgOverlay)) {
                            picked.preview?.let { Image(it, contentDescription = picked.upload.fileName, modifier = Modifier.size(84.dp), contentScale = ContentScale.Crop) }
                            FmIconButton(
                                "close",
                                "Remove picture",
                                onPressed = if (composer.posting) null else ({ composer.media.remove(picked) }),
                                modifier = Modifier.align(Alignment.TopEnd),
                                color = c.textPrimary,
                                iconSize = 16.dp,
                            )
                        }
                    }
                }
            }
            Row(verticalAlignment = Alignment.CenterVertically) {
                FmToneButton(
                    "CW",
                    onPressed = { composer.warning = !composer.warning },
                    tone = if (composer.warning) FmTone.Accent else FmTone.Neutral,
                    tooltip = if (composer.warning) "Remove the content warning" else "Add a content warning",
                )
                Spacer(Modifier.width(FmSpace.x2))
                FmIconButton(
                    "photo_library",
                    "Add a picture",
                    onPressed = if (composer.media.size < limits.maxMedia && !composer.posting) ({
                        composer.error = ""
                        picker.launch(PickVisualMediaRequest(ActivityResultContracts.PickVisualMedia.ImageOnly))
                    }) else null,
                    color = c.textSecondary,
                    iconSize = 20.dp,
                )
                Spacer(Modifier.width(FmSpace.x2))
                VisibilityPicker(composer)
                Spacer(Modifier.weight(1f))
                Text(
                    "$remaining",
                    style = Fm.text.bodySmall.copy(color = if (remaining < 0) c.danger else c.textTertiary, fontFeatureSettings = FmType.tabular),
                )
            }
            Spacer(Modifier.size(FmSpace.x4))
        }
    }
}

@Composable
private fun VisibilityPicker(composer: MastodonComposer) {
    var open by remember { mutableStateOf(false) }
    val icon = when (composer.visibility) {
        "unlisted" -> "lock_open"
        "private" -> "lock"
        "direct" -> "alternate_email"
        else -> "public"
    }
    Box {
        FmToneButton(MastodonCompose.visibilityLabel(composer.visibility), onPressed = { open = true }, icon = icon)
        FmDropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            for (value in MastodonCompose.VISIBILITIES) {
                FmDropdownMenuItem(
                    MastodonCompose.visibilityLabel(value),
                    onClick = {
                        open = false
                        composer.visibility = value
                    },
                    icon = if (value == composer.visibility) "check" else null,
                )
            }
        }
    }
}

private fun submit(scope: kotlinx.coroutines.CoroutineScope, composer: MastodonComposer, onPosted: (MastodonStatus) -> Unit) {
    val client = MastodonState.client() ?: return
    if (composer.posting) return
    composer.posting = true
    composer.error = ""
    scope.launch {
        try {
            val created = withContext(Dispatchers.IO) {
                val ids = composer.media.toList().map { client.upload(it.upload).id }
                client.post(
                    composer.text.trim(),
                    composer.visibility,
                    spoiler = if (composer.warning) composer.spoiler.trim() else "",
                    replyTo = composer.replyTo?.id,
                    mediaIds = ids,
                    idempotencyKey = composer.idempotencyKey,
                )
            }
            onPosted(created)
        } catch (failure: CancellationException) {
            throw failure
        } catch (_: MastodonUnauthorized) {
            MastodonState.revoked()
        } catch (failure: Exception) {
            composer.error = "Could not post: ${MastodonState.describe(failure)}"
        } finally {
            composer.posting = false
        }
    }
}

@Composable
internal fun MastodonComposeIcon(onPressed: () -> Unit) {
    FmIconButton("edit", "Write a post", onPressed = onPressed, color = Fm.colors.textPrimary)
}

