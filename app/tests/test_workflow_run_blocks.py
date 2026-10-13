"""Every `run: |` step in .forkmesh/*.yml must survive the desktop runner's
YAML block-scalar capture as one syntactically complete bash script.

desktop/src/ActionFile.cpp captures a block scalar by taking lines while they
are indented deeper than the parent key (captureBlockScalar). A line written
flush-left inside a step -- a heredoc body pasted at column 0, for example --
therefore ends the script early instead of failing the parse, and the runner
hands bash a truncated file. Release run #8337 (v0.8.4) died that way with
"unexpected EOF while looking for matching `)'" after a full desktop build.
"""

import re
import shutil
import subprocess
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[2]
WORKFLOWS = sorted((ROOT / ".forkmesh").glob("*.yml"))
RUN_KEY = re.compile(r"^(?P<indent> *)run: \|-?\s*$")
YAML_STRUCTURE = re.compile(r"^ *(#|- |[A-Za-z_][\w-]*:)")


def _indent(line: str) -> int:
    return len(line) - len(line.lstrip(" "))


def _run_blocks(path: Path):
    """Yield (line_no, script, next_line) the way the desktop runner sees them."""
    lines = path.read_text(encoding="utf-8").split("\n")
    pos = 0
    while pos < len(lines):
        match = RUN_KEY.match(lines[pos])
        if not match:
            pos += 1
            continue
        parent = len(match.group("indent"))
        start = pos + 1
        pos = start
        block_indent = None
        captured = []
        while pos < len(lines):
            line = lines[pos]
            if line.strip():
                if _indent(line) <= parent:
                    break
                if block_indent is None:
                    block_indent = _indent(line)
            strip = min(block_indent or 0, _indent(line))
            captured.append(line[strip:])
            pos += 1
        next_line = lines[pos] if pos < len(lines) else ""
        yield start + 1, "\n".join(captured) + "\n", next_line


def _cases():
    for workflow in WORKFLOWS:
        for line_no, script, next_line in _run_blocks(workflow):
            yield pytest.param(
                workflow, line_no, script, next_line,
                id=f"{workflow.name}:{line_no}",
            )


@pytest.mark.parametrize("workflow,line_no,script,next_line", list(_cases()))
def test_run_block_is_not_cut_short_by_a_flush_left_line(
    workflow, line_no, script, next_line
):
    # The capture may only stop at the next YAML key, list item, or comment.
    # Anything else at or below the key's indentation is script text the
    # runner drops.
    assert not next_line.strip() or YAML_STRUCTURE.match(next_line), (
        f"{workflow.relative_to(ROOT)}: the run block starting at line "
        f"{line_no} is cut short by a flush-left line: {next_line!r}"
    )


@pytest.mark.skipif(shutil.which("bash") is None, reason="bash not installed")
@pytest.mark.parametrize("workflow,line_no,script,next_line", list(_cases()))
def test_run_block_is_a_complete_bash_script(
    workflow, line_no, script, next_line, tmp_path
):
    path = tmp_path / "step.sh"
    path.write_text(script, encoding="utf-8")
    result = subprocess.run(
        ["bash", "-n", str(path)], capture_output=True, text=True, check=False
    )
    assert result.returncode == 0, (
        f"{workflow.relative_to(ROOT)}: the run block starting at line "
        f"{line_no} does not parse as bash:\n{result.stderr}"
    )
