const { test, expect } = require("@playwright/test");
const path = require("node:path");

const PUBLIC = path.resolve(__dirname, "..", "..", "public");
const REPO_PAGE_PATH = path.join(PUBLIC, "dashboard", "repo.html");
const DASHBOARD_JS_PATH = path.join(PUBLIC, "dashboard.js");
const FORUM_JS_PATH = path.join(PUBLIC, "dashboard-page-repo-forum.js");
const DASHBOARD_CSS_PATH = path.join(PUBLIC, "dashboard", "tailwind.css");
const LUCIDE_PATH = path.resolve(__dirname, "..", "..", "..", "www", "public", "assets", "vendor", "lucide-0.468.0.min.js");
const SHOTS = process.env.FORKMESH_FORUM_SHOTS || "";
const ORIGIN = "https://forum.example.org";
const UPLOAD = "0123456789abcdef0123456789abcdef.png";
const PNG = Buffer.from(
  "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg==",
  "base64",
);
const NOW = Date.now();
const ago = (hours) => new Date(NOW - hours * 3600 * 1000).toISOString();

const REPOSITORY = {
  owner: "alice",
  name: "proj",
  visibility: "public",
  source: "local-node",
  cloneUrl: "https://forkmesh.test/alice/proj",
  rootCommit: "root",
  updatedAt: "2",
  liveHost: true,
  cloneOnline: true,
  defaultBranch: "main",
};

function row(id, title, categoryId, extra = {}) {
  return {
    id, title, slug: title.toLowerCase().replace(/[^a-z0-9]+/g, "-"), category_id: categoryId, tags: [],
    created_at: ago(400), bumped_at: ago(id), last_posted_at: ago(id), posts_count: 3, reply_count: 2,
    views: 100 * id, like_count: 4, pinned: false, pinned_globally: false, closed: false, archived: false,
    visible: true, excerpt: "", last_poster_username: "bob", origin: "discourse",
    posters: [
      { username: "alice", name: "Alice", description: "Original Poster" },
      { username: "bob", name: "Bob", description: "Most Recent Poster" },
    ],
    ...extra,
  };
}

const ROWS = {
  10: row(10, "Read this first", 1, { pinned: true, pinned_globally: true, closed: true, excerpt: "The rules of the forum." }),
  11: row(11, "Blocks related to the protests", 2, { tags: ["dpi"], views: 3050 }),
  12: row(12, "Looking for VPN measurements", 2),
  13: row(13, "Release notes", 3),
  14: row(14, "Older thread", 2),
};

const HOSTILE = [
  "<p>Hello <a class=\"mention\" href=\"/u/bob\">@bob</a>, see <a href=\"" + ORIGIN + "/t/read-this-first/10/2\">the rules</a>.</p>",
  "<p><img src=\"/uploads/default/original/1X/shot.png\" alt=\"screenshot\" width=\"40\" height=\"20\" onerror=\"window.__pwned=1\"></p>",
  "<p><img src=\"https://other.example/remote.png\" srcset=\"https://other.example/remote2.png 2x\" alt=\"remote\"></p>",
  "<script>window.__pwned = 2</script><iframe src=\"https://other.example/frame\"></iframe><svg onload=\"window.__pwned=3\"></svg>",
  "<p onclick=\"window.__pwned=4\" style=\"background:url(https://other.example/bg.png)\">Styled</p>",
  "<p><a href=\"javascript:window.__pwned=5\">bad link</a> <a href=\"https://example.org/page\">good link</a></p>",
  "<p><img class=\"emoji\" src=\"/images/emoji/twitter/smile.png\" alt=\":smile:\"></p>",
  "<aside class=\"quote\"><div class=\"title\">carol:</div><blockquote><p>Quoted words</p></blockquote></aside>",
].join("");

function post(number, username, cooked, extra = {}) {
  return {
    id: 1100 + number, post_number: number, post_type: 1, username, name: username[0].toUpperCase() + username.slice(1),
    created_at: ago(30 - number), updated_at: ago(30 - number), reply_to_post_number: null, action_code: "",
    cooked, like_count: number, reads: 4, reply_count: 0, hidden: false, format: "html", origin: "discourse", ...extra,
  };
}

function fixture() {
  const list = (ids, more = false, page = 1, pages = 1) => ({ page, pages, more, rows: ids.map((id) => ROWS[id]) });
  return {
    "forum.json": {
      format: 1, kind: "forkmesh-forum", title: "Example Forum", description: "A forum for tests",
      source: { type: "discourse", origin: ORIGIN, lastSync: ago(1), pending: 0 },
      counts: { topics: 5, posts: 15, categories: 3 }, pageSize: 50, postsPerChunk: 100,
    },
    "categories.json": [
      { id: 1, name: "Internet censorship", slug: "censorship", color: "0088CC", text_color: "FFFFFF",
        description: "Technical and political aspects of censorship", parent_category_id: null, position: 0,
        subcategory_ids: [2], topic_count: 1, total_topic_count: 4, post_count: 3,
        latest: [ROWS[11], ROWS[12], ROWS[10]] },
      { id: 2, name: "Other countries", slug: "other", color: "e45735", description: "Countries without their own category",
        parent_category_id: 1, position: 1, subcategory_ids: [], topic_count: 3, total_topic_count: 3, post_count: 9,
        latest: [ROWS[11], ROWS[12], ROWS[14]] },
      { id: 3, name: "Community software", slug: "software", color: "bf1e2e\"><script>x</script>",
        description: "Software from our members", parent_category_id: null, position: 2, subcategory_ids: [],
        topic_count: 1, total_topic_count: 1, post_count: 3, latest: [ROWS[13]] },
    ],
    "tags.json": [{ name: "dpi", key: "dpi", count: 1 }],
    "latest/1.json": list([10, 11, 12], true, 1, 2),
    "latest/2.json": list([13, 14], false, 2, 2),
    "c/1/1.json": list([10, 11, 12, 14]),
    "c/2/1.json": list([11, 12, 14]),
    "c/3/1.json": list([13]),
    "tag/dpi/1.json": list([11]),
    "top/all.json": { period: "all", rows: [ROWS[11], ROWS[12]] },
    "topics/11/topic.json": {
      ...ROWS[11], highest_post_number: 3, participant_count: 3, link_count: 2,
      source: `${ORIGIN}/t/blocks-related-to-the-protests/11`, origin: "discourse", chunks: 2,
      uploads: { [`${ORIGIN}/uploads/default/original/1X/shot.png`]: UPLOAD },
      posts: [post(1, "alice", HOSTILE), post(2, "bob", "<p>Thanks for the list.</p>", { reply_to_post_number: 1 })],
    },
    "topics/11/posts/1.json": { posts: [post(3, "carol", "<p>A later reply.</p>")] },
    "topics/10/topic.json": {
      ...ROWS[10], source: `${ORIGIN}/t/read-this-first/10`, origin: "discourse", chunks: 1, uploads: {},
      posts: [post(1, "admin", "<p>Be kind.</p>"), post(2, "admin", "<p>Second rule.</p>")],
    },
  };
}

function applyPosted(files, event, account) {
  const result = { ok: true, commit: "c".repeat(40), event: event.id };
  if (event.type === "post") {
    const path = `topics/${event.topic}/local.json`;
    const local = files[path] || { posts: [], next: 1 };
    const key = `l${local.next}`;
    local.posts.push({ key, username: account, author: event.author, created_at: new Date().toISOString(), format: "markdown", raw: event.content.body, origin: "forkmesh", reply_to: event.content.replyTo || "" });
    local.next += 1;
    files[path] = local;
    Object.assign(result, { topic: event.topic, post: key });
  } else if (event.type === "like" || event.type === "unlike") {
    const path = `topics/${event.topic}/likes.json`;
    const likes = files[path] || {};
    likes[event.post] = event.type === "like" ? [account] : [];
    files[path] = likes;
    Object.assign(result, { topic: event.topic, post: event.post });
  } else if (event.type === "close") {
    files[`topics/${event.topic}/state.json`] = { ...(files[`topics/${event.topic}/state.json`] || {}), closed: true };
  } else if (event.type === "topic") {
    files["topics/n1/topic.json"] = { id: "n1", title: event.content.title, slug: "new", category_id: event.content.category, tags: event.content.tags, posts: [], chunks: 1, uploads: {}, origin: "forkmesh" };
    files["topics/n1/local.json"] = { posts: [{ key: "l1", username: account, author: event.author, created_at: new Date().toISOString(), format: "markdown", raw: event.content.body, origin: "forkmesh" }], next: 2 };
    Object.assign(result, { topic: "n1", post: "l1" });
  }
  return result;
}

async function stageForum(page, { files = fixture(), features = { forum: true }, start = "/alice/proj/forum", viewer = null, posted = [] } = {}) {
  if (viewer) {
    await page.addInitScript((account) => {
      localStorage.setItem("forkmesh.session", JSON.stringify({ nodeName: account, sessionToken: "test-session", status: "active" }));
    }, viewer.account);
  }
  const outside = [];
  page.on("request", (request) => {
    const url = new URL(request.url());
    if (url.hostname !== "127.0.0.1" && url.hostname !== "localhost") outside.push(request.url());
  });
  await page.route("https://cdn.tailwindcss.com/**", (route) =>
    route.fulfill({ contentType: "text/javascript", body: "window.tailwind = {};" }));
  await page.route("**/assets/vendor/lucide-0.468.0.min.js", (route) =>
    route.fulfill({ path: LUCIDE_PATH, contentType: "text/javascript; charset=utf-8" }));
  await page.route("**/alice/proj/forum**", (route) =>
    route.fulfill({ path: REPO_PAGE_PATH, contentType: "text/html; charset=utf-8" }));
  await page.route("**/dashboard.js*", (route) =>
    route.fulfill({ path: DASHBOARD_JS_PATH, contentType: "text/javascript; charset=utf-8" }));
  await page.route("**/dashboard-page-repo-forum.js*", (route) =>
    route.fulfill({ path: FORUM_JS_PATH, contentType: "text/javascript; charset=utf-8" }));
  await page.route("**/dashboard/tailwind.css", (route) =>
    route.fulfill({ path: DASHBOARD_CSS_PATH, contentType: "text/css; charset=utf-8" }));
  const reads = [];
  await page.route("**/api/**", (route) => {
    const url = new URL(route.request().url());
    if (url.pathname === "/api/repositories") {
      return route.fulfill({ json: { ok: true, repositories: [REPOSITORY] } });
    }
    if (url.pathname === "/api/repo/alice/proj/about") {
      return route.fulfill({ json: { ok: true, features: { issues: true, pulls: true, discussions: true, actions: true, ...features } } });
    }
    if (url.pathname === "/api/repo/alice/proj/blobs") {
      const blobs = {};
      url.searchParams.getAll("path").forEach((name) => {
        reads.push(name);
        const key = name.replace(/^\.forkmesh\/forum\//, "");
        blobs[name] = name.startsWith(".forkmesh/forum/") && key in files ? { content: JSON.stringify(files[key]) } : null;
      });
      return route.fulfill({ json: { ok: true, blobs } });
    }
    if (url.pathname === "/api/repo/alice/proj/forum") {
      if (route.request().method() === "GET") {
        return route.fulfill({ json: { ok: true, account: viewer?.account || "", moderator: Boolean(viewer?.moderator) } });
      }
      const body = JSON.parse(route.request().postData() || "{}");
      posted.push(body);
      return route.fulfill({ json: applyPosted(files, body.event, viewer?.account || "") });
    }
    if (url.pathname === "/api/repo/alice/proj/raw") {
      if (url.searchParams.get("path") === `.forkmesh/forum/uploads/${UPLOAD}`) {
        return route.fulfill({ status: 200, contentType: "image/png", body: PNG });
      }
      return route.fulfill({ status: 404, json: { ok: false } });
    }
    return route.fulfill({ json: { ok: true } });
  });
  await page.goto(start);
  return { outside, reads };
}

async function shot(page, name) {
  if (SHOTS) await page.screenshot({ path: path.join(SHOTS, `forum-${name}.png`), fullPage: true });
}

test("the Forum tab shows Discourse-style categories, lists and topics read from the repository", async ({ page }) => {
  await page.setViewportSize({ width: 1280, height: 900 });
  const { outside, reads } = await stageForum(page);
  const forum = page.locator("[data-repo-forum]");
  await expect(page.locator('[data-dashboard-repo-tab="forum"]')).toBeVisible();
  await expect(page.locator('[data-dashboard-repo-tab="forum"]')).toHaveAttribute("aria-selected", "true");
  const categories = forum.locator("[data-forum-categories] tbody tr");
  await expect(categories).toHaveCount(2);
  await expect(categories.nth(0)).toContainText("Internet censorship");
  await expect(categories.nth(0)).toContainText("Other countries");
  await expect(categories.nth(0)).toContainText("Blocks related to the protests");
  await expect(categories.nth(0).locator(".fm-forum-topics")).toHaveText("4");
  expect(await categories.nth(0).getAttribute("style")).toContain("--fm-forum-color:#0088cc");
  expect(await categories.nth(1).getAttribute("style")).toBeNull();
  await expect(forum.locator(".fm-forum-nav a[aria-current=page]")).toHaveText("Categories");
  await shot(page, "categories");

  await categories.nth(0).locator(".fm-forum-cat-name").click();
  await expect(page).toHaveURL(/\/alice\/proj\/forum\/c\/censorship\/1$/);
  await expect(forum.locator("[data-forum-categories] tbody tr")).toHaveCount(1);
  await expect(forum.locator("[data-forum-categories]")).toContainText("Other countries");
  await shot(page, "category");

  await forum.locator("[data-forum-categories] .fm-forum-cat-name").click();
  await expect(page).toHaveURL(/\/alice\/proj\/forum\/c\/other\/2$/);
  const topics = forum.locator("[data-forum-topic-rows] tr");
  await expect(topics).toHaveCount(3);
  await expect(topics.nth(0)).toContainText("3.0k");
  await expect(topics.nth(0).locator(".fm-forum-avatar")).toHaveCount(2);
  await expect(topics.nth(0).locator(".fm-forum-chip")).toHaveCount(0);
  await shot(page, "topics");

  await topics.nth(0).locator(".fm-forum-title").click();
  await expect(page).toHaveURL(/\/alice\/proj\/forum\/t\/blocks-related-to-the-protests\/11$/);
  const posts = forum.locator("[data-forum-posts]");
  await expect(posts.locator("[data-forum-post]")).toHaveCount(2);
  await expect(forum.locator("h1")).toContainText("Blocks related to the protests");
  await expect(forum.locator(".fm-forum-header .fm-forum-chip")).toHaveCount(2);
  const first = posts.locator('[data-forum-post="1"] .fm-forum-cooked');
  await expect(first.locator("script, iframe, svg, [onerror], [onclick], [style], [srcset]")).toHaveCount(0);
  await expect(first.locator('a[href^="javascript:"]')).toHaveCount(0);
  await expect(first.locator("a", { hasText: "bad link" })).toHaveCount(0);
  await expect(first).toContainText("bad link");
  await expect(first.locator('a[href="https://example.org/page"]')).toHaveAttribute("rel", "nofollow noopener noreferrer");
  await expect(first.locator(".fm-forum-mention")).toHaveText("@bob");
  await expect(first.locator("a[data-forum-link]")).toHaveAttribute("href", "/alice/proj/forum/t/topic/10/2");
  const images = first.locator("img");
  await expect(images).toHaveCount(1);
  expect(await images.first().getAttribute("src")).toContain(`/api/repo/alice/proj/raw?path=.forkmesh%2Fforum%2Fuploads%2F${UPLOAD}`);
  await expect(first.locator('a[href="https://other.example/remote.png"]')).toHaveText("[image: remote]");
  await expect(first).toContainText(":smile:");
  await expect(first.locator("aside.quote blockquote")).toContainText("Quoted words");
  expect(await page.evaluate(() => window.__pwned)).toBeUndefined();
  await expect(forum.locator(".fm-forum-stats")).toContainText("3.0k");
  await expect(forum.locator("[data-forum-original]")).toHaveAttribute("href", `${ORIGIN}/t/blocks-related-to-the-protests/11`);
  await expect(forum.locator("[data-forum-topics]")).toContainText("Looking for VPN measurements");
  await forum.locator("[data-forum-more-posts]").click();
  await expect(posts.locator("[data-forum-post]")).toHaveCount(3);
  await expect(forum.locator("[data-forum-more-posts]")).toHaveCount(0);
  await shot(page, "topic");

  await forum.locator("a[data-forum-link]", { hasText: "the rules" }).click();
  await expect(page).toHaveURL(/\/alice\/proj\/forum\/t\/read-this-first\/10\/2$/);
  await expect(forum.locator("h1")).toContainText("Read this first");
  await expect(forum.locator("[data-forum-reply-box]")).toHaveCount(0);
  await expect(forum).toContainText("This topic is closed for new replies.");

  await page.goBack();
  await expect(page).toHaveURL(/\/forum\/t\/blocks-related-to-the-protests\/11$/);
  await expect(forum.locator("h1")).toContainText("Blocks related to the protests");

  await page.goto("/alice/proj/forum/latest");
  await expect(forum.locator(".fm-forum-nav a[aria-current=page]")).toHaveText("Latest");
  await expect(forum.locator("[data-forum-topic-rows] tr")).toHaveCount(3);
  await expect(forum.locator("[data-forum-topic-rows] tr").first().locator(".fm-forum-excerpt")).toHaveText("The rules of the forum.");
  await forum.locator("[data-forum-more]").click();
  await expect(forum.locator("[data-forum-topic-rows] tr")).toHaveCount(5);
  await expect(forum.locator("[data-forum-more]")).toHaveCount(0);
  await shot(page, "latest");

  await page.goto("/alice/proj/forum/top");
  await expect(forum.locator(".fm-forum-nav a[aria-current=page]")).toHaveText("Top");
  await expect(forum.locator("[data-forum-topic-rows] tr")).toHaveCount(2);

  await page.goto("/alice/proj/forum/tag/dpi");
  await expect(forum.locator("[data-forum-topic-rows] tr")).toHaveCount(1);
  await expect(forum.locator("details[data-forum-pick] summary").last()).toContainText("dpi");

  expect(outside.filter((url) => !url.startsWith("https://cdn.tailwindcss.com/"))).toEqual([]);
  expect(reads.every((name) => name.startsWith(".forkmesh/forum/"))).toBe(true);
  expect(reads).not.toContain(".forkmesh/forum/source/discourse.json");
});

test("an enabled forum with nothing in it explains how to fill it", async ({ page }) => {
  await stageForum(page, { files: {} });
  await expect(page.locator("[data-forum-empty]")).toContainText("No forum in this repository yet");
  await expect(page.locator("[data-forum-empty]")).toContainText("--forum");
});

test("the Forum tab stays hidden until the owner turns the forum on", async ({ page }) => {
  await stageForum(page, { features: { forum: false } });
  await expect(page.locator("[data-forum-off]")).toContainText("turned off");
  await expect(page.locator('[data-dashboard-repo-tab="forum"]')).toBeHidden();
});

test("a signed-in reader replies, likes, and an operator closes the topic", async ({ page }) => {
  await page.setViewportSize({ width: 1280, height: 900 });
  const posted = [];
  await stageForum(page, { start: "/alice/proj/forum/t/blocks-related-to-the-protests/11", viewer: { account: "ada", moderator: true }, posted });
  const forum = page.locator("[data-repo-forum]");
  await expect(forum.locator("[data-forum-post]")).toHaveCount(2);
  await expect(forum.locator("[data-forum-operator]")).toBeVisible();
  await forum.locator('[data-forum-post="2"] [data-forum-reply-to]').click();
  await expect(forum.locator("[data-forum-replying]")).toContainText("Replying to post 2");
  await forum.locator("[data-forum-reply-box] textarea").fill("Thanks, **bob**. <script>window.__pwned=9</script>");
  await forum.locator("[data-forum-reply-box] button[type=submit]").click();
  await expect(forum.locator('[data-forum-post="l1"]')).toContainText("Thanks, bob.");
  await expect(forum.locator('[data-forum-post="l1"] strong', { hasText: "bob" })).toHaveCount(1);
  await expect(forum.locator('[data-forum-post="l1"] script')).toHaveCount(0);
  expect(await page.evaluate(() => window.__pwned)).toBeUndefined();
  const reply = posted[0];
  expect(reply.branch).toBe("main");
  expect(reply.event).toMatchObject({ type: "post", topic: "11", post: "", content: { body: "Thanks, **bob**. <script>window.__pwned=9</script>", replyTo: "2" }, authorName: "ada" });
  expect(reply.event.author).toMatch(/^[A-Za-z0-9_-]{43}$/);
  expect(reply.event.sig).toMatch(/^[A-Za-z0-9_-]{86}$/);
  expect(reply.event.id).toMatch(/^post-[0-9a-z]+-[0-9a-z]+$/);
  await expect(forum.locator('[data-forum-post="l1"] [data-forum-edit]')).toBeVisible();
  await forum.locator('[data-forum-post="1"] [data-forum-like]').click();
  await expect(forum.locator('[data-forum-post="1"] [data-forum-like]')).toHaveAttribute("aria-pressed", "true");
  expect(posted[1].event).toMatchObject({ type: "like", topic: "11", post: "1" });
  await shot(page, "topic-interactive");
  await forum.locator("[data-forum-operator] summary").click();
  await forum.locator('[data-forum-op="close"]').click();
  await expect(forum).toContainText("This topic is closed for new replies.");
  expect(posted[2].event).toMatchObject({ type: "close", topic: "11", post: "" });
});

test("a signed-in reader starts a topic and an operator opens the settings", async ({ page }) => {
  const posted = [];
  await stageForum(page, { start: "/alice/proj/forum/latest", viewer: { account: "ada", moderator: true }, posted });
  const forum = page.locator("[data-repo-forum]");
  await expect(forum.locator("[data-forum-admin-link]")).toBeVisible();
  await forum.locator("[data-forum-new]").click();
  await expect(page).toHaveURL(/\/alice\/proj\/forum\/new$/);
  await forum.locator("[data-forum-title]").fill("A new question");
  await forum.locator("[data-forum-category]").selectOption("2");
  await forum.locator("[data-forum-tags]").fill("DPI, vpn");
  await forum.locator("[data-forum-body-input]").fill("How do I check?");
  await forum.locator("[data-forum-composer] button[type=submit]").click();
  await expect(page).toHaveURL(/\/alice\/proj\/forum\/t\/[^/]+\/n1$/);
  await expect(forum.locator('[data-forum-post="l1"]')).toContainText("How do I check?");
  expect(posted[0].event).toMatchObject({ type: "topic", topic: "", content: { title: "A new question", category: 2, tags: ["dpi", "vpn"], body: "How do I check?" } });
  await page.goto("/alice/proj/forum/admin");
  await expect(forum.locator("[data-forum-site-form] [data-forum-site-title]")).toHaveValue("Example Forum");
  await expect(forum.locator("[data-forum-admin-categories] tr")).toHaveCount(3);
  await forum.locator('[data-forum-category-edit="2"]').click();
  await expect(forum.locator("[data-forum-category-form] [data-forum-category-name]")).toHaveValue("Other countries");
  await shot(page, "admin");
});

test("a reader who is not signed in sees no composer", async ({ page }) => {
  await stageForum(page, { start: "/alice/proj/forum/t/blocks-related-to-the-protests/11" });
  const forum = page.locator("[data-repo-forum]");
  await expect(forum.locator("[data-forum-post]")).toHaveCount(2);
  await expect(forum.locator("[data-forum-reply-box]")).toHaveCount(0);
  await expect(forum.locator("[data-forum-like]")).toHaveCount(0);
  await expect(forum.locator("[data-forum-operator]")).toHaveCount(0);
  await expect(forum).toContainText("Sign in to reply");
});
