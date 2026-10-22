# Thread notifications

An issue page's sidebar has a Notifications box: a Subscribe/Unsubscribe
button and one sentence saying whether you will hear about the thread. The
sentence used to be wrong for the repository owner, and the pings it described
often said nothing about what happened.

## Who hears about a thread

| Who | How | Can mute it |
| --- | --- | --- |
| The repository owner (the hosting node, or the account that owns that node) | `notify_pending_inbox`: every event that lands in the relay inbox | Yes, except the ping for a new issue or pull request |
| Anyone who commented | `subscribe_thread` after their comment, then `notify_subscribers` | Yes |
| Anyone assigned to the issue | `notify_issue_assignees` pings them and subscribes them | Yes |
| Anyone @mentioned in an issue or pull request | `notify_mentions` pings them and subscribes them | Yes |
| Anyone who pressed Subscribe | `subscribe_handler` | Yes |

A mute (Unsubscribe) is a `thread_subscriptions` row with `muted` set. It is
sticky: a later comment, assignment or mention does not re-subscribe you.
`notify_subscribers` skips muted rows, and `notify_pending_inbox` now checks
`_thread_muted` for the hosting node and its owning account, so an owner who
unsubscribes really stops hearing about that thread. The ping for a brand-new
item is never muted: there is no thread to mute yet.

## What the sidebar says

`POST /api/repo/<owner>/<repo>/subscribe` with `action: "status"` answers
`subscribed` and a `reason`:

| `reason` | `subscribed` | Sentence |
| --- | --- | --- |
| `subscribed` | true | You're receiving notifications because you're subscribed to this thread. |
| `owner` | true | You're receiving notifications because you own this repository. |
| `muted` | false | You're not receiving notifications from this thread. |
| empty | false | You're not receiving notifications from this thread. |

Before, the status only looked for a subscription row. The owner gets every
inbox ping without one, so the page told them they were not receiving
notifications from a thread that had just pinged them. `issueSubscriptionNote`
in `06-repo-content.js` picks the sentence.

## What an inbox ping says

An event without text (an approval with no note, closing a pull request,
resolving a review thread, closing, assigning or relabelling an issue) used to
reach the owner as "Pull request activity for owner/repo" or "Issue submitted
for owner/repo" over "A signed item is waiting in your desktop inbox.", linked
to the repository root. `pending_inbox_action` now names the event:

- title: `Pull request approved in owner/repo`, `Pull request closed in …`,
  `Issue assigned in …`, `Issue closed in …`, and so on;
- body: the pull request's title (read with `pull_attention.pull_record`),
  then a sentence such as `alice approved pull request #3.`, then any note the
  person wrote;
- link: the issue or pull request itself whenever its number is known.

An event type it does not know still gets `There's new activity on issue #12.`
instead of the desktop inbox sentence. The pull request title is looked up
only for events without text, so a plain comment costs no extra read.
