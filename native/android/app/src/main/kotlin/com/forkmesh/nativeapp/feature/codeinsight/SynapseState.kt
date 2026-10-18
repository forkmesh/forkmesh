package com.forkmesh.nativeapp.feature.codeinsight

import androidx.compose.runtime.Stable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import com.forkmesh.nativeapp.codeinsight.InsightSnapshot
import com.forkmesh.nativeapp.codeinsight.Synapse
import com.forkmesh.nativeapp.codeinsight.SynapseCamera
import com.forkmesh.nativeapp.codeinsight.SynapseColourBy
import com.forkmesh.nativeapp.codeinsight.SynapseGraph
import com.forkmesh.nativeapp.codeinsight.SynapseProjection
import com.forkmesh.nativeapp.codeinsight.SynapseScene
import com.forkmesh.nativeapp.codeinsight.SynapseSceneOptions
import com.forkmesh.nativeapp.codeinsight.SynapseShowMode
import com.forkmesh.nativeapp.codeinsight.SynapseStyle
import java.util.WeakHashMap
import java.util.concurrent.atomic.AtomicReference
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.isActive
import kotlinx.coroutines.withContext

internal class SynapseLaidOut(val graph: SynapseGraph, val positions: FloatArray)

internal object SynapseLayoutCache {
    private val cache = WeakHashMap<InsightSnapshot, HashMap<Boolean, SynapseLaidOut>>()

    @Synchronized
    fun get(snapshot: InsightSnapshot, isolated: Boolean): SynapseLaidOut? = cache[snapshot]?.get(isolated)

    @Synchronized
    fun put(snapshot: InsightSnapshot, isolated: Boolean, laidOut: SynapseLaidOut) {
        cache.getOrPut(snapshot) { HashMap() }[isolated] = laidOut
    }
}

internal enum class SynapseSheet { None, Overview, File }

@Stable
internal class SynapsePageState(val snapshot: InsightSnapshot) {
    var graph by mutableStateOf<SynapseGraph?>(null)
        private set
    var positions by mutableStateOf(FloatArray(0))
        private set
    var progress by mutableFloatStateOf(0f)
        private set
    var running by mutableStateOf(false)
        private set
    var style by mutableStateOf<SynapseStyle?>(null)
        private set
    var camera by mutableStateOf(SynapseCamera())
        private set
    var frame by mutableLongStateOf(0L)
        private set
    var clock by mutableFloatStateOf(0f)
        private set

    var colourBy by mutableStateOf(SynapseColourBy.Module)
        private set
    var showMode by mutableStateOf(SynapseShowMode.Everything)
        private set
    var depth by mutableIntStateOf(1)
        private set
    var selected by mutableIntStateOf(-1)
        private set
    var isolatedCycle by mutableIntStateOf(-1)
        private set
    var query by mutableStateOf("")
        private set
    var pulses by mutableStateOf(true)
        private set
    var autoRotate by mutableStateOf(true)
        private set
    var touching by mutableStateOf(false)
        private set
    var sheet by mutableStateOf(SynapseSheet.None)
    var reducedMotion by mutableStateOf(false)
    var animatorScale = 1f

    val orphans: List<Int> = Synapse.orphans(snapshot)
    val leaves: List<Int> = Synapse.leaves(snapshot)
    val importCount: Int = Synapse.importCount(snapshot)
    val summary: String = Synapse.summary(snapshot)

    val projection = SynapseProjection()
    var width = 1f
        private set
    var height = 1f
        private set

    private val pending = AtomicReference<Pair<FloatArray, Double>?>(null)
    private var flightFrom: SynapseCamera? = null
    private var flightTo: SynapseCamera? = null
    private var flightStart = 0L
    private var flightDuration = 900L
    private var lastFrame = 0L
    private var pauseUntil = 0L
    private var nowNanos = 0L
    private var defaultsApplied = false

    val includeIsolated: Boolean get() = showMode == SynapseShowMode.Orphans

    fun applyMotionDefaults(reduced: Boolean, scale: Float) {
        reducedMotion = reduced
        animatorScale = scale
        if (!defaultsApplied) {
            defaultsApplied = true
            if (reduced) {
                autoRotate = false
                pulses = false
                restyle()
            }
        }
    }

    fun setSize(w: Float, h: Float) {
        if (w <= 0f || h <= 0f || (w == width && h == height)) return
        val first = width <= 1f
        width = w
        height = h
        if (first && flightTo == null) camera = fitted(null)
        touchFrame()
    }

    suspend fun ensureLayout() {
        val isolated = includeIsolated
        val cached = SynapseLayoutCache.get(snapshot, isolated)
        if (cached != null) {
            val replacing = graph != null
            install(cached.graph, cached.positions, 1f)
            running = false
            if (replacing && selected < 0 && isolatedCycle < 0) fit(true)
            return
        }
        running = true
        progress = 0f
        val graph = withContext(Dispatchers.Default) { Synapse.buildGraph(snapshot, isolated) }
        if (!currentCoroutineContext().isActive) return
        install(graph, withContext(Dispatchers.Default) { Synapse.seedPositions(graph) }, 0f)
        val context = currentCoroutineContext()
        val positions = withContext(Dispatchers.Default) {
            Synapse.runLayout(
                graph,
                publish = { snapshotPositions, fraction -> pending.set(snapshotPositions to fraction) },
                stop = { !context.isActive },
            )
        }
        if (!context.isActive) return
        pending.set(null)
        SynapseLayoutCache.put(snapshot, isolated, SynapseLaidOut(graph, positions))
        if (this.graph === graph) {
            this.positions = positions
            progress = 1f
            running = false
            if (selected < 0 && isolatedCycle < 0) fit(true)
            touchFrame()
        }
    }

    private fun install(graph: SynapseGraph, positions: FloatArray, progress: Float) {
        val first = this.graph == null
        this.graph = graph
        this.positions = positions
        this.progress = progress
        if (isolatedCycle >= graph.cycles.size) isolatedCycle = -1
        restyle()
        if (first && width > 1f) camera = fitted(null)
        touchFrame()
    }

    fun restyle() {
        val graph = graph ?: return
        style = SynapseScene.restyle(
            graph,
            SynapseSceneOptions(
                colourBy = colourBy,
                showMode = showMode,
                depth = depth,
                selected = selected,
                isolatedCycle = isolatedCycle,
                query = query,
                pulses = pulses,
            ),
        )
        touchFrame()
    }

    fun touchFrame() {
        frame++
    }

    fun setColour(mode: SynapseColourBy) {
        if (colourBy == mode) return
        colourBy = mode
        restyle()
    }

    fun setShow(mode: SynapseShowMode): Boolean {
        val graphChanges = (mode == SynapseShowMode.Orphans) != includeIsolated
        if (showMode == mode && isolatedCycle < 0) return false
        showMode = mode
        isolatedCycle = -1
        restyle()
        if (!graphChanges) fit(true)
        return graphChanges
    }

    fun setNeighbourhoodDepth(hops: Int) {
        val clamped = hops.coerceIn(1, 3)
        if (depth == clamped) return
        depth = clamped
        restyle()
    }

    fun setSearch(text: String) {
        if (query == text) return
        query = text
        restyle()
    }

    fun togglePulses() {
        pulses = !pulses
        restyle()
    }

    fun toggleAutoRotate() {
        autoRotate = !autoRotate
        pauseUntil = 0L
        touchFrame()
    }

    fun select(file: Int, fly: Boolean) {
        val graph = graph
        val valid = if (file >= 0 && file < snapshot.files.size) file else -1
        if (valid >= 0 && graph != null && isolatedCycle >= 0) {
            val loop = graph.cycles.getOrNull(isolatedCycle)
            if (loop == null || snapshot.files[valid].cycle != loop.index) isolatedCycle = -1
        }
        selected = valid
        restyle()
        if (fly && valid >= 0) flyToFile(valid)
    }

    fun clearSelection() {
        if (selected < 0 && isolatedCycle < 0) return
        selected = -1
        isolatedCycle = -1
        restyle()
    }

    fun isolateCycle(cycle: Int): Boolean {
        val graph = graph ?: return false
        if (cycle < 0 || cycle >= graph.cycles.size) return false
        val relayout = includeIsolated
        showMode = SynapseShowMode.Cycles
        selected = -1
        isolatedCycle = cycle
        restyle()
        if (!relayout) flyToCycle(cycle)
        return relayout
    }

    fun showEverything(): Boolean {
        val relayout = includeIsolated
        showMode = SynapseShowMode.Everything
        isolatedCycle = -1
        restyle()
        if (!relayout) fit(true)
        return relayout
    }

    private val aspect: Float get() = width / maxOf(1f, height)

    private fun fitted(nodes: IntArray?): SynapseCamera {
        val style = style ?: return camera
        return SynapseScene.fittedCamera(style, positions, camera, nodes, aspect, autoRotate, progress >= 1f)
    }

    fun fit(animate: Boolean) {
        val graph = graph ?: return
        val nodes = if (isolatedCycle >= 0) graph.cycles.getOrNull(isolatedCycle)?.files?.map { graph.nodeFor(it) }?.toIntArray() else null
        val target = fitted(nodes)
        if (animate) flyTo(target) else setCameraNow(target)
    }

    fun flyToFile(file: Int) {
        val style = style ?: return
        SynapseScene.fileCamera(style, positions, camera, file, aspect, autoRotate)?.let(::flyTo)
    }

    fun flyToCycle(cycle: Int) {
        val style = style ?: return
        SynapseScene.cycleCamera(style, positions, camera, cycle, aspect, autoRotate)?.let(::flyTo)
    }

    fun canFlyTo(file: Int): Boolean = graph?.nodeFor(file)?.let { it >= 0 } == true

    private fun setCameraNow(next: SynapseCamera) {
        flightFrom = null
        flightTo = null
        camera = next.clamped()
        touchFrame()
    }

    private fun flyTo(target: SynapseCamera) {
        val to = target.copy(yaw = camera.yaw)
        if (reducedMotion || animatorScale <= 0f) {
            setCameraNow(to)
            return
        }
        flightFrom = camera
        flightTo = to
        flightStart = 0L
        flightDuration = (900L * animatorScale).toLong().coerceAtLeast(1L)
        pauseRotation()
        touchFrame()
    }

    private fun pauseRotation() {
        pauseUntil = nowNanos + 2_500_000_000L
    }

    fun touchStarted() {
        touching = true
        flightFrom = null
        flightTo = null
        pauseRotation()
    }

    fun touchEnded() {
        touching = false
        pauseRotation()
        touchFrame()
    }

    fun orbit(dx: Float, dy: Float) {
        camera = SynapseScene.orbit(camera, dx, dy)
        pauseRotation()
    }

    fun panZoom(panX: Float, panY: Float, zoom: Float, centroidX: Float, centroidY: Float) {
        var next = camera
        if (zoom > 0f && zoom != 1f) next = SynapseScene.zoomAt(next, 1f / zoom, centroidX, centroidY, width, height)
        if (panX != 0f || panY != 0f) next = SynapseScene.pan(next, panX, panY, height)
        camera = next.clamped()
        pauseRotation()
    }

    fun pick(x: Float, y: Float, reach: Float, near: Float): Int {
        val style = style ?: return -1
        return SynapseScene.pick(style, projection, x, y, reach, near)
    }

    private val rotating: Boolean
        get() = autoRotate && !touching && flightTo == null && graph != null && !reducedMotion

    fun wantsFrames(): Boolean {
        if (graph == null) return running
        if (running || pending.get() != null || flightTo != null) return true
        if (rotating) return true
        val style = style ?: return false
        return pulses && !reducedMotion && (style.pulseEdges > 0 || style.cycleNodesVisible)
    }

    fun onFrame(nanos: Long) {
        nowNanos = nanos
        val dt = if (lastFrame == 0L) 0f else ((nanos - lastFrame) / 1e9f).coerceIn(0f, 0.1f)
        lastFrame = nanos
        pending.getAndSet(null)?.let { (next, fraction) ->
            if (graph != null && next.size == graph!!.nodes.size * 3 && fraction < 1.0) {
                positions = next
                progress = fraction.toFloat()
            }
        }
        val to = flightTo
        val from = flightFrom
        if (to != null && from != null) {
            if (flightStart == 0L) flightStart = nanos
            val u = ((nanos - flightStart) / 1e6f / flightDuration).coerceIn(0f, 1f)
            camera = SynapseScene.interpolate(from, to, u)
            if (u >= 1f) {
                flightTo = null
                flightFrom = null
                pauseRotation()
            }
        }
        if (rotating && nanos >= pauseUntil) camera = camera.copy(yaw = camera.yaw + dt * 0.12f / maxOf(0.25f, animatorScale))
        if (!reducedMotion) clock += dt
        frame++
    }

    fun resetClock() {
        lastFrame = 0L
    }
}
