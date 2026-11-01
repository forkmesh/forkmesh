(() => {
  const $ = (selector) => document.querySelector(selector);
  let send = null;
  let forum = null;

  async function render(tab) {
    const section = $("[data-forum]");
    section.hidden = true;
    forum = null;
    if (!Number.isInteger(tab?.id) || !/^https:/.test(tab?.url || "")) return;
    const result = await send({ type: "fm-forum-detect", tabId: tab.id });
    forum = result?.ok ? result.forum : null;
    if (!forum) return;
    const host = new URL(forum.origin).host;
    $("[data-forum-title]").textContent = forum.title || host;
    $("[data-forum-host]").textContent = host;
    section.hidden = false;
  }

  function bind(sender) {
    send = sender;
    $("[data-forum-mirror]").addEventListener("click", () => {
      if (forum) void send({ type: "fm-forum-mirror", origin: forum.origin }).then(() => window.close());
    });
  }

  self.ForkMeshForumPopup = { bind, render };
})();
