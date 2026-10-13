# Workflows: Productivity

An integration pack for the [Workflows](../workflows/README.md) package. It adds these 13 steps to the Add step menu:

- Notion: Create pages and query databases in Notion.
- Airtable: Create, list and update Airtable records.
- Google Sheets: Append and read spreadsheet rows.
- Google Calendar: Create and list calendar events.
- Google Drive: List files and create folders in Google Drive.
- Gmail: Search and read Gmail messages.
- Microsoft 365: Send Outlook mail and add To Do tasks through Microsoft Graph.
- Todoist: Create, list and close Todoist tasks.
- Trello: Create cards and comments on Trello boards.
- Asana: Create and list Asana tasks.
- ClickUp: Create and list ClickUp tasks.
- monday.com: Create items and run GraphQL queries on monday.com boards.
- Baserow: Create and list rows in Baserow tables.

It depends on Workflows. Installing it installs Workflows too; removing Workflows removes this pack from every device on your account. Removing only this pack hides its integrations from the Add step menu; workflows that already use them keep running. The pack carries no code: it lists its integrations in a `workflows.integrations` contribution and the Workflows page filters its menu by it. See `docs/app/workflows.md`.
