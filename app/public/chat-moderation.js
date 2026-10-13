// Client-side moderation for encrypted chat. The relay retains ciphertext, so
// plaintext must be filtered before encryption and again after decryption.
// Only SHA-256 digests are shipped; the blocked vocabulary is never embedded
// as readable strings in the client bundle.
const BLOCKED_TERM_HASHES = new Set([
  "08a841e996781e9e77d30a4e4420a8f501a280b00624e6d1224bf54aaff73eba",
  "0f28c4960d96647e77e7ab6d13b85bd16c7ca56f45df802cdc763a5e5c0c7863",
  "120f6e5b4ea32f65bda68452fcfaaef06b0136e1d0e4a6f60bc3771fa0936dd6",
  "158869a97379229b7681efae9d7f9c9214134e836d649ba53477c0c111414d59",
  "16ea09fc78ca83ca502cbcf2377acdf280bf18f61e259153f0868405eedab5ef",
  "2189c0ed714f0c54ea91fc1d8355e3b1d68723a4fb580e99b088651c80a50f26",
  "2f5f6ce5ae30b54aa5d7ced1ba566982bab34ba2814a51ce1865d2c2d8815cd4",
  "566f532d486c947709d3d0e6b7575af8380248db66dada211d58eb00ad585297",
  "6ac3c336e4094835293a3fed8a4b5fedde1b5e2626d9838fed50693bba00af0e",
  "796e43a5a8cdb73b92b5f59eb50610cea3efa8ce229cd7f0557983091b2b4552",
  "7bc671151cbfaee7f32cd56e86a87b0be30fde8dc72c7f236d3ab2ce42cddbd5",
  "85fc17f7069acd39a5c636cd0a6530651096128da447959f5e250824857dc559",
  "886d51e97ad7931d0d2af8439ca6d9e4887e3c2b469ed247cbd68ceb3649ccde",
  "8f5083e3e5c7dc8932f2bf58212f963f3a44752618c96297f82623f736c52738",
  "98b52c4b6b7d1f48e7477a5ccc10955dd195d0ac5a38c8281bfeb08762634909",
  "9ae315a94e428a7ee3b5e48adae6541965d93b86acf10ffa1c45b93b6fe577b4",
  "ad505b0be8a49b89273e307106fa42133cbd804456724c5e7635bd953215d92a",
  "c2c3b68b48832afd9a4dbdd474c1b6c81c8baecdb71446f9947dac72dd0fe93d",
  "c3de533e9b7fe63b79f648687a30d2861edd92fe7c3cd1f2c485e0a605367624",
  "d75a838dc758ba17f28bd8dbac605cb70c35465263d5733164521de2f7ef7926",
  "dd92623b0a4b255f87cc4aaee7990ee182d91db49189df6229ce65b5e9d960da",
  "e512a05583448f44790783f986b1f36925c8cfc42338ca0e1caa637755bd15ae",
  "e7b98c6aa5b944e0b315d350d423f895ac9e44fb84f1534b18c2572370a67b9e",
  "eef3bd091670c3447022d619c06ad15de96da72b5a66f28bb8b75d1b1c12a05f",
  "f50c51ed2315dcf3fa88181cf033f8029cac64f7dea4048327ca032ec102ea74",
  "f9d0d9b18ae9033a5ea36df19bf279b059e887a9ae785db81117bceaecc95933",
]);

const encoder = new TextEncoder();
const digestCache = new Map();
const BLOCKED_TERM_LENGTHS = new Set([4, 5, 6, 7, 12]);
const MAX_DIGEST_CACHE_ENTRIES = 2048;
const FILTERED_TYPES = new Set(["chat", "thread-reply", "edit", "dm"]);

function moderationForms(value) {
  const normalized = String(value || "")
    .normalize("NFKD")
    .replace(/\p{M}+/gu, "")
    .toLowerCase()
    .replaceAll("0", "o")
    .replaceAll("1", "i")
    .replaceAll("3", "e")
    .replaceAll("4", "a")
    .replaceAll("5", "s")
    .replaceAll("7", "t")
    .replaceAll("8", "b")
    .replaceAll("9", "g")
    .replace(/[^a-z]+/g, "");
  return [...new Set([normalized, normalized.replace(/(.)\1+/g, "$1")])]
    .filter((form) => BLOCKED_TERM_LENGTHS.has(form.length));
}

async function sha256Hex(value) {
  if (!digestCache.has(value)) {
    if (digestCache.size >= MAX_DIGEST_CACHE_ENTRIES) digestCache.clear();
    digestCache.set(
      value,
      globalThis.crypto.subtle.digest("SHA-256", encoder.encode(value)).then(
        (digest) => Array.from(new Uint8Array(digest), (byte) =>
          byte.toString(16).padStart(2, "0")).join(""),
      ),
    );
  }
  return digestCache.get(value);
}

async function blockedRun(value) {
  const forms = moderationForms(value);
  const digests = await Promise.all(forms.map(sha256Hex));
  return digests.some((digest) => BLOCKED_TERM_HASHES.has(digest));
}

export async function moderateChatText(value) {
  const source = String(value || "");
  const runs = [...source.matchAll(
    /[\p{L}\p{N}](?:[\p{L}\p{N}]|[._-](?=[\p{L}\p{N}]))*/gu,
  )];
  const blocked = await Promise.all(runs.map((match) => blockedRun(match[0])));
  let output = "";
  let cursor = 0;
  let changed = false;
  runs.forEach((match, index) => {
    output += source.slice(cursor, match.index);
    if (blocked[index]) {
      output += "***";
      changed = true;
    } else {
      output += match[0];
    }
    cursor = match.index + match[0].length;
  });
  output += source.slice(cursor);
  return { text: output, changed };
}

export async function moderateChatPlain(plain) {
  if (!plain || !FILTERED_TYPES.has(plain.type)) return plain;
  const moderated = { ...plain };
  if (typeof moderated.text === "string") {
    moderated.text = (await moderateChatText(moderated.text)).text;
  }
  if (moderated.richText && typeof moderated.richText.source === "string") {
    moderated.richText = {
      ...moderated.richText,
      source: (await moderateChatText(moderated.richText.source)).text,
    };
  }
  return moderated;
}
