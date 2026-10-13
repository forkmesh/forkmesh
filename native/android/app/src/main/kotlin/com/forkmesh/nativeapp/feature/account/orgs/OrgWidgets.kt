package com.forkmesh.nativeapp.feature.account.orgs

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import com.forkmesh.nativeapp.account.OrgRoleTone
import com.forkmesh.nativeapp.account.orgRoleLabel
import com.forkmesh.nativeapp.account.orgRoleTone
import com.forkmesh.nativeapp.feature.account.AccountLabeledField
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmPanelHeader
import com.forkmesh.nativeapp.ui.kit.FmStatusBadge
import com.forkmesh.nativeapp.ui.theme.Fm

@Composable
fun OrgScreenHeader(
    title: String,
    onBack: () -> Unit,
    modifier: Modifier = Modifier,
    subtitle: String? = null,
    trailing: (@Composable () -> Unit)? = null,
) {
    Box(
        modifier
            .fillMaxWidth()
            .background(Fm.colors.bgRaised),
    ) {
        FmPanelHeader(
            title = title,
            subtitle = subtitle,
            leading = { FmIconButton(icon = "arrow_back", label = "Back", onPressed = onBack, color = Fm.colors.textPrimary) },
            trailing = trailing,
        )
    }
}

@Composable
fun OrgRoleBadge(role: String, modifier: Modifier = Modifier) {
    val c = Fm.colors
    val color = when (orgRoleTone(role)) {
        OrgRoleTone.Accent -> c.accent
        OrgRoleTone.Warning -> c.warning
        OrgRoleTone.Tertiary -> c.textTertiary
    }
    FmStatusBadge(label = orgRoleLabel(role), color = color, modifier = modifier)
}

@Composable
fun OrgTextField(
    value: String,
    onValueChange: (String) -> Unit,
    label: String,
    modifier: Modifier = Modifier,
    autofocus: Boolean = false,
) {
    AccountLabeledField(
        label = label,
        value = value,
        onValueChange = onValueChange,
        modifier = modifier,
        autofocus = autofocus,
    )
}
