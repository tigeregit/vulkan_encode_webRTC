#define VIEWER_RTC_BUILD
#include "rtc_bridge.h"
#include "api/audio/create_audio_device_module.h"
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/create_peerconnection_factory.h"
#include "api/environment/environment_factory.h"
#include "api/jsep.h"
#include "api/make_ref_counted.h"
#include "api/video/encoded_image.h"
#include "api/video/video_broadcaster.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_encoder.h"
#include "api/video_codecs/video_encoder_factory.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "pc/video_track_source.h"
#include "rtc_base/logging.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/time_utils.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
using namespace webrtc;
struct Context;
struct Shared {
  void *user;
  RtcCallbacks cb;
};
class Native : public VideoFrameBuffer {
public:
  Native(Shared s, int slot, int w, int h) : shared(s), slot(slot), w(w), h(h) {}
  ~Native() override {
    shared.cb.release_slot(shared.user, slot);
  }
  Type type() const override {
    return Type::kNative;
  }
  int width() const override {
    return w;
  }
  int height() const override {
    return h;
  }
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
class Encoder : public VideoEncoder {
  Shared shared;
  EncodedImageCallback *callback = nullptr;
  unsigned bitrate = 6000000;

public:
  explicit Encoder(Shared s) : shared(s) {}
  int InitEncode(const VideoCodec *, const Settings &) override {
    return WEBRTC_VIDEO_CODEC_OK;
  }
  int32_t RegisterEncodeCompleteCallback(EncodedImageCallback *cb) override {
    callback = cb;
    return 0;
  }
  int32_t Release() override {
    callback = nullptr;
    return 0;
  }
  void SetRates(const RateControlParameters &p) override {
    bitrate = std::clamp(p.bitrate.get_sum_bps(), 200000u, 20000000u);
  }
  EncoderInfo GetEncoderInfo() const override {
    EncoderInfo i;
    i.supports_native_handle = true;
    i.is_hardware_accelerated = true;
    i.implementation_name = "Vulkan-CUDA-NVENC";
    i.has_trusted_rate_controller = true;
    i.preferred_pixel_formats = {VideoFrameBuffer::Type::kNative};
    i.supports_simulcast = false;
    return i;
  }
  int32_t Encode(const VideoFrame &f, const std::vector<VideoFrameType> *types) override {
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
};
class Factory : public VideoEncoderFactory {
  Shared shared;

public:
  explicit Factory(Shared s) : shared(s) {}
  std::vector<SdpVideoFormat> GetSupportedFormats() const override {
    return {SdpVideoFormat("H264", {{"profile-level-id", "42e01f"},
                                    {"packetization-mode", "1"},
                                    {"level-asymmetry-allowed", "1"}})};
  }
  std::unique_ptr<VideoEncoder> Create(const Environment &, const SdpVideoFormat &f) override {
    if (f.name != "H264")
      return nullptr;
    return std::make_unique<Encoder>(shared);
  }
};
struct Wait {
  std::mutex mutex;
  std::condition_variable cv;
  bool done = false;
  std::string error;
  void finish(std::string e = {}) {
    {
      std::lock_guard lock(mutex);
      error = std::move(e);
      done = true;
    }
    cv.notify_all();
  }
  bool wait() {
    std::unique_lock lock(mutex);
    return cv.wait_for(lock, std::chrono::seconds(15), [&] { return done; }) && error.empty();
  }
};
class SetObserver : public SetSessionDescriptionObserver {
public:
  std::shared_ptr<Wait> result;
  explicit SetObserver(std::shared_ptr<Wait> r) : result(std::move(r)) {}
  void OnSuccess() override {
    result->finish();
  }
  void OnFailure(RTCError e) override {
    result->finish(e.message());
  }
};
class AnswerObserver : public CreateSessionDescriptionObserver {
public:
  scoped_refptr<PeerConnectionInterface> peer;
  std::shared_ptr<Wait> result;
  AnswerObserver(scoped_refptr<PeerConnectionInterface> p, std::shared_ptr<Wait> r)
      : peer(std::move(p)), result(std::move(r)) {}
  void OnSuccess(SessionDescriptionInterface *d) override {
    peer->SetLocalDescription(make_ref_counted<SetObserver>(result).get(), d);
  }
  void OnFailure(RTCError e) override {
    result->finish(e.message());
  }
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
  void OnSignalingChange(PeerConnectionInterface::SignalingState) override {}
  void OnIceGatheringChange(PeerConnectionInterface::IceGatheringState s) override {
    if (s == PeerConnectionInterface::kIceGatheringComplete && gathered)
      gathered->finish();
  }
  void OnIceCandidate(const IceCandidate *) override {}
  void OnDataChannel(scoped_refptr<DataChannelInterface> dc) override {
    if (channel)
      channel->UnregisterObserver();
    channel = dc;
    channel->RegisterObserver(this);
  }
  void OnStateChange() override {}
  void OnMessage(const DataBuffer &b) override {
    if (!b.binary && b.data.size() <= 16384)
      shared.cb.message(shared.user, reinterpret_cast<const char *>(b.data.data()), b.data.size());
  }
  void OnConnectionChange(PeerConnectionInterface::PeerConnectionState state) override {
    if (state == PeerConnectionInterface::PeerConnectionState::kFailed)
      shared.cb.error(shared.user, "ICE connection failed; configure reachable TURN server");
  }
  void close() {
    if (channel) {
      channel->UnregisterObserver();
      channel->Close();
      channel = nullptr;
    }
    if (peer) {
      peer->Close();
      peer = nullptr;
    }
  }
};
#ifdef _WIN32
#define EXPORT VIEWER_RTC_API
#else
#define EXPORT extern "C" __attribute__((visibility("default")))
#endif
EXPORT void *viewer_rtc_create(void *u, RtcCallbacks cb, const char *url, const char *user,
                               const char *pass, int, int) {
  InitializeSSL();
  LogMessage::LogToDebug(LS_WARNING);
  auto *c = new Context;
  c->shared = {u, cb};
  c->iceUrl = url ? url : "";
  c->iceUser = user ? user : "";
  c->icePass = pass ? pass : "";
  c->network = Thread::CreateWithSocketServer();
  c->worker = Thread::Create();
  c->signaling = Thread::Create();
  c->network->Start();
  c->worker->Start();
  c->signaling->Start();
  c->signaling->BlockingCall([&] {
    c->source = make_ref_counted<Source>();
    c->factory = CreatePeerConnectionFactory(
        c->network.get(), c->worker.get(), c->signaling.get(),
        CreateAudioDeviceModule(CreateEnvironment(), AudioDeviceModule::kDummyAudio),
        CreateBuiltinAudioEncoderFactory(), CreateBuiltinAudioDecoderFactory(),
        std::make_unique<Factory>(c->shared), CreateBuiltinVideoDecoderFactory(), nullptr, nullptr);
  });
  if (!c->factory) {
    cb.error(u, "Google PeerConnectionFactory creation failed");
    viewer_rtc_destroy(c);
    return nullptr;
  }
  LogMessage::LogToDebug(LS_ERROR);
  return c;
}
EXPORT int viewer_rtc_offer(void *p, const char *sdp, char *output, size_t capacity) {
  auto *c = static_cast<Context *>(p);
  std::lock_guard operation(c->operation);
  auto remote = std::make_shared<Wait>();
  c->gathered = std::make_shared<Wait>();
  c->signaling->BlockingCall([&] {
    c->close();
    PeerConnectionInterface::RTCConfiguration config;
    config.sdp_semantics = SdpSemantics::kUnifiedPlan;
    if (!c->iceUrl.empty()) {
      PeerConnectionInterface::IceServer ice;
      ice.urls = {c->iceUrl};
      ice.username = c->iceUser;
      ice.password = c->icePass;
      config.servers.push_back(ice);
    }
    auto result = c->factory->CreatePeerConnectionOrError(config, PeerConnectionDependencies(c));
    if (!result.ok()) {
      remote->finish(result.error().message());
      return;
    }
    c->peer = result.MoveValue();
    auto track = c->factory->CreateVideoTrack(c->source, "server-render");
    auto added = c->peer->AddTrack(track, {"viewer"});
    if (!added.ok()) {
      remote->finish(added.error().message());
      return;
    }
    auto desc = CreateSessionDescription(SdpType::kOffer, sdp);
    if (!desc) {
      remote->finish("invalid SDP offer");
      return;
    }
    c->peer->SetRemoteDescription(make_ref_counted<SetObserver>(remote).get(), desc.release());
  });
  auto failure = [&](std::string error) {
    if (error.empty())
      error = "SDP or ICE gathering timeout";
    if (capacity) {
      size_t n = std::min(capacity - 1, error.size());
      memcpy(output, error.data(), n);
      output[n] = 0;
    }
    c->signaling->BlockingCall([&] { c->close(); });
    return -1;
  };
  if (!remote->wait())
    return failure(remote->error);
  auto answer = std::make_shared<Wait>();
  c->signaling->BlockingCall([&] {
    PeerConnectionInterface::RTCOfferAnswerOptions options;
    c->peer->CreateAnswer(make_ref_counted<AnswerObserver>(c->peer, answer).get(), options);
  });
  if (!answer->wait())
    return failure(answer->error);
  if (!c->gathered->wait())
    return failure(c->gathered->error);
  std::string result;
  c->signaling->BlockingCall([&] { c->peer->local_description()->ToString(&result); });
  if (result.size() + 1 > capacity)
    return failure("SDP answer buffer too small");
  memcpy(output, result.c_str(), result.size() + 1);
  return 0;
}
EXPORT void viewer_rtc_push(void *p, int slot, int w, int h, int64_t us) {
  auto *c = static_cast<Context *>(p);
  auto frame = VideoFrame::Builder()
                   .set_video_frame_buffer(make_ref_counted<Native>(c->shared, slot, w, h))
                   .set_timestamp_us(us)
                   .set_rotation(kVideoRotation_0)
                   .build();
  c->source->broadcaster.OnFrame(frame);
}
EXPORT void viewer_rtc_send(void *p, const char *text) {
  auto *c = static_cast<Context *>(p);
  c->signaling->PostTask([c, message = std::string(text)] {
    if (c->channel && c->channel->state() == DataChannelInterface::kOpen &&
        c->channel->buffered_amount() < 65536)
      c->channel->Send(DataBuffer(message));
  });
}
EXPORT void viewer_rtc_close_peer(void *p) {
  auto *c = static_cast<Context *>(p);
  std::lock_guard lock(c->operation);
  c->signaling->BlockingCall([&] { c->close(); });
}
EXPORT void viewer_rtc_destroy(void *p) {
  if (!p)
    return;
  auto *c = static_cast<Context *>(p);
  c->signaling->BlockingCall([&] {
    c->close();
    c->source = nullptr;
    c->factory = nullptr;
  });
  c->signaling->Stop();
  c->worker->Stop();
  c->network->Stop();
  delete c;
  CleanupSSL();
}
