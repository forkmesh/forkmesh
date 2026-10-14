import {
  callVoiceChoices,
  callVoicesSupported,
  createVoiceStage,
  rememberCallerPitch,
  resolveCallVoice,
  savedCallerPitch,
  savedVoiceChoice,
} from "./chat-call-voice.js";
import {
  CALL_FACES,
  CALL_FACE_STORAGE_KEY,
  callBackgroundsSupported,
  callFaceById,
  callFacesSupported,
  createFaceStage,
} from "./chat-call-faces.js";
import { CALL_BACKGROUNDS, savedCallBackground } from "./chat-call-backgrounds.js";
import { registerViewOnceFilterStages } from "./chat-view-once-recorder.js";

export const VIEW_ONCE_FACE_READY_MS = 20_000;

export async function startViewOnceFaceStage(cameraStream, choice, {
  createFaceStageImpl = createFaceStage,
  readyMs = VIEW_ONCE_FACE_READY_MS,
  now = () => globalThis.performance.now(),
  wait = (ms) => new Promise((resolve) => globalThis.setTimeout(resolve, ms)),
} = {}) {
  const stage = createFaceStageImpl({
    cameraStream,
    faceId: choice.faceId || "",
    backgroundId: choice.backgroundId || "",
    backgroundImage: choice.backgroundImage || "",
  });
  try {
    if (!stage.track) throw new Error("face_stage_unavailable");
    if (choice.faceId && stage.face() !== choice.faceId) throw new Error("face_unavailable");
    if (choice.backgroundId && stage.background() !== choice.backgroundId) {
      throw new Error("background_unavailable");
    }
    const deadline = now() + readyMs;
    while (!stage.ready()) {
      if (now() > deadline) throw new Error("face_stage_timeout");
      await wait(100);
    }
    return stage;
  } catch (error) {
    stage.stop();
    throw error;
  }
}

export function installViewOnceFilters(globalRef = globalThis) {
  const document = globalRef.document;
  const faces = callFacesSupported(document);
  const backgrounds = callBackgroundsSupported(document);
  const voices = callVoicesSupported(globalRef);
  registerViewOnceFilterStages({
    face: faces || backgrounds
      ? (cameraStream, choice) => startViewOnceFaceStage(cameraStream, {
        ...choice,
        faceId: faces ? choice.faceId : "",
        backgroundId: backgrounds ? choice.backgroundId : "",
      })
      : null,
    voice: voices
      ? (micTrack, voiceId) => createVoiceStage({
        sourceTrack: micTrack,
        voiceId,
        userPitchHz: savedCallerPitch(),
        onStats: (stats) => rememberCallerPitch(stats?.userPitchHz),
      })
      : null,
  });
  return { faces, backgrounds, voices };
}

export function viewOnceFilterChoices(support, storage = globalThis.localStorage) {
  let face = "";
  try {
    face = callFaceById(storage?.getItem(CALL_FACE_STORAGE_KEY) || "")?.id || "";
  } catch (_) {}
  const background = savedCallBackground(storage);
  return {
    faces: support.faces
      ? [["", "Your own face"], ...CALL_FACES.map((item) => [item.id, item.label])]
      : [],
    backgrounds: support.backgrounds
      ? CALL_BACKGROUNDS
        .filter((item) => !item.custom || (background.id === item.id && background.image))
        .map((item) => [item.id, item.label])
      : [],
    voices: support.voices ? callVoiceChoices() : [],
    defaults: {
      face: support.faces ? face : "",
      background: support.backgrounds ? background.id : "",
      backgroundImage: background.image,
      voice: savedVoiceChoice(storage),
    },
    resolveVoice: resolveCallVoice,
  };
}
