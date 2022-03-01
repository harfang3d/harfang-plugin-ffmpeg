// Copyright (c) NWNC HARFANG and contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for details.
#include <foundation/log.h>
#include <foundation/format.h>
#include <foundation/clock.h>
#include <foundation/math.h>
#include <foundation/projection.h>
#include <foundation/matrix3.h>

#include <platform/input_system.h>
#include <platform/window_system.h>

#include <engine/render_pipeline.h>
#include <engine/scene.h>
#include <engine/assets.h>
#include <engine/video_stream_interface.h>
#include <engine/video_stream.h>

void draw_texture(bgfx::ViewId& view_id, const hg::Texture& texture, const hg::Vec2& position, const hg::Vec2& size, bgfx::ProgramHandle program, bgfx::UniformHandle u_source) {
	bgfx::VertexLayout layout;
	layout.begin().add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float).add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float).end();

	bgfx::TransientIndexBuffer index_buffer;
	bgfx::allocTransientIndexBuffer(&index_buffer, 6);
	uint16_t* idx = reinterpret_cast<uint16_t*>(index_buffer.data);
	idx[0] = 0;
	idx[1] = 1;
	idx[2] = 2;
	idx[3] = 0;
	idx[4] = 2;
	idx[5] = 3;

	hg::RenderState render_state = hg::ComputeRenderState(hg::BM_Alpha, hg::DT_Always, hg::FC_Disabled, false);
	
	bgfx::TransientVertexBuffer vertex_buffer;
	bgfx::allocTransientVertexBuffer(&vertex_buffer, hg::numeric_cast<uint32_t>(4), layout);

	float* data = (float*)vertex_buffer.data;
	hg::Vec3 p;
	p = hg::Vec3(position.x - size.x / 2.f, position.y - size.y / 2.f, 1.f);
	*data++ = p.x;
	*data++ = p.y;
	*data++ = p.z;
	*data++ = 0.f;
	*data++ = 0.f;

	p = hg::Vec3(position.x - size.x / 2.f, position.y + size.y / 2.f, 1.f);
	*data++ = p.x;
	*data++ = p.y;
	*data++ = p.z;
	*data++ = 0.f;
	*data++ = 1.f;

	p = hg::Vec3(position.x + size.x / 2.f, position.y + size.y / 2.f, 1.f);
	*data++ = p.x;
	*data++ = p.y;
	*data++ = p.z;
	*data++ = 1.f;
	*data++ = 1.f;

	p = hg::Vec3(position.x + size.x / 2.f, position.y - size.y / 2.f, 1.f);
	*data++ = p.x;
	*data++ = p.y;
	*data++ = p.z;
	*data++ = 1.f;
	*data++ = 0.f;

	bgfx::setTexture(0, u_source, texture.handle, uint32_t(texture.flags));

	bgfx::setVertexBuffer(0, &vertex_buffer);
	bgfx::setIndexBuffer(&index_buffer);
	bgfx::setState(render_state.state, render_state.rgba);
	bgfx::submit(view_id, program, 1);
}



int main(int argc, char** argv) {
	hg::set_log_level(hg::LL_All);
	if (argc != 2) {
		hg::error("usage: hg_vid_play video");
		return EXIT_FAILURE;
	}

	hg::InputInit();
	hg::WindowSystemInit();

	int res_x = 1280, res_y = 720;

	hg::Window* window = hg::RenderInit("Harfang - FFMpeg video stream plugin test", res_x, res_y, BGFX_RESET_VSYNC);
	if (!window) {
		hg::error("failed to create window.");
		return EXIT_FAILURE;
	}

	hg::AddAssetsFolder("assets");

	bgfx::UniformHandle u_source = bgfx::createUniform("u_source", bgfx::UniformType::Sampler);
	if (!bgfx::isValid(u_source)) {
		hg::error("failed to create uniform.");
		return EXIT_FAILURE;
	}

	bgfx::ProgramHandle program = hg::LoadProgramFromAssets("shaders/video_stream.vsb", "shaders/video_stream.fsb");
	if (!bgfx::isValid(program)) {
		hg::error("failed to create shader program.");
		return EXIT_FAILURE;
	}

	hg::Keyboard keyboard;
	hg::Mouse mouse;

	IVideoStreamer streamer = hg::MakeVideoStreamer("hg_ffmpeg.dll");
	if (!hg::IsValid(streamer)) {
		hg::error("failed to load video stream plugin");
		return EXIT_FAILURE;
	}

	if (!streamer.Startup()) {
		hg::error("failed to start video stream plugin");
		return EXIT_FAILURE;
	}

	VideoStreamHandle handle = streamer.Open(argv[1]);
	if (!handle) {
		hg::error(hg::format("failed to open video %1").arg((const char*)argv[1]));
		return EXIT_FAILURE;
	}

	if (!streamer.Play(handle)) {
		hg::error(hg::format("failed to play video %1").arg((const char*)argv[1]).c_str());
		return EXIT_FAILURE;
	}

	uint8_t* data;
	int width;
	int height;
	int pitch;
	VideoFrameFormat fmt;

	hg::Texture texture;
	hg::iVec2 size;
	bgfx::TextureFormat::Enum tex_fmt;

	while (!keyboard.Down(hg::K_Escape)) {
		hg::time_ns dt = hg::tick_clock();

		keyboard.Update();
		mouse.Update();
		
		if (keyboard.Pressed(hg::K_Space)) {
			streamer.Pause(handle);
		}
		else if (keyboard.Pressed(hg::K_Enter)) {
			streamer.Seek(handle, (5*60) * 1000000000LL);
		}
		hg::UpdateTexture(streamer, handle, texture, size, tex_fmt);

		bgfx::ViewId view_id = 0;

		bgfx::setViewFrameBuffer(view_id, BGFX_INVALID_HANDLE);
		bgfx::setViewMode(view_id, bgfx::ViewMode::DepthDescending);
		bgfx::touch(view_id);
		bgfx::setViewClear(view_id, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, hg::ColorToABGR32(hg::Color::Green), 1, 0);
		bgfx::setViewRect(view_id, 0, 0, res_x, res_y);
		bgfx::setViewName(view_id, "draw_texture");

		float proj[16];
		memcpy(proj, hg::to_bgfx(hg::Compute2DProjectionMatrix(0.1f, 100.f, res_x, res_y, false)).data(), sizeof(float[16]));
		bgfx::setViewTransform(view_id, nullptr, proj);
		float ar = size.x / (float)size.y;
		draw_texture(view_id, texture, hg::Vec2(res_x/2.f, res_y/2.f), hg::Vec2(600.f * ar, 600.f), program, u_source);

		bgfx::frame();
		hg::UpdateWindow(window);
	}

	streamer.Close(handle);
	streamer.Shutdown();


	bgfx::destroy(u_source);
	bgfx::destroy(program);

	hg::RenderShutdown();
	hg::DestroyWindow(window);

	hg::WindowSystemShutdown();
	hg::InputShutdown();

	return EXIT_SUCCESS;
}
