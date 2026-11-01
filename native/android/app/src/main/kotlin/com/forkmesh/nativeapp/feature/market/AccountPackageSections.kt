package com.forkmesh.nativeapp.feature.market

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.feature.insights.StatusPageFeature
import com.forkmesh.nativeapp.insights.ServiceState
import com.forkmesh.nativeapp.insights.StatusDots
import com.forkmesh.nativeapp.market.AccountCatalogEntry
import com.forkmesh.nativeapp.market.AccountPackageGraph
import com.forkmesh.nativeapp.market.AccountPackagesStore
import com.forkmesh.nativeapp.market.MarketPackage
import com.forkmesh.nativeapp.market.PackagePlaces
import com.forkmesh.nativeapp.market.PackageSetting
import com.forkmesh.nativeapp.market.PackageSettingType
import com.forkmesh.nativeapp.market.PackagesException
import com.forkmesh.nativeapp.market.StatusMonitorDraft
import com.forkmesh.nativeapp.market.StatusMonitorsStore
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmConfirmDialog
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmSwitch
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.rememberFmStatus
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

@Composable
internal fun AccountPackageSections(pkg: MarketPackage, nested: List<MarketPackage> = emptyList()) {
    val store = StatusPageFeature.store ?: return
    val version by StatusPageFeature.version.collectAsState()
    val scope = rememberCoroutineScope()
    val status = rememberFmStatus()
    val nestedStatus = rememberFmStatus()
    var busy by remember { mutableStateOf(false) }
    var busyNested by remember { mutableStateOf<String?>(null) }
    var confirmRemove by remember { mutableStateOf(false) }
    var confirmNested by remember { mutableStateOf<MarketPackage?>(null) }
    LaunchedEffect(pkg.id) { StatusPageFeature.refreshAccount() }
    @Suppress("UNUSED_EXPRESSION") version
    val entry = store.catalog[pkg.id] ?: AccountCatalogEntry.of(pkg)
    val catalog = store.catalog.values.ifEmpty { listOf(entry) }
    val installed = store.isInstalled(pkg.id)
    val project = store.perProject
    val removeTitle = if (project) "from this project" else "from all your devices"
    val removeNote = AccountPackageGraph.removeNote(entry, catalog, project, store::isInstalled)
    val installNote = AccountPackageGraph.installNote(entry, catalog, project, store::isInstalled)
    val subtitle = when {
        !project -> if (installed) removeNote else installNote
        installed -> AccountPackageGraph.PROJECT_FOOTER
        else -> "$installNote ${AccountPackageGraph.PROJECT_FOOTER}"
    }

    fun change(installing: Boolean) {
        busy = true
        status.clear()
        scope.launch {
            val result = withContext(Dispatchers.IO) { runCatching { if (installing) store.install(pkg.id) else store.uninstall(pkg.id) } }
            busy = false
            result.onSuccess { state ->
                StatusPageFeature.adopt(state)
                status.show(AccountPackageGraph.changedNote(installing, entry, state.changed, store.catalog.values, project))
            }.onFailure { status.error(PackagesException.describe(it)) }
        }
    }

    fun changeNested(child: MarketPackage, installing: Boolean) {
        busyNested = child.id
        nestedStatus.clear()
        scope.launch {
            val result = withContext(Dispatchers.IO) { runCatching { if (installing) store.install(child.id) else store.uninstall(child.id) } }
            busyNested = null
            result.onSuccess { state -> StatusPageFeature.adopt(state) }.onFailure { nestedStatus.error(PackagesException.describe(it)) }
        }
    }

    if (confirmRemove) {
        FmConfirmDialog(
            title = "Remove ${pkg.name} $removeTitle?",
            body = removeNote,
            confirmLabel = "Remove",
            destructive = true,
            onConfirm = {
                confirmRemove = false
                change(installing = false)
            },
            onDismissRequest = { confirmRemove = false },
        )
    }
    confirmNested?.let { child ->
        val childEntry = store.catalog[child.id] ?: AccountCatalogEntry.of(child)
        FmConfirmDialog(
            title = "Remove ${child.name} $removeTitle?",
            body = AccountPackageGraph.removeNote(childEntry, catalog, project, store::isInstalled),
            confirmLabel = "Remove",
            destructive = true,
            onConfirm = {
                confirmNested = null
                changeNested(child, installing = false)
            },
            onDismissRequest = { confirmNested = null },
        )
    }
    FmListRow(
        if (project) "Installed in this project" else "Installed on all your devices",
        subtitle = subtitle,
        trailing = {
            if (busy) {
                FmCircularProgressIndicator(Modifier.size(20.dp))
            } else {
                FmSwitch(checked = installed, onCheckedChange = { on -> if (on) change(installing = true) else confirmRemove = true })
            }
        },
        showChevron = false,
    )
    FmInlineStatus(status)
    val row = store.state?.find(pkg.id)
    if (installed && row != null && row.availablePlaces.isNotEmpty()) PackagePlacesSection(pkg.id, row.places, row.availablePlaces, store)
    if (nested.isNotEmpty()) {
        FmSectionHeader("Included packages", Modifier.padding(top = 8.dp), count = nested.size)
        Text(
            "They are added and removed with ${pkg.name}." + if (installed) " Turn one off to hide it." else "",
            style = Fm.text.bodySmall,
            color = Fm.colors.textTertiary,
        )
        nested.forEach { child ->
            FmListRow(
                child.name,
                subtitle = child.summary.take(120),
                trailing = {
                    if (busyNested == child.id) {
                        FmCircularProgressIndicator(Modifier.size(20.dp))
                    } else {
                        FmSwitch(
                            checked = installed && store.isInstalled(child.id),
                            onCheckedChange = { on -> if (on) changeNested(child, true) else confirmNested = child },
                            enabled = installed && !busy && busyNested == null,
                        )
                    }
                },
                showChevron = false,
            )
        }
        FmInlineStatus(nestedStatus)
    }
    val settings = pkg.settings.ifEmpty { entry.settings }
    if (installed && settings.isNotEmpty()) PackageSettingsSection(pkg.id, settings, store)
    if (installed && pkg.id == StatusDots.STATUS_PAGE) StatusPageFeature.monitors?.let { StatusMonitorsSection(it) }
}

@Composable
private fun PackagePlacesSection(packageId: String, places: List<String>, available: List<String>, store: AccountPackagesStore) {
    val scope = rememberCoroutineScope()
    val status = rememberFmStatus()
    var saving by remember { mutableStateOf("") }

    fun change(place: String, on: Boolean) {
        val next = if (on) places + place else places - place
        saving = place
        status.clear()
        scope.launch {
            val result = withContext(Dispatchers.IO) { runCatching { store.setPlaces(packageId, next) } }
            saving = ""
            result.onSuccess { StatusPageFeature.adopt(it) }.onFailure { status.error(PackagesException.describe(it)) }
        }
    }

    FmSectionHeader("Show on", Modifier.padding(top = 8.dp))
    if (PackagePlaces.HERE !in places) {
        Text("Not shown on this phone. Turn on Phone to show it here.", style = Fm.text.bodySmall, color = Fm.colors.textSecondary)
    }
    available.forEach { place ->
        val on = place in places
        FmListRow(
            PackagePlaces.label(place),
            leading = {
                val icon = when (place) {
                    PackagePlaces.MOBILE -> "smartphone"
                    PackagePlaces.DESKTOP -> "desktop_windows"
                    else -> "language"
                }
                FmIcon(icon, tint = if (on) Fm.colors.textPrimary else Fm.colors.textTertiary)
            },
            trailing = {
                if (saving == place) {
                    FmCircularProgressIndicator(Modifier.size(20.dp))
                } else {
                    FmSwitch(
                        checked = on,
                        onCheckedChange = { change(place, it) },
                        enabled = saving.isEmpty() && !(on && places.count { it in available } <= 1),
                    )
                }
            },
            showChevron = false,
        )
    }
    FmInlineStatus(status)
}

@Composable
private fun PackageSettingsSection(packageId: String, declared: List<PackageSetting>, store: AccountPackagesStore) {
    val scope = rememberCoroutineScope()
    val status = rememberFmStatus()
    var saving by remember { mutableStateOf("") }
    val values = store.settings(packageId)

    fun save(setting: PackageSetting, value: Any) {
        saving = setting.key
        status.clear()
        scope.launch {
            val result = withContext(Dispatchers.IO) { runCatching { store.saveSettings(packageId, mapOf(setting.key to value)) } }
            saving = ""
            result.onSuccess { StatusPageFeature.adopt(it) }.onFailure { status.error(PackagesException.describe(it)) }
        }
    }

    FmSectionHeader("Settings", Modifier.padding(top = 8.dp))
    declared.forEach { setting ->
        val value = setting.valueIn(values)
        when (setting.type) {
            PackageSettingType.BOOLEAN -> FmListRow(
                setting.label,
                trailing = {
                    if (saving == setting.key) {
                        FmCircularProgressIndicator(Modifier.size(20.dp))
                    } else {
                        FmSwitch(checked = value == true, onCheckedChange = { save(setting, it) })
                    }
                },
                showChevron = false,
            )
            PackageSettingType.CHOICE -> {
                var open by remember(setting.key) { mutableStateOf(false) }
                Box {
                    FmListRow(
                        setting.label,
                        subtitle = setting.optionLabel(value),
                        trailing = if (saving == setting.key) ({ FmCircularProgressIndicator(Modifier.size(20.dp)) }) else null,
                        onTap = { open = true },
                    )
                    FmDropdownMenu(expanded = open, onDismissRequest = { open = false }) {
                        setting.options.forEach { option ->
                            FmDropdownMenuItem(
                                option.label,
                                onClick = {
                                    open = false
                                    if (option.value != value) save(setting, option.value)
                                },
                                icon = if (option.value == value) "check" else null,
                            )
                        }
                    }
                }
            }
        }
    }
    FmInlineStatus(status)
}

@Composable
private fun StatusMonitorsSection(store: StatusMonitorsStore) {
    val scope = rememberCoroutineScope()
    val status = rememberFmStatus()
    val listStatus = rememberFmStatus()
    val row by StatusPageFeature.row.collectAsState()
    var name by remember { mutableStateOf("") }
    var url by remember { mutableStateOf("") }
    var adding by remember { mutableStateOf(false) }
    var removing by remember { mutableStateOf("") }
    LaunchedEffect(Unit) { withContext(Dispatchers.IO) { runCatching { store.refresh() } } }
    val list = store.list
    val issue = if (name.isBlank() && url.isBlank()) null else StatusMonitorDraft.issue(name, url)

    fun add() {
        adding = true
        status.clear()
        scope.launch {
            val result = withContext(Dispatchers.IO) { runCatching { store.add(name, url) } }
            adding = false
            result.onSuccess {
                status.show("Added ${name.trim()} to your monitors.")
                name = ""
                url = ""
            }.onFailure { status.error(PackagesException.describe(it)) }
        }
    }

    fun remove(id: String, label: String) {
        removing = id
        listStatus.clear()
        scope.launch {
            val result = withContext(Dispatchers.IO) { runCatching { store.remove(id) } }
            removing = ""
            result.onSuccess { listStatus.show("Removed $label.") }.onFailure { listStatus.error(PackagesException.describe(it)) }
        }
    }

    FmSectionHeader("Your monitors", Modifier.padding(top = 8.dp), count = list.monitors.size)
    Text(
        "Each of your devices loads these addresses itself, on the same schedule as the other dots. An answer below 400 is green; an error, no answer within 10 seconds or 400 and up is red.",
        style = Fm.text.bodySmall,
        color = Fm.colors.textTertiary,
    )
    if (list.monitors.isEmpty()) Text("No monitors yet.", style = Fm.text.bodySmall, color = Fm.colors.textSecondary)
    list.monitors.forEach { monitor ->
        val state = row.dots.firstOrNull { it.id == StatusDots.CUSTOM_PREFIX + monitor.id }?.state
        FmListRow(
            monitor.name,
            subtitle = monitor.url,
            leading = {
                val color = when (state) {
                    ServiceState.Operational -> Fm.colors.success
                    ServiceState.Down -> Fm.colors.danger
                    ServiceState.Degraded -> Fm.colors.warning
                    else -> Fm.colors.textTertiary
                }
                Box(Modifier.size(10.dp).clip(CircleShape).background(color))
            },
            trailing = {
                if (removing == monitor.id) {
                    FmCircularProgressIndicator(Modifier.size(20.dp))
                } else {
                    FmIconButton("delete_outline", "Remove ${monitor.name}", onPressed = { remove(monitor.id, monitor.name) }, color = Fm.colors.danger)
                }
            },
            showChevron = false,
        )
    }
    FmInlineStatus(listStatus)
    Column(Modifier.fillMaxWidth().padding(top = 4.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
        FmTextField(
            name,
            { name = it.take(StatusMonitorDraft.NAME_LIMIT) },
            hint = "Name",
            errorText = if (issue == "invalid_name" && name.isNotEmpty()) PackagesException.sentence("invalid_name") else null,
        )
        FmTextField(
            url,
            { url = it.take(StatusMonitorDraft.URL_LIMIT) },
            hint = "https://example.com/",
            errorText = if (issue == "invalid_url" && url.isNotEmpty()) PackagesException.sentence("invalid_url") else null,
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri, imeAction = ImeAction.Done),
        )
        FmInlineStatus(status)
        if (list.full) Text("You have ${list.max} monitors, the most allowed. Remove one to add another.", style = Fm.text.bodySmall, color = Fm.colors.textTertiary)
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            FmToneButton(
                if (adding) "Adding…" else "Add monitor",
                onPressed = if (!adding && !list.full && name.isNotBlank() && url.isNotBlank() && issue == null) ({ add() }) else null,
                tone = FmTone.Accent,
                filled = true,
                busy = adding,
            )
        }
    }
}
