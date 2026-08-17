/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 3 of the License, or (at your option) any later
 * version.
 *
 * mpv is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along
 * with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

// GPLv3, not LGPL like the rest of mpv: this file includes Steinberg's ASIO
// SDK headers, taken under the GPLv3 half of the SDK's dual licence (since
// 2025-10-15; the other half is Steinberg's proprietary licence, which mpv
// cannot use). ao_asio is therefore gated on -Dgpl=true, and an LGPL build
// has no ASIO output. See DOCS/contribute.md and the Copyright file.
//
// Everything the ABI defines — types, structs, enums, sample-type and
// selector constants — comes from the SDK's <asio.h>. What follows is the
// one thing the SDK cannot give a C translation unit: IASIO itself, which
// the SDK declares in iasiodrv.h as a C++ class with virtual methods
// ("interface IASIO : public IUnknown"). Re-expressed here as the COM vtbl
// that class produces, because C has no other way to call through it. The
// slot order is not a design choice: it is what the ABI is.

#ifndef MP_AO_ASIO_H_
#define MP_AO_ASIO_H_

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include <windows.h>
#include <objbase.h>

// Steinberg ASIO SDK (GPLv3 option). asiosys.h must come first: it sets the
// platform macros asio.h keys off.
#include <asiosys.h>
#include <asio.h>

#include "common/msg.h"
#include "osdep/windows_utils.h"
#include "internal.h"
#include "ao.h"

// Sample-type converter signature: writes `n` samples from planar f32
// src into the ASIO output buffer dst, scaled by `vol`.
struct asio_state;
typedef void (*asio_convert_fn)(void *dst, const float *src, int n, float vol);

// --- IASIO as a C vtbl (see the note at the top of this file) ---
//
// Each ASIO driver implements IASIO with a vtbl in the same layout the
// C++ "interface IASIO : public IUnknown" produces on MSVC/MinGW (vtbl
// pointer first, IUnknown methods at slots 0..2, IASIO methods after).

typedef struct IASIO IASIO;

typedef struct IASIOVtbl {
    // IUnknown
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IASIO *, REFIID, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IASIO *);
    ULONG   (STDMETHODCALLTYPE *Release)(IASIO *);
    // IASIO
    ASIOBool (STDMETHODCALLTYPE *init)(IASIO *, void *sysHandle);
    void     (STDMETHODCALLTYPE *getDriverName)(IASIO *, char *name);
    long     (STDMETHODCALLTYPE *getDriverVersion)(IASIO *);
    void     (STDMETHODCALLTYPE *getErrorMessage)(IASIO *, char *string);
    ASIOError(STDMETHODCALLTYPE *start)(IASIO *);
    ASIOError(STDMETHODCALLTYPE *stop)(IASIO *);
    ASIOError(STDMETHODCALLTYPE *getChannels)(IASIO *, long *in, long *out);
    ASIOError(STDMETHODCALLTYPE *getLatencies)(IASIO *, long *in, long *out);
    ASIOError(STDMETHODCALLTYPE *getBufferSize)(IASIO *, long *min, long *max,
                                                long *pref, long *gran);
    ASIOError(STDMETHODCALLTYPE *canSampleRate)(IASIO *, ASIOSampleRate);
    ASIOError(STDMETHODCALLTYPE *getSampleRate)(IASIO *, ASIOSampleRate *);
    ASIOError(STDMETHODCALLTYPE *setSampleRate)(IASIO *, ASIOSampleRate);
    ASIOError(STDMETHODCALLTYPE *getClockSources)(IASIO *, ASIOClockSource *,
                                                  long *num);
    ASIOError(STDMETHODCALLTYPE *setClockSource)(IASIO *, long ref);
    ASIOError(STDMETHODCALLTYPE *getSamplePosition)(IASIO *, ASIOSamples *,
                                                    ASIOTimeStamp *);
    ASIOError(STDMETHODCALLTYPE *getChannelInfo)(IASIO *, ASIOChannelInfo *);
    ASIOError(STDMETHODCALLTYPE *createBuffers)(IASIO *, ASIOBufferInfo *,
                                                long n, long size,
                                                ASIOCallbacks *);
    ASIOError(STDMETHODCALLTYPE *disposeBuffers)(IASIO *);
    ASIOError(STDMETHODCALLTYPE *controlPanel)(IASIO *);
    ASIOError(STDMETHODCALLTYPE *future)(IASIO *, long selector, void *opt);
    ASIOError(STDMETHODCALLTYPE *outputReady)(IASIO *);
} IASIOVtbl;

struct IASIO {
    const IASIOVtbl *lpVtbl;
};

// Only the vtbl slots this AO calls are wrapped; the vtbl itself must
// stay complete for the layout to match.
#define IASIO_Release(p)                  ((p)->lpVtbl->Release((p)))
#define IASIO_Init(p,h)                   ((p)->lpVtbl->init((p),(h)))
#define IASIO_GetErrorMessage(p,buf)      ((p)->lpVtbl->getErrorMessage((p),(buf)))
#define IASIO_Start(p)                    ((p)->lpVtbl->start((p)))
#define IASIO_Stop(p)                     ((p)->lpVtbl->stop((p)))
#define IASIO_GetChannels(p,i,o)          ((p)->lpVtbl->getChannels((p),(i),(o)))
#define IASIO_GetLatencies(p,i,o)         ((p)->lpVtbl->getLatencies((p),(i),(o)))
#define IASIO_GetBufferSize(p,a,b,c,d)    ((p)->lpVtbl->getBufferSize((p),(a),(b),(c),(d)))
#define IASIO_CanSampleRate(p,r)          ((p)->lpVtbl->canSampleRate((p),(r)))
#define IASIO_GetSampleRate(p,r)          ((p)->lpVtbl->getSampleRate((p),(r)))
#define IASIO_SetSampleRate(p,r)          ((p)->lpVtbl->setSampleRate((p),(r)))
#define IASIO_GetChannelInfo(p,info)      ((p)->lpVtbl->getChannelInfo((p),(info)))
#define IASIO_CreateBuffers(p,bi,n,s,cb)  ((p)->lpVtbl->createBuffers((p),(bi),(n),(s),(cb)))
#define IASIO_DisposeBuffers(p)           ((p)->lpVtbl->disposeBuffers((p)))
#define IASIO_OutputReady(p)              ((p)->lpVtbl->outputReady((p)))

#endif /* MP_AO_ASIO_H_ */
