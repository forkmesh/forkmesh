# Workflows: CRM and support

An integration pack for the [Workflows](../workflows/README.md) package. It adds these 9 steps to the Add step menu:

- HubSpot: Create and search HubSpot contacts and deals.
- Salesforce: Create records and run SOQL queries.
- Pipedrive: Create deals and people in Pipedrive.
- Zoho CRM: Create leads in Zoho CRM.
- Freshdesk: Create and list support tickets.
- Zendesk: Create and list Zendesk tickets.
- Intercom: Create Intercom contacts and notes.
- Help Scout: List Help Scout conversations.
- Attio: Create records in Attio.

It depends on Workflows. Installing it installs Workflows too; removing Workflows removes this pack from every device on your account. Removing only this pack hides its integrations from the Add step menu; workflows that already use them keep running. The pack carries no code: it lists its integrations in a `workflows.integrations` contribution and the Workflows page filters its menu by it. See `docs/app/workflows.md`.
