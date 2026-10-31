import { CALL_FACE_STORAGE_KEY, callBackgroundsSupported, callFaceById, callFacesSupported, createFaceStage } from "../chat-call-faces.js";
import { faceImageFor, photoFaceId, stageFaceFor } from "../chat-call-face-custom.js";
import { callBackgroundById, savedCallBackground } from "../chat-call-backgrounds.js";
import { callVoicesSupported, createVoiceStage, rememberCallerPitch, resolveCallVoice, savedCallerPitch, savedVoiceChoice } from "../chat-call-voice.js";

export function savedCallFace(storage, avatarImage = null) {
  try {
    const id = storage?.getItem(CALL_FACE_STORAGE_KEY) || "";
    if (photoFaceId(id)) return faceImageFor(id, { storage, avatarImage }) ? id : "";
    return callFaceById(id)?.id || "";
  } catch (_) {
    return "";
  }
}

export function createFmmLocal({ storage = globalThis.localStorage, documentRef = globalThis.document, scope = globalThis, voiceModuleUrl = "", avatarImage = null, faceChoice = null } = {}) {
  return async function prepareLocal(local, media) {
    const stops = [];
    let audio = local.getAudioTracks()[0] || null;
    let video = media === "video" ? local.getVideoTracks()[0] || null : null;
    let stage = null;
    const faceId = typeof faceChoice === "function" ? faceChoice() : savedCallFace(storage, avatarImage);
    const voice = resolveCallVoice(savedVoiceChoice(storage), faceId);
    if (audio && voice && callVoicesSupported(scope)) {
      try {
        const options = {
          sourceTrack: audio,
          voiceId: voice,
          userPitchHz: savedCallerPitch(storage),
          onStats: (stats) => rememberCallerPitch(stats?.userPitchHz, storage),
          globalRef: scope,
        };
        if (voiceModuleUrl) options.moduleUrl = voiceModuleUrl;
        const voiceStage = await createVoiceStage(options);
        audio = voiceStage.track;
        stops.push(() => voiceStage.stop());
      } catch (_) {}
    }
    if (video) {
      const background = savedCallBackground(storage);
      const backgroundId = callBackgroundById(background.id)?.id && callBackgroundsSupported(documentRef) ? background.id : "";
      if ((faceId && callFacesSupported(documentRef)) || backgroundId) {
        try {
          stage = createFaceStage({
            cameraStream: local,
            faceId: callFacesSupported(documentRef) ? stageFaceFor(faceId) : "",
            faceImage: faceImageFor(faceId, { storage, avatarImage }),
            backgroundId,
            backgroundImage: background.image || "",
            documentRef,
          });
          if (stage?.track) {
            video = stage.track;
            const current = stage;
            stops.push(() => current.stop());
          } else {
            stage = null;
          }
        } catch (_) {
          stage = null;
        }
      }
    }
    const tracks = [audio, video].filter(Boolean);
    return {
      stream: new scope.MediaStream(tracks),
      stage,
      stop() {
        for (const stop of stops.splice(0)) {
          try { stop(); } catch (_) {}
        }
      },
    };
  };
}
