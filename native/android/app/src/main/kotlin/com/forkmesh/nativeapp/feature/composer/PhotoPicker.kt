package com.forkmesh.nativeapp.feature.composer

import android.content.Context
import android.net.Uri
import androidx.activity.compose.ManagedActivityResultLauncher
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.PickVisualMediaRequest
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.platform.LocalContext
import androidx.core.content.FileProvider
import com.forkmesh.nativeapp.agents.PromptAttachment
import com.forkmesh.nativeapp.agents.PromptAttachmentLimits
import java.io.File
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.withContext

enum class PhotoSource { Library, Camera, File }

class PhotoPicker internal constructor(private val context: Context) {
    internal var many: ManagedActivityResultLauncher<PickVisualMediaRequest, List<Uri>>? = null
    internal var one: ManagedActivityResultLauncher<PickVisualMediaRequest, Uri?>? = null
    internal var camera: ManagedActivityResultLauncher<Uri, Boolean>? = null
    internal var files: ManagedActivityResultLauncher<Array<String>, List<Uri>>? = null
    private var pending: CompletableDeferred<List<Uri>>? = null
    private var cameraFile: File? = null

    internal fun deliver(uris: List<Uri>) {
        pending?.complete(uris)
        pending = null
    }

    internal fun deliverCamera(saved: Boolean) {
        val file = cameraFile
        val uri = file?.let { uriFor(it) }
        deliver(if (saved && uri != null && file.length() > 0) listOf(uri) else emptyList())
    }

    private fun uriFor(file: File): Uri = FileProvider.getUriForFile(context, authority(context), file)

    suspend fun pickUris(source: PhotoSource, room: Int): List<Uri> {
        if (room <= 0) return emptyList()
        pending?.complete(emptyList())
        val waiting = CompletableDeferred<List<Uri>>()
        pending = waiting
        val images = PickVisualMediaRequest(ActivityResultContracts.PickVisualMedia.ImageOnly)
        when (source) {
            PhotoSource.Library -> if (room > 1) many?.launch(images) else one?.launch(images)
            PhotoSource.File -> files?.launch(arrayOf("image/*"))
            PhotoSource.Camera -> {
                val file = withContext(Dispatchers.IO) {
                    val dir = File(context.cacheDir, CAMERA_DIR).apply { mkdirs() }
                    dir.listFiles()?.forEach { it.delete() }
                    File(dir, "capture-${System.currentTimeMillis()}.jpg")
                }
                cameraFile = file
                camera?.launch(uriFor(file))
            }
        }
        return waiting.await().take(room)
    }

    suspend fun pick(source: PhotoSource, room: Int, onError: (String) -> Unit = {}): List<PromptAttachment> {
        val uris = pickUris(source, room)
        val prepared = ArrayList<PromptAttachment>()
        try {
            for (uri in uris) {
                try {
                    prepared.add(PhotoIntake.fromUri(context, uri))
                } catch (error: Exception) {
                    if (error is kotlinx.coroutines.CancellationException) throw error
                    onError(error.message ?: "That photo could not be attached.")
                }
            }
        } finally {
            if (source == PhotoSource.Camera) {
                withContext(NonCancellable + Dispatchers.IO) {
                    cameraFile?.delete()
                    cameraFile = null
                }
            }
        }
        return prepared
    }

    companion object {
        const val CAMERA_DIR = "composer-camera"

        fun authority(context: Context): String = "${context.packageName}.composer.files"
    }
}

@Composable
fun rememberPhotoPicker(): PhotoPicker {
    val context = LocalContext.current
    val picker = remember(context) { PhotoPicker(context) }
    picker.many = rememberLauncherForActivityResult(
        ActivityResultContracts.PickMultipleVisualMedia(PromptAttachmentLimits.MAX_COUNT),
    ) { picker.deliver(it) }
    picker.one = rememberLauncherForActivityResult(ActivityResultContracts.PickVisualMedia()) { uri ->
        picker.deliver(listOfNotNull(uri))
    }
    picker.camera = rememberLauncherForActivityResult(ActivityResultContracts.TakePicture()) { saved -> picker.deliverCamera(saved) }
    picker.files = rememberLauncherForActivityResult(ActivityResultContracts.OpenMultipleDocuments()) { picker.deliver(it) }
    return picker
}
