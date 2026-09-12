"use strict";
const session = crypto.randomUUID();
const $ = (id) => document.getElementById(id),
  video = $("video");
let pc = null,
  dc = null,
  config = null,
  sequence = 0,
  connected = false,
  connecting = false;
let camera = { yaw: 0.5, pitch: 0.25, distance: 3.5, panX: 0, panY: 0 },
  dirty = false;
let clockOffset = null,
  clockUncertainty = null,
  bestRtt = Infinity,
  lastStats = null;
const frames = new Map(),
  presentations = new Map(),
  seen = new Set(),
  samples = {};
const stages = [
  ["total", "控制 → 呈现", "同一帧编号 / 浏览器时钟"],
  ["upload", "控制上传 ≈", "时钟对齐估算"],
  ["processing", "控制处理 / 等待", "服务器单调时钟"],
  ["render", "Vulkan 渲染", "GPU 时间戳"],
  ["convert", "CUDA 色彩转换", "显式 stream CUDA events"],
  ["encode", "NVENC 编码", "提交 → 码流可读"],
  ["downlink", "编码后 → 接收 ≈", "含发送队列、分包与网络"],
  ["decode", "浏览器解码", "每帧 processingDuration"],
  ["display", "接收 → 呈现", "含解码、抖动缓冲及合成"],
];
for (const [id, label, method] of stages) {
  samples[id] = [];
  const tr = document.createElement("tr");
  tr.id = "row-" + id;
  for (const text of [label, "—", "—", "—", "0", method]) {
    const td = document.createElement("td");
    td.textContent = text;
    tr.append(td);
  }
  $("latencies").append(tr);
}
function log(text) {
  const node = document.createElement("div");
  node.textContent = new Date().toLocaleTimeString() + " " + text;
  $("events").prepend(node);
  while ($("events").children.length > 8) $("events").lastChild.remove();
}
function add(id, value) {
  if (!Number.isFinite(value) || value < 0 || value > 60000) return;
  samples[id].push(value);
  if (samples[id].length > 512) samples[id].shift();
}
function pct(a, p) {
  if (!a.length) return null;
  const b = [...a].sort((x, y) => x - y);
  return b[Math.min(b.length - 1, Math.ceil(b.length * p) - 1)];
}
function ms(v) {
  return v == null ? "—" : v.toFixed(v < 1 ? 3 : 1) + " ms";
}
function redraw() {
  for (const [id] of stages) {
    const cells = $("row-" + id).children;
    [0.5, 0.95, 0.99].forEach(
      (p, i) => (cells[i + 1].textContent = ms(pct(samples[id], p))),
    );
    cells[4].textContent = samples[id].length;
  }
  $("total").textContent = ms(pct(samples.total, 0.5));
  $("clock").textContent =
    clockOffset == null
      ? "时钟尚未同步。单向网络值是估算。"
      : `时钟估算不确定度 ±${clockUncertainty.toFixed(1)} ms；基于最小 RTT ${bestRtt.toFixed(1)} ms 的往返样本，假设路径对称。`;
}
function send(obj) {
  if (dc?.readyState === "open" && dc.bufferedAmount < 16384)
    dc.send(JSON.stringify(obj));
}
function flushCamera() {
  if (
    dirty &&
    connected &&
    dc?.readyState === "open" &&
    dc.bufferedAmount < 16384
  ) {
    dirty = false;
    send({
      type: "camera",
      ...camera,
      seq: ++sequence,
      client: performance.now(),
    });
  }
  requestAnimationFrame(flushCamera);
}
requestAnimationFrame(flushCamera);
function match(id) {
  const f = frames.get(id),
    p = presentations.get(id);
  if (!f || !p) return;
  if (f.seq && !seen.has(f.seq)) {
    seen.add(f.seq);
    if (seen.size > 1024) seen.delete(seen.values().next().value);
    add("total", p.present - f.client);
    add("processing", f.applied - f.received);
    if (clockOffset != null) add("upload", f.received - clockOffset - f.client);
  }
  if (clockOffset != null && p.receive != null)
    add("downlink", p.receive - (f.encodeEnd - clockOffset));
  if (p.decode != null) add("decode", p.decode);
  if (p.receive != null) add("display", p.present - p.receive);
  frames.delete(id);
  presentations.delete(id);
}
function onMessage(event) {
  try {
    const j = JSON.parse(event.data);
    if (j.type === "pong") {
      const t = performance.now(),
        rtt = t - j.client - (j.sent - j.received);
      if (rtt >= 0 && rtt < bestRtt) {
        bestRtt = rtt;
        clockOffset = (j.received - j.client + (j.sent - t)) / 2;
        clockUncertainty = rtt / 2;
      }
    } else if (j.type === "frame") {
      add("render", j.renderMs);
      add("convert", j.convertMs);
      add("encode", j.encodeMs);
      frames.set(j.frame % 16777216, j);
      if (frames.size > 256) frames.delete(frames.keys().next().value);
      match(j.frame % 16777216);
    } else if (j.type === "loaded") {
      $("loaded").textContent = j.id;
      log(`加载 ${j.id} · ${j.triangles.toLocaleString()} 个三角形`);
    } else if (j.type === "error") log("错误：" + j.message);
  } catch (e) {
    log("遥测解析失败：" + e.message);
  }
}
const marker = document.createElement("canvas");
marker.width = 192;
marker.height = 8;
const markerContext = marker.getContext("2d", { willReadFrequently: true });
function presented(now, metadata) {
  if ($("debug").checked && video.videoWidth) {
    markerContext.drawImage(video, 0, 0, 192, 8, 0, 0, 192, 8);
    const data = markerContext.getImageData(0, 0, 192, 8).data;
    let id = 0;
    for (let bit = 0; bit < 24; bit++) {
      const p = (4 * 192 + bit * 8 + 4) * 4;
      if (data[p] + data[p + 1] + data[p + 2] > 384) id += 2 ** bit;
    }
    presentations.set(id, {
      present: metadata.expectedDisplayTime ?? now,
      receive: metadata.receiveTime,
      decode:
        metadata.processingDuration == null
          ? null
          : metadata.processingDuration * 1000,
    });
    if (presentations.size > 256)
      presentations.delete(presentations.keys().next().value);
    match(id);
  }
  video.requestVideoFrameCallback(presented);
}
if (video.requestVideoFrameCallback) video.requestVideoFrameCallback(presented);
else log("此浏览器不支持视频帧回调，无法测量控制到呈现的延迟");
async function gather(peer) {
  if (peer.iceGatheringState === "complete") return;
  await new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      peer.removeEventListener("icegatheringstatechange", changed);
      reject(new Error("ICE 收集超时，请检查 TURN 配置"));
    }, 15000);
    function changed() {
      if (peer.iceGatheringState === "complete") {
        clearTimeout(timer);
        peer.removeEventListener("icegatheringstatechange", changed);
        resolve();
      }
    }
    peer.addEventListener("icegatheringstatechange", changed);
  });
}
async function disconnect() {
  dc?.close();
  pc?.close();
  dc = pc = null;
  connected = false;
  $("connect").textContent = "连接服务器";
  $("status").textContent = "未连接";
  $("empty").style.display = "grid";
  video.srcObject = null;
  await fetch("/api/disconnect", {
    method: "POST",
    body: JSON.stringify({ session }),
  }).catch(() => {});
}
$("connect").onclick = async () => {
  if (connected) return disconnect();
  if (connecting) return;
  connecting = true;
  $("connect").disabled = true;
  $("status").textContent = "建立连接…";
  try {
    config = await (await fetch("/api/config")).json();
    pc = new RTCPeerConnection({ iceServers: config.iceServers });
    pc.addTransceiver("video", { direction: "recvonly" });
    const transceiver = pc.getTransceivers()[0];
    const codecs = RTCRtpReceiver.getCapabilities("video").codecs.filter(
      (c) => c.mimeType.toLowerCase() === "video/h264",
    );
    if (!codecs.length) throw new Error("浏览器缺少 H.264 解码支持");
    transceiver.setCodecPreferences(codecs);
    dc = pc.createDataChannel("control", { ordered: true });
    dc.onmessage = onMessage;
    dc.onopen = () => {
      log("控制通道已连接");
      dirty = true;
      send({ type: "ping", client: performance.now() });
    };
    pc.ontrack = (e) => {
      video.srcObject = new MediaStream([e.track]);
      video.play().catch((e) => log(e.message));
      $("empty").style.display = "none";
    };
    pc.onconnectionstatechange = () => {
      $("status").textContent = pc?.connectionState ?? "未连接";
      if (pc?.connectionState === "failed")
        log("连接失败，请检查 TURN 或网络端口");
    };
    await pc.setLocalDescription(await pc.createOffer());
    await gather(pc);
    const response = await fetch("/api/offer", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        type: pc.localDescription.type,
        sdp: pc.localDescription.sdp,
        session,
      }),
    });
    if (!response.ok) throw new Error(await response.text());
    await pc.setRemoteDescription(await response.json());
    connected = true;
    $("connect").textContent = "断开连接";
    bestRtt = Infinity;
    clockOffset = null;
    lastStats = null;
    log("视频已协商：H.264 / Google WebRTC");
  } catch (e) {
    log(e.message);
    pc?.close();
    pc = null;
    $("status").textContent = "连接失败";
    await fetch("/api/disconnect", {
      method: "POST",
      body: JSON.stringify({ session }),
    }).catch(() => {});
  } finally {
    connecting = false;
    $("connect").disabled = false;
  }
};
$("reset").onclick = () => {
  camera = { yaw: 0.5, pitch: 0.25, distance: 3.5, panX: 0, panY: 0 };
  dirty = true;
};
const pointers = new Map();
let previousGesture = null;
video.oncontextmenu = (e) => e.preventDefault();
video.onpointerdown = (e) => {
  video.setPointerCapture(e.pointerId);
  pointers.set(e.pointerId, { x: e.clientX, y: e.clientY });
  previousGesture = null;
};
video.onpointermove = (e) => {
  if (!pointers.has(e.pointerId)) return;
  const old = pointers.get(e.pointerId);
  pointers.set(e.pointerId, { x: e.clientX, y: e.clientY });
  if (pointers.size === 2) {
    const [a, b] = [...pointers.values()],
      g = {
        x: (a.x + b.x) / 2,
        y: (a.y + b.y) / 2,
        d: Math.hypot(a.x - b.x, a.y - b.y),
      };
    if (previousGesture) {
      camera.distance *= previousGesture.d / Math.max(g.d, 1);
      camera.panX -= (g.x - previousGesture.x) * 0.003 * camera.distance;
      camera.panY += (g.y - previousGesture.y) * 0.003 * camera.distance;
    }
    previousGesture = g;
  } else if (e.buttons === 2 || e.shiftKey) {
    camera.panX -= (e.clientX - old.x) * 0.002 * camera.distance;
    camera.panY += (e.clientY - old.y) * 0.002 * camera.distance;
  } else {
    camera.yaw -= (e.clientX - old.x) * 0.007;
    camera.pitch += (e.clientY - old.y) * 0.007;
  }
  clamp();
  dirty = true;
};
function clamp() {
  camera.pitch = Math.max(-1.5, Math.min(1.5, camera.pitch));
  camera.distance = Math.max(0.3, Math.min(25, camera.distance));
  camera.panX = Math.max(-10, Math.min(10, camera.panX));
  camera.panY = Math.max(-10, Math.min(10, camera.panY));
}
for (const type of ["pointerup", "pointercancel", "lostpointercapture"])
  video.addEventListener(type, (e) => {
    pointers.delete(e.pointerId);
    previousGesture = null;
  });
video.addEventListener(
  "wheel",
  (e) => {
    e.preventDefault();
    camera.distance *= Math.exp(
      Math.max(-500, Math.min(500, e.deltaY)) * 0.001,
    );
    clamp();
    dirty = true;
  },
  { passive: false },
);
$("clear").onclick = () => {
  for (const key of Object.keys(samples)) samples[key] = [];
  seen.clear();
  frames.clear();
  presentations.clear();
  redraw();
};
$("export").onclick = () => {
  const result = {
    at: new Date().toISOString(),
    clock: {
      offsetMs: clockOffset,
      uncertaintyMs: clockUncertainty,
      bestRttMs: bestRtt,
    },
    units: "milliseconds",
    samples,
    notes: "One-way values are estimates. Do not sum stage percentiles.",
  };
  const url = URL.createObjectURL(
    new Blob([JSON.stringify(result, null, 2)], { type: "application/json" }),
  );
  const a = document.createElement("a");
  a.href = url;
  a.download = "viewer-latency.json";
  a.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
};
setInterval(async () => {
  if (!connected || !pc) return;
  send({ type: "ping", client: performance.now() });
  try {
    const stats = await pc.getStats();
    let inbound, pair, codec;
    stats.forEach((s) => {
      if (s.type === "inbound-rtp" && s.kind === "video") inbound = s;
      if (s.type === "candidate-pair" && s.state === "succeeded" && s.nominated)
        pair = s;
    });
    if (inbound) {
      codec = stats.get(inbound.codecId);
      const elapsed = lastStats
        ? (inbound.timestamp - lastStats.timestamp) / 1000
        : 0;
      if (elapsed > 0) {
        $("bandwidth").textContent =
          (
            ((inbound.bytesReceived - lastStats.bytesReceived) * 8) /
            elapsed /
            1e6
          ).toFixed(2) + " Mbps";
        $("fps").textContent =
          ((inbound.framesDecoded - lastStats.framesDecoded) / elapsed).toFixed(
            0,
          ) + " fps";
      }
      $("stream").textContent =
        `${inbound.frameWidth ?? 0} × ${inbound.frameHeight ?? 0} · ${codec?.mimeType ?? "H264"} · ${inbound.decoderImplementation ?? "浏览器解码"}`;
      lastStats = inbound;
    }
    $("rtt").textContent = ms(
      pair?.currentRoundTripTime == null
        ? null
        : pair.currentRoundTripTime * 1000,
    );
    const local = pair && stats.get(pair.localCandidateId),
      remote = pair && stats.get(pair.remoteCandidateId);
    $("details").textContent = JSON.stringify(
      {
        packetsLost: inbound?.packetsLost,
        framesDropped: inbound?.framesDropped,
        jitterMs: inbound?.jitter * 1000,
        jitterBufferMeanMs: inbound?.jitterBufferEmittedCount
          ? (inbound.jitterBufferDelay / inbound.jitterBufferEmittedCount) *
            1000
          : null,
        decodeMeanMs: inbound?.framesDecoded
          ? (inbound.totalDecodeTime / inbound.framesDecoded) * 1000
          : null,
        availableIncomingMbps: pair?.availableIncomingBitrate / 1e6,
        ice: local
          ? `${local.candidateType}/${local.protocol} → ${remote?.candidateType}/${remote?.protocol}`
          : null,
        framesMatched: samples.total.length,
        clockUncertaintyMs: clockUncertainty,
        rawFrameHostCopies: 0,
        gpuCopies: config.gpuCopies,
      },
      null,
      2,
    );
  } catch (e) {
    log(e.message);
  }
  redraw();
}, 1000);
window.addEventListener("pagehide", () => {
  if (connected)
    navigator.sendBeacon("/api/disconnect", JSON.stringify({ session }));
});
(async () => {
  try {
    const [c, models] = await Promise.all([
      fetch("/api/config").then((r) => r.json()),
      fetch("/api/models").then((r) => r.json()),
    ]);
    config = c;
    $("gpu").textContent = c.gpu;
    $("format").textContent = `${c.width} × ${c.height} · ${c.codec}`;
    $("loaded").textContent = models[0] ?? "无模型";
    for (const name of models) {
      const button = document.createElement("button");
      button.textContent = name;
      button.onclick = () => {
        if (!connected) return log("请先连接服务器");
        send({ type: "load", id: name });
        for (const b of $("models").children)
          b.classList.toggle("selected", b === button);
      };
      $("models").append(button);
    }
  } catch (e) {
    log("无法读取服务器配置：" + e.message);
  }
})();
// Read-only test/debug snapshot; contains no model geometry or credentials.
window.viewerDebug = () => ({
  connected,
  camera: { ...camera },
  sampleCounts: Object.fromEntries(
    Object.entries(samples).map(([k, v]) => [k, v.length]),
  ),
  p50: Object.fromEntries(
    Object.entries(samples).map(([k, v]) => [k, pct(v, 0.5)]),
  ),
  clockUncertainty,
  video: { width: video.videoWidth, height: video.videoHeight },
  errors: $("events").textContent,
});
