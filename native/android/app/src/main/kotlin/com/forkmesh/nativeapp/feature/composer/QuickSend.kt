package com.forkmesh.nativeapp.feature.composer

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.agents.AgentModelChoice
import com.forkmesh.nativeapp.agents.AgentStartTarget
import com.forkmesh.nativeapp.agents.PromptAttachment
import com.forkmesh.nativeapp.composer.QuickAgentLauncher
import com.forkmesh.nativeapp.composer.QuickLaunchDestination
import com.forkmesh.nativeapp.composer.QuickLaunchFix
import com.forkmesh.nativeapp.composer.QuickLaunchOutcome
import com.forkmesh.nativeapp.composer.QuickLaunchRunOptions
import com.forkmesh.nativeapp.ui.kit.FmMessenger
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext

class QuickSendState internal constructor(internal val launcher: QuickAgentLauncher) {
    private val lock = Mutex()
    private var version by mutableIntStateOf(0)

    var sending by mutableStateOf(false)
        private set
    var fix by mutableStateOf<QuickLaunchFix?>(null)
        private set
    var message by mutableStateOf<String?>(null)
        private set

    val contextLine: String get() = version.let { launcher.contextLine }
    val model: AgentModelChoice get() = version.let { launcher.model }
    val targets: List<AgentStartTarget> get() = version.let { launcher.targets ?: emptyList() }
    val repository: AgentStartTarget? get() = version.let { launcher.repository }
    val destination: QuickLaunchDestination? get() = version.let { launcher.destination }
    val destinations: List<QuickLaunchDestination> get() = version.let { launcher.destinations }
    val modelChoices: List<AgentModelChoice> get() = version.let { launcher.modelChoices }
    val runOptions: QuickLaunchRunOptions get() = version.let { launcher.runOptions }
    val takesRunOptions: Boolean get() = version.let { launcher.takesRunOptions }
    val takesNoWorktree: Boolean get() = version.let { launcher.takesNoWorktree }

    fun detail(destination: QuickLaunchDestination): String = version.let { launcher.detail(destination) }

    private suspend fun <T> run(block: QuickAgentLauncher.() -> T): T {
        val result = lock.withLock { withContext(Dispatchers.IO) { launcher.block() } }
        version++
        return result
    }

    suspend fun prepare(currentRepo: String?) = run { prepare(currentRepo) }

    suspend fun choose(target: AgentStartTarget) {
        clearFix()
        run { choose(target) }
    }

    suspend fun choose(destination: QuickLaunchDestination) {
        clearFix()
        run { choose(destination) }
    }

    suspend fun chooseModel(choice: AgentModelChoice) {
        clearFix()
        run { chooseModel(choice) }
    }

    suspend fun credentialsChanged() = run { credentialsChanged() }

    suspend fun toggleYolo() = run { toggleYolo() }

    suspend fun toggleWorktree() = run { toggleWorktree() }

    fun clearFix() {
        fix = null
        message = null
    }

    suspend fun send(prompt: String, images: List<PromptAttachment>): QuickLaunchOutcome {
        if (sending) return QuickLaunchOutcome.Ignored
        sending = true
        clearFix()
        try {
            val outcome = run { send(prompt, images) }
            when (outcome) {
                is QuickLaunchOutcome.NeedsFix -> {
                    fix = outcome.fix
                    message = outcome.message
                }
                is QuickLaunchOutcome.Failed -> message = outcome.message
                else -> {}
            }
            return outcome
        } finally {
            sending = false
        }
    }
}

@Composable
internal fun QuickContextLine(state: QuickSendState, onOpenPicker: () -> Unit, onFix: (QuickLaunchFix) -> Unit) {
    val c = Fm.colors
    Column(Modifier.fillMaxWidth().padding(bottom = FmSpace.x2)) {
        if (state.takesRunOptions) QuickRunOptions(state)
        Row(
            Modifier
                .clip(RoundedCornerShape(FmRadius.full))
                .clickable(onClick = onOpenPicker)
                .semantics {
                    role = Role.Button
                    contentDescription = "Runs on ${state.contextLine}. Change"
                }
                .padding(horizontal = FmSpace.x2, vertical = FmSpace.x1),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            FmIcon("smart_toy_outlined", size = 16.dp, tint = c.textTertiary)
            Spacer(Modifier.width(FmSpace.x1))
            Text(
                state.contextLine,
                Modifier.weight(1f, fill = false),
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                style = Fm.text.bodySmall.copy(color = c.textSecondary),
            )
            FmIcon("keyboard_arrow_down_rounded", size = 16.dp, tint = c.textTertiary)
        }
        val note = state.message
        if (note != null) {
            Text(
                note,
                Modifier.padding(horizontal = FmSpace.x2, vertical = FmSpace.x1),
                style = Fm.text.bodySmall.copy(color = if (state.fix == null) c.danger else c.warning),
            )
        }
        val fix = state.fix
        if (fix != null) {
            Text(
                fix.action(ComposerFeature.DEVICE_NAME),
                Modifier
                    .padding(horizontal = FmSpace.x2)
                    .clip(RoundedCornerShape(FmRadius.full))
                    .clickable { onFix(fix) }
                    .semantics { role = Role.Button }
                    .padding(horizontal = FmSpace.x2, vertical = FmSpace.x1),
                style = Fm.text.labelLarge.copy(color = c.accent),
            )
        }
    }
}

@Composable
private fun QuickRunOptions(state: QuickSendState) {
    val scope = rememberCoroutineScope()
    val options = state.runOptions
    val noWorktree = state.takesNoWorktree
    Row(
        Modifier.padding(horizontal = FmSpace.x1, vertical = FmSpace.x1),
        horizontalArrangement = Arrangement.spacedBy(FmSpace.x2),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        RunOptionToggle(
            "YOLO",
            "bolt_outlined",
            on = options.yolo,
            enabled = options.worktree || !noWorktree,
            description = "YOLO: when the run finishes, merge it into the default branch, push, and delete the agent, its branch and worktree",
        ) { scope.launch { state.toggleYolo() } }
        if (noWorktree) {
            RunOptionToggle(
                "No worktree",
                "folder_off_outlined",
                on = !options.worktree,
                enabled = true,
                description = "No worktree: the next run works straight in the repository folder on the desktop, with no branch or worktree",
            ) { scope.launch { state.toggleWorktree() } }
        }
    }
}

@Composable
private fun RunOptionToggle(title: String, icon: String, on: Boolean, enabled: Boolean, description: String, onTap: () -> Unit) {
    val c = Fm.colors
    val active = on && enabled
    val tint = if (active) c.accent else c.textTertiary
    val shape = RoundedCornerShape(FmRadius.full)
    Row(
        Modifier
            .alpha(if (enabled) 1f else 0.5f)
            .clip(shape)
            .then(if (active) Modifier.background(c.accentSubtle, shape) else Modifier.border(1.dp, c.textTertiary.copy(alpha = 0.35f), shape))
            .toggleable(value = on, role = Role.Switch, onValueChange = { onTap() })
            .semantics { contentDescription = description }
            .heightIn(min = 24.dp)
            .padding(horizontal = FmSpace.x2),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        FmIcon(icon, size = 12.dp, tint = tint)
        Spacer(Modifier.width(3.dp))
        Text(title, style = Fm.text.labelSmall.copy(color = tint))
    }
}

internal sealed class QuickPick {
    data class Repo(val target: AgentStartTarget) : QuickPick()
    data class Where(val destination: QuickLaunchDestination) : QuickPick()
}

internal suspend fun pickQuickContext(messenger: FmMessenger, state: QuickSendState): QuickPick? =
    messenger.showModalBottomSheet(isScrollControlled = true) { close ->
        val c = Fm.colors
        Column(Modifier.verticalScroll(rememberScrollState()).padding(bottom = FmSpace.x5)) {
            val destinations = state.destinations
            if (destinations.isNotEmpty()) {
                SheetHeading("Runs on")
                for (destination in destinations) {
                    val title = when (destination) {
                        is QuickLaunchDestination.Node -> destination.name
                        QuickLaunchDestination.Device -> "This phone"
                        is QuickLaunchDestination.Desktop -> "${destination.name} (desktop)"
                    }
                    SheetOption(title, state.detail(destination), destination == state.destination) {
                        close(QuickPick.Where(destination))
                    }
                }
            }
            SheetHeading("Repository")
            val targets = state.targets
            if (targets.isEmpty()) {
                Text(
                    "No repositories yet. Create or mirror one from the Code tab.",
                    Modifier.padding(horizontal = FmSpace.x4),
                    style = Fm.text.bodySmall.copy(color = c.textTertiary),
                )
            }
            for (target in targets) {
                SheetOption(target.label, target.detail, target.key == state.repository?.key) { close(QuickPick.Repo(target)) }
            }
        }
    }

@Composable
private fun SheetHeading(text: String) {
    Text(
        text,
        Modifier.padding(start = FmSpace.x4, end = FmSpace.x4, top = FmSpace.x4, bottom = FmSpace.x2),
        style = Fm.text.titleMedium.copy(color = Fm.colors.textPrimary),
    )
}

@Composable
private fun SheetOption(title: String, detail: String, selected: Boolean, onTap: () -> Unit) {
    val c = Fm.colors
    Row(
        Modifier
            .fillMaxWidth()
            .heightIn(min = 56.dp)
            .clickable(onClick = onTap)
            .padding(horizontal = FmSpace.x4, vertical = FmSpace.x2),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f)) {
            Text(title, style = Fm.text.bodyLarge.copy(color = c.textPrimary))
            if (detail.isNotEmpty()) Text(detail, style = Fm.text.bodySmall.copy(color = c.textSecondary))
        }
        if (selected) FmIcon("check", size = 18.dp, tint = c.accent)
    }
}
