# Workflows

A visual automation builder in the style of n8n. Connect a trigger (the Run button, a secret webhook URL or a schedule), logic steps (HTTP request, Set fields, If, Filter, Merge, Limit, Split out), the steps Apple's Shortcuts offers for automations (variables, choose by value, repeat, wait, run another workflow, stop; text, number, list and date actions; hashing and encoding), your own JavaScript in a sandboxed Run script step, and integrations on one canvas, run it, and read every step's output. Workflows download and upload as n8n-compatible JSON and import from n8n template links.

It works on the web, natively on the desktop, and natively on iPhone and Android. The desktop page (`content/desktop/`) shows a dashboard of stats, workflows and recent runs, edits workflows as a list of steps, manages credentials, imports and downloads n8n files, and runs workflows with desktop steps on the computer itself; the phones list, edit and run workflows on their own screens. A phone can run a workflow itself: the iPhone runs `content/mobile/workflow-engine.js` in Apple's JavaScriptCore, Android runs a Kotlin port of the same engine, and both are checked against the server's results. Phone runs use only credentials saved on that phone.

On the desktop it also runs the workflows your repositories keep in `.forkmesh/workflows/`: the special agents (each one is a workflow with a Special agent step that holds its role, instructions, skills, permissions and learning journal), shortcut runs and desktop steps. Every run shows each step and keeps a Markdown output file, and any of these workflows can be pinned as a button to the top, left, right or bottom rail of the window. This needs the `desktop.workflows.run` permission; see [docs/app/workflows.md](../../../docs/app/workflows.md#desktop-workflows-and-special-agents).

Workflows is a plugin like the Home tiles: off in projects and accounts created from 2026-10-14 (`install.installedBefore`), kept where it was already there. Its packs depend on it, so they come and go with it. See [core-plugins.md](../../../docs/marketplace/core-plugins.md).

Its nested packages each add one part and can be turned off on their own:

- `workflows-pack-communication`: Workflows: Communication
- `workflows-pack-email`: Workflows: Email
- `workflows-pack-productivity`: Workflows: Productivity
- `workflows-pack-forms-and-meetings`: Workflows: Forms and meetings
- `workflows-pack-developer-tools`: Workflows: Developer tools
- `workflows-pack-crm-and-support`: Workflows: CRM and support
- `workflows-pack-commerce-and-finance`: Workflows: Commerce and finance
- `workflows-pack-social-and-content`: Workflows: Social and content
- `workflows-pack-analytics`: Workflows: Analytics
- `workflows-pack-ai`: Workflows: AI
- `workflows-pack-data-and-storage`: Workflows: Data and storage
- `workflows-pack-utilities`: Workflows: Utilities
- `workflows-examples`: Workflows: Examples
- `workflows-repository-files`: Workflows: Repository Files

The package also ships files a repository can add from the Repository view: the Weaver special agent (`.forkmesh/teammates/workflow-builder.json`), the Build a workflow and Check ForkMesh files shortcuts, the Workflow design skill and the Check ForkMesh files action.

The host implementation stays in the App (`builtin:workflows`); see `docs/app/workflows.md`.
