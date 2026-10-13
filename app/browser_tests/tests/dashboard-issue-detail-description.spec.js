const { test, expect } = require("@playwright/test");
const path = require("node:path");

const PUBLIC = path.resolve(__dirname, "..", "..", "public");
const REPO_PAGE_PATH = path.join(PUBLIC, "dashboard", "repo.html");
const DASHBOARD_JS_PATH = path.join(PUBLIC, "dashboard.js");
const DASHBOARD_CSS_PATH = path.join(PUBLIC, "dashboard", "tailwind.css");

const OPENED_AT = 1_795_000_000_000;

// Narrow phone and wide desktop: the compact opening block has to hold at both
// ends, because a padded placeholder card costs the most exactly where there is
// least room for it.
const VIEWPORTS = [
  { name: "narrow", width: 390, height: 844 },
  { name: "wide", width: 1440, height: 900 },
];

function issueJson(number, body, status = "open", authorName = "jett") {
  return JSON.stringify({
    number,
    title: "Web issue detail duplicates the reporter",
    status,
    createdAt: OPENED_AT,
    events: [
      {
        id: `open-${number}`,
        type: "open",
        ts: OPENED_AT,
        author: `${authorName}-public-key`,
        authorName,
        title: "Web issue detail duplicates the reporter",
        body,
      },
      ...(status === "closed"
        ? [{ id: `status-${number}`, type: "status", ts: OPENED_AT + 1, status }]
        : []),
    ],
  });
}

// dana owns the repository, so the page renders with the owner's controls and
// the "add a description" path is the one under test.
async function mockIssuePage(page) {
  await page.addInitScript(() => {
    localStorage.setItem(
      "forkmesh.session",
      JSON.stringify({
        nodeName: "dana",
        sessionToken: "test-session",
        status: "active",
      }),
    );
  });
  await page.route("https://cdn.tailwindcss.com/**", (route) =>
    route.fulfill({
      contentType: "text/javascript",
      body: "window.tailwind = {};",
    }),
  );
  await page.route(
    "https://cdn.jsdelivr.net/npm/lucide@0.468.0/dist/umd/lucide.min.js",
    (route) =>
      route.fulfill({
        contentType: "text/javascript",
        body: "window.lucide = { createIcons() {} };",
      }),
  );
  await page.route("**/dana/notes/**", (route) =>
    route.fulfill({
      path: REPO_PAGE_PATH,
      contentType: "text/html; charset=utf-8",
    }),
  );
  await page.route("**/dashboard.js*", (route) =>
    route.fulfill({
      path: DASHBOARD_JS_PATH,
      contentType: "text/javascript; charset=utf-8",
    }),
  );
  await page.route("**/dashboard/tailwind.css", (route) =>
    route.fulfill({
      path: DASHBOARD_CSS_PATH,
      contentType: "text/css; charset=utf-8",
    }),
  );
  await page.route("**/api/**", (route) => {
    const url = new URL(route.request().url());
    const blobPath = url.searchParams.get("path") || "";
    let body = { ok: true };
    if (url.pathname === "/api/repositories") {
      body = {
        ok: true,
        repositories: [
          {
            owner: "dana",
            name: "notes",
            visibility: "public",
            source: "local-node",
            cloneUrl: "https://forkmesh.test/dana/notes",
            rootCommit: "root",
            stateHash: "state",
            updatedAt: "2",
          },
        ],
      };
    } else if (url.pathname === "/api/repo/dana/notes/branches") {
      body = {
        ok: true,
        branches: [
          { name: "main", commit: "2222222222222222222222222222222222222222" },
        ],
      };
    } else if (url.pathname === "/api/repo/dana/notes/blob") {
      // #7 is the reported case: a title and nothing else. #8 is the ordinary
      // issue that must keep its rendered description.
      if (blobPath === ".forkmesh/issues/open/7/issue-7.json") {
        body = { ok: true, content: issueJson(7, "") };
      } else if (blobPath === ".forkmesh/issues/open/8/issue-8.json") {
        body = { ok: true, content: issueJson(8, "A described issue.", "closed") };
      } else if (blobPath === ".forkmesh/issues/open/9/issue-9.json") {
        // Filed by someone else, so #8's mention row does not repeat jett.
        body = { ok: true, content: issueJson(9, "Finish the linked work first.\n\nParent issue: #8", "open", "mona") };
      } else {
        return route.fulfill({
          status: 404,
          contentType: "application/json; charset=utf-8",
          body: JSON.stringify({ ok: false, error: "not_found" }),
        });
      }
    } else if (url.pathname === "/api/repo/dana/notes/tree") {
      const entries = blobPath === ".forkmesh/issues/open"
        ? [{ type: "tree", name: "7" }, { type: "tree", name: "8" }, { type: "tree", name: "9" }]
        : [];
      body = {
        ok: true,
        commit: "2222222222222222222222222222222222222222",
        entries,
      };
    }
    return route.fulfill({
      status: 200,
      contentType: "application/json; charset=utf-8",
      body: JSON.stringify(body),
    });
  });
}

test("an issue with no description opens compact and names its author once", async ({
  page,
}) => {
  await mockIssuePage(page);
  await page.setViewportSize(VIEWPORTS[0]);
  await page.goto("/dana/notes/issues/7");

  const detail = page.locator('[data-repo-record-detail="issues"]');
  await expect(detail).toBeVisible({ timeout: 15_000 });
  const opening = detail.locator("[data-repo-record-description]");

  for (const viewport of VIEWPORTS) {
    await page.setViewportSize({
      width: viewport.width,
      height: viewport.height,
    });

    // One issue, one reporter, one avatar in the page body: as on GitHub the
    // opening comment introduces jett and the hero (title and state) does not
    // repeat them. The sidebar's participant list is the only other face.
    await expect(
      detail.locator('[data-repo-record-hero] [title="jett"]'),
    ).toHaveCount(0);
    await expect(
      detail.locator('[data-repo-record-conversation] [title="jett"]'),
    ).toHaveCount(1);
    await expect(
      detail.locator('[data-repo-issue-opening]'),
    ).toContainText("jett opened");

    // Nothing is invented to fill an empty description, and there is no body
    // container left to pad out around it.
    await expect(detail).not.toContainText("No description was committed");
    await expect(detail.locator("[data-repo-record-body]")).toHaveCount(0);
    await expect(opening).toHaveAttribute("data-repo-record-description", "add");
    // innerText, so the collapsed editor below the control does not count:
    // what is painted here is the control and nothing else.
    await expect(opening).toHaveText("Add a description", {
      useInnerText: true,
    });

    // One row tall, on the wide screen too. The conversation column used to be
    // stretched to the taller sidebar and hand that surplus down to its rows,
    // which inflated whatever sat here into the padded box that was reported.
    const openBox = await opening.boundingBox();
    expect(
      openBox.height,
      `opening block stays one row tall on the ${viewport.name} screen`,
    ).toBeLessThan(48);
    expect(openBox.width).toBeLessThanOrEqual(viewport.width);
  }

  // The compact state is not a dead end: the owner still writes the first
  // description from here.
  await detail.locator("[data-repo-issue-description-edit]").click();
  const input = detail.locator("[data-repo-issue-description-input]");
  await expect(input).toBeVisible();
  await expect(input).toBeFocused();
  await expect(input).toHaveValue("");
  await input.fill("Now it has one.");
  await expect(input).toHaveValue("Now it has one.");
});

test("an issue with a description still renders it once, under one author", async ({
  page,
}) => {
  await mockIssuePage(page);
  await page.setViewportSize(VIEWPORTS[1]);
  await page.goto("/dana/notes/issues/8");

  const detail = page.locator('[data-repo-record-detail="issues"]');
  await expect(detail).toBeVisible({ timeout: 15_000 });

  for (const viewport of VIEWPORTS) {
    await page.setViewportSize({
      width: viewport.width,
      height: viewport.height,
    });
    await expect(
      detail.locator('[data-repo-record-conversation] [title="jett"]'),
    ).toHaveCount(1);
    const card = detail.locator("[data-repo-record-description]");
    await expect(card).toHaveAttribute("data-repo-record-description", "body");
    await expect(detail.locator("[data-repo-record-body]")).toContainText(
      "A described issue.",
    );

    // The card ends where its own last row ends. It is not grown to meet the
    // sidebar, which is what turned a short description into a mostly empty
    // bordered box on a wide screen.
    const cardBox = await card.boundingBox();
    const lastRowBox = await detail
      .locator("[data-repo-issue-reactions]")
      .boundingBox();
    expect(
      cardBox.y + cardBox.height - (lastRowBox.y + lastRowBox.height),
      `description card hugs its content on the ${viewport.name} screen`,
    ).toBeLessThan(24);
  }

  // Editing an existing description is unchanged: the body hides, the form
  // opens on the committed text.
  await detail.locator("[data-repo-issue-description-edit]").click();
  await expect(detail.locator("[data-repo-record-body]")).toBeHidden();
  await expect(
    detail.locator("[data-repo-issue-description-input]"),
  ).toHaveValue("A described issue.");
});

test("a sub-issue names its parent beside the title with live status", async ({
  page,
}) => {
  await mockIssuePage(page);
  await page.setViewportSize(VIEWPORTS[1]);
  await page.goto("/dana/notes/issues/9");

  const detail = page.locator('[data-repo-record-detail="issues"]');
  await expect(detail).toBeVisible({ timeout: 15_000 });
  const parents = detail.locator("[data-repo-record-hero] [data-repo-issue-parents]");
  await expect(parents).toBeVisible();
  await expect(parents).toContainText("Sub-issue of");
  await expect(detail.locator("[data-repo-record-body]")).not.toContainText(
    "Parent issue",
  );
  await expect(parents.locator("a")).toHaveText(
    "#8Web issue detail duplicates the reporterClosed",
  );
  await expect(parents.locator("a")).toHaveAttribute(
    "href", "/dana/notes/issues/8",
  );
});

test("a parent lists sub-issue status, records the mention, and adds children", async ({
  page,
}) => {
  await mockIssuePage(page);
  await page.setViewportSize(VIEWPORTS[1]);
  await page.goto("/dana/notes/issues/8");

  const detail = page.locator('[data-repo-record-detail="issues"]');
  await expect(detail).toBeVisible({ timeout: 15_000 });
  const subIssues = detail.locator("[data-repo-issue-subissues]");
  await expect(subIssues).toBeVisible();
  await expect(subIssues).toContainText("0 of 1 closed");
  const child = subIssues.locator('[data-repo-issue-subissue="9"]');
  await expect(child).toHaveAttribute("href", "/dana/notes/issues/9");
  await expect(child).toContainText("0%");
  await expect(child).toContainText("Unassigned");
  await expect(child).toContainText("Open");

  // #9's description names #8, so #8's history says who mentioned it where.
  const mention = detail.locator('[data-repo-issue-mention="9"]');
  await expect(mention).toContainText("mona");
  await expect(mention).toContainText("mentioned this in");
  await expect(mention.locator("a", { hasText: "#9" })).toHaveAttribute(
    "href", "/dana/notes/issues/9",
  );

  await subIssues.locator("[data-repo-issue-add-subissue]").click();
  await expect(page.locator("[data-repo-issue-form]")).toContainText(
    "New sub-issue of #8",
  );
  await expect(page.locator("[data-repo-issue-body]")).toHaveValue(
    "Parent issue: #8\n\n",
  );
});
