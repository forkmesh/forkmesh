# Workflows: Examples

Ten example workflows for the [Workflows](../workflows/README.md) package, listed under Examples:

- GitHub bug reports to Slack: A GitHub webhook calls this workflow for every issue event; new issues labelled bug are posted to a Slack channel.
- Morning weather on Telegram: Every morning reads the weather for a city and sends a short summary to a Telegram chat.
- Summarize text with Claude and post it: Send any text to the webhook; Claude summarizes it and the summary is posted to a Discord channel.
- Stripe customers to HubSpot: Every hour copies the newest Stripe customers into HubSpot contacts.
- Form submissions to Airtable with a confirmation email: Point a website form at the webhook: each submission becomes an Airtable record and the sender gets a confirmation email.
- Website uptime alert: Every five minutes checks a URL; when the request fails it opens a PagerDuty incident and warns Slack.
- Translate messages with DeepL: Translate text sent to the webhook into German and send the translation to Telegram.
- Shopify orders to Google Sheets: Every hour appends the latest Shopify orders to a spreadsheet.
- Draft release notes with AI: Run it by hand: reads recently closed GitHub issues, asks OpenAI for release notes and creates a draft release.
- Calendly bookings to Notion: Every 30 minutes saves new Calendly meetings as pages in a Notion database and posts them to Microsoft Teams.

Each example opens as an unsaved copy you finish with your own credentials and IDs. Removing this pack hides the Examples list. The pack carries no code: it names its examples in a `workflows.templates` contribution. See `docs/app/workflows.md`.
