package com.forkmesh.nativeapp.feature.docs

import android.content.Context
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.docs.DocsLibrary
import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.feature.tools.ToolsAppBar
import com.forkmesh.nativeapp.navigation.Route
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmIconTile
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmRowDivider
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmSkeletonList
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.fmSafeAreaInsets
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext

object DocsFeature {
    const val ASSET = "docs/library.json"

    private val mutex = Mutex()

    @Volatile
    private var cached: DocsLibrary? = null

    fun install() {
        NavigationFeature.register(Route.Kind.Docs, Route.Kind.Docs.owner) { DocsScreen() }
    }

    internal val current: DocsLibrary? get() = cached

    suspend fun library(context: Context): DocsLibrary? {
        cached?.let { return it }
        return mutex.withLock {
            cached ?: withContext(Dispatchers.IO) {
                runCatching {
                    context.applicationContext.assets.open(ASSET).use { String(it.readBytes(), Charsets.UTF_8) }
                }.getOrNull()?.let(DocsLibrary::parse)
            }?.also { cached = it }
        }
    }
}

internal sealed interface DocsLoad {
    data object Loading : DocsLoad
    data object Missing : DocsLoad
    data class Ready(val library: DocsLibrary) : DocsLoad
}

@Composable
internal fun rememberDocsLibrary(): DocsLoad {
    val context = LocalContext.current
    val initial: DocsLoad = DocsFeature.current?.let(DocsLoad::Ready) ?: DocsLoad.Loading
    val load by produceState(initial, context) {
        value = DocsFeature.library(context)?.let(DocsLoad::Ready) ?: DocsLoad.Missing
    }
    return load
}

@Composable
internal fun DocsFrame(
    navigator: Navigator,
    title: (DocsLibrary?) -> String,
    actions: @Composable RowScope.(DocsLibrary) -> Unit = {},
    content: @Composable (DocsLibrary) -> Unit,
) {
    val load = rememberDocsLibrary()
    val library = (load as? DocsLoad.Ready)?.library
    Column(Modifier.fillMaxSize().background(Fm.colors.canvas)) {
        ToolsAppBar(title(library), navigator) { if (library != null) actions(library) }
        Column(Modifier.fillMaxSize().windowInsetsPadding(fmSafeAreaInsets(top = false))) {
            when (load) {
                DocsLoad.Loading -> FmSkeletonList(
                    Modifier.fillMaxWidth().padding(FmSpace.x4),
                    rows = 6,
                    hasLeading = false,
                    label = "Loading the documentation",
                )
                DocsLoad.Missing -> FmEmptyState(
                    "library_books",
                    "Documentation unavailable",
                    message = "This build does not include the documentation library.",
                )
                is DocsLoad.Ready -> content(load.library)
            }
        }
    }
}

@Composable
internal fun DocsSearchField(query: String, hint: String, onChange: (String) -> Unit) {
    FmTextField(
        query,
        onChange,
        modifier = Modifier.fillMaxWidth().padding(horizontal = FmSpace.x4, vertical = FmSpace.x3),
        hint = hint,
        leading = { FmIcon("search", size = 20.dp, tint = Fm.colors.textTertiary) },
    )
}

@Composable
internal fun DocsSubtitle(text: String, lines: Int = 2) {
    Text(
        text,
        maxLines = lines,
        overflow = TextOverflow.Ellipsis,
        style = Fm.text.bodySmall.copy(color = Fm.colors.textTertiary, lineHeight = FmType.meta * FmType.leadingSnug),
    )
}

internal fun docsSubtitle(text: String): (@Composable () -> Unit)? = if (text.isEmpty()) null else ({ DocsSubtitle(text) })

@Composable
internal fun DocsNotice(text: String) {
    Text(
        text,
        style = Fm.text.bodyMedium.copy(color = Fm.colors.textSecondary),
        modifier = Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x4),
    )
}

@Composable
private fun DocsAreaHeader(label: String, blurb: String, count: Int?) {
    Column(Modifier.fillMaxWidth().padding(start = FmSpace.x4, end = FmSpace.x4, top = FmSpace.x5, bottom = FmSpace.x1)) {
        FmSectionHeader(label, count = count)
        if (blurb.isNotEmpty()) {
            Spacer(Modifier.height(FmSpace.x1))
            DocsSubtitle(blurb, lines = 3)
        }
    }
}

class DocsScreen : Screen() {
    override val title: String get() = "Docs"

    private var query by mutableStateOf("")

    @Composable
    override fun Content(navigator: Navigator) {
        DocsFrame(navigator, title = { "Docs" }) { library ->
            val trimmed = query.trim()
            val hits = remember(library, trimmed) { if (trimmed.isEmpty()) emptyList() else library.search(trimmed, 60) }
            val dependencies = remember(library, trimmed) { if (trimmed.isEmpty()) emptyList() else library.searchDependencies(trimmed) }
            LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(bottom = FmSpace.x6)) {
                item(key = "search") { DocsSearchField(query, "Search the documentation") { query = it } }
                if (trimmed.isEmpty()) {
                    for (area in library.documentAreas) {
                        val pages = library.pagesIn(area.key)
                        if (pages.isEmpty()) continue
                        item(key = "area:${area.key}") { DocsAreaHeader(area.label, area.blurb, pages.size) }
                        items(pages, key = { "page:${it.path}" }) { page ->
                            FmListRow(
                                title = page.title,
                                subtitleContent = docsSubtitle(page.description),
                                onTap = { navigator.push(DocsPageScreen(page.path)) },
                            )
                            FmRowDivider(indent = FmSpace.x4)
                        }
                    }
                    val area = library.dependenciesArea
                    if (area != null && library.dependencies.isNotEmpty()) {
                        item(key = "area:${area.key}") { DocsAreaHeader(area.label, area.blurb, null) }
                        item(key = "dependencies") {
                            FmListRow(
                                title = area.label,
                                subtitle = library.dependenciesSummary,
                                leading = { FmIconTile("inventory_2") },
                                onTap = { navigator.push(DocsDependenciesScreen()) },
                            )
                        }
                    }
                } else {
                    if (hits.isEmpty() && dependencies.isEmpty()) {
                        item(key = "none") { DocsNotice("Nothing in the documentation matches “$trimmed”.") }
                    }
                    if (hits.isNotEmpty()) {
                        item(key = "hits") { DocsAreaHeader("Pages", "", hits.size) }
                        items(hits, key = { "hit:${it.path}" }) { hit ->
                            FmListRow(
                                title = hit.title,
                                subtitleContent = docsSubtitle(hit.snippet),
                                onTap = { navigator.push(DocsPageScreen(hit.path)) },
                            )
                            FmRowDivider(indent = FmSpace.x4)
                        }
                    }
                    if (dependencies.isNotEmpty()) {
                        item(key = "dependency-hits") { DocsAreaHeader("Dependencies", "", dependencies.size) }
                        items(dependencies, key = { "dependency:${it.id}" }) { dependency ->
                            FmListRow(
                                title = dependency.title,
                                subtitleContent = {
                                    DocsSubtitle(
                                        listOf(DocsLibrary.ecosystemLabel(dependency.ecosystem), dependency.description)
                                            .filter { it.isNotEmpty() }.joinToString(" · "),
                                    )
                                },
                                onTap = { navigator.push(DocsDependencyScreen(dependency.id)) },
                            )
                            FmRowDivider(indent = FmSpace.x4)
                        }
                    }
                }
            }
        }
    }
}
