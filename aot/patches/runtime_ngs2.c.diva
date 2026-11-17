/* DIVA minimal Ngs2 audio HLE: valid opaque handles + plausible buffer sizes + silence render.
 * Full synthesis is a later phase; this unblocks game audio subsystem initialization. */
#include "runtime.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef struct { void* hostBuffer; size_t hostBufferSize; uintptr_t reserved[5]; uintptr_t userData; } Ngs2BufInfo;
typedef struct { uint32_t stateFlags; } Ngs2VoiceState;
typedef struct { void* buffer; size_t bufferSize; uint32_t waveformType; uint32_t numChannels; } Ngs2RenderBuf;

static ABI int32_t ngs2_system_create(const void* option, const Ngs2BufInfo* bi, uintptr_t* out) { (void)option;(void)bi;
    void* h = malloc(64); if (!h) return 0x80260001; memset(h,0,64); if (out) *out = (uintptr_t)h; return 0;
}
static ABI int32_t ngs2_system_destroy(uintptr_t h, Ngs2BufInfo* out) { (void)out; if (h) free((void*)h); return 0; }
static ABI int32_t ngs2_system_query_bufsize(const void* option, Ngs2BufInfo* out) { (void)option;
    if (out) { out->hostBufferSize = 0x200000; } return 0;
}
static ABI int32_t ngs2_system_render(uintptr_t h, const Ngs2RenderBuf* bufs, uint32_t n) { (void)h;
    for (uint32_t i = 0; i < n; ++i) if (bufs && bufs[i].buffer && bufs[i].bufferSize) memset(bufs[i].buffer, 0, bufs[i].bufferSize);
    return 0;
}
static ABI int32_t ngs2_rack_create(uintptr_t sys, uint32_t rackId, const void* option, const Ngs2BufInfo* bi, uintptr_t* out) { (void)sys;(void)rackId;(void)option;(void)bi;
    void* h = malloc(64); if (!h) return 0x80260001; memset(h,0,64); if (out) *out = (uintptr_t)h; return 0;
}
static ABI int32_t ngs2_rack_destroy(uintptr_t h, Ngs2BufInfo* out) { (void)out; if (h) free((void*)h); return 0; }
static ABI int32_t ngs2_rack_query_bufsize(uint32_t rackId, const void* option, Ngs2BufInfo* out) { (void)rackId;(void)option;
    if (out) { out->hostBufferSize = 0x100000; } return 0;
}
static ABI int32_t ngs2_rack_get_voice(uintptr_t rack, uint32_t idx, uintptr_t* out) { (void)rack;(void)idx;
    void* h = malloc(32); if (!h) return 0x80260001; memset(h,0,32); if (out) *out = (uintptr_t)h; return 0;
}
static ABI int32_t ngs2_voice_control(uintptr_t voice, const void* ctrl, uint32_t n) { (void)voice;(void)ctrl;(void)n; return 0; }
static ABI int32_t ngs2_voice_get_state(uintptr_t voice, Ngs2VoiceState* out, size_t sz) { (void)voice;(void)sz; if (out) out->stateFlags = 0; return 0; }
static ABI int32_t ngs2_voice_get_state_flags(uintptr_t voice, uint32_t* out) { (void)voice; if (out) *out = 0; return 0; }

static const RuntimeExport exports[] = {
    {"sceNgs2SystemCreate",ngs2_system_create}, {"sceNgs2SystemDestroy",ngs2_system_destroy},
    {"sceNgs2SystemQueryBufferSize",ngs2_system_query_bufsize}, {"sceNgs2SystemRender",ngs2_system_render},
    {"sceNgs2RackCreate",ngs2_rack_create}, {"sceNgs2RackDestroy",ngs2_rack_destroy},
    {"sceNgs2RackQueryBufferSize",ngs2_rack_query_bufsize}, {"sceNgs2RackGetVoiceHandle",ngs2_rack_get_voice},
    {"sceNgs2VoiceControl",ngs2_voice_control}, {"sceNgs2VoiceGetState",ngs2_voice_get_state},
    {"sceNgs2VoiceGetStateFlags",ngs2_voice_get_state_flags},
};
uintptr_t runtime_ngs2_resolve(const char* name) { return RUNTIME_LOOKUP(exports, name); }
