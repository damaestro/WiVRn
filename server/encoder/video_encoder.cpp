/*
 * WiVRn VR streaming
 * Copyright (C) 2022  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2022  Patrick Nicolas <patricknicolas@laposte.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

// Include first because of incompatibility between Eigen and X includes
#include "driver/wivrn_session.h"

#include "video_encoder.h"

#include "os/os_time.h"
#include "util/u_logging.h"

#include <string>

#include "wivrn_config.h"

#ifdef WIVRN_USE_NVENC
#include "video_encoder_nvenc.h"
#endif
#ifdef WIVRN_USE_VAAPI
#include "ffmpeg/video_encoder_va.h"
#endif
#ifdef WIVRN_USE_X264
#include "video_encoder_x264.h"
#endif

namespace xrt::drivers::wivrn
{

VideoEncoder::sender::sender() :
        thread([this](std::stop_token t) {
	        while (not t.stop_requested())
	        {
		        data d;
		        {
			        std::unique_lock lock(mutex);
			        if (pending.empty())
			        {
				        cv.wait_for(lock, std::chrono::milliseconds(100));
			        }
			        else
			        {
				        d = std::move(pending.front());
				        pending.pop_front();
				        if (pending.empty())
					        cv.notify_all();
			        }
		        }
		        if (not d.span.empty())
			        d.encoder->SendData(d.span, true);
	        }
	        std::unique_lock lock(mutex);
	        pending.clear();
	        cv.notify_all();
        })
{
}

void VideoEncoder::sender::push(data && d)
{
	std::unique_lock lock(mutex);
	pending.push_back(std::move(d));
	cv.notify_all();
}

void VideoEncoder::sender::wait_idle()
{
	std::unique_lock lock(mutex);
	while (not pending.empty())
		cv.wait_for(lock, std::chrono::milliseconds(100));
}

std::shared_ptr<VideoEncoder::sender> VideoEncoder::sender::get()
{
	static std::weak_ptr<VideoEncoder::sender> instance;
	static std::mutex m;
	std::unique_lock lock(m);
	auto s = instance.lock();
	if (s)
		return s;
	s.reset(new VideoEncoder::sender());
	instance = s;
	return s;
}

std::unique_ptr<VideoEncoder> VideoEncoder::Create(
        wivrn_vk_bundle & wivrn_vk,
        encoder_settings & settings,
        uint8_t stream_idx,
        int input_width,
        int input_height,
        float fps)
{
	using namespace std::string_literals;
	std::unique_ptr<VideoEncoder> res;
#ifdef WIVRN_USE_X264
	if (settings.encoder_name == encoder_x264)
	{
		res = std::make_unique<VideoEncoderX264>(wivrn_vk, settings, fps);
	}
#endif
#ifdef WIVRN_USE_NVENC
	if (settings.encoder_name == encoder_nvenc)
	{
		res = std::make_unique<VideoEncoderNvenc>(wivrn_vk, settings, fps);
	}
#endif
#ifdef WIVRN_USE_VAAPI
	if (settings.encoder_name == encoder_vaapi)
	{
		res = std::make_unique<video_encoder_va>(wivrn_vk, settings, fps);
	}
#endif
	if (not res)
		throw std::runtime_error("Failed to create encoder " + settings.encoder_name);
	res->stream_idx = stream_idx;

	auto wivrn_dump_video = std::getenv("WIVRN_DUMP_VIDEO");
	if (wivrn_dump_video)
	{
		std::string file(wivrn_dump_video);
		file += "-" + std::to_string(stream_idx);
		switch (settings.codec)
		{
			case h264:
				file += ".h264";
				break;
			case h265:
				file += ".h265";
				break;
		}
		res->video_dump.open(file);
	}
	return res;
}

static const uint64_t idr_throttle = 100;

VideoEncoder::VideoEncoder(bool async_send) :
        last_idr_frame(-idr_throttle),
        shared_sender(async_send ? sender::get() : nullptr)
{}

VideoEncoder::~VideoEncoder()
{
	if (shared_sender)
		shared_sender->wait_idle();
}

void VideoEncoder::SyncNeeded()
{
	sync_needed = true;
}

void VideoEncoder::Encode(wivrn_session & cnx,
                          const to_headset::video_stream_data_shard::view_info_t & view_info,
                          uint64_t frame_index)
{
	if (shared_sender)
		shared_sender->wait_idle();
	this->cnx = &cnx;
	auto target_timestamp = std::chrono::steady_clock::time_point(std::chrono::nanoseconds(view_info.display_time));
	bool idr = sync_needed.exchange(false);
	// Throttle idr to prevent overloading the decoder
	if (idr and frame_index < last_idr_frame + idr_throttle)
	{
		U_LOG_D("Throttle IDR: stream %d frame %ld", stream_idx, frame_index);
		sync_needed = true;
		idr = false;
	}
	if (idr)
		last_idr_frame = frame_index;
	const char * extra = idr ? ",idr" : ",p";
	clock = cnx.get_offset();

	timing_info.encode_begin = clock.to_headset(os_monotonic_get_ns());
	cnx.dump_time("encode_begin", frame_index, os_monotonic_get_ns(), stream_idx, extra);

	// Prepare the video shard template
	shard.stream_item_idx = stream_idx;
	shard.frame_idx = frame_index;
	shard.shard_idx = 0;
	shard.view_info = view_info;
	shard.timing_info.reset();

	auto data = encode(idr, target_timestamp);
	if (data)
	{
		assert(shared_sender);
		shared_sender->push(std::move(*data));
	}
	cnx.dump_time("encode_end", frame_index, os_monotonic_get_ns(), stream_idx, extra);
}

void VideoEncoder::SendData(std::span<uint8_t> data, bool end_of_frame)
{
	std::lock_guard lock(mutex);
	if (end_of_frame)
		timing_info.send_end = clock.to_headset(os_monotonic_get_ns());
	if (video_dump)
		video_dump.write((char *)data.data(), data.size());
	if (shard.shard_idx == 0)
	{
		cnx->dump_time("send_begin", shard.frame_idx, os_monotonic_get_ns(), stream_idx);
		timing_info.send_begin = clock.to_headset(os_monotonic_get_ns());
	}

	shard.flags = to_headset::video_stream_data_shard::start_of_slice;
	auto begin = data.begin();
	auto end = data.end();
	while (begin != end)
	{
		const size_t view_info_size = sizeof(to_headset::video_stream_data_shard::view_info_t);
		const size_t max_payload_size = to_headset::video_stream_data_shard::max_payload_size - (shard.view_info ? view_info_size : 0);
		auto next = std::min(end, begin + max_payload_size);
		if (next == end)
		{
			shard.flags |= to_headset::video_stream_data_shard::end_of_slice;
			if (end_of_frame)
			{
				shard.flags |= to_headset::video_stream_data_shard::end_of_frame;
				shard.timing_info = timing_info;
			}
		}
		shard.payload = {begin, next};
		try
		{
			cnx->send_stream(shard);
		}
		catch (...)
		{
			// Ignore network errors
		}
		++shard.shard_idx;
		shard.flags = 0;
		shard.view_info.reset();
		begin = next;
	}
	if (end_of_frame)
		cnx->dump_time("send_end", shard.frame_idx, os_monotonic_get_ns(), stream_idx);
}

} // namespace xrt::drivers::wivrn
