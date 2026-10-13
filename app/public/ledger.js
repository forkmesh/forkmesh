(() => {
  const list = document.querySelector("[data-ledger-list]");
  const rows = document.querySelector("[data-ledger-rows]");
  if (!list || !rows) return;

  const state = {
    entries: [],
    cursor: null,
    hasMore: false,
    loading: false,
    quote: null,
    backfill: null,
    expected: [],
    wallets: [],
    memberWallet: null,
    memberWalletLoaded: false,
    observedAt: 0,
    view: "all",
    category: "all",
    receipt: "all",
    search: "",
  };

  const CATEGORY_LABELS = {
    "member-tip": "Member tip",
    "pool-contribution": "Pool contribution",
    "pending-reward": "Account reward",
    "mirror-reward": "Mirror reward",
    "microjob-payout": "Microjob",
    "contributor-reward": "Issue / PR reward",
    "world-element-purchase": "World purchase",
    "world-element-sweep": "Deposit sweep",
    "world-mirror-share": "World mirror share",
    "advertising-revenue": "Advertising revenue",
    "operating-expense": "Operating expense",
    "reimbursement-debt": "Reimbursement debt",
    "expected-expense": "Expected expense",
    "historical-chain-transfer": "Historical chain transfer",
  };

  const FAILED_STATUSES = new Set([
    "expired",
    "rejected",
    "distribution_failed",
    "failed",
  ]);

  const PENDING_STATUSES = new Set([
    "pending_wallet",
    "pending_merge",
    "pending_review",
    "pending_signature",
    "prepared",
    "approved",
    "queued",
    "ready",
    "submitted",
    "awaiting_finality",
    "awaiting_distribution",
    "outstanding",
  ]);

  function setText(selector, value) {
    const element = document.querySelector(selector);
    if (element) element.textContent = value;
  }

  function exactInteger(value) {
    try {
      return BigInt(String(value ?? 0));
    } catch (_) {
      return 0n;
    }
  }

  function groupWhole(value) {
    return String(value || "0").replace(/\B(?=(\d{3})+(?!\d))/g, ",");
  }

  function formatSol(value) {
    const text = String(value ?? "0");
    const [whole, fraction = ""] = text.split(".");
    return `${groupWhole(whole)}${fraction ? `.${fraction}` : ""}`;
  }

  function solFromLamports(lamports) {
    const amount = exactInteger(lamports);
    const whole = amount / 1_000_000_000n;
    const fraction = String(amount % 1_000_000_000n)
      .padStart(9, "0")
      .replace(/0+$/, "");
    return `${groupWhole(whole)}${fraction ? `.${fraction}` : ""}`;
  }

  function usdFromLamports(lamports) {
    const amount = exactInteger(lamports);
    const priceMicros = exactInteger(state.quote?.priceMicros);
    if (!amount || !priceMicros) return { amountUsd: null, amountUsdMicros: 0 };
    const micros = (amount * priceMicros + 500_000_000n) / 1_000_000_000n;
    const whole = micros / 1_000_000n;
    const fraction = String(micros % 1_000_000n).padStart(6, "0");
    return {
      amountUsd: `${whole}.${fraction}`,
      amountUsdMicros: Number(micros),
    };
  }

  function formatUsd(value) {
    if (value === null || value === undefined || value === "") return "—";
    const amount = Number(value);
    if (!Number.isFinite(amount)) return "—";
    const digits = amount >= 1 ? 2 : amount >= 0.01 ? 4 : 6;
    return `$${amount.toLocaleString(undefined, {
      minimumFractionDigits: digits,
      maximumFractionDigits: digits,
    })}`;
  }

  function formatUsdMicros(micros) {
    const amount = Number(micros) / 1_000_000;
    if (!Number.isFinite(amount)) return "—";
    return `$${amount.toLocaleString(undefined, {
      minimumFractionDigits: 2,
      maximumFractionDigits: 2,
    })}`;
  }

  function emptyAccountingTotal() {
    return { usdMicros: 0n, unvaluedLamports: 0n };
  }

  function addAccountingAmount(total, entry) {
    const usdMicros = exactInteger(entry?.amountUsdMicros);
    if (usdMicros) {
      total.usdMicros += usdMicros;
      return;
    }
    // Do not apply today's quote to a historical transfer. Keep an exact SOL
    // subtotal until the record carries an authoritative payment-time value.
    total.unvaluedLamports += exactInteger(entry?.amountLamports);
  }

  function formatAccountingTotal(total) {
    const parts = [];
    if (total.usdMicros || !total.unvaluedLamports) {
      parts.push(formatUsdMicros(total.usdMicros));
    }
    if (total.unvaluedLamports) {
      parts.push(`${solFromLamports(total.unvaluedLamports)} SOL`);
    }
    return parts.join(" + ");
  }

  function formatDate(timestamp) {
    const value = Number(timestamp) || 0;
    if (!value) return { date: "Unknown", time: "" };
    const date = new Date(value);
    return {
      date: date.toLocaleDateString(undefined, {
        year: "numeric",
        month: "short",
        day: "numeric",
      }),
      time: date.toLocaleTimeString(undefined, {
        hour: "numeric",
        minute: "2-digit",
        timeZoneName: "short",
      }),
    };
  }

  // A row that names people should be scannable by face. Portraits come from
  // the same public profile endpoint chat.js reads, are cached per name for
  // the life of the page (every filter change rebuilds the rows, and a
  // re-render must not re-fetch), and are never waited on: the chip is
  // readable as an initial from the first paint, and stays one for an account
  // that published no picture and for a name that is not a user account.
  const avatarPortraits = new Map();

  function avatarHue(name) {
    let hash = 0;
    for (const character of String(name || "")) {
      hash = (hash * 31 + character.codePointAt(0)) % 360;
    }
    return hash;
  }

  function avatarImageSource(png) {
    const value = String(png || "");
    if (!value) return "";
    return value.startsWith("data:") ? value : `data:image/png;base64,${value}`;
  }

  let directoryPortraits = null;

  function loadDirectoryPortraits() {
    if (directoryPortraits) return directoryPortraits;
    directoryPortraits = fetch("/api/accounts/users", {
      headers: { accept: "application/json" },
    })
      .then((response) => (response.ok ? response.json() : null))
      .then((body) => {
        const portraits = new Map();
        for (const user of Array.isArray(body?.users) ? body.users : []) {
          const key = String(user?.name || "").trim().toLowerCase();
          if (key) portraits.set(key, avatarImageSource(user?.avatarPng));
        }
        return portraits;
      })
      .catch(() => new Map());
    return directoryPortraits;
  }

  function loadAvatarPortrait(name) {
    if (avatarPortraits.has(name)) return avatarPortraits.get(name);
    const pending = loadDirectoryPortraits().then((portraits) => (
      portraits.has(name) ? portraits.get(name) : loadAccountPortrait(name)
    ));
    avatarPortraits.set(name, pending);
    return pending;
  }

  function loadAccountPortrait(name) {
    return fetch(`/api/accounts/${encodeURIComponent(name)}`, {
      headers: { accept: "application/json" },
    })
      .then((response) => (response.ok ? response.json() : null))
      .then((profile) => (
        profile && profile.exists !== false && profile.kind === "user"
          ? avatarImageSource(profile.avatarPng)
          : ""
      ))
      .catch(() => "");
  }

  function makeUserAvatar(name) {
    const element = document.createElement("span");
    element.className = "ledger-avatar";
    element.setAttribute("aria-hidden", "true");
    // Either half of a user link may be the one that carries the handle, and
    // the label carries it with its leading "@".
    const key = String(name || "").trim().replace(/^@/, "").toLowerCase();
    element.textContent = (key.charAt(0) || "?").toUpperCase();
    element.style.setProperty("--avatar-hue", String(avatarHue(key)));
    if (!key) return element;
    void loadAvatarPortrait(key).then((source) => {
      if (!source || element.firstElementChild) return;
      const image = document.createElement("img");
      image.alt = "";
      image.loading = "lazy";
      image.src = source;
      element.textContent = "";
      element.style.background = "transparent";
      element.append(image);
    });
    return element;
  }

  function categoryGroup(category) {
    if (category === "member-tip") return "tips";
    if (["pool-contribution", "pending-reward", "mirror-reward"].includes(category)) {
      return "rewards";
    }
    if (category === "microjob-payout") return "microjobs";
    if (category === "contributor-reward") return "contributor-rewards";
    if (String(category || "").startsWith("world-")) return "world";
    if (category === "advertising-revenue") return "revenue";
    if (["operating-expense", "reimbursement-debt", "expected-expense"].includes(category)) {
      return "operations";
    }
    if (category === "historical-chain-transfer") return "historical";
    return "other";
  }

  function receiptGroup(entry) {
    if (entry.currency !== "SOL") return "external";
    return entry.transactionId ? "onchain" : "pending";
  }

  function isPendingPayment(entry) {
    const status = String(entry?.status || "").toLowerCase();
    if (entry?.category === "reimbursement-debt") {
      return status === "outstanding";
    }
    return entry?.currency === "SOL"
      && !entry?.transactionId
      && PENDING_STATUSES.has(status)
      && !FAILED_STATUSES.has(status);
  }

  function isPaidPayment(entry) {
    // A submitted signature can still be awaiting finality. Count only
    // finalized receipts (or a member-reported public receipt), not a merely
    // submitted payment.
    const status = String(entry?.status || "").toLowerCase();
    const verification = String(entry?.verification || "").toLowerCase();
    return entry?.currency === "SOL"
      && Boolean(String(entry?.transactionId || "").trim())
      && !PENDING_STATUSES.has(status)
      && !FAILED_STATUSES.has(status)
      && (verification.startsWith("finalized-") || status === "reported-on-chain");
  }

  function isExpectedExpense(entry) {
    return entry?.category === "expected-expense";
  }

  function isAccountingIncome(entry) {
    if (entry?.direction !== "inbound") return false;
    return entry?.currency !== "SOL" || isPaidPayment(entry);
  }

  function isAccountingExpense(entry) {
    if (entry?.category === "operating-expense") return true;
    if (entry?.category === "member-tip"
        && entry?.verification === "finalized-exact-work-payment") return true;
    return entry?.direction === "outbound" && isPaidPayment(entry);
  }

  function isAccountingDebt(entry) {
    const status = String(entry?.status || "").toLowerCase();
    if (entry?.category === "reimbursement-debt") {
      return status === "outstanding";
    }
    return (entry?.direction === "allocation" || entry?.direction === "outbound")
      && isPendingPayment(entry);
  }

  function safeHref(value) {
    const href = String(value || "").trim();
    if (href.startsWith("/") && !href.startsWith("//")) return href;
    try {
      const parsed = new URL(href);
      return parsed.protocol === "https:" || parsed.protocol === "http:"
        ? parsed.href
        : "";
    } catch (_) {
      return "";
    }
  }

  function contributorRewardEntry(reward) {
    const rewardId = String(reward?.rewardId || "").trim();
    if (!rewardId) return null;
    const recipient = String(reward?.recipient || "").trim();
    const recipientAddress = String(reward?.recipientAddress || "").trim();
    const repository = String(reward?.repository || "").trim();
    const sourceKind = reward?.sourceKind === "pull" ? "pull" : "issue";
    const subjectNumber = Number(reward?.subjectNumber) || 0;
    const subjectTitle = String(reward?.subjectTitle || "").trim();
    const subjectSummary = String(reward?.subjectSummary || "").trim();
    const subjectUrl = safeHref(reward?.subjectUrl);
    const signature = String(reward?.transactionSignature || "").trim();
    const amountLamports = exactInteger(reward?.amountLamports);
    const liveUsd = usdFromLamports(amountLamports);
    const paymentValuedAt = Number(reward?.paymentValuedAt) || 0;
    const paymentUsd = paymentValuedAt
      ? (reward?.paymentUsd ?? null)
      : liveUsd.amountUsd;
    const paymentUsdMicros = paymentValuedAt
      ? Number(reward?.paymentUsdMicros || 0)
      : liveUsd.amountUsdMicros;
    const links = [];
    if (recipient) {
      links.push({
        type: "user",
        role: "recipient",
        key: recipient.toLowerCase(),
        label: `@${recipient}`,
        url: `/@${encodeURIComponent(recipient.toLowerCase())}`,
        discordUsername: String(reward?.recipientDiscordUsername || "").trim(),
        discordDisplayName: String(reward?.recipientDiscordDisplayName || "").trim(),
      });
    }
    if (recipientAddress) {
      links.push({
        type: "wallet",
        role: "recipient",
        key: recipientAddress,
        label: recipientAddress,
        url: `https://explorer.solana.com/address/${encodeURIComponent(recipientAddress)}`,
      });
    }
    if (repository && subjectNumber && subjectUrl) {
      links.push({
        type: sourceKind,
        role: "subject",
        key: `${repository}#${subjectNumber}`,
        label: `${repository} ${sourceKind} #${subjectNumber}`,
        title: subjectTitle,
        summary: subjectSummary,
        url: subjectUrl,
      });
    }
    return {
      id: `contributor-reward:${rewardId}`,
      category: "contributor-reward",
      title: sourceKind === "pull" ? "Pull request reward" : "Issue report reward",
      status: String(reward?.status || "recorded"),
      direction: signature ? "outbound" : "allocation",
      verification: signature ? "finalized-exact-transfer" : "owner-authored-estimate",
      network: "mainnet-beta",
      transactionId: signature,
      explorerUrl: signature
        ? `https://explorer.solana.com/tx/${encodeURIComponent(signature)}`
        : "",
      amountLamports: Number(amountLamports),
      amountSol: String(reward?.amountSol || solFromLamports(amountLamports)),
      amountKnown: true,
      currency: "SOL",
      amountMinor: 0,
      amountUsd: paymentUsd,
      amountUsdMicros: paymentUsdMicros,
      valuationBasis: paymentValuedAt
        ? String(reward?.valuationBasis || "payment-sol-usd-unavailable")
        : "current-spot-estimate",
      valuationObservedAt: paymentValuedAt,
      occurredAt: Number(reward?.updatedAt) || Number(reward?.createdAt) || 0,
      sourceTable: "contributor_reward_estimates",
      sourceId: rewardId,
      links,
      note: [
        reward?.condition === "merged" && reward?.status === "pending_merge"
          ? "Payment is conditional on merge."
          : "Paid directly from the repository owner's Qt wallet.",
      ].filter(Boolean).join(" · "),
    };
  }

  async function loadContributorRewards() {
    try {
      const response = await fetch("/api/rewards/estimates", {
        headers: { accept: "application/json" },
        cache: "no-store",
      });
      if (!response.ok) return [];
      const payload = await response.json();
      if (!payload?.ok || !Array.isArray(payload.rewards)) return [];
      return payload.rewards.map(contributorRewardEntry).filter(Boolean);
    } catch (_) {
      return [];
    }
  }

  function readSession() {
    try {
      const session = JSON.parse(localStorage.getItem("forkmesh.session") || "null");
      if (!session?.nodeName || session.kind === "node") return null;
      return session;
    } catch (_) {
      return null;
    }
  }

  // Every balance on this strip is a cached public reading, not a live check
  // this page load performed, so each card says how old its number is.
  function balanceAgeLabel(checkedAt) {
    const stamp = Number(checkedAt) || 0;
    if (!stamp) return "never read";
    const minutes = Math.floor((Date.now() - stamp) / 60_000);
    if (minutes < 1) return "read just now";
    if (minutes < 60) return `read ${minutes} min ago`;
    const hours = Math.floor(minutes / 60);
    if (hours < 24) return `read ${hours === 1 ? "1 hour" : `${hours} hours`} ago`;
    const days = Math.floor(hours / 24);
    return `read ${days === 1 ? "1 day" : `${days} days`} ago`;
  }

  function abbreviatedAddress(address) {
    const text = String(address || "");
    return text.length <= 18 ? text : `${text.slice(0, 7)}…${text.slice(-7)}`;
  }

  // The signed-in reader's own published payout address. It is the one wallet
  // on this strip the relay cannot name in a public ledger response, because
  // which member is reading is not part of a cached public page.
  async function loadMemberWallet({ refresh = false } = {}) {
    const session = readSession();
    if (!session) return null;
    const account = String(session.nodeName || "").trim().toLowerCase();
    if (!account) return null;
    try {
      const profileResponse = await fetch(
        `/api/accounts/${encodeURIComponent(account)}`,
        { headers: { accept: "application/json" }, cache: "no-store" },
      );
      if (!profileResponse.ok) return null;
      const profile = await profileResponse.json();
      const address = String(profile?.solana || "").trim();
      const wallet = {
        key: "member",
        label: "Your wallet",
        purpose:
          "The public payout address published on your ForkMesh profile. "
          + "Tips and rewards other people send you arrive here.",
        emptyText: "Not published",
        emptyPurpose:
          "Publish a public payout address on your account so tips and "
          + "rewards can reach you.",
        address,
        network: "",
        explorerUrl: address
          ? `https://explorer.solana.com/address/${encodeURIComponent(address)}`
          : "",
        balanceLamports: 0,
        balanceKnown: false,
        balanceCheckedAt: 0,
      };
      if (!address) return wallet;
      const balanceResponse = await fetch(
        `/api/world/wallet?address=${encodeURIComponent(address)}${refresh ? "&refresh=1" : ""}`,
        { headers: { accept: "application/json" }, cache: "no-store" },
      );
      if (!balanceResponse.ok) return wallet;
      const balance = await balanceResponse.json();
      if (balance?.lamports === null || balance?.lamports === undefined) return wallet;
      wallet.balanceLamports = Number(balance.lamports) || 0;
      wallet.balanceKnown = true;
      wallet.balanceCheckedAt = Number(balance.checkedAt) || 0;
      return wallet;
    } catch (_) {
      return null;
    }
  }

  const ICONS = {
    wallet: '<path d="M4 7.5A2.5 2.5 0 0 1 6.5 5H18v3"></path><path d="M4 7.5V17a2 2 0 0 0 2 2h13a1 1 0 0 0 1-1V9a1 1 0 0 0-1-1H6.5A2.5 2.5 0 0 1 4 7.5Z"></path><path d="M16 13.5h.01"></path>',
    issue: '<circle cx="12" cy="12" r="8"></circle><circle cx="12" cy="12" r="1.6"></circle>',
    pull: '<circle cx="7" cy="6" r="2"></circle><circle cx="7" cy="18" r="2"></circle><circle cx="17" cy="18" r="2"></circle><path d="M7 8v8"></path><path d="M17 16V9a3 3 0 0 0-3-3h-3"></path><path d="m13 4-2 2 2 2"></path>',
    record: '<path d="M7 3h7l5 5v13H7Z"></path><path d="M14 3v5h5"></path><path d="M10 13h6M10 17h6"></path>',
    external: '<path d="M14 5h5v5"></path><path d="M19 5 11 13"></path><path d="M18 14v4a1 1 0 0 1-1 1H6a1 1 0 0 1-1-1V7a1 1 0 0 1 1-1h4"></path>',
    chevron: '<path d="m6 9 6 6 6-6"></path>',
    arrow: '<path d="M5 12h14"></path><path d="m13 6 6 6-6 6"></path>',
  };

  function icon(name, className = "") {
    const svg = document.createElementNS("http://www.w3.org/2000/svg", "svg");
    svg.setAttribute("viewBox", "0 0 24 24");
    svg.setAttribute("fill", "none");
    svg.setAttribute("stroke", "currentColor");
    svg.setAttribute("stroke-width", "1.8");
    svg.setAttribute("stroke-linecap", "round");
    svg.setAttribute("stroke-linejoin", "round");
    svg.setAttribute("aria-hidden", "true");
    if (className) svg.setAttribute("class", className);
    svg.innerHTML = ICONS[name] || "";
    return svg;
  }

  function capitalized(value) {
    const text = String(value || "");
    return text.charAt(0).toUpperCase() + text.slice(1);
  }

  function makeWalletCard(wallet) {
    const card = document.createElement("article");
    card.className = "ledger-wallet";
    card.dataset.walletKey = String(wallet.key || "wallet");

    const head = document.createElement("div");
    head.className = "ledger-wallet-head";
    const tile = document.createElement("span");
    tile.className = "ledger-wallet-tile";
    tile.appendChild(icon("wallet"));
    const label = document.createElement("span");
    label.className = "ledger-wallet-label";
    label.textContent = String(wallet.label || "Wallet");
    head.append(tile, label);

    const amount = document.createElement("strong");
    amount.className = "ledger-wallet-amount";
    amount.textContent = !wallet.address
      ? String(wallet.emptyText || "Not configured")
      : wallet.balanceKnown
        ? `${solFromLamports(wallet.balanceLamports)} SOL`
        : "Not read yet";
    card.append(head, amount);

    if (wallet.address) {
      const href = safeHref(wallet.explorerUrl);
      const address = document.createElement(href ? "a" : "code");
      address.className = "ledger-wallet-address";
      address.textContent = abbreviatedAddress(wallet.address);
      address.title = String(wallet.address);
      if (href) {
        address.href = href;
        address.target = "_blank";
        address.rel = "noopener noreferrer";
      }
      card.appendChild(address);
    }

    const note = document.createElement("small");
    note.className = "ledger-wallet-note";
    note.textContent = !wallet.address
      ? String(wallet.emptyPurpose || wallet.purpose || "")
      : [
          wallet.balanceKnown ? capitalized(balanceAgeLabel(wallet.balanceCheckedAt)) : "",
          wallet.sharedAddress
            ? "This host configured one address for both system roles."
            : "",
        ].filter(Boolean).join(" · ") || String(wallet.purpose || "");
    note.title = String(wallet.purpose || "");
    card.appendChild(note);
    return card;
  }

  function renderWallets() {
    const strip = document.querySelector("[data-ledger-wallets]");
    if (!strip) return;
    const cards = [
      ...(state.memberWallet ? [state.memberWallet] : []),
      ...state.wallets,
    ];
    if (!cards.length) {
      strip.hidden = state.memberWalletLoaded && !state.loading;
      return;
    }
    strip.hidden = false;
    strip.replaceChildren(...cards.map(makeWalletCard));
  }

  function abbreviatedLinkLabel(link) {
    const label = String(link?.label || link?.key || "record");
    if (link?.type !== "wallet" || label.length <= 18) return label;
    return `${label.slice(0, 7)}…${label.slice(-7)}`;
  }

  function shortSignature(signature) {
    const text = String(signature || "");
    return text.length <= 16 ? text : `${text.slice(0, 6)}…${text.slice(-6)}`;
  }

  function entryLinks(entry, predicate) {
    return (Array.isArray(entry.links) ? entry.links : [])
      .filter((link) => link && typeof link === "object" && predicate(link));
  }

  function discordIdentity(link) {
    const username = String(link.discordUsername || "").trim().replace(/^@+/, "");
    const displayName = String(link.discordDisplayName || "").trim();
    if (username && displayName) return `Discord @${username} (${displayName})`;
    if (username) return `Discord @${username}`;
    return displayName ? `Discord ${displayName}` : "";
  }

  function roleLabel(link) {
    return capitalized(String(link.role || link.type || "record").replaceAll("-", " "));
  }

  function openOutside(element, href) {
    element.href = href;
    if (!href.startsWith("/")) {
      element.target = "_blank";
      element.rel = "noopener noreferrer";
    }
  }

  function makeLinkChip(link) {
    const href = safeHref(link.url);
    const element = document.createElement(href ? "a" : "span");
    element.className = href ? "ledger-link" : "ledger-link-static";
    const person = link.type === "user";
    if (person) {
      element.classList.add("ledger-link-person");
      element.appendChild(makeUserAvatar(link.key || link.label));
    } else {
      const role = document.createElement("span");
      role.className = "ledger-link-role";
      role.textContent = roleLabel(link);
      element.appendChild(role);
    }
    const label = document.createElement(link.type === "wallet" ? "code" : "span");
    label.className = "ledger-link-label";
    label.textContent = abbreviatedLinkLabel(link);
    element.appendChild(label);
    element.title = [
      person ? `${roleLabel(link)} ${link.label || link.key || ""}` : String(link.label || link.key || ""),
      person ? discordIdentity(link) : "",
    ].filter(Boolean).join(" · ");
    if (href) openOutside(element, href);
    return element;
  }

  function makeParties(entry) {
    const people = entryLinks(entry, (link) => link.type === "user");
    if (!people.length) return null;
    const container = document.createElement("div");
    container.className = "ledger-parties";
    const senders = people.filter((link) => link.role === "sender");
    const recipients = people.filter((link) => link.role === "recipient");
    const others = people.filter((link) => link.role !== "sender" && link.role !== "recipient");
    const lead = (text) => {
      const element = document.createElement("span");
      element.className = "ledger-parties-role";
      element.textContent = text;
      container.appendChild(element);
    };
    if (senders.length && !recipients.length) lead("From");
    senders.forEach((link) => container.appendChild(makeLinkChip(link)));
    if (senders.length && recipients.length) {
      container.appendChild(icon("arrow", "ledger-parties-arrow"));
    } else if (recipients.length) {
      lead("To");
    }
    recipients.forEach((link) => container.appendChild(makeLinkChip(link)));
    others.forEach((link) => {
      lead(roleLabel(link));
      container.appendChild(makeLinkChip(link));
    });
    return container;
  }

  function makeSubjects(entry) {
    const subjects = entryLinks(entry, (link) => link.role === "subject");
    if (!subjects.length) return null;
    const container = document.createElement("div");
    container.className = "ledger-subjects";
    subjects.forEach((link) => {
      const href = safeHref(link.url);
      const line = document.createElement(href ? "a" : "span");
      line.className = "ledger-subject";
      line.appendChild(icon(link.type === "pull" ? "pull" : link.type === "issue" ? "issue" : "record"));
      const key = document.createElement("span");
      key.className = "ledger-subject-key";
      key.textContent = String(link.label || link.key || "Subject");
      line.appendChild(key);
      const subjectTitle = String(link.title || "").trim();
      if (subjectTitle) {
        const titleElement = document.createElement("span");
        titleElement.className = href
          ? "ledger-subject-title"
          : "ledger-subject-title ledger-subject-title-static";
        titleElement.textContent = subjectTitle;
        line.appendChild(titleElement);
        line.title = subjectTitle;
      }
      if (href) {
        openOutside(line, href);
        line.setAttribute("aria-label", `Open ${subjectTitle || key.textContent}`);
      }
      container.appendChild(line);
    });
    return container;
  }

  function statusTone(entry) {
    const status = String(entry.status || "").toLowerCase();
    if (FAILED_STATUSES.has(status)) return "failed";
    if (isExpectedExpense(entry)) return status === "missing_receipt" ? "pending" : "neutral";
    if (entry.transactionId || status === "completed" || status === "paid" || status === "confirmed") {
      return "settled";
    }
    return PENDING_STATUSES.has(status) ? "pending" : "neutral";
  }

  function recordSearchText(entry) {
    return [
      entry.id,
      entry.category,
      entry.title,
      entry.status,
      entry.direction,
      entry.verification,
      entry.transactionId,
      entry.sourceTable,
      entry.sourceId,
      entry.amountSol,
      entry.amountUsd,
      entry.note,
      ...(Array.isArray(entry.links)
        ? entry.links.flatMap((link) => [
          link?.type,
          link?.role,
          link?.key,
          link?.label,
          link?.title,
          link?.summary,
          link?.discordUsername,
          link?.discordDisplayName,
        ])
        : []),
    ].filter(Boolean).join(" ").toLowerCase();
  }

  function usdBasis(entry) {
    if (entry.amountKnown === false) return "legacy amount unavailable";
    if (entry.valuationBasis === "recorded-fiat-amount") return "recorded amount";
    if (entry.valuationBasis === "monthly-history-estimate") return "estimate from past monthly charges";
    if (entry.valuationBasis === "recorded-at-payment-sol-usd") return "value at payment";
    if (entry.valuationBasis === "payment-sol-usd-unavailable") return "payment-time quote unavailable";
    if (entry.amountUsd === null || entry.amountUsd === undefined) return "quote unavailable";
    return "current spot estimate";
  }

  function solAmountText(entry) {
    if (entry.currency !== "SOL") return "";
    return entry.amountKnown !== false ? `${formatSol(entry.amountSol)} SOL` : "Not recorded";
  }

  function usdSummary(entry) {
    if (entry.amountKnown === false) return "Amount not recorded";
    if (isExpectedExpense(entry)) return "Estimate · not in totals";
    if (entry.currency !== "SOL") return "Recorded amount";
    const usd = entry.amountUsd;
    if (usd === null || usd === undefined || usd === "") {
      return entry.valuationBasis === "payment-sol-usd-unavailable"
        ? "No USD quote at payment"
        : "USD quote unavailable";
    }
    return entry.valuationBasis === "recorded-at-payment-sol-usd"
      ? `${formatUsd(usd)} at payment`
      : `≈ ${formatUsd(usd)} today`;
  }

  function detailItem(term, content, wide = false) {
    const item = document.createElement("div");
    item.className = wide ? "ledger-detail is-wide" : "ledger-detail";
    const name = document.createElement("dt");
    name.textContent = term;
    const value = document.createElement("dd");
    value.append(...(Array.isArray(content) ? content : [content]));
    item.append(name, value);
    return item;
  }

  function textLine(text, tag = "span") {
    const element = document.createElement(tag);
    element.textContent = text;
    return element;
  }

  function chipGroup(links) {
    const group = document.createElement("div");
    group.className = "ledger-links";
    links.forEach((link) => group.appendChild(makeLinkChip(link)));
    return group;
  }

  function makeDetails(entry, detailsId, noteShown) {
    const panel = document.createElement("div");
    panel.className = "ledger-details";
    panel.id = detailsId;
    panel.hidden = true;
    const list = document.createElement("dl");
    list.className = "ledger-details-grid";

    if (entry.transactionId) {
      const href = safeHref(entry.explorerUrl);
      const transaction = document.createElement(href ? "a" : "div");
      transaction.className = "ledger-signature";
      const signature = document.createElement("code");
      signature.textContent = String(entry.transactionId);
      transaction.appendChild(signature);
      if (href) {
        transaction.href = href;
        transaction.target = "_blank";
        transaction.rel = "noopener noreferrer";
        transaction.title = "Open this transaction in Solana Explorer";
        const network = document.createElement("small");
        network.textContent = `${String(entry.network || "mainnet-beta")} · open in Solana Explorer ↗`;
        transaction.appendChild(network);
      }
      list.appendChild(detailItem("Transaction ID", transaction, true));
    } else {
      list.appendChild(detailItem("Transaction ID", textLine(
        entry.currency === "SOL"
          ? "No on-chain transaction ID recorded yet."
          : "External accounting record; no Solana transaction expected.",
      )));
    }

    const amountLines = [];
    if (entry.currency === "SOL") {
      amountLines.push(textLine(entry.amountKnown !== false
        ? `${solAmountText(entry)} · ${exactInteger(entry.amountLamports).toLocaleString()} lamports`
        : "Legacy receipt amount unavailable"));
    } else {
      amountLines.push(textLine(`${Number(entry.amountMinor || 0).toLocaleString()} ${entry.currency || "units"} minor units`));
    }
    const usd = entry.amountKnown === false ? "—" : formatUsd(entry.amountUsd);
    amountLines.push(textLine(
      `${usd === "—" ? "No USD value" : usd} · ${usdBasis(entry)}`,
      "small",
    ));
    list.appendChild(detailItem("Amount", amountLines));

    list.appendChild(detailItem("Status", [
      textLine(`${capitalized(String(entry.status || "recorded").replaceAll("_", " "))} · ${String(entry.direction || "record")}`),
      textLine(String(entry.verification || "recorded").replaceAll("-", " "), "small"),
    ]));

    const source = document.createElement("code");
    source.textContent = [
      String(entry.sourceTable || "canonical record"),
      String(entry.sourceId || entry.id || "unknown"),
    ].join(" · ");
    list.appendChild(detailItem("Source record", source));

    const wallets = entryLinks(entry, (link) => link.type === "wallet");
    if (wallets.length) list.appendChild(detailItem("Wallets", chipGroup(wallets)));

    const people = entryLinks(entry, (link) => link.type === "user" && discordIdentity(link));
    if (people.length) {
      list.appendChild(detailItem("People", people.map((link) => textLine(
        `${roleLabel(link)} ${link.label || link.key || ""} · ${discordIdentity(link)}`,
      ))));
    }

    const linked = entryLinks(entry, (link) => (
      link.type !== "user" && link.type !== "wallet" && link.role !== "subject"
    ));
    if (linked.length) list.appendChild(detailItem("Linked records", chipGroup(linked)));

    entryLinks(entry, (link) => link.role === "subject" && String(link.summary || "").trim())
      .forEach((link) => {
        list.appendChild(detailItem(
          `About ${link.label || "the subject"}`,
          textLine(String(link.summary).trim(), "p"),
          true,
        ));
      });

    if (entry.note && !noteShown) {
      list.appendChild(detailItem("Note", textLine(String(entry.note), "p"), true));
    }

    panel.appendChild(list);
    return panel;
  }

  function makeRow(entry) {
    const row = document.createElement("article");
    row.className = "ledger-row";
    row.id = `record-${encodeURIComponent(String(entry.id || "record"))}`;
    row.dataset.category = categoryGroup(entry.category);
    row.dataset.receipt = receiptGroup(entry);
    row.dataset.pendingPayment = String(isPendingPayment(entry));
    row.dataset.paidPayment = String(isPaidPayment(entry));
    row.dataset.accountingExpense = String(isAccountingExpense(entry));
    row.dataset.accountingIncome = String(isAccountingIncome(entry));
    row.dataset.accountingDebt = String(isAccountingDebt(entry));
    row.dataset.expectedExpense = String(isExpectedExpense(entry));
    row.dataset.search = recordSearchText(entry);

    const main = document.createElement("div");
    main.className = "ledger-row-main";

    const dateCell = document.createElement("div");
    dateCell.className = "ledger-date";
    const formatted = formatDate(entry.occurredAt);
    const time = document.createElement("time");
    time.dateTime = entry.occurredAt
      ? new Date(Number(entry.occurredAt)).toISOString()
      : "";
    time.textContent = formatted.date;
    const clock = document.createElement("small");
    clock.textContent = isExpectedExpense(entry) ? "Estimated date" : formatted.time;
    dateCell.append(time, clock);

    const recordCell = document.createElement("div");
    recordCell.className = "ledger-record";
    const heading = document.createElement("div");
    heading.className = "ledger-record-title";
    const title = document.createElement("strong");
    title.textContent = String(entry.title || "Financial record");
    const category = document.createElement("span");
    category.className = "ledger-category";
    category.textContent = CATEGORY_LABELS[entry.category] || capitalized(String(entry.category || "record").replaceAll("-", " "));
    heading.append(title, category);
    recordCell.appendChild(heading);
    const parties = makeParties(entry);
    if (parties) recordCell.appendChild(parties);
    const subjects = makeSubjects(entry);
    if (subjects) recordCell.appendChild(subjects);
    const noteShown = Boolean(entry.note && !subjects);
    if (noteShown) {
      const note = textLine(String(entry.note), "p");
      note.className = "ledger-note";
      recordCell.appendChild(note);
    }

    const statusCell = document.createElement("div");
    statusCell.className = "ledger-status";
    const status = document.createElement("span");
    status.className = "ledger-status-pill";
    status.dataset.tone = statusTone(entry);
    status.textContent = capitalized(String(entry.status || "recorded").replaceAll("_", " "));
    const direction = document.createElement("small");
    direction.className = "ledger-direction";
    direction.textContent = capitalized(String(entry.direction || "record"));
    statusCell.append(status, direction);

    const amountCell = document.createElement("div");
    amountCell.className = "ledger-amount";
    const amount = document.createElement("strong");
    amount.textContent = entry.currency === "SOL"
      ? solAmountText(entry)
      : entry.amountKnown === false ? "Not recorded"
        : `${isExpectedExpense(entry) ? "≈ " : ""}${formatUsd(entry.amountUsd)}`;
    const value = document.createElement("small");
    value.textContent = usdSummary(entry);
    amountCell.append(amount, value);

    const receiptCell = document.createElement("div");
    receiptCell.className = "ledger-receipt";
    const explorer = safeHref(entry.explorerUrl);
    if (entry.transactionId) {
      const receipt = document.createElement(explorer ? "a" : "span");
      receipt.className = "ledger-receipt-link";
      receipt.title = String(entry.transactionId);
      const short = document.createElement("span");
      short.textContent = shortSignature(entry.transactionId);
      receipt.appendChild(short);
      if (explorer) {
        receipt.href = explorer;
        receipt.target = "_blank";
        receipt.rel = "noopener noreferrer";
        receipt.setAttribute("aria-label", `Open transaction ${entry.transactionId} in Solana Explorer`);
        receipt.appendChild(icon("external"));
      }
      const network = document.createElement("small");
      network.textContent = String(entry.network || "mainnet-beta");
      receiptCell.append(receipt, network);
    } else {
      const empty = document.createElement("span");
      empty.className = "ledger-no-transaction";
      empty.textContent = entry.currency === "SOL" ? "No receipt yet" : "Off-chain record";
      empty.title = entry.currency === "SOL"
        ? "No on-chain transaction ID recorded yet."
        : "External accounting record; no Solana transaction expected.";
      receiptCell.appendChild(empty);
    }

    const detailsId = `${row.id}-details`;
    const details = makeDetails(entry, detailsId, noteShown);
    const toggle = document.createElement("button");
    toggle.type = "button";
    toggle.className = "ledger-toggle";
    toggle.setAttribute("aria-expanded", "false");
    toggle.setAttribute("aria-controls", detailsId);
    toggle.setAttribute("aria-label", `Show details for ${title.textContent}`);
    toggle.appendChild(icon("chevron"));
    toggle.addEventListener("click", () => {
      const open = details.hidden;
      details.hidden = !open;
      toggle.setAttribute("aria-expanded", String(open));
      toggle.setAttribute("aria-label", `${open ? "Hide" : "Show"} details for ${title.textContent}`);
      row.classList.toggle("is-open", open);
    });
    main.addEventListener("click", (event) => {
      if (event.target.closest("a, button")) return;
      if (String(window.getSelection?.() || "")) return;
      toggle.click();
    });

    const meta = document.createElement("div");
    meta.className = "ledger-row-meta";
    meta.append(dateCell, statusCell, receiptCell);
    main.append(recordCell, amountCell, meta, toggle);
    row.append(main, details);
    return row;
  }

  function matchesFilters(row) {
    const viewMatches = state.view === "all"
      || (state.view === "expenses" && row.dataset.accountingExpense === "true")
      || (state.view === "income" && row.dataset.accountingIncome === "true")
      || (state.view === "debt" && row.dataset.accountingDebt === "true")
      || (state.view === "expected" && row.dataset.expectedExpense === "true")
      || (state.view === "pending" && row.dataset.pendingPayment === "true")
      || (state.view === "paid" && row.dataset.paidPayment === "true");
    const categoryMatches = state.category === "all" || row.dataset.category === state.category;
    const receiptMatches = state.receipt === "all" || row.dataset.receipt === state.receipt;
    const searchMatches = !state.search || String(row.dataset.search || "").includes(state.search);
    return viewMatches && categoryMatches && receiptMatches && searchMatches;
  }

  function applyFilters() {
    let visible = 0;
    rows.querySelectorAll(".ledger-row").forEach((row) => {
      row.hidden = !matchesFilters(row);
      if (!row.hidden) visible += 1;
    });
    rows.querySelector("[data-ledger-filter-empty]")?.remove();
    if (!visible && state.entries.length) {
      const empty = document.createElement("p");
      empty.className = "ledger-empty";
      empty.dataset.ledgerFilterEmpty = "true";
      empty.textContent = "No loaded financial records match those filters.";
      rows.appendChild(empty);
    }
    const total = state.entries.length;
    const expenses = state.entries.filter(isAccountingExpense).length;
    const income = state.entries.filter(isAccountingIncome).length;
    const debt = state.entries.filter(isAccountingDebt).length;
    const expected = state.entries.filter(isExpectedExpense).length;
    const pending = state.entries.filter(isPendingPayment).length;
    const paid = state.entries.filter(isPaidPayment).length;
    setText("[data-ledger-expense-count]", expenses.toLocaleString());
    setText("[data-ledger-income-count]", income.toLocaleString());
    setText("[data-ledger-debt-count]", debt.toLocaleString());
    setText("[data-ledger-expected-count]", expected.toLocaleString());
    setText("[data-ledger-pending-count]", pending.toLocaleString());
    setText("[data-ledger-paid-count]", paid.toLocaleString());
    setText("[data-ledger-status]", state.loading
      ? "Loading public records…"
      : `${visible.toLocaleString()} of ${total.toLocaleString()} loaded records shown`);
  }

  function updateSummary() {
    const accounting = state.entries.reduce((totals, entry) => {
      if (isAccountingExpense(entry)) addAccountingAmount(totals.expenses, entry);
      if (isAccountingIncome(entry)) addAccountingAmount(totals.income, entry);
      if (isAccountingDebt(entry)) addAccountingAmount(totals.debt, entry);
      return totals;
    }, {
      expenses: emptyAccountingTotal(),
      income: emptyAccountingTotal(),
      debt: emptyAccountingTotal(),
    });
    setText("[data-ledger-expenses-total]", formatAccountingTotal(accounting.expenses));
    setText("[data-ledger-income-total]", formatAccountingTotal(accounting.income));
    setText("[data-ledger-debt-total]", formatAccountingTotal(accounting.debt));

    const quote = state.quote || {};
    const quoteSummary = document.querySelector("[data-ledger-quote-summary]");
    const priceNote = document.querySelector("[data-ledger-price-note]");
    if (quote.available) {
      const observed = Number(quote.observedAt) || state.observedAt;
      quoteSummary.textContent = `${formatUsd(quote.price)} per SOL now`;
      priceNote.replaceChildren(
        document.createTextNode(`SOL/USD observed ${new Date(observed).toLocaleString()} · `),
      );
      const source = document.createElement("a");
      source.href = safeHref(quote.sourceUrl);
      source.target = "_blank";
      source.rel = "noopener noreferrer";
      source.textContent = String(quote.source || "price source");
      priceNote.appendChild(source);
    } else {
      quoteSummary.textContent = "SOL/USD quote unavailable";
      priceNote.textContent = "SOL amounts remain exact; USD estimates are temporarily unavailable.";
    }

    const backfill = state.backfill || {};
    const backfillNote = document.querySelector("[data-ledger-backfill-note]");
    if (backfillNote) {
      const sources = Number(backfill.sourceAddresses) || 0;
      const completed = Number(backfill.completedSourceAddresses) || 0;
      const transfers = Number(backfill.discoveredTransfers) || 0;
      backfillNote.textContent = sources
        ? `Historical scan: ${transfers.toLocaleString()} transfers · ${completed.toLocaleString()}/${sources.toLocaleString()} system addresses complete${backfill.running ? " · scanning" : ""}`
        : "Historical scan will begin when a public system address is available.";
    }
  }

  function selectView(view) {
    state.view = view;
    document.querySelectorAll("[data-ledger-view]").forEach((candidate) => {
      candidate.setAttribute(
        "aria-selected",
        String(candidate.dataset.ledgerView === state.view),
      );
    });
    applyFilters();
  }

  function renderExpectedReminder() {
    const reminder = document.querySelector("[data-ledger-expected-reminder]");
    if (!reminder) return;
    const missing = state.expected.filter((item) => item?.status === "missing_receipt");
    const upcoming = state.expected.filter((item) => item?.status === "expected");
    reminder.hidden = !missing.length && !upcoming.length;
    if (reminder.hidden) return;
    setText("[data-ledger-expected-title]", missing.length
      ? `${missing.length.toLocaleString()} monthly ${missing.length === 1 ? "expense has" : "expenses have"} no receipt yet`
      : `${upcoming.length.toLocaleString()} monthly ${upcoming.length === 1 ? "charge is" : "charges are"} expected this month`);
    const items = [...missing, ...upcoming].map((item) => {
      const line = document.createElement("li");
      const minor = Number(item.amountMinor) || 0;
      const amount = item.currency === "USD"
        ? formatUsdMicros(minor * 10_000)
        : `${(minor / 100).toFixed(2)} ${item.currency || ""}`.trim();
      line.textContent = `${item.vendor} · ${item.monthLabel} · about ${amount}${item.status === "expected" ? ` · due ${formatDate(item.expectedAt).date}` : ""}`;
      return line;
    });
    document.querySelector("[data-ledger-expected-list]")?.replaceChildren(...items);
  }

  function render() {
    const fragments = state.entries.map(makeRow);
    if (fragments.length) {
      rows.replaceChildren(...fragments);
    } else if (!state.loading) {
      const empty = document.createElement("p");
      empty.className = "ledger-empty";
      empty.textContent = "No financial records have been recorded yet.";
      rows.replaceChildren(empty);
    }
    list.setAttribute("aria-busy", String(state.loading));
    renderWallets();
    renderExpectedReminder();
    updateSummary();
    applyFilters();
    const more = document.querySelector("[data-ledger-load-more]");
    more.hidden = !state.hasMore;
    more.disabled = state.loading;
    more.textContent = state.loading ? "Loading…" : "Load older records";
  }

  async function load({ append = false, refresh = false } = {}) {
    if (state.loading) return;
    state.loading = true;
    list.setAttribute("aria-busy", "true");
    document.querySelector("[data-ledger-refresh]").disabled = true;
    const more = document.querySelector("[data-ledger-load-more]");
    if (more) more.disabled = true;
    if (!append) setText("[data-ledger-status]", "Loading public records…");
    try {
      const parameters = new URLSearchParams({ limit: "100" });
      // Only an explicit Refresh may cost an on-chain read; an ordinary page
      // load is served from the relay's shared hourly balance cache.
      if (refresh) parameters.set("refresh", "1");
      if (append && state.cursor) {
        parameters.set("beforeAt", String(state.cursor.beforeAt || 0));
        parameters.set("beforeId", String(state.cursor.beforeId || ""));
      }
      const response = await fetch(`/api/ledger?${parameters}`, {
        headers: { accept: "application/json" },
        cache: "no-store",
      });
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      const payload = await response.json();
      if (!payload?.ok || !Array.isArray(payload.entries)) throw new Error("Invalid ledger response");
      const incoming = payload.entries;
      state.quote = payload.solUsd || null;
      state.wallets = Array.isArray(payload.wallets) ? payload.wallets : [];
      const rewardQueue = append ? [] : await loadContributorRewards();
      if (!append) {
        state.memberWallet = await loadMemberWallet({ refresh });
        state.memberWalletLoaded = true;
      }
      // Both feeds describe the same reward, and the dedupe below keeps
      // whichever copy comes first. Each is the better one for a different
      // half of a reward's life: the estimate feed resolves the recipient's
      // current payout address and re-reads "ready" from it, which a cached
      // public page cannot do, while a reward the relay has already settled is
      // published there with its payer, both wallets and the verified receipt.
      // So the estimate feed leads for rewards nothing has paid yet, and
      // stands aside for the ones it has.
      const settled = new Set(
        incoming.filter((entry) => entry?.transactionId)
          .map((entry) => String(entry?.id || "")),
      );
      const combined = append
        ? [
          ...state.entries.filter((entry) => !settled.has(String(entry?.id || ""))),
          ...incoming,
        ]
        : [
          ...rewardQueue.filter((entry) => !settled.has(String(entry?.id || ""))),
          ...incoming,
        ];
      const seen = new Set();
      state.entries = combined.filter((entry) => {
        const id = String(entry?.id || "");
        if (!id || seen.has(id)) return false;
        seen.add(id);
        return true;
      });
      state.entries.sort((left, right) => (
        (Number(right?.occurredAt) || 0) - (Number(left?.occurredAt) || 0)
        || String(right?.id || "").localeCompare(String(left?.id || ""))
      ));
      state.cursor = payload.page?.nextCursor || null;
      state.hasMore = Boolean(payload.page?.hasMore && state.cursor);
      state.backfill = payload.backfill || null;
      if (Array.isArray(payload.expectedExpenses)) state.expected = payload.expectedExpenses;
      state.observedAt = Number(payload.observedAt) || Date.now();
      state.loading = false;
      render();
    } catch (_) {
      state.loading = false;
      list.setAttribute("aria-busy", "false");
      state.memberWalletLoaded = true;
      renderWallets();
      setText("[data-ledger-status]", state.entries.length
        ? "Refresh failed · showing the last loaded records"
        : "The public ledger is temporarily unavailable.");
      if (!state.entries.length) {
        const empty = document.createElement("p");
        empty.className = "ledger-empty";
        empty.textContent = "The public ledger is temporarily unavailable. Please try again.";
        rows.replaceChildren(empty);
      }
    } finally {
      document.querySelector("[data-ledger-refresh]").disabled = false;
      if (more) more.disabled = false;
    }
  }

  document.querySelector("[data-ledger-search]")?.addEventListener("input", (event) => {
    state.search = String(event.currentTarget.value || "").trim().toLowerCase();
    applyFilters();
  });

  document.querySelector("[data-ledger-category]")?.addEventListener("change", (event) => {
    state.category = String(event.currentTarget.value || "all");
    applyFilters();
  });

  document.querySelector("[data-ledger-receipt]")?.addEventListener("change", (event) => {
    state.receipt = String(event.currentTarget.value || "all");
    applyFilters();
  });

  document.querySelectorAll("[data-ledger-view]").forEach((button) => {
    button.addEventListener("click", (event) => {
      selectView(String(event.currentTarget.dataset.ledgerView || "all"));
    });
  });

  document.querySelector("[data-ledger-expected-show]")?.addEventListener("click", () => {
    selectView("expected");
    document.querySelector("[data-ledger-list]")?.scrollIntoView({ behavior: "smooth", block: "start" });
  });

  document.querySelector("[data-ledger-refresh]")?.addEventListener("click", () => {
    state.cursor = null;
    void load({ refresh: true });
  });

  document.querySelector("[data-ledger-load-more]")?.addEventListener("click", () => {
    void load({ append: true });
  });

  void load();
})();
