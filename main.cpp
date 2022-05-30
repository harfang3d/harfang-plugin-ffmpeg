// Copyright (c) NWNC HARFANG and contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for details.
#include "video_stream_interface.h"

#include <chrono>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <thread>
#include <queue>
#include <deque>
#include <map>

#include <ctype.h>

extern "C" {
#include <libavformat/avformat.h> 
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libavdevice/avdevice.h>
}

// This should be dropped as soon as the major LTS linux distros switch to ffmpeg >= 59
#if (LIBAVFORMAT_VERSION_MAJOR < 59)
#	define ff_const 
#else
#	define ff_const const
#endif

struct VideoStream {
	struct Demuxer {
		enum State : int {
			Stopped = 0,
			Running,
			Finished,
			Faulted
		};
		Demuxer();
		~Demuxer();
		/// Initialize and launch demuxer job.
		/// @param [in] ctx Input context.
		/// @param [in] stream Video stream index.
		/// @param [in] n Max queue size (default: 16). 
		/// @return @b true if the demuxer job was successfully started.
		bool Start(AVFormatContext* ctx, int stream, size_t n = 16);
		/// Stop demuxer job.
		void Stop();
		/// Resume stream.
		bool Resume();
		/// Empty packet queue.
		void Clear();
		/// Retrieve the first packet from the queue.
		AVPacket* Get();
		/// Push a new packet into the queue.
		/// @param [in] packet Packet to add to the queue.
		/// @return @b true upon success or @b false if the queue is already full.
		bool Push(AVPacket* packet);
		/// Get packet from the stream and adds it to the stream.
		/// This is the job callback.
		void Update();

		/// Packet queue.
		std::queue<AVPacket*> queue;
		/// Maximum number of entries in the queue.
		size_t max_size;
		/// Queue lock.
		std::mutex mutex;
		/// Used to notify when a new packet is available.
		std::condition_variable available;
		/// 
		std::mutex resume_mutex;
		/// Used to notify the demuxer to resume work.
		std::condition_variable resume;
		/// 
		std::mutex pause_mutex;
		/// 
		std::condition_variable pause;
		///
		std::atomic_bool paused;
		/// Current state.
		std::atomic<int> state;
		/// Processing thread.
		std::thread job;
		/// FFMpeg input context.
		AVFormatContext* input_ctx;
		/// Video stream id.
		int video_stream;
	};

	VideoStream();
	~VideoStream();

	/// Initializes video decoding libraries.
	static int Init();
	/// Deinitializes video decoding libraries.
	static void Deinit();

	/// Open video stream decoding.
	/// @param [in] uri Video stream uri of video filepath.
	/// @return @b true if the video decoding job was successfully launched.
	bool Open(const char* uri);
	/// Start video stream.
	bool Start();
	/// Stop video stream.
	void Stop();
	/// Seek stream to a given timestamp.
	/// @param [in] t Timestamp.
	/// @return @b true upon success.
	bool Seek(int64_t t);
	/// Pause stream.
	bool Pause();
	/// Resume stream.
	bool Resume();

	/// Thread callback.
	void Loop();
	/// Return the last frame
	int CommitFrame(uint8_t** out);
	/// Release (mark as available) a frame
	bool ReleaseFrame(int id);

	AVCodecContext* codec_ctx;
	AVDictionary* dict;
	AVFormatContext* fmt_ctx;

	enum AVPixelFormat hw_pix_fmt;

	AVBufferRef* hw_device_ctx;

	Demuxer demuxer;

	AVFrame* output;

	int buffer_id;
	struct BufferRef {
		uint32_t count;
		uint8_t* ptr;
	};
	std::map<int, BufferRef> in_use;
	std::deque<uint8_t*> available;
	std::mutex buffer_mutex;

	int video_index;

	enum State {
		Stopped,
		Run,
		Paused
	};

	std::atomic<int> state;
	std::mutex state_mutex;
	std::condition_variable pause;

	std::atomic<bool> new_frame;
	std::thread th;

	/// Seek timestamp.
	std::atomic<int64_t> seek;
	/// Current timestamp.
	std::atomic<int64_t> elapsed;
};

VideoStream::Demuxer::Demuxer()
	: max_size(0)
	, paused(false)
	, state(VideoStream::Demuxer::Stopped)
	, input_ctx(NULL)
	, video_stream(-1)
{}

VideoStream::Demuxer::~Demuxer() {}

bool VideoStream::Demuxer::Start(AVFormatContext* ctx, int stream, size_t n) {
	if (!ctx || (stream < 0) || !n) {
		return false;
	}
	if (state != VideoStream::Demuxer::Stopped) {
		return false;
	}
	input_ctx = ctx;
	video_stream = stream;
	max_size = n;
	state = VideoStream::Demuxer::Running;
	job = std::thread(&VideoStream::Demuxer::Update, this);
	return true;
}

void VideoStream::Demuxer::Stop() {
	if (state == VideoStream::Demuxer::Stopped) {
		return;
	}
	state = VideoStream::Demuxer::Stopped;
	available.notify_all();
	resume.notify_all();
	if (job.joinable()) {
		job.join();
	}
	Clear();
}

bool VideoStream::Demuxer::Resume() {
	if ((state == VideoStream::Demuxer::Stopped) || (state == VideoStream::Demuxer::Faulted)) {
		return false;
	}
	{
		std::unique_lock<std::mutex> guard(resume_mutex);
		state = VideoStream::Demuxer::Running;
	}
	resume.notify_all();
	return true;
}

AVPacket* VideoStream::Demuxer::Get() {
	std::unique_lock<std::mutex> guard(mutex);
	if ((state == VideoStream::Demuxer::Stopped) || queue.empty()) {
		return NULL;
	}
	AVPacket* packet = queue.front();
	queue.pop();
	available.notify_all();
	return packet;
}

bool VideoStream::Demuxer::Push(AVPacket* packet) {
	std::unique_lock<std::mutex> guard(mutex);
	while (state == VideoStream::Demuxer::Running) {
		if (queue.size() >= max_size) {
			available.wait(guard);
		}
		else {
			queue.push(packet);
			return true;
		}
	}
	return false;
}

void VideoStream::Demuxer::Clear() {
	if (!paused && (state == VideoStream::Demuxer::Running)) {
		{
			std::unique_lock<std::mutex> guard(resume_mutex);
			state = VideoStream::Demuxer::Finished;
		}
		available.notify_all();
		{
			std::unique_lock<std::mutex> lock(pause_mutex);
			pause.wait(lock, [this]() { return paused.load(); });
		}
	}

	std::unique_lock<std::mutex> guard(mutex);
	for (; !queue.empty(); queue.pop()) {
		AVPacket* packet = queue.front();
		av_packet_free(&packet);
	}
}

void VideoStream::Demuxer::Update() {
	do {
		while (state == VideoStream::Demuxer::Running) {
			std::this_thread::yield();

			bool keep = false;
			AVPacket* packet = av_packet_alloc();

			int ret = av_read_frame(input_ctx, packet);
			if ((ret >= 0) && (video_stream == packet->stream_index)) {
				keep = Push(packet);
			}
			else if (ret == AVERROR_EOF) {
				state = VideoStream::Demuxer::Finished;
			}
			else if (ret < 0) {
				state = VideoStream::Demuxer::Faulted;
			}

			if (!keep) {
				av_packet_free(&packet);
			}
		}

		if ((state != VideoStream::Demuxer::Stopped) && (state != VideoStream::Demuxer::Faulted)) {
			{
				std::unique_lock<std::mutex> lock(pause_mutex);
				paused = true;
			}
			pause.notify_all();
			{
				std::unique_lock<std::mutex> guard(resume_mutex);
				resume.wait(guard, [this]() { return (state != VideoStream::Demuxer::Finished); });
			}
			{
				std::unique_lock<std::mutex> lock(pause_mutex);
				paused = false;
			}
		}
	} while ((state != Stopped) && (state != Faulted));
}

VideoStream::VideoStream()
	: codec_ctx(NULL)
	, dict(NULL)
	, fmt_ctx(NULL)
	, hw_pix_fmt(AV_PIX_FMT_NONE)
	, hw_device_ctx(NULL)
	, buffer_id(0)
	, in_use()
	, video_index(-1)
	, state(VideoStream::Stopped)
	, new_frame(false)
	, seek(-1)
	, elapsed(0)
{
}

VideoStream::~VideoStream() {}

VideoStreamHandle g_id;
std::map<VideoStreamHandle, VideoStream> g_streams;

int VideoStream::Init() {
	avdevice_register_all();
	return (avformat_network_init() == 0);
}

void VideoStream::Deinit() {
	avformat_network_deinit();
}

bool VideoStream::Open(const char* uri) {
	Stop();

	av_dict_set(&dict, "rtsp_transport", "tcp", 0);
	av_dict_set(&dict, "max_delay", "1000000", 0);			// [todo] this may be too big
	av_dict_set(&dict, "buffer_size", "2097152", 0);		// [todo] this may be too big
	av_dict_set(&dict, "flags", "low_delay", 0);
	av_dict_set(&dict, "fflags", "+genpts+discardcorrupt", 0);
	av_dict_set(&dict, "thread_queue_size", "1024", 0);
	av_dict_set(&dict, "tune", "zerolatency", 0);
	//av_dict_set(&dict, "pixel_format", "nv12", 0);

	ff_const AVInputFormat* input_format = NULL;

#ifdef _WINDOWS
	// eat spaces
	const char* it = uri;
	const char* end = uri + strlen(uri) + 1;
	for (; (it < end) && isspace(*it); it++) {}
	if (*it && (strncmp(it, "video=", 6) == 0)) {
		input_format = av_find_input_format("dshow");
	}
#endif

	elapsed = 0;

	int ret;
	ret = avformat_open_input(&fmt_ctx, uri, input_format, &dict);
	if (ret < 0) {
		return false;
	}

	ret = avformat_find_stream_info(fmt_ctx, 0);
	if (ret < 0) {
		return false;
	}

	// grab "best" video stream.
	ff_const AVCodec* decoder = NULL;
	video_index = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
	if (video_index < 0) {
		return false;
	}

	// try to grab a hardware decoder
	const AVCodecHWConfig* config;
	for (int i = 0;; i++) {
		config = avcodec_get_hw_config(decoder, i);
		if (config == NULL) {
			break;
		}
		if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) {
			break;
		}
	}

	codec_ctx = avcodec_alloc_context3(decoder);
	if (codec_ctx == NULL) {
		return false;
	}

	if (codec_ctx->thread_count == 1) {
		codec_ctx->thread_count = std::thread::hardware_concurrency();
		if (codec_ctx->thread_count > 8) {
			codec_ctx->thread_count = 8;
		}
		codec_ctx->thread_type = FF_THREAD_SLICE;
	}

	if (avcodec_parameters_to_context(codec_ctx, fmt_ctx->streams[video_index]->codecpar) < 0) {
		return false;
	}

	hw_device_ctx = NULL;
	if (config) {
		ret = av_hwdevice_ctx_create(&hw_device_ctx, config->device_type, NULL, NULL, 0);
		if (ret >= 0) {
			hw_pix_fmt = config->pix_fmt;
			codec_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
		}
	}

	// initialize video decoder.
	ret = avcodec_open2(codec_ctx, decoder, NULL);
	if (ret < 0) {
		return false;
	}

	seek = -1;

	new_frame = true;
	buffer_id = 0;
	output = av_frame_alloc();

	available.push_back(new uint8_t[codec_ctx->width * codec_ctx->height * 4]);
	av_image_fill_arrays(output->data, output->linesize, available.front(), AV_PIX_FMT_RGBA, codec_ctx->width, codec_ctx->height, 1);

	avcodec_flush_buffers(codec_ctx);

	return true;
}

bool VideoStream::Start() {
	if ((fmt_ctx == NULL) || (video_index < 0)) {				// [todo] better flags ?
		return false;
	}
	if (!demuxer.Start(fmt_ctx, video_index)) {
		return false;
	}

	th = std::thread(&VideoStream::Loop, this);
	return true;
}

void VideoStream::Stop() {
	{
		std::unique_lock<std::mutex> lock(state_mutex);
		state = VideoStream::Stopped;
	}
	pause.notify_all();

	new_frame = false;
	if (th.joinable()) {
		th.join();
	}

	demuxer.Stop();

	if (hw_device_ctx != NULL) {
		av_buffer_unref(&hw_device_ctx);
		hw_device_ctx = NULL;
	}
	if (codec_ctx != NULL) {
		avcodec_free_context(&codec_ctx);
		codec_ctx = NULL;
	}
	if (fmt_ctx != NULL) {
		avformat_close_input(&fmt_ctx);
		fmt_ctx = NULL;
	}
	if (dict != NULL) {
		av_dict_free(&dict);
		dict = NULL;
	}

	for (auto& it : in_use) {
		if (it.second.ptr) {
			delete[] it.second.ptr;
		}
	}
	in_use.clear();

	while(!available.empty()) {
		uint8_t* ptr = available.front();
		available.pop_front();
		delete[] ptr;
	}

	buffer_id = 0;

	hw_pix_fmt = AV_PIX_FMT_NONE;
	video_index = -1;
}

static const AVRational g_us_base = { 1, 1000000 };
static const AVRational g_av_base = { 1, AV_TIME_BASE };

void VideoStream::Loop() {
	AVFrame* frame = av_frame_alloc();
	AVFrame* sw_frame = (hw_device_ctx != NULL) ? av_frame_alloc() : NULL;
	struct SwsContext* converter = NULL;

	AVStream* input_ctx = fmt_ctx->streams[video_index];
	int64_t start_pts = (input_ctx->start_time != AV_NOPTS_VALUE) ? av_rescale_q(input_ctx->start_time, input_ctx->time_base, g_us_base) : 0;

	std::chrono::time_point<std::chrono::system_clock, std::chrono::nanoseconds> current, previous;
	current = std::chrono::system_clock::now();

	int ret = 1;
	state = VideoStream::Run;
	new_frame = false;
	while ((ret >= 0) && (state != VideoStream::Stopped)) {
		std::this_thread::yield();
		{
			std::unique_lock<std::mutex> guard(state_mutex);
			if (state == VideoStream::Paused) {
				pause.wait(guard, [this]() { return (state != VideoStream::Paused); });
				current = std::chrono::system_clock::now();
			}
		}

		if (seek >= 0) {
			demuxer.Clear();
			int64_t ts = seek * AV_TIME_BASE / 1000000000L;
			avformat_seek_file(fmt_ctx, -1, INT64_MIN, ts, INT64_MAX, 0);
			avcodec_flush_buffers(codec_ctx);
			current = std::chrono::system_clock::now();
			elapsed = seek.exchange(-1) / 1000;
			demuxer.Resume();
		}

		AVPacket* packet = demuxer.Get();
		if (!packet) {
			continue;
		}

		ret = avcodec_send_packet(codec_ctx, packet);
		while ((ret >= 0) && (state == VideoStream::Run) && (seek < 0)) {
			ret = avcodec_receive_frame(codec_ctx, frame);
			if ((ret == AVERROR(EAGAIN)) || (ret == AVERROR_EOF)) {
				ret = 0;
				break;
			}
			else if (ret >= 0) {
				int64_t current_pts = av_rescale_q(frame->best_effort_timestamp, input_ctx->time_base, g_us_base) - start_pts;
				previous = current;
				current = std::chrono::system_clock::now();
				elapsed += std::chrono::duration<int64_t, std::nano>(current - previous).count() / 1000;

				AVFrame* src = frame;
				if (hw_device_ctx != NULL) {
					if (frame->format == hw_pix_fmt) {
						av_hwframe_transfer_data(sw_frame, frame, 0);
						src = sw_frame;
					}
				}

				if (elapsed < current_pts) {
					int64_t dt = current_pts - elapsed;
					std::this_thread::sleep_for(std::chrono::microseconds(dt));
				}

				if (converter == NULL) {
					converter = sws_getContext(src->width, src->height, (AVPixelFormat)src->format, codec_ctx->width, codec_ctx->height, AV_PIX_FMT_RGBA, SWS_FAST_BILINEAR, NULL, NULL, NULL);
				}
				
				{
					std::lock_guard<std::mutex> lock(buffer_mutex);
					sws_scale(converter, (const unsigned char* const*)src->data, src->linesize, 0, codec_ctx->height, output->data, output->linesize);
					new_frame = true;
				}
			}

			av_frame_unref(frame);
			if (sw_frame) {
				av_frame_unref(sw_frame);
			}
		}

		av_packet_free(&packet);
	}
	state = VideoStream::Stopped;

	if (converter) {
		sws_freeContext(converter);
	}
	if (sw_frame) {
		av_frame_free(&sw_frame);
	}
	if (frame) {
		av_frame_free(&frame);
	}

	demuxer.Stop();
}

int VideoStream::CommitFrame(uint8_t **out) {
	std::lock_guard<std::mutex> lock(buffer_mutex);
	*out = NULL;
	if (available.empty()) {
		return 0;
	}

	int id = 0;
	*out = NULL;
	if(!new_frame) {
		// try to recycle last frame
		id = buffer_id;
		auto it = in_use.find(buffer_id);
		if (it != in_use.end()) {
			it->second.count++;
			*out = it->second.ptr;
			return it->first;
		}
		else {
			// The last frame has been released. 
			// Grab it from the end of the queue.
			*out = available.back();
			available.pop_back();
		}
	}
	else {
		*out = available.front();
		available.pop_front();
	}

	id = ++buffer_id;
	auto& it = in_use[id];
	it.ptr = *out;
	it.count = 1;

	// prepare new frame
	if (available.empty()) {
		available.push_back(new uint8_t[codec_ctx->width * codec_ctx->height * 4]);
	}
	av_image_fill_arrays(output->data, output->linesize, available.front(), AV_PIX_FMT_RGBA, codec_ctx->width, codec_ctx->height, 1);

	new_frame = false;

	return id;
}

bool VideoStream::ReleaseFrame(int id) {
	std::lock_guard<std::mutex> lock(buffer_mutex);
	auto it = in_use.find(id);
	if (it == in_use.end()) {
		return false;
	}
	it->second.count--;
	if (!it->second.count) {
		available.push_back(it->second.ptr);
		in_use.erase(it);
	}
	return true;
}

bool VideoStream::Seek(int64_t t) {
	seek = t;
	return true;
}

bool VideoStream::Pause() {
	std::unique_lock<std::mutex> guard(state_mutex);
	if (state == VideoStream::Paused) {
		return true;
	}
	else if (state != VideoStream::Run) {
		return false;
	}
	state = VideoStream::Paused;
	return true;
}

bool VideoStream::Resume() {
	std::unique_lock<std::mutex> guard(state_mutex);
	if (state == VideoStream::Paused) {
		state = VideoStream::Run;
	}
	pause.notify_all();
	return true;
}

VIDEO_STREAM_API int Startup() {
	g_id = 0;
	return VideoStream::Init();
}

VIDEO_STREAM_API void Shutdown() {
	for (auto &it : g_streams) {
		it.second.Stop();
	}
	g_streams.clear();
	g_id = 0;
	VideoStream::Deinit();
}

VIDEO_STREAM_API VideoStreamHandle Open(const char* name) {
	++g_id;
	VideoStream& stream = g_streams[g_id];
	if (!stream.Open(name)) {
		stream.Stop();
		g_streams.erase(g_id);
		return 0;
	}
	return g_id;
}

VIDEO_STREAM_API int Play(VideoStreamHandle h) {
	auto it = g_streams.find(h);
	if (it == g_streams.end()) {
		return 0;
	}
	return it->second.Start();
}

VIDEO_STREAM_API int Pause(VideoStreamHandle h) {
	auto it = g_streams.find(h);
	if (it == g_streams.end()) {
		return 0;
	}
	if (it->second.state == VideoStream::Paused) {
		return it->second.Resume();
	}
	return it->second.Pause();
}

VIDEO_STREAM_API int Close(VideoStreamHandle h) {
	auto it = g_streams.find(h);
	if (it == g_streams.end()) {
		return 0;
	}
	it->second.Stop();
	return 1;
}

VIDEO_STREAM_API int Seek(VideoStreamHandle h, time_ns t) {
	auto it = g_streams.find(h);
	if (it == g_streams.end()) {
		return 0;
	}
	return it->second.Seek(t);
}

VIDEO_STREAM_API time_ns GetDuration(VideoStreamHandle h) {
	auto it = g_streams.find(h);
	if (it == g_streams.end()) {
		return 0;
	}
	AVStream* input_ctx = it->second.fmt_ctx->streams[it->second.video_index];
	if (input_ctx->duration == AV_NOPTS_VALUE) {
		return -1;
	}
	int64_t duration_us = av_rescale_q(input_ctx->duration, input_ctx->time_base, g_us_base);
	return duration_us * 1000;
}

VIDEO_STREAM_API time_ns GetTimeStamp(VideoStreamHandle h) {
	auto it = g_streams.find(h);
	if (it == g_streams.end()) {
		return 0;
	}
	return it->second.elapsed * 1000;
}

VIDEO_STREAM_API int IsEnded(VideoStreamHandle h) {
	return 1;
}

VIDEO_STREAM_API int GetFrame(VideoStreamHandle h, const void** data, int* width, int* height, int* pitch, VideoFrameFormat* format) {
	*data = NULL;
	*width = *height = *pitch = 0;
	*format = VFF_UNKNOWN;

	auto it = g_streams.find(h);
	if (it == g_streams.end()) {
		return 0;
	}

	int id = it->second.CommitFrame((uint8_t**)data);
	if (!id) {
		return 0;
	}

	*width = it->second.codec_ctx->width;
	*height = it->second.codec_ctx->height;
	*pitch = it->second.codec_ctx->width * 4;
	*format = VFF_RGBA32;
	return id;
}

VIDEO_STREAM_API int FreeFrame(VideoStreamHandle h, int frame) {
	auto it = g_streams.find(h);
	if (it == g_streams.end()) {
		return 0;
	}
	return it->second.ReleaseFrame(frame);
}