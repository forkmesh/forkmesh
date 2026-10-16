package com.forkmesh.nativeapp.feature.facechanger

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.ActivityResultLauncher
import androidx.activity.result.PickVisualMediaRequest
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.facechanger.FaceChangerLook
import com.forkmesh.nativeapp.ui.kit.FmIconTile
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmMessenger
import com.forkmesh.nativeapp.ui.kit.FmRowDivider
import com.forkmesh.nativeapp.ui.kit.LocalFmMessenger
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

data class FaceChangerView(
    val face: String,
    val faceImage: String,
    val background: String,
    val backgroundImage: String,
    val voiceChoice: String,
    val effectiveVoice: String,
)

private fun FaceChangerLook.view(): FaceChangerView =
    FaceChangerView(face, faceImage, background, backgroundImage, voiceChoice, effectiveVoice)

@Composable
fun rememberFaceChangerView(): FaceChangerView {
    val revision by FaceChangerFeature.revision.collectAsState()
    return remember(revision) { FaceChangerFeature.look.view() }
}

@Composable
fun FaceChangerButtons(
    faces: Boolean,
    thumbs: Map<String, String>?,
    voiceShown: String? = null,
    onProblem: (String) -> Unit,
) {
    val state = rememberFaceChangerView()
    val messenger = LocalFmMessenger.current
    val scope = rememberCoroutineScope()
    val accent = Fm.colors.accent
    val facePhoto = rememberFacePhotoPicker(onProblem)
    val backgroundPhoto = rememberBackgroundPhotoPicker(onProblem)
    if (faces) {
        CallRoundButton(
            icon = if (state.face.isEmpty()) "face" else "theater_comedy",
            label = "Face: ${FaceChangerLook.faceLabel(state.face)}",
            background = if (state.face.isEmpty()) Color.White.copy(alpha = .18f) else accent,
        ) {
            scope.launch {
                val choice = pickFace(messenger, state.face, state.faceImage.isNotEmpty()) ?: return@launch
                if (choice == PICK_FACE_PHOTO) {
                    facePhoto.launch(PickVisualMediaRequest(ActivityResultContracts.PickVisualMedia.ImageOnly))
                } else {
                    FaceChangerFeature.run { it.setFace(choice) }
                }
            }
        }
    }
    if (thumbs != null) {
        CallRoundButton(
            icon = "image",
            label = "Background: ${FaceChangerLook.backgroundLabel(state.background)}",
            background = if (state.background.isEmpty()) Color.White.copy(alpha = .18f) else accent,
        ) {
            scope.launch {
                val choice = pickBackground(messenger, state, thumbs) ?: return@launch
                if (choice == FaceChangerLook.CUSTOM_BACKGROUND && state.backgroundImage.isEmpty()) {
                    backgroundPhoto.launch(PickVisualMediaRequest(ActivityResultContracts.PickVisualMedia.ImageOnly))
                } else {
                    FaceChangerFeature.run { it.setBackground(choice) }
                }
            }
        }
    }
    val voice = voiceShown ?: state.effectiveVoice
    CallRoundButton(
        icon = "graphic_eq",
        label = "Voice: ${FaceChangerLook.voiceLabel(voice)}",
        background = if (voice.isEmpty()) Color.White.copy(alpha = .18f) else accent,
    ) {
        scope.launch {
            val choice = pickVoice(messenger, state.voiceChoice) ?: return@launch
            FaceChangerFeature.run { it.setVoiceChoice(choice) }
        }
    }
}

private const val PICK_FACE_PHOTO = "pick-photo"

private suspend fun pickFace(messenger: FmMessenger, current: String, hasPhoto: Boolean): String? =
    messenger.showModalBottomSheet<String> { close ->
        Column(Modifier.padding(bottom = FmSpace.x4)) {
            Text(
                "Face",
                modifier = Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x2),
                style = Fm.text.titleMedium.copy(color = Fm.colors.textPrimary),
            )
            val photo = if (hasPhoto) listOf(FaceChangerLook.CUSTOM_FACE to FaceChangerLook.faceLabel(FaceChangerLook.CUSTOM_FACE)) else emptyList()
            val options = listOf("" to "Your own face") + FaceChangerLook.FACES.map { it.id to it.label } + photo +
                listOf(PICK_FACE_PHOTO to if (hasPhoto) "Choose another photo…" else "Use a photo of a face…")
            options.forEachIndexed { index, (id, label) ->
                if (index > 0) FmRowDivider()
                FmListRow(
                    title = label,
                    leading = { FmIconTile(icon = if (id.isEmpty()) "face" else "theater_comedy") },
                    trailing = if (id == current) ({ FmIcon("check", size = 20.dp, tint = Fm.colors.accent) }) else null,
                    showChevron = false,
                    onTap = { close(id) },
                )
            }
        }
    }

@Composable
private fun rememberFacePhotoPicker(onProblem: (String) -> Unit): ActivityResultLauncher<PickVisualMediaRequest> {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val problem by rememberUpdatedState(onProblem)
    return rememberLauncherForActivityResult(ActivityResultContracts.PickVisualMedia()) { uri ->
        if (uri == null) return@rememberLauncherForActivityResult
        FaceChangerFeature.run { look ->
            val image = CallFaceStore.fromUri(context, uri)
            if (image == null) {
                problem("That photo could not be opened.")
                return@run
            }
            val previous = look.face
            if (!look.setFace(FaceChangerLook.CUSTOM_FACE, image)) return@run
            val peer = FaceChangerFeature.peers?.previewed() ?: return@run
            scope.launch(Dispatchers.IO) {
                repeat(120) {
                    when (runCatching { peer.faceState() }.getOrDefault("")) {
                        "ready" -> return@launch
                        "failed" -> {
                            problem("No face was found in that photo. Try a clear photo of one face looking at the camera.")
                            val back = if (previous == FaceChangerLook.CUSTOM_FACE) "" else previous
                            FaceChangerFeature.run { again -> again.setFace(back) }
                            return@launch
                        }
                    }
                    delay(250)
                }
            }
        }
    }
}

@Composable
private fun rememberBackgroundPhotoPicker(onProblem: (String) -> Unit): ActivityResultLauncher<PickVisualMediaRequest> {
    val context = LocalContext.current
    val problem by rememberUpdatedState(onProblem)
    return rememberLauncherForActivityResult(ActivityResultContracts.PickVisualMedia()) { uri ->
        if (uri == null) return@rememberLauncherForActivityResult
        FaceChangerFeature.run { look ->
            val image = CallBackgroundStore.fromUri(context, uri)
            if (image == null) {
                problem("That photo could not be used as a background.")
                return@run
            }
            look.setBackground(FaceChangerLook.CUSTOM_BACKGROUND, image)
        }
    }
}

private suspend fun pickBackground(messenger: FmMessenger, state: FaceChangerView, thumbs: Map<String, String>): String? =
    messenger.showModalBottomSheet<String> { close ->
        Column(Modifier.padding(start = FmSpace.x4, end = FmSpace.x4, bottom = FmSpace.x4)) {
            Text(
                "Background",
                modifier = Modifier.padding(vertical = FmSpace.x2),
                style = Fm.text.titleMedium.copy(color = Fm.colors.textPrimary),
            )
            FaceChangerLook.BACKGROUNDS.chunked(3).forEach { row ->
                Row(Modifier.fillMaxWidth().padding(vertical = FmSpace.x1), horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                    row.forEach { option ->
                        val selected = option.id == state.background
                        val picture = remember(option.id, state.backgroundImage) {
                            val text = if (option.id == FaceChangerLook.CUSTOM_BACKGROUND) state.backgroundImage else thumbs[option.id].orEmpty()
                            if (text.isEmpty()) null else CallBackgroundStore.thumbnail(text)?.asImageBitmap()
                        }
                        Column(
                            Modifier
                                .weight(1f)
                                .clip(RoundedCornerShape(FmRadius.md))
                                .clickable { close(option.id) }
                                .semantics { contentDescription = option.label },
                            horizontalAlignment = androidx.compose.ui.Alignment.CenterHorizontally,
                        ) {
                            Box(
                                Modifier
                                    .fillMaxWidth()
                                    .aspectRatio(16f / 9f)
                                    .clip(RoundedCornerShape(FmRadius.md))
                                    .background(Fm.colors.bgElevated)
                                    .border(
                                        if (selected) 3.dp else 0.dp,
                                        if (selected) Fm.colors.accent else Color.Transparent,
                                        RoundedCornerShape(FmRadius.md),
                                    ),
                                contentAlignment = androidx.compose.ui.Alignment.Center,
                            ) {
                                if (picture != null) {
                                    Image(picture, contentDescription = null, contentScale = ContentScale.Crop, modifier = Modifier.fillMaxSize())
                                } else {
                                    FmIcon(
                                        when (option.id) {
                                            "" -> "block"
                                            "blur", "blur-strong" -> "grain"
                                            else -> "photo_library"
                                        },
                                        size = 22.dp,
                                        tint = Fm.colors.textSecondary,
                                    )
                                }
                            }
                            Text(
                                option.label,
                                maxLines = 1,
                                overflow = TextOverflow.Ellipsis,
                                style = Fm.text.bodySmall.copy(color = Fm.colors.textPrimary),
                                modifier = Modifier.padding(top = 4.dp),
                            )
                        }
                    }
                    repeat(3 - row.size) { Spacer(Modifier.weight(1f)) }
                }
            }
            if (thumbs.isEmpty()) {
                Text(
                    "Backgrounds need a phone that can run the background model.",
                    style = Fm.text.bodySmall.copy(color = Fm.colors.textTertiary),
                )
            }
        }
    }

private suspend fun pickVoice(messenger: FmMessenger, current: String): String? =
    messenger.showModalBottomSheet<String> { close ->
        Column(Modifier.verticalScroll(rememberScrollState()).padding(bottom = FmSpace.x4)) {
            Text(
                "Voice",
                modifier = Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x2),
                style = Fm.text.titleMedium.copy(color = Fm.colors.textPrimary),
            )
            val options = listOf(
                FaceChangerLook.VOICE_MATCH_FACE to "Match face",
                FaceChangerLook.VOICE_OFF to "Your own voice",
            ) + FaceChangerLook.VOICES.map { it.id to it.label }
            options.forEachIndexed { index, (id, label) ->
                if (index > 0) FmRowDivider()
                FmListRow(
                    title = label,
                    leading = { FmIconTile(icon = if (id == FaceChangerLook.VOICE_OFF) "mic" else "graphic_eq") },
                    trailing = if (id == current) ({ FmIcon("check", size = 20.dp, tint = Fm.colors.accent) }) else null,
                    showChevron = false,
                    onTap = { close(id) },
                )
            }
        }
    }

@Composable
fun CallRoundButton(
    icon: String,
    label: String,
    background: Color,
    tint: Color = Color.White,
    size: Int = 60,
    enabled: Boolean = true,
    onPressed: () -> Unit,
) {
    Box(
        Modifier
            .size(size.dp)
            .clip(CircleShape)
            .background(background)
            .clickable(enabled = enabled, onClick = onPressed)
            .semantics { contentDescription = label },
        contentAlignment = androidx.compose.ui.Alignment.Center,
    ) {
        FmIcon(icon, size = (size * 0.45f).dp, tint = if (enabled) tint else tint.copy(alpha = .4f))
    }
}
