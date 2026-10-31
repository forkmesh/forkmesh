package com.forkmesh.nativeapp.shell

import com.forkmesh.nativeapp.api.PrefStore
import kotlin.math.floor
import kotlin.math.max
import kotlin.math.min

enum class ShellBarShape(override val raw: String, val title: String) : ShellRaw {
    capsule("capsule", "Capsule"),
    panel("panel", "Panel"),
    squares("squares", "Squares"),
    circles("circles", "Circles"),
    icons("icons", "Icons only");

    val separate: Boolean get() = this == squares || this == circles || this == icons

    val cornered: Boolean get() = this == panel || this == squares

    val backed: Boolean get() = this != icons
}

enum class ShellBarFill(override val raw: String, val title: String) : ShellRaw {
    glass("glass", "Glass"),
    clear("clear", "Clear"),
    tinted("tinted", "Tinted"),
    solid("solid", "Solid")
}

enum class ShellBarLabels(override val raw: String, val title: String) : ShellRaw {
    always("always", "Always"),
    selected("selected", "Selected"),
    never("never", "Never");

    fun shows(selected: Boolean): Boolean = when (this) {
        always -> true
        ShellBarLabels.selected -> selected
        never -> false
    }
}

enum class ShellBarSelection(override val raw: String, val title: String) : ShellRaw {
    lens("lens", "Lens"),
    fill("fill", "Fill"),
    outline("outline", "Outline"),
    glow("glow", "Glow"),
    dot("dot", "Dot"),
    underline("underline", "Line"),
    none("none", "None")
}

enum class ShellBarMarks(override val raw: String, val title: String) : ShellRaw {
    home("home", "Like Home"),
    plain("plain", "Plain")
}

enum class ShellBarSize(override val raw: String, val title: String, val label: String, val scale: Double) : ShellRaw {
    small("small", "S", "Small", 0.85),
    medium("medium", "M", "Medium", 1.0),
    large("large", "L", "Large", 1.15),
    extraLarge("extraLarge", "XL", "Extra large", 1.3)
}

enum class ShellBarWidth(override val raw: String, val title: String) : ShellRaw {
    full("full", "Full width"),
    fit("fit", "Fit the icons")
}

enum class ShellBarRows(override val raw: String, val title: String, val count: Int) : ShellRaw {
    single("single", "Single", 1),
    double("double", "Double", 2)
}

data class ShellBarMetrics(
    val slot: Double,
    val height: Double,
    val gap: Double,
    val padding: Double,
    val width: Double,
    val scrolls: Boolean,
    val rows: Int = 1,
    val columns: Int = 1,
    val rowGap: Double = 0.0,
)

data class ShellBarStyle(
    var shape: ShellBarShape = ShellBarShape.capsule,
    var fill: ShellBarFill = ShellBarFill.glass,
    var icons: ShellHomeIconStyle = ShellHomeIconStyle.filled,
    var size: ShellBarSize = ShellBarSize.medium,
    var labels: ShellBarLabels = ShellBarLabels.always,
    var selection: ShellBarSelection = ShellBarSelection.lens,
    var width: ShellBarWidth = ShellBarWidth.full,
    var corner: Double = 0.5,
    var spacing: Double = 0.0,
    var limit: Int = ShellBarLayout.BAR_LIMIT,
    var showCounts: Boolean = false,
    var showActivity: Boolean = true,
    var highlightFrequent: Boolean = false,
    var marks: ShellBarMarks = ShellBarMarks.home,
    var rows: ShellBarRows = ShellBarRows.single,
) {
    val isDefault: Boolean get() = this == ShellBarStyle()

    val clamped: ShellBarStyle
        get() = copy(corner = unit(corner), spacing = unit(spacing), limit = clampedLimit(limit))

    val capacity: Int get() = limit * rows.count

    val gap: Double get() = if (shape.separate) 4 + 12 * spacing else 2 + 10 * spacing

    fun save(store: PrefStore) {
        ShellDecode.write(store, KEY, if (this == ShellBarStyle()) null else toJson())
    }

    fun toJson(): Map<String, Any?> = linkedMapOf(
        "shape" to shape.raw,
        "fill" to fill.raw,
        "icons" to icons.raw,
        "size" to size.raw,
        "labels" to labels.raw,
        "selection" to selection.raw,
        "width" to width.raw,
        "corner" to corner,
        "spacing" to spacing,
        "limit" to limit,
        "showCounts" to showCounts,
        "showActivity" to showActivity,
        "highlightFrequent" to highlightFrequent,
        "marks" to marks.raw,
        "rows" to rows.raw,
    )

    fun columns(count: Int): Int {
        val lines = rows.count
        return max(1, (max(1, count) + lines - 1) / lines)
    }

    fun <Element> rowSplit(elements: List<Element>): List<List<Element>> {
        if (rows != ShellBarRows.double || elements.size <= 1) return listOf(elements)
        val across = columns(elements.size)
        return listOf(elements.take(across), elements.drop(across))
    }

    fun cellHeight(base: Double): Double {
        val scaled = base * size.scale * (if (labels == ShellBarLabels.never) 0.8 else 1.0)
        return max(MINIMUM_SLOT, Math.round(scaled).toDouble())
    }

    fun barRadius(height: Double): Double {
        val outer = height / 2 + PADDING
        return when (shape) {
            ShellBarShape.capsule -> outer
            ShellBarShape.panel -> 4 + corner * (outer - 4)
            ShellBarShape.squares, ShellBarShape.circles, ShellBarShape.icons -> 0.0
        }
    }

    fun cellRadius(side: Double): Double = when (shape) {
        ShellBarShape.circles -> side / 2
        ShellBarShape.squares -> 2 + corner * (side / 2 - 2)
        ShellBarShape.capsule, ShellBarShape.panel, ShellBarShape.icons -> 0.0
    }

    fun metrics(available: Double, count: Int, cell: Double, baseHeight: Double): ShellBarMetrics {
        val lines = if (count > 1) rows.count else 1
        val across = columns(count)
        val n = across.toDouble()
        val pad = PADDING
        val rowGap = this.gap
        var gap = this.gap
        val height = cellHeight(baseHeight)
        val room = max(0.0, available - 2 * pad - gap * (n - 1)) / n
        val slot: Double
        val side: Double
        when (shape) {
            ShellBarShape.squares, ShellBarShape.circles -> {
                side = max(MINIMUM_SLOT, min(height, if (available > 0) room else height))
                slot = side
            }
            ShellBarShape.capsule, ShellBarShape.panel, ShellBarShape.icons -> {
                slot = max(MINIMUM_SLOT, min(cell * size.scale, if (available > 0) room else cell * size.scale))
                side = height
            }
        }
        var content = 2 * pad + n * slot + gap * (n - 1)
        if (available > 0 && width == ShellBarWidth.full && shape.separate && across > 1 && content < available) {
            gap = (available - 2 * pad - n * slot) / (n - 1)
            content = available
        }
        val shown = if (available > 0) (if (width == ShellBarWidth.full) available else min(available, content)) else content
        return ShellBarMetrics(
            slot = floor(slot),
            height = floor(side),
            gap = gap,
            padding = pad,
            width = shown,
            scrolls = available > 0 && content > available + 0.5,
            rows = lines,
            columns = across,
            rowGap = if (lines > 1) rowGap else 0.0,
        )
    }

    companion object {
        const val KEY = "shell/bar-style"
        val LIMIT_CHOICES: List<Int> = listOf(3, 4, 5, 6, 7, 8)
        const val MINIMUM_SLOT = 44.0
        const val PADDING = 4.0

        fun unit(value: Double): Double {
            if (!value.isFinite()) return 0.0
            return min(1.0, max(0.0, value))
        }

        fun clampedLimit(value: Int): Int = min(LIMIT_CHOICES.last(), max(LIMIT_CHOICES.first(), value))

        fun fromJson(values: Map<String, Any?>): ShellBarStyle {
            val base = ShellBarStyle()
            return ShellBarStyle(
                shape = ShellDecode.enumOr(values, "shape", ShellBarShape.entries, base.shape),
                fill = ShellDecode.enumOr(values, "fill", ShellBarFill.entries, base.fill),
                icons = ShellDecode.enumOr(values, "icons", ShellHomeIconStyle.entries, base.icons),
                size = ShellDecode.enumOr(values, "size", ShellBarSize.entries, base.size),
                labels = ShellDecode.enumOr(values, "labels", ShellBarLabels.entries, base.labels),
                selection = ShellDecode.enumOr(values, "selection", ShellBarSelection.entries, base.selection),
                width = ShellDecode.enumOr(values, "width", ShellBarWidth.entries, base.width),
                corner = unit(ShellDecode.doubleOr(values, "corner", base.corner)),
                spacing = unit(ShellDecode.doubleOr(values, "spacing", base.spacing)),
                limit = clampedLimit(ShellDecode.intOr(values, "limit", base.limit)),
                showCounts = ShellDecode.boolOr(values, "showCounts", base.showCounts),
                showActivity = ShellDecode.boolOr(values, "showActivity", base.showActivity),
                highlightFrequent = ShellDecode.boolOr(values, "highlightFrequent", base.highlightFrequent),
                marks = ShellDecode.enumOr(values, "marks", ShellBarMarks.entries, base.marks),
                rows = ShellDecode.enumOr(values, "rows", ShellBarRows.entries, base.rows),
            )
        }

        fun load(store: PrefStore): ShellBarStyle =
            ShellDecode.parseObject(store.getString(KEY))?.let(::fromJson) ?: ShellBarStyle()
    }
}
