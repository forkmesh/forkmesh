package com.forkmesh.nativeapp.feature.account.projects

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.account.projects.ProbeResult
import com.forkmesh.nativeapp.account.projects.Project
import com.forkmesh.nativeapp.feature.account.AccountFilledButton
import com.forkmesh.nativeapp.feature.account.AccountLabeledField
import com.forkmesh.nativeapp.feature.account.AccountOutlinedButton
import com.forkmesh.nativeapp.ui.kit.fmSafeAreaInsets
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.launch

@Composable
fun ChooseInstance(projects: ProjectsService, project: Project) {
    val c = Fm.colors
    val scope = rememberCoroutineScope()
    var host by remember(project.id) { mutableStateOf("") }
    var checking by remember(project.id) { mutableStateOf(false) }
    var result by remember(project.id) { mutableStateOf<ProbeResult?>(null) }
    val verified = result?.takeIf { it.ok }?.project

    fun check() {
        if (checking || host.isBlank()) return
        checking = true
        scope.launch {
            result = projects.checkInstance(host)
            checking = false
        }
    }

    fun attach() {
        val instance = verified ?: return
        checking = true
        scope.launch {
            projects.attachInstance(project.id, instance)
            checking = false
        }
    }

    Column(
        Modifier
            .fillMaxSize()
            .background(c.bgBase)
            .windowInsetsPadding(fmSafeAreaInsets())
            .padding(FmSpace.x4)
            .testTag("choose-instance"),
        verticalArrangement = Arrangement.Center,
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Column(Modifier.widthIn(max = 480.dp).fillMaxWidth()) {
            ProjectIcon(project, size = 48.dp)
            Spacer(Modifier.height(FmSpace.x3))
            Text("Choose an instance", style = MaterialTheme.typography.titleLarge)
            Spacer(Modifier.height(FmSpace.x2))
            Text(
                "${project.label} is not connected to any instance yet. Enter the one it should use: its account, " +
                    "repositories and chat live there. Until you choose, nothing is loaded from any instance.",
                style = Fm.text.bodySmall.copy(color = c.textSecondary),
            )
            Spacer(Modifier.height(FmSpace.x4))
            AccountLabeledField(
                label = "Instance",
                value = host,
                onValueChange = { host = it; result = null },
                hint = "relay.example.com",
            )
            result?.let { probe ->
                Text(probe.message, style = Fm.text.bodySmall.copy(color = if (probe.ok) c.success else c.danger))
                Spacer(Modifier.height(FmSpace.x3))
            }
            if (verified == null) {
                AccountFilledButton(
                    label = if (checking) "Checking…" else "Check instance",
                    icon = "refresh",
                    onClick = { check() },
                    enabled = !checking && host.isNotBlank(),
                    modifier = Modifier.fillMaxWidth().testTag("choose-instance-check"),
                )
            } else {
                AccountFilledButton(
                    label = "Connect to ${verified.host}",
                    icon = "check",
                    onClick = { attach() },
                    enabled = !checking,
                    modifier = Modifier.fillMaxWidth().testTag("choose-instance-connect"),
                )
            }
            Spacer(Modifier.height(FmSpace.x2))
            AccountOutlinedButton(
                label = "Switch project",
                onClick = { ProjectDrawer.open() },
                modifier = Modifier.fillMaxWidth(),
            )
        }
    }
}
