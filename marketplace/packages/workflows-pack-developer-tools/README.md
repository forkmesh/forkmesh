# Workflows: Developer tools

An integration pack for the [Workflows](../workflows/README.md) package. It adds these 13 steps to the Add step menu:

- GitHub: Issues, comments, releases and Actions on GitHub.
- GitLab: Issues and notes on GitLab projects.
- Bitbucket: Issues and pull requests on Bitbucket Cloud.
- Jira: Create, search and comment on Jira issues.
- Linear: Create Linear issues and run GraphQL queries.
- Sentry: List and resolve Sentry issues.
- PagerDuty: Trigger and resolve PagerDuty incidents.
- Opsgenie: Create and close Opsgenie alerts.
- CircleCI: Trigger CircleCI pipelines.
- Netlify: List sites and start Netlify builds.
- Vercel: List Vercel projects and deployments.
- Cloudflare: Purge cache and manage DNS records.
- DigitalOcean: List and reboot droplets.

It depends on Workflows. Installing it installs Workflows too; removing Workflows removes this pack from every device on your account. Removing only this pack hides its integrations from the Add step menu; workflows that already use them keep running. The pack carries no code: it lists its integrations in a `workflows.integrations` contribution and the Workflows page filters its menu by it. See `docs/app/workflows.md`.
