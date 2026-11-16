/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CPN_API_H
#define CPN_API_H
#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && defined(CPN_SHARED)
# if defined(CPN_BUILDING_API)
#  define CPN_API __declspec(dllexport)
# else
#  define CPN_API __declspec(dllimport)
# endif
#else
# define CPN_API
#endif
#ifdef __cplusplus
extern "C" {
#endif

#define CPN_ABI_VERSION 1u
typedef uint64_t cpn_engine_handle;
typedef uint64_t cpn_job_handle;
typedef uint64_t cpn_buffer_handle;
typedef enum cpn_status {
    CPN_OK = 0,
    CPN_INVALID_ARGUMENT = 1,
    CPN_ABI_MISMATCH = 2,
    CPN_INVALID_HANDLE = 3,
    CPN_CAPACITY = 4,
    CPN_BUSY = 5,
    CPN_CLOSED = 6,
    CPN_INTERNAL_ERROR = 7
} cpn_status;
typedef struct cpn_bytes {
    const uint8_t* data;
    size_t size;
} cpn_bytes;
typedef struct cpn_buffer {
    const uint8_t* data;
    size_t size;
    cpn_buffer_handle handle;
} cpn_buffer;
typedef struct cpn_engine_options {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t max_workers;
    uint32_t max_jobs;
    uint32_t max_buffers;
    uint32_t reserved;
    uint64_t max_request_bytes;
    uint64_t max_retained_output_bytes;
} cpn_engine_options;
/* Optional caller-owned error storage. Initialize struct_size before calling. */
typedef struct cpn_error {
    uint32_t struct_size;
    uint32_t status;
    char message[512];
} cpn_error;

CPN_API uint32_t cpn_abi_version(void);
/* NULL options selects finite defaults. Nonzero fields override those defaults. */
CPN_API cpn_status cpn_engine_create(const cpn_engine_options*, cpn_engine_handle*, cpn_error*);
/* close/destroy cancel and join workers; call them from a background coordinator. */
CPN_API cpn_status cpn_engine_close(cpn_engine_handle, cpn_error*);
CPN_API cpn_status cpn_engine_destroy(cpn_engine_handle, cpn_error*);
/* Copies JSON before returning. Source/text and tests belong to that immutable job. */
CPN_API cpn_status cpn_engine_submit_json(cpn_engine_handle, cpn_bytes, cpn_job_handle*, cpn_error*);
CPN_API cpn_status cpn_engine_cancel(cpn_engine_handle, cpn_job_handle, cpn_error*);
/* Release only after its terminal event has been drained. Unknown IDs are errors. */
CPN_API cpn_status cpn_engine_release_job(cpn_engine_handle, cpn_job_handle, cpn_error*);
/* A successful drain acknowledges only events represented in its allocated buffer. */
CPN_API cpn_status cpn_engine_drain_json(cpn_engine_handle, uint32_t max_events, cpn_buffer*, cpn_error*);
CPN_API cpn_status cpn_engine_info_json(cpn_engine_handle, cpn_buffer*, cpn_error*);
/* Buffers remain valid until this call, including after engine destruction. */
CPN_API cpn_status cpn_buffer_release(cpn_buffer_handle, cpn_error*);

#ifdef __cplusplus
}
#endif
#endif
