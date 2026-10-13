import { wrapCanvasText } from "./boards.js";
import { canvasTexture, roundedRect } from "./canvas.js";
import { clamp } from "./object-utils.js";

export function worldQaCardPainter(snapshot = {}) {
  const current = snapshot?.current && typeof snapshot.current === "object"
    ? snapshot.current
    : null;
  const view = ["detail", "pass", "fail", "unsure"].includes(String(snapshot?.view || ""))
    ? String(snapshot.view)
    : "cards";
  const list = (Array.isArray(snapshot?.list) ? snapshot.list : []).slice(0, 5);
  const selectedKey = String(snapshot?.selectedKey || "");
  const page = Math.max(0, Number(snapshot?.page) || 0);
  const pages = Math.max(1, Number(snapshot?.pages) || 1);
  const canRoute = snapshot?.canRoute === true;
  const authorized = snapshot?.authorized === true;
  const stats = snapshot?.stats && typeof snapshot.stats === "object"
    ? snapshot.stats
    : {};
  const globalStats =
    snapshot?.globalStats && typeof snapshot.globalStats === "object"
      ? snapshot.globalStats
      : {};
  const currentGlobal =
    current?.global && typeof current.global === "object"
      ? current.global
      : {};
  const currentFailed =
    current?.verdict === "fail" ||
    (view === "detail" && current?.taskQaStatus === "failed");
  const total = Math.max(0, Number(stats.total) || 0);
  const currentIndex = Math.max(0, Number(snapshot?.currentIndex) || 0);
  return (context) => {
    context.fillStyle = currentFailed ? "#381719" : "#2b1f17";
    context.fillRect(0, 0, 1200, 1050);
    context.strokeStyle = currentFailed ? "#ff4c55" : "#a5764d";
    context.lineWidth = 8;
    for (let y = 10; y < 1050; y += 30) {
      context.beginPath();
      context.moveTo(0, y);
      context.bezierCurveTo(300, y - 7, 860, y + 8, 1200, y - 2);
      context.stroke();
    }
    context.fillStyle = currentFailed ? "#fff0ed" : "#f4e8ce";
    roundedRect(context, 62, 52, 1076, 782, 42);
    context.fill();
    context.strokeStyle = currentFailed ? "#e33d48" : "#111d1a";
    context.lineWidth = 10;
    context.stroke();
    context.fillStyle = "#14392e";
    context.font = '900 48px "ForkMesh Mono", ui-monospace, monospace';
    context.textAlign = "center";
    context.fillText("24-HOUR QA DECK", 600, 128);
    context.fillStyle = "#396b5b";
    context.font = '800 25px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText(
      view === "cards" || view === "detail"
        ? current
          ? view === "detail"
            ? "QA RESULT DETAIL · REVIEW OR RETURN"
            : `CARD ${Math.min(total, currentIndex + 1)} OF ${total} · ONE AT A TIME`
          : authorized
            ? "NO QA TASKS ARE WAITING"
            : "SIGN IN TO WORK ON QA"
        : `${view.toUpperCase()} HISTORY · PAGE ${page + 1}/${pages}`,
      600,
      174,
    );
    context.strokeStyle = "#9e7652";
    context.lineWidth = 3;
    context.beginPath();
    context.moveTo(120, 204);
    context.lineTo(1080, 204);
    context.stroke();
    if (view === "cards" || view === "detail") {
      context.fillStyle = "#16211d";
      context.textAlign = "left";
      context.font = '900 48px "ForkMesh Mono", ui-monospace, monospace';
      wrapCanvasText(
        context,
        String(current?.title || "Open the QA deck"),
        120,
        248,
        960,
        58,
        3,
      );
      context.fillStyle = "#2f5a4d";
      context.font = '900 25px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText("HOW TO TEST", 120, 438);
      context.fillStyle = "#1f2c27";
      context.font = '600 30px "ForkMesh Mono", ui-monospace, monospace';
      wrapCanvasText(
        context,
        String(
          current?.howToTest ||
            "Tap this board to review the recent work one card at a time.",
        ),
        120,
        486,
        960,
        42,
        6,
      );
      context.fillStyle = "#365c50";
      context.font = '800 23px "ForkMesh Mono", ui-monospace, monospace';
      context.textAlign = "center";
      context.fillText(
        current?.verdict
          ? current.verdict === "fail" && current?.failureReason
            ? `FAIL REASON · ${String(current.failureReason).slice(0, 54)}${
                current?.hasFailureScreenshot ? " · SCREENSHOT" : ""
              }`
            : `YOUR RESULT · ${String(current.verdict).toUpperCase()}`
          : `THIS CARD · ${Number(currentGlobal.pass) || 0} PASS · ${
              Number(currentGlobal.fail) || 0
            } FAIL · ${Number(currentGlobal.unsure) || 0} UNSURE`,
        600,
        view === "detail" ? 684 : 810,
      );
      if (current?.organizationTask && current?.lastReviewer) {
        context.fillStyle = "#365c50";
        context.font = '700 19px "ForkMesh Mono", ui-monospace, monospace';
        context.fillText(
          `LATEST TASK QA · ${String(
            current.taskQaStatus || "unknown",
          ).toUpperCase()} · @${String(current.lastReviewer).slice(0, 32)}`,
          600,
          view === "detail" ? 646 : 774,
        );
      }
      if (view === "detail") {
        [
          { action: "fail", label: "← FAIL", x: 100, color: "#b63b3f" },
          { action: "unsure", label: "↓ UNSURE", x: 355, color: "#68736e" },
          { action: "pass", label: "PASS →", x: 610, color: "#1a7f37" },
          { action: "back", label: "BACK TO CARDS", x: 865, color: "#315b52" },
        ].forEach((control) => {
          context.fillStyle = control.color;
          roundedRect(context, control.x, 720, 235, 76, 12);
          context.fill();
          context.fillStyle = "#effff7";
          context.font = '900 21px "ForkMesh Mono", ui-monospace, monospace';
          context.fillText(control.label, control.x + 117.5, 760);
        });
      }
    } else {
      context.textAlign = "left";
      if (!list.length) {
        context.fillStyle = "#365c50";
        context.font = '800 32px "ForkMesh Mono", ui-monospace, monospace';
        context.fillText(`NO ${view.toUpperCase()} RESULTS YET`, 120, 300);
        context.font = '600 25px "ForkMesh Mono", ui-monospace, monospace';
        context.fillText("Use CARDS to test recent work.", 120, 350);
      }
      list.forEach((card, index) => {
        const y = 230 + index * 98;
        const selected = String(card?.key || "") === selectedKey;
        const failed =
          String(card?.verdict || "") === "fail" ||
          String(card?.taskQaStatus || "") === "failed";
        context.fillStyle = failed
          ? "#ffe8e6"
          : selected ? "#d7f5e5" : "#eadfc6";
        roundedRect(context, 105, y, 990, 82, 12);
        context.fill();
        context.strokeStyle = failed ? "#e33d48" : selected ? "#168a4d" : "#b89b73";
        context.lineWidth = failed || selected ? 5 : 2;
        context.stroke();
        context.fillStyle = "#16211d";
        context.font = '800 25px "ForkMesh Mono", ui-monospace, monospace';
        context.fillText(
          String(card?.title || "QA task").slice(0, 62),
          130,
          y + 35,
        );
        context.fillStyle = "#47675b";
        context.font = '700 19px "ForkMesh Mono", ui-monospace, monospace';
        context.fillText(
          `${Number(card?.global?.pass) || 0} PASS · ${
            Number(card?.global?.fail) || 0
          } FAIL · ${Number(card?.global?.unsure) || 0} UNSURE`,
          130,
          y + 66,
        );
      });
      context.textAlign = "center";
      context.font = '800 22px "ForkMesh Mono", ui-monospace, monospace';
      context.fillStyle = "#365c50";
      context.fillText("◀ PREV", 175, 758);
      context.fillText("NEXT ▶", 1025, 758);
      if (selectedKey && canRoute) {
        context.fillStyle = "#315f91";
        roundedRect(context, 460, 720, 280, 70, 12);
        context.fill();
        context.fillStyle = "#effff7";
        context.fillText("SEND TO ISSUES", 600, 764);
      } else if (selectedKey) {
        context.fillText("OWNER / MAINTAIN ACCESS REQUIRED TO ROUTE", 600, 810);
      } else {
        context.fillText("SELECT A TASK FOR ROUTING ACTIONS", 600, 810);
      }
    }

    const tabData = [
      { key: "cards", label: "CARDS", count: total, color: "#315b52" },
      { key: "pass", label: "PASS", count: globalStats.pass, color: "#1a7f37" },
      { key: "fail", label: "FAIL", count: globalStats.fail, color: "#b63b3f" },
      { key: "unsure", label: "UNSURE", count: globalStats.unsure, color: "#68736e" },
    ];
    tabData.forEach((tab, index) => {
      const x = 70 + index * 266;
      context.fillStyle = view === tab.key ? tab.color : "#10251e";
      roundedRect(context, x, 872, 252, 100, 18);
      context.fill();
      context.strokeStyle = tab.color;
      context.lineWidth = view === tab.key ? 6 : 3;
      context.stroke();
      context.fillStyle = "#effff7";
      context.textAlign = "center";
      context.font = '900 25px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText(
        `${tab.label} ${Number(tab.count) || 0}`,
        x + 126,
        932,
      );
    });
    context.fillStyle = "#c9e7da";
    context.font = '700 18px "ForkMesh Mono", ui-monospace, monospace';
    context.textAlign = "center";
    context.fillText(
      `${Number(globalStats.testers) || 0} TESTERS · ${total} CARDS WAITING${
        view === "cards" ? " · GRAB + SWIPE" : " · SHARED HISTORY"
      }`,
      600,
      1010,
    );
  };
}

export function worldQaCardTexture(THREE, snapshot = {}) {
  return canvasTexture(THREE, 1200, 1050, worldQaCardPainter(snapshot));
}

export function qaBoardHitAction(uv, snapshot = {}) {
  if (!uv) return null;
  const x = clamp(Number(uv.x) || 0, 0, 1) * 1200;
  const y = (1 - clamp(Number(uv.y) || 0, 0, 1)) * 1050;
  if (y >= 872 && y <= 972) {
    const index = Math.floor((x - 70) / 266);
    const views = ["cards", "pass", "fail", "unsure"];
    if (index >= 0 && index < views.length) {
      return { action: "tab", view: views[index] };
    }
  }
  const view = String(snapshot?.view || "cards");
  if (view === "detail" && y >= 720 && y <= 796) {
    if (x >= 100 && x <= 335) return { action: "verdict", verdict: "fail" };
    if (x >= 355 && x <= 590) return { action: "verdict", verdict: "unsure" };
    if (x >= 610 && x <= 845) return { action: "verdict", verdict: "pass" };
    if (x >= 865 && x <= 1100) return { action: "back" };
  }
  if (view === "cards") return { action: "swipe" };
  if (y >= 230 && y <= 720) {
    const index = Math.floor((y - 230) / 98);
    const card = (Array.isArray(snapshot?.list) ? snapshot.list : [])[index];
    if (card?.key) {
      return { action: "select", key: String(card.key) };
    }
  }
  if (y >= 720 && y <= 800) {
    if (x >= 110 && x <= 240) return { action: "page", delta: -1 };
    if (x >= 960 && x <= 1090) return { action: "page", delta: 1 };
    if (x >= 460 && x <= 740) return { action: "route", target: "issues" };
  }
  return null;
}

export function worldQaArrowTexture(THREE, direction, label, color) {
  return canvasTexture(THREE, 512, 260, (context) => {
    context.clearRect(0, 0, 512, 260);
    roundedRect(context, 8, 8, 496, 244, 40);
    context.fillStyle = "rgba(5,18,15,0.94)";
    context.fill();
    context.strokeStyle = color;
    context.lineWidth = 10;
    context.stroke();
    context.fillStyle = color;
    context.textAlign = "center";
    context.textBaseline = "middle";
    context.font = '900 116px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText(direction, 150, 128);
    context.font = '900 40px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText(label, 340, 128);
  });
}
