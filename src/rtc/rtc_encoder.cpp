// The WebRTC-facing take-over points: a native VideoFrameBuffer carrying only a
// slot index, and a VideoEncoder that delegates all compression to NVENC.
#include "rtc/rtc_internal.hpp"
#include <algorithm>
Native::~Native() {
  shared.cb.release_slot(shared.user, slot);
}
int Encoder::InitEncode(const VideoCodec *, const Settings &) {
  return WEBRTC_VIDEO_CODEC_OK;
}
int32_t Encoder::RegisterEncodeCompleteCallback(EncodedImageCallback *cb) {
  callback = cb;
  return 0;
}
int32_t Encoder::Release() {
  callback = nullptr;
  return 0;
}
// WebRTC's congestion controller drives NVENC bitrate through this path.
void Encoder::SetRates(const RateControlParameters &p) {
  bitrate = std::clamp(p.bitrate.get_sum_bps(), 200000u, 20000000u);
}
// The return type is qualified because an out-of-class member definition
// resolves its return type in namespace scope, not in the class scope.
VideoEncoder::EncoderInfo Encoder::GetEncoderInfo() const {
  EncoderInfo i;
  i.supports_native_handle = true;
  i.is_hardware_accelerated = true;
  i.implementation_name = "Vulkan-CUDA-NVENC";
  i.has_trusted_rate_controller = true;
  i.preferred_pixel_formats = {VideoFrameBuffer::Type::kNative};
  i.supports_simulcast = false;
  return i;
}
int32_t Encoder::Encode(const VideoFrame &f, const std::vector<VideoFrameType> *types) {
  if (!callback || f.video_frame_buffer()->type() != VideoFrameBuffer::Type::kNative)
    return WEBRTC_VIDEO_CODEC_ERROR;
  auto *native = static_cast<Native *>(f.video_frame_buffer().get());
  bool key = types && std::find(types->begin(), types->end(), VideoFrameType::kVideoFrameKey) !=
                          types->end();
  const unsigned char *data = nullptr;
  size_t size = 0;
  int isKey = 0;
  if (shared.cb.encode(shared.user, native->slot, key, bitrate, f.rtp_timestamp(), &data, &size,
                       &isKey))
    return WEBRTC_VIDEO_CODEC_ERROR;
  EncodedImage image;
  image.SetEncodedData(EncodedImageBuffer::Create(data, size));
  image.SetRtpTimestamp(f.rtp_timestamp());
  image._encodedWidth = f.width();
  image._encodedHeight = f.height();
  image.SetFrameType(isKey ? VideoFrameType::kVideoFrameKey : VideoFrameType::kVideoFrameDelta);
  CodecSpecificInfo info{};
  info.codecType = kVideoCodecH264;
  info.codecSpecific.H264.packetization_mode = H264PacketizationMode::NonInterleaved;
  info.codecSpecific.H264.temporal_idx = 0;
  info.codecSpecific.H264.idr_frame = isKey;
  shared.cb.encoded_done(shared.user, native->slot, f.rtp_timestamp());
  callback->OnEncodedImage(image, &info);
  return 0;
}
// H.264 baseline level 3.1 is the only format offered, matching the NVENC
// configuration and the 1280x720 ceiling negotiated by SDP.
std::vector<SdpVideoFormat> Factory::GetSupportedFormats() const {
  return {SdpVideoFormat("H264", {{"profile-level-id", "42e01f"},
                                  {"packetization-mode", "1"},
                                  {"level-asymmetry-allowed", "1"}})};
}
std::unique_ptr<VideoEncoder> Factory::Create(const Environment &, const SdpVideoFormat &f) {
  if (f.name != "H264")
    return nullptr;
  return std::make_unique<Encoder>(shared);
}
