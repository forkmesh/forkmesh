package com.forkmesh.nativeapp.feature.market

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.market.PackageOrganization
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter

internal fun shortDate(ms: Long): String =
    if (ms <= 0) "" else Instant.ofEpochMilli(ms).atZone(ZoneId.systemDefault()).format(DateTimeFormatter.ofPattern("d MMM yyyy"))

@Composable
internal fun ErrorLine(text: String) {
    if (text.isNotEmpty()) Text(text, Modifier.padding(horizontal = 16.dp, vertical = 4.dp), color = Fm.colors.danger, style = Fm.text.bodySmall)
}

@Composable
internal fun OrganizationPicker(
    organizations: List<PackageOrganization>,
    selected: String,
    packageName: String,
    onSelect: (String) -> Unit,
) {
    var open by remember { mutableStateOf(false) }
    Box {
        FmListRow(
            selected.ifEmpty { "Choose an organization" },
            subtitle = if (organizations.none { it.installed }) "$packageName is not installed for any of your organizations." else "Organization",
            leading = { FmIcon("corporate_fare") },
            onTap = { open = true },
            minHeight = 52.dp,
        )
        FmDropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            organizations.forEach { org ->
                FmDropdownMenuItem(
                    org.name + if (org.installed) "" else " (not installed)",
                    onClick = { open = false; onSelect(org.name) },
                )
            }
        }
    }
}
