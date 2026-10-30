package com.forkmesh.nativeapp.feature.market

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.ui.draw.alpha
import com.forkmesh.nativeapp.market.InstallAllPlan
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.market.MarketCardAction
import com.forkmesh.nativeapp.market.MarketModel
import com.forkmesh.nativeapp.market.MarketPackage
import com.forkmesh.nativeapp.market.MarketText
import com.forkmesh.nativeapp.market.MarketUninstall
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius

internal val MARKET_LIST_TAIL = 72.dp
private val THUMB_WIDTH = 96.dp
private val ACTION_WIDTH = 84.dp

@Composable
internal fun MarketInstallAll(plan: InstallAllPlan, busy: Boolean, onClick: () -> Unit) {
    Row(
        Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 6.dp),
        horizontalArrangement = Arrangement.spacedBy(8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        val label = when {
            busy -> "Installing…"
            plan.count == 0 -> "All installed"
            else -> "Install all (${plan.count})"
        }
        MarketChip(label, selected = plan.count > 0, icon = "download", enabled = plan.count > 0 && !busy, onClick = onClick)
        if (plan.skipped > 0) {
            Text("${plan.skipped} need a desktop or organization", color = Fm.colors.textSecondary, style = Fm.text.bodySmall)
        }
    }
}

@Composable
private fun MarketChip(label: String, selected: Boolean, icon: String? = null, dropdown: Boolean = false, enabled: Boolean = true, onClick: () -> Unit) {
    val c = Fm.colors
    val shape = RoundedCornerShape(FmRadius.full)
    val tint = if (selected) c.accent else c.textSecondary
    Row(
        Modifier
            .height(32.dp)
            .clip(shape)
            .background(if (selected) c.accent.copy(alpha = .14f) else c.bgBase)
            .border(1.dp, if (selected) c.accent else c.border, shape)
            .alpha(if (enabled) 1f else .5f)
            .clickable(enabled = enabled, role = Role.Button, onClick = onClick)
            .padding(start = if (icon != null) 8.dp else 12.dp, end = if (dropdown) 6.dp else 12.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(4.dp),
    ) {
        if (icon != null) FmIcon(icon, size = 18.dp, tint = tint)
        Text(label, style = Fm.text.labelMedium, color = if (selected) c.accent else c.textPrimary, maxLines = 1)
        if (dropdown) FmIcon("arrow_drop_down", size = 18.dp, tint = tint)
    }
}

@Composable
internal fun MarketCard(
    pkg: MarketPackage,
    model: MarketModel,
    action: MarketCardAction,
    busy: Boolean,
    failure: String?,
    onOpenPackage: () -> Unit,
    onOpen: () -> Unit,
    onInstall: (String?) -> Unit,
    version: Int,
    uninstall: MarketUninstall? = null,
    uninstalling: Boolean = false,
    onUninstall: (String?) -> Unit = {},
) {
    @Suppress("UNUSED_EXPRESSION") version
    val c = Fm.colors
    val nested = model.nested(pkg).size
    val stats = MarketText.stats(model.counts[pkg.id], model.usage[pkg.id], model.ratings[pkg.id])
    val facts = stats + listOfNotNull(
        if (pkg.free) "Free" else "Paid",
        nested.takeIf { it > 0 }?.let { "+$it included" },
    )
    Row(
        Modifier.fillMaxWidth().clickable(onClick = onOpenPackage).padding(horizontal = 16.dp, vertical = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        val screenshot = pkg.screenshots.firstOrNull()
        if (screenshot != null) {
            MarketScreenshotImage(
                screenshot,
                Modifier.width(THUMB_WIDTH),
                maxWidth = MarketImages.THUMBNAIL_WIDTH,
                radius = FmRadius.md,
                describe = false,
                contentScale = ContentScale.Fit,
            )
        } else {
            Box(
                Modifier.width(THUMB_WIDTH).aspectRatio(16f / 9f).clip(RoundedCornerShape(FmRadius.md)).background(c.bgOverlay),
                contentAlignment = Alignment.Center,
            ) { FmIcon("extension", size = 22.dp, tint = c.textTertiary) }
        }
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Text(pkg.name, style = Fm.text.titleSmall, color = c.textPrimary, maxLines = 1, overflow = TextOverflow.Ellipsis)
            if (pkg.summary.isNotEmpty()) {
                Text(pkg.summary, style = Fm.text.bodySmall, color = c.textSecondary, maxLines = 1, overflow = TextOverflow.Ellipsis)
            }
            Text(facts.joinToString(" · "), style = Fm.text.labelSmall, color = c.textTertiary, maxLines = 1, overflow = TextOverflow.Ellipsis)
            if (failure != null) Text(failure, style = Fm.text.labelSmall, color = c.danger, maxLines = 2, overflow = TextOverflow.Ellipsis)
        }
        Column(horizontalAlignment = Alignment.End, verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Box(Modifier.widthIn(min = ACTION_WIDTH).heightIn(min = 34.dp), contentAlignment = Alignment.CenterEnd) {
                MarketCardButton(pkg, model, action, busy, onOpen, onInstall)
            }
            if (uninstalling) {
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    FmCircularProgressIndicator(Modifier.size(16.dp), strokeWidth = 2.dp)
                    Text("Removing", style = Fm.text.labelSmall, color = c.textSecondary)
                }
            } else if (uninstall != null && !busy) {
                MarketUninstallButton(uninstall, onUninstall)
            }
        }
    }
}

@Composable
private fun MarketUninstallButton(uninstall: MarketUninstall, onUninstall: (String?) -> Unit) {
    val organizations = (uninstall as? MarketUninstall.Organization)?.organizations.orEmpty()
    if (organizations.size <= 1) {
        FmToneButton("Uninstall", onPressed = { onUninstall(organizations.firstOrNull()) }, tone = FmTone.Danger)
        return
    }
    var open by remember { mutableStateOf(false) }
    Box {
        FmToneButton("Uninstall", onPressed = { open = true }, tone = FmTone.Danger)
        FmDropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            organizations.forEach { org ->
                FmDropdownMenuItem("Uninstall from $org", onClick = { open = false; onUninstall(org) }, textColor = Fm.colors.danger)
            }
        }
    }
}

@Composable
private fun MarketCardButton(
    pkg: MarketPackage,
    model: MarketModel,
    action: MarketCardAction,
    busy: Boolean,
    onOpen: () -> Unit,
    onInstall: (String?) -> Unit,
) {
    val c = Fm.colors
    if (busy) {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            FmCircularProgressIndicator(Modifier.size(16.dp), strokeWidth = 2.dp)
            Text("Installing", style = Fm.text.labelSmall, color = c.textSecondary)
        }
        return
    }
    when (action) {
        MarketCardAction.OPEN -> FmToneButton("Open", onPressed = onOpen, tone = FmTone.Accent)
        MarketCardAction.INSTALLED -> Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(4.dp)) {
            FmIcon("check", size = 16.dp, tint = c.success)
            Text("Installed", style = Fm.text.labelMedium, color = c.success)
        }
        MarketCardAction.INSTALL, MarketCardAction.INSTALL_ON_PHONE ->
            FmToneButton("Install", onPressed = { onInstall(null) }, tone = FmTone.Accent, filled = true)
        MarketCardAction.NEEDS_ORGANIZATION -> FmToneButton("Get", onPressed = { onInstall(null) }, tone = FmTone.Accent)
        MarketCardAction.PICK_ORGANIZATION -> {
            var open by remember { mutableStateOf(false) }
            Box {
                FmToneButton("Install", onPressed = { open = true }, tone = FmTone.Accent, filled = true)
                FmDropdownMenu(expanded = open, onDismissRequest = { open = false }) {
                    model.installTargets(pkg).filter { !it.isInstalled(pkg.id) }.forEach { org ->
                        FmDropdownMenuItem("Install for ${org.organization}", onClick = { open = false; onInstall(org.organization) })
                    }
                }
            }
        }
        MarketCardAction.DESKTOP -> Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(4.dp)) {
            val world = pkg.world && !pkg.desktop
            FmIcon(if (world) "public" else "desktop_windows", size = 16.dp, tint = c.textTertiary)
            Text(if (world) "World" else "Desktop", style = Fm.text.labelMedium, color = c.textTertiary)
        }
    }
}
