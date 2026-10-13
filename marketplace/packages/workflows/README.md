# Workflows

A visual automation builder in the style of n8n. Connect a trigger (the Run button, a secret webhook URL or a schedule), logic steps (HTTP request, Set fields, If, Filter, Merge, Limit, Split out) and integrations on one canvas, run it, and read every step's output. Workflows download and upload as n8n-compatible JSON and import from n8n template links.

It works on the web, on phones (the page is touch-friendly and the phone Market and Hub open it), and from the desktop rail.

Since 1.1.0 (2026-10-13) Workflows is a plugin like the Home tiles: off in projects and accounts created from 2026-10-14 (`install.installedBefore`), kept where it was already there. Its packs depend on it, so they come and go with it. See [core-plugins.md](../../../docs/marketplace/core-plugins.md).

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
