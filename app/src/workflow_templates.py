COLUMN = 288
ROW = 120


def S(node_id, node_type, label, column, row=0, /, **params):
    return {"id": node_id, "type": node_type, "name": label,
            "x": column * COLUMN, "y": row * ROW, "params": params,
            "credential": "", "continueOnFail": False, "disabled": False}


def C(source, target, output="main"):
    return {"from": source, "output": output, "to": target}


def T(key, name, description, tags, nodes, connections):
    return {"key": key, "name": name, "description": description,
            "tags": list(tags),
            "workflow": {"name": name, "nodes": list(nodes),
                         "connections": list(connections)}}


TEMPLATES = [
    T("github-bugs-to-slack", "GitHub bug reports to Slack",
      "A GitHub webhook calls this workflow for every issue event; new issues "
      "labelled bug are posted to a Slack channel.",
      ("GitHub", "Slack", "Webhook"), (
          S("t", "core.webhook", "GitHub issue event", 0),
          S("f", "core.filter", "Only opened issues", 1,
            left="{{ $json.body.action }}", operator="equals",
            right="opened"),
          S("i", "core.if", "Labelled bug?", 2,
            left="{{ $json.body.issue.labels }}", operator="contains",
            right='{"name": "bug"}'),
          S("s", "app.slack", "Post to #bugs", 3, -1,
            operation="postMessage", channel="#bugs",
            text="New bug: {{ $json.body.issue.title }} "
                 "{{ $json.body.issue.html_url }}"),
          S("n", "core.noop", "Ignore other issues", 3, 1),
      ), (C("t", "f"), C("f", "i"), C("i", "s", "true"),
          C("i", "n", "false"))),
    T("daily-weather-telegram", "Morning weather on Telegram",
      "Every morning reads the weather for a city and sends a short summary "
      "to a Telegram chat.",
      ("Schedule", "OpenWeatherMap", "Telegram"), (
          S("t", "core.schedule", "Every day", 0, every="1440"),
          S("w", "app.openweathermap", "Weather in Berlin", 1,
            operation="current", q="Berlin,DE", units="metric"),
          S("m", "app.telegram", "Send to Telegram", 2,
            operation="sendMessage", chat_id="123456789",
            text="Good morning! {{ $json.name }}: "
                 "{{ $json.weather[0].description }}, "
                 "{{ $json.main.temp }}°C"),
      ), (C("t", "w"), C("w", "m"))),
    T("summarize-with-claude", "Summarize text with Claude and post it",
      "Send any text to the webhook; Claude summarizes it and the summary is "
      "posted to a Discord channel.",
      ("Webhook", "AI", "Discord"), (
          S("t", "core.webhook", "Text arrives", 0),
          S("a", "app.anthropic", "Summarize", 1, operation="message",
            prompt="Summarize in three bullet points:\n\n{{ $json.body.text }}",
            maxTokens="1024"),
          S("d", "app.discordWebhook", "Post summary", 2, operation="send",
            content="{{ $json.text }}", username="Summary bot"),
      ), (C("t", "a"), C("a", "d"))),
    T("stripe-customers-to-hubspot", "Stripe customers to HubSpot",
      "Every hour copies the newest Stripe customers into HubSpot contacts.",
      ("Schedule", "Stripe", "HubSpot"), (
          S("t", "core.schedule", "Every hour", 0, every="60"),
          S("s", "app.stripe", "Newest customers", 1,
            operation="listCustomers", limit="10"),
          S("x", "core.split", "One item per customer", 2, field="data"),
          S("f", "core.filter", "Has an email", 3,
            left="{{ $json.email }}", operator="isNotEmpty"),
          S("h", "app.hubspot", "Create contact", 4,
            operation="createContact", email="{{ $json.email }}",
            firstName="{{ $json.name }}"),
      ), (C("t", "s"), C("s", "x"), C("x", "f"), C("f", "h"))),
    T("form-to-airtable-and-email", "Form submissions to Airtable with a "
      "confirmation email",
      "Point a website form at the webhook: each submission becomes an "
      "Airtable record and the sender gets a confirmation email.",
      ("Webhook", "Airtable", "SendGrid"), (
          S("t", "core.webhook", "Form submitted", 0),
          S("v", "core.set", "Clean up fields", 1,
            values='{"name": "{{ $json.body.name }}", '
                   '"email": "{{ $json.body.email }}", '
                   '"message": "{{ $json.body.message }}"}',
            keep="false"),
          S("a", "app.airtable", "Save to Airtable", 2, -1,
            operation="createRecord", baseId="appXXXXXXXXXXXXXX",
            table="Submissions",
            fields='{"Name": "{{ $json.name }}", '
                   '"Email": "{{ $json.email }}", '
                   '"Message": "{{ $json.message }}"}'),
          S("e", "app.sendgrid", "Confirmation email", 2, 1,
            operation="sendEmail", **{"from": "hello@example.com"},
            to="{{ $json.email }}", subject="We got your message",
            text="Hi {{ $json.name }}, thanks for writing. "
                 "We will answer soon."),
      ), (C("t", "v"), C("v", "a"), C("v", "e"))),
    T("uptime-alert", "Website uptime alert",
      "Every five minutes checks a URL; when the request fails it opens a "
      "PagerDuty incident and warns Slack.",
      ("Schedule", "HTTP", "PagerDuty", "Slack"), (
          S("t", "core.schedule", "Every 5 minutes", 0, every="5"),
          dict(S("h", "core.http", "Check the site", 1, method="GET",
                 url="https://example.com/health"), continueOnFail=True),
          S("i", "core.if", "Request failed?", 2,
            left="{{ $json.error }}", operator="isNotEmpty"),
          S("p", "app.pagerduty", "Open incident", 3, -1,
            operation="trigger", summary="example.com is down: "
                                         "{{ $json.error }}",
            source="forkmesh-uptime", severity="critical",
            dedupKey="example-com-down"),
          S("s", "app.slack", "Warn #ops", 3, 1, operation="postMessage",
            channel="#ops", text="example.com check failed: {{ $json.error }}"),
      ), (C("t", "h"), C("h", "i"), C("i", "p", "true"),
          C("i", "s", "true"))),
    T("translate-and-reply", "Translate messages with DeepL",
      "Translate text sent to the webhook into German and send the "
      "translation to Telegram.",
      ("Webhook", "DeepL", "Telegram"), (
          S("t", "core.webhook", "Text to translate", 0),
          S("d", "app.deepl", "Translate to German", 1,
            operation="translate", text="{{ $json.body.text }}",
            targetLang="DE"),
          S("m", "app.telegram", "Send translation", 2,
            operation="sendMessage", chat_id="{{ $trigger.body.chat_id }}",
            text="{{ $json.translations[0].text }}"),
      ), (C("t", "d"), C("d", "m"))),
    T("shopify-orders-to-sheets", "Shopify orders to Google Sheets",
      "Every hour appends the latest Shopify orders to a spreadsheet.",
      ("Schedule", "Shopify", "Google Sheets"), (
          S("t", "core.schedule", "Every hour", 0, every="60"),
          S("o", "app.shopify", "Latest orders", 1, operation="listOrders",
            status="any", limit="25"),
          S("x", "core.split", "One item per order", 2, field="orders"),
          S("g", "app.googleSheets", "Append a row", 3,
            operation="appendRow", spreadsheetId="SPREADSHEET_ID",
            range="Orders!A1",
            values='[["{{ $json.name }}", "{{ $json.email }}", '
                   '"{{ $json.total_price }}", "{{ $json.created_at }}"]]'),
      ), (C("t", "o"), C("o", "x"), C("x", "g"))),
    T("new-github-release-notes", "Draft release notes with AI",
      "Run it by hand: reads recently closed GitHub issues, asks OpenAI for "
      "release notes and creates a draft release.",
      ("Manual", "GitHub", "AI"), (
          S("t", "core.manual", "Run by hand", 0,
            payload='{"tag": "v1.0.0"}'),
          S("g", "app.github", "Closed issues", 1, operation="listIssues",
            owner="your-org", repo="your-repo", state="closed"),
          S("l", "core.limit", "Last 20", 2, max="20"),
          S("m", "core.set", "Titles only", 3,
            values='{"title": "{{ $json.title }}"}', keep="false"),
          S("o", "app.openai", "Write notes", 4, operation="chat",
            model="gpt-4.1-mini",
            prompt="Write short release notes for: {{ $json.title }}"),
          S("r", "app.github", "Create release", 5,
            operation="createRelease", owner="your-org", repo="your-repo",
            tag_name="{{ $trigger.tag }}", name="{{ $trigger.tag }}",
            body="{{ $json.text }}"),
          S("x", "core.limit", "One release", 4, 1, max="1"),
      ), (C("t", "g"), C("g", "l"), C("l", "m"), C("m", "x"),
          C("x", "o"), C("o", "r"))),
    T("calendly-to-notion", "Calendly bookings to Notion",
      "Every 30 minutes saves new Calendly meetings as pages in a Notion "
      "database and posts them to Microsoft Teams.",
      ("Schedule", "Calendly", "Notion", "Teams"), (
          S("t", "core.schedule", "Every 30 minutes", 0, every="30"),
          S("c", "app.calendly", "Scheduled events", 1,
            operation="listEvents",
            user="https://api.calendly.com/users/YOUR_USER",
            status="active"),
          S("x", "core.split", "One item per meeting", 2, field="collection"),
          S("n", "app.notion", "Add to Notion", 3, -1,
            operation="createPage", databaseId="DATABASE_ID",
            properties='{"Name": {"title": [{"text": {"content": '
                       '"{{ $json.name }}"}}]}, "When": {"date": '
                       '{"start": "{{ $json.start_time }}"}}}'),
          S("m", "app.teams", "Tell the team", 3, 1, operation="send",
            title="New meeting", text="{{ $json.name }} at "
                                     "{{ $json.start_time }}"),
      ), (C("t", "c"), C("c", "x"), C("x", "n"), C("x", "m"))),
]


def template_list():
    return [{"key": t["key"], "name": t["name"],
             "description": t["description"], "tags": t["tags"],
             "steps": len(t["workflow"]["nodes"])} for t in TEMPLATES]


def template(key):
    for item in TEMPLATES:
        if item["key"] == key:
            return item
    return None
