/*
 * WiVRn VR streaming — Perfetto tracing layer (via Percetto).
 *
 * This translation unit holds the single PERCETTO_CATEGORY_DEFINE invocation
 * and the init() entry point. Compiled in unconditionally so that
 * wivrn::trace::init/instant_feedback/gpu_slice remain valid no-ops when
 * Perfetto is disabled.
 */

#include "wivrn_trace.h"

#ifdef WIVRN_USE_PERFETTO

#include "util/u_logging.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <mutex>

PERCETTO_CATEGORY_DEFINE(WIVRN_PERCETTO_CATEGORIES)

PERCETTO_TRACK_DEFINE(wivrn_vulkan_encode, PERCETTO_TRACK_EVENTS);
PERCETTO_TRACK_DEFINE(wivrn_nvenc_encode, PERCETTO_TRACK_EVENTS);
PERCETTO_TRACK_DEFINE(wivrn_nvenc_copy, PERCETTO_TRACK_EVENTS);
PERCETTO_TRACK_DEFINE(wivrn_va_encode, PERCETTO_TRACK_EVENTS);
PERCETTO_TRACK_DEFINE(wivrn_va_copy, PERCETTO_TRACK_EVENTS);
PERCETTO_TRACK_DEFINE(wivrn_feedback, PERCETTO_TRACK_EVENTS);

namespace wivrn::trace
{
namespace
{
std::once_flag init_flag;
std::atomic<bool> init_ok{false};

// Per-frame name buffers. We can't safely return a temporary's c_str() to
// percetto (it captures the pointer for later emit), so we keep a tiny ring
// of stable buffers per category. The ring depth needs to outlast the
// percetto emit, which happens synchronously on the calling thread — so a
// small ring is enough.
constexpr size_t name_ring_depth = 16;
thread_local std::array<std::string, name_ring_depth> name_ring;
thread_local size_t name_ring_idx = 0;
const char * stash_name(std::string s)
{
	auto & slot = name_ring[name_ring_idx];
	slot = std::move(s);
	name_ring_idx = (name_ring_idx + 1) % name_ring_depth;
	return slot.c_str();
}
} // namespace

static bool env_truthy(const char * v)
{
	if (!v || !*v)
		return false;
	return std::strcmp(v, "1") == 0 or
	       std::strcmp(v, "true") == 0 or
	       std::strcmp(v, "TRUE") == 0 or
	       std::strcmp(v, "yes") == 0 or
	       std::strcmp(v, "on") == 0;
}

void init()
{
	std::call_once(init_flag, [] {
		// Runtime gate, mirrors Monado's XRT_TRACING. Without the env
		// var set we don't open a percetto producer connection at all —
		// every trace macro short-circuits on the category session mask.
		if (!env_truthy(std::getenv("WIVRN_TRACING")))
		{
			U_LOG_I("wivrn::trace: WIVRN_TRACING not set, percetto tracing disabled");
			return;
		}

		U_LOG_I("wivrn::trace: WIVRN_TRACING=on, initializing percetto");

		// Two coexistence paths with Monado's own percetto integration
		// (src/xrt/auxiliary/util/u_trace_marker.c):
		//
		//  - If WiVRn is the only percetto producer in the process (e.g.
		//    Monado tracing is off via XRT_FEATURE_TRACING/XRT_TRACING),
		//    PERCETTO_INIT here succeeds and registers our category list
		//    with the producer.
		//
		//  - If Monado already called percetto_init() before us (the
		//    XRT_TRACING=true case), the percetto contract is "Only one
		//    call is allowed" and our PERCETTO_INIT returns a non-zero
		//    error. The producer is up, but it only knows about Monado's
		//    categories. We register ours late via the documented
		//    percetto_register_group_category() API — same approach Mesa
		//    uses to plug its driver categories into another component's
		//    percetto producer.
		//
		// Clock: both Monado and WiVRn use CLOCK_MONOTONIC, matching the
		// os_monotonic_get_ns() values we pass as explicit timestamps in
		// gpu_slice / instant_feedback. PERCETTO_CLOCK_DONT_CARE could
		// resolve to BOOTTIME and would shift slices on the timeline.
		const int rc = PERCETTO_INIT(PERCETTO_CLOCK_MONOTONIC);
		U_LOG_I("wivrn::trace: PERCETTO_INIT(CLOCK_MONOTONIC) rc=%d (%s)",
		        rc,
		        rc == 0 ? "this process owns the percetto producer"
		                : "another component already owns it; falling back to register_group_category");
		if (rc != 0)
		{
			// Assume another component (typically Monado) already owns
			// the producer. Register our categories with it.
			for (auto * cat : g_percetto_categories)
			{
				const int rr = percetto_register_group_category(cat);
				if (rr != 0)
				{
					U_LOG_E("wivrn::trace: percetto_register_group_category(%s) failed: %d "
					        "(matching events will not capture)",
					        cat->ext->name, rr);
				}
				else
				{
					U_LOG_I("wivrn::trace: registered category '%s' with existing producer", cat->ext->name);
				}
			}
		}
		// Track registration is independent of which component called
		// percetto_init: it operates on our own statically-allocated
		// percetto_track structs.
		PERCETTO_REGISTER_TRACK(wivrn_vulkan_encode);
		PERCETTO_REGISTER_TRACK(wivrn_nvenc_encode);
		PERCETTO_REGISTER_TRACK(wivrn_nvenc_copy);
		PERCETTO_REGISTER_TRACK(wivrn_va_encode);
		PERCETTO_REGISTER_TRACK(wivrn_va_copy);
		PERCETTO_REGISTER_TRACK(wivrn_feedback);
		init_ok.store(true, std::memory_order_release);
		U_LOG_I("wivrn::trace: init complete (5 categories, 6 GPU tracks registered)");
	});
}

void instant_feedback(const char * name, int64_t time_ns, uint64_t frame, int stream)
{
	if (!init_ok.load(std::memory_order_acquire))
		return;
	// Emit a zero-duration slice on the dedicated feedback track at the
	// caller-supplied timestamp. This anchors headset feedback events
	// (decode_begin, blit, display, ...) to their real clock-corrected
	// position on the timeline rather than to the moment the server
	// processed the feedback packet.
	const char * stashed = stash_name(
	        std::format("{} f={} s={}", name, frame, stream));
	TRACE_EVENT_BEGIN_ON_TRACK(feedback, wivrn_feedback, time_ns, stashed);
	TRACE_EVENT_END_ON_TRACK(feedback, wivrn_feedback, time_ns);
}

void gpu_slice(gpu_track which, const char * slice_name,
               int64_t begin_ns, int64_t end_ns,
               uint64_t frame, int stream)
{
	if (!init_ok.load(std::memory_order_acquire))
		return;
	const char * stashed = stash_name(
	        std::format("{} f={} s={}", slice_name, frame, stream));
	switch (which)
	{
		case gpu_track::vulkan_encode:
			TRACE_EVENT_BEGIN_ON_TRACK(encoder_gpu, wivrn_vulkan_encode, begin_ns, stashed);
			TRACE_EVENT_END_ON_TRACK(encoder_gpu, wivrn_vulkan_encode, end_ns);
			break;
		case gpu_track::nvenc_encode:
			TRACE_EVENT_BEGIN_ON_TRACK(encoder_gpu, wivrn_nvenc_encode, begin_ns, stashed);
			TRACE_EVENT_END_ON_TRACK(encoder_gpu, wivrn_nvenc_encode, end_ns);
			break;
		case gpu_track::nvenc_copy:
			TRACE_EVENT_BEGIN_ON_TRACK(encoder_gpu, wivrn_nvenc_copy, begin_ns, stashed);
			TRACE_EVENT_END_ON_TRACK(encoder_gpu, wivrn_nvenc_copy, end_ns);
			break;
		case gpu_track::va_encode:
			TRACE_EVENT_BEGIN_ON_TRACK(encoder_gpu, wivrn_va_encode, begin_ns, stashed);
			TRACE_EVENT_END_ON_TRACK(encoder_gpu, wivrn_va_encode, end_ns);
			break;
		case gpu_track::va_copy:
			TRACE_EVENT_BEGIN_ON_TRACK(encoder_gpu, wivrn_va_copy, begin_ns, stashed);
			TRACE_EVENT_END_ON_TRACK(encoder_gpu, wivrn_va_copy, end_ns);
			break;
	}
}
} // namespace wivrn::trace

#else // !WIVRN_USE_PERFETTO

namespace wivrn::trace
{
void init() {}
void instant_feedback(const char *, int64_t, uint64_t, int) {}
void gpu_slice(gpu_track, const char *, int64_t, int64_t, uint64_t, int) {}
} // namespace wivrn::trace

#endif // WIVRN_USE_PERFETTO
