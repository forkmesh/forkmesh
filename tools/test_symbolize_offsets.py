import gzip
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "native" / "ios" / "tools"))

import symbolize_offsets  # noqa: E402

NM = """\
0000000100000000 T __mh_execute_header
0000000100004000 T _main
0000000100004100 t _$s8ForkMesh9HomeShellV4bodyQrvg
0000000100004400 t _$s8ForkMesh15ShellTokenTanksC8syncTiltyyF
0000000100008000 S _someData
0000000100008100 t ltmp0
"""


def symbols(tmp_path):
    path = tmp_path / "ForkMesh-native-1.5.0-22-symbols.txt.gz"
    with gzip.open(path, "wt") as handle:
        handle.write(NM)
    return path


def test_an_offset_is_named_by_the_nearest_text_symbol_below_it(tmp_path):
    lines = symbolize_offsets.describe(symbols(tmp_path), [0x4120, 0x4400, 0x4000], readable=False)
    assert lines == [
        "+0x4120  _$s8ForkMesh9HomeShellV4bodyQrvg + 0x20",
        "+0x4400  _$s8ForkMesh15ShellTokenTanksC8syncTiltyyF + 0x0",
        "+0x4000  _main + 0x0",
    ]


def test_data_symbols_are_skipped_and_an_offset_before_every_symbol_says_so(tmp_path):
    addresses, names = symbolize_offsets.read_symbols(symbols(tmp_path))
    assert "_someData" not in names
    assert symbolize_offsets.locate(addresses, names, 0x8050) == ("_$s8ForkMesh15ShellTokenTanksC8syncTiltyyF", 0x3c50)
    assert symbolize_offsets.describe(symbols(tmp_path), [0x10], base=0x100004000, readable=False)[0] == "+0x10  _main + 0x10"
    plain = tmp_path / "plain.txt"
    plain.write_text(NM)
    assert symbolize_offsets.describe(plain, [0x0], base=0xFFFF0000, readable=False) == ["+0x0  (before the first symbol)"]


def test_a_pasted_report_gives_every_app_frame_once():
    report = (
        "1x     AttributeGraph < SwiftUICore < SwiftUI < ForkMesh +0x2c240\n"
        "1x     SwiftUICore < libswiftObservation.dylib < ForkMesh +0x1f1a4f4 < ForkMesh +0x12dcc\n"
        "5 ForkMesh 0x104c9ed4d +0x16d4d\n"
        "again ForkMesh +0x2c240\n"
    )
    assert symbolize_offsets.offsets_from(["+0x10"], report) == [0x10, 0x2c240, 0x1f1a4f4, 0x12dcc]
