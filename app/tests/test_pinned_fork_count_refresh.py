from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
NETWORK = (ROOT / "app/public/dashboard/js/08-repo-detail-network.js").read_text(encoding="utf-8")


def _function(name):
    start = NETWORK.index(f"  async function {name}(")
    return NETWORK[start:NETWORK.index("\n  }\n", start)]


def test_background_catalog_refresh_checks_pinned_fork_counts_once():
    refresh = _function("refreshCatalogOnNewForks")
    assert "if (!fresh) void refreshCatalogOnNewForks();" in _function("refreshRepositoryCatalog")
    assert "state.pinnedForkCountsChecked = true" in refresh
    assert "sidebarPinnedGroups(nestRepositoryForks(homeRepositoryEntries()" in refresh and "() => []" in refresh
    assert "${repoApiBase(repo)}/forks" in refresh
    assert "live > group.forks.length && live > seen" in refresh
    assert "loadRepositories({ fresh: true })" in refresh
    assert "repo.isPrivate" in refresh
