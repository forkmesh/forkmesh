# Discord preview

Discord contributes a **Discord · <bot name>** connector button to the Chat
sidebar and places every server text channel, active thread, and bot direct
message the connected bot can reach in that sidebar, automatically grouped under
a collapsible **Discord · <server>** header. Messages, replies, reactions,
typing, edits, avatars, and attachments render in the ordinary Chat transcript.

The package requests only its declared capabilities: the bot-API connection it
makes as the token you provide, the sidebar/transcript contribution, and the
device-local token it keeps until the package is removed. Removing it stops the
client and clears the token, cached history, avatars, and sidebar group.

`market.png` is the required Market listing screenshot. It shows the grouped
sidebar the installed contribution produces; the live client remains the source
of truth for behavior, and the image is a bounded preview rather than
executable package content.
