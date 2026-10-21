const INBOUND = [
  "packetsReceived", "packetsLost", "bytesReceived", "jitter",
  "jitterBufferDelay", "jitterBufferTargetDelay", "jitterBufferEmittedCount",
  "totalSamplesReceived", "concealedSamples", "silentConcealedSamples", "concealmentEvents",
  "insertedSamplesForDeceleration", "removedSamplesForAcceleration",
  "framesReceived", "framesDecoded", "framesDropped", "keyFramesDecoded",
  "frameWidth", "frameHeight", "framesPerSecond",
  "freezeCount", "totalFreezesDuration", "pauseCount", "totalPauseDuration",
  "nackCount", "pliCount", "firCount", "totalDecodeTime",
  "retransmittedPacketsReceived", "fecPacketsReceived",
];
const OUTBOUND = [
  "packetsSent", "bytesSent", "retransmittedPacketsSent", "retransmittedBytesSent",
  "framesEncoded", "framesSent", "frameWidth", "frameHeight", "framesPerSecond",
  "targetBitrate", "qualityLimitationReason", "qualityLimitationResolutionChanges",
  "nackCount", "pliCount", "firCount", "totalEncodeTime",
];
const TRANSPORT = ["currentRoundTripTime", "availableOutgoingBitrate", "availableIncomingBitrate", "bytesSent", "bytesReceived"];

function pick(entry, keys) {
  const out = {};
  for (const key of keys) if (entry[key] !== undefined) out[key] = entry[key];
  return out;
}

export function normalize(report, owners) {
  const sample = { inbound: [], outbound: [], transport: {} };
  if (!report) return sample;
  let selected = "";
  report.forEach((entry) => {
    if (entry.type === "transport" && entry.selectedCandidatePairId) selected = entry.selectedCandidatePairId;
  });
  report.forEach((entry) => {
    if (entry.type === "inbound-rtp") {
      sample.inbound.push({ peer: owners.get(entry.trackIdentifier) || "", kind: entry.kind, ssrc: entry.ssrc, ...pick(entry, INBOUND) });
    } else if (entry.type === "outbound-rtp") {
      sample.outbound.push({ kind: entry.kind, ssrc: entry.ssrc, ...pick(entry, OUTBOUND) });
    } else if (entry.type === "candidate-pair" && (selected ? entry.id === selected : entry.nominated && entry.state === "succeeded")) {
      sample.transport = { rtt: entry.currentRoundTripTime, ...pick(entry, TRANSPORT) };
      const remote = report.get(entry.remoteCandidateId);
      if (remote) sample.transport.remote = remote.address + ":" + remote.port;
    }
  });
  return sample;
}
