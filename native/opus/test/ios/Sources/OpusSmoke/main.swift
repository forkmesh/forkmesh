import Foundation
import Opus

let rate: Int32 = 48000
let frame = 960
var error: Int32 = 0
guard let encoder = opus_encoder_create(rate, 1, OPUS_APPLICATION_VOIP, &error), error == OPUS_OK else {
    fatalError("opus_encoder_create failed: \(error)")
}
guard let decoder = opus_decoder_create(rate, 1, &error), error == OPUS_OK else {
    fatalError("opus_decoder_create failed: \(error)")
}
precondition(fm_opus_encoder_set(encoder, OPUS_SET_BITRATE_REQUEST, 48000) == OPUS_OK)
precondition(fm_opus_encoder_set(encoder, OPUS_SET_INBAND_FEC_REQUEST, 1) == OPUS_OK)
precondition(fm_opus_encoder_set(encoder, OPUS_SET_PACKET_LOSS_PERC_REQUEST, 30) == OPUS_OK)
precondition(fm_opus_encoder_set(encoder, OPUS_SET_SIGNAL_REQUEST, OPUS_SIGNAL_VOICE) == OPUS_OK)
precondition(fm_opus_encoder_set(encoder, OPUS_SET_DTX_REQUEST, 1) == OPUS_OK)
precondition(fm_opus_encoder_set(encoder, OPUS_SET_DRED_DURATION_REQUEST, 100) == OPUS_OK)
precondition(fm_opus_decoder_set(decoder, OPUS_SET_COMPLEXITY_REQUEST, 10) == OPUS_OK)
guard let dredDecoder = opus_dred_decoder_create(&error), error == OPUS_OK,
      let dred = opus_dred_alloc(&error), error == OPUS_OK else {
    fatalError("DRED state failed: \(error)")
}
var fec: Int32 = 0
precondition(fm_opus_encoder_get(encoder, OPUS_GET_INBAND_FEC_REQUEST, &fec) == OPUS_OK)

var pcm = [Float](repeating: 0, count: frame)
var packet = [UInt8](repeating: 0, count: 1500)
var out = [Float](repeating: 0, count: frame)
var lbrr = 0
var decoded = 0
var bytes = 0
var dredPackets = 0
var dredSamples: Int32 = 0
var recovered: Int32 = 0
for index in 0..<50 {
    for i in 0..<frame {
        pcm[i] = 0.5 * sin(2 * Float.pi * 440 * Float(index * frame + i) / Float(rate))
    }
    let length = opus_encode_float(encoder, pcm, Int32(frame), &packet, Int32(packet.count))
    precondition(length > 0, "opus_encode_float failed: \(length)")
    bytes += Int(length)
    if opus_packet_has_lbrr(packet, length) == 1 { lbrr += 1 }
    var dredEnd: Int32 = 0
    let available = opus_dred_parse(dredDecoder, dred, packet, length, rate, rate, &dredEnd, 0)
    if available > 0 {
        dredPackets += 1
        dredSamples = max(dredSamples, available)
    }
    if index == 40 && available >= Int32(2 * frame) {
        recovered = opus_decoder_dred_decode_float(decoder, dred, Int32(2 * frame), &out, Int32(frame))
    }
    decoded += Int(opus_decode_float(decoder, packet, length, &out, Int32(frame), 0))
}
let concealed = opus_decode_float(decoder, nil, 0, &out, Int32(frame), 0)
precondition(fec == 1 && lbrr >= 25 && decoded == 50 * frame && concealed == Int32(frame))
precondition(dredPackets >= 45 && recovered == Int32(frame))
print("\(String(cString: opus_get_version_string())) fec=\(fec) lbrr=\(lbrr) bytes=\(bytes) decoded=\(decoded) plc=\(concealed) dred_packets=\(dredPackets) dred_samples=\(dredSamples) dred_recovered=\(recovered)")
opus_dred_free(dred)
opus_dred_decoder_destroy(dredDecoder)
opus_encoder_destroy(encoder)
opus_decoder_destroy(decoder)
