package com.forkmesh.nativeapp.feature.people

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.feature.tools.copyToClipboard
import com.forkmesh.nativeapp.navigation.ProfileLinks
import com.forkmesh.nativeapp.navigation.Route
import com.forkmesh.nativeapp.settings.SolanaAddress
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.QrImage
import com.forkmesh.nativeapp.ui.kit.rememberFmStatus
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmSpace

fun Modifier.openProfile(name: String): Modifier {
    val account = ProfileLinks.account(name) ?: return this
    return clickable(role = Role.Button, onClickLabel = "View @$account") { NavigationFeature.open(Route.Profile(account)) }
}

@Composable
fun ProfileTipContent(name: String, address: String) {
    val valid = SolanaAddress.isValid(address)
    if (!valid) {
        Text("@$name has not entered a Solana address for tips.", style = Fm.text.bodyMedium, color = Fm.colors.textSecondary)
        return
    }
    val context = LocalContext.current
    val status = rememberFmStatus()
    Column(Modifier.fillMaxWidth(), horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
        QrImage(SolanaAddress.qrText(address), size = 180.dp, description = "Solana tip address QR code for @$name")
        SelectionContainer {
            Text(address, style = Fm.mono(), color = Fm.colors.textPrimary, textAlign = TextAlign.Center)
        }
        Text("Scan with a Solana wallet to tip @$name.", style = Fm.text.bodySmall, color = Fm.colors.textSecondary, textAlign = TextAlign.Center)
        FmTextButton(onClick = {
            copyToClipboard(context, address)
            status.show("Address copied")
        }) { Text("Copy address") }
        FmInlineStatus(status)
    }
}
