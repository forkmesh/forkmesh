package com.forkmesh.nativeapp.shell

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.height
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmSpace

class PlaceholderScreen(private val name: String, private val icon: String) : Screen() {
    override val title: String get() = name

    @Composable
    override fun Content(navigator: Navigator) {
        Column(
            modifier = Modifier.fillMaxSize().background(Fm.colors.bgBase),
            verticalArrangement = Arrangement.Center,
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            FmIcon(icon, size = 40.dp, tint = Fm.colors.textTertiary)
            Spacer(Modifier.height(FmSpace.x3))
            Text(name, style = MaterialTheme.typography.titleMedium, color = Fm.colors.textPrimary)
        }
    }
}
