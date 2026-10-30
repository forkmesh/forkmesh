  function openImageResizeModal(file, maxBytes, options = {}) {
    return new Promise((resolve) => {
      const outputType = options.outputType || "image/jpeg";
      const aspectRatio = Number(options.aspectRatio) > 0 ? Number(options.aspectRatio) : 0;
      const confirmLabel = options.confirmLabel || "Add image";
      const objectUrl = URL.createObjectURL(file);
      let settled = false;
      const finish = (result) => {
        if (settled) return;
        settled = true;
        document.removeEventListener("keydown", onKeydown);
        URL.revokeObjectURL(objectUrl);
        overlay.remove();
        options.returnFocus?.focus();
        resolve(result);
      };
      const onKeydown = (event) => {
        if (event.key === "Escape") { event.preventDefault(); finish(null); }
      };

      const overlay = document.createElement("div");
      overlay.className = "fixed inset-0 z-50 flex items-start justify-center overflow-auto bg-background/70 p-4 pt-10";
      overlay.setAttribute("role", "dialog");
      overlay.setAttribute("aria-modal", "true");
      overlay.setAttribute("aria-label", options.dialogLabel || "Resize image");
      overlay.innerHTML = `
        <div class="fm-dialog w-full max-w-2xl">
          <div class="fm-dialog-header">
            <span class="fm-dialog-title truncate">${escapeHtml(options.title || `Resize "${file.name}"`)}</span>
          </div>
          <div class="fm-dialog-body grid gap-3">
            <p class="text-xs text-muted-foreground">${escapeHtml(options.description || "Drag on the image to crop it, then it's auto-compressed to fit. Attached images share the issue's size limit - redraw a smaller crop if it still doesn't fit.")}</p>
            <div data-resize-stage class="relative mx-auto inline-block max-h-[24rem] max-w-full touch-none select-none overflow-hidden rounded-md border border-border bg-secondary/30">
              <img data-resize-image src="${objectUrl}" class="block max-h-[24rem] max-w-full select-none" alt="" draggable="false" />
              <div data-resize-crop class="absolute hidden border-2 border-border bg-primary/10"></div>
            </div>
            <div class="flex flex-wrap items-center justify-between gap-2 text-xs">
              <span data-resize-status class="text-muted-foreground">Estimating size…</span>
              <button type="button" data-resize-reset class="fm-btn fm-btn-sm fm-btn-invisible">Reset crop</button>
            </div>
          </div>
          <div class="fm-dialog-footer">
            <button type="button" data-resize-cancel class="fm-btn fm-btn-sm">Cancel</button>
            <button type="button" data-resize-add disabled class="fm-btn fm-btn-sm fm-btn-cta"><i data-lucide="check"></i>${escapeHtml(confirmLabel)}</button>
          </div>
        </div>`;
      document.body.appendChild(overlay);
      window.lucide?.createIcons();
      document.addEventListener("keydown", onKeydown);

      const stage = overlay.querySelector("[data-resize-stage]");
      const modalImg = overlay.querySelector("[data-resize-image]");
      const cropDiv = overlay.querySelector("[data-resize-crop]");
      const status = overlay.querySelector("[data-resize-status]");
      const addButton = overlay.querySelector("[data-resize-add]");
      const resetButton = overlay.querySelector("[data-resize-reset]");
      const cancelButton = overlay.querySelector("[data-resize-cancel]");

      let cropRectCss = null;
      let dragStart = null;
      let pendingResult = null;
      let compressToken = 0;

      const clamp = (value, min, max) => Math.min(Math.max(value, min), max);

      const updateCropVisual = () => {
        if (!cropRectCss) {
          cropDiv.classList.add("hidden");
          return;
        }
        cropDiv.classList.remove("hidden");
        cropDiv.style.left = `${cropRectCss.left}px`;
        cropDiv.style.top = `${cropRectCss.top}px`;
        cropDiv.style.width = `${cropRectCss.width}px`;
        cropDiv.style.height = `${cropRectCss.height}px`;
      };

      const attemptCompress = async () => {
        if (!modalImg.naturalWidth) return;
        const attemptId = (compressToken += 1);
        pendingResult = null;
        addButton.disabled = true;
        status.className = "text-muted-foreground";
        status.textContent = "Compressing…";

        const scaleX = modalImg.naturalWidth / modalImg.clientWidth;
        const scaleY = modalImg.naturalHeight / modalImg.clientHeight;
        let region = { sx: 0, sy: 0, sw: modalImg.naturalWidth, sh: modalImg.naturalHeight };
        if (cropRectCss) {
          region = {
            sx: Math.round(cropRectCss.left * scaleX),
            sy: Math.round(cropRectCss.top * scaleY),
            sw: Math.max(1, Math.round(cropRectCss.width * scaleX)),
            sh: Math.max(1, Math.round(cropRectCss.height * scaleY)),
          };
        }

        const result = await encodeImageWithinBytes(modalImg, region, maxBytes, options,
          () => attemptId === compressToken);
        if (attemptId !== compressToken) return;
        if (result) {
          pendingResult = result;
          status.className = "text-muted-foreground";
          status.textContent = `Ready - ${formatSize(result.size)} (fits under ${formatSize(maxBytes)}).`;
          addButton.disabled = false;
          return;
        }
        status.className = "text-destructive";
        status.textContent = "Still too large after compressing - drag to crop a smaller area and it'll retry.";
      };

      stage.addEventListener("pointerdown", (event) => {
        const rect = stage.getBoundingClientRect();
        dragStart = {
          x: clamp(event.clientX - rect.left, 0, rect.width),
          y: clamp(event.clientY - rect.top, 0, rect.height),
        };
        stage.setPointerCapture(event.pointerId);
      });
      stage.addEventListener("pointermove", (event) => {
        if (!dragStart) return;
        const rect = stage.getBoundingClientRect();
        const x = clamp(event.clientX - rect.left, 0, rect.width);
        const y = clamp(event.clientY - rect.top, 0, rect.height);
        if (aspectRatio) {
          const directionX = x < dragStart.x ? -1 : 1;
          const directionY = y < dragStart.y ? -1 : 1;
          const availableWidth = directionX < 0 ? dragStart.x : rect.width - dragStart.x;
          const availableHeight = directionY < 0 ? dragStart.y : rect.height - dragStart.y;
          const width = Math.min(
            Math.max(Math.abs(x - dragStart.x), Math.abs(y - dragStart.y) * aspectRatio),
            availableWidth,
            availableHeight * aspectRatio,
          );
          const height = width / aspectRatio;
          cropRectCss = {
            left: directionX < 0 ? dragStart.x - width : dragStart.x,
            top: directionY < 0 ? dragStart.y - height : dragStart.y,
            width,
            height,
          };
        } else {
          cropRectCss = {
            left: Math.min(dragStart.x, x),
            top: Math.min(dragStart.y, y),
            width: Math.abs(x - dragStart.x),
            height: Math.abs(y - dragStart.y),
          };
        }
        updateCropVisual();
      });
      stage.addEventListener("pointerup", () => {
        if (!dragStart) return;
        dragStart = null;
        if (!cropRectCss || cropRectCss.width < 8 || cropRectCss.height < 8) cropRectCss = null;
        updateCropVisual();
        attemptCompress();
      });

      resetButton.addEventListener("click", () => {
        cropRectCss = null;
        updateCropVisual();
        attemptCompress();
      });
      cancelButton.addEventListener("click", () => finish(null));
      addButton.addEventListener("click", () => {
        if (pendingResult) finish(pendingResult);
      });

      let imageLoaded = false;
      const onImageLoad = () => {
        if (imageLoaded) return;
        imageLoaded = true;
        if (aspectRatio) {
          const width = Math.min(modalImg.clientWidth, modalImg.clientHeight * aspectRatio);
          const height = width / aspectRatio;
          cropRectCss = {
            left: (modalImg.clientWidth - width) / 2,
            top: (modalImg.clientHeight - height) / 2,
            width,
            height,
          };
          updateCropVisual();
        }
        attemptCompress();
      };
      modalImg.addEventListener("load", onImageLoad);
      modalImg.addEventListener("error", () => finish(null));
      if (modalImg.complete && modalImg.naturalWidth) onImageLoad();
      cancelButton.focus();
    });
  }

  function dataUrlByteLength(dataUrl) {
    const base64 = dataUrl.slice(dataUrl.indexOf(",") + 1);
    return Math.floor(base64.length * 3 / 4) - (base64.endsWith("==") ? 2 : base64.endsWith("=") ? 1 : 0);
  }

  async function encodeImageWithinBytes(source, region, maxBytes, options = {}, isCurrent = () => true) {
    const outputType = options.outputType || "image/jpeg";
    const aspectRatio = Number(options.aspectRatio) > 0 ? Number(options.aspectRatio) : 0;
    const { sx, sy, sw, sh } = region;
    const qualities = outputType === "image/png"
      ? [undefined]
      : [0.82, 0.65, 0.5, 0.35, 0.22, 0.12];
    const maxDimension = Number(options.maxDimension) || 0;
    let scale = maxDimension ? Math.min(1, maxDimension / Math.max(sw, sh)) : 1;
    const rounds = outputType === "image/png" ? 12 : 6;
    for (let round = 0; round < rounds; round += 1) {
      const outW = Math.max(24, Math.round(sw * scale));
      const outH = aspectRatio
        ? Math.max(24, Math.round(outW / aspectRatio))
        : Math.max(24, Math.round(sh * scale));
      const canvas = document.createElement("canvas");
      canvas.width = outW;
      canvas.height = outH;
      const ctx = canvas.getContext("2d");
      if (outputType !== "image/png") {
        ctx.fillStyle = "#fff";
        ctx.fillRect(0, 0, outW, outH);
      }
      ctx.drawImage(source, sx, sy, sw, sh, 0, 0, outW, outH);
      for (const quality of qualities) {
        await new Promise((resolve) => setTimeout(resolve, 0));
        if (!isCurrent()) return null;
        const dataUrl = canvas.toDataURL(outputType, quality);
        const size = dataUrlByteLength(dataUrl);
        if (dataUrl.startsWith(`data:${outputType};`) && size <= maxBytes) return { dataUrl, size };
      }
      scale *= outputType === "image/png" ? 0.75 : 0.65;
    }
    return null;
  }

  function loadImageElement(src) {
    return new Promise((resolve, reject) => {
      const image = new Image();
      image.decoding = "async";
      image.onload = () => resolve(image);
      image.onerror = () => reject(new Error("image_decode_failed"));
      image.src = src;
    });
  }

  async function fitImageWithinBytes(src, maxBytes, options = {}) {
    try {
      const image = await loadImageElement(src);
      const region = { sx: 0, sy: 0, sw: image.naturalWidth, sh: image.naturalHeight };
      if (!region.sw || !region.sh) return null;
      return await encodeImageWithinBytes(image, region, maxBytes, options);
    } catch (_) {
      return null;
    }
  }

  const IMAGE_MARKUP_COLORS = ["#ef4444", "#f59e0b", "#22c55e", "#3b82f6", "#111827", "#ffffff"];
  const IMAGE_MARKUP_TOOLS = [
    ["pen", "pencil", "Draw"],
    ["box", "square", "Box"],
    ["arrow", "move-up-right", "Arrow"],
  ];
  const IMAGE_MARKUP_MAX_DIMENSION = 1600;

  function drawImageMarkupShape(ctx, shape, width, height) {
    const lineWidth = Math.max(2, Math.round(Math.max(width, height) / 250));
    const points = shape.points.map((point) => [point.x * width, point.y * height]);
    ctx.strokeStyle = shape.color;
    ctx.lineWidth = lineWidth;
    ctx.lineCap = "round";
    ctx.lineJoin = "round";
    ctx.beginPath();
    if (shape.tool === "pen") {
      points.forEach(([x, y], index) => (index ? ctx.lineTo(x, y) : ctx.moveTo(x, y)));
      if (points.length === 1) ctx.lineTo(points[0][0] + 0.1, points[0][1]);
      ctx.stroke();
      return;
    }
    if (points.length < 2) return;
    const [[x1, y1], [x2, y2]] = points;
    if (shape.tool === "box") {
      ctx.strokeRect(Math.min(x1, x2), Math.min(y1, y2), Math.abs(x2 - x1), Math.abs(y2 - y1));
      return;
    }
    const angle = Math.atan2(y2 - y1, x2 - x1);
    const head = lineWidth * 4;
    ctx.moveTo(x1, y1);
    ctx.lineTo(x2, y2);
    for (const side of [-1, 1]) {
      ctx.moveTo(x2, y2);
      ctx.lineTo(x2 - head * Math.cos(angle + side * Math.PI / 6), y2 - head * Math.sin(angle + side * Math.PI / 6));
    }
    ctx.stroke();
  }

  function mountImageMarkupEditor(host, { src, name, shapes = [], maxBytes, encodeOptions = {}, onDone, onCancel }) {
    const history = shapes.map((shape) => ({ ...shape, points: shape.points.map((point) => ({ ...point })) }));
    let tool = "pen";
    let color = IMAGE_MARKUP_COLORS[0];
    let draft = null;
    let base = null;
    let busy = false;
    const toolButton = ([value, icon, label]) => `<button type="button" data-markup-tool="${value}" aria-pressed="${value === tool}" title="${label}" aria-label="${label}" class="inline-flex h-7 w-7 items-center justify-center rounded-md border border-transparent text-muted-foreground hover:bg-secondary hover:text-foreground aria-pressed:border-border aria-pressed:bg-secondary aria-pressed:text-foreground"><i data-lucide="${icon}" class="h-4 w-4"></i></button>`;
    const swatch = (value) => `<button type="button" data-markup-color="${value}" aria-pressed="${value === color}" title="Color ${value}" aria-label="Color ${value}" class="h-5 w-5 rounded-full border border-border aria-pressed:ring-2 aria-pressed:ring-foreground aria-pressed:ring-offset-1 aria-pressed:ring-offset-background" style="background:${value}"></button>`;
    host.innerHTML = `
      <div data-image-markup tabindex="-1" class="grid gap-2 rounded-md border border-border bg-background p-2 outline-none">
        <div class="flex flex-wrap items-center gap-1">
          ${IMAGE_MARKUP_TOOLS.map(toolButton).join("")}
          <span aria-hidden="true" class="mx-1 h-4 w-px bg-border"></span>
          <span class="flex items-center gap-1.5">${IMAGE_MARKUP_COLORS.map(swatch).join("")}</span>
          <span aria-hidden="true" class="mx-1 h-4 w-px bg-border"></span>
          <button type="button" data-markup-undo title="Undo (Ctrl+Z)" aria-label="Undo" class="inline-flex h-7 w-7 items-center justify-center rounded-md text-muted-foreground hover:bg-secondary hover:text-foreground disabled:opacity-40"><i data-lucide="undo-2" class="h-4 w-4"></i></button>
          <button type="button" data-markup-clear class="inline-flex h-7 items-center rounded-md px-2 text-xs text-muted-foreground hover:bg-secondary hover:text-foreground disabled:opacity-40">Clear</button>
          <span class="ml-auto flex items-center gap-1.5">
            <button type="button" data-markup-cancel class="fm-btn fm-btn-sm">Cancel</button>
            <button type="button" data-markup-done class="fm-btn fm-btn-sm fm-btn-cta"><i data-lucide="check"></i>Done</button>
          </span>
        </div>
        <canvas data-markup-canvas aria-label="Mark up ${escapeHtml(name)}" class="block h-auto max-h-[70vh] w-auto max-w-full cursor-crosshair touch-none select-none justify-self-center rounded border border-border bg-secondary/30"></canvas>
        <span data-markup-status class="text-[11px] text-muted-foreground">Loading image…</span>
      </div>`;
    window.lucide?.createIcons();
    const root = host.querySelector("[data-image-markup]");
    const canvas = root.querySelector("[data-markup-canvas]");
    const status = root.querySelector("[data-markup-status]");
    const undoButton = root.querySelector("[data-markup-undo]");
    const clearButton = root.querySelector("[data-markup-clear]");
    const doneButton = root.querySelector("[data-markup-done]");
    const ctx = canvas.getContext("2d");
    const setStatus = (text, bad = false) => {
      status.className = `text-[11px] ${bad ? "text-destructive" : "text-muted-foreground"}`;
      status.textContent = text;
    };
    const redraw = () => {
      if (!base) return;
      ctx.clearRect(0, 0, canvas.width, canvas.height);
      ctx.drawImage(base, 0, 0, canvas.width, canvas.height);
      for (const shape of draft ? [...history, draft] : history) drawImageMarkupShape(ctx, shape, canvas.width, canvas.height);
      undoButton.disabled = clearButton.disabled = !history.length;
    };
    const point = (event) => {
      const rect = canvas.getBoundingClientRect();
      return {
        x: Math.min(Math.max((event.clientX - rect.left) / (rect.width || 1), 0), 1),
        y: Math.min(Math.max((event.clientY - rect.top) / (rect.height || 1), 0), 1),
      };
    };
    const undo = () => {
      history.pop();
      redraw();
    };
    const finish = async () => {
      if (!base || busy) return;
      busy = true;
      doneButton.disabled = true;
      setStatus("Compressing…");
      const flat = document.createElement("canvas");
      flat.width = canvas.width;
      flat.height = canvas.height;
      const flatCtx = flat.getContext("2d");
      flatCtx.drawImage(base, 0, 0, flat.width, flat.height);
      for (const shape of history) drawImageMarkupShape(flatCtx, shape, flat.width, flat.height);
      const result = await encodeImageWithinBytes(flat, { sx: 0, sy: 0, sw: flat.width, sh: flat.height }, maxBytes, encodeOptions);
      if (!root.isConnected) return;
      if (!result) {
        busy = false;
        doneButton.disabled = false;
        setStatus(`Still too large to fit under ${formatSize(maxBytes)} - remove another image first.`, true);
        return;
      }
      onDone({ ...result, shapes: history });
    };

    root.addEventListener("click", (event) => {
      const toolTarget = event.target.closest("[data-markup-tool]");
      const colorTarget = event.target.closest("[data-markup-color]");
      if (toolTarget) {
        tool = toolTarget.dataset.markupTool;
        root.querySelectorAll("[data-markup-tool]").forEach((button) => button.setAttribute("aria-pressed", String(button === toolTarget)));
      } else if (colorTarget) {
        color = colorTarget.dataset.markupColor;
        root.querySelectorAll("[data-markup-color]").forEach((button) => button.setAttribute("aria-pressed", String(button === colorTarget)));
      } else if (event.target.closest("[data-markup-undo]")) {
        undo();
      } else if (event.target.closest("[data-markup-clear]")) {
        history.length = 0;
        redraw();
      } else if (event.target.closest("[data-markup-cancel]")) {
        onCancel();
      } else if (event.target.closest("[data-markup-done]")) {
        void finish();
      }
    });
    root.addEventListener("keydown", (event) => {
      if (event.key === "Escape") {
        event.preventDefault();
        event.stopPropagation();
        onCancel();
      } else if ((event.ctrlKey || event.metaKey) && !event.shiftKey && event.key.toLowerCase() === "z") {
        event.preventDefault();
        undo();
      } else if ((event.ctrlKey || event.metaKey) && event.key === "Enter") {
        event.preventDefault();
        event.stopPropagation();
        void finish();
      }
    });
    canvas.addEventListener("pointerdown", (event) => {
      if (!base || event.button > 0) return;
      event.preventDefault();
      root.focus({ preventScroll: true });
      canvas.setPointerCapture(event.pointerId);
      const start = point(event);
      draft = { tool, color, points: tool === "pen" ? [start] : [start, start] };
      redraw();
    });
    canvas.addEventListener("pointermove", (event) => {
      if (!draft) return;
      if (draft.tool === "pen") draft.points.push(point(event));
      else draft.points[1] = point(event);
      redraw();
    });
    const endStroke = () => {
      if (!draft) return;
      const [first, last] = [draft.points[0], draft.points[draft.points.length - 1]];
      if (draft.tool === "pen" || Math.hypot(last.x - first.x, last.y - first.y) > 0.01) history.push(draft);
      draft = null;
      redraw();
    };
    canvas.addEventListener("pointerup", endStroke);
    canvas.addEventListener("pointercancel", endStroke);

    loadImageElement(src).then((image) => {
      if (!root.isConnected) return;
      const scale = Math.min(1, IMAGE_MARKUP_MAX_DIMENSION / Math.max(image.naturalWidth, image.naturalHeight, 1));
      canvas.width = Math.max(1, Math.round(image.naturalWidth * scale));
      canvas.height = Math.max(1, Math.round(image.naturalHeight * scale));
      base = image;
      redraw();
      setStatus("Draw, box or point at what matters, then press Done. Esc cancels.");
      root.focus({ preventScroll: true });
    }).catch(() => setStatus("This image could not be opened for markup.", true));
  }

  function bytesToB64url(bytes) {
    const arr = new Uint8Array(bytes);
    let bin = "";
    for (let i = 0; i < arr.length; i += 1) bin += String.fromCharCode(arr[i]);
    return btoa(bin).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
  }

  async function sha256HexLower(text) {
    const digest = await crypto.subtle.digest("SHA-256", ISSUE_TEXT_ENCODER.encode(text));
    return Array.from(new Uint8Array(digest)).map((b) => b.toString(16).padStart(2, "0")).join("");
  }

  function webSessionAuthored(ev) {
    const me = String(state.session?.nodeName || "").trim().toLowerCase();
    if (!me || !ev) return false;
    if (String(ev.authorName || "").trim().toLowerCase() === me) return true;
    const pub = webIssueKeyPublic();
    return Boolean(pub) && String(ev.author || "") === pub;
  }

  async function getWebIssueKey() {
    let stored = null;
    try { stored = JSON.parse(localStorage.getItem(WEB_ISSUE_KEY_STORAGE) || "null"); } catch (_) {}
    if (stored && stored.jwk && stored.pub) {
      try {
        const privateKey = await crypto.subtle.importKey("jwk", stored.jwk, { name: "Ed25519" }, false, ["sign"]);
        return { privateKey, pub: stored.pub };
      } catch (_) { /* fall through and mint a fresh key */ }
    }
    const pair = await crypto.subtle.generateKey({ name: "Ed25519" }, true, ["sign", "verify"]);
    const rawPub = await crypto.subtle.exportKey("raw", pair.publicKey);
    const jwk = await crypto.subtle.exportKey("jwk", pair.privateKey);
    const pub = bytesToB64url(rawPub);
    try { localStorage.setItem(WEB_ISSUE_KEY_STORAGE, JSON.stringify({ jwk, pub })); } catch (_) {}
    const privateKey = await crypto.subtle.importKey("jwk", jwk, { name: "Ed25519" }, false, ["sign"]);
    return { privateKey, pub };
  }

  // Mirrors IssueStore::contentForSigning + canonicalString and the desktop's
  // submission POST (verify_issue_event in the worker). New issues are signed
  // with number 0; the relay atomically reserves the durable number after it
  // verifies the event, and eligible mirrors keep that slot when they commit.
  //
  // Split out from submitWebIssue because the microjobs page needs the signed
  // event on its own: a paid report is reviewed for duplicates before anything
  // reaches the issue inbox, so it hands the event to /api/microjobs/claims
  // instead of posting it to the repo itself.
  function webEventSigningPrefix(kind, repo) {
    const owner = String(repo?.owner || "").trim().toLowerCase();
    const name = String(repo?.name || "").trim().toLowerCase();
    return `forkmesh-${kind}-v2\n${owner}/${name}\n`;
  }

  async function signWebIssueOpenEvent(title, body, repo) {
    const { privateKey, pub } = await getWebIssueKey();
    // IssueStore timestamps are epoch milliseconds. Keep browser-created
    // events on the same wire format as desktop-created events; seconds are
    // rendered by Qt as dates in 1970 and appear as decades-old activity.
    const ts = Date.now();
    const cleanBody = String(body || "").replace(/[\r\n]+$/, "");
    // open event content = title \0 body \0 attachments(joined by ","; empty here)
    const NUL = String.fromCharCode(0);
    const content = title + NUL + cleanBody + NUL;
    const contentHash = await sha256HexLower(content);
    const canonical = `${webEventSigningPrefix("issue-event", repo)}open\n0\n${pub}\n${ts}\n${contentHash}`;
    const sig = bytesToB64url(await crypto.subtle.sign({ name: "Ed25519" }, privateKey, ISSUE_TEXT_ENCODER.encode(canonical)));
    return {
      type: "open",
      id: "open-web-" + ts,
      title,
      body: cleanBody,
      attachments: [],
      author: pub,
      authorName: state.session?.nodeName || "",
      ts,
      sig,
    };
  }

  async function submitWebIssue(repo, title, body, assignAgent = false, agentModel = "", agentProvider = "", extraMeta = {}, onProgress = null) {
    onProgress?.({ phase: "signing" });
    let event;
    try {
      event = await signWebIssueOpenEvent(title, body, repo);
    } catch (cause) {
      // WebCrypto Ed25519 is missing from older browsers; without it nothing
      // can be signed, and "try again" would never help.
      const error = new Error("signing_unavailable");
      error.cause = cause;
      throw error;
    }
    const payload = {
      owner: repo.owner,
      repo: repo.name,
      number: 0,
      titleIfNew: title,
      event,
      meta: {
        labels: Array.isArray(extraMeta.labels) ? extraMeta.labels.map((x) => String(x)) : [],
        milestone: String(extraMeta.milestone || ""),
        project: String(extraMeta.project || ""),
        priority: Number.isFinite(extraMeta.priority) ? extraMeta.priority : 0,
        assignees: Array.isArray(extraMeta.assignees) ? extraMeta.assignees.map((x) => String(x)) : [],
        wantsAgent: Boolean(assignAgent),
        model: assignAgent ? String(agentModel || "") : "",
        provider: assignAgent ? String(agentProvider || "") : "",
      },
    };
    // When assignAgent is true, include the account session so the server can
    // verify the caller really is the repo owner or an admin (adhoc #225). The
    // session token is the proof; ownerAccount stays only as a display hint.
    if (assignAgent) {
      payload.ownerAccount = state.session?.nodeName || "";
      payload.sessionToken = state.session?.sessionToken || "";
    }
    const data = await fetchRepoMutation(
      repo, "issues", payload, { onProgress, retryTransient: true });
    onProgress?.({ phase: "accepted", number: Number(data?.number || 0) });
    return { ...data, event };
  }

  // A checklist is a portable sub-issue queue. Keep the relationship in the
  // signed descriptions so older clients and raw Git readers still see it:
  // children say "Parent issue: #N" and converted parent rows carry the stable
  // "(sub-issue #N)" suffix. The latter also makes retries idempotent.
  function issueSubtaskCandidates(body) {
    const lines = String(body || "").split("\n");
    const tasks = [];
    for (let line = 0; line < lines.length && tasks.length < 50; line++) {
      const match = /^(\s*(?:[-*+]\s+)?\[\s\]\s+)(.*?)(?:\r)?$/.exec(lines[line]);
      if (!match) continue;
      const label = match[2].trim();
      if (!label || /\s+\(sub-issue\s+#\d+\)\s*$/i.test(label)) continue;
      const title = label.slice(0, 240).trim();
      if (title) tasks.push({ line, prefix: match[1], label, title });
    }
    return tasks;
  }

  function linkIssueSubtasks(body, linkedTasks) {
    const lines = String(body || "").split("\n");
    (linkedTasks || []).forEach((task) => {
      if (Number.isInteger(task.line) && task.line >= 0 && task.line < lines.length) {
        lines[task.line] = `${task.prefix}${task.label} (sub-issue #${task.number})`;
      }
    });
    return lines.join("\n").replace(/[\r\n]+$/, "");
  }

  // Row actions address the source Markdown by line number. Keep these body
  // transforms independent of the DOM so a checkbox click or drag operation
  // becomes one normal signed description edit and remains portable to Git,
  // desktop, and older clients.
  function issueSubtaskCandidateAtLine(body, line) {
    const lines = String(body || "").split("\n");
    if (!Number.isInteger(line) || line < 0 || line >= lines.length) return null;
    const match = /^(\s*(?:[-*+]\s+)?\[([ xX])\]\s+)(.*?)(?:\r)?$/.exec(lines[line]);
    if (!match) return null;
    const label = match[3].trim();
    if (!label || /\s+\(sub-issue\s+#\d+\)\s*$/i.test(label)) return null;
    const title = label.slice(0, 240).trim();
    return title
      ? { line, prefix: match[1], label, title, checked: match[2].toLowerCase() === "x" }
      : null;
  }

  function setIssueTaskChecked(body, line, checked) {
    const source = String(body || "");
    const lines = source.split("\n");
    if (!Number.isInteger(line) || line < 0 || line >= lines.length) return null;
    const match = /^(\s*(?:[-*+]\s+)?\[)([ xX])(\]\s+.*?)(\r?)$/.exec(lines[line]);
    if (!match) return null;
    lines[line] = `${match[1]}${checked ? "x" : " "}${match[3]}${match[4]}`;
    return lines.join("\n");
  }

  function issueTaskIndent(value) {
    return String(value || "").replace(/\t/g, "  ").length;
  }

  // A task can own wrapped prose and nested bullets. Dragging its visible row
  // moves that whole block, stopping at the next sibling or outer block.
  function issueTaskBlockEnd(lines, start) {
    const first = /^(\s*)(?:[-*+]\s+)?\[[ xX]\]\s+/.exec(lines[start] || "");
    if (!first) return start + 1;
    const baseIndent = issueTaskIndent(first[1]);
    let end = start + 1;
    while (end < lines.length) {
      const line = lines[end];
      if (!line.trim()) {
        let next = end + 1;
        while (next < lines.length && !lines[next].trim()) next += 1;
        if (next >= lines.length) break;
        const leading = /^(\s*)/.exec(lines[next])?.[1] || "";
        if (issueTaskIndent(leading) > baseIndent) {
          end = next;
          continue;
        }
        break;
      }
      const leading = /^(\s*)/.exec(line)?.[1] || "";
      const indent = issueTaskIndent(leading);
      if (/^\s*(?:[-*+]|\d+[.)])\s+/.test(line) && indent <= baseIndent) break;
      if (indent <= baseIndent
          && /^\s*(?:#{1,6}\s|```|~~~|>|(?:-{3,}|\*{3,}|_{3,})\s*$|<)/.test(line)) break;
      end += 1;
    }
    return end;
  }

  function reorderIssueTaskBody(body, fromLine, toLine, placeAfter) {
    const source = String(body || "");
    const lines = source.split("\n");
    const from = Number(fromLine);
    const to = Number(toLine);
    if (!Number.isInteger(from) || !Number.isInteger(to)
        || from < 0 || to < 0 || from >= lines.length || to >= lines.length
        || from === to) return source;
    const taskPattern = /^(\s*)(?:[-*+]\s+)?\[[ xX]\]\s+/;
    const sourceTask = taskPattern.exec(lines[from]);
    const targetTask = taskPattern.exec(lines[to]);
    // Cross-level moves are ambiguous Markdown (and could silently re-parent a
    // nested list), so the UI and this defensive check keep peers together.
    if (!sourceTask || !targetTask
        || issueTaskIndent(sourceTask[1]) !== issueTaskIndent(targetTask[1])) return source;
    const sourceEnd = issueTaskBlockEnd(lines, from);
    const targetEnd = issueTaskBlockEnd(lines, to);
    const moved = lines.splice(from, sourceEnd - from);
    const removedBeforeTarget = from < to ? moved.length : 0;
    const adjustedTargetStart = to - removedBeforeTarget;
    const adjustedTargetEnd = targetEnd - removedBeforeTarget;
    const insertAt = placeAfter ? adjustedTargetEnd : adjustedTargetStart;
    lines.splice(insertAt, 0, ...moved);
    return lines.join("\n");
  }

  // The compact World chat composer is a second presentation of the canonical
  // dashboard actions. Expose the signed issue operation narrowly so the chat
  // bundle can file through the same eligible-mirror delivery path without duplicating key,
  // signature, authorization, or payload logic.
  window.ForkMeshDashboardActions = Object.assign(
    window.ForkMeshDashboardActions || {},
    { submitWebIssue },
  );

  // Mirrors IssueStore::contentForSigning's "comment" case (body NUL
  // attachments) and the desktop's inbox POST (verify_issue_event in the
  // worker). Unlike a new issue's "open" event, a comment is signed against the
  // issue's real number - the maintainer's node appends it to that issue when it
  // drains the inbox (IssueStore::applyRemoteEvent).
  async function submitWebIssueComment(repo, number, body) {
    const { privateKey, pub } = await getWebIssueKey();
    const ts = Date.now();
    const cleanBody = String(body || "").replace(/[\r\n]+$/, "");
    // comment event content = body \0 attachments(joined by ","; empty here)
    const NUL = String.fromCharCode(0);
    const contentHash = await sha256HexLower(cleanBody + NUL);
    const canonical = `${webEventSigningPrefix("issue-event", repo)}comment\n${number}\n${pub}\n${ts}\n${contentHash}`;
    const sig = bytesToB64url(await crypto.subtle.sign({ name: "Ed25519" }, privateKey, ISSUE_TEXT_ENCODER.encode(canonical)));
    const event = {
      type: "comment",
      id: "comment-web-" + ts,
      body: cleanBody,
      attachments: [],
      author: pub,
      authorName: state.session?.nodeName || "",
      ts,
      sig,
    };
    const payload = { owner: repo.owner, repo: repo.name, number, event };
    const data = await fetchRepoMutation(repo, "issues", payload);
    return { ...data, event };
  }

  // Lifecycle controls name the state they move an issue TO, and they spell it
  // differently depending on where they were authored: the detail header emits
  // "closed"/"open" (it reads the rendered state badge), the comment form emits
  // "close"/"open". Both must land on the two canonical Qt status values, since
  // the mirror stores whatever string the signed event carries verbatim
  // (applyIssueProjection in server/intake.go) - a stray "close" would leave the
  // issue neither open nor closed. Returns "" for anything that isn't a
  // lifecycle action, which is how callers tell a plain comment apart.
  function webIssueStatusForAction(action) {
    const name = String(action || "").trim().toLowerCase();
    if (name === "close" || name === "closed") return "closed";
    if (name === "open" || name === "reopen" || name === "reopened") return "open";
    return "";
  }

  function webIssueEventContent(type, fields = {}) {
    const NUL = String.fromCharCode(0);
    if (type === "edit") {
      return String(fields.body || "") + NUL
        + (Array.isArray(fields.attachments) ? fields.attachments.join(",") : "");
    }
    if (type === "title") return String(fields.title || "");
    if (type === "status") return String(fields.status || "");
    if (type === "labels") {
      return (Array.isArray(fields.labels) ? fields.labels : []).join(",");
    }
    if (type === "milestone") return String(fields.milestone || "");
    if (type === "dates") {
      return `${Number(fields.startDate) || 0}${NUL}${Number(fields.endDate) || 0}`;
    }
    if (type === "priority") return String(Number(fields.priority) || 0);
    if (type === "progress") return String(Number(fields.progress) || 0);
    if (type === "assignees") {
      return (Array.isArray(fields.assignees) ? fields.assignees : []).join(",");
    }
    if (type === "claim") {
      return String(fields.assignee || "") + NUL + String(fields.claimState || "");
    }
    if (type === "delete") return String(fields.target || "");
    if (type === "vote") return "";
    throw new Error("unsupported_issue_action");
  }

  function webIssueEventId(type, ts) {
    const suffix = new Uint32Array(1);
    crypto.getRandomValues(suffix);
    return `${type}-web-${ts}-${suffix[0].toString(36)}`;
  }

  // Owner-side issue actions use the same append-only event model as Qt. The
  // event is signed by this browser key, while the account session separately
  // proves repository authority to the Worker before it enters the mirror
  // inbox. Votes remain available to any signed-in contributor.
  async function submitWebIssueEvent(repo, number, type, fields = {}) {
    const issueNumber = Number(number);
    if (!Number.isInteger(issueNumber) || issueNumber <= 0) {
      throw new Error("invalid_issue_number");
    }
    const { privateKey, pub } = await getWebIssueKey();
    const ts = Date.now();
    const event = {
      type,
      id: webIssueEventId(type, ts),
      author: pub,
      authorName: state.session?.nodeName || "",
      ts,
    };
    const fieldNames = {
      edit: ["target", "body", "attachments"],
      title: ["title"],
      status: ["status"],
      labels: ["labels"],
      milestone: ["milestone"],
      dates: ["startDate", "endDate"],
      priority: ["priority"],
      progress: ["progress"],
      assignees: ["assignees"],
      claim: ["assignee", "claimState"],
      delete: ["target"],
      vote: [],
    }[type];
    if (!fieldNames) throw new Error("unsupported_issue_action");
    fieldNames.forEach((name) => {
      event[name] = fields[name];
    });
    if (type === "status") {
      // Signed and then replayed verbatim onto the issue JSON, so a status this
      // side does not recognize is refused here rather than committed.
      event.status = webIssueStatusForAction(event.status);
      if (!event.status) throw new Error("unsupported_issue_action");
    }
    if (type === "edit") {
      event.body = String(event.body || "").replace(/[\r\n]+$/, "");
      event.attachments = Array.isArray(event.attachments)
        ? event.attachments.map((value) => String(value))
        : [];
    }
    if (type === "labels" || type === "assignees") {
      event[type] = (Array.isArray(event[type]) ? event[type] : [])
        .map((value) => String(value).trim())
        .filter(Boolean)
        .slice(0, 20);
    }
    const contentHash = await sha256HexLower(
      webIssueEventContent(type, event),
    );
    const canonical = `${webEventSigningPrefix("issue-event", repo)}${type}\n${issueNumber}\n${pub}\n${ts}\n${contentHash}`;
    event.sig = bytesToB64url(await crypto.subtle.sign(
      { name: "Ed25519" },
      privateKey,
      ISSUE_TEXT_ENCODER.encode(canonical),
    ));
    const payload = {
      owner: repo.owner,
      repo: repo.name,
      number: issueNumber,
      event,
      ownerAccount: state.session?.nodeName || "",
      sessionToken: state.session?.sessionToken || "",
    };
    const data = await fetchRepoMutation(repo, "issues", payload);
    // Kept past this page so the next mirror read, which still predates the
    // owner's drain, cannot undo the change on screen (issue #715).
    if (type === "status") rememberPendingIssueStatus(repo, issueNumber, event);
    return { ...data, event };
  }

  async function setWebIssueSubscription(repo, number, subscribed) {
    const response = await fetch(`${repoApiBase(repo)}/subscribe`, {
      method: "POST",
      headers: {
        "content-type": "application/json",
        accept: "application/json",
      },
      body: JSON.stringify({
        node: state.session?.nodeName || "",
        sessionToken: state.session?.sessionToken || "",
        source: "issue",
        number: Number(number),
        subscribed: subscribed === true,
      }),
    });
    const data = await response.json().catch(() => ({}));
    if (!response.ok || data.ok === false) {
      throw new Error(data.error || `HTTP ${response.status}`);
    }
    return data;
  }

  async function loadWebIssueSubscription(repo, number) {
    if (!state.session?.nodeName || !state.session?.sessionToken) return { subscribed: false, reason: "" };
    const response = await fetch(`${repoApiBase(repo)}/subscribe`, {
      method: "POST",
      headers: {
        "content-type": "application/json",
        accept: "application/json",
      },
      body: JSON.stringify({
        action: "status",
        node: state.session.nodeName,
        sessionToken: state.session.sessionToken,
        source: "issue",
        number: Number(number),
      }),
    });
    const data = await response.json().catch(() => ({}));
    if (!response.ok || data.ok === false) return { subscribed: false, reason: "" };
    return { subscribed: data.subscribed === true, reason: String(data.reason || (data.subscribed === true ? "subscribed" : "")) };
  }

  // Mirrors DiscussionStore::contentForSigning's "comment" case (just the
  // reply body) and the desktop's discussion inbox POST (verify_discussion_event
  // in the worker). Replies are signed against the discussion's real number -
  // unlike a new discussion's "open" event, they don't use the placeholder 0.
  async function submitWebDiscussionComment(repo, number, body) {
    const { privateKey, pub } = await getWebIssueKey();
    const ts = Math.floor(Date.now() / 1000);
    const cleanBody = String(body || "").replace(/[\r\n]+$/, "");
    const contentHash = await sha256HexLower(cleanBody);
    const canonical = `${webEventSigningPrefix("discussion-event", repo)}comment\n${number}\n${pub}\n${ts}\n${contentHash}`;
    const sig = bytesToB64url(await crypto.subtle.sign({ name: "Ed25519" }, privateKey, ISSUE_TEXT_ENCODER.encode(canonical)));
    const event = {
      type: "comment",
      id: webDiscussionEventId("comment", ts),
      body: cleanBody,
      author: pub,
      authorName: state.session?.nodeName || "",
      ts,
      sig,
    };
    const payload = { owner: repo.owner, repo: repo.name, number, event };
    const data = await fetchRepoMutation(repo, "discussions", payload, { retryTransient: true });
    return { ...data, event };
  }

  function webDiscussionEventId(type, ts) {
    const suffix = new Uint32Array(1);
    crypto.getRandomValues(suffix);
    return `${type}-web-${ts}-${suffix[0].toString(36)}`;
  }

  const WEB_DISCUSSION_CATEGORIES = ["Announcements", "Ideas", "Q&A", "Show and tell", "Maintainer notes"];

  async function signWebDiscussionOpenEvent(repo, title, body, category) {
    const { privateKey, pub } = await getWebIssueKey();
    const ts = Date.now();
    const cleanTitle = String(title || "").trim().slice(0, 240);
    const cleanBody = String(body || "").replace(/^[\r\n]+|[\r\n]+$/g, "");
    const cleanCategory = WEB_DISCUSSION_CATEGORIES.find(
      (name) => name.toLowerCase() === String(category || "").trim().toLowerCase()) || "Ideas";
    const NUL = String.fromCharCode(0);
    const contentHash = await sha256HexLower(cleanTitle + NUL + cleanBody + NUL + cleanCategory);
    const canonical = `${webEventSigningPrefix("discussion-event", repo)}open\n0\n${pub}\n${ts}\n${contentHash}`;
    const sig = bytesToB64url(await crypto.subtle.sign({ name: "Ed25519" }, privateKey, ISSUE_TEXT_ENCODER.encode(canonical)));
    return {
      type: "open",
      id: webDiscussionEventId("open", ts),
      title: cleanTitle,
      body: cleanBody,
      category: cleanCategory,
      author: pub,
      authorName: state.session?.nodeName || "",
      ts,
      sig,
    };
  }

  async function submitWebDiscussion(repo, { title, body, category } = {}, onProgress = null) {
    onProgress?.({ phase: "signing" });
    let event;
    try {
      event = await signWebDiscussionOpenEvent(repo, title, body, category);
    } catch (cause) {
      const error = new Error("signing_unavailable");
      error.cause = cause;
      throw error;
    }
    const payload = {
      owner: repo.owner,
      repo: repo.name,
      number: 0,
      titleIfNew: event.title,
      event,
    };
    const data = await fetchRepoMutation(
      repo, "discussions", payload, { onProgress, retryTransient: true });
    onProgress?.({ phase: "accepted", number: Number(data?.number || 0) });
    return { ...data, number: Number(data?.number || 0), event };
  }

  async function submitWebDiscussionModeration(repo, number, type, value) {
    const discussionNumber = Number(number);
    if (!Number.isInteger(discussionNumber) || discussionNumber <= 0) {
      throw new Error("invalid_discussion_number");
    }
    if (type === "status" && !["open", "closed", "archived"].includes(value)) {
      throw new Error("unsupported_discussion_action");
    }
    if (type !== "status" && (type !== "delete" || !String(value || ""))) {
      throw new Error("unsupported_discussion_action");
    }
    const { privateKey, pub } = await getWebIssueKey();
    const ts = Math.floor(Date.now() / 1000);
    const content = String(value || "");
    const contentHash = await sha256HexLower(content);
    const canonical = `${webEventSigningPrefix("discussion-event", repo)}${type}\n${discussionNumber}\n${pub}\n${ts}\n${contentHash}`;
    const sig = bytesToB64url(await crypto.subtle.sign({ name: "Ed25519" }, privateKey, ISSUE_TEXT_ENCODER.encode(canonical)));
    const event = {
      type,
      id: webDiscussionEventId(type, ts),
      ...(type === "status" ? { status: content } : { target: content }),
      author: pub,
      authorName: state.session?.nodeName || "",
      ts,
      sig,
    };
    const payload = {
      owner: repo.owner,
      repo: repo.name,
      number: discussionNumber,
      event,
      ownerAccount: state.session?.nodeName || "",
      sessionToken: state.session?.sessionToken || "",
    };
    const data = await fetchRepoMutation(repo, "discussions", payload);
    return { ...data, event };
  }

  async function submitWebDiscussionAnswer(repo, number, target) {
    const { privateKey, pub } = await getWebIssueKey();
    const ts = Math.floor(Date.now() / 1000);
    const cleanTarget = String(target || "");
    const contentHash = await sha256HexLower(cleanTarget);
    const canonical = `${webEventSigningPrefix("discussion-event", repo)}answer\n${number}\n${pub}\n${ts}\n${contentHash}`;
    const sig = bytesToB64url(await crypto.subtle.sign({ name: "Ed25519" }, privateKey, ISSUE_TEXT_ENCODER.encode(canonical)));
    const event = {
      type: "answer",
      id: "answer-web-" + ts,
      target: cleanTarget,
      author: pub,
      authorName: state.session?.nodeName || "",
      ts,
      sig,
    };
    const payload = {
      owner: repo.owner,
      repo: repo.name,
      number,
      event,
      sessionToken: state.session?.sessionToken || "",
    };
    return fetchRepoMutation(repo, "discussions", payload);
  }

  function webProjectEventContent(ev) {
    const NUL = String.fromCharCode(0);
    if (ev.type === "open") return `${ev.title}${NUL}${ev.body}`;
    if (ev.type === "title") return ev.title;
    if (ev.type === "edit") return ev.body;
    if (ev.type === "status") return ev.status;
    if (ev.type === "dates") return `${Number(ev.startDate) || 0}${NUL}${Number(ev.endDate) || 0}`;
    if (ev.type === "milestone") return ev.milestone;
    if (ev.type === "issues") return [...new Set(ev.issues)].sort((a, b) => a - b).join(",");
    if (ev.type === "delete") return ev.target;
    throw new Error("unsupported_project_event");
  }

  async function signWebProjectEvents(number, specs) {
    const { privateKey, pub } = await getWebIssueKey();
    const base = Date.now();
    const events = [];
    for (let index = 0; index < specs.length; index += 1) {
      const ts = base + index;
      const ev = {
        ...specs[index],
        id: webDiscussionEventId(specs[index].type, ts),
        author: pub,
        authorName: state.session?.nodeName || "",
        ts,
      };
      if (ev.type === "issues") ev.issues = [...new Set(ev.issues.map(Number))].sort((a, b) => a - b);
      const hash = await sha256HexLower(webProjectEventContent(ev));
      const canonical = `forkmesh-project-event-v1\n${ev.type}\n${Number(number)}\n${pub}\n${ts}\n${hash}`;
      ev.sig = bytesToB64url(await crypto.subtle.sign({ name: "Ed25519" }, privateKey, ISSUE_TEXT_ENCODER.encode(canonical)));
      events.push(ev);
    }
    return events;
  }

  async function submitWebProjectEvents(repo, number, specs) {
    const events = await signWebProjectEvents(number, specs);
    const headers = { accept: "application/json", "content-type": "application/json" };
    const token = String(state.session?.sessionToken || "");
    if (token && token !== "cookie") headers.authorization = `Bearer ${token}`;
    const response = await fetch(`${repoApiBase(repo)}/project-edit`, {
      method: "POST", credentials: "same-origin", cache: "no-store", headers,
      body: JSON.stringify({
        branch: repoDefaultBranch(repo) || "main", number: Number(number), events,
        ...(token && token !== "cookie" ? { sessionToken: token } : {}),
      }),
    });
    const data = await response.json().catch(() => ({}));
    if (!response.ok || data?.ok === false) {
      const error = new Error(data?.message || data?.error || `HTTP ${response.status}`);
      error.status = response.status;
      error.code = data?.error || "";
      error.next = Number(data?.next || 0);
      throw error;
    }
    return data;
  }

  function webIssueKeyPublic() {
    try {
      return String(JSON.parse(localStorage.getItem(WEB_ISSUE_KEY_STORAGE) || "null")?.pub || "");
    } catch (_) {
      return "";
    }
  }

  // Mirrors PullStore::contentForSigning's per-type field order (comment,
  // review, line-comment, thread-comment, thread-reply, thread-state,
  // suggestion-state) and the desktop's pull inbox POST
  // (verify_pull_comment_event/pull_comment_content in the worker). Covers
  // every conversation-shaped pull event; opening a brand new pull request is
  // a distinct signed shape (submitWebPullOpen, below).
  async function submitWebPullEvent(repo, number, type, fields = {}, delivery = {}) {
    const { privateKey, pub } = await getWebIssueKey();
    const ts = Math.floor(Date.now() / 1000);
    const randomId = typeof crypto.randomUUID === "function"
      ? crypto.randomUUID()
      : bytesToB64url(crypto.getRandomValues(new Uint8Array(12)));
    const eventId = `${type}-web-${ts}-${randomId}`;
    // Native clients use the opening event id as the review-thread id. Do the
    // same in the browser so an inline comment can receive replies/resolution
    // events later without changing the signed wire format.
    if (type === "thread-comment" && !fields.threadId) fields.threadId = eventId;
    const NUL = String.fromCharCode(0);
    const cleanBody = String(fields.body || "").replace(/[\r\n]+$/, "");
    // A line-anchored event that names the patchset it was written against
    // and/or the commented text signs both (PullEvent::hasAnchor on the
    // desktop); without them the signed bytes are exactly the pre-anchor ones.
    const anchorFields = (fields.patchset || fields.anchorLines)
      ? [String(fields.patchset || ""), String(fields.anchorLines || "")]
      : [];
    let content;
    if (type === "comment") {
      content = cleanBody;
    } else if (type === "review") {
      content = [fields.state || "", cleanBody].join(NUL);
    } else if (type === "line-comment") {
      content = [fields.path || "", fields.side || "", String(fields.line || 0), cleanBody, ...anchorFields].join(NUL);
    } else if (type === "thread-comment") {
      content = [
        fields.threadId || "", fields.path || "", fields.side || "",
        String(fields.lineStart || 0), String(fields.lineEnd || 0),
        cleanBody, fields.suggestionPatch || "", ...anchorFields,
      ].join(NUL);
    } else if (type === "thread-reply") {
      content = [fields.threadId || "", fields.parentId || "", cleanBody].join(NUL);
    } else if (type === "thread-state") {
      content = [fields.threadId || "", fields.state || "", cleanBody].join(NUL);
    } else if (type === "suggestion-state") {
      content = [fields.threadId || "", fields.state || "", fields.appliedCommit || "", cleanBody].join(NUL);
    } else if (type === "status") {
      content = [fields.state || "", cleanBody].join(NUL);
    } else if (type === "delete") {
      content = cleanBody;
    } else if (type === "comment-edit") {
      if (!fields.target) throw new Error("unsupported_pull_event_type");
      content = [fields.target, cleanBody].join(NUL);
    } else if (type === "comment-delete") {
      if (!fields.target) throw new Error("unsupported_pull_event_type");
      content = String(fields.target);
    } else {
      throw new Error("unsupported_pull_event_type");
    }
    const contentHash = await sha256HexLower(content);
    const canonical = `${webEventSigningPrefix("pull-comment", repo)}${type}\n${number}\n${pub}\n${ts}\n${contentHash}`;
    const sig = bytesToB64url(await crypto.subtle.sign({ name: "Ed25519" }, privateKey, ISSUE_TEXT_ENCODER.encode(canonical)));
    const event = {
      type,
      id: eventId,
      body: cleanBody,
      author: pub,
      authorName: state.session?.nodeName || "",
      ts,
      sig,
    };
    for (const key of ["state", "path", "side", "line", "threadId", "parentId", "lineStart", "lineEnd", "suggestionPatch", "appliedCommit", "patchset", "anchorLines", "target"]) {
      if (fields[key] !== undefined && fields[key] !== "") event[key] = fields[key];
    }
    const payload = { owner: repo.owner, repo: repo.name, number, event, ...(delivery.extra || {}) };
    const data = await fetchRepoMutation(repo, "pulls", payload, delivery.relayOnly ? { relayOnly: true } : {});
    return { ...data, event };
  }

  async function submitWebPullCommentEdit(repo, number, target, body) {
    return submitWebPullEvent(repo, number, "comment-edit", { target, body });
  }

  async function submitWebPullCommentDelete(repo, number, target) {
    return submitWebPullEvent(repo, number, "comment-delete", { target });
  }

  async function submitWebPullComment(repo, number, body) {
    return submitWebPullEvent(repo, number, "comment", { body });
  }

  async function submitWebPullReview(repo, number, reviewState, body) {
    return submitWebPullEvent(repo, number, "review", { state: reviewState, body });
  }

  async function submitWebPullClose(repo, number) {
    return submitWebPullEvent(repo, number, "status", { state: "closed" });
  }

  async function submitWebPullDelete(repo, number) {
    return submitWebPullEvent(repo, number, "delete", {});
  }

  function pullRequestParent(repo) {
    const parent = repoForkParent(repo);
    if (!parent) return null;
    const parentRecord = findRepository(parent.key);
    return parentRecord && repoReadOnlyMirror(parentRecord) ? null : parent;
  }

  function pullRequestTargetRepo(repo) {
    const parent = pullRequestParent(repo);
    if (!parent) return repo;
    return findRepository(parent.key) || {
      owner: parent.owner,
      name: parent.name,
      canonicalOwner: parent.owner,
      canonicalName: parent.name,
    };
  }

  // Mirrors PullStore::canonicalString for a new pull (verify_pull_event in
  // the Worker). Branch names alone are mutable and may not exist on the
  // owner's node, so resolve them through an attested mirror once and sign the
  // resulting portable binary patch + commit series. For a fork, comparison
  // happens on the fork's mirror while the signed, self-contained proposal is
  // delivered to the parent's durable inbox; the parent node can be offline.
  async function submitWebPullOpen(repo, title, body, base, head, aiModel = "") {
    const targetRepo = pullRequestTargetRepo(repo);
    const parent = pullRequestParent(repo);
    const signedHead = parent
      ? `${repoDisplayOwner(repo) || repo.owner}:${head}`
      : head;
    const pull = withPullAiModel(await signWebPull(repo, targetRepo, title, body, base, head, signedHead), aiModel);
    const payload = { owner: targetRepo.owner, repo: targetRepo.name, pull };
    try {
      const data = await fetchRepoMutation(targetRepo, "pulls", payload);
      return { ...data, pull, targetRepo };
    } catch (error) {
      if (targetRepo === repo || String(error?.code || error?.message || "") !== "read_only_mirror") throw error;
    }
    const ownPull = withPullAiModel(await signWebPull(repo, repo, title, body, base, head, head), aiModel);
    const data = await fetchRepoMutation(repo, "pulls", { owner: repo.owner, repo: repo.name, pull: ownPull });
    return { ...data, pull: ownPull, targetRepo: repo, refusedBy: targetRepo };
  }

  const REMOTE_FORK_ORIGIN = /^https:\/\/[a-z0-9.-]+(?::\d{1,5})?$/;

  function withPullAiModel(pull, aiModel) {
    const model = cleanAiModel(String(aiModel || ""));
    return model ? { ...pull, aiModel: model } : pull;
  }

  async function submitRemotePullOpen(repo, remote, title, body, base, head, compareBase = "", aiModel = "") {
    const origin = String(remote?.origin || "").toLowerCase();
    const owner = String(remote?.owner || "");
    const name = String(remote?.name || "");
    if (!REMOTE_FORK_ORIGIN.test(origin) || !owner || !name) {
      throw new Error("invalid_remote_fork");
    }
    const signedHead = `${location.host}/${repoDisplayKey(repo)}:${head}`;
    const pull = withPullAiModel(await signWebPull(repo, { owner, name }, title, body, base, head, signedHead, compareBase), aiModel);
    let response;
    try {
      response = await fetch(`${origin}/api/repo/${encodeURIComponent(owner)}/${encodeURIComponent(name)}/pulls`, {
        method: "POST",
        credentials: "omit",
        cache: "no-store",
        headers: { accept: "application/json", "content-type": "application/json" },
        body: JSON.stringify({ owner, repo: name, pull }),
      });
    } catch (cause) {
      const error = new Error("remote_unreachable");
      error.cause = cause;
      throw error;
    }
    const data = await response.json().catch(() => ({}));
    if (!response.ok || data.ok === false) {
      const error = new Error(String(data.error || `HTTP ${response.status}`));
      error.code = String(data.error || "");
      error.status = response.status;
      error.data = data;
      throw error;
    }
    return { ...data, pull, remote };
  }

  async function signWebPull(repo, signingRepo, title, body, base, head, signedHead, compareBase = "") {
    // The comparison is the first thing that can fail, and it fails BEFORE
    // anything is signed or sent - a mirror answers 404 for a branch name it
    // cannot resolve yet, which is the common case for a branch that was just
    // pushed. Tag it so the composer reports a comparison that never ran
    // instead of a pull request that could not be delivered; the raw 404
    // surfaced as "Could not send the pull request (not_found, HTTP 404)",
    // which reads as a rejected submission and invites a pointless resend.
    let comparison;
    try {
      comparison = await fetchRepoJson(repoLiveUrl(repo, "compare", {
        base: compareBase || base,
        head,
        ref: "",
      }));
    } catch (cause) {
      const error = new Error("comparison_unavailable");
      error.status = Number(cause?.status) || 0;
      error.code = String(cause?.code || "");
      error.cause = cause;
      throw error;
    }
    const patch = String(comparison.patch || "");
    const commits = String(comparison.commits || "");
    const baseOid = String(comparison.baseOid || "").toLowerCase();
    const headOid = String(comparison.headOid || "").toLowerCase();
    const validOid = (value) => /^[0-9a-f]{40}(?:[0-9a-f]{24})?$/.test(value);
    if (!validOid(baseOid) || !validOid(headOid)) {
      throw new Error("invalid_comparison");
    }
    if (!patch.trim()) {
      throw new Error("no_changes");
    }
    return signWebPullContent(signingRepo, title, body, base, signedHead, patch, commits);
  }

  async function submitWebFileProposal(repo, proposal, title, body = "") {
    const patch = String(proposal?.patch || "");
    const commits = String(proposal?.commits || "");
    const base = String(proposal?.base || "");
    const head = String(proposal?.head || "");
    if (!patch.trim() || !base || !head) throw new Error("no_changes");
    const pull = await signWebPullContent(repo, title, body, base, head, patch, commits);
    const data = await fetchRepoMutation(repo, "pulls", { owner: repo.owner, repo: repo.name, pull });
    return { ...data, pull, targetRepo: repo };
  }

  async function signWebPullContent(signingRepo, title, body, base, signedHead, patch, commits) {
    if (ISSUE_TEXT_ENCODER.encode(patch).byteLength
        + ISSUE_TEXT_ENCODER.encode(commits).byteLength > 4 * 1024 * 1024) {
      throw new Error("pull_too_large");
    }
    const { privateKey, pub } = await getWebIssueKey();
    const ts = Math.floor(Date.now() / 1000);
    const cleanBody = String(body || "").replace(/[\r\n]+$/, "");
    const NUL = String.fromCharCode(0);
    const content = [title, base, signedHead, patch, commits].join(NUL);
    const contentHash = await sha256HexLower(content);
    const canonical = `${webEventSigningPrefix("pull-event", signingRepo)}${pub}\n${ts}\n${contentHash}`;
    const sig = bytesToB64url(await crypto.subtle.sign({ name: "Ed25519" }, privateKey, ISSUE_TEXT_ENCODER.encode(canonical)));
    return {
      title,
      description: cleanBody,
      base,
      head: signedHead,
      patch,
      commits,
      author: pub,
      authorName: state.session?.nodeName || "",
      ts,
      sig,
    };
  }

  async function handleIssueCommentSubmit(repo, form) {
    if (!repo || !form) return;
    const number = Number(form.dataset.repoIssueCommentNumber || 0);
    const bodyInput = form.querySelector("[data-repo-issue-comment-body]");
    const submit = form.querySelector("[data-repo-issue-comment-submit]");
    const actionButtons = form.querySelectorAll(
      "[data-repo-issue-comment-action]",
    );
    // Resolved per call, not captured: a lifecycle submit re-renders the whole
    // issue detail on success, so the form (and its hint) this handler started
    // with is detached by the time there is an outcome to report.
    const setHint = (text, tone) => {
      const hint = document.querySelector("[data-repo-issue-comment-hint]");
      if (!hint) return;
      hint.className = `text-[11px] ${tone === "bad" ? "text-destructive" : "text-muted-foreground"}`;
      hint.textContent = text;
    };
    const body = String(bodyInput?.value || "").trim();
    if (!number) {
      setHint("This issue hasn't finished loading yet.", "bad");
      return;
    }
    // "Close issue" / "Reopen issue" with an empty box is the lifecycle
    // change alone, as on GitHub.
    const requestedAction = String(form._issueSubmitAction || "comment");
    if (!body && webIssueStatusForAction(requestedAction)) {
      form._issueSubmitAction = "comment";
      await handleWebIssueAction(requestedAction);
      return;
    }
    if (!body) {
      setHint("Write a comment before sending.", "bad");
      bodyInput?.focus();
      return;
    }
    actionButtons.forEach((button) => { button.disabled = true; });
    // The Comment button spins while the event is signed and delivered
    // (issue #687).
    const spinner = form.querySelector("[data-repo-issue-comment-spinner]");
    const stopSpinner = () => { if (spinner) spinner.hidden = true; };
    if (spinner) spinner.hidden = false;
    setHint("Signing and sending…");
    try {
      const result = await submitWebIssueComment(repo, number, body);
      if (result?.pending === "review") {
        stopSpinner();
        if (bodyInput) bodyInput.value = "";
        actionButtons.forEach((button) => { button.disabled = false; });
        form._issueSubmitAction = "comment";
        setHint(moderationPendingMessage(result));
        return;
      }
      // Show the comment straight away: it only reaches the mirror once the
      // maintainer's node drains the inbox, so the timeline can't reload it yet.
      // The composer sits in its own row of the conversation, so find the
      // timeline through the conversation rather than the form's parent.
      const timeline = form.closest("[data-repo-record-conversation]")
        ?.querySelector("[data-repo-issue-timeline]");
      if (timeline) {
        if (timeline.dataset.empty === "true") {
          timeline.innerHTML = "";
          timeline.dataset.empty = "false";
        }
        timeline.insertAdjacentHTML("beforeend", renderIssueTimelineComment({
          authorName: state.session?.nodeName || "you",
          ts: Date.now(), body,
        }));
      }
      stopSpinner();
      if (bodyInput) bodyInput.value = "";
      optimisticWebIssueEvent(result.event);
      const action = String(form._issueSubmitAction || "comment");
      form._issueSubmitAction = "comment";
      const nextStatus = webIssueStatusForAction(action);
      if (nextStatus) {
        // The comment is already accepted at this point, so a rejected status
        // event must not read as "the comment failed" - that invites a resend
        // and a duplicate. Report the half that failed and keep the rest.
        try {
          const statusResult = await submitWebIssueEvent(
            repo,
            number,
            "status",
            { status: nextStatus },
          );
          optimisticWebIssueEvent(statusResult.event);
          syncWebIssueListStatus(number, nextStatus);
        } catch (error) {
          actionButtons.forEach((button) => { button.disabled = false; });
          renderCurrentWebIssueDetail();
          setHint(
            `Comment sent, but the issue stayed ${nextStatus === "closed" ? "open" : "closed"}. ${webIssueActionError(error)}`,
            "bad",
          );
          return;
        }
      }
      actionButtons.forEach((button) => { button.disabled = false; });
      renderCurrentWebIssueDetail();
      setHint(
        nextStatus === "closed"
          ? "Comment sent and the issue is closed."
          : nextStatus === "open"
            ? "Comment sent and the issue is reopened."
            : "Comment accepted for direct delivery to an eligible mirror.",
        "good",
      );
    } catch (error) {
      stopSpinner();
      actionButtons.forEach((button) => { button.disabled = false; });
      const code = String(error?.message || "");
      setHint(
        code === "inbox_full" ? "Comment delivery is temporarily full. Try again later."
          : code === "author_quota" ? "You've reached the submission limit for this repository."
          : code === "issue_too_large" ? "The comment is too large - please shorten it."
          : code === "bad_signature" ? "Could not verify the comment's signature."
          : "Could not send the comment. Please try again.",
        "bad");
    }
  }

  function optimisticWebIssueEvent(event) {
    const detail = state.repoRecordDetail;
    if (!event || detail?.kind !== "issues") return;
    const parsed = detail.parsed || {};
    const values = parsed.values || (parsed.values = {});
    const events = Array.isArray(parsed.issueEvents)
      ? parsed.issueEvents
      : (parsed.issueEvents = []);
    if (!events.some((existing) => existing?.id === event.id)) events.push(event);
    if (event.type === "title") values.title = event.title;
    if (event.type === "status") values.status = event.status;
    if (event.type === "labels") values.labels = `[${event.labels.join(", ")}]`;
    if (event.type === "milestone") values.milestone = event.milestone;
    if (event.type === "priority") values.priority = Number(event.priority) || 0;
    if (event.type === "progress") values.progress = Number(event.progress) || 0;
    if (event.type === "assignees") {
      values.assignees = `[${event.assignees.join(", ")}]`;
    }
    if (event.type === "claim") {
      values.assignees = `[${webIssueClaimApplied(
        parseFrontMatterList(values.assignees), event,
      ).join(", ")}]`;
    }
    if (event.type === "dates") {
      values.startDate = Number(event.startDate) || 0;
      values.endDate = Number(event.endDate) || 0;
    }
    if (event.type === "vote") values.votes = (Number(values.votes) || 0) + 1;
    if (event.type === "edit" && event.target === parsed.issueOpenEventId) {
      parsed.body = event.body;
    }
    if (event.type === "delete" && event.target === "self") {
      parsed.issueDeleted = true;
    }
  }

  function syncWebIssueListClaim(number, event) {
    if (!event) return;
    const rows = new Set([state.issuesView?.items, state.issueRelationIndex?.items]
      .map((items) => (Array.isArray(items) ? items.find((issue) => Number(issue.number) === Number(number)) : null))
      .filter(Boolean));
    rows.forEach((row) => {
      row.events = Array.isArray(row.events) ? row.events.slice() : [];
      row.events.push(event);
    });
  }

  async function webIssueClaimBlockers(repo, number) {
    try {
      const settings = await loadRepoCollabSettings(repo);
      const limit = Number(settings?.assignmentLimit) || 0;
      if (!limit) return [];
      const [issues, pulls] = await Promise.all([
        loadRepoIssueRelationIndex(repo),
        loadRepoDevelopmentPulls(repo),
      ]);
      if (!Array.isArray(pulls)) return [];
      const held = issueAssignmentsWithoutPull(state.session?.nodeName, issues, pulls, number);
      return held.length >= limit ? held : [];
    } catch (_) {
      return [];
    }
  }

  // The Issues list, its Open/Closed filter and the tab badge are all served
  // from the cached rows in state, and the mirror only learns about the signed
  // status event once the owner node drains its inbox. Move the row here so
  // navigating back from a just-closed issue does not show it as open again.
  function syncWebIssueListStatus(number, status) {
    const view = state.issuesView;
    const items = Array.isArray(view?.items) ? view.items : null;
    if (!items) return;
    const row = items.find((issue) => Number(issue.number) === Number(number));
    if (!row || row.status === status) return;
    const closing = status === "closed";
    row.status = status;
    row.state = status;
    if (Number.isFinite(view.openCount) && Number.isFinite(view.closedCount)) {
      view.openCount = Math.max(0, view.openCount + (closing ? -1 : 1));
      view.closedCount = Math.max(0, view.closedCount + (closing ? 1 : -1));
      setRepoTabCount("issues", view.openCount);
      setRepoCollectionCounts("issues", view.openCount, view.closedCount);
    }
    // A reopened issue is no longer part of the lazily paged closed history;
    // dropping its path keeps loadClosedIssues from re-reading a stale blob.
    if (!closing && view.closedPaths instanceof Map) {
      view.closedPaths.delete(Number(number));
    }
  }

  // Newly filed web issues are that many more open issues than the header
  // already counts. Move the tallies by that much rather than recounting the
  // loaded rows: the list may never have loaded (a recount then read 1), and
  // the recount left the Open strip's openCount behind the badge (issue #716).
  // unservedOpen keeps them counted until a list load finds them served.
  function countFiledWebIssues(added) {
    const view = state.issuesView;
    if (!view || !(added > 0)) return;
    view.unservedOpen = Math.max(0, Number(view.unservedOpen) || 0) + added;
    if (Number.isFinite(view.openCount)) {
      view.openCount += added;
      setRepoTabCount("issues", view.openCount);
      if (Number.isFinite(view.closedCount)) {
        setRepoCollectionCounts("issues", view.openCount, view.closedCount);
      }
      return;
    }
    const badge = $('[data-dashboard-repo-tab-count="issues"]');
    const shown = Number(String(badge?.textContent || "").replace(/\D/g, ""));
    setRepoTabCount("issues", (Number.isFinite(shown) ? shown : 0) + added);
  }

  function renderCurrentWebIssueDetail() {
    const detail = state.repoRecordDetail;
    if (detail?.kind !== "issues" || !detail.repo) return;
    const container = $("[data-repo-issues]");
    if (!container) return;
    paintRepoIssueDetail(container, renderRepoRecordDetail(
      detail.repo,
      "issues",
      detail.number,
      detail.parsed,
    ));
    mountIssueStickyHeader(container);
    decorateIssueModeration(container, detail.repo, detail.number, detail.parsed);
    window.lucide?.createIcons();
  }

  function webIssueActionError(error, action = "") {
    const code = String(error?.message || "");
    // A claim never needs repository authority, so "not_authorized" there means
    // the account session behind it is gone, not that the viewer lacks a role.
    if (code === "not_authorized" && (action === "claim" || action === "release")) {
      return "Your sign-in has expired. Sign in again to change your assignment.";
    }
    if (moderationRefusalText(code)) return moderationRefusalText(code);
    if (code === "not_authorized" && (action === "edit-comment" || action === "delete-comment")) {
      return "Only the comment's author, the repository owner or an organization admin can change this comment.";
    }
    return ({
      not_authorized: "Only the repository owner or an organization admin can do that.",
      bad_signature: "The signed issue action could not be verified.",
      inbox_full: "Issue delivery is temporarily full. Try again later.",
      author_quota: "Too many issue changes are waiting to sync. Try again after the owner node drains them.",
      issue_too_large: "That issue update is too large.",
      number_unavailable: "A durable sub-issue number could not be reserved. Try again.",
    })[code] || "The issue could not be updated. Please try again.";
  }

  // Fold one signed "claim" into an assignee list. A claim is a delta naming a
  // single account, not a replacement for the whole list — that is exactly what
  // makes it safe to let a public contributor sign one for themselves (issue
  // #570), because it cannot drop anybody else's name. Matching is
  // case-insensitive, like the relay's account check; the stored spelling is
  // whatever was signed.
  function webIssueClaimApplied(assignees, event) {
    const name = String(event?.assignee || "").trim();
    const current = (Array.isArray(assignees) ? assignees : []).map(String);
    if (!name) return current;
    const isName = (value) => value.trim().toLowerCase() === name.toLowerCase();
    if (event?.claimState === "released") return current.filter((v) => !isName(v));
    return current.some(isName) ? current : [...current, name];
  }

  // The assignee list a viewer should see, given the serving node's projection
  // plus the signed events it carries. Claims recorded AFTER the last full
  // "assignees" event are re-folded here so a self-assignment shows up even
  // when the mirror that served this issue has not learned to project claims
  // yet; re-folding is idempotent against a node that has, and stopping at the
  // last "assignees" event keeps a maintainer's later replacement authoritative.
  function webIssueAssigneesWithClaims(assignees, events) {
    const log = Array.isArray(events) ? events : [];
    let start = 0;
    log.forEach((event, index) => {
      if (event?.type === "assignees") start = index + 1;
    });
    return log.slice(start).reduce(
      (list, event) => (event?.type === "claim" ? webIssueClaimApplied(list, event) : list),
      (Array.isArray(assignees) ? assignees : []).map(String),
    );
  }

  function splitWebIssueList(value) {
    return [...new Set(String(value || "").split(",")
      .map((item) => item.trim())
      .filter(Boolean))].slice(0, 20);
  }

  async function handleWebPullCommentAction(action, target) {
    const detail = state.repoRecordDetail;
    const repo = detail?.repo;
    const number = Number(detail?.number || 0);
    if (!repo || detail?.kind !== "pulls" || !number || !target) return;
    if (!state.session?.nodeName) {
      location.href = "/login?next="
        + encodeURIComponent(`${location.pathname}${location.search}`);
      return;
    }
    const parsed = detail.parsed || {};
    const original = foldPullCommentChanges(parsed.pullConversation)
      .find((ev) => String(ev.id || "") === String(target));
    if (!original) return;
    let result;
    try {
      if (action === "delete") {
        const verdict = pullReviewKeepsVerdict(original);
        if (!await confirmAccountAction(verdict ? {
          title: "Delete this review's text?",
          body: `Your ${original.state === "approved" ? "approval" : "change request"} stays on the pull request. Its Git history remains recoverable.`,
          confirmLabel: "Delete text",
        } : {
          title: original.type === "review" ? "Delete this review?" : "Delete this comment?",
          body: "Its Git history remains recoverable.",
          confirmLabel: original.type === "review" ? "Delete review" : "Delete comment",
        })) return;
        result = await submitWebPullCommentDelete(repo, number, target);
      } else {
        const nextBody = await openIssueCommentEditor(
          target, original.body || "",
          "[data-repo-pull-comment-edit]", "repoPullCommentEdit");
        if (nextBody === null) return;
        const trimmed = String(nextBody).replace(/[\r\n]+$/, "");
        if (!trimmed.trim() || trimmed === String(original.body || "").replace(/[\r\n]+$/, "")) return;
        result = await submitWebPullCommentEdit(repo, number, target, trimmed);
      }
    } catch (error) {
      showErrorNotice(webIssueActionError(error, action === "delete" ? "delete-comment" : "edit-comment"));
      return;
    }
    if (state.repoRecordDetail !== detail) return;
    parsed.pullConversation = [...(parsed.pullConversation || []), result.event];
    refreshPullConversationProjection();
    refreshRepoPullDiff();
    if (action !== "delete") restoreCommentEditFocus(target, "[data-repo-pull-comment-edit]", "repoPullCommentEdit");
  }

  async function handleWebIssueAction(action, target = "") {
    const detail = state.repoRecordDetail;
    const repo = detail?.repo;
    const number = Number(detail?.number || 0);
    if (!repo || detail?.kind !== "issues" || !number) return;
    if (!state.session?.nodeName) {
      location.href = "/login?next="
        + encodeURIComponent(`${location.pathname}${location.search}`);
      return;
    }
    let type = "";
    let fields = {};
    if (action === "subscribe" || action === "unsubscribe") {
      try {
        await setWebIssueSubscription(repo, number, action === "subscribe");
        detail.parsed.issueSubscribed = action === "subscribe";
        detail.parsed.issueSubscriptionReason = action === "subscribe" ? "subscribed" : "muted";
        renderCurrentWebIssueDetail();
      } catch (error) {
        showErrorNotice(webIssueActionError(error));
      }
      return;
    }
    if (action === "vote") type = "vote";
    if (action === "claim" && !detail.parsed?.issueMutationAuthorized) {
      const held = await webIssueClaimBlockers(repo, number);
      if (held.length) {
        showErrorNotice(issueAssignmentLimitMessage(held));
        return;
      }
    }
    if (action === "claim" || action === "release") {
      type = "claim";
      fields = {
        assignee: String(state.session.nodeName || ""),
        claimState: action === "claim" ? "assigned" : "released",
      };
    }
    const nextStatus = webIssueStatusForAction(action);
    if (nextStatus) {
      type = "status";
      fields = { status: nextStatus };
    }
    if (action === "delete-issue") {
      const title = String(detail.parsed?.values?.title || `issue #${number}`);
      if (!await confirmAccountAction({
        title: `Delete issue #${number} “${title}”?`,
        body: "The repository's node removes it everywhere once the signed deletion syncs. Its Git history keeps the record.",
        confirmLabel: "Delete issue",
      })) return;
      type = "delete";
      fields = { target: "self" };
    }
    if (action === "delete-comment") {
      if (!await confirmAccountAction({
        title: "Delete this comment?",
        body: "Its Git history remains recoverable.",
        confirmLabel: "Delete comment",
      })) return;
      type = "delete";
      fields = { target };
    }
    if (action === "edit-comment") {
      const events = detail.parsed?.issueEvents || [];
      const original = events.find((event) => event?.id === target);
      if (!original) return;
      const latest = events.filter(
        (event) => event?.type === "edit" && event.target === target,
      ).at(-1);
      const nextBody = await openIssueCommentEditor(target, latest?.body ?? original.body ?? "");
      if (nextBody === null) return;
      type = "edit";
      fields = { target, body: nextBody, attachments: latest?.attachments || original.attachments || [] };
    }
    if (!type) return;
    try {
      const result = await submitWebIssueEvent(repo, number, type, fields);
      optimisticWebIssueEvent(result.event);
      if (type === "status") syncWebIssueListStatus(number, fields.status);
      if (type === "claim") syncWebIssueListClaim(number, result.event);
      if (action === "delete-issue") {
        state.issuesView.items = (state.issuesView.items || []).filter(
          (issue) => Number(issue.number) !== number,
        );
        state.repoRecordDetail = null;
        navigateHistory(`${repoLinkUrl(repo)}/issues`);
        renderRepoIssues();
        return;
      }
      renderCurrentWebIssueDetail();
      if (action === "edit-comment") restoreCommentEditFocus(target);
    } catch (error) {
      showErrorNotice(webIssueActionError(error, action));
    }
  }

  function webIssueTaskError(error) {
    return ({
      org_member_required: "Only members of this site's organization can create tasks.",
      invalid_session: "Your sign-in has expired. Sign in again to move this issue.",
      office_organization_unavailable: "Organization tasks are unavailable right now. Try again later.",
      payload_too_large: "This issue is too large to move into a task.",
    })[String(error?.code || "")] || "The task could not be created, so the issue was kept.";
  }

  function webIssueTaskBody(repo, number, values, body) {
    const owner = repoPublicOwner(repo) || String(repo.owner || "");
    const name = String(repo.name || "");
    const segment = /^[A-Za-z0-9._-]{1,100}$/;
    const repository = segment.test(owner) && segment.test(name) ? `${owner}/${name}` : "";
    const source = `Moved from issue ${repository || name}#${number} by ${state.session?.nodeName || "a repository manager"}.`;
    const text = String(body || "").trim();
    const room = 4000 - source.length - 2;
    return {
      title: String(values.title || `Issue #${number}`).trim().slice(0, 160),
      details: text ? `${text.length > room ? `${text.slice(0, room - 1)}…` : text}\n\n${source}` : source,
      repository,
      department: "general",
      destination: "department",
      assigneeKind: "unassigned",
    };
  }

  async function moveWebIssueToTask(button = null) {
    const detail = state.repoRecordDetail;
    const repo = detail?.repo;
    const number = Number(detail?.number || 0);
    if (!repo || detail?.kind !== "issues" || !number) return;
    if (!state.session?.nodeName) {
      location.href = "/login?next="
        + encodeURIComponent(`${location.pathname}${location.search}`);
      return;
    }
    const values = detail.parsed?.values || {};
    const title = String(values.title || `issue #${number}`);
    if (!await confirmAccountAction({
      title: `Move issue #${number} “${title}” to a task?`,
      body: "An organization task is created from its title and description, then the issue is deleted. If the issue cannot be deleted, the new task is moved to trash so nothing is left twice.",
      confirmLabel: "Move to task",
    })) return;
    if (button) button.disabled = true;
    let task = null;
    try {
      const created = await orgApiRequest(
        "POST", "/api/tasks", webIssueTaskBody(repo, number, values, detail.parsed?.body));
      task = created.task;
    } catch (error) {
      if (button) button.disabled = false;
      showErrorNotice(webIssueTaskError(error));
      return;
    }
    try {
      const result = await submitWebIssueEvent(repo, number, "delete", { target: "self" });
      optimisticWebIssueEvent(result.event);
    } catch (error) {
      const undone = await orgApiRequest(
        "DELETE", `/api/tasks/${encodeURIComponent(task.id)}`,
        { reason: `Issue #${number} could not be deleted.` },
      ).then(() => true, () => false);
      if (button) button.disabled = false;
      showErrorNotice(`${webIssueActionError(error, "delete-issue")} ${undone
        ? "The new task was moved to trash, so the issue is unchanged."
        : "The new task could not be removed; delete it from Tasks."}`);
      return;
    }
    state.issuesView.items = (state.issuesView.items || []).filter(
      (issue) => Number(issue.number) !== number,
    );
    state.repoRecordDetail = null;
    location.assign(`/dashboard/tasks?task=${encodeURIComponent(task.id)}`);
  }

  async function handleWebIssueTitleSubmit(form) {
    const detail = state.repoRecordDetail;
    const title = String(
      form.querySelector("[data-repo-issue-title-input]")?.value || "",
    ).trim();
    if (!title) return;
    try {
      const result = await submitWebIssueEvent(
        detail.repo,
        detail.number,
        "title",
        { title },
      );
      optimisticWebIssueEvent(result.event);
      renderCurrentWebIssueDetail();
    } catch (error) {
      showErrorNotice(webIssueActionError(error));
    }
  }

  async function handleWebIssueDescriptionSubmit(form) {
    const detail = state.repoRecordDetail;
    const body = String(
      form.querySelector("[data-repo-issue-description-input]")?.value || "",
    );
    try {
      const result = await submitWebIssueEvent(
        detail.repo,
        detail.number,
        "edit",
        {
          target: detail.parsed.issueOpenEventId,
          body,
          attachments: detail.parsed.issueOpenAttachments || [],
        },
      );
      optimisticWebIssueEvent(result.event);
      renderCurrentWebIssueDetail();
    } catch (error) {
      showErrorNotice(webIssueActionError(error));
    }
  }

  let issueTaskMutationPending = false;
  let issueTaskDrag = null;

  function setIssueTaskControlsBusy(busy) {
    const taskList = document.querySelector("[data-repo-issue-task-list]");
    taskList?.setAttribute("aria-busy", busy ? "true" : "false");
    document.querySelectorAll(
      "[data-repo-issue-task-toggle], [data-repo-issue-task-drag], [data-repo-issue-create-subissues]",
    ).forEach((control) => {
      control.disabled = busy;
      if (control.matches("[data-repo-issue-task-drag]")) {
        control.draggable = !busy;
      }
    });
  }

  function setIssueTaskRowChecked(row, checked) {
    if (!row) return;
    row.classList.toggle("is-complete", checked);
    const toggle = row.querySelector("[data-repo-issue-task-toggle]");
    if (toggle) {
      toggle.checked = checked;
      const label = String(toggle.getAttribute("aria-label") || "")
        .replace(/^(?:Mark complete|Mark incomplete)/, checked ? "Mark incomplete" : "Mark complete");
      toggle.setAttribute("aria-label", label);
    }
  }

  // The checkbox is a little ForkMesh constellation: completing a task sends
  // ten colored nodes out from the box; reopening reels them back in. Motion is
  // decorative and omitted for people who request reduced motion.
  function playIssueTaskToggleMotion(toggle, checked) {
    const row = toggle?.closest("[data-repo-issue-task-row]");
    const control = toggle?.closest(".issue-task-control");
    if (!row || !control) return;
    row.classList.remove("is-checking", "is-unchecking");
    void row.offsetWidth;
    row.classList.add(checked ? "is-checking" : "is-unchecking");
    window.setTimeout(
      () => row.classList.remove("is-checking", "is-unchecking"),
      760,
    );
    if (window.matchMedia?.("(prefers-reduced-motion: reduce)").matches) return;
    control.querySelector(".issue-task-burst")?.remove();
    const burst = document.createElement("span");
    burst.className = `issue-task-burst ${checked ? "is-bursting" : "is-rewinding"}`;
    burst.setAttribute("aria-hidden", "true");
    for (let index = 0; index < 10; index += 1) {
      const particle = document.createElement("span");
      particle.style.setProperty("--task-angle", `${index * 36}deg`);
      particle.style.setProperty("--task-hue", String(138 + (index * 19)));
      particle.style.setProperty("--task-delay", `${index * 12}ms`);
      burst.appendChild(particle);
    }
    control.appendChild(burst);
    window.setTimeout(() => burst.remove(), 900);
  }

  async function submitWebIssueTaskBody(body) {
    const detail = state.repoRecordDetail;
    if (!detail?.repo || detail.kind !== "issues" || !detail.number) {
      throw new Error("invalid_issue_number");
    }
    return submitWebIssueEvent(
      detail.repo,
      detail.number,
      "edit",
      {
        target: detail.parsed.issueOpenEventId,
        body,
        attachments: detail.parsed.issueOpenAttachments || [],
      },
    );
  }

  async function handleWebIssueTaskToggle(toggle) {
    if (!toggle || issueTaskMutationPending) return;
    const detail = state.repoRecordDetail;
    const body = String(detail?.parsed?.body || "");
    const line = Number(toggle.dataset.repoIssueTaskToggle);
    const lines = body.split("\n");
    const current = /^(?:\s*(?:[-*+]\s+)?\[)([ xX])\]/.exec(lines[line] || "");
    const checked = Boolean(toggle.checked);
    if (!current) {
      renderCurrentWebIssueDetail();
      return;
    }
    const wasChecked = current[1].toLowerCase() === "x";
    const nextBody = setIssueTaskChecked(body, line, checked);
    if (nextBody === null || checked === wasChecked) return;

    issueTaskMutationPending = true;
    const row = toggle.closest("[data-repo-issue-task-row]");
    const status = row?.querySelector("[data-repo-issue-task-status]");
    setIssueTaskRowChecked(row, checked);
    playIssueTaskToggleMotion(toggle, checked);
    if (status) status.textContent = checked ? "Marking complete…" : "Reopening task…";
    setIssueTaskControlsBusy(true);
    const motionDone = new Promise((resolve) => window.setTimeout(resolve, 680));
    try {
      const result = await submitWebIssueTaskBody(nextBody);
      optimisticWebIssueEvent(result.event);
      if (status) status.textContent = checked ? "Task complete." : "Task reopened.";
      await motionDone;
      renderCurrentWebIssueDetail();
    } catch (error) {
      setIssueTaskRowChecked(row, wasChecked);
      playIssueTaskToggleMotion(toggle, wasChecked);
      if (status) status.textContent = "Could not save the task.";
      showErrorNotice(webIssueActionError(error));
    } finally {
      issueTaskMutationPending = false;
      setIssueTaskControlsBusy(false);
    }
  }

  async function handleWebIssueTaskReorder(sourceRow, targetRow, placeAfter) {
    if (!sourceRow || !targetRow || sourceRow === targetRow || issueTaskMutationPending) return;
    if (sourceRow.dataset.repoIssueTaskGroup !== targetRow.dataset.repoIssueTaskGroup) return;
    const detail = state.repoRecordDetail;
    const body = String(detail?.parsed?.body || "");
    const nextBody = reorderIssueTaskBody(
      body,
      Number(sourceRow.dataset.repoIssueTaskLine),
      Number(targetRow.dataset.repoIssueTaskLine),
      Boolean(placeAfter),
    );
    if (nextBody === body) return;

    issueTaskMutationPending = true;
    if (placeAfter) targetRow.after(sourceRow);
    else targetRow.before(sourceRow);
    sourceRow.classList.add("is-task-settling");
    const status = sourceRow.querySelector("[data-repo-issue-task-status]");
    if (status) status.textContent = "Saving new position…";
    setIssueTaskControlsBusy(true);
    const motionDone = new Promise((resolve) => window.setTimeout(resolve, 320));
    try {
      const result = await submitWebIssueTaskBody(nextBody);
      optimisticWebIssueEvent(result.event);
      await motionDone;
      renderCurrentWebIssueDetail();
    } catch (error) {
      renderCurrentWebIssueDetail();
      showErrorNotice(webIssueActionError(error));
    } finally {
      issueTaskMutationPending = false;
      setIssueTaskControlsBusy(false);
    }
  }

  function clearIssueTaskDragState() {
    document.querySelectorAll(
      ".is-task-dragging, .is-task-drop-before, .is-task-drop-after",
    ).forEach((row) => row.classList.remove(
      "is-task-dragging", "is-task-drop-before", "is-task-drop-after",
    ));
  }

  function handleIssueTaskDragStart(event) {
    const handle = event.target.closest?.("[data-repo-issue-task-drag]");
    const row = handle?.closest("[data-repo-issue-task-row]");
    if (!row || issueTaskMutationPending) {
      event.preventDefault();
      return;
    }
    issueTaskDrag = { row, group: row.dataset.repoIssueTaskGroup };
    row.classList.add("is-task-dragging");
    if (event.dataTransfer) {
      event.dataTransfer.effectAllowed = "move";
      event.dataTransfer.setData("text/plain", row.dataset.repoIssueTaskLine || "");
    }
  }

  function handleIssueTaskDragOver(event) {
    if (!issueTaskDrag) return;
    const targetRow = event.target.closest?.("[data-repo-issue-task-row]");
    if (!targetRow || targetRow === issueTaskDrag.row
        || targetRow.dataset.repoIssueTaskGroup !== issueTaskDrag.group) return;
    event.preventDefault();
    clearIssueTaskDragState();
    issueTaskDrag.row.classList.add("is-task-dragging");
    const bounds = targetRow.getBoundingClientRect();
    targetRow.classList.add(
      event.clientY >= bounds.top + (bounds.height / 2)
        ? "is-task-drop-after"
        : "is-task-drop-before",
    );
    if (event.dataTransfer) event.dataTransfer.dropEffect = "move";
  }

  function handleIssueTaskDrop(event) {
    if (!issueTaskDrag) return;
    const targetRow = event.target.closest?.("[data-repo-issue-task-row]");
    const sourceRow = issueTaskDrag.row;
    const sameGroup = targetRow
      && targetRow !== sourceRow
      && targetRow.dataset.repoIssueTaskGroup === issueTaskDrag.group;
    if (sameGroup) {
      event.preventDefault();
      const placeAfter = targetRow.classList.contains("is-task-drop-after");
      clearIssueTaskDragState();
      issueTaskDrag = null;
      void handleWebIssueTaskReorder(sourceRow, targetRow, placeAfter);
      return;
    }
    clearIssueTaskDragState();
    issueTaskDrag = null;
  }

  function handleIssueTaskDragEnd() {
    clearIssueTaskDragState();
    issueTaskDrag = null;
  }

  function handleIssueTaskReorderKey(event) {
    const handle = event.target.closest?.("[data-repo-issue-task-drag]");
    if (!handle || !["ArrowUp", "ArrowDown"].includes(event.key)) return false;
    const row = handle.closest("[data-repo-issue-task-row]");
    const peers = [...(row?.parentElement?.children || [])].filter(
      (candidate) => candidate.matches?.("[data-repo-issue-task-row]")
        && candidate.dataset.repoIssueTaskGroup === row.dataset.repoIssueTaskGroup,
    );
    const index = peers.indexOf(row);
    const target = event.key === "ArrowUp" ? peers[index - 1] : peers[index + 1];
    if (!target) return false;
    event.preventDefault();
    void handleWebIssueTaskReorder(row, target, event.key === "ArrowDown");
    return true;
  }

  async function handleWebIssueSubtasks(button) {
    const detail = state.repoRecordDetail;
    const repo = detail?.repo;
    const parentNumber = Number(detail?.number || 0);
    const parentBody = String(detail?.parsed?.body || "");
    const requestedLine = button?.hasAttribute("data-repo-issue-task-subissue")
      ? Number(button.dataset.repoIssueTaskSubissue)
      : null;
    const selectedCandidate = requestedLine === null
      ? null
      : issueSubtaskCandidateAtLine(parentBody, requestedLine);
    const candidates = requestedLine === null
      ? issueSubtaskCandidates(parentBody)
      : (selectedCandidate ? [selectedCandidate] : []);
    if (!repo || detail?.kind !== "issues" || !parentNumber || !candidates.length) {
      return;
    }
    if (issueTaskMutationPending) return;
    const prompt = requestedLine !== null
      ? "Turn this task into an attached sub-issue? The child will link back here and this row will keep its issue number."
      : candidates.length === 1
      ? "Create one sub-issue from this unchecked task? The child will link back here and the task will gain its issue number."
      : `Create ${candidates.length} sub-issues from the unchecked tasks? Each child will link back here and each task will gain its issue number.`;
    if (!await confirmAccountAction({
      title: requestedLine !== null ? "Turn this task into a sub-issue?" : "Create sub-issues?",
      body: prompt,
      confirmLabel: requestedLine !== null || candidates.length === 1 ? "Create sub-issue" : "Create sub-issues",
      danger: false,
    })) return;

    issueTaskMutationPending = true;
    setIssueTaskControlsBusy(true);
    if (button) {
      button.disabled = true;
      button.textContent = "Creating sub-issues…";
    }
    const linked = [];
    let failure = null;
    for (const candidate of candidates) {
      try {
        const childBody = `Parent issue: #${parentNumber}`;
        const result = await submitWebIssue(repo, candidate.title, childBody);
        const childNumber = Number(result.number || 0);
        if (!Number.isInteger(childNumber) || childNumber <= 0) {
          throw new Error("number_unavailable");
        }
        linked.push({ ...candidate, number: childNumber });
        const author = result.event.authorName || state.session?.nodeName || "you";
        const item = {
          number: childNumber,
          title: candidate.title,
          status: "open",
          state: "open",
          author,
          date: "just now",
          meta: `parent: #${parentNumber}`,
          body: childBody,
        };
        state.issuesView.items = [
          item,
          ...(state.issuesView.items || []).filter(
            (issue) => Number(issue.number) !== childNumber),
        ];
        rememberOptimisticRepoRecord(repo, "issues", childNumber, {
          values: {
            title: candidate.title,
            status: "open",
            author: result.event.author,
            authorName: author,
            createdAt: result.event.ts,
          },
          body: childBody,
          issueEvents: [result.event],
          issueCreator: result.event.author,
          issueOpenEventId: result.event.id,
          issueOpenAttachments: [],
        });
      } catch (error) {
        failure = error;
        break;
      }
    }

    if (linked.length) {
      try {
        const result = await submitWebIssueEvent(
          repo,
          parentNumber,
          "edit",
          {
            target: detail.parsed.issueOpenEventId,
            body: linkIssueSubtasks(parentBody, linked),
            attachments: detail.parsed.issueOpenAttachments || [],
          },
        );
        optimisticWebIssueEvent(result.event);
      } catch (error) {
        failure ||= error;
      }
    }

    if (linked.length) {
      setRepoTabCount(
        "issues",
        (state.issuesView.items || []).filter(
          (issue) => issue.status !== "closed" && !issue.deleted).length,
      );
      renderCurrentWebIssueDetail();
    } else if (button) {
      if (requestedLine !== null) renderCurrentWebIssueDetail();
      else {
        button.disabled = false;
        button.textContent = candidates.length === 1
          ? "Create sub-issue"
          : `Create ${candidates.length} sub-issues`;
      }
    }
    issueTaskMutationPending = false;
    setIssueTaskControlsBusy(false);
    if (failure) {
      const prefix = linked.length
        ? `${linked.length} sub-issue${linked.length === 1 ? " was" : "s were"} created. `
        : "";
      showErrorNotice(prefix + webIssueActionError(failure));
    }
  }

  async function handleWebIssueMetadataSubmit(form) {
    const detail = state.repoRecordDetail;
    const values = detail?.parsed?.values || {};
    // The sidebar pickers submit a hidden form; its status line sits above
    // the sidebar sections rather than inside the form.
    const hint = form.querySelector("[data-repo-issue-metadata-hint]")
      || document.querySelector("[data-repo-issue-metadata-hint]");
    const submit = form.querySelector("[data-repo-issue-metadata-save]");
    const labels = splitWebIssueList(form.querySelector("[data-repo-issue-labels]")?.value);
    const assignees = splitWebIssueList(form.querySelector("[data-repo-issue-assignees]")?.value);
    const milestone = String(form.querySelector("[data-repo-issue-milestone-edit]")?.value || "").trim();
    const priority = Math.min(99, Math.max(0, Number(form.querySelector("[data-repo-issue-priority]")?.value) || 0));
    const progress = Math.min(100, Math.max(0, Number(form.querySelector("[data-repo-issue-progress]")?.value) || 0));
    const dateValue = (selector) => {
      const value = String(form.querySelector(selector)?.value || "");
      return value ? Date.parse(`${value}T00:00:00Z`) || 0 : 0;
    };
    const startDate = dateValue("[data-repo-issue-start-date]");
    const endDate = dateValue("[data-repo-issue-end-date]");
    const actions = [];
    if (labels.join(",") !== parseFrontMatterList(values.labels).join(",")) actions.push(["labels", { labels }]);
    // Compare against the same claim-folded list the form was seeded with, so
    // opening and saving the editor without touching assignees does not emit an
    // "assignees" event that silently reverts a contributor's self-assignment.
    const shownAssignees = webIssueAssigneesWithClaims(
      parseFrontMatterList(values.assignees), detail?.parsed?.issueEvents,
    );
    if (assignees.join(",") !== shownAssignees.join(",")) actions.push(["assignees", { assignees }]);
    if (milestone !== String(values.milestone || "")) actions.push(["milestone", { milestone }]);
    if (priority !== (Number(values.priority) || 0)) actions.push(["priority", { priority }]);
    if (progress !== (Number(values.progress) || 0)) actions.push(["progress", { progress }]);
    if (startDate !== (Number(values.startDate) || 0) || endDate !== (Number(values.endDate) || 0)) actions.push(["dates", { startDate, endDate }]);
    if (!actions.length) {
      if (hint) hint.textContent = "No changes";
      return;
    }
    if (submit) submit.disabled = true;
    if (hint) hint.textContent = "Signing…";
    try {
      for (const [type, fields] of actions) {
        const result = await submitWebIssueEvent(
          detail.repo,
          detail.number,
          type,
          fields,
        );
        optimisticWebIssueEvent(result.event);
      }
      renderCurrentWebIssueDetail();
    } catch (error) {
      if (submit) submit.disabled = false;
      if (hint) {
        hint.className = "text-[10px] text-destructive";
        hint.textContent = webIssueActionError(error);
      }
    }
  }

  async function handleDiscussionAnswerClick(repo, button) {
    if (!repo || !button) return;
    const number = Number(button.dataset.repoDiscussionAnswerNumber || 0);
    const target = String(button.dataset.repoDiscussionAnswer || "");
    const section = button.closest("[data-repo-record-conversation]");
    const hint = section?.querySelector("[data-repo-discussion-reply-hint]");
    const setHint = (text, bad) => {
      if (!hint) return;
      hint.className = `text-[11px] ${bad ? "text-destructive" : "text-muted-foreground"}`;
      hint.textContent = text;
    };
    if (!number) return;
    button.disabled = true;
    setHint("Signing and sending…");
    try {
      await submitWebDiscussionAnswer(repo, number, target);
      const detail = state.repoRecordDetail;
      const parsed = detail?.parsed;
      if (parsed && Number(detail.number) === number) {
        parsed.discussionAnswer = target ? { target } : null;
        const list = section?.querySelector("[data-repo-discussion-conversation]");
        if (list) {
          list.innerHTML = renderRepoDiscussionConversation(parsed.discussionConversation || [], {
            answer: parsed.discussionAnswer,
            canMark: parsed.discussionAnswerAuthorized,
            number,
          });
          window.lucide?.createIcons();
        }
      }
      setHint(target ? "Answer marked. It shows for everyone once the repository records it." : "Answer cleared.");
    } catch (error) {
      button.disabled = false;
      const code = String(error?.message || "");
      setHint(
        code === "bad_signature" ? "Could not verify the answer's signature."
          : code === "inbox_full" ? "Delivery is temporarily full. Try again later."
          : "Could not mark the answer. Please try again.",
        true,
      );
    }
  }

  async function handleDiscussionReplySubmit(repo, form) {
    if (!repo || !form) return;
    const number = Number(form.dataset.repoDiscussionReplyNumber || 0);
    const bodyInput = form.querySelector("[data-repo-discussion-reply-body]");
    const submit = form.querySelector("[data-repo-discussion-reply-submit]");
    const hint = form.querySelector("[data-repo-discussion-reply-hint]");
    const setHint = (text, tone) => {
      if (hint) hint.className = `text-[11px] ${tone === "bad" ? "text-destructive" : "text-muted-foreground"}`;
      if (hint) hint.textContent = text;
    };
    const body = String(bodyInput?.value || "").trim();
    if (!number) {
      setHint("This discussion hasn't finished loading yet.", "bad");
      return;
    }
    if (!body) {
      setHint("Write a reply before sending.", "bad");
      bodyInput?.focus();
      return;
    }
    if (submit) submit.disabled = true;
    setHint("Signing and sending…");
    try {
      await submitWebDiscussionComment(repo, number, body);
      const list = form.parentElement?.querySelector("[data-repo-discussion-conversation]");
      if (list) {
        if (list.dataset.empty === "true") list.innerHTML = "";
        list.dataset.empty = "false";
        list.insertAdjacentHTML("beforeend", renderPullConversationEvent({
          type: "comment", authorName: state.session?.nodeName || "you",
          ts: Math.floor(Date.now() / 1000), body,
        }));
      }
      if (bodyInput) bodyInput.value = "";
      if (submit) submit.disabled = false;
      setHint("Reply accepted for direct delivery to an eligible mirror.", "good");
    } catch (error) {
      if (submit) submit.disabled = false;
      const code = String(error?.message || "");
      setHint(
        code === "inbox_full" ? "Reply delivery is temporarily full. Try again later."
          : code === "discussion_locked" ? "This discussion was closed, so it no longer takes replies."
          : code === "author_quota" ? "You've reached the submission limit for this repository."
          : code === "discussion_too_large" ? "The reply is too large - please shorten it."
          : code === "bad_signature" ? "Could not verify the reply's signature."
          : "Could not send the reply. Please try again.",
        "bad");
    }
  }

  function webDiscussionSubmitErrorText(error) {
    const code = String(error?.message || error?.code || "");
    const status = Number(error?.status || 0);
    if (code === "signing_unavailable") return "This browser cannot sign discussions (Ed25519 is unsupported). Update your browser and try again.";
    if (code === "bad_signature") return "The discussion's signature was rejected. Reload the page and try again.";
    if (code === "discussion_too_large") return "The discussion is too large (64 KB at most) - please shorten it.";
    if (code === "inbox_full") return "This repository's discussion inbox is full right now. Try again later.";
    if (code === "author_quota") return "You have too many discussion posts waiting for this repository. Try again once they are published.";
    if (code === "feature_disabled") return "Discussions are turned off for this repository.";
    if (code === "not_authorized") return "Only the repository owner can do that.";
    if (code === "discussion_locked") return "This discussion was closed, so it no longer takes replies.";
    if (code === "not_found" || status === 404) return "This repository is not available to your account. It may be private, or not published yet.";
    if (code === "network_error") return `Could not reach ${platformName()} - check your connection.`;
    if (code === "timeout") return "The repository did not answer in time. The discussion may still arrive - check the list before sending it again.";
    return `Could not send the discussion (${code || `HTTP ${status || "error"}`}). Please try again.`;
  }

  async function handleDiscussionComposeSubmit(repo, form) {
    if (!repo || !form) return;
    const titleInput = form.querySelector("[data-repo-discussion-title]");
    const bodyInput = form.querySelector("[data-repo-discussion-body]");
    const submit = form.querySelector("[data-repo-discussion-submit]");
    const spinner = form.querySelector("[data-repo-discussion-spinner]");
    const hint = form.querySelector("[data-repo-discussion-hint]");
    const setHint = (text, bad) => {
      if (!hint) return;
      hint.className = `text-[12px] ${bad ? "text-destructive" : "text-muted-foreground"}`;
      hint.textContent = text;
    };
    const title = String(titleInput?.value || "").trim();
    const body = String(bodyInput?.value || "");
    const category = String(form.querySelector("[data-repo-discussion-category-input]:checked")?.value || "Ideas");
    if (!state.session?.nodeName) {
      location.href = "/login?next=" + encodeURIComponent(`${location.pathname}${location.search}`);
      return;
    }
    if (!title) {
      setHint("Enter a title for the discussion.", true);
      titleInput?.focus();
      return;
    }
    if (submit) submit.disabled = true;
    if (spinner) spinner.hidden = false;
    const progress = ({ phase, route, retry } = {}) => {
      if (phase === "signing") setHint("Signing your discussion in this browser…");
      else if (phase === "delivering" && route === "mirror") setHint(retry ? "Trying another mirror…" : "Sending it to a mirror of this repository…");
      else if (phase === "delivering") setHint(retry ? "Retrying the same signed discussion…" : "Sending it to the repository…");
    };
    try {
      const result = await submitWebDiscussion(repo, { title, body, category }, progress);
      const event = result.event;
      if (spinner) spinner.hidden = true;
      if (submit) submit.disabled = false;
      const number = Number(result.number || 0);
      if (Number.isInteger(number) && number > 0) {
        rememberOptimisticRepoRecord(repo, "discussions", number, {
          values: {
            title: event.title,
            category: event.category,
            status: "open",
            author: event.author,
            authorName: event.authorName,
            ts: event.ts,
            createdAt: event.ts,
          },
          body: event.body,
          discussionConversation: [],
          discussionAnswer: null,
        });
        const items = state.repoCollectionItems.discussions;
        if (Array.isArray(items) && !items.some((item) => Number(item.number) === number)) {
          items.unshift({
            number, title: event.title, category: event.category, state: "open",
            author: event.authorName || state.session?.nodeName || "", authorKey: event.author,
            createdAtMs: event.ts, updatedAtMs: event.ts,
          });
        }
        navigateHistory(repoRecordUrl(repo, "discussions", number, "conversation"));
        await loadRepoRecordDetail(repo, "discussions", number);
        return;
      }
      if (titleInput) titleInput.value = "";
      if (bodyInput) bodyInput.value = "";
      renderRepoDiscussions();
      const container = $("[data-repo-discussions]");
      container?.insertAdjacentHTML("afterbegin", `<div data-repo-discussion-pending role="status" class="flex items-start gap-2 border-b border-border bg-[color-mix(in_srgb,var(--dashboard-link)_8%,transparent)] px-4 py-3 text-[14px] text-foreground"><i data-lucide="clock" class="mt-0.5 h-4 w-4 shrink-0 text-muted-foreground"></i><span><span class="font-semibold">${escapeHtml(event.title)}</span> was accepted. It appears here, numbered, as soon as a mirror of this repository records it.</span></div>`);
      window.lucide?.createIcons();
    } catch (error) {
      if (spinner) spinner.hidden = true;
      if (submit) submit.disabled = false;
      console.error("[forkmesh] discussion submission failed", error);
      setHint(`${webDiscussionSubmitErrorText(error)} Your draft is still here.`, true);
    }
  }

  async function handleDiscussionStatusClick(repo, button) {
    if (!repo || !button) return;
    const number = Number(button.dataset.repoDiscussionStatusNumber || 0);
    const status = String(button.dataset.repoDiscussionStatus || "");
    const bar = button.closest("[data-repo-discussion-moderation]");
    const hint = bar?.querySelector("[data-repo-discussion-moderation-hint]");
    if (!number || !status) return;
    bar?.querySelectorAll("button").forEach((control) => { control.disabled = true; });
    if (hint) {
      hint.className = "mr-auto text-[12px] text-muted-foreground";
      hint.textContent = "Signing and sending…";
    }
    try {
      const result = await submitWebDiscussionModeration(repo, number, "status", status);
      rememberPendingDiscussionState(repo, number, result.event);
      const detail = state.repoRecordDetail;
      if (detail?.kind === "discussions" && Number(detail.number) === number && detail.parsed) {
        detail.parsed.values = { ...(detail.parsed.values || {}), status };
        rememberOptimisticRepoRecord(repo, "discussions", number, detail.parsed);
        const container = $("[data-repo-discussions]");
        if (container) {
          container.innerHTML = renderRepoRecordDetail(repo, "discussions", number, detail.parsed);
          window.lucide?.createIcons();
        }
      }
      const items = state.repoCollectionItems.discussions;
      const row = Array.isArray(items) ? items.find((item) => Number(item.number) === number) : null;
      if (row) row.state = status;
    } catch (error) {
      bar?.querySelectorAll("button").forEach((control) => { control.disabled = false; });
      if (hint) {
        hint.className = "mr-auto text-[12px] text-destructive";
        hint.textContent = webDiscussionSubmitErrorText(error);
      }
    }
  }

  async function handleDiscussionRemoveClick(repo, button) {
    if (!repo || !button) return;
    const number = Number(button.dataset.repoDiscussionRemove || 0);
    if (!number) return;
    if (!window.confirm(`Delete discussion #${number} and all its replies? This cannot be undone.`)) return;
    const bar = button.closest("[data-repo-discussion-moderation]");
    const hint = bar?.querySelector("[data-repo-discussion-moderation-hint]");
    bar?.querySelectorAll("button").forEach((control) => { control.disabled = true; });
    try {
      const result = await submitWebDiscussionModeration(repo, number, "delete", "#discussion");
      rememberPendingDiscussionState(repo, number, result.event);
      forgetOptimisticRepoRecord(repo, "discussions", number);
      const items = state.repoCollectionItems.discussions;
      if (Array.isArray(items)) {
        state.repoCollectionItems.discussions = items.filter((item) => Number(item.number) !== number);
      }
      navigateHistory(`${repoLinkUrl(repo)}/discussions`);
      renderRepoDiscussions();
    } catch (error) {
      bar?.querySelectorAll("button").forEach((control) => { control.disabled = false; });
      if (hint) {
        hint.className = "mr-auto text-[12px] text-destructive";
        hint.textContent = webDiscussionSubmitErrorText(error);
      }
    }
  }

  async function handleDiscussionDeleteClick(repo, button) {
    if (!repo || !button) return;
    const number = Number(button.dataset.repoDiscussionDeleteNumber || 0);
    const target = String(button.dataset.repoDiscussionDelete || "");
    if (!number || !target) return;
    if (!window.confirm("Delete this reply? It is removed from the repository for everyone.")) return;
    button.disabled = true;
    const reply = button.closest("[data-repo-discussion-reply]");
    try {
      await submitWebDiscussionModeration(repo, number, "delete", target);
      const detail = state.repoRecordDetail;
      if (detail?.kind === "discussions" && Number(detail.number) === number && detail.parsed) {
        detail.parsed.discussionConversation = (detail.parsed.discussionConversation || [])
          .filter((ev) => ev.id !== target);
      }
      reply?.remove();
    } catch (error) {
      button.disabled = false;
      showErrorNotice(webDiscussionSubmitErrorText(error));
    }
  }

  function sessionFromAccountPayload(body, base = {}) {
    rememberUserNameCase(body.nodeName || body.name || base.nodeName || "", body.nameCase);
    const nextSession = {
      ...(base || {}),
      nodeName: body.nodeName || body.name || base.nodeName || "",
      nameCase: body.nameCase ?? base.nameCase ?? "",
      email: body.email ?? base.email ?? "",
      status: body.status || base.status || "active",
      pubkey: body.pubkey || base.pubkey || "",
      emailVerified: Object.prototype.hasOwnProperty.call(body, "emailVerified")
        ? Boolean(body.emailVerified)
        : Boolean(base.emailVerified),
      isAdmin: Object.prototype.hasOwnProperty.call(body, "isAdmin")
        ? Boolean(body.isAdmin)
        : Boolean(base.isAdmin),
      adminUrl: body.adminUrl || base.adminUrl || "",
      solana: body.solana || base.solana || "",
      hasPayoutAddress: Object.prototype.hasOwnProperty.call(body, "hasPayoutAddress")
        ? Boolean(body.hasPayoutAddress)
        : Boolean(base.hasPayoutAddress),
      sessionToken: body.sessionToken ?? base.sessionToken ?? "",
      avatarPng: body.avatarPng || "",
      avatarUpdatedAt: Number(body.avatarUpdatedAt) || 0,
      createdAt: Number(body.createdAt ?? base.createdAt ?? 0) || 0,
      profileBio: body.profileBio ?? base.profileBio ?? "",
      userStatus: body.userStatus ?? base.userStatus ?? null,
      profileAbout: body.profileAbout ?? body.profileReadme ?? base.profileAbout ?? base.profileReadme ?? "",
      profileReadme: body.profileReadme ?? body.profileAbout ?? base.profileReadme ?? base.profileAbout ?? "",
      profileLocation: body.profileLocation ?? base.profileLocation ?? "",
      profileTimezone: body.profileTimezone ?? base.profileTimezone ?? "",
      profileFollowers: Number(body.followers ?? body.profileFollowers ?? base.profileFollowers ?? 0) || 0,
      profileFollowing: Number(body.following ?? body.profileFollowing ?? base.profileFollowing ?? 0) || 0,
      profileMirrorCount: Number(body.mirrorCount ?? body.profileMirrorCount ?? base.profileMirrorCount ?? 0) || 0,
      referralClicks: Number(body.referralClicks ?? base.referralClicks ?? 0) || 0,
      creditedReferrals: Number(body.creditedReferrals ?? base.creditedReferrals ?? 0) || 0,
      referralLastAt: Number(body.referralLastAt ?? base.referralLastAt ?? 0) || 0,
      referredUsers: Array.isArray(body.referredUsers)
        ? body.referredUsers
        : (base.referredUsers || []),
      legacyUnattributed: Number(body.legacyUnattributed ?? base.legacyUnattributed ?? 0) || 0,
      profilePrivate: Object.prototype.hasOwnProperty.call(body, "profilePrivate")
        ? Boolean(body.profilePrivate)
        : Boolean(base.profilePrivate),
      followersPublic: Object.prototype.hasOwnProperty.call(body, "followersPublic")
        ? Boolean(body.followersPublic)
        : Boolean(base.followersPublic),
      publicEmail: body.publicEmail ?? base.publicEmail ?? "",
      mastodon: body.mastodon ?? base.mastodon ?? "",
      mastodonUrl: body.mastodonUrl ?? base.mastodonUrl ?? "",
      profileLinks: Array.isArray(body.profileLinks)
        ? body.profileLinks
        : (base.profileLinks || []),
      emailNotifications: Object.prototype.hasOwnProperty.call(body, "emailNotifications")
        ? Boolean(body.emailNotifications)
        : Boolean(base.emailNotifications ?? true),
      notificationPreferences: body.notificationPreferences && typeof body.notificationPreferences === "object"
        ? body.notificationPreferences
        : (base.notificationPreferences || {}),
      notificationDelivery: body.notificationDelivery || base.notificationDelivery || "digest",
      notificationPauseUntil: Number(
        Object.prototype.hasOwnProperty.call(body, "notificationPauseUntil")
          ? body.notificationPauseUntil
          : base.notificationPauseUntil,
      ) || 0,
      // The Pings settings page (#347) paints from these before its own read.
      pingMutedKinds: Array.isArray(body.pingMutedKinds)
        ? body.pingMutedKinds
        : (Array.isArray(base.pingMutedKinds) ? base.pingMutedKinds : []),
      notificationQuietHours: body.notificationQuietHours && typeof body.notificationQuietHours === "object"
        ? body.notificationQuietHours
        : (base.notificationQuietHours || null),
      notificationQuietUntil: Number(body.notificationQuietUntil ?? base.notificationQuietUntil ?? 0) || 0,
      pingKindCatalog: Array.isArray(body.pingKindCatalog)
        ? body.pingKindCatalog
        : (Array.isArray(base.pingKindCatalog) ? base.pingKindCatalog : null),
      kind: body.kind || base.kind || "",
      owner: body.owner ?? base.owner ?? "",
      online: Object.prototype.hasOwnProperty.call(body, "online")
        ? Boolean(body.online)
        : Boolean(base.online),
      nodes: Array.isArray(body.nodes) ? body.nodes : (base.nodes || []),
      accountSnapshotVersion: 1,
      at: Date.now(),
    };
    if (!Object.prototype.hasOwnProperty.call(body, "avatarPng")) {
      nextSession.avatarPng = base.avatarPng || "";
    }
    if (!Object.prototype.hasOwnProperty.call(body, "avatarUpdatedAt")) {
      nextSession.avatarUpdatedAt = Number(base.avatarUpdatedAt) || 0;
    }
    return nextSession;
  }

  function applyAvatar(container, session) {
    if (!container) return;
    const name = session?.nodeName || session?.email || platformName();
    const initial = (name[0] || "F").toUpperCase();
    const avatarPng = session?.avatarPng || "";
    const image = container.querySelector("img");
    const initialEl = container.querySelector("[data-avatar-initial]");
    if (image) {
      if (avatarPng) {
        image.src = `data:image/png;base64,${avatarPng}`;
        image.alt = `${name} profile picture`;
      } else {
        image.removeAttribute("src");
        image.alt = "";
      }
      image.classList.toggle("hidden", !avatarPng);
      image.hidden = !avatarPng;
    }
    if (initialEl) {
      initialEl.textContent = initial;
      initialEl.classList.toggle("hidden", Boolean(avatarPng));
    } else if (!image) {
      container.textContent = initial;
    }
    container.classList.toggle("font-mono", !avatarPng);
    const handle = profileAvatarHandle(session);
    if (container.dataset.profileAvatarFor !== handle
      || ["image", "bot"].includes(container.dataset.profileAvatarState)) {
      delete container.dataset.profileAvatarState;
      delete container.dataset.profileAvatarTried;
    }
    if (avatarPng || !handle || !image) {
      delete container.dataset.profileAvatarFor;
      return;
    }
    container.dataset.profileAvatarFor = handle;
    hydrateProfileAvatar(container);
  }

  const PROFILE_AVATAR_ROSTER_REPOS = 4;

  function profileAvatarHandle(session) {
    const handle = String(session?.nodeName || "").trim().replace(/^@/, "").toLowerCase();
    return session?.kind !== "organization" && validNodeName(handle) ? handle : "";
  }

  function profileAvatarRosterRepos(handle) {
    const seen = new Set();
    const own = [];
    const others = [];
    (Array.isArray(state.repositories) ? state.repositories : []).forEach((repo) => {
      if (!repo?.name || repo.isPrivate) return;
      const key = repositoryViewKey(repo);
      if (seen.has(key)) return;
      seen.add(key);
      const owners = [repo.owner, repoPublicOwner(repo)].map((value) => String(value || "").toLowerCase());
      (owners.includes(handle) ? own : others).push(repo);
    });
    const parents = [];
    own.forEach((repo) => {
      const parentKey = repoForkParent(repo)?.key;
      const parent = parentKey ? findRepository(parentKey) : null;
      if (!parent || parent.isPrivate || parents.includes(parent)) return;
      parents.push(parent);
    });
    const ordered = [];
    own.concat(parents, others).forEach((repo) => {
      if (!ordered.includes(repo)) ordered.push(repo);
    });
    return ordered.slice(0, PROFILE_AVATAR_ROSTER_REPOS);
  }

  async function profileAvatarPicture(handle, repos) {
    const account = await issueActorAccount(handle);
    if (account?.png) return `data:image/png;base64,${account.png}`;
    if (typeof repoPeopleRosterMatch !== "function") return "";
    for (const repo of repos) {
      const person = await repoPeopleRosterMatch(repo, handle).catch(() => null);
      if (person?.avatarUrl) return person.avatarUrl;
    }
    return "";
  }

  function showProfileAvatarPicture(container, src) {
    const image = container.querySelector("img");
    const initialEl = container.querySelector("[data-avatar-initial]");
    if (!image || !src) return;
    image.addEventListener("error", () => {
      image.removeAttribute("src");
      image.classList.add("hidden");
      image.hidden = true;
      initialEl?.classList.remove("hidden");
      container.dataset.profileAvatarState = "initial";
    }, { once: true });
    image.referrerPolicy = "no-referrer";
    image.src = src;
    image.alt = `${container.dataset.profileAvatarFor} profile picture`;
    image.classList.remove("hidden");
    image.hidden = false;
    initialEl?.classList.add("hidden");
    container.classList.remove("font-mono");
  }

  function showProfileAvatarBot(container) {
    const kind = botAvatarKind(container.dataset.profileAvatarFor);
    const initialEl = container.querySelector("[data-avatar-initial]");
    if (!kind || !initialEl) return;
    initialEl.innerHTML = botAvatarSvg(kind, "h-auto w-1/2");
    initialEl.classList.add("flex", "h-full", "w-full", "items-center", "justify-center");
    container.dataset.profileAvatarState = "bot";
  }

  function hydrateProfileAvatar(container) {
    const handle = container?.dataset?.profileAvatarFor;
    if (!handle) return;
    const repos = profileAvatarRosterRepos(handle);
    const tried = repos.map((repo) => repositoryViewKey(repo)).join(",");
    const known = container.dataset.profileAvatarState;
    if (known === "image" || known === "loading" || known === "bot"
      || (known === "initial" && container.dataset.profileAvatarTried === tried)) return;
    container.dataset.profileAvatarState = "loading";
    container.dataset.profileAvatarTried = tried;
    void profileAvatarPicture(handle, repos).then((src) => {
      if (container.dataset.profileAvatarFor !== handle) return;
      container.dataset.profileAvatarState = src ? "image" : "initial";
      if (src) showProfileAvatarPicture(container, src);
      else showProfileAvatarBot(container);
    });
  }

  function hydrateProfileAvatars() {
    document.querySelectorAll("[data-profile-avatar-for]").forEach(hydrateProfileAvatar);
  }

  async function refreshPublicProfile(session = state.session, options = {}) {
    return hydrateCanonicalProfile(session, options);
  }

  async function hydrateCanonicalProfile(session, { force = false } = {}) {
    const nodeName = String(session?.nodeName || "").trim().toLowerCase();
    if (!validNodeName(nodeName)) return session;
    // Login already returns the same canonical account payload this endpoint
    // serves. Keep one fallback read for sessions written by older clients or
    // restored from a cross-origin cookie, then persist the version above so
    // subsequent dashboard documents do not request the account again.
    if (!force && Number(session?.accountSnapshotVersion) >= 1) return session;
    const mutationVersion = state.profileMutationVersion;
    try {
      // A viewer-qualified lookup is deliberately outside the guest profile
      // edge cache. Combined with a no-store browser fetch, this makes the
      // signed-in owner's reload an authoritative read after an edit instead
      // of reusing the pre-edit public snapshot.
      const encodedName = encodeURIComponent(nodeName);
      const body = await fetchJson(
        `/api/accounts/${encodedName}?viewer=${encodedName}`,
        {
          fresh: true,
          // The viewer query already bypasses the Worker's manual edge cache.
          // Keep the URL stable so simultaneous boot/page hydration shares
          // one in-flight no-store request.
          cacheBust: false,
        });
      // A profile POST may have started while this request was in flight. Its
      // response is newer even if this older GET happens to finish last.
      if (mutationVersion !== state.profileMutationVersion) {
        return state.session || session;
      }
      if (body.exists === false) return session;
      const nextSession = sessionFromAccountPayload(body, session);
      writeSession(nextSession);
      renderProfile(nextSession);
      return nextSession;
    } catch (_) {
      return session;
    }
  }

  async function backfillAdminUrl(session) {
    if (!session?.isAdmin || session.adminUrl || session.adminUrlCheckedAt) return session;
    try {
      const token = String(session.sessionToken || "");
      const response = await fetch("/api/accounts/sessions", {
        credentials: "same-origin",
        cache: "no-store",
        headers: {
          accept: "application/json",
          ...(token && token !== "cookie" ? { authorization: `Bearer ${token}` } : {}),
        },
      });
      if (!response.ok) return session;
      const account = (await response.json().catch(() => null))?.account;
      const current = state.session || session;
      if (!account || account.isAdmin !== true ||
          String(account.nodeName || "").toLowerCase() !== String(current?.nodeName || "").toLowerCase()) {
        return session;
      }
      const nextSession = {
        ...current,
        isAdmin: true,
        adminUrl: String(account.adminUrl || ""),
        adminUrlCheckedAt: Date.now(),
      };
      writeSession(nextSession);
      renderProfile(nextSession);
      return nextSession;
    } catch (_) {
      return session;
    }
  }

  const MARKDOWN_VIDEO_TEXTAREAS = [
    "[data-repo-issue-body]", "[data-repo-pull-body]",
    "[data-repo-issue-comment-body]", "[data-repo-issue-comment-edit-body]",
    "[data-repo-pull-review-body]", "[data-repo-issue-description-input]",
    "[data-repo-discussion-reply-body]",
  ].join(",");
  const MARKDOWN_VIDEO_TYPES = { "video/mp4": "mp4", "video/webm": "webm", "video/quicktime": "mov" };
  const MARKDOWN_VIDEO_EXTENSIONS = { mp4: "video/mp4", m4v: "video/mp4", webm: "video/webm", mov: "video/quicktime" };
  const MARKDOWN_VIDEO_MAX_BYTES = 25 * 1024 * 1024;
  const MARKDOWN_VIDEO_PART_BYTES = 512 * 1024;

  function markdownVideoButtonHtml() {
    return '<button type="button" data-markdown-video-attach class="inline-flex h-control-sm items-center gap-1.5 rounded-md border border-border px-2 text-control text-foreground hover:bg-secondary"><i data-lucide="video" class="h-3.5 w-3.5 text-muted-foreground"></i>Attach video</button>';
  }

  function markdownVideoMime(file) {
    const type = String(file?.type || "").toLowerCase();
    if (MARKDOWN_VIDEO_TYPES[type]) return type;
    const extension = String(file?.name || "").split(".").pop().toLowerCase();
    return MARKDOWN_VIDEO_EXTENSIONS[extension] || "";
  }

  function markdownVideoFiles(list) {
    return Array.from(list || []).filter((file) => markdownVideoMime(file));
  }

  function markdownVideoError(code) {
    return ({
      unsupported_media_type: "Attach an MP4, WebM or MOV video.",
      media_type_mismatch: "That file is not the video type its name says.",
      media_too_large: `Videos can be at most ${formatSize(MARKDOWN_VIDEO_MAX_BYTES)}.`,
      media_quota_count: "You have uploaded the most videos allowed today. Try again tomorrow.",
      media_quota_bytes: "You have used today's video upload allowance. Try again tomorrow.",
      email_unverified: "Verify your email address before uploading videos.",
      repository_unavailable: "Videos can only be attached on public repositories.",
      invalid_session: "Sign in to attach a video.",
    })[code] || "The video could not be uploaded. Please try again.";
  }

  function readFileSliceBase64(blob) {
    return new Promise((resolve, reject) => {
      const reader = new FileReader();
      reader.onload = () => resolve(String(reader.result || "").replace(/^data:[^,]*,/, ""));
      reader.onerror = () => reject(reader.error || new Error("read_failed"));
      reader.readAsDataURL(blob);
    });
  }

  async function markdownVideoRequest(url, method, payload) {
    const response = await fetch(url, {
      method,
      credentials: "include",
      cache: "no-store",
      headers: { accept: "application/json", "content-type": "application/json" },
      body: JSON.stringify({ ...payload, sessionToken: state.session?.sessionToken || "" }),
    });
    const data = await response.json().catch(() => ({}));
    if (!response.ok || data.ok === false) {
      const error = new Error(String(data.error || `HTTP ${response.status}`));
      error.code = String(data.error || "");
      throw error;
    }
    return data;
  }

  async function uploadMarkdownVideo(repo, file, onProgress) {
    const mime = markdownVideoMime(file);
    if (!mime) throw Object.assign(new Error("unsupported_media_type"), { code: "unsupported_media_type" });
    if (file.size > MARKDOWN_VIDEO_MAX_BYTES) throw Object.assign(new Error("media_too_large"), { code: "media_too_large" });
    const base = `${repoApiBase(repo)}/media`;
    const parts = Math.ceil(file.size / MARKDOWN_VIDEO_PART_BYTES);
    const created = await markdownVideoRequest(base, "POST", { name: file.name || "", mime, size: file.size, parts });
    const media = created.media || {};
    for (let index = 0; index < parts; index += 1) {
      const slice = file.slice(index * MARKDOWN_VIDEO_PART_BYTES, (index + 1) * MARKDOWN_VIDEO_PART_BYTES);
      const data = await readFileSliceBase64(slice);
      await markdownVideoRequest(`${base}/${media.id}/parts/${index}`, "PUT", { data });
      onProgress?.((index + 1) / parts);
    }
    const finished = await markdownVideoRequest(`${base}/${media.id}/finish`, "POST", {});
    return String(finished.media?.url || media.url || "");
  }

  function replaceInTextarea(textarea, from, to) {
    const index = textarea.value.indexOf(from);
    if (index < 0) return false;
    textarea.value = textarea.value.slice(0, index) + to + textarea.value.slice(index + from.length);
    textarea.dispatchEvent(new Event("input", { bubbles: true }));
    return true;
  }

  async function insertMarkdownVideo(textarea, file) {
    const repo = state.repoRecordDetail?.repo || state.selectedRepo;
    if (!repo || !textarea) return;
    if (!state.session?.nodeName) {
      showErrorNotice(markdownVideoError("invalid_session"));
      return;
    }
    const name = String(file.name || "video").replace(/[\[\]()<>]/g, "_");
    const token = `${Date.now().toString(36)}${Math.random().toString(36).slice(2, 6)}`;
    let placeholder = `![Uploading ${name} ${token}…]()`;
    const start = textarea.selectionStart ?? textarea.value.length;
    const end = textarea.selectionEnd ?? start;
    const insertion = `\n${placeholder}\n`;
    textarea.value = textarea.value.slice(0, start) + insertion + textarea.value.slice(end);
    textarea.selectionStart = textarea.selectionEnd = start + insertion.length;
    textarea.dispatchEvent(new Event("input", { bubbles: true }));
    try {
      const url = await uploadMarkdownVideo(repo, file, (fraction) => {
        const next = `![Uploading ${name} ${token} ${Math.round(fraction * 100)}%…]()`;
        if (replaceInTextarea(textarea, placeholder, next)) placeholder = next;
      });
      if (!url) throw new Error("upload_failed");
      replaceInTextarea(textarea, placeholder, `![${name}](${url})`);
    } catch (error) {
      replaceInTextarea(textarea, `\n${placeholder}\n`, "\n") || replaceInTextarea(textarea, placeholder, "");
      showErrorNotice(markdownVideoError(error?.code || error?.message));
    }
  }

  document.addEventListener("paste", (event) => {
    const textarea = event.target?.closest?.(MARKDOWN_VIDEO_TEXTAREAS);
    const files = markdownVideoFiles(event.clipboardData?.files);
    if (!textarea || !files.length) return;
    event.preventDefault();
    files.forEach((file) => void insertMarkdownVideo(textarea, file));
  });

  document.addEventListener("dragover", (event) => {
    if (!event.target?.closest?.(MARKDOWN_VIDEO_TEXTAREAS)) return;
    if (!Array.from(event.dataTransfer?.types || []).includes("Files")) return;
    event.preventDefault();
    event.dataTransfer.dropEffect = "copy";
  });

  document.addEventListener("drop", (event) => {
    const textarea = event.target?.closest?.(MARKDOWN_VIDEO_TEXTAREAS);
    const files = markdownVideoFiles(event.dataTransfer?.files);
    if (!textarea || !files.length) return;
    event.preventDefault();
    files.forEach((file) => void insertMarkdownVideo(textarea, file));
  });

  document.addEventListener("click", (event) => {
    const button = event.target?.closest?.("[data-markdown-video-attach]");
    if (!button) return;
    const textarea = button.closest("form")?.querySelector(MARKDOWN_VIDEO_TEXTAREAS);
    if (!textarea) return;
    const input = document.createElement("input");
    input.type = "file";
    input.accept = "video/mp4,video/webm,video/quicktime,.mp4,.m4v,.webm,.mov";
    input.multiple = true;
    input.addEventListener("change", () => {
      markdownVideoFiles(input.files).forEach((file) => void insertMarkdownVideo(textarea, file));
    });
    input.click();
  });

  function pullRevisionSource(values) {
    const head = String(values?.head || "").trim();
    if (!head) return null;
    const colon = head.lastIndexOf(":");
    if (colon < 0) return { owner: "", branch: head };
    const prefix = head.slice(0, colon);
    if (!prefix || prefix.includes("/")) return null;
    return { owner: prefix, branch: head.slice(colon + 1) };
  }

  function pullRevisionAllowed(parsed) {
    const values = parsed?.values || {};
    return String(values.status || "open").trim().toLowerCase() === "open"
      && values.derive !== "branch"
      && webSessionAuthored({ author: values.author, authorName: values.authorName })
      && Boolean(pullRevisionSource(values));
  }

  async function submitWebPullRevision(repo, number, values, branch) {
    const source = pullRevisionSource(values);
    if (!source || !branch) throw new Error("revision_unsupported");
    const sourceRepo = source.owner ? { owner: source.owner, name: repo.name } : repo;
    const base = String(values.base || "");
    let comparison;
    try {
      comparison = await fetchRepoJson(repoLiveUrl(sourceRepo, "compare", { base, head: branch, ref: "" }));
    } catch (_) {
      throw new Error("comparison_unavailable");
    }
    const patch = String(comparison.patch || "");
    const commits = String(comparison.commits || "");
    const baseOid = String(comparison.baseOid || "").toLowerCase();
    const headOid = String(comparison.headOid || "").toLowerCase();
    const validOid = (value) => /^[0-9a-f]{40}(?:[0-9a-f]{24})?$/.test(value);
    if (!validOid(baseOid) || !validOid(headOid)) throw new Error("invalid_comparison");
    if (!patch.trim()) throw new Error("no_changes");
    if (headOid === String(values.creationHeadOid || "").toLowerCase()) throw new Error("no_new_commits");
    const signedHead = source.owner ? `${source.owner}:${branch}` : branch;
    const revision = await signWebPullContent(
      repo, String(values.title || ""), "", base, signedHead, patch, commits);
    delete revision.description;
    revision.creationBaseOid = baseOid;
    revision.creationHeadOid = headOid;
    const { privateKey } = await getWebIssueKey();
    const binding = `${Number(number)}\n${revision.author}\n${revision.ts}\n${revision.sig}`;
    revision.revisionSig = bytesToB64url(await crypto.subtle.sign(
      { name: "Ed25519" }, privateKey,
      ISSUE_TEXT_ENCODER.encode(`${webEventSigningPrefix("pull-revision", repo)}${binding}`)));
    const data = await fetchRepoMutation(repo, "pulls", {
      owner: repo.owner, repo: repo.name, number: Number(number), revision,
    });
    return { ...data, revision };
  }

  function pullRevisionError(code) {
    return ({
      comparison_unavailable: "That branch is not on the serving mirror yet. A branch you just pushed can take a moment to arrive.",
      invalid_comparison: "That branch could not be compared with the base branch.",
      no_changes: "That branch has no changes against the base branch.",
      no_new_commits: "This pull request already shows that branch's latest commit.",
      revision_stale: "A newer version of this pull request is already waiting.",
      pull_not_open: "Only an open pull request can be updated.",
      pull_follows_branch: "This pull request already follows its branch.",
      not_authorized: "Only the pull request's author can update it.",
      pull_too_large: "That change is too large to send.",
      revision_unsupported: "This pull request came from another instance; update it there.",
      author_mismatch: "Sign in with the account that opened this pull request.",
    })[code] || "The pull request could not be updated. Please try again.";
  }

  function renderPullRevisionPanel(number, parsed) {
    if (!pullRevisionAllowed(parsed)) return "";
    const branch = pullRevisionSource(parsed.values)?.branch || "";
    return `
      <form data-repo-pull-revision-form data-repo-pull-revision-number="${escapeHtml(number)}" class="flex flex-wrap items-center gap-2 rounded-md border border-border px-4 py-3 text-[14px] sm:ml-14">
        <span class="min-w-0 flex-1 text-muted-foreground">Pushed more commits, or rewrote the branch? Update this pull request from a branch. No force push is needed.</span>
        <input data-repo-pull-revision-branch value="${escapeHtml(branch)}" aria-label="Source branch" spellcheck="false" class="h-control-sm w-48 rounded-md border border-border bg-background px-2 font-mono text-control text-foreground outline-none focus:border-[var(--dashboard-link)]" />
        <button type="submit" data-repo-pull-revision-submit class="inline-flex h-control-sm items-center gap-1.5 rounded-md border border-border bg-secondary px-2 text-control font-medium text-foreground hover:bg-secondary/70 disabled:opacity-50"><i data-lucide="git-commit-horizontal" class="h-3.5 w-3.5 text-muted-foreground"></i>Update pull request</button>
        <span data-repo-pull-revision-hint class="w-full text-[11px] text-muted-foreground"></span>
      </form>`;
  }

  document.addEventListener("submit", async (event) => {
    const form = event.target?.closest?.("[data-repo-pull-revision-form]");
    if (!form) return;
    event.preventDefault();
    const detail = state.repoRecordDetail;
    const number = Number(form.dataset.repoPullRevisionNumber || 0);
    if (!detail?.repo || detail.kind !== "pulls" || Number(detail.number) !== number) return;
    const hint = form.querySelector("[data-repo-pull-revision-hint]");
    const submit = form.querySelector("[data-repo-pull-revision-submit]");
    const branch = String(form.querySelector("[data-repo-pull-revision-branch]")?.value || "").trim();
    const setHint = (text, bad = false) => {
      if (!hint) return;
      hint.className = `w-full text-[11px] ${bad ? "text-destructive" : "text-muted-foreground"}`;
      hint.textContent = text;
    };
    if (submit) submit.disabled = true;
    setHint("Comparing, signing and sending…");
    try {
      await submitWebPullRevision(detail.repo, number, detail.parsed?.values || {}, branch);
      setHint("Sent. The pull request shows the new version once the repository's node takes it in.");
    } catch (error) {
      setHint(pullRevisionError(error?.code || error?.message), true);
    } finally {
      if (submit) submit.disabled = false;
    }
  });
