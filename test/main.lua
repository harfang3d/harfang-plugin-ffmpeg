-- Copyright (c) NWNC HARFANG and contributors. All rights reserved.
-- Licensed under the MIT license. See LICENSE file in the project root for details.
hg = require("harfang")

hg.InputInit()
hg.WindowSystemInit()

res_x, res_y = 1280, 720
win = hg.RenderInit('Harfang - FFMpeg video stream plugin test', res_x, res_y, hg.RF_VSync)

hg.AddAssetsFolder('assets')

pipeline = hg.CreateForwardPipeline()
res = hg.PipelineResources()

prg = hg.LoadProgramFromAssets('shaders/video_stream.vsb', 'shaders/video_stream.fsb')

vtx_layout = hg.VertexLayoutPosFloatNormUInt8TexCoord0UInt8()

cube_mdl = hg.CreateCubeModel(vtx_layout, 1, 1, 1)
cube_ref = res:AddModel('cube', cube_mdl)

texture = hg.CreateTexture(res_x, res_y, "Video texture", 0)
size = hg.iVec2(res_x, res_y)
fmt = hg.TF_RGB8

streamer = hg.MakeVideoStreamer('hg_ffmpeg.dll')

streamer:Startup()

handle = streamer:Open(arg[1]);

streamer:Play(handle)

angle = 0

while not hg.ReadKeyboard('default'):Key(hg.K_Escape) do
	dt = hg.TickClock()
	angle = angle + hg.time_to_sec_f(dt)

	_, texture, size, fmt = hg.UpdateTexture(streamer, handle, texture, size, fmt)
	tex_uniforms = {hg.MakeUniformSetTexture('u_source', texture, 0)}

	view_id = 0

	hg.SetViewPerspective(view_id, 0, 0, res_x, res_y, hg.TranslationMat4(hg.Vec3(0, 0, -1.8)))

	hg.DrawModel(view_id, cube_mdl, prg, {}, tex_uniforms, hg.TransformationMat4(hg.Vec3(0, 0, 0), hg.Vec3(angle * 0.1, angle * 0.05, angle * 0.2)))

	hg.Frame()
	hg.UpdateWindow(win)
end

hg.RenderShutdown()
hg.DestroyWindow(win)
