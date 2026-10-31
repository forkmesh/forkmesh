# Thread notifications

Part of [the App docs](README.md). An issue page's sidebar has a
Notifications box: a Subscribe/Unsubscribe button and one sentence saying
whether you will hear about the thread.

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
`notify_subscribers` skips muted rows, and `notify_pending_inbox` checks
`_thread_muted` for the hosting node and its owning account, so an owner who
unsubscribes stops hearing about that thread. The ping for a brand-new
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

The owner gets every inbox ping without a subscription row, so the status
must report `owner` rather than look only for a row. `issueSubscriptionNote`
in `06-repo-content.js` picks the sentence.

## What an inbox ping says

For an event without text (an approval with no note, closing a pull request,
resolving a review thread, closing, assigning or relabelling an issue),
`pending_inbox_action` names the event instead of a generic "activity" title:

- title: `Pull request approved in owner/repo`, `Pull request closed in …`,
  `Issue assigned in …`, `Issue closed in …`, and so on;
- body: the pull request's title (read with `pull_attention.pull_record`),
  then a sentence such as `alice approved pull request #3.`, then any note the
  person wrote;
- link: the issue or pull request itself whenever its number is known.

An unknown event type gets `There's new activity on issue #12.`. The pull request title is looked up
only for events without text, so a plain comment costs no extra read.

## Opening the comment

A mention or a followed-thread ping for a comment stores the comment's event
id with the number and source (`meta.comment`, at most 80 characters;
`notify_mentions` and `notify_subscribers` take it as `comment=`). The feed
and its detail panel (`homeFeedHref` in `05-repo-list-explorer.js`) link
`/<owner>/<repo>/{issues,pull,discussions}/<n>#comment-<id>`, and
`initCommentAnchor` in `09-comment-reactions.js` waits up to 20 seconds for
that comment's reaction bar, scrolls it into view and outlines it for four
seconds. Pings without a comment id still open the thread.
`app/tests/test_comment_notification_links.py` covers the links.
