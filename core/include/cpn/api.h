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
    CPN_INTERNAL_ERROR = 7,
    CPN_SESSION_CAPACITY = 8
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
typedef struct cpn_session_limits {
    uint64_t max_request_bytes;
    uint64_t max_documents;
    uint64_t max_tests_per_document;
    uint64_t max_total_tests;
    uint64_t max_source_bytes;
    uint64_t max_test_bytes;
    uint64_t max_total_content_bytes;
    uint64_t max_preferences_bytes;
    uint64_t max_id_bytes;
} cpn_session_limits;
/* Optional caller-owned error storage. Initialize struct_size before calling. */
typedef struct cpn_error {
    uint32_t struct_size;
    uint32_t status;
    char message[512];
} cpn_error;

CPN_API uint32_t cpn_abi_version(void);
/* Validates a versioned toolchain profile without creating an engine or starting a process. */
CPN_API cpn_status cpn_validate_toolchain_json(cpn_bytes profile_json, cpn_error*);
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
/* Recovery file I/O is synchronous; native clients call these from their background actor. */
/* Returns the limits for a recovery client using the largest supported request envelope. */
CPN_API cpn_session_limits cpn_session_default_limits(void);
CPN_API cpn_status cpn_engine_session_load(cpn_engine_handle, cpn_bytes path_utf8, cpn_buffer*, cpn_error*);
CPN_API cpn_status cpn_engine_session_save_json(cpn_engine_handle, cpn_bytes request_json, cpn_buffer*, cpn_error*);
/* Companion calls are synchronous and use schema-versioned JSON requests.
   page is non-destructive; accept/mark_applied reserve response quota before file I/O.
   prepare returns one session candidate containing both the document and its receipt. */
CPN_API cpn_status cpn_engine_companion_page_json(cpn_engine_handle, cpn_bytes request_json, cpn_buffer*, cpn_error*);
CPN_API cpn_status cpn_engine_companion_accept_json(cpn_engine_handle, cpn_bytes request_json, cpn_buffer*, cpn_error*);
CPN_API cpn_status cpn_engine_companion_prepare_json(cpn_engine_handle, cpn_bytes request_json, cpn_buffer*, cpn_error*);
CPN_API cpn_status cpn_engine_companion_mark_applied_json(cpn_engine_handle, cpn_bytes request_json, cpn_buffer*, cpn_error*);
/* Each engine owns at most one loopback listener. Start accepts schema_version=1,
   workspace_id, paths, optional bounds/options; stop/info accept only schema_version.
   Start/stop reserve response quota before binding/stopping. Close/destroy also stop
   and join the listener. Native clients invoke these calls on a background actor.
   Active same configuration is AlreadyStarted; a different active configuration is
   CPN_BUSY. After stop, start creates a new listener. Info remains available after close. */
CPN_API cpn_status cpn_engine_companion_listener_start_json(cpn_engine_handle, cpn_bytes request_json, cpn_buffer*, cpn_error*);
CPN_API cpn_status cpn_engine_companion_listener_stop_json(cpn_engine_handle, cpn_bytes request_json, cpn_buffer*, cpn_error*);
CPN_API cpn_status cpn_engine_companion_listener_info_json(cpn_engine_handle, cpn_bytes request_json, cpn_buffer*, cpn_error*);
/* Buffers remain valid until this call, including after engine destruction. */
CPN_API cpn_status cpn_buffer_release(cpn_buffer_handle, cpn_error*);

#ifdef __cplusplus
}
#endif
#endif
