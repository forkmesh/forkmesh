#!/usr/bin/env python3
"""The Market rating widget's conditional, required improvement form.

Runs app/public/market-review-form.js under node:vm against a small fake DOM
built from the real app/public/market-reviews.html markup, then drives it the
way a person would: click or arrow-key a star, type into the fields, submit.
"""

import json
import re
import shutil
import subprocess
from pathlib import Path

import pytest


APP = Path(__file__).resolve().parents[1]
FORM_JS = APP / "public" / "market-review-form.js"
PAGE_JS = APP / "public" / "market-reviews.js"
PAGE = APP / "public" / "market-reviews.html"

GOOD_PROBLEM = ("The contact import stopped halfway through my CSV file and "
                "dropped every row after the first blank line.")
GOOD_IMPROVEMENT = ("Skip blank lines instead of stopping, then show which "
                    "rows were imported and which ones failed.")

# A fake DOM: every element in the page's review form, keyed by the attribute
# selectors the widget uses. Only what market-review-form.js touches.
_NODE_HARNESS = r"""
const fs = require("fs");
const vm = require("vm");
const [formPath, spec] = [process.argv[1], JSON.parse(process.argv[2])];

class Element {
  constructor(tag, attrs) {
    this.tagName = tag.toUpperCase();
    this.attrs = { ...attrs };
    this.listeners = {};
    this.hidden = "hidden" in this.attrs;
    this.value = "";
    this.textContent = "";
    this.required = false;
    this.disabled = "disabled" in this.attrs;
  }
  getAttribute(name) { return name in this.attrs ? String(this.attrs[name]) : null; }
  setAttribute(name, value) { this.attrs[name] = String(value); }
  removeAttribute(name) { delete this.attrs[name]; }
  hasAttribute(name) { return name in this.attrs; }
  addEventListener(type, fn) { (this.listeners[type] ||= []).push(fn); }
  dispatch(type, extra = {}) {
    const event = { type, target: this, defaultPrevented: false,
      preventDefault() { this.defaultPrevented = true; }, ...extra };
    (this.listeners[type] || []).forEach((fn) => fn(event));
    return event;
  }
  focus() { document.activeElement = this; }
}

const elements = spec.elements.map((item) => new Element(item.tag, item.attrs));
const form = elements.find((node) => "data-review-form" in node.attrs);
const matches = (node, selector) => {
  const match = /^\[([\w-]+)(?:="([^"]*)")?\]$/.exec(selector);
  if (!match) throw new Error("unsupported selector " + selector);
  const [, name, value] = match;
  return name in node.attrs && (value === undefined || node.attrs[name] === value);
};
form.querySelector = (selector) =>
  elements.find((node) => node !== form && matches(node, selector)) || null;
form.querySelectorAll = (selector) =>
  elements.filter((node) => node !== form && matches(node, selector));
const document = { activeElement: null };
const sandbox = { console, document, globalThis: null };
sandbox.globalThis = sandbox;
vm.createContext(sandbox);
vm.runInContext(fs.readFileSync(formPath, "utf8"), sandbox);
const api = sandbox.ForkMeshReviewForm;

const $ = (selector) => form.querySelector(selector);
const field = (name) => $(`[data-field="${name}"]`);
const star = (n) => $(`[data-star="${n}"]`);
const snapshot = () => ({
  detailsHidden: $("[data-review-details]").hidden,
  problemRequired: field("problem").required,
  problemAriaRequired: field("problem").getAttribute("aria-required"),
  improvementRequired: field("improvement").required,
  requirement: $("[data-review-requirement]").textContent,
  starLabel: $("[data-star-label]").textContent,
  checked: [1, 2, 3, 4, 5].map((n) => star(n).getAttribute("aria-checked")),
  tabStops: [1, 2, 3, 4, 5].map((n) => star(n).getAttribute("tabindex")),
  problemCounter: $('[data-counter-for="problem"]').textContent,
  problemCounterMet: $('[data-counter-for="problem"]').getAttribute("data-met"),
  problemError: $('[data-error-for="problem"]').textContent,
  problemErrorHidden: $('[data-error-for="problem"]').hidden,
  problemInvalid: field("problem").getAttribute("aria-invalid"),
  improvementError: $('[data-error-for="improvement"]').textContent,
  submitDisabled: $("[data-review-submit]").disabled,
  status: $("[data-review-status]").textContent,
  focused: document.activeElement ? document.activeElement.attrs["data-field"]
    || document.activeElement.attrs["data-star"] || "" : "",
});

const submitted = [];
const widget = api.mount(form, { onSubmit: (payload) => { submitted.push(payload); return payload; } });
const steps = {};
steps.initial = snapshot();

star(3).dispatch("click");
steps.threeStars = snapshot();

form.dispatch("submit");
steps.emptySubmit = { ...snapshot(), submitted: submitted.length };

field("problem").value = "It is bad.";
field("problem").dispatch("input");
field("problem").dispatch("blur");
steps.shortProblem = snapshot();

field("problem").value = "aaaaaaa bbbbbbbb broken broken thing thing everywhere now ok";
field("problem").dispatch("input");
steps.lowEffortProblem = snapshot();

field("problem").value = spec.problem;
field("problem").dispatch("input");
field("improvement").value = spec.improvement;
field("improvement").dispatch("input");
field("category").value = "bug";
steps.filled = snapshot();

form.dispatch("submit");
steps.validSubmit = { ...snapshot(), submitted: submitted.slice() };

// Keyboard: ArrowRight from 3 stars reaches 5; the form stops requiring text.
star(3).dispatch("keydown", { key: "ArrowRight" });
star(4).dispatch("keydown", { key: "ArrowRight" });
field("problem").value = "";
field("improvement").value = "";
field("problem").dispatch("input");
steps.fiveStars = snapshot();
form.dispatch("submit");
steps.fiveSubmit = { submitted: submitted.slice(-1)[0], count: submitted.length };

// Home jumps back to one star and the fields are required again.
star(5).dispatch("keydown", { key: "Home" });
steps.home = snapshot();

widget.showServerErrors({ improvement: "too_few_words" });
steps.serverErrors = snapshot();

steps.rules = {
  required4: api.reviewRequired(4), required5: api.reviewRequired(5),
  words: api.countWords(spec.problem),
  issues: [
    api.fieldIssue("short", true),
    api.fieldIssue("nooooooo this is not what I wanted from this app at all", true),
    api.fieldIssue("bad bad bad bad bad bad bad bad bad bad bad bad bad bad", true),
    api.fieldIssue(spec.problem, true),
    api.fieldIssue("", false),
  ],
};
process.stdout.write(JSON.stringify(steps));
"""


def _form_elements():
    """Every element of the page's review form, from the real markup."""
    html = PAGE.read_text(encoding="utf-8")
    start = html.index("<form class=\"mr-form\" data-review-form")
    form_html = html[start:html.index("</form>", start)]
    elements = []
    for match in re.finditer(r"<(\w+)\s([^<>]*?)/?>", form_html):
        tag, raw = match.group(1), match.group(2)
        attrs = {}
        for attr in re.finditer(r'([\w-]+)(?:="([^"]*)")?', raw):
            attrs[attr.group(1)] = attr.group(2) if attr.group(2) is not None else ""
        if any(name.startswith("data-") for name in attrs):
            elements.append({"tag": tag, "attrs": attrs})
    return elements


@pytest.fixture(scope="module")
def steps():
    node = shutil.which("node")
    if node is None:
        pytest.skip("node is not installed")
    spec = {"elements": _form_elements(), "problem": GOOD_PROBLEM,
            "improvement": GOOD_IMPROVEMENT}
    result = subprocess.run(
        [node, "-e", _NODE_HARNESS, str(FORM_JS), json.dumps(spec)],
        capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, result.stderr
    return json.loads(result.stdout)


def test_the_page_markup_carries_every_widget_hook():
    kinds = {next(k for k in e["attrs"] if k.startswith("data-"))
             for e in _form_elements()}
    for hook in ("data-review-form", "data-star-group", "data-star",
                 "data-review-details", "data-review-requirement",
                 "data-field", "data-counter-for", "data-error-for",
                 "data-review-submit", "data-review-status", "data-star-label"):
        assert hook in kinds, hook
    html = PAGE.read_text(encoding="utf-8")
    assert 'role="radiogroup"' in html
    assert html.count('role="radio"') == 5
    assert 'aria-live="polite" data-review-requirement' in html
    assert "/market-review-form.js?v=${version}" in html
    assert "/market-reviews.js?v=${version}" in html


def test_nothing_is_required_or_shown_before_a_star_is_chosen(steps):
    initial = steps["initial"]
    assert initial["detailsHidden"] is True
    assert initial["problemRequired"] is False
    assert initial["submitDisabled"] is True
    assert initial["checked"] == ["false"] * 5
    assert initial["tabStops"] == ["0", "-1", "-1", "-1", "-1"]
    assert initial["starLabel"] == "No rating selected"


def test_fewer_than_five_stars_immediately_expands_the_required_form(steps):
    three = steps["threeStars"]
    assert three["detailsHidden"] is False
    assert three["problemRequired"] is True
    assert three["improvementRequired"] is True
    assert three["problemAriaRequired"] == "true"
    assert "need a short review" in three["requirement"]
    assert three["checked"] == ["false", "false", "true", "false", "false"]
    assert three["tabStops"] == ["-1", "-1", "0", "-1", "-1"]
    assert three["starLabel"] == "3 stars selected"
    assert three["problemCounter"] == "0/40 characters · 0/8 words"
    assert three["problemCounterMet"] == "false"
    assert three["submitDisabled"] is False
    # No error shouts at the person before they have typed or submitted.
    assert three["problemErrorHidden"] is True


def test_submitting_without_the_review_is_blocked_inline(steps):
    empty = steps["emptySubmit"]
    assert empty["submitted"] == 0
    assert empty["problemError"] == "Required for ratings under 5 stars."
    assert empty["problemInvalid"] == "true"
    assert empty["improvementError"] == "Required for ratings under 5 stars."
    assert empty["status"] == "Please fix the highlighted fields."
    assert empty["focused"] == "problem"


def test_inline_validation_and_counters_follow_typing(steps):
    short = steps["shortProblem"]
    assert short["problemError"] == "Please write at least 40 characters."
    assert short["problemCounter"] == "10/40 characters · 3/8 words"
    low = steps["lowEffortProblem"]
    assert low["problemError"] == "Please remove the repeated characters."
    filled = steps["filled"]
    assert filled["problemErrorHidden"] is True
    assert filled["problemInvalid"] is None
    assert filled["problemCounterMet"] == "true"


def test_a_complete_review_submits_the_structured_payload(steps):
    payload = steps["validSubmit"]["submitted"][0]
    assert payload == {
        "stars": 3, "problem": GOOD_PROBLEM, "improvement": GOOD_IMPROVEMENT,
        "context": "", "comment": "", "category": "bug", "version": "",
    }


def test_five_stars_makes_the_review_optional_and_keyboard_works(steps):
    five = steps["fiveStars"]
    assert five["checked"] == ["false", "false", "false", "false", "true"]
    assert five["focused"] == "5"
    assert five["problemRequired"] is False
    assert five["problemAriaRequired"] == "false"
    assert five["requirement"] == "Thanks! A review is optional for 5 stars."
    assert five["problemErrorHidden"] is True
    assert steps["fiveSubmit"]["submitted"]["stars"] == 5
    assert steps["fiveSubmit"]["submitted"]["problem"] == ""
    home = steps["home"]
    assert home["checked"][0] == "true" and home["problemRequired"] is True


def test_server_field_errors_land_on_the_same_inline_slots(steps):
    assert steps["serverErrors"]["improvementError"] == (
        "Please use at least 8 words.")


def test_client_rules_mirror_the_server_rules(steps):
    rules = steps["rules"]
    assert rules["required4"] is True and rules["required5"] is False
    assert rules["words"] >= 8
    assert rules["issues"] == [
        "too_short", "repeated_characters", "repetitive", "", ""]


def test_page_script_never_renders_review_text_as_markup():
    source = PAGE_JS.read_text(encoding="utf-8")
    assert "innerHTML" not in source
    assert "insertAdjacentHTML" not in source
    assert "ForkMeshReviewForm.mount(" in source
    assert '"/api/market/reviews/maintained"' in source
    assert "/feedback?" in source
