export const CALL_BACKGROUND_STORAGE_KEY = "forkmesh.chat.callBackground";
export const CALL_BACKGROUND_IMAGE_KEY = "forkmesh.chat.callBackgroundImage";
export const CALL_BACKGROUND_BASE = "/assets/call-backgrounds/";
export const CUSTOM_BACKGROUND_ID = "custom";
export const CUSTOM_BACKGROUND_SIDE = 1920;
export const PORTRAIT_SUFFIX = "-portrait";
export const PORTRAIT_RATIO = 1.05;

export const CALL_BACKGROUNDS = Object.freeze([
  Object.freeze({ id: "", label: "No background", mode: "off" }),
  Object.freeze({ id: "blur", label: "Blur", mode: "blur", strength: 0.4 }),
  Object.freeze({ id: "blur-strong", label: "Strong blur", mode: "blur", strength: 1 }),
  Object.freeze({ id: "world", label: "ForkMesh World", mode: "image", file: "world" }),
  Object.freeze({ id: "office", label: "ForkMesh office", mode: "image", file: "office" }),
  Object.freeze({ id: "meeting", label: "Meeting room", mode: "image", file: "meeting" }),
  Object.freeze({ id: "library", label: "Home library", mode: "image", file: "library" }),
  Object.freeze({ id: "skyline", label: "City window", mode: "image", file: "skyline" }),
  Object.freeze({ id: CUSTOM_BACKGROUND_ID, label: "Your photo", mode: "image", custom: true }),
]);

export function callBackgroundById(id) {
  return CALL_BACKGROUNDS.find((background) => background.id === id) || null;
}

export function isPortraitFrame(width, height) {
  return Boolean(width && height && height > width * PORTRAIT_RATIO);
}

export function callBackgroundImageUrl(background, portrait = false) {
  if (!background?.file) return "";
  const stem = portrait ? `${background.file}${PORTRAIT_SUFFIX}` : background.file;
  return globalThis.FmCallBackgroundImages?.[stem] || `${CALL_BACKGROUND_BASE}${stem}.jpg`;
}

export function callBackgroundThumbUrl(background) {
  if (!background?.file) return "";
  return globalThis.FmCallBackgroundImages?.[`${background.file}-thumb`] || `${CALL_BACKGROUND_BASE}${background.file}-thumb.jpg`;
}

export function savedCallBackground(storage = globalThis.localStorage) {
  try {
    const id = storage?.getItem(CALL_BACKGROUND_STORAGE_KEY) || "";
    const background = callBackgroundById(id);
    if (!background) return { id: "", image: "" };
    if (background.custom) {
      const image = storage?.getItem(CALL_BACKGROUND_IMAGE_KEY) || "";
      return image ? { id: background.id, image } : { id: "", image: "" };
    }
    return { id: background.id, image: "" };
  } catch (_) {
    return { id: "", image: "" };
  }
}

export function rememberCallBackground(id, image = "", storage = globalThis.localStorage) {
  try {
    const background = callBackgroundById(id);
    if (!background || !background.id) {
      storage?.removeItem(CALL_BACKGROUND_STORAGE_KEY);
      return true;
    }
    if (background.custom) {
      if (!image) return false;
      storage?.setItem(CALL_BACKGROUND_IMAGE_KEY, image);
    }
    storage?.setItem(CALL_BACKGROUND_STORAGE_KEY, background.id);
    return true;
  } catch (_) {
    return false;
  }
}

export function fitSide(width, height, limit = CUSTOM_BACKGROUND_SIDE) {
  const scale = Math.min(1, limit / Math.max(width || 1, height || 1));
  return [Math.max(1, Math.round((width || 1) * scale)), Math.max(1, Math.round((height || 1) * scale))];
}

export async function customBackgroundFromFile(file, documentRef = globalThis.document, ImageImpl = globalThis.Image) {
  if (!file || !/^image\//.test(file.type || "")) throw new Error("not an image");
  const url = globalThis.URL.createObjectURL(file);
  try {
    const image = new ImageImpl();
    image.src = url;
    await image.decode();
    const [width, height] = fitSide(image.naturalWidth, image.naturalHeight);
    const canvas = documentRef.createElement("canvas");
    canvas.width = width;
    canvas.height = height;
    canvas.getContext("2d").drawImage(image, 0, 0, width, height);
    return canvas.toDataURL("image/jpeg", 0.86);
  } finally {
    globalThis.URL.revokeObjectURL(url);
  }
}
