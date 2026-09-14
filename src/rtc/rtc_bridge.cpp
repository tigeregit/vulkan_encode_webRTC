// The only exported surface of the bridge. Everything crossing this boundary is
// plain C -- a void*, size_t and byte pointers -- so libwebrtc's std::__Cr
// (Linux) / static-CRT (Windows) ABI never leaks into the viewer.
#define VIEWER_RTC_BUILD
#include "rtc/rtc_internal.hpp"
#include <cstring>
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
}
