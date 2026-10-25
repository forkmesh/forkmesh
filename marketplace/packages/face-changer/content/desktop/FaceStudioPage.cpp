#include "FaceStudioPage.h"

namespace forkmesh::facechanger {

QString faceStudioMessagePrefix()
{
    return QStringLiteral("fmface:");
}

QString faceStudioPage()
{
    return QStringLiteral(R"HTML(<!doctype html><html><head><meta charset="utf-8">
<style>html,body{margin:0;width:100%;height:100%;overflow:hidden;background:#0b0d10}#stage{position:absolute;inset:0;display:flex;align-items:center;justify-content:center}#stage canvas{max-width:100%;max-height:100%;width:auto;height:auto}</style>
</head><body><div id="stage"></div><script type="module">
const CAMERA_STYLE = "position:fixed;left:0;top:0;width:1px;height:1px;opacity:0;pointer-events:none";
const SKIP = /forkmesh|dummy video|loopback|virtual/i;
const JPEG_QUALITY = 0.85;
const box = document.getElementById("stage");
const state = { m: null, stream: null, stage: null, cam: null, unwatch: null, latest: "", seq: 0, taken: 0, busy: false, generation: 0, faceId: "", backgroundId: "", outfitId: "" };

function post(message) {
  try {
    console.log("fmface:" + JSON.stringify(message));
  } catch (_) {}
}

function stopStream(stream) {
  for (const track of stream?.getTracks?.() || []) {
    try {
      track.stop();
    } catch (_) {}
  }
}

function backgroundImage(id) {
  const { backgrounds } = state.m;
  if (id !== backgrounds.CUSTOM_BACKGROUND_ID) return "";
  try {
    return localStorage.getItem(backgrounds.CALL_BACKGROUND_IMAGE_KEY) || "";
  } catch (_) {
    return "";
  }
}

function look() {
  const { custom, backgrounds } = state.m;
  return {
    face: state.faceId,
    background: state.backgroundId,
    outfit: state.outfitId,
    customFace: Boolean(custom.savedCustomFaceImage()),
    customBackground: Boolean(backgroundImage(backgrounds.CUSTOM_BACKGROUND_ID)),
  };
}

function postLook() {
  post({ type: "look", ...look() });
}

async function cameras() {
  try {
    const list = await navigator.mediaDevices.enumerateDevices();
    return list
      .filter((device) => device.kind === "videoinput" && !SKIP.test(device.label || ""))
      .map((device, index) => [device.deviceId, device.label || `Camera ${index + 1}`]);
  } catch (_) {
    return [];
  }
}

function videoOnly(deviceId) {
  const video = { ...state.m.studio.STUDIO_VIDEO };
  if (deviceId) {
    delete video.facingMode;
    video.deviceId = { exact: deviceId };
  }
  return { audio: false, video };
}

async function openCamera(deviceId) {
  let stream = null;
  try {
    stream = await navigator.mediaDevices.getUserMedia(videoOnly(deviceId));
  } catch (error) {
    if (!deviceId || (error?.name !== "OverconstrainedError" && error?.name !== "NotFoundError")) throw error;
    stream = await navigator.mediaDevices.getUserMedia(videoOnly(""));
  }
  if (!SKIP.test(stream.getVideoTracks()[0]?.label || "")) return stream;
  stopStream(stream);
  const other = (await cameras())[0];
  if (!other) throw Object.assign(new Error("only_virtual"), { name: "OnlyVirtual" });
  return navigator.mediaDevices.getUserMedia(videoOnly(other[0]));
}

function watchFace() {
  state.unwatch?.();
  state.unwatch = null;
  if (!state.stage || state.faceId !== state.m.custom.CUSTOM_FACE_ID) return;
  state.unwatch = state.m.studio.watchStudioFace(state.stage, {
    onFailed: () => {
      post({ type: "error", text: "No face found in that photo. Try a clear, front-facing photo." });
      window.fmFace("");
    },
  });
}

function stop() {
  state.generation += 1;
  state.unwatch?.();
  state.unwatch = null;
  try {
    state.stage?.stop();
  } catch (_) {}
  state.stage = null;
  stopStream(state.stream);
  state.stream = null;
  if (state.cam) {
    state.cam.srcObject = null;
    state.cam.remove();
  }
  state.cam = null;
  state.latest = "";
  state.busy = false;
  box.replaceChildren();
}

window.fmStart = async (deviceId) => {
  if (!state.m || state.stream) return;
  const generation = ++state.generation;
  let stream = null;
  try {
    stream = await openCamera(String(deviceId || ""));
    if (generation !== state.generation) {
      stopStream(stream);
      return;
    }
    const { faces, studio } = state.m;
    state.stream = stream;
    state.stage = faces.createFaceStage({
      cameraStream: stream,
      manual: true,
      faceId: state.faceId,
      faceImage: studio.studioFaceImage(state.faceId),
      backgroundId: state.backgroundId,
      backgroundImage: backgroundImage(state.backgroundId),
      outfitId: state.outfitId,
    });
    if (!state.stage?.canvas) throw new Error("face_stage_unavailable");
    const cam = document.createElement("video");
    cam.muted = true;
    cam.playsInline = true;
    cam.autoplay = true;
    cam.setAttribute("style", CAMERA_STYLE);
    cam.srcObject = stream;
    document.body.append(cam);
    cam.play?.()?.catch?.(() => {});
    state.cam = cam;
    box.replaceChildren(state.stage.canvas);
    watchFace();
    const track = stream.getVideoTracks()[0];
    post({ type: "live", camera: track?.getSettings?.().deviceId || "", label: track?.label || "", cameras: await cameras() });
  } catch (error) {
    const started = Boolean(state.stream);
    if (!started) stopStream(stream);
    stop();
    let text = state.m.studio.studioMediaError(error, { video: true });
    if (error?.name === "OnlyVirtual") text = "The only camera here is a virtual one. Connect a camera and try again.";
    else if (started) text = "The face changer could not start here.";
    post({ type: "error", text, stopped: true });
  }
};

window.fmStop = () => {
  stop();
  post({ type: "stopped" });
};

window.fmLook = postLook;

window.fmTake = (encode) => {
  const { stage, cam } = state;
  if (!stage || !cam) return "";
  if (cam.paused) cam.play?.()?.catch?.(() => {});
  if (!state.busy && cam.videoWidth && cam.readyState >= 2) {
    state.busy = true;
    stage.processFrame(cam).then((drawn) => {
      if (drawn && encode && stage === state.stage) {
        state.latest = stage.canvas.toDataURL("image/jpeg", JPEG_QUALITY);
        state.seq += 1;
      }
    }).catch(() => {}).finally(() => {
      if (stage === state.stage) state.busy = false;
    });
  }
  if (state.seq === state.taken) return "";
  state.taken = state.seq;
  return state.latest;
};

window.fmFace = (id) => {
  const { studio } = state.m;
  state.faceId = studio.rememberStudioFace(String(id || ""));
  state.stage?.setFace(state.faceId, studio.studioFaceImage(state.faceId));
  watchFace();
  postLook();
};

window.fmBackground = (id) => {
  const { backgrounds } = state.m;
  const background = backgrounds.callBackgroundById(String(id || ""));
  if (!background) return postLook();
  const image = backgroundImage(background.id);
  if (background.custom && !image) return postLook();
  backgrounds.rememberCallBackground(background.id, image);
  state.backgroundId = background.id;
  state.stage?.setBackground(state.backgroundId, image);
  postLook();
};

window.fmOutfit = (id) => {
  const { outfits } = state.m;
  const outfit = outfits.callOutfitById(String(id || ""));
  outfits.rememberCallOutfit(outfit?.id || "");
  state.outfitId = outfit?.id || "";
  state.stage?.setOutfit(state.outfitId);
  postLook();
};

async function photoFile(dataUrl, name) {
  const blob = await (await fetch(String(dataUrl))).blob();
  return new File([blob], name, { type: blob.type || "image/jpeg" });
}

window.fmFacePhoto = async (dataUrl) => {
  try {
    await state.m.studio.studioFaceFromFile(await photoFile(dataUrl, "face"));
    window.fmFace(state.m.custom.CUSTOM_FACE_ID);
  } catch (_) {
    post({ type: "error", text: "That file could not be read as a photo." });
    postLook();
  }
};

window.fmBackgroundPhoto = async (dataUrl) => {
  try {
    await state.m.studio.studioBackgroundFromFile(await photoFile(dataUrl, "background"));
    window.fmBackground(state.m.backgrounds.CUSTOM_BACKGROUND_ID);
  } catch (_) {
    post({ type: "error", text: "That file could not be read as a photo." });
    postLook();
  }
};

window.addEventListener("pagehide", stop);

try {
  const [faces, studio, backgrounds, outfits, custom] = await Promise.all([
    import("/chat-call-faces.js"),
    import("/call-studio.js"),
    import("/chat-call-backgrounds.js"),
    import("/chat-call-outfits.js"),
    import("/chat-call-face-custom.js"),
  ]);
  state.m = { faces, studio, backgrounds, outfits, custom };
  const support = studio.studioSupport(globalThis);
  state.faceId = support.faces ? studio.savedStudioFace() : "";
  state.backgroundId = support.backgrounds ? backgrounds.savedCallBackground().id : "";
  state.outfitId = support.backgrounds ? outfits.savedCallOutfit() : "";
  post({
    type: "ready",
    support,
    faces: studio.studioFaceChoices(),
    backgrounds: backgrounds.CALL_BACKGROUNDS.map((background) => [background.id, background.label]),
    outfits: outfits.CALL_OUTFITS.map((outfit) => [outfit.id, outfit.label]),
    customFaceId: custom.CUSTOM_FACE_ID,
    customBackgroundId: backgrounds.CUSTOM_BACKGROUND_ID,
    ...look(),
  });
} catch (_) {
  post({ type: "failed", text: "The face changer could not load from your ForkMesh host. Check the connection and try again." });
}
</script></body></html>)HTML");
}

}
