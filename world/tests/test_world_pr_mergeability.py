"""Evidence-based mergeability scores on the in-world pull-request board."""

from pathlib import Path
from world_scene_source import world_scene_source


ROOT = Path(__file__).resolve().parents[2] / "app"
SCENE = world_scene_source()
WORLD = (ROOT.parent / "world/public/world/world.js").read_text(encoding="utf-8")
PULL_REVIEW = (
    ROOT.parent / "world/public/world/world-pull-review.js"
).read_text(encoding="utf-8")


def test_pull_cards_show_a_bounded_evidence_based_mergeability_score():
    assert "function repositoryPullMergeabilityScore" in SCENE
    assert "Math.max(0, Math.min(100, Math.round(score)))" in SCENE
    assert 'factors.push("META+")' in SCENE
    assert 'factors.push("REFS+")' in SCENE
    assert 'factors.push("OIDS+")' in SCENE
    assert 'factors.push("CLEAN+")' in SCENE
    assert 'factors.push("REVIEW+")' in SCENE
    assert 'factors.push("CI+")' in SCENE
    assert "mergeability.factors.join" in SCENE
    assert "`${mergeability.score}%`" in SCENE


def test_missing_merge_evidence_is_unknown_instead_of_counted_as_passing():
    assert 'factors.push("CLEAN?")' in SCENE
    assert 'factors.push("REVIEW?")' in SCENE
    assert 'factors.push("CI?")' in SCENE
    assert "if (conflicts) score = Math.min(score, 24)" in SCENE
    assert "if (checksFailing) score = Math.min(score, 49)" in SCENE
    assert 'return { score: 0, label: "CLOSED"' in SCENE
    assert 'return { score: 100, label: "MERGED"' in SCENE


def test_immutable_refs_review_and_ci_evidence_reach_the_scene():
    assert "creationBaseOid: parsed.creationBaseOid" in WORLD
    assert "creationHeadOid: parsed.creationHeadOid" in WORLD
    assert "checksStatus: parsed.checksStatus" in WORLD
    assert "creationBaseOid: String(record?.creationBaseOid" in SCENE
    assert 'typeof record?.hasConflicts === "boolean"' in SCENE
    assert "record?.checksStatus" in SCENE
    assert "values.checksStatus" in PULL_REVIEW
    assert "values.ciStatus" in PULL_REVIEW
