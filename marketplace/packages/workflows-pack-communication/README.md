# Workflows: Communication

An integration pack for the [Workflows](../workflows/README.md) package. It adds these 12 steps to the Add step menu:

- Slack: Post messages and manage channels in a Slack workspace.
- Discord: Send messages as a Discord bot.
- Discord Webhook: Post to a Discord channel through an incoming webhook.
- Microsoft Teams: Post to a Teams channel through an incoming webhook.
- Telegram: Send messages and photos with a Telegram bot.
- Mattermost: Post to Mattermost through an incoming webhook.
- Google Chat: Post to a Google Chat space through a webhook.
- Pushover: Send push notifications to phones and desktops.
- ntfy: Publish push notifications to an ntfy topic.
- Matrix: Send messages to a Matrix room.
- Twilio: Send SMS and WhatsApp messages and start calls.
- WhatsApp Business: Send WhatsApp messages through the Cloud API.

It depends on Workflows. Installing it installs Workflows too; removing Workflows removes this pack from every device on your account. Removing only this pack hides its integrations from the Add step menu; workflows that already use them keep running. The pack carries no code: it lists its integrations in a `workflows.integrations` contribution and the Workflows page filters its menu by it. See `docs/app/workflows.md`.
