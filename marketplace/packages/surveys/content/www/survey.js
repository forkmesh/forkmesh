(() => {
  const TOKEN_RE = /^[0-9a-f]{32}\.[0-9a-f]{24}$/;
  const token = String(new URLSearchParams(location.search).get("t") || "");
  const answeredKey = "forkmesh-survey-answered:" + token.slice(0, 32);
  let questions = [];

  function node(selector) { return document.querySelector(selector); }
  function text(selector, value) { const target = node(selector); if (target) target.textContent = value || ""; }
  function status(message) { const target = node("[data-sv-status]"); target.textContent = message; target.hidden = !message; }
  function element(tag, className, value) {
    const created = document.createElement(tag);
    if (className) created.className = className;
    if (value !== undefined) created.textContent = value;
    return created;
  }
  function remembered() { try { return localStorage.getItem(answeredKey) === "1"; } catch (_) { return false; } }
  function remember() { try { localStorage.setItem(answeredKey, "1"); } catch (_) { return; } }
  function shuffled(items) {
    const copy = items.slice();
    for (let index = copy.length - 1; index > 0; index -= 1) {
      const swap = Math.floor(Math.random() * (index + 1));
      [copy[index], copy[swap]] = [copy[swap], copy[index]];
    }
    return copy;
  }
  function date(stamp) {
    return new Date(stamp).toLocaleString(undefined, { day: "numeric", month: "long", year: "numeric", hour: "numeric", minute: "2-digit" });
  }
  function heading(question, tag, forId) {
    const label = element(tag, "", question.title);
    if (forId) label.htmlFor = forId;
    if (question.required) {
      const mark = element("span", "sv-required", "*");
      mark.setAttribute("aria-hidden", "true");
      label.append(mark);
    }
    return label;
  }
  function help(question, wrapper) {
    if (!question.help) return;
    const hint = element("p", "sv-help sv-pre", question.help);
    hint.id = "sv-help-" + question.id;
    wrapper.append(hint);
  }
  function choiceQuestion(question, shuffle) {
    const wrapper = element("fieldset", "sv-question");
    wrapper.append(heading(question, "legend"));
    help(question, wrapper);
    const list = element("div", "sv-choices");
    const type = question.type === "single" ? "radio" : "checkbox";
    for (const option of shuffle ? shuffled(question.options) : question.options) {
      const label = element("label", "sv-choice");
      const input = document.createElement("input");
      input.type = type; input.name = "q-" + question.id; input.value = option;
      label.append(input, element("span", "", option));
      list.append(label);
    }
    wrapper.append(list);
    return wrapper;
  }
  function ratingQuestion(question) {
    const wrapper = element("fieldset", "sv-question");
    wrapper.append(heading(question, "legend"));
    help(question, wrapper);
    const scale = element("div", "sv-scale");
    for (let value = 1; value <= question.scale; value += 1) {
      const label = element("label");
      const input = document.createElement("input");
      input.type = "radio"; input.name = "q-" + question.id; input.value = String(value);
      input.setAttribute("aria-label", value + " of " + question.scale);
      label.append(input, element("span", "", String(value)));
      scale.append(label);
    }
    const ends = element("div", "sv-scale-ends");
    ends.append(element("span", "", "Lowest"), element("span", "", "Highest"));
    wrapper.append(scale, ends);
    return wrapper;
  }
  function textQuestion(question) {
    const wrapper = element("div", "sv-question");
    const id = "sv-input-" + question.id;
    wrapper.append(heading(question, "label", id));
    help(question, wrapper);
    const input = document.createElement(question.type === "paragraph" ? "textarea" : "input");
    input.className = "sv-input"; input.id = id; input.name = "q-" + question.id;
    input.maxLength = question.type === "paragraph" ? 5000 : 500;
    if (question.help) input.setAttribute("aria-describedby", "sv-help-" + question.id);
    wrapper.append(input);
    return wrapper;
  }
  function questionNode(question, shuffle) {
    const item = element("li");
    item.dataset.svQuestion = question.id;
    if (question.type === "single" || question.type === "multiple") item.append(choiceQuestion(question, shuffle));
    else if (question.type === "rating") item.append(ratingQuestion(question));
    else item.append(textQuestion(question));
    return item;
  }
  function value(question) {
    const form = node("[data-sv-form]");
    const fields = [...form.querySelectorAll('[name="q-' + question.id + '"]')];
    if (question.type === "multiple") return fields.filter((field) => field.checked).map((field) => field.value);
    if (question.type === "single") return (fields.find((field) => field.checked) || {}).value || "";
    if (question.type === "rating") { const picked = fields.find((field) => field.checked); return picked ? Number(picked.value) : ""; }
    return (fields[0] || {}).value || "";
  }
  function empty(answer) { return answer === "" || (Array.isArray(answer) && !answer.length) || (typeof answer === "string" && !answer.trim()); }
  function mark(questionId) {
    document.querySelectorAll("[data-sv-question]").forEach((item) => {
      const wrapper = item.firstElementChild;
      if (wrapper) wrapper.dataset.invalid = String(item.dataset.svQuestion === questionId);
    });
    const target = document.querySelector('[data-sv-question="' + questionId + '"]');
    if (target) target.scrollIntoView({ behavior: "smooth", block: "center" });
  }
  function error(message, questionId) {
    const target = node("[data-sv-error]");
    target.textContent = message || ""; target.hidden = !message;
    if (questionId) mark(questionId);
  }
  function done(title, message) {
    node("[data-sv-form]").hidden = true;
    text("[data-sv-done-title]", title);
    text("[data-sv-thanks]", message);
    node("[data-sv-done]").hidden = false;
    window.scrollTo({ top: 0 });
  }

  async function submit(event) {
    event.preventDefault();
    const answers = {};
    for (const question of questions) {
      const answer = value(question);
      if (empty(answer)) {
        if (question.required) return error("Please answer “" + question.title + "”.", question.id);
        continue;
      }
      answers[question.id] = answer;
    }
    if (!Object.keys(answers).length) return error("Answer at least one question before sending.");
    error("");
    const button = node("[data-sv-submit]");
    button.disabled = true;
    try {
      const response = await fetch("/api/surveys/-/public/" + token, {
        method: "POST", credentials: "omit", cache: "no-store",
        headers: { accept: "application/json", "content-type": "application/json" },
        body: JSON.stringify({ answers }),
      });
      const payload = await response.json().catch(() => ({}));
      if (response.ok) { remember(); return done("Thank you", payload.thankYou); }
      if (payload.error === "already_responded") { remember(); return done("Already answered", "You have already answered this survey. Thank you."); }
      if (payload.error === "survey_closed") return done("This survey is closed", "It stopped taking answers before yours arrived.");
      if (payload.error === "too_many_responses") return error("Too many answers came from your connection in the last hour. Try again later.");
      if (payload.error === "answer_required") return error("Please answer every question marked *.", payload.question);
      if (payload.error === "invalid_answer") return error("One answer could not be used. Check it and send again.", payload.question);
      if (response.status === 404) return done("Survey not found", "This survey is no longer available.");
      error("Your answers could not be sent. Try again in a moment.");
    } catch (_) {
      error("Your answers could not be sent. Check your connection and try again.");
    } finally {
      button.disabled = false;
    }
  }

  function render(survey) {
    document.title = survey.title;
    text("[data-sv-organization]", survey.organization);
    text("[data-sv-title]", survey.title);
    text("[data-sv-description]", survey.description);
    node("[data-sv-description]").hidden = !survey.description;
    if (survey.status === "scheduled") return status("This survey opens " + (survey.opensAt ? date(survey.opensAt) : "soon") + ".");
    if (survey.status !== "open") return status("This survey is closed and no longer takes answers.");
    if (survey.oneResponsePerPerson && remembered()) return status("You have already answered this survey. Thank you.");
    questions = survey.questions || [];
    node("[data-sv-required-note]").hidden = !questions.some((question) => question.required);
    node("[data-sv-questions]").replaceChildren(...questions.map((question) => questionNode(question, survey.shuffleOptions)));
    node("[data-sv-form]").addEventListener("submit", submit);
    node("[data-sv-form]").hidden = false;
    status("");
  }

  async function load() {
    if (!TOKEN_RE.test(token)) return status("Open the full survey link you were sent to answer it here.");
    let payload = null;
    try {
      const response = await fetch("/api/surveys/-/public/" + token, { headers: { accept: "application/json" }, credentials: "omit", cache: "no-store" });
      if (response.status === 404) return status("This survey is no longer available, or the link is incomplete.");
      payload = await response.json();
      if (!response.ok || !payload?.survey) throw new Error("HTTP " + response.status);
    } catch (_) {
      return status("Could not load the survey. Try again in a moment.");
    }
    render(payload.survey);
  }

  void load();
})();
