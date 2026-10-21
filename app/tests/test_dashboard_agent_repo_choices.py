from pathlib import Path

JS = Path(__file__).resolve().parents[1] / "public" / "dashboard" / "js"


def test_agent_repo_choices_name_organization_repositories_once():
    # A repository an organization owns is listed once, under the organization,
    # not again under the account node behind it.
    source = (JS / "06-repo-content.js").read_text(encoding="utf-8")
    choices = source[source.index("function agentModalRepoChoices()"):
                     source.index("function agentModalPreferredRepo(")]
    assert choices.index("state.homeOrganizationRepositories") < choices.index(
        "groupRepositories(state.repositories")
    assert "groupOrganizationAlias(group)" in choices
    assert "${escapeHtml(agentModalRepoLabel(repo))}</option>" in source


def test_composer_repo_choices_use_the_same_label():
    boot = (JS / "10-boot.js").read_text(encoding="utf-8")
    assert "display: agentModalRepoLabel(repo)," in boot
