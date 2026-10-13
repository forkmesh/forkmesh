# Workflows: Utilities

An integration pack for the [Workflows](../workflows/README.md) package. It adds these 5 steps to the Add step menu:

- Bitly: Shorten links with Bitly.
- OpenWeatherMap: Current weather and forecasts.
- Wikipedia: Read article summaries from Wikipedia.
- Home Assistant: Call services and read states in Home Assistant.
- Zapier: Send data to a Zapier catch hook.

It depends on Workflows. Installing it installs Workflows too; removing Workflows removes this pack from every device on your account. Removing only this pack hides its integrations from the Add step menu; workflows that already use them keep running. The pack carries no code: it lists its integrations in a `workflows.integrations` contribution and the Workflows page filters its menu by it. See `docs/app/workflows.md`.
