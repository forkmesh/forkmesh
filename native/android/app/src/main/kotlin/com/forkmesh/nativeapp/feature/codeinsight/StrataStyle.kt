package com.forkmesh.nativeapp.feature.codeinsight

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.FilterChip
import androidx.compose.material3.FilterChipDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.codeinsight.StrataGateStatus
import com.forkmesh.nativeapp.codeinsight.StrataSeverity
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon

object StrataColors {
    val healthy = Color(0xFF3FB98F)
    val problematic = Color(0xFFE3B341)
    val unhealthy = Color(0xFFE5534B)
    val dependentDark = Color(0xFFBC8CFF)
    val dependentLight = Color(0xFF8250DF)
    private val ratings = listOf(Color(0xFF3FB98F), Color(0xFF8CC265), Color(0xFFE3B341), Color(0xFFF0883E), Color(0xFFE5534B))
    private val stable = Color(0xFF4C8EDA)
    private val middle = Color(0xFF9A7CE0)
    private val volatile = Color(0xFFF09A4A)
    private val warm = Color(0xFFD29922)
    private val hot = Color(0xFFF85149)

    fun mix(a: Color, b: Color, t: Double): Color {
        val k = t.coerceIn(0.0, 1.0).toFloat()
        return Color(
            red = a.red + (b.red - a.red) * k,
            green = a.green + (b.green - a.green) * k,
            blue = a.blue + (b.blue - a.blue) * k,
            alpha = a.alpha + (b.alpha - a.alpha) * k,
        )
    }

    fun scale(t: Double): Color {
        val v = t.coerceIn(0.0, 1.0)
        return if (v < 0.5) mix(healthy, problematic, v * 2.0) else mix(problematic, unhealthy, (v - 0.5) * 2.0)
    }

    fun health(health: Double): Color = scale((10.0 - health.coerceIn(1.0, 10.0)) / 9.0)

    fun rating(rating: Int): Color = ratings[rating.coerceIn(0, 4)]

    fun severity(severity: StrataSeverity): Color = when (severity) {
        StrataSeverity.Critical -> Color(0xFFE5534B)
        StrataSeverity.High -> Color(0xFFF0883E)
        StrataSeverity.Medium -> Color(0xFFE3B341)
        StrataSeverity.Low -> Color(0xFF8B949E)
    }

    fun severity(index: Int): Color = if (index < 0) healthy else severity(StrataSeverity.entries[index.coerceIn(0, 3)])

    fun gate(status: StrataGateStatus): Color = when (status) {
        StrataGateStatus.Fail -> unhealthy
        StrataGateStatus.Warn -> problematic
        StrataGateStatus.Pass -> healthy
    }

    fun instability(instability: Double): Color {
        val t = instability.coerceIn(0.0, 1.0)
        return if (t < 0.5) mix(stable, middle, t * 2.0) else mix(middle, volatile, (t - 0.5) * 2.0)
    }

    fun change(t: Double, dark: Boolean): Color {
        val cold = if (dark) Color(0xFF2A3A4E) else Color(0xFFC8D3E0)
        return if (t < 0.5) mix(cold, warm, t * 2.0) else mix(warm, hot, (t - 0.5) * 2.0)
    }

    fun readableOn(fill: Color): Color {
        val l = 0.2126 * fill.red + 0.7152 * fill.green + 0.0722 * fill.blue
        return if (l > 0.52) Color(0xFF0D1117) else Color.White
    }
}

@Composable
internal fun strataDependent(): Color = if (Fm.dark) StrataColors.dependentDark else StrataColors.dependentLight

@Composable
internal fun StrataRatingChip(rating: Int, size: Dp = 22.dp, description: String? = null) {
    val color = StrataColors.rating(rating)
    Box(
        Modifier
            .size(size)
            .background(color, RoundedCornerShape(size * 0.25f))
            .then(if (description != null) Modifier.semantics { contentDescription = description } else Modifier),
        contentAlignment = Alignment.Center,
    ) {
        Text(
            com.forkmesh.nativeapp.codeinsight.Strata.ratingLetter(rating).toString(),
            color = StrataColors.readableOn(color),
            fontWeight = FontWeight.Bold,
            fontSize = (size.value * 0.55f).sp,
        )
    }
}

@Composable
internal fun StrataDot(color: Color, size: Dp = 9.dp) {
    Box(Modifier.size(size).background(color, CircleShape))
}

@Composable
internal fun StrataChip(label: String, selected: Boolean, onClick: () -> Unit, icon: String? = null, description: String? = null) {
    val colors = Fm.colors
    FilterChip(
        selected = selected,
        onClick = onClick,
        label = { Text(label, maxLines = 1) },
        leadingIcon = if (icon != null) {
            { FmIcon(icon, size = 18.dp) }
        } else {
            null
        },
        modifier = if (description != null) Modifier.semantics { contentDescription = description } else Modifier,
        colors = FilterChipDefaults.filterChipColors(
            containerColor = Color.Transparent,
            labelColor = colors.textSecondary,
            iconColor = colors.textTertiary,
            selectedContainerColor = colors.accentSubtle,
            selectedLabelColor = colors.accent,
            selectedLeadingIconColor = colors.accent,
        ),
        border = FilterChipDefaults.filterChipBorder(
            enabled = true,
            selected = selected,
            borderColor = colors.border,
            selectedBorderColor = Color.Transparent,
        ),
    )
}
