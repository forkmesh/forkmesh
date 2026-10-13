@file:OptIn(ExperimentalLayoutApi::class)

package com.forkmesh.nativeapp.feature.code.collab

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.MenuDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.SideEffect
import androidx.compose.runtime.Stable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.collab.IssueCard
import com.forkmesh.nativeapp.collab.IssueFilterField
import com.forkmesh.nativeapp.collab.IssueFilterOption
import com.forkmesh.nativeapp.collab.IssueFilters
import com.forkmesh.nativeapp.collab.IssueStatusFilter
import com.forkmesh.nativeapp.collab.issueFilterOptions
import com.forkmesh.nativeapp.collab.issueFilterSelection
import com.forkmesh.nativeapp.collab.issueMatchesFilters
import com.forkmesh.nativeapp.collab.normalizeIssueFilters
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.model.Issue
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmTooltip
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmMaterialTypography
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import java.util.WeakHashMap

@Stable
class IssueFilterState {
    var filters: IssueFilters by mutableStateOf(IssueFilters())

    internal companion object {
        private val byHost = WeakHashMap<Screen, HashMap<String, IssueFilterState>>()

        fun forHost(host: Screen, repo: Repository): IssueFilterState = synchronized(byHost) {
            byHost.getOrPut(host) { HashMap() }.getOrPut(repo.fullName) { IssueFilterState() }
        }
    }
}

@Composable
internal fun IssuesTabContent(repo: Repository, navigator: Navigator, filterState: IssueFilterState) {
    val account = rememberSession()?.nodeName?.trim() ?: ""
    val state = rememberLoad(repo.owner, repo.name) {
        Services.require<ApiService>().publishedIssues(repo.owner, repo.name)
    }
    when (state) {
        Loadable.Loading -> MessageList { LoadingCard("Loading…") }
        is Loadable.Failed -> MessageList { EmptyCard(state.message) }
        is Loadable.Ready -> {
            val issues = state.value
            if (issues.isEmpty()) {
                MessageList { EmptyCard("No published issues yet.") }
            } else {
                IssueList(repo, navigator, issues, account, filterState)
            }
        }
    }
}

@Composable
private fun IssueList(
    repo: Repository,
    navigator: Navigator,
    issues: List<Issue>,
    account: String,
    filterState: IssueFilterState,
) {
    val filters = normalizeIssueFilters(filterState.filters, issues, account)
    SideEffect {
        if (filterState.filters != filters) filterState.filters = filters
    }
    val visible = issues.filter { issueMatchesFilters(it, filters, account) }
    val colors = Fm.colors
    Column(Modifier.fillMaxSize()) {
        IssueFilterBar(
            issues = issues,
            filters = filters,
            account = account,
            visibleCount = visible.size,
            onChanged = { filterState.filters = it },
        )
        if (visible.isEmpty()) {
            MessageList(Modifier.weight(1f)) {
                EmptyCard("No issues match these filters. Clear them to see the whole list again.")
            }
        } else {
            LazyColumn(
                Modifier
                    .weight(1f)
                    .fillMaxWidth(),
                contentPadding = PaddingValues(top = FmSpace.x3, bottom = 88.dp),
            ) {
                items(visible) { issue ->
                    MobileCard(
                        icon = IssueCard.icon(issue),
                        iconColor = if (issue.isOpen) colors.success else colors.accentEdge,
                        title = issue.title,
                        subtitle = IssueCard.subtitle(issue),
                        badge = issue.status,
                        onTap = { navigator.push(IssueDetailScreen(repo, issue.number, issue)) },
                    )
                }
                item { NavigationBarSpacer() }
            }
        }
    }
}

@Composable
private fun IssueFilterBar(
    issues: List<Issue>,
    filters: IssueFilters,
    account: String,
    visibleCount: Int,
    onChanged: (IssueFilters) -> Unit,
) {
    val colors = Fm.colors
    Column(
        Modifier
            .fillMaxWidth()
            .background(colors.bgBase)
            .drawBehind {
                val rule = 1.dp.toPx()
                drawLine(colors.border, Offset(0f, size.height - rule / 2), Offset(size.width, size.height - rule / 2), rule)
            }
            .padding(horizontal = FmSpace.x4, vertical = FmSpace.x3),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            for (option in IssueStatusFilter.entries) {
                IssueStatusChip(option, selected = option == filters.status) { onChanged(filters.withStatus(option)) }
                Spacer(Modifier.width(FmSpace.x2))
            }
            Text(
                "$visibleCount of ${issues.size}",
                modifier = Modifier.weight(1f),
                textAlign = TextAlign.End,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                style = Fm.text.bodyMedium.copy(
                    color = colors.textTertiary,
                    fontSize = FmType.meta,
                    fontWeight = FmType.emphasis,
                ).withHeight(FmType.leadingSnug),
            )
            if (filters.hasMetadataFilter) {
                FmIconButton(
                    icon = "filter_alt_off_outlined",
                    label = "Clear issue filters",
                    onPressed = { onChanged(filters.clearMetadata()) },
                    modifier = Modifier.testTag("issue-filter-clear"),
                    color = colors.textSecondary,
                    iconSize = 18.dp,
                )
            }
        }
        Spacer(Modifier.height(FmSpace.x2))
        FlowRow(
            horizontalArrangement = Arrangement.spacedBy(FmSpace.x2),
            verticalArrangement = Arrangement.spacedBy(FmSpace.x2),
        ) {
            for (field in IssueFilterField.entries) {
                IssueFilterPill(
                    field = field,
                    options = issueFilterOptions(field, issues, account),
                    value = filters.valueOf(field),
                    onSelected = { onChanged(filters.select(field, it)) },
                )
            }
        }
    }
}

@Composable
private fun IssueStatusChip(option: IssueStatusFilter, selected: Boolean, onSelected: () -> Unit) {
    val colors = Fm.colors
    val shape = RoundedCornerShape(FmRadius.md)
    Box(
        Modifier
            .testTag("issue-status-${option.key}")
            .height(28.dp)
            .clip(shape)
            .background(if (selected) colors.accentSubtle else colors.bgRaised)
            .border(1.dp, if (selected) colors.accent else colors.border, shape)
            .selectable(selected = selected, role = Role.RadioButton, onClick = onSelected)
            .padding(horizontal = FmSpace.x3),
        contentAlignment = Alignment.Center,
    ) {
        Text(
            option.label,
            style = Fm.text.bodyMedium.copy(
                color = if (selected) colors.accent else colors.textSecondary,
                fontSize = FmType.meta,
                fontWeight = FmType.emphasis,
            ).withHeight(FmType.leadingSnug),
        )
    }
}

@Composable
private fun IssueFilterPill(
    field: IssueFilterField,
    options: List<IssueFilterOption>,
    value: String,
    onSelected: (String) -> Unit,
) {
    val colors = Fm.colors
    var expanded by remember { mutableStateOf(false) }
    val current = issueFilterSelection(options, value)
    val active = current.value.isNotEmpty()
    val shape = RoundedCornerShape(FmRadius.full)
    Box {
        FmTooltip("Filter by ${field.title.lowercase()}") {
            Row(
                Modifier
                    .testTag("issue-filter-${field.key}")
                    .height(34.dp)
                    .widthIn(max = 190.dp)
                    .clip(shape)
                    .background(if (active) colors.accentSubtle else colors.bgRaised)
                    .border(1.dp, if (active) colors.accent else colors.border, shape)
                    .clickable(enabled = options.size > 1, role = Role.Button) { expanded = true }
                    .padding(horizontal = FmSpace.x3),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(
                    if (active) current.label else field.title,
                    modifier = Modifier.weight(1f, fill = false),
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    style = Fm.text.bodyMedium.copy(
                        color = if (active) colors.accent else colors.textSecondary,
                        fontSize = FmType.meta,
                        fontWeight = FmType.emphasis,
                    ).withHeight(FmType.leadingSnug),
                )
                Spacer(Modifier.width(FmSpace.x1))
                FmIcon(
                    "expand_more",
                    size = 16.dp,
                    tint = if (active) colors.accent else colors.textTertiary,
                )
            }
        }
        FmDropdownMenu(expanded = expanded, onDismissRequest = { expanded = false }) {
            for (option in options) {
                val selected = option.value == current.value
                DropdownMenuItem(
                    text = {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            FmIcon(
                                if (selected) "check_circle" else "circle_outlined",
                                size = 16.dp,
                                tint = if (selected) colors.accent else colors.textTertiary,
                            )
                            Spacer(Modifier.width(FmSpace.x2))
                            Text(
                                option.label,
                                modifier = Modifier.weight(1f, fill = false),
                                maxLines = 1,
                                overflow = TextOverflow.Ellipsis,
                                style = FmMaterialTypography.bodyMedium.copy(fontWeight = FmType.medium),
                            )
                        }
                    },
                    onClick = {
                        expanded = false
                        onSelected(option.value)
                    },
                    colors = MenuDefaults.itemColors(textColor = colors.textPrimary),
                )
            }
        }
    }
}
