#pragma once
#include "rtc/rtc_bridge.h"
// The pinned libwebrtc package is ABI-sensitive, so the whole include set lives
// in this one internal header instead of being spread over the bridge sources.
#include "api/audio/create_audio_device_module.h"
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/create_peerconnection_factory.h"
#include "api/data_channel_interface.h"
#include "api/environment/environment_factory.h"
#include "api/jsep.h"
#include "api/make_ref_counted.h"
#include "api/peer_connection_interface.h"
#include "api/video/encoded_image.h"
#include "api/video/video_broadcaster.h"
#include "api/video/video_frame_buffer.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_encoder.h"
#include "api/video_codecs/video_encoder_factory.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "pc/video_track_source.h"
#include "rtc_base/logging.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/thread.h"
#include "rtc_base/time_utils.h"
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
using namespace webrtc;
// Callback table plus the owning viewer, passed by value into every class below
// so the plain-C boundary stays the only thing crossing the ABI.
struct Shared {
  void *user;
  RtcCallbacks cb;
};
// GPU-backed VideoFrameBuffer: carries the frame-slot index and dimensions and
// no pixel data at all. Its destructor returns the slot to the renderer, which
// is what protects a slot from being rewritten while an encoder still reads it.
class Native : public VideoFrameBuffer {
public:
  Native(Shared s, int slot, int w, int h) : shared(s), slot(slot), w(w), h(h) {}
  ~Native() override;
  Type type() const override {
    return Type::kNative;
  }
  int width() const override {
    return w;
  }
  int height() const override {
    return h;
  }
  // Non-null would invite WebRTC to copy pixels to host memory.
  scoped_refptr<I420BufferInterface> ToI420() override {
    return nullptr;
  }
  Shared shared;
  int slot, w, h;
};
class Source : public VideoTrackSource {
public:
  Source() : VideoTrackSource(false) {}
  VideoBroadcaster broadcaster;
  bool is_screencast() const override {
    return true;
  }
  VideoSourceInterface<VideoFrame> *source() override {
    return &broadcaster;
  }
};
// Appears to WebRTC as an encoder but does no pixel work: it forwards the slot
// to NVENC and hands the resulting H.264 buffer straight back.
class Encoder : public VideoEncoder {
public:
  explicit Encoder(Shared s) : shared(s) {}
  int InitEncode(const VideoCodec *, const Settings &) override;
  int32_t RegisterEncodeCompleteCallback(EncodedImageCallback *) override;
  int32_t Release() override;
  void SetRates(const RateControlParameters &) override;
  EncoderInfo GetEncoderInfo() const override;
  int32_t Encode(const VideoFrame &, const std::vector<VideoFrameType> *) override;

private:
  Shared shared;
  EncodedImageCallback *callback = nullptr;
  unsigned bitrate = 6000000;
};
class Factory : public VideoEncoderFactory {
public:
  explicit Factory(Shared s) : shared(s) {}
  std::vector<SdpVideoFormat> GetSupportedFormats() const override;
  std::unique_ptr<VideoEncoder> Create(const Environment &, const SdpVideoFormat &) override;

private:
  Shared shared;
};
// Bridges libwebrtc's asynchronous callbacks back to synchronous control flow
// with a bounded wait.
struct Wait {
  std::mutex mutex;
  std::condition_variable cv;
  bool done = false;
  std::string error;
  void finish(std::string e = {});
  bool wait();
};
class SetObserver : public SetSessionDescriptionObserver {
public:
  std::shared_ptr<Wait> result;
  explicit SetObserver(std::shared_ptr<Wait> r) : result(std::move(r)) {}
  void OnSuccess() override;
  void OnFailure(RTCError) override;
};
class AnswerObserver : public CreateSessionDescriptionObserver {
public:
  scoped_refptr<PeerConnectionInterface> peer;
  std::shared_ptr<Wait> result;
  AnswerObserver(scoped_refptr<PeerConnectionInterface> p, std::shared_ptr<Wait> r)
      : peer(std::move(p)), result(std::move(r)) {}
  void OnSuccess(SessionDescriptionInterface *) override;
  void OnFailure(RTCError) override;
};
struct Context : PeerConnectionObserver, DataChannelObserver {
  Shared shared;
  std::unique_ptr<Thread> network, worker, signaling;
  scoped_refptr<PeerConnectionFactoryInterface> factory;
  scoped_refptr<Source> source;
  scoped_refptr<PeerConnectionInterface> peer;
  scoped_refptr<DataChannelInterface> channel;
  std::mutex operation;
  std::shared_ptr<Wait> gathered;
  std::string iceUrl, iceUser, icePass;
  void OnSignalingChange(PeerConnectionInterface::SignalingState) override;
  void OnIceGatheringChange(PeerConnectionInterface::IceGatheringState) override;
  void OnIceCandidate(const IceCandidate *) override;
  void OnDataChannel(scoped_refptr<DataChannelInterface>) override;
  void OnStateChange() override;
  void OnMessage(const DataBuffer &) override;
  void OnConnectionChange(PeerConnectionInterface::PeerConnectionState) override;
  void close();
};
