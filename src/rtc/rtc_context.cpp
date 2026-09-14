// PeerConnection lifecycle and observer plumbing. Everything here runs on the
// libwebrtc signalling thread; the viewer only ever calls in through
// viewer_rtc_offer / viewer_rtc_close_peer / viewer_rtc_destroy.
#include "rtc/rtc_internal.hpp"
void Wait::finish(std::string e) {
  {
    std::lock_guard lock(mutex);
    error = std::move(e);
    done = true;
  }
  cv.notify_all();
}
bool Wait::wait() {
  std::unique_lock lock(mutex);
  return cv.wait_for(lock, std::chrono::seconds(15), [&] { return done; }) && error.empty();
}
void SetObserver::OnSuccess() {
  result->finish();
}
void SetObserver::OnFailure(RTCError e) {
  result->finish(e.message());
}
void AnswerObserver::OnSuccess(SessionDescriptionInterface *d) {
  peer->SetLocalDescription(make_ref_counted<SetObserver>(result).get(), d);
}
void AnswerObserver::OnFailure(RTCError e) {
  result->finish(e.message());
}
void Context::OnSignalingChange(PeerConnectionInterface::SignalingState) {}
void Context::OnIceGatheringChange(PeerConnectionInterface::IceGatheringState s) {
  if (s == PeerConnectionInterface::kIceGatheringComplete && gathered)
    gathered->finish();
}
void Context::OnIceCandidate(const IceCandidate *) {}
void Context::OnDataChannel(scoped_refptr<DataChannelInterface> dc) {
  if (channel)
    channel->UnregisterObserver();
  channel = dc;
  channel->RegisterObserver(this);
}
void Context::OnStateChange() {}
void Context::OnMessage(const DataBuffer &b) {
  if (!b.binary && b.data.size() <= 16384)
    shared.cb.message(shared.user, reinterpret_cast<const char *>(b.data.data()), b.data.size());
}
void Context::OnConnectionChange(PeerConnectionInterface::PeerConnectionState state) {
  if (state == PeerConnectionInterface::PeerConnectionState::kFailed)
    shared.cb.error(shared.user, "ICE connection failed; configure reachable TURN server");
}
void Context::close() {
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
