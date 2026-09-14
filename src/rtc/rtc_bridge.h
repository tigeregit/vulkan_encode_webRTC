#pragma once
#include <stddef.h>
#include <stdint.h>
#if defined(_WIN32) && defined(VIEWER_RTC_BUILD)
#define VIEWER_RTC_API __declspec(dllexport)
#else
#define VIEWER_RTC_API
#endif
#ifdef __cplusplus
extern "C" {
#endif
struct RtcCallbacks {
  void (*message)(void *, const char *, size_t);
  int (*encode)(void *, int, int, unsigned, uint32_t, const unsigned char **, size_t *, int *);
  void (*encoded_done)(void *, int, uint32_t);
  void (*release_slot)(void *, int);
  void (*error)(void *, const char *);
};
VIEWER_RTC_API void *viewer_rtc_create(void *, struct RtcCallbacks, const char *ice_url,
                                       const char *ice_user, const char *ice_pass, int min_port,
                                       int max_port);
VIEWER_RTC_API int viewer_rtc_offer(void *, const char *, char *, size_t);
VIEWER_RTC_API void viewer_rtc_push(void *, int, int, int, int64_t);
VIEWER_RTC_API void viewer_rtc_send(void *, const char *);
VIEWER_RTC_API void viewer_rtc_close_peer(void *);
VIEWER_RTC_API void viewer_rtc_destroy(void *);
#ifdef __cplusplus
}
#endif
