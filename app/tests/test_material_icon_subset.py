import importlib.util
import io
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("subset_material_icons", ROOT / "tools" / "subset_material_icons.py")
tool = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(tool)


def test_every_native_icon_literal_is_in_the_subset():
    full = tool.read_table(tool.FULL_TABLE)
    shipped = tool.read_table(tool.SHIPPED_TABLE)
    used = tool.native_literal_names(full)
    assert len(used) > 500
    assert sorted(used - set(shipped)) == []


def test_shipped_table_is_a_slice_of_the_full_table():
    full = tool.read_table(tool.FULL_TABLE)
    shipped = tool.read_table(tool.SHIPPED_TABLE)
    assert len(full) > 8000
    assert shipped and all(full.get(name) == code for name, code in shipped.items())
    assert "help_outline_baseline" in shipped or "help_outline" in shipped


def test_ios_table_draws_from_the_subset():
    codes = set(tool.read_table(tool.SHIPPED_TABLE).values())
    assert sorted(tool.ios_codepoints() - codes) == []


def test_both_apps_ship_the_same_subset_font_covering_the_table():
    android, ios = (path.read_bytes() for path in tool.FONTS)
    assert android == ios
    assert len(android) < 300_000
    ttLib = pytest.importorskip("fontTools.ttLib")
    font = ttLib.TTFont(io.BytesIO(android))
    cmap = font.getBestCmap()
    assert sorted(name for name, code in tool.read_table(tool.SHIPPED_TABLE).items() if code not in cmap) == []
    assert font["name"].getDebugName(6) == "MaterialIcons-Regular"
