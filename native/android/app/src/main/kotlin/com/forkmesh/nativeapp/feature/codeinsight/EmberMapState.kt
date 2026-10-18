package com.forkmesh.nativeapp.feature.codeinsight

import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.tween
import androidx.compose.runtime.Stable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import com.forkmesh.nativeapp.codeinsight.EmberLayout
import com.forkmesh.nativeapp.codeinsight.EmberMap
import com.forkmesh.nativeapp.codeinsight.EmberView
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch

@Stable
class EmberMapState(private val scope: CoroutineScope) {
    var layout by mutableStateOf(EmberLayout.EMPTY)
        private set
    var width by mutableStateOf(0.0)
        private set
    var height by mutableStateOf(0.0)
        private set
    var density = 1.0
    var view by mutableStateOf(EmberView())
        private set
    var focus by mutableIntStateOf(0)
        private set
    var selected by mutableIntStateOf(-1)
        private set
    var linkProgress by mutableFloatStateOf(1f)
        private set
    var moving by mutableStateOf(false)
        private set
    var interaction by mutableIntStateOf(0)
        private set
    var reducedMotion = false

    private val transition = Animatable(0f)
    private val links = Animatable(1f)
    private var transitionJob: Job? = null

    val ready: Boolean get() = !layout.isEmpty && width > 0 && height > 0

    val margin: Double get() = 14.0 * density

    fun useLayout(next: EmberLayout) {
        if (next === layout) return
        layout = next
        focus = 0
        selected = -1
        if (ready) view = fit(0)
    }

    fun resize(w: Double, h: Double) {
        if (w == width && h == height) return
        width = w
        height = h
        if (!layout.isEmpty && focus in layout.nodes.indices) view = fit(focus)
    }

    fun fit(node: Int): EmberView = EmberMap.fitTransform(layout.nodes[node], width, height, margin)

    fun screenRadius(node: Int): Double = if (node in layout.nodes.indices) layout.nodes[node].r * view.k else 0.0

    fun screenX(x: Double): Double = EmberMap.toScreenX(view, x, width)

    fun screenY(y: Double): Double = EmberMap.toScreenY(view, y, height)

    fun minScale(): Double = EmberMap.minScale(layout, width, height, margin)

    fun maxScale(): Double = EmberMap.maxScale(layout, width, height, density, margin)

    fun focusNode(node: Int, animate: Boolean = true) {
        if (!ready || node !in layout.nodes.indices) return
        val target = if (layout.isFolder(node)) node else maxOf(0, layout.nodes[node].parent)
        focus = target
        animateTo(fit(target), animate)
    }

    fun zoomOut() {
        if (!ready) return
        val parent = layout.nodes[focus].parent
        focusNode(if (parent >= 0) parent else 0)
    }

    fun zoomBy(factor: Double, px: Double = width / 2, py: Double = height / 2) {
        if (!ready) return
        animateTo(EmberMap.zoomAround(view, factor, px, py, width, height, minScale(), maxScale()), true)
        touched()
    }

    fun transform(centroidX: Double, centroidY: Double, panX: Double, panY: Double, zoom: Double) {
        if (!ready) return
        stop()
        val k = (view.k * zoom).coerceIn(minScale(), maxScale())
        val ax = EmberMap.toWorldX(view, centroidX - panX, width)
        val ay = EmberMap.toWorldY(view, centroidY - panY, height)
        view = EmberView(ax - (centroidX - width / 2) / k, ay - (centroidY - height / 2) / k, k)
        moving = true
        touched()
    }

    fun settle() {
        moving = false
        if (!ready) return
        focus = EmberMap.focusFromView(layout, view, width, height)
    }

    fun select(file: Int, zoomTo: Boolean) {
        val changed = file != selected
        selected = file
        if (changed && file >= 0) {
            if (reducedMotion) {
                linkProgress = 1f
            } else {
                scope.launch {
                    links.snapTo(0f)
                    links.animateTo(1f, tween(420, easing = LinearEasing)) { linkProgress = value }
                }
            }
        }
        if (file >= 0 && zoomTo && ready) {
            val node = layout.fileNode(file)
            if (node < 0) return
            val r = screenRadius(node)
            val x = screenX(layout.nodes[node].x)
            val y = screenY(layout.nodes[node].y)
            val inset = 40.0 * density
            val visible = r >= 20 * density && x in inset..(width - inset) && y in inset..(height - inset)
            if (!visible) focusNode(EmberMap.readableFocus(layout, node, width, height, 48 * density, margin))
        }
    }

    private fun touched() {
        interaction++
    }

    private fun stop() {
        transitionJob?.cancel()
        transitionJob = null
    }

    private fun animateTo(target: EmberView, animate: Boolean) {
        stop()
        if (!animate || reducedMotion) {
            view = target
            moving = false
            return
        }
        val from = view
        moving = true
        transitionJob = scope.launch {
            transition.snapTo(0f)
            try {
                transition.animateTo(1f, tween(450, easing = LinearEasing)) {
                    view = EmberMap.interpolateView(from, target, EmberMap.easeInOut(value.toDouble()), width, height)
                }
                view = target
            } finally {
                moving = false
            }
        }
    }
}
