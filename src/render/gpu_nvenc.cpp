// NVENC H.264 session setup and the per-frame encode call. The encoder is
// configured for P1 / ultra-low-latency, CBR, no B frames and no lookahead;
// WebRTC drives the bitrate and IDR requests through encode().
#include "render/gpu_internal.hpp"
#include <cstdint>
#include <cstdlib>
#include <iostream>
using PNVENCODEAPICREATEINSTANCE = NVENCSTATUS(NVENCAPI *)(NV_ENCODE_API_FUNCTION_LIST *);
using PNVENCODEAPIGETMAXSUPPORTEDVERSION = NVENCSTATUS(NVENCAPI *)(uint32_t *);
void Gpu::initEncoder() {
  nv = std::make_unique<NvencState>();
#ifdef _WIN32
  nv->lib = std::make_unique<DynamicLibrary>("nvEncodeAPI64.dll");
#else
  nv->lib = std::make_unique<DynamicLibrary>("libnvidia-encode.so.1");
#endif
  auto create = (PNVENCODEAPICREATEINSTANCE)nv->lib->symbol("NvEncodeAPICreateInstance");
  auto version =
      (PNVENCODEAPIGETMAXSUPPORTEDVERSION)nv->lib->symbol("NvEncodeAPIGetMaxSupportedVersion");
  if (!create || !version)
    throw std::runtime_error("NVENC entrypoints missing");
  uint32_t max = 0;
  NV(version(&max));
  if (max < ((NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION))
    throw std::runtime_error("driver NVENC API older than SDK 13.0; update driver");
  CUcontext context{};
  if (cuCtxGetCurrent(&context) != CUDA_SUCCESS || !context)
    throw std::runtime_error("CUDA primary context missing");
  nv->api.version = NV_ENCODE_API_FUNCTION_LIST_VER;
  NV(create(&nv->api));
  NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open{NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER};
  open.device = context;
  open.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
  open.apiVersion = NVENCAPI_VERSION;
  NV(nv->api.nvEncOpenEncodeSessionEx(&open, nv->session.out(&nv->api)));
  void *session = nv->session;
  NV_ENC_PRESET_CONFIG preset{NV_ENC_PRESET_CONFIG_VER};
  preset.presetCfg.version = NV_ENC_CONFIG_VER;
  NV(nv->api.nvEncGetEncodePresetConfigEx(session, NV_ENC_CODEC_H264_GUID, NV_ENC_PRESET_P1_GUID,
                                          NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &preset));
  auto config = preset.presetCfg;
  config.profileGUID = NV_ENC_H264_PROFILE_BASELINE_GUID;
  config.gopLength = 120;
  config.frameIntervalP = 1;
  config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
  config.rcParams.averageBitRate = nv->currentBitrate;
  config.rcParams.maxBitRate = nv->currentBitrate;
  config.rcParams.vbvBufferSize = nv->currentBitrate / 30;
  config.rcParams.vbvInitialDelay = config.rcParams.vbvBufferSize;
  config.rcParams.enableLookahead = 0;
  config.rcParams.zeroReorderDelay = 1;
  auto &h = config.encodeCodecConfig.h264Config;
  h.idrPeriod = 120;
  h.repeatSPSPPS = 1;
  h.chromaFormatIDC = 1;
  h.h264VUIParameters.videoSignalTypePresentFlag = 1;
  h.h264VUIParameters.colourDescriptionPresentFlag = 1;
  h.h264VUIParameters.colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
  h.h264VUIParameters.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
  h.h264VUIParameters.colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_BT709;
  h.h264VUIParameters.videoFullRangeFlag = 0;
  NV_ENC_INITIALIZE_PARAMS init{NV_ENC_INITIALIZE_PARAMS_VER};
  init.encodeGUID = NV_ENC_CODEC_H264_GUID;
  init.presetGUID = NV_ENC_PRESET_P1_GUID;
  init.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
  init.encodeWidth = init.darWidth = width;
  init.encodeHeight = init.darHeight = height;
  init.frameRateNum = 30;
  init.frameRateDen = 1;
  init.enablePTD = 1;
  init.encodeConfig = &config;
  init.maxEncodeWidth = width;
  init.maxEncodeHeight = height;
  NV(nv->api.nvEncInitializeEncoder(session, &init));
  nv->config = config;
  nv->init = init;
  // Re-point the stored params at the stored config so a later reconfigure does
  // not read the local copy.
  nv->init.encodeConfig = &nv->config;
  for (int i = 0; i < SLOT_COUNT; i++) {
    auto &s = slots[i];
    auto &res = nv->resources[i];
    NV_ENC_REGISTER_RESOURCE reg{NV_ENC_REGISTER_RESOURCE_VER};
    reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR;
    reg.resourceToRegister = s.nv12;
    reg.width = width;
    reg.height = height;
    reg.pitch = pitch;
    reg.bufferFormat = NV_ENC_BUFFER_FORMAT_NV12;
    reg.bufferUsage = NV_ENC_INPUT_IMAGE;
    auto status = nv->api.nvEncRegisterResource(session, &reg);
    if (status == NV_ENC_ERR_RESOURCE_REGISTER_FAILED && std::getenv("VIEWER_NVENC_LEGACY_INPUT") &&
        std::string(std::getenv("VIEWER_NVENC_LEGACY_INPUT")) == "1") {
      // Opt-in compatibility: fixed initialization-only NVENC allocations. Never in the frame loop.
      void *raw = nullptr;
      CU(cudaMalloc(&raw, size_t(pitch) * height * 3 / 2));
      s.legacyBuffer.adopt(raw);
      s.legacyInput = true;
      s.encodeInput = static_cast<unsigned char *>(raw);
      reg.resourceToRegister = s.encodeInput;
      NV(nv->api.nvEncRegisterResource(session, &reg));
    } else
      NV(status);
    res.registered.adopt(session, &nv->api, reg.registeredResource);
    NV_ENC_CREATE_BITSTREAM_BUFFER bs{NV_ENC_CREATE_BITSTREAM_BUFFER_VER};
    NV(nv->api.nvEncCreateBitstreamBuffer(session, &bs));
    res.bitstream.adopt(session, &nv->api, bs.bitstreamBuffer);
  }
  std::cout << "GPU: " << deviceName << " | Vulkan/CUDA UUID matched | NVENC H264 | " << SLOT_COUNT
            << " slots | explicit CUDA streams + pool\n";
}
int Gpu::encode(int i, bool key, unsigned bitrate, const unsigned char **data, size_t *length,
                int *isKey) {
  auto &s = slots.at(i);
  auto &res = nv->resources[i];
  void *session = nv->session;
  CU(cudaSetDevice(cudaDevice));
  CU(cudaStreamSynchronize(s.stream));
  CU(cudaEventElapsedTime(&s.timing.convertMs, s.start, s.end));
  uint64_t ts[2]{};
  VK(vkGetQueryPoolResults(device, queries, i * 2, 2, sizeof(ts), ts, sizeof(uint64_t),
                           VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
  s.timing.renderMs = float(ts[1] - ts[0]) * timestampPeriod / 1e6f;
  // nvEncSetIOCudaStreams keeps these pointers for the session's lifetime, so it
  // must be given the slot's stored handle, not a local copy.
  NV(nv->api.nvEncSetIOCudaStreams(session, s.stream.address(), s.stream.address()));
  if (bitrate >= 200000 && bitrate != nv->currentBitrate) {
    nv->config.rcParams.averageBitRate = nv->config.rcParams.maxBitRate = bitrate;
    nv->config.rcParams.vbvBufferSize = nv->config.rcParams.vbvInitialDelay = bitrate / 30;
    NV_ENC_RECONFIGURE_PARAMS reconfigure{NV_ENC_RECONFIGURE_PARAMS_VER};
    reconfigure.reInitEncodeParams = nv->init;
    NV(nv->api.nvEncReconfigureEncoder(session, &reconfigure));
    nv->currentBitrate = bitrate;
  }
  s.timing.encodeStart = now_ms();
  // The mapping spans this call and unlock(), so it cannot be a local owner. A
  // failed NVENC call in between must still release it, otherwise the slot stays
  // mapped and can never be encoded again.
  NV_ENC_MAP_INPUT_RESOURCE map{NV_ENC_MAP_INPUT_RESOURCE_VER};
  map.registeredResource = res.registered;
  NV(nv->api.nvEncMapInputResource(session, &map));
  res.mapped.adopt(session, &nv->api, map.mappedResource);
  NV_ENC_PIC_PARAMS pic{NV_ENC_PIC_PARAMS_VER};
  pic.inputBuffer = res.mapped;
  pic.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;
  pic.inputWidth = width;
  pic.inputHeight = height;
  pic.inputPitch = pitch;
  pic.outputBitstream = res.bitstream;
  pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
  pic.inputTimeStamp = s.timing.frame;
  if (key)
    pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
  NV_ENC_LOCK_BITSTREAM lock{NV_ENC_LOCK_BITSTREAM_VER};
  lock.outputBitstream = res.bitstream;
  lock.doNotWait = 0;
  try {
    NV(nv->api.nvEncEncodePicture(session, &pic));
    NV(nv->api.nvEncLockBitstream(session, &lock));
  } catch (...) {
    res.mapped.reset();
    throw;
  }
  s.timing.encodeEnd = now_ms();
  *data = (const unsigned char *)lock.bitstreamBufferPtr;
  *length = lock.bitstreamSizeInBytes;
  *isKey = lock.pictureType == NV_ENC_PIC_TYPE_IDR;
  return 0;
}
void Gpu::unlock(int i) {
  auto &res = nv->resources[i];
  NV(nv->api.nvEncUnlockBitstream(nv->session, res.bitstream));
  res.mapped.reset();
}
