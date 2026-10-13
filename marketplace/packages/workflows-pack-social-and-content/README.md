# Workflows: Social and content

An integration pack for the [Workflows](../workflows/README.md) package. It adds these 8 steps to the Add step menu:

- X (Twitter): Post on X.
- Mastodon: Post statuses on a Mastodon server.
- LinkedIn: Share posts on LinkedIn.
- Facebook Pages: Publish posts to a Facebook Page.
- YouTube: Search videos and read channel statistics.
- Reddit: Submit posts to a subreddit.
- WordPress: Create and list WordPress posts.
- Webflow: Create CMS collection items in Webflow.

It depends on Workflows. Installing it installs Workflows too; removing Workflows removes this pack from every device on your account. Removing only this pack hides its integrations from the Add step menu; workflows that already use them keep running. The pack carries no code: it lists its integrations in a `workflows.integrations` contribution and the Workflows page filters its menu by it. See `docs/app/workflows.md`.
