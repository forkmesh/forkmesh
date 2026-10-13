#!/usr/bin/env python3
import argparse
import shlex
import sys
from pathlib import Path

import yaml


def step_body(step):
    if isinstance(step, dict):
        return step.get("name", ""), step["script"]
    return "", step


def write_steps(config, workflow_name, out, skip=()):
    workflow = (config.get("workflows") or {}).get(workflow_name)
    if workflow is None:
        raise SystemExit("no workflow %r in codemagic.yaml" % workflow_name)
    out.mkdir(parents=True, exist_ok=True)
    rows = []
    for index, step in enumerate(workflow.get("scripts") or []):
        name, body = step_body(step)
        if name in skip:
            continue
        path = out / ("%02d.sh" % index)
        path.write_text(body if body.startswith("#!") else "#!/usr/bin/env bash\n" + body)
        path.chmod(0o755)
        rows.append("%s\t%s\n" % (path.name, name))
    (out / "index.tsv").write_text("".join(rows))
    variables = (workflow.get("environment") or {}).get("vars") or {}
    (out / "env.sh").write_text("".join(
        "export %s=%s\n" % (key, shlex.quote(str(value))) for key, value in variables.items()))
    return rows


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Write one codemagic.yaml workflow's scripts as numbered shell steps.")
    parser.add_argument("workflow")
    parser.add_argument("out", type=Path)
    parser.add_argument("--config", type=Path, default=Path("codemagic.yaml"))
    parser.add_argument("--skip", action="append", default=[],
                        help="step name to leave out (repeatable)")
    args = parser.parse_args(argv)
    rows = write_steps(yaml.safe_load(args.config.read_text()), args.workflow, args.out,
                       set(args.skip))
    sys.stdout.write("".join(rows))
    return 0


if __name__ == "__main__":
    sys.exit(main())
