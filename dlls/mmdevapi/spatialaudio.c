/*
 * Copyright 2020 Andrew Eikum for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#define COBJMACROS

#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "winnls.h"
#include "winreg.h"
#include "wine/debug.h"
#include "wine/list.h"

#include "ole2.h"
#include "mmdeviceapi.h"
#include "mmsystem.h"
#include "audioclient.h"
#include "endpointvolume.h"
#include "audiopolicy.h"
#include "spatialaudioclient.h"

#include "mmdevapi_private.h"
#include <math.h>

WINE_DEFAULT_DEBUG_CHANNEL(mmdevapi);

static BOOL static_object_direction(AudioObjectType type, float *az, float *el);

static UINT32 AudioObjectType_to_index(AudioObjectType type)
{
    UINT32 o = 0;
    while(type){
        type >>= 1;
        ++o;
    }
    return o - 2;
}

static const char *debugstr_fmtex(const WAVEFORMATEX *fmt)
{
    static char buf[2048];

    if (!fmt)
    {
        strcpy(buf, "(null)");
    }
    else if(fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
    {
        const WAVEFORMATEXTENSIBLE *fmtex = (const WAVEFORMATEXTENSIBLE *)fmt;
        snprintf(buf, sizeof(buf), "tag: 0x%x (%s), ch: %u (mask: 0x%lx), rate: %lu, depth: %u",
                fmt->wFormatTag, debugstr_guid(&fmtex->SubFormat),
                fmt->nChannels, fmtex->dwChannelMask, fmt->nSamplesPerSec,
                fmt->wBitsPerSample);
    }
    else
    {
        snprintf(buf, sizeof(buf), "tag: 0x%x, ch: %u, rate: %lu, depth: %u",
                fmt->wFormatTag, fmt->nChannels, fmt->nSamplesPerSec,
                fmt->wBitsPerSample);
    }
    return buf;
}

static BOOL formats_equal(const WAVEFORMATEX *fmt1, const WAVEFORMATEX *fmt2)
{
    return !memcmp(fmt1, fmt2, sizeof(*fmt1)) && !memcmp(fmt1 + 1, fmt2 + 1, fmt1->cbSize);
}

typedef struct SpatialAudioImpl SpatialAudioImpl;
typedef struct SpatialAudioStreamImpl SpatialAudioStreamImpl;
typedef struct SpatialAudioObjectImpl SpatialAudioObjectImpl;

struct SpatialAudioObjectImpl {
    ISpatialAudioObject ISpatialAudioObject_iface;
    LONG ref;

    SpatialAudioStreamImpl *sa_stream;
    AudioObjectType type;
    UINT32 static_idx;

    float *buf;

    /* dynamic objects (WineForge-dev, Luke David 1, 21 Oct 2026): position in the Windows spatial
     * frame (x right, y up, z toward the listener, metres), gain, and end-of-stream state */
    float x, y, z, vol;
    BOOL ending, inactive;

    struct list entry;
};

#define WINE_SA_MAX_DYNAMIC 32
#define SA_PI 3.14159265f
#define WINE_SA_MAX_BED 16

struct sa_bed_speaker {
    UINT32 chan;      /* channel index in the stream */
    float az, el;     /* degrees; az 0 = front, positive = right */
    BOOL top;
};

struct SpatialAudioStreamImpl {
    ISpatialAudioObjectRenderStream ISpatialAudioObjectRenderStream_iface;
    LONG ref;
    CRITICAL_SECTION lock;

    SpatialAudioImpl *sa_client;
    SpatialAudioObjectRenderStreamActivationParams params;

    IAudioClient *client;
    IAudioRenderClient *render;

    UINT32 period_frames, update_frames;
    WAVEFORMATEXTENSIBLE stream_fmtex;

    float *buf;

    UINT32 dyn_max, dyn_active;
    /* output speakers of the device mix format (LFE kept separately), used to render every object */
    struct sa_bed_speaker bed[WINE_SA_MAX_BED];
    UINT32 bed_count;
    UINT32 lfe_chan;        /* ~0 if the output has no LFE channel */

    struct list objects;
};

struct SpatialAudioImpl {
    ISpatialAudioClient ISpatialAudioClient_iface;
    IAudioFormatEnumerator IAudioFormatEnumerator_iface;
    IMMDevice *mmdev;
    LONG ref;
    WAVEFORMATEXTENSIBLE object_fmtex;
};

static inline SpatialAudioObjectImpl *impl_from_ISpatialAudioObject(ISpatialAudioObject *iface)
{
    return CONTAINING_RECORD(iface, SpatialAudioObjectImpl, ISpatialAudioObject_iface);
}

static inline SpatialAudioStreamImpl *impl_from_ISpatialAudioObjectRenderStream(ISpatialAudioObjectRenderStream *iface)
{
    return CONTAINING_RECORD(iface, SpatialAudioStreamImpl, ISpatialAudioObjectRenderStream_iface);
}

static inline SpatialAudioImpl *impl_from_ISpatialAudioClient(ISpatialAudioClient *iface)
{
    return CONTAINING_RECORD(iface, SpatialAudioImpl, ISpatialAudioClient_iface);
}

static inline SpatialAudioImpl *impl_from_IAudioFormatEnumerator(IAudioFormatEnumerator *iface)
{
    return CONTAINING_RECORD(iface, SpatialAudioImpl, IAudioFormatEnumerator_iface);
}

static HRESULT WINAPI SAO_QueryInterface(ISpatialAudioObject *iface,
        REFIID riid, void **ppv)
{
    SpatialAudioObjectImpl *This = impl_from_ISpatialAudioObject(iface);

    TRACE("(%p)->(%s,%p)\n", This, debugstr_guid(riid), ppv);

    if (!ppv)
        return E_POINTER;

    *ppv = NULL;

    if (IsEqualIID(riid, &IID_IUnknown) ||
            IsEqualIID(riid, &IID_ISpatialAudioObjectBase) ||
            IsEqualIID(riid, &IID_ISpatialAudioObject)) {
        *ppv = &This->ISpatialAudioObject_iface;
    }
    else
        return E_NOINTERFACE;

    IUnknown_AddRef((IUnknown *)*ppv);

    return S_OK;
}

static ULONG WINAPI SAO_AddRef(ISpatialAudioObject *iface)
{
    SpatialAudioObjectImpl *This = impl_from_ISpatialAudioObject(iface);
    ULONG ref = InterlockedIncrement(&This->ref);
    TRACE("(%p) new ref %lu\n", This, ref);
    return ref;
}

static ULONG WINAPI SAO_Release(ISpatialAudioObject *iface)
{
    SpatialAudioObjectImpl *This = impl_from_ISpatialAudioObject(iface);
    ULONG ref = InterlockedDecrement(&This->ref);
    TRACE("(%p) new ref %lu\n", This, ref);
    if(!ref){
        EnterCriticalSection(&This->sa_stream->lock);
        list_remove(&This->entry);
        if(This->type == AudioObjectType_Dynamic && !This->inactive)
            This->sa_stream->dyn_active--;
        LeaveCriticalSection(&This->sa_stream->lock);

        ISpatialAudioObjectRenderStream_Release(&This->sa_stream->ISpatialAudioObjectRenderStream_iface);
        free(This->buf);
        free(This);
    }
    return ref;
}

static HRESULT WINAPI SAO_GetBuffer(ISpatialAudioObject *iface,
        BYTE **buffer, UINT32 *bytes)
{
    SpatialAudioObjectImpl *This = impl_from_ISpatialAudioObject(iface);

    TRACE("(%p)->(%p, %p)\n", This, buffer, bytes);

    EnterCriticalSection(&This->sa_stream->lock);

    if(This->sa_stream->update_frames == ~0){
        LeaveCriticalSection(&This->sa_stream->lock);
        return SPTLAUDCLNT_E_OUT_OF_ORDER;
    }

    *buffer = (BYTE *)This->buf;
    *bytes = This->sa_stream->update_frames *
        This->sa_stream->sa_client->object_fmtex.Format.nBlockAlign;

    LeaveCriticalSection(&This->sa_stream->lock);

    return S_OK;
}

static HRESULT WINAPI SAO_SetEndOfStream(ISpatialAudioObject *iface, UINT32 frames)
{
    SpatialAudioObjectImpl *This = impl_from_ISpatialAudioObject(iface);
    TRACE("(%p)->(%u)\n", This, frames);
    EnterCriticalSection(&This->sa_stream->lock);
    if(This->sa_stream->update_frames == ~0){
        LeaveCriticalSection(&This->sa_stream->lock);
        return SPTLAUDCLNT_E_OUT_OF_ORDER;
    }
    This->ending = TRUE;
    LeaveCriticalSection(&This->sa_stream->lock);
    return S_OK;
}

static HRESULT WINAPI SAO_IsActive(ISpatialAudioObject *iface, BOOL *active)
{
    SpatialAudioObjectImpl *This = impl_from_ISpatialAudioObject(iface);
    TRACE("(%p)->(%p)\n", This, active);
    *active = !This->inactive;
    return S_OK;
}

static HRESULT WINAPI SAO_GetAudioObjectType(ISpatialAudioObject *iface,
        AudioObjectType *type)
{
    SpatialAudioObjectImpl *This = impl_from_ISpatialAudioObject(iface);

    TRACE("(%p)->(%p)\n", This, type);

    *type = This->type;

    return S_OK;
}

static HRESULT WINAPI SAO_SetPosition(ISpatialAudioObject *iface, float x,
        float y, float z)
{
    SpatialAudioObjectImpl *This = impl_from_ISpatialAudioObject(iface);
    TRACE("(%p)->(%f, %f, %f)\n", This, x, y, z);
    if(This->type != AudioObjectType_Dynamic)
        return SPTLAUDCLNT_E_PROPERTY_NOT_SUPPORTED;
    EnterCriticalSection(&This->sa_stream->lock);
    if(This->sa_stream->update_frames == ~0){
        LeaveCriticalSection(&This->sa_stream->lock);
        return SPTLAUDCLNT_E_OUT_OF_ORDER;
    }
    This->x = x; This->y = y; This->z = z;
    LeaveCriticalSection(&This->sa_stream->lock);
    return S_OK;
}

static HRESULT WINAPI SAO_SetVolume(ISpatialAudioObject *iface, float vol)
{
    SpatialAudioObjectImpl *This = impl_from_ISpatialAudioObject(iface);
    TRACE("(%p)->(%f)\n", This, vol);
    EnterCriticalSection(&This->sa_stream->lock);
    if(This->sa_stream->update_frames == ~0){
        LeaveCriticalSection(&This->sa_stream->lock);
        return SPTLAUDCLNT_E_OUT_OF_ORDER;
    }
    This->vol = vol;
    LeaveCriticalSection(&This->sa_stream->lock);
    return S_OK;
}

static ISpatialAudioObjectVtbl ISpatialAudioObject_vtbl = {
    SAO_QueryInterface,
    SAO_AddRef,
    SAO_Release,
    SAO_GetBuffer,
    SAO_SetEndOfStream,
    SAO_IsActive,
    SAO_GetAudioObjectType,
    SAO_SetPosition,
    SAO_SetVolume,
};

static HRESULT WINAPI SAORS_QueryInterface(ISpatialAudioObjectRenderStream *iface,
        REFIID riid, void **ppv)
{
    SpatialAudioStreamImpl *This = impl_from_ISpatialAudioObjectRenderStream(iface);

    TRACE("(%p)->(%s,%p)\n", This, debugstr_guid(riid), ppv);

    if (!ppv)
        return E_POINTER;

    *ppv = NULL;

    if (IsEqualIID(riid, &IID_IUnknown) ||
            IsEqualIID(riid, &IID_ISpatialAudioObjectRenderStreamBase) ||
            IsEqualIID(riid, &IID_ISpatialAudioObjectRenderStream)) {
        *ppv = &This->ISpatialAudioObjectRenderStream_iface;
    }
    else
        return E_NOINTERFACE;

    IUnknown_AddRef((IUnknown *)*ppv);

    return S_OK;
}

static ULONG WINAPI SAORS_AddRef(ISpatialAudioObjectRenderStream *iface)
{
    SpatialAudioStreamImpl *This = impl_from_ISpatialAudioObjectRenderStream(iface);
    ULONG ref = InterlockedIncrement(&This->ref);
    TRACE("(%p) new ref %lu\n", This, ref);
    return ref;
}

static ULONG WINAPI SAORS_Release(ISpatialAudioObjectRenderStream *iface)
{
    SpatialAudioStreamImpl *This = impl_from_ISpatialAudioObjectRenderStream(iface);
    ULONG ref = InterlockedDecrement(&This->ref);
    TRACE("(%p) new ref %lu\n", This, ref);
    if(!ref){
        IAudioClient_Stop(This->client);
        if(This->update_frames != ~0 && This->update_frames > 0)
            IAudioRenderClient_ReleaseBuffer(This->render, This->update_frames, 0);
        IAudioRenderClient_Release(This->render);
        IAudioClient_Release(This->client);
        if(This->params.NotifyObject)
            ISpatialAudioObjectRenderStreamNotify_Release(This->params.NotifyObject);
        free((void*)This->params.ObjectFormat);
        CloseHandle(This->params.EventHandle);
        DeleteCriticalSection(&This->lock);
        ISpatialAudioClient_Release(&This->sa_client->ISpatialAudioClient_iface);
        free(This);
    }
    return ref;
}

static HRESULT WINAPI SAORS_GetAvailableDynamicObjectCount(
        ISpatialAudioObjectRenderStream *iface, UINT32 *count)
{
    SpatialAudioStreamImpl *This = impl_from_ISpatialAudioObjectRenderStream(iface);
    TRACE("(%p)->(%p)\n", This, count);

    EnterCriticalSection(&This->lock);
    *count = This->dyn_max - This->dyn_active;
    LeaveCriticalSection(&This->lock);
    return S_OK;
}

static HRESULT WINAPI SAORS_GetService(ISpatialAudioObjectRenderStream *iface,
        REFIID riid, void **service)
{
    SpatialAudioStreamImpl *This = impl_from_ISpatialAudioObjectRenderStream(iface);
    FIXME("(%p)->(%s, %p)\n", This, debugstr_guid(riid), service);
    return E_NOTIMPL;
}

static HRESULT WINAPI SAORS_Start(ISpatialAudioObjectRenderStream *iface)
{
    SpatialAudioStreamImpl *This = impl_from_ISpatialAudioObjectRenderStream(iface);
    HRESULT hr;

    TRACE("(%p)->()\n", This);

    hr = IAudioClient_Start(This->client);
    if(FAILED(hr)){
        WARN("IAudioClient::Start failed: %08lx\n", hr);
        return hr;
    }

    return S_OK;
}

static HRESULT WINAPI SAORS_Stop(ISpatialAudioObjectRenderStream *iface)
{
    SpatialAudioStreamImpl *This = impl_from_ISpatialAudioObjectRenderStream(iface);
    HRESULT hr;

    TRACE("(%p)->()\n", This);

    hr = IAudioClient_Stop(This->client);
    if(FAILED(hr)){
        WARN("IAudioClient::Stop failed: %08lx\n", hr);
        return hr;
    }

    return S_OK;
}

static HRESULT WINAPI SAORS_Reset(ISpatialAudioObjectRenderStream *iface)
{
    SpatialAudioStreamImpl *This = impl_from_ISpatialAudioObjectRenderStream(iface);
    HRESULT hr;

    TRACE("(%p)->()\n", This);

    hr = IAudioClient_Reset(This->client);
    if (hr == AUDCLNT_E_NOT_STOPPED)
        return SPTLAUDCLNT_E_STREAM_NOT_STOPPED;
    return hr;
}

static HRESULT WINAPI SAORS_BeginUpdatingAudioObjects(ISpatialAudioObjectRenderStream *iface,
        UINT32 *dyn_count, UINT32 *frames)
{
    static BOOL fixme_once = FALSE;
    SpatialAudioStreamImpl *This = impl_from_ISpatialAudioObjectRenderStream(iface);
    SpatialAudioObjectImpl *object;
    HRESULT hr;

    TRACE("(%p)->(%p, %p)\n", This, dyn_count, frames);

    EnterCriticalSection(&This->lock);

    if(This->update_frames != ~0){
        LeaveCriticalSection(&This->lock);
        return SPTLAUDCLNT_E_OUT_OF_ORDER;
    }

    This->update_frames = This->period_frames;

    if(This->update_frames > 0){
        hr = IAudioRenderClient_GetBuffer(This->render, This->update_frames, (BYTE **)&This->buf);
        if(FAILED(hr)){
            WARN("GetBuffer failed: %08lx\n", hr);
            This->update_frames = ~0;
            LeaveCriticalSection(&This->lock);
            return hr;
        }

        LIST_FOR_EACH_ENTRY(object, &This->objects, SpatialAudioObjectImpl, entry){
            memset(object->buf, 0, This->update_frames * This->sa_client->object_fmtex.Format.nBlockAlign);
        }
    }else if (!fixme_once){
        fixme_once = TRUE;
        FIXME("Zero frame update.\n");
    }

    *dyn_count = This->dyn_max - This->dyn_active;
    *frames = This->update_frames;

    LeaveCriticalSection(&This->lock);

    return S_OK;
}

/* gain of each bed speaker for a source direction: constant-power pan between the two speakers of
 * the nearest ring (ground or top) that bracket the source azimuth */
static void direction_gains(SpatialAudioStreamImpl *stream, float az, float el, float vol, float *gains)
{
    UINT32 i, best_l = ~0, best_r = ~0;
    float dl = 1e9f, dr = 1e9f, d, frac;
    BOOL want_top;

    for(i = 0; i < stream->bed_count; ++i) gains[i] = 0.f;
    if(!stream->bed_count) return;

    want_top = el > 30.f;

    for(;;){
        /* nearest speaker to the left of (or at) the azimuth, and nearest to the right, on the ring */
        for(i = 0; i < stream->bed_count; ++i){
            if(stream->bed[i].top != want_top) continue;
            d = stream->bed[i].az - az;
            while(d > 180.f) d -= 360.f;
            while(d < -180.f) d += 360.f;
            if(d <= 0.f && -d < dl){ dl = -d; best_l = i; }
            if(d >= 0.f && d < dr){ dr = d; best_r = i; }
        }
        if(best_l != ~0 || best_r != ~0) break;
        if(!want_top) return;                            /* no ground ring at all */
        want_top = FALSE;                                /* no top ring: use the ground ring */
    }
    if(best_l == ~0) best_l = best_r;
    if(best_r == ~0) best_r = best_l;

    if(best_l == best_r){
        gains[best_l] = vol;
    }else{
        /* constant-power pan across the gap between the two bracketing speakers */
        frac = (dl + dr) > 0.f ? dl / (dl + dr) : 0.5f;
        gains[best_l] = cosf(frac * SA_PI / 2.f) * vol;
        gains[best_r] = sinf(frac * SA_PI / 2.f) * vol;
        /* a wide gap (a stereo-only bed with a rear source) gets a touch of attenuation */
        if(dl + dr > 120.f){
            gains[best_l] *= 0.8f;
            gains[best_r] *= 0.8f;
        }
    }
}

static void mix_object(SpatialAudioStreamImpl *stream, SpatialAudioObjectImpl *object)
{
    float gains[WINE_SA_MAX_BED];
    float *in = object->buf, *out = stream->buf;
    UINT32 i, s, nch = stream->stream_fmtex.Format.nChannels;
    float az, el;

    if(object->inactive) return;

    if(object->type == AudioObjectType_LowFrequency){
        /* to the output LFE if there is one, otherwise quietly into the ground speakers */
        if(stream->lfe_chan != ~0){
            out = stream->buf + stream->lfe_chan;
            for(i = 0; i < stream->update_frames; ++i){ *out += in[i] * object->vol; out += nch; }
            return;
        }
        for(s = 0; s < stream->bed_count; ++s) gains[s] = stream->bed[s].top ? 0.f : 0.5f * object->vol;
    }else if(object->type == AudioObjectType_Dynamic){
        float x = object->x, y = object->y, z = object->z, horiz = sqrtf(x * x + z * z);
        if(horiz < 1e-4f && fabsf(y) < 1e-4f){
            /* at the listener: everywhere at once */
            float sum = 0.f;
            for(s = 0; s < stream->bed_count; ++s){ gains[s] = stream->bed[s].top ? 0.f : 1.f; sum += gains[s] * gains[s]; }
            if(sum > 0.f) for(s = 0; s < stream->bed_count; ++s) gains[s] *= object->vol / sqrtf(sum);
        }else{
            az = atan2f(x, -z) * (180.f / SA_PI);           /* 0 front, +90 right, +-180 behind */
            el = atan2f(y, horiz) * (180.f / SA_PI);
            direction_gains(stream, az, el, object->vol, gains);
        }
    }else{
        if(!static_object_direction(object->type, &az, &el)) return;
        direction_gains(stream, az, el, object->vol, gains);
    }

    for(s = 0; s < stream->bed_count; ++s){
        float g = gains[s];
        if(g == 0.f) continue;
        out = stream->buf + stream->bed[s].chan;
        for(i = 0; i < stream->update_frames; ++i){
            *out += in[i] * g;
            out += nch;
        }
    }
}

static HRESULT WINAPI SAORS_EndUpdatingAudioObjects(ISpatialAudioObjectRenderStream *iface)
{
    SpatialAudioStreamImpl *This = impl_from_ISpatialAudioObjectRenderStream(iface);
    SpatialAudioObjectImpl *object;
    HRESULT hr;

    TRACE("(%p)->()\n", This);

    EnterCriticalSection(&This->lock);

    if(This->update_frames == ~0){
        LeaveCriticalSection(&This->lock);
        return SPTLAUDCLNT_E_OUT_OF_ORDER;
    }

    if(This->update_frames > 0){
        LIST_FOR_EACH_ENTRY(object, &This->objects, SpatialAudioObjectImpl, entry){
            mix_object(This, object);
            if(object->ending && !object->inactive){
                object->inactive = TRUE;
                object->ending = FALSE;
                if(object->type == AudioObjectType_Dynamic) This->dyn_active--;
            }
        }

        hr = IAudioRenderClient_ReleaseBuffer(This->render, This->update_frames, 0);
        if(FAILED(hr))
            WARN("ReleaseBuffer failed: %08lx\n", hr);
    }

    This->update_frames = ~0;

    LeaveCriticalSection(&This->lock);

    return S_OK;
}

static HRESULT WINAPI SAORS_ActivateSpatialAudioObject(ISpatialAudioObjectRenderStream *iface,
        AudioObjectType type, ISpatialAudioObject **object)
{
    SpatialAudioStreamImpl *This = impl_from_ISpatialAudioObjectRenderStream(iface);
    SpatialAudioObjectImpl *obj;

    TRACE("(%p)->(0x%x, %p)\n", This, type, object);

    if(type == AudioObjectType_Dynamic){
        EnterCriticalSection(&This->lock);
        if(This->dyn_active >= This->dyn_max){
            LeaveCriticalSection(&This->lock);
            return SPTLAUDCLNT_E_NO_MORE_OBJECTS;
        }
        obj = calloc(1, sizeof(*obj));
        obj->ISpatialAudioObject_iface.lpVtbl = &ISpatialAudioObject_vtbl;
        obj->ref = 1;
        obj->type = type;
        obj->static_idx = ~0;
        obj->vol = 1.f;
        obj->z = -1.f;          /* straight ahead until the app positions it */
        obj->sa_stream = This;
        SAORS_AddRef(&This->ISpatialAudioObjectRenderStream_iface);
        obj->buf = calloc(This->period_frames, This->sa_client->object_fmtex.Format.nBlockAlign);
        list_add_tail(&This->objects, &obj->entry);
        This->dyn_active++;
        LeaveCriticalSection(&This->lock);
        *object = &obj->ISpatialAudioObject_iface;
        return S_OK;
    }

    if(type & ~This->params.StaticObjectTypeMask)
        return SPTLAUDCLNT_E_STATIC_OBJECT_NOT_AVAILABLE;

    LIST_FOR_EACH_ENTRY(obj, &This->objects, SpatialAudioObjectImpl, entry){
        if(obj->static_idx == AudioObjectType_to_index(type))
            return SPTLAUDCLNT_E_OBJECT_ALREADY_ACTIVE;
    }

    obj = calloc(1, sizeof(*obj));
    obj->ISpatialAudioObject_iface.lpVtbl = &ISpatialAudioObject_vtbl;
    obj->ref = 1;
    obj->type = type;
    if(type == AudioObjectType_None){
        FIXME("AudioObjectType_None not implemented yet!\n");
        obj->static_idx = ~0;
    }else{
        obj->static_idx = AudioObjectType_to_index(type);
    }
    obj->z = -1.f;

    obj->vol = 1.f;
    obj->sa_stream = This;
    SAORS_AddRef(&This->ISpatialAudioObjectRenderStream_iface);

    obj->buf = calloc(This->period_frames, This->sa_client->object_fmtex.Format.nBlockAlign);

    EnterCriticalSection(&This->lock);

    list_add_tail(&This->objects, &obj->entry);

    LeaveCriticalSection(&This->lock);

    *object = &obj->ISpatialAudioObject_iface;

    return S_OK;
}

static ISpatialAudioObjectRenderStreamVtbl ISpatialAudioObjectRenderStream_vtbl = {
    SAORS_QueryInterface,
    SAORS_AddRef,
    SAORS_Release,
    SAORS_GetAvailableDynamicObjectCount,
    SAORS_GetService,
    SAORS_Start,
    SAORS_Stop,
    SAORS_Reset,
    SAORS_BeginUpdatingAudioObjects,
    SAORS_EndUpdatingAudioObjects,
    SAORS_ActivateSpatialAudioObject,
};

static HRESULT WINAPI SAC_QueryInterface(ISpatialAudioClient *iface, REFIID riid, void **ppv)
{
    SpatialAudioImpl *This = impl_from_ISpatialAudioClient(iface);

    TRACE("(%p)->(%s,%p)\n", This, debugstr_guid(riid), ppv);

    if (!ppv)
        return E_POINTER;

    *ppv = NULL;

    if (IsEqualIID(riid, &IID_IUnknown) ||
            IsEqualIID(riid, &IID_ISpatialAudioClient)) {
        *ppv = &This->ISpatialAudioClient_iface;
    }
    else
        return E_NOINTERFACE;

    IUnknown_AddRef((IUnknown *)*ppv);

    return S_OK;
}

static ULONG WINAPI SAC_AddRef(ISpatialAudioClient *iface)
{
    SpatialAudioImpl *This = impl_from_ISpatialAudioClient(iface);
    ULONG ref = InterlockedIncrement(&This->ref);
    TRACE("(%p) new ref %lu\n", This, ref);
    return ref;
}

static ULONG WINAPI SAC_Release(ISpatialAudioClient *iface)
{
    SpatialAudioImpl *This = impl_from_ISpatialAudioClient(iface);
    ULONG ref = InterlockedDecrement(&This->ref);
    TRACE("(%p) new ref %lu\n", This, ref);
    if (!ref) {
        IMMDevice_Release(This->mmdev);
        free(This);
    }
    return ref;
}

static HRESULT WINAPI SAC_GetStaticObjectPosition(ISpatialAudioClient *iface,
        AudioObjectType type, float *x, float *y, float *z)
{
    SpatialAudioImpl *This = impl_from_ISpatialAudioClient(iface);
    float az, el;

    TRACE("(%p)->(0x%x, %p, %p, %p)\n", This, type, x, y, z);

    if(!static_object_direction(type, &az, &el))
        return E_INVALIDARG;
    az *= SA_PI / 180.f;
    el *= SA_PI / 180.f;
    *x = sinf(az) * cosf(el);
    *y = sinf(el);
    *z = -cosf(az) * cosf(el);
    return S_OK;
}

static HRESULT WINAPI SAC_GetNativeStaticObjectTypeMask(ISpatialAudioClient *iface,
        AudioObjectType *mask)
{
    SpatialAudioImpl *This = impl_from_ISpatialAudioClient(iface);
    TRACE("(%p)->(%p)\n", This, mask);
    /* the 7.1.4 bed that Windows Sonic for Headphones offers */
    *mask = AudioObjectType_FrontLeft | AudioObjectType_FrontRight | AudioObjectType_FrontCenter |
            AudioObjectType_LowFrequency | AudioObjectType_SideLeft | AudioObjectType_SideRight |
            AudioObjectType_BackLeft | AudioObjectType_BackRight | AudioObjectType_TopFrontLeft |
            AudioObjectType_TopFrontRight | AudioObjectType_TopBackLeft | AudioObjectType_TopBackRight;
    return S_OK;
}

static HRESULT WINAPI SAC_GetMaxDynamicObjectCount(ISpatialAudioClient *iface,
        UINT32 *value)
{
    SpatialAudioImpl *This = impl_from_ISpatialAudioClient(iface);
    TRACE("(%p)->(%p)\n", This, value);

    *value = WINE_SA_MAX_DYNAMIC;

    return S_OK;
}

static HRESULT WINAPI SAC_GetSupportedAudioObjectFormatEnumerator(
        ISpatialAudioClient *iface, IAudioFormatEnumerator **enumerator)
{
    SpatialAudioImpl *This = impl_from_ISpatialAudioClient(iface);

    TRACE("(%p)->(%p)\n", This, enumerator);

    *enumerator = &This->IAudioFormatEnumerator_iface;
    SAC_AddRef(iface);

    return S_OK;
}

static HRESULT WINAPI SAC_GetMaxFrameCount(ISpatialAudioClient *iface,
        const WAVEFORMATEX *format, UINT32 *count)
{
    SpatialAudioImpl *This = impl_from_ISpatialAudioClient(iface);
    REFERENCE_TIME period = 100000;
    IAudioClient *client;

    TRACE("(%p)->(%p, %p)\n", This, format, count);

    if(SUCCEEDED(IMMDevice_Activate(This->mmdev, &IID_IAudioClient, CLSCTX_INPROC_SERVER, NULL, (void**)&client))){
        if(FAILED(IAudioClient_GetDevicePeriod(client, &period, NULL)))
            period = 100000;
        IAudioClient_Release(client);
    }

    *count = MulDiv(period, format->nSamplesPerSec, 10000000);

    return S_OK;
}

static HRESULT WINAPI SAC_IsAudioObjectFormatSupported(ISpatialAudioClient *iface,
        const WAVEFORMATEX *format)
{
    SpatialAudioImpl *sac = impl_from_ISpatialAudioClient(iface);

    TRACE("sac %p, format %s.\n", sac, debugstr_fmtex(format));

    if (!format)
        return E_POINTER;

    if (!formats_equal(&sac->object_fmtex.Format, format))
    {
        FIXME("Reporting format %s as unsupported.\n", debugstr_fmtex(format));
        return E_INVALIDARG;
    }

    return S_OK;
}

static HRESULT WINAPI SAC_IsSpatialAudioStreamAvailable(ISpatialAudioClient *iface,
        REFIID stream_uuid, const PROPVARIANT *info)
{
    SpatialAudioImpl *This = impl_from_ISpatialAudioClient(iface);
    FIXME("(%p)->(%s, %p)\n", This, debugstr_guid(stream_uuid), info);
    return E_NOTIMPL;
}

static WAVEFORMATEX *clone_fmtex(const WAVEFORMATEX *src)
{
    WAVEFORMATEX *r = malloc(sizeof(WAVEFORMATEX) + src->cbSize);
    memcpy(r, src, sizeof(WAVEFORMATEX) + src->cbSize);
    return r;
}

static BOOL static_object_direction(AudioObjectType type, float *az, float *el)
{
    switch(type){
    case AudioObjectType_FrontLeft:     *az = -30.f;  *el = 0.f;  return TRUE;
    case AudioObjectType_FrontRight:    *az = 30.f;   *el = 0.f;  return TRUE;
    case AudioObjectType_FrontCenter:   *az = 0.f;    *el = 0.f;  return TRUE;
    case AudioObjectType_SideLeft:      *az = -90.f;  *el = 0.f;  return TRUE;
    case AudioObjectType_SideRight:     *az = 90.f;   *el = 0.f;  return TRUE;
    case AudioObjectType_BackLeft:      *az = -135.f; *el = 0.f;  return TRUE;
    case AudioObjectType_BackRight:     *az = 135.f;  *el = 0.f;  return TRUE;
    case AudioObjectType_BackCenter:    *az = 180.f;  *el = 0.f;  return TRUE;
    case AudioObjectType_TopFrontLeft:  *az = -45.f;  *el = 45.f; return TRUE;
    case AudioObjectType_TopFrontRight: *az = 45.f;   *el = 45.f; return TRUE;
    case AudioObjectType_TopBackLeft:   *az = -135.f; *el = 45.f; return TRUE;
    case AudioObjectType_TopBackRight:  *az = 135.f;  *el = 45.f; return TRUE;
    case AudioObjectType_LowFrequency:  *az = 0.f;    *el = 0.f;  return TRUE;
    case AudioObjectType_BottomFrontLeft:  *az = -45.f;  *el = -45.f; return TRUE;
    case AudioObjectType_BottomFrontRight: *az = 45.f;   *el = -45.f; return TRUE;
    case AudioObjectType_BottomBackLeft:   *az = -135.f; *el = -45.f; return TRUE;
    case AudioObjectType_BottomBackRight:  *az = 135.f;  *el = -45.f; return TRUE;
    default: return FALSE;
    }
}

/* direction of an output speaker given its KSAUDIO SPEAKER_* bit */
static BOOL speaker_direction(DWORD bit, float *az, float *el)
{
    switch(bit){
    case SPEAKER_FRONT_LEFT:            *az = -30.f;  *el = 0.f;  return TRUE;
    case SPEAKER_FRONT_RIGHT:           *az = 30.f;   *el = 0.f;  return TRUE;
    case SPEAKER_FRONT_CENTER:          *az = 0.f;    *el = 0.f;  return TRUE;
    case SPEAKER_BACK_LEFT:             *az = -135.f; *el = 0.f;  return TRUE;
    case SPEAKER_BACK_RIGHT:            *az = 135.f;  *el = 0.f;  return TRUE;
    case SPEAKER_FRONT_LEFT_OF_CENTER:  *az = -15.f;  *el = 0.f;  return TRUE;
    case SPEAKER_FRONT_RIGHT_OF_CENTER: *az = 15.f;   *el = 0.f;  return TRUE;
    case SPEAKER_BACK_CENTER:           *az = 180.f;  *el = 0.f;  return TRUE;
    case SPEAKER_SIDE_LEFT:             *az = -90.f;  *el = 0.f;  return TRUE;
    case SPEAKER_SIDE_RIGHT:            *az = 90.f;   *el = 0.f;  return TRUE;
    case SPEAKER_TOP_CENTER:            *az = 0.f;    *el = 90.f; return TRUE;
    case SPEAKER_TOP_FRONT_LEFT:        *az = -45.f;  *el = 45.f; return TRUE;
    case SPEAKER_TOP_FRONT_CENTER:      *az = 0.f;    *el = 45.f; return TRUE;
    case SPEAKER_TOP_FRONT_RIGHT:       *az = 45.f;   *el = 45.f; return TRUE;
    case SPEAKER_TOP_BACK_LEFT:         *az = -135.f; *el = 45.f; return TRUE;
    case SPEAKER_TOP_BACK_CENTER:       *az = 180.f;  *el = 45.f; return TRUE;
    case SPEAKER_TOP_BACK_RIGHT:        *az = 135.f;  *el = 45.f; return TRUE;
    default: return FALSE;
    }
}

static HRESULT activate_stream(SpatialAudioStreamImpl *stream)
{
    WAVEFORMATEXTENSIBLE *object_fmtex = (WAVEFORMATEXTENSIBLE *)stream->params.ObjectFormat;
    HRESULT hr;
    REFERENCE_TIME period;
    WAVEFORMATEX *mix;

    if(!(object_fmtex->Format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                (object_fmtex->Format.wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                 IsEqualGUID(&object_fmtex->SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)))){
        FIXME("Only float formats are supported for now\n");
        return E_INVALIDARG;
    }

    hr = IMMDevice_Activate(stream->sa_client->mmdev, &IID_IAudioClient,
            CLSCTX_INPROC_SERVER, NULL, (void**)&stream->client);
    if(FAILED(hr)){
        WARN("Activate failed: %08lx\n", hr);
        return hr;
    }

    hr = IAudioClient_GetDevicePeriod(stream->client, &period, NULL);
    if(FAILED(hr)){
        WARN("GetDevicePeriod failed: %08lx\n", hr);
        IAudioClient_Release(stream->client);
        return hr;
    }

    stream->dyn_max = min(stream->params.MaxDynamicObjectCount, WINE_SA_MAX_DYNAMIC);

    /* Render everything ourselves into the device's own channel layout (WineForge-dev, Luke David 1,
     * 21 Oct 2026). Upstream opened a stream with one channel per static bed object and left the
     * fold-down to the audio driver; on macOS CoreAudio dropped everything but the first two
     * channels of that stream, which is why dialogue (front centre) went missing. */
    hr = IAudioClient_GetMixFormat(stream->client, &mix);
    if(FAILED(hr)){
        WARN("GetMixFormat failed: %08lx\n", hr);
        IAudioClient_Release(stream->client);
        return hr;
    }
    if(mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE){
        memcpy(&stream->stream_fmtex, mix, sizeof(WAVEFORMATEXTENSIBLE));
    }else{
        memcpy(&stream->stream_fmtex.Format, mix, sizeof(WAVEFORMATEX));
        stream->stream_fmtex.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        stream->stream_fmtex.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        stream->stream_fmtex.Samples.wValidBitsPerSample = mix->wBitsPerSample;
        stream->stream_fmtex.dwChannelMask = mix->nChannels == 1 ? SPEAKER_FRONT_CENTER : KSAUDIO_SPEAKER_STEREO;
        stream->stream_fmtex.SubFormat = mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
    }
    CoTaskMemFree(mix);
    if(!IsEqualGUID(&stream->stream_fmtex.SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) ||
            stream->stream_fmtex.Format.wBitsPerSample != 32){
        /* ask for float at the mix rate; mmdevapi converts (AUTOCONVERTPCM) */
        stream->stream_fmtex.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        stream->stream_fmtex.Format.wBitsPerSample = 32;
        stream->stream_fmtex.Samples.wValidBitsPerSample = 32;
        stream->stream_fmtex.Format.nBlockAlign = (stream->stream_fmtex.Format.nChannels * 32) / 8;
        stream->stream_fmtex.Format.nAvgBytesPerSec = stream->stream_fmtex.Format.nSamplesPerSec * stream->stream_fmtex.Format.nBlockAlign;
    }
    if(!stream->stream_fmtex.dwChannelMask)
        stream->stream_fmtex.dwChannelMask = stream->stream_fmtex.Format.nChannels == 1 ? SPEAKER_FRONT_CENTER : KSAUDIO_SPEAKER_STEREO;

    /* speaker table for the panner, in stream channel order (LFE kept aside) */
    stream->bed_count = 0;
    stream->lfe_chan = ~0;
    {
        UINT32 chan = 0;
        DWORD bit;
        for(bit = 1; bit && chan < stream->stream_fmtex.Format.nChannels; bit <<= 1){
            float az, el;
            if(!(stream->stream_fmtex.dwChannelMask & bit)) continue;
            if(bit == SPEAKER_LOW_FREQUENCY){
                stream->lfe_chan = chan++;
                continue;
            }
            if(speaker_direction(bit, &az, &el) && stream->bed_count < WINE_SA_MAX_BED){
                stream->bed[stream->bed_count].chan = chan;
                stream->bed[stream->bed_count].az = az;
                stream->bed[stream->bed_count].el = el;
                stream->bed[stream->bed_count].top = el > 30.f;
                stream->bed_count++;
            }
            chan++;
        }
        /* a mask with fewer bits than channels (or unknown bits): treat the rest as a stereo pair */
        if(!stream->bed_count && stream->stream_fmtex.Format.nChannels >= 2){
            stream->bed[0].chan = 0; stream->bed[0].az = -30.f; stream->bed[0].el = 0.f; stream->bed[0].top = FALSE;
            stream->bed[1].chan = 1; stream->bed[1].az = 30.f;  stream->bed[1].el = 0.f; stream->bed[1].top = FALSE;
            stream->bed_count = 2;
        }else if(!stream->bed_count){
            stream->bed[0].chan = 0; stream->bed[0].az = 0.f; stream->bed[0].el = 0.f; stream->bed[0].top = FALSE;
            stream->bed_count = 1;
        }
    }
    TRACE("output: %u channels, mask 0x%lx, %u panner speakers, lfe %d, %u dynamic objects\n",
          stream->stream_fmtex.Format.nChannels, stream->stream_fmtex.dwChannelMask, stream->bed_count,
          (int)stream->lfe_chan, stream->dyn_max);

    hr = IAudioClient_Initialize(stream->client, AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
            period * 3, 0, &stream->stream_fmtex.Format, NULL);
    if(FAILED(hr)){
        WARN("Initialize failed: %08lx\n", hr);
        IAudioClient_Release(stream->client);
        return hr;
    }

    hr = IAudioClient_SetEventHandle(stream->client, stream->params.EventHandle);
    if(FAILED(hr)){
        WARN("SetEventHandle failed: %08lx\n", hr);
        IAudioClient_Release(stream->client);
        return hr;
    }

    hr = IAudioClient_GetService(stream->client, &IID_IAudioRenderClient, (void**)&stream->render);
    if(FAILED(hr)){
        WARN("GetService(AudioRenderClient) failed: %08lx\n", hr);
        IAudioClient_Release(stream->client);
        return hr;
    }

    stream->period_frames = MulDiv(period, stream->stream_fmtex.Format.nSamplesPerSec, 10000000);

    return S_OK;
}

static HRESULT WINAPI SAC_ActivateSpatialAudioStream(ISpatialAudioClient *iface,
        const PROPVARIANT *prop, REFIID riid, void **stream)
{
    SpatialAudioImpl *This = impl_from_ISpatialAudioClient(iface);
    SpatialAudioObjectRenderStreamActivationParams *params;
    HRESULT hr;

    TRACE("(%p)->(%s, %p)\n", This, debugstr_guid(riid), stream);

    if(IsEqualIID(riid, &IID_ISpatialAudioObjectRenderStream)){
        SpatialAudioStreamImpl *obj;

        if(prop &&
                (prop->vt != VT_BLOB ||
                 prop->blob.cbSize != sizeof(SpatialAudioObjectRenderStreamActivationParams))){
            WARN("Got invalid params\n");
            *stream = NULL;
            return E_INVALIDARG;
        }

        params = (SpatialAudioObjectRenderStreamActivationParams*) prop->blob.pBlobData;

        if(params->StaticObjectTypeMask & AudioObjectType_Dynamic){
            *stream = NULL;
            return E_INVALIDARG;
        }

        if(params->EventHandle == INVALID_HANDLE_VALUE ||
                params->EventHandle == 0){
            *stream = NULL;
            return E_INVALIDARG;
        }

        if(!(params->ObjectFormat && formats_equal(params->ObjectFormat, &This->object_fmtex.Format))) {
            *stream = NULL;
            return AUDCLNT_E_UNSUPPORTED_FORMAT;
        }

        obj = calloc(1, sizeof(SpatialAudioStreamImpl));

        obj->ISpatialAudioObjectRenderStream_iface.lpVtbl = &ISpatialAudioObjectRenderStream_vtbl;
        obj->ref = 1;
        memcpy(&obj->params, params, sizeof(obj->params));

        obj->update_frames = ~0;

        InitializeCriticalSection(&obj->lock);
        list_init(&obj->objects);

        obj->sa_client = This;
        SAC_AddRef(&This->ISpatialAudioClient_iface);

        obj->params.ObjectFormat = clone_fmtex(obj->params.ObjectFormat);

        DuplicateHandle(GetCurrentProcess(), obj->params.EventHandle,
                GetCurrentProcess(), &obj->params.EventHandle, 0, FALSE,
                DUPLICATE_SAME_ACCESS);

        if(obj->params.NotifyObject)
            ISpatialAudioObjectRenderStreamNotify_AddRef(obj->params.NotifyObject);

        if(TRACE_ON(mmdevapi)){
            TRACE("ObjectFormat: {%s}\n", debugstr_fmtex(obj->params.ObjectFormat));
            TRACE("StaticObjectTypeMask: 0x%x\n", obj->params.StaticObjectTypeMask);
            TRACE("MinDynamicObjectCount: 0x%x\n", obj->params.MinDynamicObjectCount);
            TRACE("MaxDynamicObjectCount: 0x%x\n", obj->params.MaxDynamicObjectCount);
            TRACE("Category: 0x%x\n", obj->params.Category);
            TRACE("EventHandle: %p\n", obj->params.EventHandle);
            TRACE("NotifyObject: %p\n", obj->params.NotifyObject);
        }

        hr = activate_stream(obj);
        if(FAILED(hr)){
            if(obj->params.NotifyObject)
                ISpatialAudioObjectRenderStreamNotify_Release(obj->params.NotifyObject);
            DeleteCriticalSection(&obj->lock);
            free((void*)obj->params.ObjectFormat);
            CloseHandle(obj->params.EventHandle);
            ISpatialAudioClient_Release(&obj->sa_client->ISpatialAudioClient_iface);
            free(obj);
            *stream = NULL;
            return hr;
        }

        *stream = &obj->ISpatialAudioObjectRenderStream_iface;
    }else{
        FIXME("Unsupported audio stream IID: %s\n", debugstr_guid(riid));
        *stream = NULL;
        return E_NOTIMPL;
    }

    return S_OK;
}

static ISpatialAudioClientVtbl ISpatialAudioClient_vtbl = {
    SAC_QueryInterface,
    SAC_AddRef,
    SAC_Release,
    SAC_GetStaticObjectPosition,
    SAC_GetNativeStaticObjectTypeMask,
    SAC_GetMaxDynamicObjectCount,
    SAC_GetSupportedAudioObjectFormatEnumerator,
    SAC_GetMaxFrameCount,
    SAC_IsAudioObjectFormatSupported,
    SAC_IsSpatialAudioStreamAvailable,
    SAC_ActivateSpatialAudioStream,
};

static HRESULT WINAPI SAOFE_QueryInterface(IAudioFormatEnumerator *iface,
        REFIID riid, void **ppvObject)
{
    SpatialAudioImpl *This = impl_from_IAudioFormatEnumerator(iface);
    return SAC_QueryInterface(&This->ISpatialAudioClient_iface, riid, ppvObject);
}

static ULONG WINAPI SAOFE_AddRef(IAudioFormatEnumerator *iface)
{
    SpatialAudioImpl *This = impl_from_IAudioFormatEnumerator(iface);
    return SAC_AddRef(&This->ISpatialAudioClient_iface);
}

static ULONG WINAPI SAOFE_Release(IAudioFormatEnumerator *iface)
{
    SpatialAudioImpl *This = impl_from_IAudioFormatEnumerator(iface);
    return SAC_Release(&This->ISpatialAudioClient_iface);
}

static HRESULT WINAPI SAOFE_GetCount(IAudioFormatEnumerator *iface, UINT32 *count)
{
    SpatialAudioImpl *This = impl_from_IAudioFormatEnumerator(iface);

    TRACE("(%p)->(%p)\n", This, count);

    *count = 1;

    return S_OK;
}

static HRESULT WINAPI SAOFE_GetFormat(IAudioFormatEnumerator *iface,
        UINT32 index, WAVEFORMATEX **format)
{
    SpatialAudioImpl *This = impl_from_IAudioFormatEnumerator(iface);

    TRACE("(%p)->(%u, %p)\n", This, index, format);

    if(index > 0)
        return E_INVALIDARG;

    *format = &This->object_fmtex.Format;

    return S_OK;
}

static IAudioFormatEnumeratorVtbl IAudioFormatEnumerator_vtbl = {
    SAOFE_QueryInterface,
    SAOFE_AddRef,
    SAOFE_Release,
    SAOFE_GetCount,
    SAOFE_GetFormat,
};

HRESULT SpatialAudioClient_Create(IMMDevice *mmdev, ISpatialAudioClient **out)
{
    SpatialAudioImpl *obj;

    obj = calloc(1, sizeof(*obj));

    obj->ref = 1;
    obj->ISpatialAudioClient_iface.lpVtbl = &ISpatialAudioClient_vtbl;
    obj->IAudioFormatEnumerator_iface.lpVtbl = &IAudioFormatEnumerator_vtbl;

    obj->object_fmtex.Format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    obj->object_fmtex.Format.nChannels = 1;
    obj->object_fmtex.Format.nSamplesPerSec = 48000;
    obj->object_fmtex.Format.wBitsPerSample = sizeof(float) * 8;
    obj->object_fmtex.Format.nBlockAlign = (obj->object_fmtex.Format.nChannels * obj->object_fmtex.Format.wBitsPerSample) / 8;
    obj->object_fmtex.Format.nAvgBytesPerSec = obj->object_fmtex.Format.nSamplesPerSec * obj->object_fmtex.Format.nBlockAlign;
    obj->object_fmtex.Format.cbSize = 0;

    obj->mmdev = mmdev;
    IMMDevice_AddRef(mmdev);

    *out = &obj->ISpatialAudioClient_iface;

    return S_OK;
}
