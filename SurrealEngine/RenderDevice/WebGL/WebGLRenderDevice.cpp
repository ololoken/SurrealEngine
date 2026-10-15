
#include "Precomp.h"
#include "WebGLRenderDevice.h"
#include "WebGLCachedTexture.h"
#include "WebGLFileResource.h"
#include "Utils/Logger.h"
#include "Packages/Engine/Resources/Level/UModel.h"
#include <surrealwidgets/core/widget.h>
#include <cmath>
#include <cstdlib>

WebGLRenderDevice::WebGLRenderDevice(Widget* InViewport)
{
	Viewport = InViewport;
	if (!Init(Viewport->GetNativePixelWidth(), Viewport->GetNativePixelHeight(), false))
		Exception::Throw("Could not init WebGLRenderDevice");
}

WebGLRenderDevice::~WebGLRenderDevice()
{
	Exit();
}

bool WebGLRenderDevice::Init(int NewX, int NewY, bool Fullscreen)
{
	ActiveHdr = Hdr;

	try
	{
		Viewport->CreateGLContext();
		Viewport->MakeGLContextCurrent();

		LogContextInfo();
		DetectExtensions();

		const char* validate = std::getenv("SURREAL_WEBGL_VALIDATE");
		ValidateEveryCall = validate && *validate && *validate != '0';
		if (ValidateEveryCall)
			LogMessage("WebGLDrv: SURREAL_WEBGL_VALIDATE is set, checking glGetError after every operation (slow)");

		CreateScenePass();
		CreatePresentPass();
		CreateBloomPass();

		Textures.reset(new WebGLTextureManager(this));
		Uploads.reset(new WebGLUploadManager(this));

		ResetStateCache();
	}
	catch (const std::exception& e)
	{
		LogMessage(std::string("Could not create WebGL renderer: ") + e.what());
		Exit();
		return false;
	}

	if (!SetRes(NewX, NewY, Fullscreen))
	{
		Exit();
		return false;
	}

	return true;
}

void WebGLRenderDevice::LogContextInfo()
{
	auto str = [](GLenum name) { const char* s = (const char*)glGetString(name); return std::string(s ? s : "?"); };
	LogMessage("WebGLDrv: GL_VERSION=" + str(GL_VERSION) + ", GL_RENDERER=" + str(GL_RENDERER) + ", GLSL=" + str(GL_SHADING_LANGUAGE_VERSION));

	GLint count = 0;
	glGetIntegerv(GL_NUM_EXTENSIONS, &count);
	std::string extensions;
	for (GLint i = 0; i < count; i++)
	{
		const char* name = (const char*)glGetStringi(GL_EXTENSIONS, (GLuint)i);
		if (!name)
			continue;
		if (!extensions.empty())
			extensions += ", ";
		extensions += name;
	}
	LogMessage("WebGLDrv: " + std::to_string(count) + " extensions: " + extensions);

	while (glGetError() != GL_NO_ERROR)
		;
}

void WebGLRenderDevice::DetectExtensions()
{
	// Emscripten reports WebGL extensions as "GL_<name>" (and enables them automatically).
	auto hasExtension = [](const char* substring)
	{
		GLint count = 0;
		glGetIntegerv(GL_NUM_EXTENSIONS, &count);
		for (GLint i = 0; i < count; i++)
		{
			const char* name = (const char*)glGetStringi(GL_EXTENSIONS, (GLuint)i);
			if (name && strstr(name, substring))
				return true;
		}
		return false;
	};

	WebGLTextureUploader::SupportsS3TC = hasExtension("compressed_texture_s3tc");
	HasAnisotropicFiltering = hasExtension("texture_filter_anisotropic");
	HasColorBufferFloat = hasExtension("color_buffer_float") || hasExtension("color_buffer_half_float");

	LogMessage(std::string("WebGLDrv: s3tc=") + (WebGLTextureUploader::SupportsS3TC ? "yes" : "no") +
		", anisotropic=" + (HasAnisotropicFiltering ? "yes" : "no") +
		", float color buffers=" + (HasColorBufferFloat ? "yes" : "no"));

	while (glGetError() != GL_NO_ERROR)
		;
}

bool WebGLRenderDevice::SetRes(int NewX, int NewY, bool Fullscreen)
{
	CurrentSizeX = NewX;
	CurrentSizeY = NewY;
	CurrentFullscreen = Fullscreen;

	Flush(1);
	return true;
}

void WebGLRenderDevice::Exit()
{
	LogMessage("WebGLDrv: exit called");

	UnmapVertices();

	Uploads.reset();
	Textures.reset();
	ReleasePresentPass();
	ReleaseBloomPass();
	ReleaseScenePass();
	ReleaseSceneBuffers();
}

void WebGLRenderDevice::ResizeSceneBuffers(int width, int height)
{
	if (SceneBuffers.Width == width && SceneBuffers.Height == height && SceneBuffers.ColorBuffer && SceneBuffers.DepthBuffer && SceneBuffers.PPImage[0] && SceneBuffers.PPImage[1])
		return;

	ReleaseSceneBuffers();

	SceneBuffers.Width = width;
	SceneBuffers.Height = height;

	// RGBA16F is only color-renderable with EXT_color_buffer_float. Fall back to RGBA8 otherwise.
	GLint colorFormat = HasColorBufferFloat ? GL_RGBA16F : GL_RGBA8;
	GLenum colorType = HasColorBufferFloat ? GL_FLOAT : GL_UNSIGNED_BYTE;

	auto createColorTexture = [&](std::shared_ptr<WebGLTexture2D>& texture, int w, int h, const char* name)
	{
		texture = std::make_shared<WebGLTexture2D>();
		glBindTexture(GL_TEXTURE_2D, texture->Handle);
		glTexImage2D(GL_TEXTURE_2D, 0, colorFormat, w, h, 0, GL_RGBA, colorType, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
		ThrowIfWebGLError((std::string("glTexImage2D(") + name + ") failed").c_str());
	};

	auto createFramebuffer = [&](std::shared_ptr<WebGLFramebuffer>& framebuffer, WebGLTexture2D* color, WebGLTexture2D* depth, const char* name)
	{
		framebuffer = std::make_shared<WebGLFramebuffer>();
		glBindFramebuffer(GL_FRAMEBUFFER, framebuffer->Handle);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color->Handle, 0);
		if (depth)
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth->Handle, 0);
		ThrowIfWebGLError((std::string("Creating ") + name + " failed").c_str());
		GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
		if (status != GL_FRAMEBUFFER_COMPLETE)
			throw std::runtime_error(std::string(name) + " is incomplete (" + std::to_string(status) + ")");
	};

	createColorTexture(SceneBuffers.ColorBuffer, SceneBuffers.Width, SceneBuffers.Height, "SceneBuffers.ColorBuffer");

	SceneBuffers.DepthBuffer = std::make_shared<WebGLTexture2D>();
	glBindTexture(GL_TEXTURE_2D, SceneBuffers.DepthBuffer->Handle);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, SceneBuffers.Width, SceneBuffers.Height, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
	ThrowIfWebGLError("glTexImage2D(SceneBuffers.DepthBuffer) failed");

	for (int i = 0; i < 2; i++)
		createColorTexture(SceneBuffers.PPImage[i], SceneBuffers.Width, SceneBuffers.Height, "SceneBuffers.PPImage");

	createFramebuffer(SceneBuffers.Framebuffer, SceneBuffers.ColorBuffer.get(), SceneBuffers.DepthBuffer.get(), "SceneBuffers.Framebuffer");

	for (int i = 0; i < 2; i++)
		createFramebuffer(SceneBuffers.PPFramebuffer[i], SceneBuffers.PPImage[i].get(), nullptr, "SceneBuffers.PPFramebuffer");

	int bloomWidth = width;
	int bloomHeight = height;
	for (PPBlurLevel& level : SceneBuffers.BlurLevels)
	{
		bloomWidth = (bloomWidth + 1) / 2;
		bloomHeight = (bloomHeight + 1) / 2;

		createColorTexture(level.VTexture, bloomWidth, bloomHeight, "SceneBuffers.BlurLevels.VTexture");
		createColorTexture(level.HTexture, bloomWidth, bloomHeight, "SceneBuffers.BlurLevels.HTexture");
		createFramebuffer(level.VFramebuffer, level.VTexture.get(), nullptr, "SceneBuffers.BlurLevels.VFramebuffer");
		createFramebuffer(level.HFramebuffer, level.HTexture.get(), nullptr, "SceneBuffers.BlurLevels.HFramebuffer");

		level.Width = bloomWidth;
		level.Height = bloomHeight;
	}

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void WebGLRenderDevice::CreateScenePass()
{
	ScenePass.VertexShader = CreateVertexShader("ScenePass.VertexShader", "shaders/Scene.vert");
	ScenePass.FragmentShader = CreateFragmentShader("ScenePass.PixelShader", "shaders/Scene.frag");
	ScenePass.FragmentShaderAlphaTest = CreateFragmentShader("ScenePass.PixelShaderAlphaTest", "shaders/Scene.frag", { "ALPHATEST" });
	ScenePass.ShaderProgram = CreateProgram("ScenePass.ShaderProgram", ScenePass.VertexShader, ScenePass.FragmentShader);
	ScenePass.ShaderProgramAlphaTest = CreateProgram("ScenePass.ShaderProgramAlphaTest", ScenePass.VertexShader, ScenePass.FragmentShaderAlphaTest);

	CreateSceneSamplers();

	WebGLRasterizerDesc rasterizerDesc = {};
	rasterizerDesc.CullEnable = false;
	rasterizerDesc.FrontCounterClockwise = false;
	ScenePass.RasterizerState = CreateRasterizerState(rasterizerDesc);

	for (int i = 0; i < 33; i++)
	{
		WebGLBlendDesc blendDesc = {};
		blendDesc.BlendEnable = true;
		if (i < 32)
		{
			switch (i & 3)
			{
			case 0: // PF_Translucent
				blendDesc.BlendOp = GL_FUNC_ADD;
				blendDesc.BlendOpAlpha = GL_FUNC_ADD;
				blendDesc.SrcBlend = GL_ONE;
				blendDesc.SrcBlendAlpha = GL_ONE;
				blendDesc.DestBlend = GL_ONE_MINUS_SRC_COLOR;
				blendDesc.DestBlendAlpha = GL_ONE_MINUS_SRC_ALPHA;
				break;
			case 1: // PF_Modulated
				blendDesc.BlendOp = GL_FUNC_ADD;
				blendDesc.BlendOpAlpha = GL_FUNC_ADD;
				blendDesc.SrcBlend = GL_DST_COLOR;
				blendDesc.SrcBlendAlpha = GL_DST_ALPHA;
				blendDesc.DestBlend = GL_SRC_COLOR;
				blendDesc.DestBlendAlpha = GL_SRC_ALPHA;
				break;
			case 2: // PF_Highlighted
				blendDesc.BlendOp = GL_FUNC_ADD;
				blendDesc.BlendOpAlpha = GL_FUNC_ADD;
				blendDesc.SrcBlend = GL_ONE;
				blendDesc.SrcBlendAlpha = GL_ONE;
				blendDesc.DestBlend = GL_ONE_MINUS_SRC_ALPHA;
				blendDesc.DestBlendAlpha = GL_ONE_MINUS_SRC_ALPHA;
				break;
			case 3: // Hmm, is it faster to keep the blend mode enabled or to toggle it?
				blendDesc.BlendOp = GL_FUNC_ADD;
				blendDesc.BlendOpAlpha = GL_FUNC_ADD;
				blendDesc.SrcBlend = GL_ONE;
				blendDesc.SrcBlendAlpha = GL_ONE;
				blendDesc.DestBlend = GL_ZERO;
				blendDesc.DestBlendAlpha = GL_ZERO;
				break;
			}
			if (i & 4) // PF_Invisible
				blendDesc.RenderTargetWriteMask = false;
			else
				blendDesc.RenderTargetWriteMask = true;
		}
		else // PF_SubpixelFont
		{
			blendDesc.BlendOp = GL_FUNC_ADD;
			blendDesc.BlendOpAlpha = GL_FUNC_ADD;
			blendDesc.SrcBlend = GL_CONSTANT_COLOR;
			blendDesc.SrcBlendAlpha = GL_CONSTANT_ALPHA;
			blendDesc.DestBlend = GL_ONE_MINUS_SRC_COLOR;
			blendDesc.DestBlendAlpha = GL_ONE_MINUS_SRC_ALPHA;
			blendDesc.RenderTargetWriteMask = true;
		}
		ScenePass.Pipelines[i].BlendState = CreateBlendState(blendDesc);

		WebGLDepthStencilDesc depthStencilDesc = {};
		depthStencilDesc.DepthEnable = true;
		depthStencilDesc.DepthFunc = GL_LEQUAL;
		if (i & 8) // PF_Occlude
			depthStencilDesc.DepthWriteMask = true;
		else
			depthStencilDesc.DepthWriteMask = false;
		ScenePass.Pipelines[i].DepthStencilState = CreateDepthStencilState(depthStencilDesc);

		if ((i & 16) || i == 32) // PF_Masked or PF_SubpixelFont
			ScenePass.Pipelines[i].ShaderProgram = ScenePass.ShaderProgramAlphaTest.get();
		else
			ScenePass.Pipelines[i].ShaderProgram = ScenePass.ShaderProgram.get();

		ScenePass.Pipelines[i].PrimitiveTopology = GL_TRIANGLES;
	}

	// Line and point pipelines
	for (int i = 0; i < 2; i++)
	{
		WebGLBlendDesc blendDesc = {};
		blendDesc.BlendEnable = true;
		blendDesc.BlendOp = GL_FUNC_ADD;
		blendDesc.BlendOpAlpha = GL_FUNC_ADD;
		blendDesc.SrcBlend = GL_ONE;
		blendDesc.SrcBlendAlpha = GL_ONE;
		blendDesc.DestBlend = GL_ONE_MINUS_SRC_ALPHA;
		blendDesc.DestBlendAlpha = GL_ONE_MINUS_SRC_ALPHA;
		blendDesc.RenderTargetWriteMask = true;

		WebGLDepthStencilDesc depthStencilDesc = {};
		depthStencilDesc.DepthEnable = true;
		depthStencilDesc.DepthFunc = GL_LEQUAL;
		depthStencilDesc.DepthWriteMask = true;

		ScenePass.LinePipeline[i].BlendState = CreateBlendState(blendDesc);
		ScenePass.LinePipeline[i].DepthStencilState = CreateDepthStencilState(depthStencilDesc);
		ScenePass.LinePipeline[i].ShaderProgram = ScenePass.ShaderProgram.get();
		ScenePass.LinePipeline[i].PrimitiveTopology = GL_LINES;

		ScenePass.PointPipeline[i].BlendState = CreateBlendState(blendDesc);
		ScenePass.PointPipeline[i].DepthStencilState = CreateDepthStencilState(depthStencilDesc);
		ScenePass.PointPipeline[i].ShaderProgram = ScenePass.ShaderProgram.get();
		ScenePass.PointPipeline[i].PrimitiveTopology = GL_TRIANGLES;

		if (i == 0)
		{
			ScenePass.LinePipeline[i].MinDepth = 0.0f;
			ScenePass.LinePipeline[i].MaxDepth = 0.1f;
			ScenePass.PointPipeline[i].MinDepth = 0.0f;
			ScenePass.PointPipeline[i].MaxDepth = 0.1f;
		}
	}

	ScenePass.VertexBuffer = CreateBuffer(GL_ARRAY_BUFFER, GL_STREAM_DRAW, nullptr, SceneVertexBufferSize * sizeof(WebGLSceneVertex), "ScenePass.VertexBuffer");
	ScenePass.IndexBuffer = CreateBuffer(GL_ELEMENT_ARRAY_BUFFER, GL_STREAM_DRAW, nullptr, SceneIndexBufferSize * sizeof(uint32_t), "ScenePass.IndexBuffer");
	ScenePass.ConstantBuffer = CreateBuffer(GL_UNIFORM_BUFFER, GL_STREAM_DRAW, nullptr, sizeof(WebGLScenePushConstants), "ScenePass.ConstantBuffer");

	ScenePass.InputLayout = std::make_shared<WebGLInputLayout>();
	glBindVertexArray(ScenePass.InputLayout->Handle);
	glBindBuffer(GL_ARRAY_BUFFER, ScenePass.VertexBuffer->Handle);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ScenePass.IndexBuffer->Handle); // element buffer binding is part of the VAO state
	glEnableVertexAttribArray(0);
	glVertexAttribIPointer(0, 1, GL_UNSIGNED_INT, sizeof(WebGLSceneVertex), (const void*)offsetof(WebGLSceneVertex, Flags));
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(WebGLSceneVertex), (const void*)offsetof(WebGLSceneVertex, Position));
	glEnableVertexAttribArray(2);
	glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(WebGLSceneVertex), (const void*)offsetof(WebGLSceneVertex, TexCoord));
	glEnableVertexAttribArray(3);
	glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, sizeof(WebGLSceneVertex), (const void*)offsetof(WebGLSceneVertex, TexCoord2));
	glEnableVertexAttribArray(4);
	glVertexAttribPointer(4, 2, GL_FLOAT, GL_FALSE, sizeof(WebGLSceneVertex), (const void*)offsetof(WebGLSceneVertex, TexCoord3));
	glEnableVertexAttribArray(5);
	glVertexAttribPointer(5, 2, GL_FLOAT, GL_FALSE, sizeof(WebGLSceneVertex), (const void*)offsetof(WebGLSceneVertex, TexCoord4));
	glEnableVertexAttribArray(6);
	glVertexAttribPointer(6, 4, GL_FLOAT, GL_FALSE, sizeof(WebGLSceneVertex), (const void*)offsetof(WebGLSceneVertex, Color));
	glBindVertexArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	ThrowIfWebGLError("Creating ScenePass.InputLayout failed");
}

void WebGLRenderDevice::CreateSceneSamplers()
{
	for (int i = 0; i < 16; i++)
	{
		ScenePass.Samplers[i] = std::make_shared<WebGLSampler>();
		GLuint sampler = ScenePass.Samplers[i]->Handle;

		int dummyMipmapCount = (i >> 2) & 3;

		// WebGL 2 has no GL_MIRROR_CLAMP_TO_EDGE
		GLint addressmode = (i & 2) ? GL_CLAMP_TO_EDGE : GL_REPEAT;

		glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, (i & 1) ? GL_NEAREST_MIPMAP_NEAREST : GL_LINEAR_MIPMAP_LINEAR);
		glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, (i & 1) ? GL_NEAREST : GL_LINEAR);
		glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, addressmode);
		glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, addressmode);
		glSamplerParameteri(sampler, GL_TEXTURE_WRAP_R, addressmode);

		// Dummy mipmap levels are skipped with GL_TEXTURE_MIN_LOD. GLES has no sampler
		// LOD bias, so the user LODBias setting has no effect in this backend.
		glSamplerParameterf(sampler, GL_TEXTURE_MIN_LOD, (float)dummyMipmapCount);
		ThrowIfWebGLError("Could not create sampler");

		if (HasAnisotropicFiltering && (i & 1) == 0)
		{
			glSamplerParameterf(sampler, GL_TEXTURE_MAX_ANISOTROPY_EXT, 8.0f);
			// Not every browser accepts the anisotropy enum on sampler objects. It is a quality
			// setting only, so swallow a rejection instead of failing device creation.
			while (glGetError() != GL_NO_ERROR)
				;
		}
	}

	ScenePass.LODBias = LODBias;
}

void WebGLRenderDevice::ReleaseSceneSamplers()
{
	for (auto& sampler : ScenePass.Samplers)
	{
		sampler.reset();
	}
	ScenePass.LODBias = 0.0f;
}

void WebGLRenderDevice::UpdateLODBias()
{
	if (ScenePass.LODBias != LODBias)
	{
		ReleaseSceneSamplers();
		CreateSceneSamplers();
	}
}

void WebGLRenderDevice::ReleaseScenePass()
{
	ScenePass.VertexShader.reset();
	ScenePass.InputLayout.reset();
	ScenePass.VertexBuffer.reset();
	ScenePass.IndexBuffer.reset();
	ScenePass.ConstantBuffer.reset();
	ScenePass.RasterizerState.reset();
	ScenePass.ShaderProgram.reset();
	ScenePass.ShaderProgramAlphaTest.reset();
	ScenePass.FragmentShader.reset();
	ScenePass.FragmentShaderAlphaTest.reset();
	ReleaseSceneSamplers();
	for (auto& pipeline : ScenePass.Pipelines)
	{
		pipeline.BlendState.reset();
		pipeline.DepthStencilState.reset();
	}
	for (int i = 0; i < 2; i++)
	{
		ScenePass.LinePipeline[i].BlendState.reset();
		ScenePass.LinePipeline[i].DepthStencilState.reset();
		ScenePass.PointPipeline[i].BlendState.reset();
		ScenePass.PointPipeline[i].DepthStencilState.reset();
	}
}

void WebGLRenderDevice::ReleaseBloomPass()
{
	BloomPass.Extract.reset();
	BloomPass.Combine.reset();
	BloomPass.BlurVertical.reset();
	BloomPass.BlurHorizontal.reset();
	BloomPass.ExtractProgram.reset();
	BloomPass.CombineProgram.reset();
	BloomPass.BlurVerticalProgram.reset();
	BloomPass.BlurHorizontalProgram.reset();
	BloomPass.ConstantBuffer.reset();
	BloomPass.AdditiveBlendState.reset();
}

void WebGLRenderDevice::ReleasePresentPass()
{
	PresentPass.PPStepLayout.reset();
	PresentPass.PPStep.reset();
	PresentPass.PPStepVertexBuffer.reset();
	for (auto& shader : PresentPass.Present) shader.reset();
	for (auto& program : PresentPass.PresentProgram) program.reset();
	PresentPass.PresentConstantBuffer.reset();
	PresentPass.DitherTexture.reset();
	PresentPass.LinearSampler.reset();
	PresentPass.NearestSampler.reset();
	PresentPass.BlendState.reset();
	PresentPass.DepthStencilState.reset();
	PresentPass.RasterizerState.reset();
}

void WebGLRenderDevice::ReleaseSceneBuffers()
{
	SceneBuffers.Framebuffer.reset();
	for (int i = 0; i < 2; i++)
	{
		SceneBuffers.PPFramebuffer[i].reset();
		SceneBuffers.PPImage[i].reset();
	}
	SceneBuffers.ColorBuffer.reset();
	SceneBuffers.DepthBuffer.reset();
	for (PPBlurLevel& level : SceneBuffers.BlurLevels)
	{
		level.VTexture.reset();
		level.VFramebuffer.reset();
		level.HTexture.reset();
		level.HFramebuffer.reset();
	}
}

WebGLRenderDevice::ScenePipelineState* WebGLRenderDevice::GetPipeline(uint32_t PolyFlags)
{
	int index;
	if (PolyFlags & PF_Translucent)
	{
		index = 0;
	}
	else if (PolyFlags & PF_Modulated)
	{
		index = 1;
	}
	else if (PolyFlags & PF_Highlighted)
	{
		index = 2;
	}
	else
	{
		index = 3;
	}

	if (PolyFlags & PF_Invisible)
	{
		index |= 4;
	}
	if (PolyFlags & PF_Occlude)
	{
		index |= 8;
	}
	if (PolyFlags & PF_Masked)
	{
		index |= 16;
	}

	if (PolyFlags == PF_SubpixelFont)
	{
		index = 32;
	}

	return &ScenePass.Pipelines[index];
}

void WebGLRenderDevice::RunBloomPass()
{
	WebGLTexture2D* srvs[1] = {};

	float blurAmount = 0.6f + BloomAmount * (1.9f / 255.0f);
	WebGLBloomPushConstants pushconstants;
	ComputeBlurSamples(7, blurAmount, pushconstants.SampleWeights);

	SetBufferData(GL_UNIFORM_BUFFER, GL_STREAM_DRAW, BloomPass.ConstantBuffer.get(), &pushconstants, sizeof(WebGLBloomPushConstants));

	WebGLBuffer* cbs[1] = { BloomPass.ConstantBuffer.get() };
	WebGLSampler* samplers[1] = { PresentPass.LinearSampler.get() };
	glBindVertexArray(PresentPass.PPStepLayout->Handle);
	SetRasterizerState(PresentPass.RasterizerState.get());
	SetUniformBuffers(0, 1, cbs);
	SetSamplers(0, 1, samplers);
	SetDepthStencilState(PresentPass.DepthStencilState.get());
	SetBlendState(PresentPass.BlendState.get());

	WebGLViewport viewport = {};
	viewport.MaxDepth = 1.0f;

	// Extract overbright pixels that we want to bloom:
	viewport.Width = (float)SceneBuffers.BlurLevels[0].Width;
	viewport.Height = (float)SceneBuffers.BlurLevels[0].Height;

	srvs[0] = SceneBuffers.PPImage[0].get();
	glBindFramebuffer(GL_FRAMEBUFFER, SceneBuffers.BlurLevels[0].VFramebuffer->Handle);
	UseProgram(BloomPass.ExtractProgram->Handle);
	SetViewport(viewport);
	SetTextures(0, 1, srvs);
	glDrawArrays(GL_TRIANGLES, 0, 6);

	// Blur and downscale:
	for (int i = 0; i < SceneBuffers.NumBloomLevels - 1; i++)
	{
		auto& blevel = SceneBuffers.BlurLevels[i];
		auto& next = SceneBuffers.BlurLevels[i + 1];

		viewport.Width = (float)blevel.Width;
		viewport.Height = (float)blevel.Height;
		SetViewport(viewport);
		BlurStep(blevel.VTexture.get(), blevel.HFramebuffer.get(), false);
		BlurStep(blevel.HTexture.get(), blevel.VFramebuffer.get(), true);

		// Linear downscale:
		viewport.Width = (float)next.Width;
		viewport.Height = (float)next.Height;
		srvs[0] = blevel.VTexture.get();

		glBindFramebuffer(GL_FRAMEBUFFER, next.VFramebuffer->Handle);
		UseProgram(BloomPass.CombineProgram->Handle);
		SetViewport(viewport);
		SetTextures(0, 1, srvs);
		glDrawArrays(GL_TRIANGLES, 0, 6);
	}

	// Blur and upscale:
	for (int i = SceneBuffers.NumBloomLevels - 1; i > 0; i--)
	{
		auto& blevel = SceneBuffers.BlurLevels[i];
		auto& next = SceneBuffers.BlurLevels[i - 1];

		viewport.Width = (float)blevel.Width;
		viewport.Height = (float)blevel.Height;
		SetViewport(viewport);
		BlurStep(blevel.VTexture.get(), blevel.HFramebuffer.get(), false);
		BlurStep(blevel.HTexture.get(), blevel.VFramebuffer.get(), true);

		// Linear upscale:
		viewport.Width = (float)next.Width;
		viewport.Height = (float)next.Height;
		srvs[0] = blevel.VTexture.get();
		glBindFramebuffer(GL_FRAMEBUFFER, next.VFramebuffer->Handle);
		UseProgram(BloomPass.CombineProgram->Handle);
		SetViewport(viewport);
		SetTextures(0, 1, srvs);
		glDrawArrays(GL_TRIANGLES, 0, 6);
	}

	viewport.Width = (float)SceneBuffers.BlurLevels[0].Width;
	viewport.Height = (float)SceneBuffers.BlurLevels[0].Height;
	SetViewport(viewport);
	BlurStep(SceneBuffers.BlurLevels[0].VTexture.get(), SceneBuffers.BlurLevels[0].HFramebuffer.get(), false);
	BlurStep(SceneBuffers.BlurLevels[0].HTexture.get(), SceneBuffers.BlurLevels[0].VFramebuffer.get(), true);

	// Add bloom back to scene post process texture:
	viewport.Width = (float)SceneBuffers.Width;
	viewport.Height = (float)SceneBuffers.Height;
	srvs[0] = SceneBuffers.BlurLevels[0].VTexture.get();
	glBindFramebuffer(GL_FRAMEBUFFER, SceneBuffers.PPFramebuffer[0]->Handle);
	UseProgram(BloomPass.CombineProgram->Handle);
	SetBlendState(BloomPass.AdditiveBlendState.get());
	SetViewport(viewport);
	SetTextures(0, 1, srvs);
	glDrawArrays(GL_TRIANGLES, 0, 6);
}

void WebGLRenderDevice::BlurStep(WebGLTexture2D* input, WebGLFramebuffer* output, bool vertical)
{
	glBindFramebuffer(GL_FRAMEBUFFER, output->Handle);
	UseProgram(vertical ? BloomPass.BlurVerticalProgram->Handle : BloomPass.BlurHorizontalProgram->Handle);
	SetTextures(0, 1, &input);
	glDrawArrays(GL_TRIANGLES, 0, 6);
}

float WebGLRenderDevice::ComputeBlurGaussian(float n, float theta) // theta = Blur Amount
{
	return (float)((1.0f / std::sqrt(2 * 3.14159265359f * theta)) * std::exp(-(n * n) / (2.0f * theta * theta)));
}

void WebGLRenderDevice::ComputeBlurSamples(int sampleCount, float blurAmount, float* sampleWeights)
{
	sampleWeights[0] = ComputeBlurGaussian(0, blurAmount);

	float totalWeights = sampleWeights[0];

	for (int i = 0; i < sampleCount / 2; i++)
	{
		float weight = ComputeBlurGaussian(i + 1.0f, blurAmount);

		sampleWeights[i * 2 + 1] = weight;
		sampleWeights[i * 2 + 2] = weight;

		totalWeights += weight * 2;
	}

	for (int i = 0; i < sampleCount; i++)
	{
		sampleWeights[i] /= totalWeights;
	}
}

void WebGLRenderDevice::CreateBloomPass()
{
	BloomPass.Extract = CreateFragmentShader("BloomPass.Extract", "shaders/BloomExtract.frag");
	BloomPass.ExtractProgram = CreateProgram("BloomPass.ExtractProgram", PresentPass.PPStep, BloomPass.Extract);
	BloomPass.Combine = CreateFragmentShader("BloomPass.Combine", "shaders/BloomCombine.frag");
	BloomPass.CombineProgram = CreateProgram("BloomPass.CombineProgram", PresentPass.PPStep, BloomPass.Combine);
	BloomPass.BlurVertical = CreateFragmentShader("BloomPass.BlurVertical", "shaders/Blur.frag", { "BLUR_VERTICAL" });
	BloomPass.BlurVerticalProgram = CreateProgram("BloomPass.BlurVerticalProgram", PresentPass.PPStep, BloomPass.BlurVertical);
	BloomPass.BlurHorizontal = CreateFragmentShader("BloomPass.BlurHorizontal", "shaders/Blur.frag", { "BLUR_HORIZONTAL" });
	BloomPass.BlurHorizontalProgram = CreateProgram("BloomPass.BlurHorizontalProgram", PresentPass.PPStep, BloomPass.BlurHorizontal);

	BloomPass.ConstantBuffer = CreateBuffer(GL_UNIFORM_BUFFER, GL_STREAM_DRAW, nullptr, sizeof(WebGLBloomPushConstants), "BloomPass.ConstantBuffer");

	WebGLBlendDesc blendDesc = {};
	blendDesc.RenderTargetWriteMask = true;
	blendDesc.BlendEnable = true;
	blendDesc.BlendOp = GL_FUNC_ADD;
	blendDesc.BlendOpAlpha = GL_FUNC_ADD;
	blendDesc.SrcBlend = GL_ONE;
	blendDesc.SrcBlendAlpha = GL_ONE;
	blendDesc.DestBlend = GL_ONE;
	blendDesc.DestBlendAlpha = GL_ONE;
	BloomPass.AdditiveBlendState = CreateBlendState(blendDesc);
}

void WebGLRenderDevice::CreatePresentPass()
{
	std::vector<vec2> positions =
	{
		vec2(-1.0, -1.0),
		vec2( 1.0, -1.0),
		vec2(-1.0,  1.0),
		vec2(-1.0,  1.0),
		vec2( 1.0, -1.0),
		vec2( 1.0,  1.0)
	};

	PresentPass.PPStepVertexBuffer = CreateBuffer(GL_ARRAY_BUFFER, GL_STATIC_DRAW, positions.data(), positions.size() * sizeof(vec2), "PresentPass.PPStepVertexBuffer");
	PresentPass.PresentConstantBuffer = CreateBuffer(GL_UNIFORM_BUFFER, GL_STREAM_DRAW, nullptr, sizeof(WebGLPresentPushConstants), "PresentPass.PresentConstantBuffer");

	PresentPass.PPStepLayout = std::make_shared<WebGLInputLayout>();
	glBindVertexArray(PresentPass.PPStepLayout->Handle);
	glBindBuffer(GL_ARRAY_BUFFER, PresentPass.PPStepVertexBuffer->Handle);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(vec2), (const void*)offsetof(vec2, x));
	glBindVertexArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	ThrowIfWebGLError("Creating PresentPass.PPStepLayout failed");

	PresentPass.PPStep = CreateVertexShader("PresentPass.PPStep", "shaders/PPStep.vert");

	static const char* transferFunctions[2] = { nullptr, "HDR_MODE" };
	static const char* gammaModes[2] = { "GAMMA_MODE_D3D9", "GAMMA_MODE_XOPENGL" };
	static const char* colorModes[4] = { nullptr, "COLOR_CORRECT_MODE0", "COLOR_CORRECT_MODE1", "COLOR_CORRECT_MODE2" };
	for (int i = 0; i < 16; i++)
	{
		std::vector<std::string> defines;
		if (transferFunctions[i & 1]) defines.push_back(transferFunctions[i & 1]);
		if (gammaModes[(i >> 1) & 1]) defines.push_back(gammaModes[(i >> 1) & 1]);
		if (colorModes[(i >> 2) & 3]) defines.push_back(colorModes[(i >> 2) & 3]);

		PresentPass.Present[i] = CreateFragmentShader("PresentPass.Present", "shaders/Present.frag", defines);
		PresentPass.PresentProgram[i] = CreateProgram("PresentPass.PresentProgram", PresentPass.PPStep, PresentPass.Present[i]);
	}

	static const float ditherdata[64] =
	{
		.0078125, .2578125, .1328125, .3828125, .0234375, .2734375, .1484375, .3984375,
		.7578125, .5078125, .8828125, .6328125, .7734375, .5234375, .8984375, .6484375,
		.0703125, .3203125, .1953125, .4453125, .0859375, .3359375, .2109375, .4609375,
		.8203125, .5703125, .9453125, .6953125, .8359375, .5859375, .9609375, .7109375,
		.0390625, .2890625, .1640625, .4140625, .0546875, .3046875, .1796875, .4296875,
		.7890625, .5390625, .9140625, .6640625, .8046875, .5546875, .9296875, .6796875,
		.1015625, .3515625, .2265625, .4765625, .1171875, .3671875, .2421875, .4921875,
		.8515625, .6015625, .9765625, .7265625, .8671875, .6171875, .9921875, .7421875
	};

	// R32F is not filterable in WebGL 2 without OES_texture_float_linear, so store the
	// dither pattern as R8. The thresholds are multiples of 1/128 and fit in 8 bits.
	uint8_t ditherbytes[64];
	for (int i = 0; i < 64; i++)
		ditherbytes[i] = (uint8_t)std::round(ditherdata[i] * 255.0f);

	PresentPass.DitherTexture = std::make_shared<WebGLTexture2D>();
	glBindTexture(GL_TEXTURE_2D, PresentPass.DitherTexture->Handle);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, 8, 8, 0, GL_RED, GL_UNSIGNED_BYTE, ditherbytes);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
	ThrowIfWebGLError("CreateTexture2D(DitherTexture) failed");

	PresentPass.LinearSampler = std::make_shared<WebGLSampler>();
	glSamplerParameteri(PresentPass.LinearSampler->Handle, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glSamplerParameteri(PresentPass.LinearSampler->Handle, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glSamplerParameteri(PresentPass.LinearSampler->Handle, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glSamplerParameteri(PresentPass.LinearSampler->Handle, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	PresentPass.NearestSampler = std::make_shared<WebGLSampler>();
	glSamplerParameteri(PresentPass.NearestSampler->Handle, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glSamplerParameteri(PresentPass.NearestSampler->Handle, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glSamplerParameteri(PresentPass.NearestSampler->Handle, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glSamplerParameteri(PresentPass.NearestSampler->Handle, GL_TEXTURE_WRAP_T, GL_REPEAT);
	ThrowIfWebGLError("Creating present pass samplers failed");

	WebGLBlendDesc blendDesc = {};
	blendDesc.RenderTargetWriteMask = true;
	PresentPass.BlendState = CreateBlendState(blendDesc);

	WebGLDepthStencilDesc depthStencilDesc = {};
	depthStencilDesc.DepthFunc = GL_ALWAYS;
	PresentPass.DepthStencilState = CreateDepthStencilState(depthStencilDesc);

	WebGLRasterizerDesc rasterizerDesc = {};
	rasterizerDesc.CullEnable = false;
	PresentPass.RasterizerState = CreateRasterizerState(rasterizerDesc);
}

void WebGLRenderDevice::Flush(bool AllowPrecache)
{
	DrawBatches();
	ClearTextureCache();

	if (AllowPrecache && UsePrecache)
		PrecacheOnFlip = 1;
}

void WebGLRenderDevice::MapVertices(bool nextBuffer)
{
	if (nextBuffer)
	{
		// Starting over at the beginning of buffers that the draws just issued may still be
		// reading from. Orphaning the storage gives the driver fresh memory for the new
		// uploads instead of making them wait for those draws to finish.
		if ((SceneVertexPos != 0 || SceneIndexPos != 0) && ScenePass.VertexBuffer && ScenePass.IndexBuffer && ScenePass.InputLayout)
		{
			glBindBuffer(GL_ARRAY_BUFFER, ScenePass.VertexBuffer->Handle);
			glBufferData(GL_ARRAY_BUFFER, SceneVertexBufferSize * sizeof(WebGLSceneVertex), nullptr, GL_STREAM_DRAW);
			glBindVertexArray(ScenePass.InputLayout->Handle);
			glBufferData(GL_ELEMENT_ARRAY_BUFFER, SceneIndexBufferSize * sizeof(uint32_t), nullptr, GL_STREAM_DRAW);
			CheckHotPathError("Could not orphan the scene buffers");
		}
		SceneVertexPos = 0;
		SceneIndexPos = 0;
		UploadedVertexPos = 0;
		UploadedIndexPos = 0;
		Stats.BuffersUsed++;
	}

	// There is no glMapBufferRange in WebGL. The draw code writes into CPU staging
	// memory and UnmapVertices uploads what was added since the last upload.
	if (!SceneVertices)
	{
		if (StagingVertices.size() != (size_t)SceneVertexBufferSize)
			StagingVertices.resize(SceneVertexBufferSize);
		SceneVertices = StagingVertices.data();
	}

	if (!SceneIndexes)
	{
		if (StagingIndexes.size() != (size_t)SceneIndexBufferSize)
			StagingIndexes.resize(SceneIndexBufferSize);
		SceneIndexes = StagingIndexes.data();
	}
}

void WebGLRenderDevice::UnmapVertices()
{
	if (SceneVertices)
	{
		if (SceneVertexPos > UploadedVertexPos && ScenePass.VertexBuffer)
		{
			glBindBuffer(GL_ARRAY_BUFFER, ScenePass.VertexBuffer->Handle);
			glBufferSubData(GL_ARRAY_BUFFER, UploadedVertexPos * sizeof(WebGLSceneVertex), (SceneVertexPos - UploadedVertexPos) * sizeof(WebGLSceneVertex), StagingVertices.data() + UploadedVertexPos);
			CheckHotPathError("Could not upload ScenePass.VertexBuffer");
			UploadedVertexPos = SceneVertexPos;
		}
		SceneVertices = nullptr;
	}

	if (SceneIndexes)
	{
		if (SceneIndexPos > UploadedIndexPos && ScenePass.IndexBuffer && ScenePass.InputLayout)
		{
			// Bind through the VAO so the element array binding stays consistent with it
			glBindVertexArray(ScenePass.InputLayout->Handle);
			glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, UploadedIndexPos * sizeof(uint32_t), (SceneIndexPos - UploadedIndexPos) * sizeof(uint32_t), StagingIndexes.data() + UploadedIndexPos);
			CheckHotPathError("Could not upload ScenePass.IndexBuffer");
			UploadedIndexPos = SceneIndexPos;
		}
		SceneIndexes = nullptr;
	}
}

void WebGLRenderDevice::Lock(vec4 InFlashScale, vec4 InFlashFog, vec4 ScreenClear, uint8_t* InHitData, int* InHitSize)
{
	// Lock sets some state directly below, and SetRes may have recreated state objects
	ResetStateCache();

	if (Viewport->GetNativePixelWidth() != CurrentSizeX || Viewport->GetNativePixelHeight() != CurrentSizeY)
	{
		if (!SetRes(Viewport->GetNativePixelWidth(), Viewport->GetNativePixelHeight(), CurrentFullscreen))
		{
			Exception::Throw("Could not resize WebGLRenderDevice");
		}
	}

	nulltex = Textures->GetNullTexture();

	if (CurrentSizeX && CurrentSizeY)
	{
		try
		{
			ResizeSceneBuffers(CurrentSizeX, CurrentSizeY);
		}
		catch (const std::exception& e)
		{
			LogMessage(std::string("Could not resize scene buffers: ") + e.what());
			return;
		}
	}

	HitData = InHitData;
	HitSize = InHitSize;

	FlashScale = InFlashScale;
	FlashFog = InFlashFog;

	glBindFramebuffer(GL_FRAMEBUFFER, SceneBuffers.Framebuffer->Handle);

	WebGLViewport viewport = {};
	viewport.Width = (float)CurrentSizeX;
	viewport.Height = (float)CurrentSizeY;
	viewport.MaxDepth = 1.0f;
	SetViewport(viewport);

	glDisable(GL_BLEND);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glDepthMask(GL_TRUE);
	glEnable(GL_DEPTH_TEST);

	glClearColor(ScreenClear.x, ScreenClear.y, ScreenClear.z, ScreenClear.w);
	glClearDepthf(1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	glBindVertexArray(ScenePass.InputLayout->Handle);

	WebGLBuffer* cbs[1] = { ScenePass.ConstantBuffer.get() };
	SetUniformBuffers(0, 1, cbs);
	SetRasterizerState(ScenePass.RasterizerState.get());

	MapVertices(true);

	SceneConstants.HitIndex = 0;
	ForceHitIndex = -1;

	IsLocked = true;

	CheckHotPathError("Lock failed");
}

WebGLPresentPushConstants WebGLRenderDevice::GetPresentPushConstants()
{
	WebGLPresentPushConstants pushconstants;
	pushconstants.HdrScale = 0.8f + HdrScale * (3.0f / 255.0f);
	if (IsOrtho)
	{
		pushconstants.GammaCorrection = { 1.0f };
		pushconstants.Contrast = 1.0f;
		pushconstants.Saturation = 1.0f;
		pushconstants.Brightness = 0.0f;
	}
	else
	{
		float brightness = clamp(Brightness * 2.0f, 0.05f, 2.99f);

		if (GammaMode == 0)
		{
			float invGammaRed = 1.0f / std::max(brightness + GammaOffset + GammaOffsetRed, 0.001f);
			float invGammaGreen = 1.0f / std::max(brightness + GammaOffset + GammaOffsetGreen, 0.001f);
			float invGammaBlue = 1.0f / std::max(brightness + GammaOffset + GammaOffsetBlue, 0.001f);
			pushconstants.GammaCorrection = vec4(invGammaRed, invGammaGreen, invGammaBlue, 0.0f);
		}
		else
		{
			float invGammaRed = (GammaOffset + GammaOffsetRed + 2.0f) > 0.0f ? 1.0f / (GammaOffset + GammaOffsetRed + 1.0f) : 1.0f;
			float invGammaGreen = (GammaOffset + GammaOffsetGreen + 2.0f) > 0.0f ? 1.0f / (GammaOffset + GammaOffsetGreen + 1.0f) : 1.0f;
			float invGammaBlue = (GammaOffset + GammaOffsetBlue + 2.0f) > 0.0f ? 1.0f / (GammaOffset + GammaOffsetBlue + 1.0f) : 1.0f;
			pushconstants.GammaCorrection = vec4(invGammaRed, invGammaGreen, invGammaBlue, brightness);
		}

		if (Contrast >= 128)
		{
			pushconstants.Contrast = 1.0f + (Contrast - 128) / 127.0f * 3.0f;
		}
		else
		{
			pushconstants.Contrast = std::max(Contrast / 128.0f, 0.1f);
		}

		pushconstants.Saturation = 1.0f - 2.0f * (255 - Saturation) / 255.0f;

		if (LinearBrightness >= 128)
		{
			pushconstants.Brightness = (LinearBrightness - 128) / 127.0f * 1.8f;
		}
		else
		{
			pushconstants.Brightness = (128 - LinearBrightness) / 128.0f * -1.8f;
		}
	}
	return pushconstants;
}

int WebGLRenderDevice::GetPresentShaderIndex(const WebGLPresentPushConstants& pushconstants)
{
	// Select present shader based on what the user is actually using
	int presentShader = 0;
	if (ActiveHdr) presentShader |= 1;
	if (GammaMode == 1) presentShader |= 2;
	if (pushconstants.Brightness != 0.0f || pushconstants.Contrast != 1.0f || pushconstants.Saturation != 1.0f) presentShader |= (clamp(GrayFormula, 0, 2) + 1) << 2;
	return presentShader;
}

void WebGLRenderDevice::Unlock(bool Blit)
{
	if (!IsLocked) // Don't trust the engine.
		return;

	DrawBatches();
	UnmapVertices();

	CheckHotPathError("Drawing failed");

	Batch.SceneIndexStart = 0;
	SceneVertexPos = 0;
	SceneIndexPos = 0;

	if (Blit)
	{
		glBindFramebuffer(GL_READ_FRAMEBUFFER, SceneBuffers.Framebuffer->Handle);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, SceneBuffers.PPFramebuffer[0]->Handle);
		glBlitFramebuffer(0, 0, CurrentSizeX, CurrentSizeY, 0, 0, CurrentSizeX, CurrentSizeY, GL_COLOR_BUFFER_BIT, GL_NEAREST);

		if (Bloom)
		{
			RunBloomPass();
		}

		WebGLPresentPushConstants pushconstants = GetPresentPushConstants();
		int presentShader = GetPresentShaderIndex(pushconstants);

		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		UseProgram(PresentPass.PresentProgram[presentShader]->Handle);

		WebGLViewport viewport = {};
		viewport.Width = (float)CurrentSizeX;
		viewport.Height = (float)CurrentSizeY;
		viewport.MaxDepth = 1.0f;
		SetViewport(viewport);

		glBindVertexArray(PresentPass.PPStepLayout->Handle);

		SetBufferData(GL_UNIFORM_BUFFER, GL_STREAM_DRAW, PresentPass.PresentConstantBuffer.get(), &pushconstants, sizeof(WebGLPresentPushConstants));

		SetRasterizerState(PresentPass.RasterizerState.get());
		WebGLBuffer* cbs[1] = { PresentPass.PresentConstantBuffer.get() };
		SetUniformBuffers(0, 1, cbs);
		WebGLTexture2D* psResources[] = { SceneBuffers.PPImage[0].get(), PresentPass.DitherTexture.get() };
		WebGLSampler* psSamplers[] = { PresentPass.LinearSampler.get(), PresentPass.NearestSampler.get() };
		SetTextures(0, 2, psResources);
		SetSamplers(0, 2, psSamplers);
		SetDepthStencilState(PresentPass.DepthStencilState.get());
		SetBlendState(PresentPass.BlendState.get());
		glDrawArrays(GL_TRIANGLES, 0, 6);

		// The browser composites the canvas on its own schedule; swap interval is a no-op in Emscripten.
		Viewport->SwapGLBuffers();

		Batch.Pipeline = nullptr;
		Batch.Tex = nullptr;
		Batch.Lightmap = nullptr;
		Batch.Detailtex = nullptr;
		Batch.Macrotex = nullptr;
		Batch.SceneIndexStart = 0;

		UpdateLODBias();
	}

	if (HitData)
	{
		// Hit testing is not implemented in this backend (the scene framebuffer has no hit
		// index attachment). Report no hit rather than leaving the output untouched.
		if (HitSize)
			*HitSize = 0;
	}

	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	HitQueryStack.clear();
	HitQueries.clear();
	HitBuffer.clear();
	HitData = nullptr;
	HitSize = nullptr;

	IsLocked = false;

	// GL errors are sticky until read, so polling every few dozen frames still catches
	// everything that went wrong in between, without a GPU-process round trip per frame.
	if (ValidateEveryCall || (++FramesSinceErrorCheck & 63) == 0)
		ThrowIfWebGLError("WebGL error during the last frames");
}

void WebGLRenderDevice::PushHit(const uint8_t* Data, int Count)
{
	if (Count <= 0) return;
	HitQueryStack.insert(HitQueryStack.end(), Data, Data + Count);

	SetHitLocation();
}

void WebGLRenderDevice::PopHit(int Count, bool bForce)
{
	if (bForce) // Force hit what we are popping
		ForceHitIndex = (int)(HitQueries.size() - 1);

	HitQueryStack.resize(HitQueryStack.size() - Count);

	SetHitLocation();
}

void WebGLRenderDevice::SetHitLocation()
{
	DrawBatches();

	if (!HitQueryStack.empty())
	{
		int index = (int)HitQueries.size();

		HitQuery query;
		query.Start = (int)HitBuffer.size();
		query.Count = (int)HitQueryStack.size();
		HitQueries.push_back(query);

		HitBuffer.insert(HitBuffer.end(), HitQueryStack.begin(), HitQueryStack.end());

		SceneConstants.HitIndex = index + 1;
	}
	else
	{
		SceneConstants.HitIndex = 0;
	}

	SetBufferData(GL_UNIFORM_BUFFER, GL_STREAM_DRAW, ScenePass.ConstantBuffer.get(), &SceneConstants, sizeof(WebGLScenePushConstants));
}

void WebGLRenderDevice::DrawComplexSurface(SceneNode* Frame, SurfaceInfo& Surface, SurfaceFacet& Facet)
{
	uint32_t PolyFlags = ApplyPrecedenceRules(Surface.PolyFlags);

	ComplexSurfaceInfo info;
	info.facet = &Facet;
	info.tex = Textures->GetTexture(Surface.Texture, !!(PolyFlags & PF_Masked));
	info.lightmap = Textures->GetTexture(Surface.LightMap, false);
	info.macrotex = Textures->GetTexture(Surface.MacroTexture, false);
	info.detailtex = Textures->GetTexture(Surface.DetailTexture, false);
	info.fogmap = (Surface.FogMap && Surface.FogMap->NumMips > 0 && !Surface.FogMap->Mips[0].Data.empty()) ?
		Textures->GetTexture(Surface.FogMap, false) : nulltex;
	info.editorcolor = nullptr;

	if (Surface.DetailTexture && Surface.FogMap) info.detailtex = nulltex;

	if (info.fogmap != nulltex)
		info.detailtex = info.fogmap;

	SetPipeline(PolyFlags);
	SetDescriptorSet(PolyFlags, info);

	DrawComplexSurfaceFaces(info);

	Stats.ComplexSurfaces++;
}

void WebGLRenderDevice::DrawComplexSurfaceFaces(const ComplexSurfaceInfo& info)
{
	if (info.facet->VertexCount < 3)
		return;

	uint32_t flags = 0;
	if (info.lightmap != nulltex) flags |= 1;
	if (info.macrotex != nulltex) flags |= 2;
	if (info.detailtex != nulltex && info.fogmap == nulltex) flags |= 4;
	if (info.fogmap != nulltex) flags |= 8;
	if (LightMode == 1) flags |= 64;

	vec3 xaxis = info.facet->MapCoords.XAxis;
	vec3 yaxis = info.facet->MapCoords.YAxis;
	float UDot = dot(xaxis, info.facet->MapCoords.Origin);
	float VDot = dot(yaxis, info.facet->MapCoords.Origin);

	float UPan = UDot + info.tex->PanX;
	float VPan = VDot + info.tex->PanY;
	float LMUPan = UDot + info.lightmap->PanX - 0.5f * info.lightmap->UScale;
	float LMVPan = VDot + info.lightmap->PanY - 0.5f * info.lightmap->VScale;
	float MacroUPan = UDot + info.macrotex->PanX;
	float MacroVPan = VDot + info.macrotex->PanY;
	float DetailUPan = UDot + (info.fogmap == nulltex ? info.detailtex->PanX : info.fogmap->PanX - 0.5f * info.fogmap->UScale);
	float DetailVPan = VDot + (info.fogmap == nulltex ? info.detailtex->PanY : info.fogmap->PanY - 0.5f * info.fogmap->VScale);

	float UMult = info.tex->UMult;
	float VMult = info.tex->VMult;
	float LMUMult = info.lightmap->UMult;
	float LMVMult = info.lightmap->VMult;
	float MacroUMult = info.macrotex->UMult;
	float MacroVMult = info.macrotex->VMult;
	float DetailUMult = info.fogmap == nulltex ? info.detailtex->UMult : info.fogmap->UMult;
	float DetailVMult = info.fogmap == nulltex ? info.detailtex->VMult : info.fogmap->VMult;

	vec4 color = info.editorcolor ? *info.editorcolor : vec4(1.0f);

	auto pts = info.facet->Vertices;
	uint32_t vcount = info.facet->VertexCount;

	uint32_t icount = (vcount - 2) * 3;
	auto alloc = ReserveVertices(vcount, icount);
	if (alloc.vptr)
	{
		WebGLSceneVertex* vptr = alloc.vptr;
		uint32_t* iptr = alloc.iptr;
		uint32_t vpos = alloc.vpos;

		for (uint32_t i = 0; i < vcount; i++)
		{
			vec3 point = pts[i];
			float u = dot(xaxis, point);
			float v = dot(yaxis, point);

			vptr->Flags = flags;
			vptr->Position.x = point.x;
			vptr->Position.y = point.y;
			vptr->Position.z = point.z;
			vptr->TexCoord.s = (u - UPan) * UMult;
			vptr->TexCoord.t = (v - VPan) * VMult;
			vptr->TexCoord2.s = (u - LMUPan) * LMUMult;
			vptr->TexCoord2.t = (v - LMVPan) * LMVMult;
			vptr->TexCoord3.s = (u - MacroUPan) * MacroUMult;
			vptr->TexCoord3.t = (v - MacroVPan) * MacroVMult;
			vptr->TexCoord4.s = (u - DetailUPan) * DetailUMult;
			vptr->TexCoord4.t = (v - DetailVPan) * DetailVMult;
			vptr->Color = color;
			vptr++;
		}

		for (uint32_t i = vpos + 2; i < vpos + vcount; i++)
		{
			*(iptr++) = vpos;
			*(iptr++) = i - 1;
			*(iptr++) = i;
		}

		UseVertices(vcount, icount);
	}
}

void WebGLRenderDevice::DrawGouraudPolygon(SceneNode* Frame, TextureInfo& Info, const GouraudVertex* Pts, int NumPts, uint32_t PolyFlags)
{
	if (NumPts < 3) return; // This can apparently happen!!

	PolyFlags = ApplyPrecedenceRules(PolyFlags);

	WebGLCachedTexture* tex = Textures->GetTexture(&Info, !!(PolyFlags & PF_Masked));

	SetPipeline(PolyFlags);
	SetDescriptorSet(PolyFlags, tex);

	float UMult = tex->UMult;
	float VMult = tex->VMult;

	int flags = (PolyFlags & (PF_RenderFog | PF_Translucent | PF_Modulated)) == PF_RenderFog ? 16 : 0;
	if ((PolyFlags & (PF_Translucent | PF_Modulated)) == 0 && LightMode == 2) flags |= 32;

	auto alloc = ReserveVertices(NumPts, (NumPts - 2) * 3);
	if (alloc.vptr)
	{
		WebGLSceneVertex* vptr = alloc.vptr;
		uint32_t* iptr = alloc.iptr;
		uint32_t vpos = alloc.vpos;

		bool modulated = !!(PolyFlags & PF_Modulated);
		WebGLSceneVertex* vertex = vptr;
		for (int i = 0; i < NumPts; i++)
		{
			const GouraudVertex* P = Pts + i;
			vertex->Flags = flags;
			vertex->Position.x = P->Point.x;
			vertex->Position.y = P->Point.y;
			vertex->Position.z = P->Point.z;
			vertex->TexCoord.s = P->UV.x * UMult;
			vertex->TexCoord.t = P->UV.y * VMult;
			vertex->TexCoord2.s = P->Fog.x;
			vertex->TexCoord2.t = P->Fog.y;
			vertex->TexCoord3.s = P->Fog.z;
			vertex->TexCoord3.t = P->Fog.w;
			vertex->TexCoord4.s = 0.0f;
			vertex->TexCoord4.t = 0.0f;
			if (modulated)
			{
				vertex->Color.r = 1.0f;
				vertex->Color.g = 1.0f;
				vertex->Color.b = 1.0f;
			}
			else
			{
				vertex->Color.r = P->Light.x;
				vertex->Color.g = P->Light.y;
				vertex->Color.b = P->Light.z;
			}
			vertex->Color.a = 1.0f;
			vertex++;
		}

		uint32_t vstart = vpos;
		uint32_t vcount = NumPts;
		for (uint32_t i = vstart + 2; i < vstart + vcount; i++)
		{
			*(iptr++) = vstart;
			*(iptr++) = i - 1;
			*(iptr++) = i;
		}

		UseVertices(NumPts, (NumPts - 2) * 3);
	}

	Stats.GouraudPolygons++;
}

void WebGLRenderDevice::DrawTile(SceneNode* Frame, TextureInfo& Info, float X, float Y, float XL, float YL, float U, float V, float UL, float VL, float Z, vec4 Color, vec4 Fog, uint32_t PolyFlags)
{
	PolyFlags = ApplyPrecedenceRules(PolyFlags);

	if (PolyFlags == PF_SubpixelFont)
	{
		AddDrawBatch();
		Batch.BlendConstants[0] = Color.x;
		Batch.BlendConstants[1] = Color.y;
		Batch.BlendConstants[2] = Color.z;
		Batch.BlendConstants[3] = Color.w;
		Color = vec4(1.0f);
	}

	WebGLCachedTexture* tex = Textures->GetTexture(&Info, !!(PolyFlags & PF_Masked));
	float UMult = tex->UMult;
	float VMult = tex->VMult;
	float u0 = U * UMult;
	float v0 = V * VMult;
	float u1 = (U + UL) * UMult;
	float v1 = (V + VL) * VMult;
	bool clamp = (u0 >= 0.0f && u1 <= 1.00001f && v0 >= 0.0f && v1 <= 1.00001f);

	SetPipeline(PolyFlags);
	SetDescriptorSet(PolyFlags, tex, clamp);

	auto alloc = ReserveVertices(4, 6);
	if (alloc.vptr)
	{
		WebGLSceneVertex* vptr = alloc.vptr;
		uint32_t* iptr = alloc.iptr;
		uint32_t vpos = alloc.vpos;

		float r, g, b, a;
		if (PolyFlags & PF_Modulated)
		{
			r = 1.0f;
			g = 1.0f;
			b = 1.0f;
		}
		else
		{
			r = Color.x;
			g = Color.y;
			b = Color.z;
		}
		a = 1.0f;

		float rfx2z = RFX2 * Z;
		float rfy2z = RFY2 * Z;
		X -= Frame->FX2;
		Y -= Frame->FY2;
		XL += X;
		YL += Y;
		U *= UMult;
		UL = U + UL * UMult;
		V *= VMult;
		VL = V + VL * VMult;

		vec2 zero2(0.0f);
		vec4 color(r, g, b, a);
		vptr[0] = { 0, vec3(rfx2z * X, rfy2z * Y, Z), vec2(U, V), zero2, zero2, zero2, color };
		vptr[1] = { 0, vec3(rfx2z * XL, rfy2z * Y, Z), vec2(UL, V), zero2, zero2, zero2, color };
		vptr[2] = { 0, vec3(rfx2z * XL, rfy2z * YL, Z), vec2(UL, VL), zero2, zero2, zero2, color };
		vptr[3] = { 0, vec3(rfx2z * X, rfy2z * YL, Z), vec2(U, VL), zero2, zero2, zero2, color };

		iptr[0] = vpos;
		iptr[1] = vpos + 1;
		iptr[2] = vpos + 2;
		iptr[3] = vpos;
		iptr[4] = vpos + 2;
		iptr[5] = vpos + 3;

		UseVertices(4, 6);
	}

	Stats.Tiles++;
}

vec4 WebGLRenderDevice::ApplyInverseGamma(vec4 color)
{
	if (IsOrtho)
		return color;
	float brightness = clamp(Brightness * 2.0f, 0.05f, 2.99f);
	float gammaRed = std::max(brightness + GammaOffset + GammaOffsetRed, 0.001f);
	float gammaGreen = std::max(brightness + GammaOffset + GammaOffsetGreen, 0.001f);
	float gammaBlue = std::max(brightness + GammaOffset + GammaOffsetBlue, 0.001f);
	return vec4(pow(color.r, gammaRed), pow(color.g, gammaGreen), pow(color.b, gammaBlue), color.a);
}

void WebGLRenderDevice::Draw3DLine(SceneNode* Frame, vec4 Color, uint32_t LineFlags, vec3 P1, vec3 P2)
{
	if (IsOrtho)
	{
		P1.x = (P1.x) / Frame->Zoom + Frame->FX2;
		P1.y = (P1.y) / Frame->Zoom + Frame->FY2;
		P1.z = 1;
		P2.x = (P2.x) / Frame->Zoom + Frame->FX2;
		P2.y = (P2.y) / Frame->Zoom + Frame->FY2;
		P2.z = 1;

		if (std::abs(P2.x - P1.x) + std::abs(P2.y - P1.y) >= 0.2)
		{
			Draw2DLine(Frame, Color, LineFlags, P1, P2);
		}
		else if (IsOrthoLowDetail)
		{
			Draw2DPoint(Frame, Color, LINE_None, P1.x - 1, P1.y - 1, P1.x + 1, P1.y + 1, P1.z);
		}
	}
	else
	{
		bool occlude = !!(LineFlags & LINE_DepthCued);
		SetPipeline(&ScenePass.LinePipeline[occlude]);
		SetDescriptorSet(PF_Highlighted);
		vec4 color = ApplyInverseGamma(vec4(Color.x, Color.y, Color.z, 1.0f));

		auto alloc = ReserveVertices(2, 2);
		if (alloc.vptr)
		{
			WebGLSceneVertex* vptr = alloc.vptr;
			uint32_t* iptr = alloc.iptr;
			uint32_t vpos = alloc.vpos;

			vptr[0] = { 0, vec3(P1.x, P1.y, P1.z), vec2(0.0f), vec2(0.0f), vec2(0.0f), vec2(0.0f), color };
			vptr[1] = { 0, vec3(P2.x, P2.y, P2.z), vec2(0.0f), vec2(0.0f), vec2(0.0f), vec2(0.0f), color };

			iptr[0] = vpos;
			iptr[1] = vpos + 1;

			UseVertices(2, 2);
		}
	}
}

void WebGLRenderDevice::Draw2DLine(SceneNode* Frame, vec4 Color, uint32_t LineFlags, vec3 P1, vec3 P2)
{
	bool occlude = !!(LineFlags & LINE_DepthCued);
	SetPipeline(&ScenePass.LinePipeline[occlude]);
	SetDescriptorSet(PF_Highlighted);
	vec4 color = ApplyInverseGamma(vec4(Color.x, Color.y, Color.z, 1.0f));

	auto alloc = ReserveVertices(2, 2);
	if (alloc.vptr)
	{
		WebGLSceneVertex* vptr = alloc.vptr;
		uint32_t* iptr = alloc.iptr;
		uint32_t vpos = alloc.vpos;

		vptr[0] = { 0, vec3(RFX2 * P1.z * (P1.x - Frame->FX2), RFY2 * P1.z * (P1.y - Frame->FY2), P1.z), vec2(0.0f), vec2(0.0f), vec2(0.0f), vec2(0.0f), color };
		vptr[1] = { 0, vec3(RFX2 * P2.z * (P2.x - Frame->FX2), RFY2 * P2.z * (P2.y - Frame->FY2), P2.z), vec2(0.0f), vec2(0.0f), vec2(0.0f), vec2(0.0f), color };

		iptr[0] = vpos;
		iptr[1] = vpos + 1;

		UseVertices(2, 2);
	}
}

void WebGLRenderDevice::Draw2DPoint(SceneNode* Frame, vec4 Color, uint32_t LineFlags, float X1, float Y1, float X2, float Y2, float Z)
{
	bool occlude = !!(LineFlags & LINE_DepthCued);
	SetPipeline(&ScenePass.PointPipeline[occlude]);
	SetDescriptorSet(PF_Highlighted);
	vec4 color = ApplyInverseGamma(vec4(Color.x, Color.y, Color.z, 1.0f));

	auto alloc = ReserveVertices(4, 6);
	if (alloc.vptr)
	{
		WebGLSceneVertex* vptr = alloc.vptr;
		uint32_t* iptr = alloc.iptr;
		uint32_t vpos = alloc.vpos;

		vptr[0] = { 0, vec3(RFX2 * Z * (X1 - Frame->FX2 - 0.5f), RFY2 * Z * (Y1 - Frame->FY2 - 0.5f), Z), vec2(0.0f), vec2(0.0f), vec2(0.0f), vec2(0.0f), color };
		vptr[1] = { 0, vec3(RFX2 * Z * (X2 - Frame->FX2 + 0.5f), RFY2 * Z * (Y1 - Frame->FY2 - 0.5f), Z), vec2(0.0f), vec2(0.0f), vec2(0.0f), vec2(0.0f), color };
		vptr[2] = { 0, vec3(RFX2 * Z * (X2 - Frame->FX2 + 0.5f), RFY2 * Z * (Y2 - Frame->FY2 + 0.5f), Z), vec2(0.0f), vec2(0.0f), vec2(0.0f), vec2(0.0f), color };
		vptr[3] = { 0, vec3(RFX2 * Z * (X1 - Frame->FX2 - 0.5f), RFY2 * Z * (Y2 - Frame->FY2 + 0.5f), Z), vec2(0.0f), vec2(0.0f), vec2(0.0f), vec2(0.0f), color };

		iptr[0] = vpos;
		iptr[1] = vpos + 1;
		iptr[2] = vpos + 2;
		iptr[3] = vpos;
		iptr[4] = vpos + 2;
		iptr[5] = vpos + 3;

		UseVertices(4, 6);
	}
}

void WebGLRenderDevice::ClearZ()
{
	DrawBatches();

	glDepthMask(GL_TRUE);
	StateCache.DepthStencilState = nullptr; // depth mask changed behind the cache's back
	glClearDepthf(1.0f);
	glClear(GL_DEPTH_BUFFER_BIT);
}

void WebGLRenderDevice::ReadPixels(TextureColor* Pixels)
{
	UnmapVertices();

	WebGLFramebuffer* source = SceneBuffers.PPFramebuffer[0].get();

	if (GammaCorrectScreenshots)
	{
		WebGLPresentPushConstants pushconstants = GetPresentPushConstants();
		int presentShader = GetPresentShaderIndex(pushconstants);

		glBindFramebuffer(GL_FRAMEBUFFER, SceneBuffers.PPFramebuffer[1]->Handle);
		UseProgram(PresentPass.PresentProgram[presentShader]->Handle);

		WebGLViewport viewport = {};
		viewport.Width = (float)CurrentSizeX;
		viewport.Height = (float)CurrentSizeY;
		viewport.MaxDepth = 1.0f;
		SetViewport(viewport);

		glBindVertexArray(PresentPass.PPStepLayout->Handle);

		SetBufferData(GL_UNIFORM_BUFFER, GL_STREAM_DRAW, PresentPass.PresentConstantBuffer.get(), &pushconstants, sizeof(WebGLPresentPushConstants));

		WebGLBuffer* cbs[1] = { PresentPass.PresentConstantBuffer.get() };
		WebGLTexture2D* psResources[] = { SceneBuffers.PPImage[0].get(), PresentPass.DitherTexture.get() };
		WebGLSampler* psSamplers[] = { PresentPass.LinearSampler.get(), PresentPass.NearestSampler.get() };
		SetRasterizerState(PresentPass.RasterizerState.get());
		SetUniformBuffers(0, 1, cbs);
		SetTextures(0, 2, psResources);
		SetSamplers(0, 2, psSamplers);
		SetDepthStencilState(PresentPass.DepthStencilState.get());
		SetBlendState(PresentPass.BlendState.get());
		glDrawArrays(GL_TRIANGLES, 0, 6);

		source = SceneBuffers.PPFramebuffer[1].get();
	}

	int w = CurrentSizeX;
	int h = CurrentSizeY;
	glBindFramebuffer(GL_READ_FRAMEBUFFER, source->Handle);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	if (HasColorBufferFloat)
	{
		std::vector<float> pixels((size_t)w * h * 4);
		glReadPixels(0, 0, w, h, GL_RGBA, GL_FLOAT, pixels.data());
		for (int y = 0; y < h; y++)
		{
			int desty = GammaCorrectScreenshots ? y : (h - y - 1);
			TextureColor* dest = Pixels + (size_t)desty * w;
			const float* src = pixels.data() + (size_t)y * w * 4;
			for (int x = 0; x < w; x++)
			{
				dest->R = (uint8_t)clamp(std::round(src[0] * 255.0f), 0.0f, 255.0f);
				dest->G = (uint8_t)clamp(std::round(src[1] * 255.0f), 0.0f, 255.0f);
				dest->B = (uint8_t)clamp(std::round(src[2] * 255.0f), 0.0f, 255.0f);
				dest->A = (uint8_t)clamp(std::round(src[3] * 255.0f), 0.0f, 255.0f);
				dest++;
				src += 4;
			}
		}
	}
	else
	{
		std::vector<TextureColor> pixels((size_t)w * h);
		glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
		for (int y = 0; y < h; y++)
		{
			int desty = GammaCorrectScreenshots ? y : (h - y - 1);
			memcpy(Pixels + (size_t)desty * w, pixels.data() + (size_t)y * w, (size_t)w * sizeof(TextureColor));
		}
	}
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	glBindFramebuffer(GL_FRAMEBUFFER, IsLocked ? SceneBuffers.Framebuffer->Handle : 0);
	while (glGetError() != GL_NO_ERROR) // a failed screenshot must not take the renderer down
		;

	if (IsLocked)
	{
		glBindVertexArray(ScenePass.InputLayout->Handle);
		SetViewport(SceneViewport);
		MapVertices(false);
	}
}

void WebGLRenderDevice::EndFlash()
{
	if (FlashScale != vec4(0.5f, 0.5f, 0.5f, 0.0f) || FlashFog != vec4(0.0f, 0.0f, 0.0f, 0.0f))
	{
		DrawBatches();

		vec4 color(FlashFog.x, FlashFog.y, FlashFog.z, 1.0f - std::min(FlashScale.x * 2.0f, 1.0f));
		vec2 zero2(0.0f);

		SceneConstants.ObjectToProjection = mat4::identity();
		SceneConstants.ObjectToView = mat4::identity();
		SceneConstants.NearClip = vec4(0.0f, 0.0f, 0.0f, 0.0f);

		SetBufferData(GL_UNIFORM_BUFFER, GL_STREAM_DRAW, ScenePass.ConstantBuffer.get(), &SceneConstants, sizeof(WebGLScenePushConstants));

		SetPipeline(PF_Highlighted);
		SetDescriptorSet(0);

		auto alloc = ReserveVertices(4, 6);
		if (alloc.vptr)
		{
			WebGLSceneVertex* vptr = alloc.vptr;
			uint32_t* iptr = alloc.iptr;
			uint32_t vpos = alloc.vpos;

			vptr[0] = { 0, vec3(-1.0f, -1.0f, 0.0f), zero2, zero2, zero2, zero2, color };
			vptr[1] = { 0, vec3(1.0f, -1.0f, 0.0f), zero2, zero2, zero2, zero2, color };
			vptr[2] = { 0, vec3(1.0f,  1.0f, 0.0f), zero2, zero2, zero2, zero2, color };
			vptr[3] = { 0, vec3(-1.0f,  1.0f, 0.0f), zero2, zero2, zero2, zero2, color };

			iptr[0] = vpos;
			iptr[1] = vpos + 1;
			iptr[2] = vpos + 2;
			iptr[3] = vpos;
			iptr[4] = vpos + 2;
			iptr[5] = vpos + 3;

			UseVertices(4, 6);
		}

		DrawBatches();
		if (CurrentFrame)
			SetSceneNode(CurrentFrame);
	}
}

void WebGLRenderDevice::SetSceneNode(SceneNode* Frame)
{
	DrawBatches();

	CurrentFrame = Frame;
	Aspect = Frame->FY / Frame->FX;
	RProjZ = (float)std::tan(radians(Frame->FovAngle) * 0.5);
	RFX2 = 2.0f * RProjZ / Frame->FX;
	RFY2 = 2.0f * RProjZ * Aspect / Frame->FY;

	SceneViewport = {};
	SceneViewport.X = (float)Frame->XB;
	SceneViewport.Y = (float)(SceneBuffers.Height - Frame->YB - Frame->Y);
	SceneViewport.Width = (float)Frame->X;
	SceneViewport.Height = (float)Frame->Y;
	SceneViewport.MinDepth = 0.1f;
	SceneViewport.MaxDepth = 1.0f;
	SetViewport(SceneViewport);

	SceneConstants.ObjectToProjection = mat4::frustum(-RProjZ, RProjZ, -Aspect * RProjZ, Aspect * RProjZ, 1.0f, 32768.0f, handedness::left, clipzrange::negative_positive_w);
	SceneConstants.ObjectToProjection = SceneConstants.ObjectToProjection * Frame->WorldToView * Frame->ObjectToWorld;

	SceneConstants.ObjectToView = Frame->WorldToView * Frame->ObjectToWorld;
	SceneConstants.NearClip = Frame->NearClip;

	SetBufferData(GL_UNIFORM_BUFFER, GL_STREAM_DRAW, ScenePass.ConstantBuffer.get(), &SceneConstants, sizeof(WebGLScenePushConstants));
}

void WebGLRenderDevice::SetBufferData(GLenum target, GLenum usage, WebGLBuffer* buffer, const void* data, size_t size)
{
	glBindBuffer(target, buffer->Handle);
	glBufferData(target, size, data, usage);
}

void WebGLRenderDevice::ResetStateCache()
{
	StateCache = GLStateCache();
	for (GLuint& texture : StateCache.Textures)
		texture = GLStateCache::Unknown;
	for (GLuint& sampler : StateCache.Samplers)
		sampler = GLStateCache::Unknown;
}

void WebGLRenderDevice::UseProgram(GLuint program)
{
	if (StateCache.Program == program)
		return;
	StateCache.Program = program;
	glUseProgram(program);
}

void WebGLRenderDevice::SetBlendState(WebGLBlendState* blendState, const float* blendConstants)
{
	if (StateCache.BlendState != blendState)
	{
		StateCache.BlendState = blendState;

		const WebGLBlendDesc& desc = blendState->desc;
		GLboolean maskValue = desc.RenderTargetWriteMask ? GL_TRUE : GL_FALSE;
		glColorMask(maskValue, maskValue, maskValue, maskValue);
		if (desc.BlendEnable)
		{
			glEnable(GL_BLEND);
			glBlendEquationSeparate(desc.BlendOp, desc.BlendOpAlpha);
			glBlendFuncSeparate(desc.SrcBlend, desc.DestBlend, desc.SrcBlendAlpha, desc.DestBlendAlpha);
		}
		else
		{
			glDisable(GL_BLEND);
		}
	}

	if (blendConstants)
	{
		bool changed = !StateCache.HasBlendConstants;
		for (int i = 0; i < 4 && !changed; i++)
			changed = StateCache.BlendConstants[i] != blendConstants[i];
		if (changed)
		{
			StateCache.HasBlendConstants = true;
			for (int i = 0; i < 4; i++)
				StateCache.BlendConstants[i] = blendConstants[i];
			glBlendColor(blendConstants[0], blendConstants[1], blendConstants[2], blendConstants[3]);
		}
	}
}

void WebGLRenderDevice::SetDepthStencilState(WebGLDepthStencilState* depthStencilState)
{
	if (StateCache.DepthStencilState == depthStencilState)
		return;
	StateCache.DepthStencilState = depthStencilState;

	if (depthStencilState->desc.DepthEnable)
		glEnable(GL_DEPTH_TEST);
	else
		glDisable(GL_DEPTH_TEST);

	glDepthFunc(depthStencilState->desc.DepthFunc);
	glDepthMask(depthStencilState->desc.DepthWriteMask ? GL_TRUE : GL_FALSE);
}

void WebGLRenderDevice::SetRasterizerState(WebGLRasterizerState* rasterizerState)
{
	if (StateCache.RasterizerState == rasterizerState)
		return;
	StateCache.RasterizerState = rasterizerState;

	// No GL_DEPTH_CLAMP or GL_MULTISAMPLE toggles in GLES. Depth clamping was disabled
	// in the desktop backend anyway (UE1 clips geometry itself).
	if (rasterizerState->desc.CullEnable)
		glEnable(GL_CULL_FACE);
	else
		glDisable(GL_CULL_FACE);

	glFrontFace(rasterizerState->desc.FrontCounterClockwise ? GL_CCW : GL_CW);
}

void WebGLRenderDevice::SetUniformBuffers(int start, int count, WebGLBuffer** buffers)
{
	for (int i = 0; i < count; i++)
	{
		glBindBufferBase(GL_UNIFORM_BUFFER, start + i, buffers[i] ? buffers[i]->Handle : 0);
	}
}

void WebGLRenderDevice::SetTextures(int start, int count, WebGLTexture** textures)
{
	int activeUnit = 0;
	for (int i = 0; i < count; i++)
	{
		GLuint handle = textures[i] ? textures[i]->Handle : 0;
		if (StateCache.Textures[start + i] == handle)
			continue;
		StateCache.Textures[start + i] = handle;

		activeUnit = start + i;
		glActiveTexture(GL_TEXTURE0 + activeUnit);
		glBindTexture(GL_TEXTURE_2D, handle);
	}

	// Leave unit 0 active: texture uploads bind to the active unit
	if (activeUnit != 0)
		glActiveTexture(GL_TEXTURE0);
}

void WebGLRenderDevice::SetSamplers(int start, int count, WebGLSampler** samplers)
{
	for (int i = 0; i < count; i++)
	{
		GLuint handle = samplers[i] ? samplers[i]->Handle : 0;
		if (StateCache.Samplers[start + i] == handle)
			continue;
		StateCache.Samplers[start + i] = handle;

		glBindSampler(start + i, handle);
	}
}

void WebGLRenderDevice::SetViewport(const WebGLViewport& viewport)
{
	glViewport((GLint)std::round(viewport.X), (GLint)std::round(viewport.Y), (GLsizei)std::round(viewport.Width), (GLsizei)std::round(viewport.Height));
	glDepthRangef(viewport.MinDepth, viewport.MaxDepth);
}

void WebGLRenderDevice::PrecacheTexture(TextureInfo& Info, uint32_t PolyFlags)
{
	PolyFlags = ApplyPrecedenceRules(PolyFlags);
	Textures->GetTexture(&Info, !!(PolyFlags & PF_Masked));
}

void WebGLRenderDevice::ClearTextureCache()
{
	Textures->ClearCache();

	// Deleted texture names get recycled by glGenTextures, so cached unit bindings are no longer trustworthy
	ResetStateCache();
}

bool WebGLRenderDevice::SupportsTextureFormat(TextureFormat Format)
{
	return Uploads->SupportsTextureFormat(Format);
}

void WebGLRenderDevice::UpdateTextureRect(TextureInfo& Info, int U, int V, int UL, int VL)
{
	Textures->UpdateTextureRect(&Info, U, V, UL, VL);
}

void WebGLRenderDevice::AddDrawBatch()
{
	if (Batch.SceneIndexStart != SceneIndexPos)
	{
		Batch.SceneIndexEnd = SceneIndexPos;
		QueuedBatches.push_back(Batch);
		Batch.SceneIndexStart = SceneIndexPos;
	}
}

void WebGLRenderDevice::DrawBatches(bool nextBuffer)
{
	AddDrawBatch();

	CheckHotPathError("DrawBatches failed (before draw)");

	UnmapVertices();

	if (!QueuedBatches.empty())
	{
		glBindVertexArray(ScenePass.InputLayout->Handle);
		for (const DrawBatchEntry& entry : QueuedBatches)
			DrawEntry(entry);
		QueuedBatches.clear();
	}

	CheckHotPathError("DrawBatches failed (after draw)");

	MapVertices(nextBuffer);

	Batch.SceneIndexStart = SceneIndexPos;
}

void WebGLRenderDevice::DrawEntry(const DrawBatchEntry& entry)
{
	size_t icount = entry.SceneIndexEnd - entry.SceneIndexStart;

	WebGLTexture2D* views[4] =
	{
		entry.Tex->Texture.get(),
		entry.Lightmap->Texture.get(),
		entry.Macrotex->Texture.get(),
		entry.Detailtex->Texture.get()
	};

	WebGLSampler* samplers[4] =
	{
		ScenePass.Samplers[entry.TexSamplerMode].get(),
		ScenePass.Samplers[0].get(),
		ScenePass.Samplers[entry.MacrotexSamplerMode].get(),
		ScenePass.Samplers[entry.DetailtexSamplerMode].get()
	};

	if (SceneViewport.MinDepth != entry.Pipeline->MinDepth || SceneViewport.MaxDepth != entry.Pipeline->MaxDepth)
	{
		SceneViewport.MinDepth = entry.Pipeline->MinDepth;
		SceneViewport.MaxDepth = entry.Pipeline->MaxDepth;
		SetViewport(SceneViewport);
	}

	UseProgram(entry.Pipeline->ShaderProgram->Handle);
	SetSamplers(0, 4, samplers);
	SetTextures(0, 4, views);
	SetBlendState(entry.Pipeline->BlendState.get(), entry.BlendConstants);
	SetDepthStencilState(entry.Pipeline->DepthStencilState.get());

	glDrawElements(entry.Pipeline->PrimitiveTopology, (GLsizei)icount, GL_UNSIGNED_INT, (void*)(entry.SceneIndexStart * sizeof(uint32_t)));
	Stats.DrawCalls++;
}

std::shared_ptr<WebGLBlendState> WebGLRenderDevice::CreateBlendState(const WebGLBlendDesc& desc)
{
	return std::make_shared<WebGLBlendState>(desc);
}

std::shared_ptr<WebGLRasterizerState> WebGLRenderDevice::CreateRasterizerState(const WebGLRasterizerDesc& desc)
{
	return std::make_shared<WebGLRasterizerState>(desc);
}

std::shared_ptr<WebGLDepthStencilState> WebGLRenderDevice::CreateDepthStencilState(const WebGLDepthStencilDesc& desc)
{
	return std::make_shared<WebGLDepthStencilState>(desc);
}

std::shared_ptr<WebGLBuffer> WebGLRenderDevice::CreateBuffer(GLenum target, GLenum usage, const void* data, size_t size, const char* debugName)
{
	auto buffer = std::make_shared<WebGLBuffer>();
	glBindBuffer(target, buffer->Handle);
	glBufferData(target, size, data, usage);
	ThrowIfWebGLError((std::string("CreateBuffer failed for ") + debugName).c_str());
	return buffer;
}

// GLSL ES 3.00 has no layout(binding). Every program has at most one uniform block,
// and SetUniformBuffers attaches it at binding point 0. Sampler uniforms are pointed
// at the texture units SetTextures uses. Only active uniforms are queried: WebGL
// raises INVALID_OPERATION for glGetUniformLocation / glGetUniformBlockIndex with
// names that are not in the program.
void WebGLRenderDevice::BindShaderResources(const std::string& programName, GLuint program)
{
	while (glGetError() != GL_NO_ERROR)
		;

	GLint blockCount = 0;
	glGetProgramiv(program, GL_ACTIVE_UNIFORM_BLOCKS, &blockCount);
	for (GLint i = 0; i < blockCount; i++)
		glUniformBlockBinding(program, (GLuint)i, 0);

	glUseProgram(program);

	GLint uniformCount = 0;
	glGetProgramiv(program, GL_ACTIVE_UNIFORMS, &uniformCount);
	for (GLint i = 0; i < uniformCount; i++)
	{
		char name[128] = {};
		GLsizei length = 0;
		GLint size = 0;
		GLenum type = 0;
		glGetActiveUniform(program, (GLuint)i, (GLsizei)sizeof(name), &length, &size, &type, name);
		if (type != GL_SAMPLER_2D || length <= 0)
			continue;

		std::string uniformName(name, name + length);
		size_t bracket = uniformName.find('[');
		if (bracket != std::string::npos)
			uniformName.resize(bracket);

		GLint unit = -1;
		if (uniformName == "tex" || uniformName == "texSampler")
			unit = 0;
		else if (uniformName == "lightmap" || uniformName == "texDither")
			unit = 1;
		else if (uniformName == "macro")
			unit = 2;
		else if (uniformName == "detail")
			unit = 3;
		if (unit < 0)
			throw std::runtime_error("Unknown sampler uniform '" + uniformName + "' in program '" + programName + "'");

		GLint location = glGetUniformLocation(program, name);
		if (location >= 0)
			glUniform1i(location, unit);
	}

	glUseProgram(0);
	StateCache.Program = 0;

	GLenum error = glGetError();
	if (error != GL_NO_ERROR)
		throw std::runtime_error("Binding shader resources failed for program '" + programName + "' (" + std::to_string(error) + ")");
}

std::shared_ptr<WebGLProgram> WebGLRenderDevice::CreateProgram(const std::string& programName, std::shared_ptr<WebGLVertexShader> vertexShader, std::shared_ptr<WebGLFragmentShader> fragmentShader)
{
	auto program = std::make_shared<WebGLProgram>();
	glAttachShader(program->Handle, vertexShader->Handle);
	glAttachShader(program->Handle, fragmentShader->Handle);
	glLinkProgram(program->Handle);
	GLint status = GL_FALSE;
	glGetProgramiv(program->Handle, GL_LINK_STATUS, &status);
	if (status == GL_FALSE)
	{
		GLint length = 0;
		glGetProgramiv(program->Handle, GL_INFO_LOG_LENGTH, &length);
		std::string log;
		log.resize(length);
		glGetProgramInfoLog(program->Handle, length, &length, log.data());
		log.resize(length);
		throw std::runtime_error("Could not link program '" + programName + "':" + log);
	}
	BindShaderResources(programName, program->Handle);
	return program;
}

std::shared_ptr<WebGLVertexShader> WebGLRenderDevice::CreateVertexShader(const std::string& shaderName, const std::string& filename, const std::vector<std::string> defines)
{
	auto shader = std::make_shared<WebGLVertexShader>();
	CompileGlsl(shader.get(), filename, defines);
	return shader;
}

std::shared_ptr<WebGLFragmentShader> WebGLRenderDevice::CreateFragmentShader(const std::string& shaderName, const std::string& filename, const std::vector<std::string> defines)
{
	auto shader = std::make_shared<WebGLFragmentShader>();
	CompileGlsl(shader.get(), filename, defines);
	return shader;
}

void WebGLRenderDevice::CompileGlsl(WebGLShader* shader, const std::string& filename, const std::vector<std::string> defines)
{
	std::string code = "#version 300 es\n";
	for (const std::string& define : defines)
	{
		code += "#define ";
		code += define;
		code += "\n";
	}
	code += "#line 1\n";
	std::string body = WebGLFileResource::readAllText(filename);
	if (body.empty())
		throw std::runtime_error("No WebGL shader source for '" + filename + "'");
	code += body;

	GLchar* source = (GLchar*)code.data();
	GLint length = (GLint)code.size();
	glShaderSource(shader->Handle, 1, &source, &length);
	glCompileShader(shader->Handle);

	GLint status = GL_FALSE;
	glGetShaderiv(shader->Handle, GL_COMPILE_STATUS, &status);
	if (status == GL_FALSE)
	{
		GLint length = 0;
		glGetShaderiv(shader->Handle, GL_INFO_LOG_LENGTH, &length);
		std::string log;
		log.resize(length);
		glGetShaderInfoLog(shader->Handle, length, &length, log.data());
		log.resize(length);
		throw std::runtime_error("Could not compile shader '" + filename + "':" + log);
	}
}
