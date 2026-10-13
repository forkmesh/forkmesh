"""The repository header splits owner and name into separate links.

The owner segment opens /@owner, which serves organizations as well as
accounts, so clicking "forkmesh" in "forkmesh/forkmesh" reaches the org page.
"""

from pathlib import Path

_ROOT = Path(__file__).resolve().parents[1]
DASHBOARD_JS = (_ROOT / "public" / "dashboard.js").read_text(encoding="utf-8")
APP_RUNTIME = (_ROOT / "src" / "app_runtime.py").read_text(encoding="utf-8")


def _slice(text, start, end):
    begin = text.index(start)
    return text[begin:text.index(end, begin)]


def test_header_links_owner_to_profile_and_name_to_repository():
    header = _slice(
        DASHBOARD_JS, "function renderHeaderContext(", "// ---- Public-profile mode")
    assert 'repoDisplayKey(repo).split("/")' in header
    assert "publicProfileUrl(owner)" in header
    assert "data-repo-header-owner" in header
    assert "repoLinkUrl(repo)" in header


def test_public_profile_falls_back_to_organizations():
    loader = _slice(
        DASHBOARD_JS, "async function loadPublicProfile(", "function applyPublicProfileChrome(")
    assert '"/api/orgs/" + encodeURIComponent(name)' in loader
    assert "loadPublicOrganizationProfile(name, organization)" in loader
    assert 'kind: "organization"' in loader
    belongs = _slice(
        DASHBOARD_JS, "function repoBelongsToProfile(", "const PROFILE_CONTRIBUTION_DAY_MS")
    assert "repoPublicOwner(repo)" in belongs


def test_profile_page_serves_organizations_but_never_private_accounts():
    body = _slice(
        APP_RUNTIME, "async def _serve_profile_page", "async def _serve_repo_page")
    # A private or inactive account keeps its 404; only a name with no
    # account row falls through to the organization lookup.
    assert "if rec or not (await _org_row(self.env, name))[1]:" in body
    assert body.index("_org_row") < body.index("edge_cache_match")


def test_unclaimed_account_name_falls_back_to_organization_page():
    loader = _slice(
        DASHBOARD_JS, "async function loadPublicProfile(", "function applyPublicProfileChrome(")
    assert "body.exists === false" in loader
    assert loader.index("body.exists === false") < loader.index('"/api/orgs/"')
    org = _slice(
        DASHBOARD_JS, "function loadPublicOrganizationProfile(", "function applyPublicProfileChrome(")
    assert 'classList.add("organization-profile-mode")' in org
    assert "renderOrganizationProfileHeader(profile)" in org
    repos = _slice(
        DASHBOARD_JS, "function renderProfileRepositories(", "function renderProfileRepositoryCount(")
    assert "renderOrganizationRepositories();" in repos


def test_organization_page_markup_and_styles():
    root = _ROOT / "public" / "dashboard"
    overview = (root / "profile" / "index.html").read_text(encoding="utf-8")
    repositories = (root / "profile" / "repositories" / "index.html").read_text(encoding="utf-8")
    for page in (overview, repositories):
        assert "data-org-profile-header hidden" in page
        assert "body.organization-profile-mode [data-view=\"profile-repositories\"] aside + section" in page
    assert "data-org-profile-overview hidden" in overview
    assert "data-org-profile-popular" in overview
    assert "data-org-profile-languages" in overview
