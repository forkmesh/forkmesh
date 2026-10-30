package com.forkmesh.nativeapp.feature.market

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.platform.LocalFocusManager
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardCapitalization
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.market.MarketFilter
import com.forkmesh.nativeapp.market.MarketModel
import com.forkmesh.nativeapp.market.MarketOptions
import com.forkmesh.nativeapp.market.MarketPlatform
import com.forkmesh.nativeapp.market.MarketPrice
import com.forkmesh.nativeapp.market.MarketSort
import com.forkmesh.nativeapp.market.MarketText
import com.forkmesh.nativeapp.market.PackageInstallMode
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmSafeInput
import com.forkmesh.nativeapp.ui.kit.fmGlass
import com.forkmesh.nativeapp.ui.kit.fmSafeAreaInsets
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon

private val SEARCH_HEIGHT: Dp = 36.dp

@Composable
internal fun MarketNavBar(model: MarketModel, version: Int, onChanged: () -> Unit, onBack: () -> Unit) {
    @Suppress("UNUSED_EXPRESSION") version
    val c = Fm.colors
    Row(
        Modifier
            .fillMaxWidth()
            .windowInsetsPadding(fmSafeAreaInsets(bottom = false))
            .height(52.dp)
            .padding(horizontal = 16.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Box(
            Modifier
                .size(44.dp)
                .fmGlass(CircleShape, interactive = true)
                .clickable(onClick = onBack)
                .semantics {
                    role = Role.Button
                    contentDescription = "Back"
                },
            contentAlignment = Alignment.Center,
        ) { FmIcon("chevron_left", size = 30.dp, tint = c.textPrimary) }
        MarketSearchField(model.query, { model.query = it; onChanged() }, Modifier.weight(1f))
        MarketSortMenu(model, onChanged)
        MarketFilterMenu(model, onChanged)
        PluginMenuButton()
    }
}

@Composable
private fun MarketSearchField(query: String, onQuery: (String) -> Unit, modifier: Modifier) {
    val c = Fm.colors
    val focus = LocalFocusManager.current
    val style = Fm.text.bodyMedium.copy(color = c.textPrimary)
    Row(
        modifier
            .height(SEARCH_HEIGHT)
            .background(c.textPrimary.copy(alpha = if (c.dark) .12f else .07f), RoundedCornerShape(50))
            .padding(horizontal = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        FmIcon("search", size = 18.dp, tint = c.textSecondary)
        BasicTextField(
            value = query,
            onValueChange = FmSafeInput.text(onQuery),
            singleLine = true,
            textStyle = style,
            cursorBrush = SolidColor(c.accent),
            keyboardOptions = KeyboardOptions(autoCorrectEnabled = false, capitalization = KeyboardCapitalization.None, imeAction = ImeAction.Search),
            keyboardActions = KeyboardActions(onSearch = { focus.clearFocus() }),
            modifier = Modifier.weight(1f).semantics { contentDescription = "Search packages" },
            decorationBox = { inner ->
                Box(contentAlignment = Alignment.CenterStart) {
                    if (query.isEmpty()) Text("Search packages", style = style.copy(color = c.textTertiary), maxLines = 1)
                    inner()
                }
            },
        )
        if (query.isNotEmpty()) {
            Box(
                Modifier.size(20.dp).clickable { onQuery("") }.semantics {
                    role = Role.Button
                    contentDescription = "Clear search"
                },
                contentAlignment = Alignment.Center,
            ) { FmIcon("cancel", size = 18.dp, tint = c.textSecondary) }
        }
    }
}

@Composable
private fun MarketSortMenu(model: MarketModel, onChanged: () -> Unit) {
    val c = Fm.colors
    var open by remember { mutableStateOf(false) }
    val options = model.options
    Box {
        FmIconButton(
            "sort",
            "Sort by ${options.sort.title}",
            onPressed = { open = true },
            color = if (options.sort != MarketSort.FEATURED) c.accent else c.textPrimary,
        )
        FmDropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            MarketSort.entries.forEach { sort ->
                FmDropdownMenuItem(sort.title, onClick = {
                    open = false
                    model.options = options.copy(sort = sort)
                    onChanged()
                }, icon = if (sort == options.sort) "check" else null)
            }
        }
    }
}

private class MarketFilterGroup(val title: String, val current: String, val choices: List<Pair<String, Boolean>>, val pick: (Int) -> Unit)

@Composable
private fun MarketFilterMenu(model: MarketModel, onChanged: () -> Unit) {
    val c = Fm.colors
    var open by remember { mutableStateOf(false) }
    var group by remember { mutableStateOf<Int?>(null) }
    val options = model.options
    val active = options.activeFilters + if (model.filter != MarketFilter.ALL) 1 else 0
    fun set(next: MarketOptions) {
        model.options = next
        onChanged()
    }
    val categories = model.categories
    val types = model.types
    val modes = PackageInstallMode.entries
    val groups = listOf(
        MarketFilterGroup("Show", model.filter.title, MarketFilter.entries.map { it.title to (it == model.filter) }) {
            model.filter = MarketFilter.entries[it]
            onChanged()
        },
        MarketFilterGroup(
            "Category",
            options.category.ifEmpty { "All categories" },
            listOf("All categories" to options.category.isEmpty()) + categories.map { it to (it == options.category) },
        ) { set(options.copy(category = if (it == 0) "" else categories[it - 1])) },
        MarketFilterGroup("Runs on", options.platform.title, MarketPlatform.entries.map { it.title to (it == options.platform) }) {
            set(options.copy(platform = MarketPlatform.entries[it]))
        },
        MarketFilterGroup(
            "Installs for",
            options.mode?.title ?: "Anyone",
            listOf("Anyone" to (options.mode == null)) + modes.map { it.title to (it == options.mode) },
        ) { set(options.copy(mode = if (it == 0) null else modes[it - 1])) },
        MarketFilterGroup(
            "Kind",
            if (options.type.isEmpty()) "Every kind" else MarketText.typeLabel(options.type),
            listOf("Every kind" to options.type.isEmpty()) + types.map { MarketText.typeLabel(it) to (it == options.type) },
        ) { set(options.copy(type = if (it == 0) "" else types[it - 1])) },
        MarketFilterGroup("Price", options.price.title, MarketPrice.entries.map { it.title to (it == options.price) }) {
            set(options.copy(price = MarketPrice.entries[it]))
        },
    )
    Box {
        FmIconButton(
            if (active > 0) "filter_alt" else "filter_list",
            if (active > 0) "Filters, $active on" else "Filters",
            onPressed = { open = true },
            color = if (active > 0) c.accent else c.textPrimary,
        )
        FmDropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            groups.forEachIndexed { index, item ->
                FmDropdownMenuItem("${item.title}: ${item.current}", onClick = { open = false; group = index }, icon = "chevron_right")
            }
            if (active > 0) {
                HorizontalDivider(thickness = Dp.Hairline, color = c.border)
                FmDropdownMenuItem("Clear filters", onClick = {
                    open = false
                    model.filter = MarketFilter.ALL
                    set(options.cleared())
                }, icon = "close", textColor = c.danger)
            }
        }
        val picked = group?.let { groups.getOrNull(it) }
        FmDropdownMenu(expanded = picked != null, onDismissRequest = { group = null }) {
            picked?.choices?.forEachIndexed { index, (title, chosen) ->
                FmDropdownMenuItem(title, onClick = { group = null; picked.pick(index) }, icon = if (chosen) "check" else null)
            }
        }
    }
}
