package com.forkmesh.nativeapp.feature.docs

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
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
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
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
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.docs.DocsAuthor
import com.forkmesh.nativeapp.docs.DocsDependency
import com.forkmesh.nativeapp.docs.DocsLibrary
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmRowDivider
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmStatusBadge
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.rememberLinkOpener
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType

class DocsDependenciesScreen : Screen() {
    override val title: String get() = "Dependencies"

    private var query by mutableStateOf("")

    @Composable
    override fun Content(navigator: Navigator) {
        DocsFrame(navigator, title = { library -> library?.dependenciesArea?.label ?: "Dependencies" }) { library ->
            val groups = remember(library, query) { DocsLibrary.groupEcosystems(library.searchDependencies(query)) }
            LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(bottom = FmSpace.x6)) {
                item(key = "intro") {
                    val blurb = library.dependenciesArea?.blurb.orEmpty()
                    Text(
                        listOf(blurb, library.dependenciesSummary.replaceFirstChar { it.uppercase() } + ".").filter { it.isNotEmpty() }.joinToString(" "),
                        style = Fm.text.bodyMedium.copy(color = Fm.colors.textSecondary),
                        modifier = Modifier.padding(start = FmSpace.x4, end = FmSpace.x4, top = FmSpace.x3),
                    )
                }
                item(key = "search") { DocsSearchField(query, "Search dependencies or authors") { query = it } }
                if (groups.isEmpty()) {
                    item(key = "none") { DocsNotice("No dependency matches “${query.trim()}”.") }
                }
                for (group in groups) {
                    item(key = "group:${group.key}") {
                        FmSectionHeader(
                            group.label,
                            count = group.dependencies.size,
                            modifier = Modifier.padding(start = FmSpace.x4, end = FmSpace.x4, top = FmSpace.x5, bottom = FmSpace.x1),
                        )
                    }
                    items(group.dependencies, key = { "dependency:${it.id}" }) { dependency ->
                        FmListRow(
                            title = dependency.title,
                            subtitleContent = docsSubtitle(dependencySubtitle(dependency)),
                            onTap = { navigator.push(DocsDependencyScreen(dependency.id)) },
                        )
                        FmRowDivider(indent = FmSpace.x4)
                    }
                }
                snapshotFooter(library)
            }
        }
    }
}

private fun dependencySubtitle(dependency: DocsDependency): String =
    listOf(
        dependency.description.ifEmpty { if (dependency.title != dependency.name) dependency.name else "" },
        dependency.repositoryLabel,
    ).filter { it.isNotEmpty() }.joinToString(" · ")

private fun androidx.compose.foundation.lazy.LazyListScope.snapshotFooter(library: DocsLibrary) {
    if (library.dependenciesGenerated.isEmpty()) return
    item(key = "snapshot") {
        Text(
            "Snapshot of ${library.dependenciesGenerated}",
            style = Fm.text.bodySmall.copy(color = Fm.colors.textTertiary),
            modifier = Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x4),
        )
    }
}

class DocsDependencyScreen(private val id: String) : Screen() {
    override val title: String get() = DocsFeature.current?.dependency(id)?.title ?: "Dependency"

    @Composable
    override fun Content(navigator: Navigator) {
        val open = rememberLinkOpener()
        DocsFrame(navigator, title = { library -> library?.dependency(id)?.title ?: "Dependency" }) { library ->
            val dependency = library.dependency(id)
            if (dependency == null) {
                FmEmptyState("inventory_2", "Dependency not found", message = "The documentation library does not list $id.")
            } else {
                Detail(library, dependency, open)
            }
        }
    }

    @Composable
    private fun Detail(library: DocsLibrary, dependency: DocsDependency, open: (String) -> Unit) {
        LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(start = FmSpace.x4, end = FmSpace.x4, top = FmSpace.x4, bottom = FmSpace.x6)) {
            item(key = "header") { Header(dependency) }
            item(key = "facts") { Facts(dependency) }
            item(key = "github") {
                Section("GitHub")
                if (dependency.repository.isEmpty()) {
                    Text("No GitHub repository is known", style = Fm.text.bodyMedium.copy(color = Fm.colors.textSecondary))
                } else {
                    LinkRow("code", dependency.repositoryLabel) { open(dependency.repository) }
                }
            }
            if (dependency.usedBy.isNotEmpty()) {
                item(key = "used") {
                    Section("Used by")
                    for (path in dependency.usedBy) LinkRow("description", path, monospace = true) { open(library.sourceUrl(path)) }
                }
            }
            item(key = "authors-title") { Section("Top authors") }
            if (dependency.authors.isEmpty()) {
                item(key = "authors-none") {
                    Text(dependency.authorsFallback, style = Fm.text.bodyMedium.copy(color = Fm.colors.textSecondary))
                }
            } else {
                items(dependency.authors.withIndex().toList(), key = { "author:${it.value.login}" }) { (rank, author) ->
                    AuthorCard(rank + 1, author, open)
                }
            }
            if (library.dependenciesGenerated.isNotEmpty()) {
                item(key = "snapshot") {
                    Text(
                        "Snapshot of ${library.dependenciesGenerated}",
                        style = Fm.text.bodySmall.copy(color = Fm.colors.textTertiary),
                        modifier = Modifier.padding(top = FmSpace.x5),
                    )
                }
            }
        }
    }

    @Composable
    private fun Header(dependency: DocsDependency) {
        Column(Modifier.fillMaxWidth()) {
            Text(DocsLibrary.ecosystemLabel(dependency.ecosystem), style = Fm.text.bodySmall.copy(color = Fm.colors.textTertiary))
            Spacer(Modifier.height(FmSpace.x1))
            Text(dependency.title, style = Fm.text.headlineSmall.copy(fontWeight = FmType.emphasis, color = Fm.colors.textPrimary))
            if (dependency.title != dependency.name) {
                Spacer(Modifier.height(FmSpace.x1))
                Text(dependency.name, style = Fm.text.bodyMedium.copy(fontFamily = FmType.mono, color = Fm.colors.textSecondary))
            }
            if (dependency.description.isNotEmpty()) {
                Spacer(Modifier.height(FmSpace.x3))
                Text(dependency.description, style = Fm.text.bodyLarge.copy(color = Fm.colors.textPrimary))
            }
        }
    }

    @OptIn(ExperimentalLayoutApi::class)
    @Composable
    private fun Facts(dependency: DocsDependency) {
        FlowRow(
            Modifier.fillMaxWidth().padding(top = FmSpace.x3),
            horizontalArrangement = Arrangement.spacedBy(FmSpace.x2),
            verticalArrangement = Arrangement.spacedBy(FmSpace.x2),
        ) {
            FmStatusBadge("Version ${dependency.versionLabel}", Fm.colors.textSecondary)
            if (dependency.development) FmStatusBadge("Development only", Fm.colors.warning)
        }
    }

    @Composable
    private fun Section(title: String) {
        FmSectionHeader(title, modifier = Modifier.padding(top = FmSpace.x5, bottom = FmSpace.x2))
    }

    @Composable
    private fun LinkRow(icon: String, label: String, monospace: Boolean = false, onClick: () -> Unit) {
        Row(
            Modifier
                .fillMaxWidth()
                .clip(RoundedCornerShape(FmRadius.md))
                .clickable(onClick = onClick)
                .padding(vertical = FmSpace.x2),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            FmIcon(icon, size = 18.dp, tint = Fm.colors.textTertiary)
            Spacer(Modifier.width(FmSpace.x2))
            Text(
                label,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis,
                style = Fm.text.bodyMedium.copy(
                    color = Fm.colors.accent,
                    fontFamily = if (monospace) FmType.mono else null,
                ),
                modifier = Modifier.weight(1f),
            )
            FmIcon("open_in_new", size = 16.dp, tint = Fm.colors.textTertiary)
        }
    }

    @OptIn(ExperimentalLayoutApi::class)
    @Composable
    private fun AuthorCard(rank: Int, author: DocsAuthor, open: (String) -> Unit) {
        FmCard(Modifier.fillMaxWidth().padding(bottom = FmSpace.x2), radius = FmRadius.lg, color = Fm.colors.bgRaised) {
            Row(Modifier.fillMaxWidth()) {
                Text("$rank", style = Fm.text.titleMedium.copy(color = Fm.colors.textTertiary), modifier = Modifier.width(28.dp))
                Column(Modifier.weight(1f)) {
                    Text(author.name, style = Fm.text.titleSmall.copy(color = Fm.colors.textPrimary))
                    val details = listOf(author.handle, author.commitsLabel).filter { it.isNotEmpty() }.joinToString(" · ")
                    if (details.isNotEmpty()) {
                        Spacer(Modifier.height(2.dp))
                        Text(details, style = Fm.text.bodySmall.copy(color = Fm.colors.textSecondary))
                    }
                    FlowRow(Modifier.fillMaxWidth().padding(top = FmSpace.x1)) {
                        FmTextButton(onClick = { open(author.projects) }, contentPadding = PaddingValues(horizontal = FmSpace.x2)) { Text(author.projectsLabel) }
                        FmTextButton(onClick = { open(author.profile) }, contentPadding = PaddingValues(horizontal = FmSpace.x2)) { Text("Profile") }
                        if (author.website.isNotEmpty()) {
                            FmTextButton(onClick = { open(author.website) }, contentPadding = PaddingValues(horizontal = FmSpace.x2)) { Text("Website") }
                        }
                    }
                }
            }
        }
    }
}
