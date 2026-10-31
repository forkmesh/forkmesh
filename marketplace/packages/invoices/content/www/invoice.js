(() => {
  const TOKEN_RE = /^[0-9a-f]{32}\.[0-9a-f]{24}$/;
  const LABELS = { sent: "Open", partial: "Part paid", overdue: "Overdue", paid: "Paid", void: "Void" };

  function node(selector) { return document.querySelector(selector); }
  function text(selector, value) { const target = node(selector); if (target) target.textContent = value || ""; }
  function status(message) { const target = node("[data-iv-status]"); target.textContent = message; target.hidden = !message; }
  function money(amount, currency) {
    const value = Number(amount || 0) / 100;
    try { return new Intl.NumberFormat(undefined, { style: "currency", currency }).format(value); }
    catch (_) { return currency + " " + value.toFixed(2); }
  }
  function date(stamp) {
    return stamp ? new Date(stamp).toLocaleDateString(undefined, { timeZone: "UTC", day: "numeric", month: "long", year: "numeric" }) : "—";
  }
  function cell(value, numeric) {
    const td = document.createElement("td");
    td.textContent = value;
    if (numeric) td.className = "iv-num";
    return td;
  }
  function row(cells, className) {
    const tr = document.createElement("tr");
    if (className) tr.className = className;
    tr.append(...cells);
    return tr;
  }
  function totalRow(label, value, className) {
    const labelCell = cell(label, true);
    labelCell.colSpan = 3;
    return row([labelCell, cell(value, true)], className);
  }
  function block(name, value) {
    text("[data-iv-" + name + "]", value);
    node("[data-iv-" + name + "-block]").hidden = !value;
  }

  function render(invoice, business) {
    const currency = invoice.currency || "USD";
    document.title = "Invoice " + invoice.number + " from " + business.name;
    text("[data-iv-business]", business.name);
    text("[data-iv-business-address]", business.address);
    text("[data-iv-business-email]", business.email);
    text("[data-iv-number]", invoice.number);
    const badge = node("[data-iv-badge]");
    badge.textContent = invoice.status === "paid" && invoice.paidAt ? "Paid " + date(invoice.paidAt) : LABELS[invoice.status] || invoice.status;
    badge.dataset.tone = invoice.status;
    text("[data-iv-client-name]", invoice.client.name || invoice.client.company);
    text("[data-iv-client-company]", invoice.client.name ? invoice.client.company : "");
    text("[data-iv-client-address]", invoice.client.address);
    text("[data-iv-issued]", date(invoice.issuedAt));
    text("[data-iv-due]", date(invoice.dueAt));
    node("[data-iv-lines]").replaceChildren(...invoice.items.map((item) => row([
      cell(item.description || "Item"),
      cell(String(item.quantity), true),
      cell(money(item.unitAmount, currency), true),
      cell(money(Math.round(item.quantity * item.unitAmount), currency), true),
    ])));
    const totals = [totalRow("Subtotal", money(invoice.subtotal, currency))];
    if (invoice.tax) totals.push(totalRow("Tax (" + invoice.taxRate / 100 + "%)", money(invoice.tax, currency)));
    totals.push(totalRow("Total", money(invoice.total, currency), "iv-grand"));
    if (invoice.paidAmount) totals.push(totalRow("Paid", "−" + money(invoice.paidAmount, currency)));
    node("[data-iv-totals]").replaceChildren(...totals);
    const open = invoice.status !== "paid" && invoice.status !== "void" && invoice.balance > 0;
    node("[data-iv-pay]").hidden = !open;
    text("[data-iv-balance]", money(invoice.balance, currency));
    const pay = node("[data-iv-pay-link]");
    if (open && /^https:\/\//.test(invoice.payUrl || "")) { pay.href = invoice.payUrl; pay.hidden = false; }
    block("instructions", open ? business.paymentInstructions : "");
    block("notes", invoice.notes);
    block("terms", invoice.terms);
    text("[data-iv-footer]", business.footer);
    node("[data-iv-sheet]").hidden = false;
    node("[data-iv-actions]").hidden = false;
    node("[data-iv-print]").addEventListener("click", () => window.print());
    status(invoice.status === "void" ? "This invoice was voided and no longer needs paying." : "");
  }

  async function load() {
    const token = String(new URLSearchParams(location.search).get("t") || "");
    if (!TOKEN_RE.test(token)) return status("Open the link in your invoice email to see the invoice here.");
    let payload = null;
    try {
      const response = await fetch("/api/invoices/-/public/" + token, { headers: { accept: "application/json" }, credentials: "omit", cache: "no-store" });
      if (response.status === 404) return status("This invoice is no longer available, or the link is incomplete.");
      payload = await response.json();
      if (!response.ok || !payload?.invoice) throw new Error("HTTP " + response.status);
    } catch (_) {
      return status("Could not load the invoice. Try again in a moment.");
    }
    render(payload.invoice, payload.business || {});
  }

  void load();
})();
