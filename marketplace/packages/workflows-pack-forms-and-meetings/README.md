# Workflows: Forms and meetings

An integration pack for the [Workflows](../workflows/README.md) package. It adds these 4 steps to the Add step menu:

- Calendly: Read scheduled Calendly events.
- Typeform: Read Typeform forms and responses.
- Jotform: Read Jotform submissions.
- Zoom: Create and list Zoom meetings.

It depends on Workflows. Installing it installs Workflows too; removing Workflows removes this pack from every device on your account. Removing only this pack hides its integrations from the Add step menu; workflows that already use them keep running. The pack carries no code: it lists its integrations in a `workflows.integrations` contribution and the Workflows page filters its menu by it. See `docs/app/workflows.md`.
