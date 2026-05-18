/*
 * WiVRn VR streaming — Perfetto tracing layer (via Percetto).
 *
 * All macros expand to no-ops when WIVRN_USE_PERFETTO is not defined, so call
 * sites can use them unconditionally without #ifdef clutter.
 *
 * Categories:
 *   encoder       — CPU-side encoder spans (present_image, encode, ...)
 *   encoder_gpu   — GPU-side encoder slices reconstructed from query
 *                   pool / host-observed timings (placed on named tracks)
 *   compositor    — Compositor thread / encoder dispatch thread spans
 *   network       — Send-path spans, packet emit instants
 *   feedback      — Headset-side instant events ferried in via
 *                   wivrn_session::dump_time
 */

#pragma once

#include <cstdint>

namespace wivrn::trace
{
// Initialise Percetto and register categories + tracks. Safe to call multiple
// times. No-op when WIVRN_USE_PERFETTO is undefined.
void init();

// Emit an instant trace event on the feedback category, tagged with frame /
// stream metadata in the event name. The `time_ns` value is monotonic from
// the server's perspective (dump_time already converts headset clocks).
void instant_feedback(const char * name, int64_t time_ns, uint64_t frame, int stream);

// Emit a slice on a named GPU track with explicit begin/end ns timestamps.
// `which` selects the track; the slice's display name is `slice_name`.
enum class gpu_track
{
	vulkan_encode,
	nvenc_encode,
	nvenc_copy,
	va_encode,
	va_copy,
};
void gpu_slice(gpu_track which, const char * slice_name,
               int64_t begin_ns, int64_t end_ns,
               uint64_t frame, int stream);
} // namespace wivrn::trace

#ifdef WIVRN_USE_PERFETTO

#include <percetto.h>

#define WIVRN_PERCETTO_CATEGORIES(C, G)                                    \
	C(encoder, "WiVRn encoder CPU spans")                              \
	C(encoder_gpu, "WiVRn encoder GPU slices")                         \
	C(compositor, "WiVRn compositor spans")                            \
	C(network, "WiVRn network send spans")                             \
	C(feedback, "WiVRn headset feedback events")

PERCETTO_CATEGORY_DECLARE(WIVRN_PERCETTO_CATEGORIES)

PERCETTO_TRACK_DECLARE(wivrn_vulkan_encode);
PERCETTO_TRACK_DECLARE(wivrn_nvenc_encode);
PERCETTO_TRACK_DECLARE(wivrn_nvenc_copy);
PERCETTO_TRACK_DECLARE(wivrn_va_encode);
PERCETTO_TRACK_DECLARE(wivrn_va_copy);
PERCETTO_TRACK_DECLARE(wivrn_feedback);

// Scoped span — begins on construction, ends at scope exit.
#define WIVRN_TRACE_SCOPE(cat, name) TRACE_EVENT(cat, name)

// Instant marker at the current time. Use a formatted/static const char *
// for `name` to attach context — percetto's instant API does not accept
// per-event arg lists.
#define WIVRN_TRACE_INSTANT(cat, name) TRACE_INSTANT(cat, name)

#else // !WIVRN_USE_PERFETTO

#define WIVRN_TRACE_SCOPE(cat, name) ((void)0)
#define WIVRN_TRACE_INSTANT(cat, name) ((void)0)

#endif // WIVRN_USE_PERFETTO
