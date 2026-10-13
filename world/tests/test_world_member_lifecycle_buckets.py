#!/usr/bin/env python3
"""Coarse activity recency supports the World member lifecycle rings."""

import ast
import json
from pathlib import Path
import subprocess
from world_scene_source import world_scene_source


ROOT = Path(__file__).resolve().parents[2]
ENTRY_PATH = ROOT / "app" / "src" / "entry.py"
# The World route bodies moved into a sibling module so Cloudflare's Python
# startup validation never parses them; the activity ladder moved with them,
# so the contract is read across both sources.
WORLD_ROUTES_PATH = ROOT / "world" / "src" / "world_routes.py"
ENTRY = (
    ENTRY_PATH.read_text(encoding="utf-8")
    + "\n\n\n"
    + WORLD_ROUTES_PATH.read_text(encoding="utf-8")
)
SCENE_PATH = ROOT / "world" / "public" / "world" / "world-scene.js"


def _load_entry_binding(name, namespace=None):
    tree = ast.parse(ENTRY, filename=str(ENTRY_PATH))
    node = next(
        item for item in tree.body
        if isinstance(item, (ast.Assign, ast.AnnAssign))
        and any(
            isinstance(target, ast.Name) and target.id == name
            for target in (
                item.targets if isinstance(item, ast.Assign)
                else [item.target]
            )
        )
    )
    scope = dict(namespace or {})
    exec(compile(ast.fix_missing_locations(
        ast.Module(body=[node], type_ignores=[])),
        str(ENTRY_PATH), "exec"), scope)
    return scope[name]


def _load_entry_function(name, namespace):
    tree = ast.parse(ENTRY, filename=str(ENTRY_PATH))
    node = next(
        item for item in tree.body
        if isinstance(item, ast.FunctionDef) and item.name == name
    )
    scope = dict(namespace)
    exec(compile(ast.fix_missing_locations(
        ast.Module(body=[node], type_ignores=[])),
        str(ENTRY_PATH), "exec"), scope)
    return scope[name]


def test_activity_ladder_keeps_lifecycle_boundaries_coarse():
    buckets = _load_entry_binding("WORLD_ACTIVITY_LIGHT_BUCKETS")
    classify = _load_entry_function(
        "_world_activity_light_bucket",
        {"WORLD_ACTIVITY_LIGHT_BUCKETS": buckets},
    )
    day = 24 * 60 * 60 * 1000
    assert classify(9 * day) == "10d"
    assert classify(10 * day) == "30d"
    assert classify(29 * day) == "30d"
    assert classify(30 * day) == "60d"
    assert classify(59 * day) == "60d"
    assert classify(60 * day) == "120d"
    assert classify(119 * day) == "120d"
    assert classify(120 * day) == "stale"


def test_activity_ladder_never_publishes_a_timestamp():
    buckets = _load_entry_binding("WORLD_ACTIVITY_LIGHT_BUCKETS")
    labels = [label for _limit, label in buckets]
    assert labels[-3:] == ["30d", "60d", "120d"]
    assert all(label.endswith(("h", "d")) or label == "hour" for label in labels)


def test_member_engagement_profiles_use_public_metrics_and_rank_warm_to_cold():
    script = """
      import {
        MEMBER_ENGAGEMENT_STATES,
        memberEngagementProfile,
        rankedMembersByEngagement,
      } from __SCENE__;
      const now = 1_800_000_000_000;
      const day = 24 * 60 * 60 * 1000;
      const old = now - 365 * day;
      const wallet = "11111111111111111111111111111111";
      const profile = (fields) => memberEngagementProfile({
        createdAt: old,
        emailVerified: true,
        ...fields,
      }, now);
      const baseline = profile({
        activityBucket: "3d",
        totalActiveMs: 60 * 60 * 1000,
      });
      const complete = profile({
        activityBucket: "3d",
        totalActiveMs: 60 * 60 * 1000,
        totalSteps: 500,
        visitCount: 12,
        officeVisits: 6,
        issues: 3,
        pulls: 2,
        commits: 18,
        discussions: 4,
        nodes: ["node-a", "node-b"],
        emailVerified: true,
        solana: wallet,
      });
      const states = [
        memberEngagementProfile({
          createdAt: now - 8 * day,
          activityBucket: "stale",
          totalSteps: 1,
        }, now).id,
        profile({
          activityBucket: "hour",
          totalActiveMs: 50 * 60 * 60 * 1000,
          visitCount: 40,
          commits: 32,
          nodes: ["node-a"],
          emailVerified: true,
          solana: wallet,
        }).id,
        baseline.id,
        memberEngagementProfile({
          createdAt: now - 2 * day,
          activityBucket: "stale",
        }, now).id,
        profile({ activityBucket: "30d", totalActiveMs: 7200000 }).id,
        profile({ activityBucket: "60d", totalActiveMs: 7200000 }).id,
        profile({ activityBucket: "120d", totalActiveMs: 7200000 }).id,
        profile({ activityBucket: "stale" }).id,
        profile({ activityBucket: "stale", totalActiveMs: 7200000 }).id,
        memberEngagementProfile({
          createdAt: now - 8 * day,
          activityBucket: "hour",
        }, now).id,
      ];
      const participation = {
        steps: profile({ activityBucket: "stale", totalSteps: 1 }).id,
        office: profile({ activityBucket: "stale", officeVisits: 1 }).id,
        contribution: profile({ activityBucket: "stale", issues: 1 }).id,
      };
      const ranked = rankedMembersByEngagement([
        { name: "cold", createdAt: old, activityBucket: "stale",
          totalActiveMs: 7200000, emailVerified: true },
        { name: "warm", createdAt: old, activityBucket: "hour",
          totalActiveMs: 50 * 60 * 60 * 1000, totalSteps: 125000,
          visitCount: 40, commits: 32, emailVerified: true, solana: wallet },
        { name: "middle", createdAt: old, activityBucket: "30d",
          totalActiveMs: 7200000, emailVerified: true },
        { name: "pending", createdAt: old, activityBucket: "hour",
          totalActiveMs: 50 * 60 * 60 * 1000 },
      ], now);
      process.stdout.write(JSON.stringify({
        expected: MEMBER_ENGAGEMENT_STATES.map((state) => state.id),
        states,
        participation,
        ranked: ranked.map((member) => member.name),
        baselineScore: baseline.score,
        completeScore: complete.score,
        signals: {
          contributions: complete.contributions,
          officeVisits: complete.officeVisits,
          nodeCount: complete.nodeCount,
          emailVerified: complete.emailVerified,
          hasSolana: complete.hasSolana,
        },
      }));
    """.replace("__SCENE__", json.dumps(SCENE_PATH.as_uri()))
    result = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        check=True,
        text=True,
        capture_output=True,
    )
    outcome = json.loads(result.stdout)
    assert outcome["states"] == outcome["expected"] == [
        "world-step",
        "power",
        "engaged",
        "new",
        "casual",
        "cooling",
        "dormant",
        "never-activated",
        "churned",
        "unverified",
    ]
    assert outcome["participation"] == {
        "steps": "world-step",
        "office": "casual",
        "contribution": "casual",
    }
    assert outcome["ranked"] == ["warm", "middle", "cold", "pending"]
    assert outcome["completeScore"] > outcome["baselineScore"]
    assert outcome["signals"] == {
        "contributions": 27,
        "officeVisits": 6,
        "nodeCount": 2,
        "emailVerified": True,
        "hasSolana": True,
    }


def test_world_step_ring_has_extra_space_before_the_remaining_lifecycle_rows():
    source = world_scene_source()
    assert "const MEMBER_CIRCLE_STATE_GAP = 0.35;" in source
    assert "const MEMBER_CIRCLE_INNER_STATE_GAP = 0.85;" in source
    layout = source.split("function buildMemberCircleLayout", 1)[1].split(
        "campfireGround.scale.setScalar", 1)[0]
    assert 'state.id === "world-step" || state.id === "power"' in layout
    assert "? MEMBER_CIRCLE_INNER_STATE_GAP" in layout
    assert ": MEMBER_CIRCLE_STATE_GAP" in layout
