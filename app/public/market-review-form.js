// Market rating widget and the conditional improvement-review form.
//
// Mirrors the server rules in app/src/marketplace_reviews.py so the page can
// explain a problem before the round trip; the server still enforces every
// rule. Selecting fewer than five stars expands the required "what didn't
// work" / "how we can improve" fields at once. Five stars keeps them optional.
//
// DOM contract (all lookups are attribute selectors inside `root`):
//   [data-star-group]           role="radiogroup" wrapper
//   [data-star="1".."5"]        role="radio" buttons (roving tabindex)
//   [data-star-label]           live text: "3 stars selected"
//   [data-review-details]       the improvement fields' container
//   [data-review-requirement]   explains required vs optional
//   [data-field="problem"|"improvement"|"context"|"comment"|"category"|"version"]
//   [data-counter-for="<field>"], [data-error-for="<field>"]
//   [data-review-status]        aria-live form status
//   [data-review-submit]        submit button
// Exposed as globalThis.ForkMeshReviewForm (the test runs it under node:vm).
(function (root) {
  "use strict";

  const DEFAULT_RULES = Object.freeze({
    reviewRequiredBelow: 5,
    minChars: 40,
    minWords: 8,
    maxChars: 4000,
    categories: [
      { id: "bug", label: "Bug" },
      { id: "missing-feature", label: "Missing feature" },
      { id: "docs", label: "Docs" },
      { id: "performance", label: "Performance" },
      { id: "ux", label: "UX" },
      { id: "other", label: "Other" },
    ],
  });
  const REQUIRED_FIELDS = ["problem", "improvement"];
  const TEXT_FIELDS = ["problem", "improvement", "context", "comment"];
  const WORD_RE = /[\p{L}\p{M}]+(?:['’-][\p{L}\p{M}]+)*/gu;
  const CJK_RE = /[぀-ヿ㐀-䶿一-鿿豈-﫿]/g;
  const REPEATED_LETTER_RE = /(\p{L})\1{4,}/u;
  const REPEATED_ANY_RE = /(\S)\1{9,}/u;

  function cleanText(value) {
    return String(value || "")
      .replace(/\r\n?/g, "\n")
      .replace(/[\u0000-\u0008\u000b\u000c\u000e-\u001f\u007f]/g, "")
      .split("\n")
      .map((line) => line.split(/\s+/).filter(Boolean).join(" "))
      .join("\n")
      .replace(/\n{3,}/g, "\n\n")
      .trim();
  }

  function words(text) {
    return String(text || "").match(WORD_RE) || [];
  }

  function countWords(text) {
    let total = 0;
    for (const token of words(text)) {
      const cjk = (token.match(CJK_RE) || []).length;
      if (cjk) total += Math.max(1, Math.floor(cjk / 2));
      else if (token.length >= 2) total += 1;
    }
    return total;
  }

  function fieldIssue(text, required, rules = DEFAULT_RULES) {
    if (!text) return required ? "required" : "";
    if (text.length > rules.maxChars) return "too_long";
    if (!required) return "";
    if (REPEATED_LETTER_RE.test(text) || REPEATED_ANY_RE.test(text)) {
      return "repeated_characters";
    }
    if (text.length < rules.minChars) return "too_short";
    if (countWords(text) < rules.minWords) return "too_few_words";
    CJK_RE.lastIndex = 0;
    if (!CJK_RE.test(text)) {
      const list = words(text)
        .filter((word) => word.length >= 2)
        .map((word) => word.toLowerCase());
      const counts = new Map();
      list.forEach((word) => counts.set(word, (counts.get(word) || 0) + 1));
      const most = Math.max(0, ...counts.values());
      if (counts.size < 5 || most * 2 > list.length) return "repetitive";
    }
    CJK_RE.lastIndex = 0;
    return "";
  }

  function reviewRequired(stars, rules = DEFAULT_RULES) {
    const value = Number(stars) || 0;
    return value >= 1 && value < (rules.reviewRequiredBelow || 5);
  }

  // Validate the whole form: { ok, required, errors: {field: code} }.
  function validate(stars, fields, rules = DEFAULT_RULES) {
    const value = Number(stars) || 0;
    const errors = {};
    if (!(value >= 1 && value <= 5)) errors.stars = "required";
    const required = reviewRequired(value, rules);
    const clean = {};
    TEXT_FIELDS.forEach((name) => { clean[name] = cleanText(fields?.[name]); });
    REQUIRED_FIELDS.forEach((name) => {
      const issue = fieldIssue(clean[name], required, rules);
      if (issue) errors[name] = issue;
    });
    ["context", "comment"].forEach((name) => {
      if (clean[name].length > rules.maxChars) errors[name] = "too_long";
    });
    if (
      required && !errors.problem && !errors.improvement &&
      clean.problem.toLowerCase() === clean.improvement.toLowerCase()
    ) {
      errors.improvement = "same_as_problem";
    }
    return { ok: Object.keys(errors).length === 0, required, errors, clean };
  }

  function message(code, rules = DEFAULT_RULES) {
    switch (code) {
      case "required": return "Required for ratings under 5 stars.";
      case "too_short": return `Please write at least ${rules.minChars} characters.`;
      case "too_few_words": return `Please use at least ${rules.minWords} words.`;
      case "repeated_characters": return "Please remove the repeated characters.";
      case "repetitive": return "Please describe it in your own words, not repeated words.";
      case "same_as_problem": return "Tell us what would fix it, not the same text again.";
      case "too_long": return `Please keep this under ${rules.maxChars} characters.`;
      case "invalid": return "Please check this value.";
      default: return code ? "Please check this field." : "";
    }
  }

  function starLabel(stars) {
    const value = Number(stars) || 0;
    return value ? `${value} star${value === 1 ? "" : "s"} selected` : "No rating selected";
  }

  function mount(container, options = {}) {
    const rules = { ...DEFAULT_RULES, ...(options.rules || {}) };
    const $ = (selector) => container.querySelector(selector);
    const stars = Array.from(container.querySelectorAll("[data-star]"));
    const details = $("[data-review-details]");
    const requirement = $("[data-review-requirement]");
    const status = $("[data-review-status]");
    const submit = $("[data-review-submit]");
    const field = (name) => $(`[data-field="${name}"]`);
    const touched = new Set();
    let selected = 0;
    let submitted = false;

    function values() {
      const out = {};
      TEXT_FIELDS.forEach((name) => { out[name] = field(name)?.value || ""; });
      out.category = field("category")?.value || "";
      out.version = field("version")?.value || "";
      return out;
    }

    function paintCounters() {
      const required = reviewRequired(selected, rules);
      TEXT_FIELDS.forEach((name) => {
        const counter = $(`[data-counter-for="${name}"]`);
        if (!counter) return;
        const text = cleanText(field(name)?.value || "");
        const count = countWords(text);
        if (REQUIRED_FIELDS.includes(name) && required) {
          counter.textContent =
            `${text.length}/${rules.minChars} characters · ${count}/${rules.minWords} words`;
          const met = text.length >= rules.minChars && count >= rules.minWords;
          counter.setAttribute("data-met", met ? "true" : "false");
        } else {
          counter.textContent = `${text.length}/${rules.maxChars} characters`;
          counter.setAttribute("data-met", "true");
        }
      });
    }

    function paintErrors() {
      const result = validate(selected, values(), rules);
      [...TEXT_FIELDS, "stars"].forEach((name) => {
        const shown = submitted || touched.has(name);
        const code = shown ? result.errors[name] || "" : "";
        const error = $(`[data-error-for="${name}"]`);
        if (error) {
          error.textContent = message(code, rules);
          error.hidden = !code;
        }
        const input = name === "stars" ? $("[data-star-group]") : field(name);
        if (input) {
          if (code) input.setAttribute("aria-invalid", "true");
          else input.removeAttribute("aria-invalid");
        }
      });
      if (submit) submit.disabled = !selected;
      if (result.ok && status && status.textContent === "Please fix the highlighted fields.") {
        status.textContent = "";
      }
      return result;
    }

    function paintStars() {
      stars.forEach((button) => {
        const value = Number(button.getAttribute("data-star"));
        const checked = value === selected;
        button.setAttribute("aria-checked", checked ? "true" : "false");
        button.setAttribute("data-filled", value <= selected ? "true" : "false");
        // Roving tabindex: the checked star (or the first) is the tab stop.
        const stop = selected ? checked : value === 1;
        button.setAttribute("tabindex", stop ? "0" : "-1");
      });
      const label = $("[data-star-label]");
      if (label) label.textContent = starLabel(selected);
    }

    function paintRequirement() {
      const required = reviewRequired(selected, rules);
      if (details) details.hidden = !selected;
      REQUIRED_FIELDS.forEach((name) => {
        const input = field(name);
        if (!input) return;
        input.required = required;
        input.setAttribute("aria-required", required ? "true" : "false");
      });
      if (requirement) {
        requirement.textContent = !selected
          ? ""
          : required
            ? "Ratings under 5 stars need a short review: tell us what didn't work and how we can improve."
            : "Thanks! A review is optional for 5 stars.";
      }
    }

    function select(value, { focus = false } = {}) {
      const next = Math.max(0, Math.min(5, Number(value) || 0));
      const expanding = reviewRequired(next, rules) && !reviewRequired(selected, rules);
      selected = next;
      touched.add("stars");
      paintStars();
      paintRequirement();
      paintCounters();
      paintErrors();
      if (focus) stars[selected - 1]?.focus?.();
      if (typeof options.onChange === "function") options.onChange(selected, expanding);
    }

    stars.forEach((button) => {
      button.addEventListener("click", () => select(button.getAttribute("data-star"), { focus: true }));
      button.addEventListener("keydown", (event) => {
        const key = event.key;
        let next = 0;
        const current = selected || 0;
        if (key === "ArrowRight" || key === "ArrowUp") next = Math.min(5, current + 1);
        else if (key === "ArrowLeft" || key === "ArrowDown") next = Math.max(1, current - 1);
        else if (key === "Home") next = 1;
        else if (key === "End") next = 5;
        else if (key === " " || key === "Enter") next = Number(button.getAttribute("data-star"));
        if (!next) return;
        event.preventDefault?.();
        select(next, { focus: true });
      });
    });

    TEXT_FIELDS.forEach((name) => {
      const input = field(name);
      if (!input) return;
      input.addEventListener("input", () => { paintCounters(); paintErrors(); });
      input.addEventListener("blur", () => { touched.add(name); paintErrors(); });
    });

    async function onSubmit(event) {
      event?.preventDefault?.();
      submitted = true;
      const result = paintErrors();
      if (!result.ok) {
        const first = ["stars", ...TEXT_FIELDS].find((name) => result.errors[name]);
        if (first === "stars") stars[0]?.focus?.();
        else field(first)?.focus?.();
        if (status) status.textContent = "Please fix the highlighted fields.";
        return null;
      }
      const payload = {
        stars: selected,
        problem: result.clean.problem,
        improvement: result.clean.improvement,
        context: result.clean.context,
        comment: result.clean.comment,
        category: values().category,
        version: values().version.trim(),
      };
      if (typeof options.onSubmit === "function") return options.onSubmit(payload);
      return payload;
    }

    const form = $("[data-review-form]") || container;
    form.addEventListener?.("submit", onSubmit);

    function fill(review) {
      if (!review) return;
      TEXT_FIELDS.forEach((name) => {
        const input = field(name);
        if (input) input.value = review[name] || "";
      });
      if (field("category")) field("category").value = review.category || "";
      if (field("version")) field("version").value = review.version || "";
      select(review.stars || 0);
      touched.clear();
      paintErrors();
    }

    // Server field errors (400 review_invalid) land on the same inline slots.
    function showServerErrors(fields) {
      submitted = true;
      Object.entries(fields || {}).forEach(([name, code]) => {
        const error = $(`[data-error-for="${name}"]`);
        if (error) {
          error.textContent = message(code, rules);
          error.hidden = false;
        }
        const input = field(name);
        if (input) input.setAttribute("aria-invalid", "true");
      });
    }

    paintStars();
    paintRequirement();
    paintCounters();
    paintErrors();
    return {
      select,
      fill,
      submit: onSubmit,
      showServerErrors,
      get stars() { return selected; },
      get required() { return reviewRequired(selected, rules); },
      validate: () => validate(selected, values(), rules),
    };
  }

  root.ForkMeshReviewForm = Object.freeze({
    DEFAULT_RULES,
    cleanText,
    countWords,
    fieldIssue,
    reviewRequired,
    validate,
    message,
    starLabel,
    mount,
  });
})(typeof globalThis !== "undefined" ? globalThis : window);
