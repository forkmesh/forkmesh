package com.forkmesh.nativeapp.feature.account.settings

import android.Manifest
import android.annotation.SuppressLint
import android.content.Context
import android.content.pm.PackageManager
import android.graphics.ImageFormat
import android.graphics.SurfaceTexture
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CaptureRequest
import android.media.ImageReader
import android.os.Handler
import android.os.HandlerThread
import android.os.Looper
import android.util.Size
import android.view.Surface
import android.view.TextureView
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.core.content.ContextCompat
import com.forkmesh.nativeapp.qr.LuminanceImage
import com.forkmesh.nativeapp.qr.QrReader
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean

class QrScannerScreen(private val heading: String, private val onScanned: (String) -> Unit) : Screen() {
    override val title: String get() = heading

    @Composable
    override fun Content(navigator: Navigator) {
        val context = LocalContext.current
        var granted by remember { mutableStateOf(ContextCompat.checkSelfPermission(context, Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED) }
        var asked by remember { mutableStateOf(false) }
        val launcher = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) {
            granted = it
            asked = true
        }
        LaunchedEffect(Unit) { if (!granted) launcher.launch(Manifest.permission.CAMERA) }
        SettingsPage(heading, navigator) {
            item {
                if (granted) {
                    Box(
                        Modifier.fillMaxWidth().aspectRatio(3f / 4f).clip(RoundedCornerShape(16.dp)),
                        contentAlignment = Alignment.Center,
                    ) {
                        CameraScanner(onResult = { text ->
                            if (navigator.pop()) onScanned(text)
                        })
                        Box(Modifier.fillMaxWidth(0.7f).aspectRatio(1f).border(3.dp, Fm.colors.accent, RoundedCornerShape(12.dp)))
                    }
                    SettingsNote("Point the camera at the code.", Fm.colors.textSecondary)
                } else {
                    SettingsCard("Camera is off") {
                        SettingsNote("Allow camera access for ForkMesh in the Settings app, or paste the link instead.")
                        if (!asked) FmTextButton(onClick = { launcher.launch(Manifest.permission.CAMERA) }) { Text("Allow camera") }
                    }
                }
            }
        }
    }
}

@Composable
private fun CameraScanner(onResult: (String) -> Unit) {
    val context = LocalContext.current
    val scanner = remember { CameraQrScanner(context.applicationContext, onResult) }
    DisposableEffect(scanner) { onDispose { scanner.close() } }
    AndroidView(
        factory = { ctx -> TextureView(ctx).also(scanner::attach) },
        modifier = Modifier.fillMaxSize().padding(0.dp),
    )
}

private class CameraQrScanner(private val context: Context, private val onResult: (String) -> Unit) : TextureView.SurfaceTextureListener {
    private val thread = HandlerThread("qr-camera").also { it.start() }
    private val handler = Handler(thread.looper)
    private val main = Handler(Looper.getMainLooper())
    private val decoder = Executors.newSingleThreadExecutor()
    private val busy = AtomicBoolean(false)
    private val done = AtomicBoolean(false)
    private val closed = AtomicBoolean(false)
    private var device: CameraDevice? = null
    private var session: CameraCaptureSession? = null
    private var reader: ImageReader? = null
    private var view: TextureView? = null

    fun attach(textureView: TextureView) {
        view = textureView
        textureView.surfaceTextureListener = this
        textureView.surfaceTexture?.let { open(it) }
    }

    override fun onSurfaceTextureAvailable(surface: SurfaceTexture, width: Int, height: Int) = open(surface)

    override fun onSurfaceTextureSizeChanged(surface: SurfaceTexture, width: Int, height: Int) {}

    override fun onSurfaceTextureDestroyed(surface: SurfaceTexture): Boolean {
        close()
        return true
    }

    override fun onSurfaceTextureUpdated(surface: SurfaceTexture) {}

    @SuppressLint("MissingPermission")
    private fun open(texture: SurfaceTexture) {
        if (closed.get() || device != null) return
        val manager = context.getSystemService(CameraManager::class.java) ?: return
        val id = runCatching {
            manager.cameraIdList.firstOrNull { manager.getCameraCharacteristics(it).get(CameraCharacteristics.LENS_FACING) == CameraCharacteristics.LENS_FACING_BACK }
                ?: manager.cameraIdList.firstOrNull()
        }.getOrNull() ?: return
        val map = manager.getCameraCharacteristics(id).get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP) ?: return
        val size = map.getOutputSizes(ImageFormat.YUV_420_888).orEmpty()
            .filter { it.width <= 1920 && it.height <= 1080 }
            .minByOrNull { kotlin.math.abs(it.width * it.height - 1280 * 720) } ?: Size(640, 480)
        texture.setDefaultBufferSize(size.width, size.height)
        val imageReader = ImageReader.newInstance(size.width, size.height, ImageFormat.YUV_420_888, 2)
        imageReader.setOnImageAvailableListener({ onFrame(it) }, handler)
        reader = imageReader
        runCatching {
            manager.openCamera(id, object : CameraDevice.StateCallback() {
                override fun onOpened(camera: CameraDevice) {
                    if (closed.get()) {
                        camera.close()
                        return
                    }
                    device = camera
                    startSession(camera, Surface(texture), imageReader)
                }

                override fun onDisconnected(camera: CameraDevice) {
                    camera.close()
                    device = null
                }

                override fun onError(camera: CameraDevice, error: Int) {
                    camera.close()
                    device = null
                }
            }, handler)
        }
    }

    @Suppress("DEPRECATION")
    private fun startSession(camera: CameraDevice, preview: Surface, imageReader: ImageReader) {
        runCatching {
            camera.createCaptureSession(listOf(preview, imageReader.surface), object : CameraCaptureSession.StateCallback() {
                override fun onConfigured(configured: CameraCaptureSession) {
                    if (closed.get()) {
                        configured.close()
                        return
                    }
                    session = configured
                    runCatching {
                        val request = camera.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW).apply {
                            addTarget(preview)
                            addTarget(imageReader.surface)
                            set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_PICTURE)
                        }.build()
                        configured.setRepeatingRequest(request, null, handler)
                    }
                }

                override fun onConfigureFailed(failed: CameraCaptureSession) {}
            }, handler)
        }
    }

    private fun onFrame(imageReader: ImageReader) {
        val image = runCatching { imageReader.acquireLatestImage() }.getOrNull() ?: return
        if (done.get() || !busy.compareAndSet(false, true)) {
            image.close()
            return
        }
        val width = image.width
        val height = image.height
        val plane = image.planes[0]
        val buffer = plane.buffer
        val rowStride = plane.rowStride
        val pixelStride = plane.pixelStride
        val step = if (width > 1280) 2 else 1
        val w = width / step
        val h = height / step
        val pixels = IntArray(w * h)
        for (y in 0 until h) {
            val row = y * step * rowStride
            for (x in 0 until w) pixels[y * w + x] = buffer.get(row + x * step * pixelStride).toInt() and 0xFF
        }
        image.close()
        decoder.execute {
            val text = runCatching { QrReader.read(LuminanceImage(w, h, pixels)) }.getOrNull()
            if (!text.isNullOrEmpty() && done.compareAndSet(false, true)) {
                main.post { if (!closed.get()) onResult(text) }
            }
            busy.set(false)
        }
    }

    fun close() {
        if (!closed.compareAndSet(false, true)) return
        runCatching { session?.close() }
        runCatching { device?.close() }
        runCatching { reader?.close() }
        session = null
        device = null
        reader = null
        view?.surfaceTextureListener = null
        view = null
        thread.quitSafely()
        decoder.shutdown()
    }
}
