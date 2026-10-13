# Workflows: Data and storage

An integration pack for the [Workflows](../workflows/README.md) package. It adds these 6 steps to the Add step menu:

- Dropbox: List and create Dropbox folders.
- Box: List and create Box folders.
- Supabase: Insert and select rows through the Supabase REST API.
- Firebase Realtime Database: Read and write Firebase Realtime Database paths.
- Algolia: Search and index records in Algolia.
- Upstash Redis: Run Redis commands over the Upstash REST API.

It depends on Workflows. Installing it installs Workflows too; removing Workflows removes this pack from every device on your account. Removing only this pack hides its integrations from the Add step menu; workflows that already use them keep running. The pack carries no code: it lists its integrations in a `workflows.integrations` contribution and the Workflows page filters its menu by it. See `docs/app/workflows.md`.
