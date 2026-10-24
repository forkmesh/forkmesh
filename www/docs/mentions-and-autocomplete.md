# Mentions and username suggestions

You never have to type a ForkMesh username from memory. Wherever you mention
someone or enter a username, ForkMesh suggests matching accounts as you type,
and every name and `@mention` you see opens that person's profile.

## @mentions

Type `@` and the first letters of a name. A short list of matching people
appears at the cursor:

- **Chat** (the `/chat` page and the dashboard chat panel): people in the
  room and the chat directory first, then anyone on ForkMesh.
- **Issues, pull requests, discussions and releases**: people already on that
  issue or pull request first (author, commenters, assignees, reviewers), then
  the repository's contributors, then anyone on ForkMesh.

Use the arrow keys and Enter or Tab to pick a name, or tap it on a phone.
Escape closes the list.

Mentioning someone in an issue or pull request pings them and subscribes them
to the thread. They can press **Unsubscribe** on the issue to stop hearing
about it, and a later mention does not subscribe them again.

Text that looks like a mention is left alone inside code, inside links and
web addresses (`https://x.social/@bob`), and in email addresses.

## Username fields

On the web, these fields suggest accounts as you type:

| Where | Field |
| --- | --- |
| Chat, channel settings, Members | The invite field; people already in the channel are not suggested |
| Chat, New channel | The members list: separate names with commas, and each name you add is left out of the next suggestions |
| Repository settings, Collaborators | The collaborator name; current collaborators are not suggested |
| Security advisory | The credit and collaborator name |

Your own name is never suggested. Prefix matches come first, then names that
contain what you typed. When the only suggestion is exactly what you typed,
the list hides so Enter submits the form.

The desktop app's invite dialogs search people too, and DJ Studio suggests
names when you invite people to a jam ([DJ Studio jams](dj-studio.md)). The
phone apps have no channel invite field.

## Profile links

Every account name and every `@mention` links to that person's profile at
`/@name`: in issues, pull requests, discussions, notes, tasks, chat, commit
lists, notification emails and more. In the phone apps a tap opens the
profile inside the app. The desktop app opens its own profile card when it
knows the person, and the web profile otherwise.

Some names stay plain text on purpose:

- bots, agents and system names such as Actions, runners and mirror nodes;
- Discord senders and fediverse authors, whose display names could match the
  wrong ForkMesh account;
- commit authors whose Git name is not a ForkMesh account;
- private profiles, accounts that do not exist, and names inside a row that
  is already a link.

Related: [Hosted repositories](hosted-repositories.md) and
[ForkMesh apps](apps.md).
