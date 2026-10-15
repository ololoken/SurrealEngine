#pragma once

#include "RenderDevice/RenderDevice.h"
#include "Packages/Engine/Resources/Level/UModel.h"
#include "Packages/Engine/Resources/Textures/UTexture.h"

#include "Math/vec.h"
#include "Math/mat.h"
#include <set>
#include <string>
#include <vector>

#include "WebGLTextureManager.h"
#include "WebGLUploadManager.h"
#include "WebGLCachedTexture.h"

#include "WebGLHandles.h"

// WebGL 2 (OpenGL ES 3.0) render device for the Emscripten build.
//
// This is a port of the desktop GLRenderDevice with the WebGL restrictions baked in
// instead of being spread over #ifdefs:
// - GL entry points are called directly (Emscripten links them), no gl_load
// - always single-sampled: WebGL 2 has no multisample textures
// - the scene framebuffer only has a color attachment; hit testing is not implemented
//   (the desktop GL backend has its hit readback disabled as well)
// - vertex/index data is staged in CPU memory and uploaded with glBufferSubData,
//   as glMapBufferRange is not available in WebGL
// - shaders are GLSL ES 3.00 (WebGLFileResource); sampler units and uniform block
//   bindings are assigned after linking since layout(binding) needs ES 3.10

struct WebGLSceneVertex
{
	uint32_t Flags;
	vec3 Position;
	vec2 TexCoord;
	vec2 TexCoord2;
	vec2 TexCoord3;
	vec2 TexCoord4;
	vec4 Color;
};

struct WebGLScenePushConstants
{
	mat4 ObjectToProjection;
	mat4 ObjectToView;
	vec4 NearClip;
	int HitIndex;
	int Padding1, Padding2, Padding3;
};

struct WebGLPresentPushConstants
{
	float Contrast;
	float Saturation;
	float Brightness;
	float HdrScale;
	vec4 GammaCorrection;
};

struct WebGLBloomPushConstants
{
	float SampleWeights[8];
};

struct WebGLViewport
{
	float X;
	float Y;
	float Width;
	float Height;
	float MinDepth;
	float MaxDepth;
};

class WebGLRenderDevice : public RenderDevice
{
public:
	WebGLRenderDevice(Widget* viewport);
	~WebGLRenderDevice();

	bool Init(int NewX, int NewY, bool Fullscreen);
	bool SetRes(int NewX, int NewY, bool Fullscreen);
	void Exit();

	void Flush(bool AllowPrecache) override;
	void Lock(vec4 FlashScale, vec4 FlashFog, vec4 ScreenClear, uint8_t* HitData, int* HitSize) override;
	void Unlock(bool Blit) override;
	void DrawComplexSurface(SceneNode* Frame, SurfaceInfo& Surface, SurfaceFacet& Facet) override;
	void DrawGouraudPolygon(SceneNode* Frame, TextureInfo& Info, const GouraudVertex* Pts, int NumPts, uint32_t PolyFlags) override;
	void DrawTile(SceneNode* Frame, TextureInfo& Info, float X, float Y, float XL, float YL, float U, float V, float UL, float VL, float Z, vec4 Color, vec4 Fog, uint32_t PolyFlags) override;
	void Draw3DLine(SceneNode* Frame, vec4 Color, uint32_t LineFlags, vec3 OrigP, vec3 OrigQ) override;
	void Draw2DLine(SceneNode* Frame, vec4 Color, uint32_t LineFlags, vec3 P1, vec3 P2) override;
	void Draw2DPoint(SceneNode* Frame, vec4 Color, uint32_t LineFlags, float X1, float Y1, float X2, float Y2, float Z) override;
	void ClearZ() override;
	void PushHit(const uint8_t* Data, int Count) override;
	void PopHit(int Count, bool bForce) override;
	void ReadPixels(TextureColor* Pixels) override;
	void EndFlash() override;
	void SetSceneNode(SceneNode* Frame) override;
	void PrecacheTexture(TextureInfo& Info, uint32_t PolyFlags) override;
	bool SupportsTextureFormat(TextureFormat Format) override;
	void UpdateTextureRect(TextureInfo& Info, int U, int V, int UL, int VL) override;

	void SetHitLocation();

	struct PPBlurLevel
	{
		std::shared_ptr<WebGLTexture2D> VTexture;
		std::shared_ptr<WebGLFramebuffer> VFramebuffer;
		std::shared_ptr<WebGLTexture2D> HTexture;
		std::shared_ptr<WebGLFramebuffer> HFramebuffer;
		int Width = 0;
		int Height = 0;
	};

	struct
	{
		std::shared_ptr<WebGLFramebuffer> Framebuffer;
		std::shared_ptr<WebGLTexture2D> ColorBuffer;
		std::shared_ptr<WebGLTexture2D> DepthBuffer;
		std::shared_ptr<WebGLFramebuffer> PPFramebuffer[2];
		std::shared_ptr<WebGLTexture2D> PPImage[2];
		enum { NumBloomLevels = 4 };
		PPBlurLevel BlurLevels[NumBloomLevels];
		int Width = 0;
		int Height = 0;
	} SceneBuffers;

	struct ScenePipelineState
	{
		GLenum PrimitiveTopology = GL_TRIANGLES;
		WebGLProgram* ShaderProgram = nullptr;
		std::shared_ptr<WebGLBlendState> BlendState;
		std::shared_ptr<WebGLDepthStencilState> DepthStencilState;
		float MinDepth = 0.1f;
		float MaxDepth = 1.0f;
	};

	struct
	{
		std::shared_ptr<WebGLVertexShader> VertexShader;
		std::shared_ptr<WebGLInputLayout> InputLayout;
		std::shared_ptr<WebGLBuffer> VertexBuffer;
		std::shared_ptr<WebGLBuffer> IndexBuffer;
		std::shared_ptr<WebGLBuffer> ConstantBuffer;
		std::shared_ptr<WebGLRasterizerState> RasterizerState;
		std::shared_ptr<WebGLFragmentShader> FragmentShader;
		std::shared_ptr<WebGLFragmentShader> FragmentShaderAlphaTest;
		std::shared_ptr<WebGLProgram> ShaderProgram;
		std::shared_ptr<WebGLProgram> ShaderProgramAlphaTest;
		std::shared_ptr<WebGLSampler> Samplers[16];
		ScenePipelineState Pipelines[33];
		ScenePipelineState LinePipeline[2];
		ScenePipelineState PointPipeline[2];
		float LODBias = 0.0f;
	} ScenePass;

	static const int SceneVertexBufferSize = 16 * 1024;
	static const int SceneIndexBufferSize = 32 * 1024;

	struct DrawBatchEntry
	{
		size_t SceneIndexStart = 0;
		size_t SceneIndexEnd = 0;
		ScenePipelineState* Pipeline = nullptr;
		WebGLCachedTexture* Tex = nullptr;
		WebGLCachedTexture* Lightmap = nullptr;
		WebGLCachedTexture* Detailtex = nullptr;
		WebGLCachedTexture* Macrotex = nullptr;
		uint32_t TexSamplerMode = 0;
		uint32_t DetailtexSamplerMode = 0;
		uint32_t MacrotexSamplerMode = 0;
		float BlendConstants[4] = {};
	} Batch;
	std::vector<DrawBatchEntry> QueuedBatches;
	WebGLCachedTexture* nulltex = nullptr;

	// CPU staging for the scene vertex/index buffers (see MapVertices/UnmapVertices)
	std::vector<WebGLSceneVertex> StagingVertices;
	std::vector<uint32_t> StagingIndexes;
	size_t UploadedVertexPos = 0; // staging range [0, UploadedVertexPos) is already in the GL buffer
	size_t UploadedIndexPos = 0;

	WebGLSceneVertex* SceneVertices = nullptr;
	size_t SceneVertexPos = 0;

	uint32_t* SceneIndexes = nullptr;
	size_t SceneIndexPos = 0;

	struct
	{
		std::shared_ptr<WebGLVertexShader> PPStep;
		std::shared_ptr<WebGLInputLayout> PPStepLayout;
		std::shared_ptr<WebGLBuffer> PPStepVertexBuffer;
		std::shared_ptr<WebGLFragmentShader> Present[16];
		std::shared_ptr<WebGLProgram> PresentProgram[16];
		std::shared_ptr<WebGLBuffer> PresentConstantBuffer;
		std::shared_ptr<WebGLTexture2D> DitherTexture;
		std::shared_ptr<WebGLSampler> LinearSampler;  // post process inputs (clamp, no mipmaps)
		std::shared_ptr<WebGLSampler> NearestSampler; // dither texture
		std::shared_ptr<WebGLBlendState> BlendState;
		std::shared_ptr<WebGLDepthStencilState> DepthStencilState;
		std::shared_ptr<WebGLRasterizerState> RasterizerState;
	} PresentPass;

	struct
	{
		std::shared_ptr<WebGLFragmentShader> Extract;
		std::shared_ptr<WebGLFragmentShader> Combine;
		std::shared_ptr<WebGLFragmentShader> BlurVertical;
		std::shared_ptr<WebGLFragmentShader> BlurHorizontal;
		std::shared_ptr<WebGLProgram> ExtractProgram;
		std::shared_ptr<WebGLProgram> CombineProgram;
		std::shared_ptr<WebGLProgram> BlurVerticalProgram;
		std::shared_ptr<WebGLProgram> BlurHorizontalProgram;
		std::shared_ptr<WebGLBuffer> ConstantBuffer;
		std::shared_ptr<WebGLBlendState> AdditiveBlendState;
	} BloomPass;

	std::unique_ptr<WebGLTextureManager> Textures;
	std::unique_ptr<WebGLUploadManager> Uploads;

	struct
	{
		int ComplexSurfaces = 0;
		int GouraudPolygons = 0;
		int Tiles = 0;
		int DrawCalls = 0;
		int Uploads = 0;
		int RectUploads = 0;
		int BuffersUsed = 0;
	} Stats;

	// Error check for code that runs every frame; see ValidateEveryCall
	void CheckHotPathError(const char* message)
	{
		if (ValidateEveryCall)
			ThrowIfWebGLError(message);
	}

	// Texture uploads bind to whatever unit is active (unit 0 outside of SetTextures),
	// which the state cache cannot see.
	void InvalidateTextureUnit(int unit) { StateCache.Textures[unit] = GLStateCache::Unknown; }

private:
	struct ComplexSurfaceInfo
	{
		SurfaceFacet* facet;
		WebGLCachedTexture* tex;
		WebGLCachedTexture* lightmap;
		WebGLCachedTexture* macrotex;
		WebGLCachedTexture* detailtex;
		WebGLCachedTexture* fogmap;
		vec4* editorcolor;
	};
	void DrawComplexSurfaceFaces(const ComplexSurfaceInfo& info);

	void DetectExtensions();
	void LogContextInfo();

	void ResizeSceneBuffers(int width, int height);
	void ClearTextureCache();

	void CreatePresentPass();
	void CreateBloomPass();
	void CreateScenePass();
	void ReleaseScenePass();
	void ReleaseBloomPass();
	void ReleasePresentPass();

	void CreateSceneSamplers();
	void ReleaseSceneSamplers();
	void UpdateLODBias();

	void ReleaseSceneBuffers();

	void RunBloomPass();
	void BlurStep(WebGLTexture2D* input, WebGLFramebuffer* output, bool vertical);
	float ComputeBlurGaussian(float n, float theta);
	void ComputeBlurSamples(int sampleCount, float blurAmount, float* sampleWeights);

	void SetPipeline(ScenePipelineState* pipeline);
	void SetPipeline(uint32_t polyflags);
	void SetDescriptorSet(uint32_t polyflags, WebGLCachedTexture* tex = nullptr, bool clamp = false);
	void SetDescriptorSet(uint32_t polyflags, const ComplexSurfaceInfo& info);

	void AddDrawBatch();
	void DrawBatches(bool nextBuffer = false);
	void DrawEntry(const DrawBatchEntry& entry);

	struct VertexReserveInfo
	{
		WebGLSceneVertex* vptr;
		uint32_t* iptr;
		uint32_t vpos;
	};

	VertexReserveInfo ReserveVertices(size_t vcount, size_t icount)
	{
		if (!SceneVertices || !SceneIndexes)
			return { nullptr, nullptr, 0 };

		// If buffers are full, flush and wait for room.
		if (SceneVertexPos + vcount > (size_t)SceneVertexBufferSize || SceneIndexPos + icount > (size_t)SceneIndexBufferSize)
		{
			// If the request is larger than our buffers we can't draw this.
			if (vcount > (size_t)SceneVertexBufferSize || icount > (size_t)SceneIndexBufferSize)
				return { nullptr, nullptr, 0 };

			DrawBatches(true);
		}

		return { SceneVertices + SceneVertexPos, SceneIndexes + SceneIndexPos, (uint32_t)SceneVertexPos };
	}

	void UseVertices(size_t vcount, size_t icount)
	{
		SceneVertexPos += vcount;
		SceneIndexPos += icount;
	}

	ScenePipelineState* GetPipeline(uint32_t PolyFlags);

	std::shared_ptr<WebGLBlendState> CreateBlendState(const WebGLBlendDesc& desc);
	std::shared_ptr<WebGLRasterizerState> CreateRasterizerState(const WebGLRasterizerDesc& desc);
	std::shared_ptr<WebGLDepthStencilState> CreateDepthStencilState(const WebGLDepthStencilDesc& desc);

	std::shared_ptr<WebGLBuffer> CreateBuffer(GLenum target, GLenum usage, const void* data, size_t size, const char* debugName);

	std::shared_ptr<WebGLProgram> CreateProgram(const std::string& programName, std::shared_ptr<WebGLVertexShader> vertexShader, std::shared_ptr<WebGLFragmentShader> fragmentShader);
	std::shared_ptr<WebGLVertexShader> CreateVertexShader(const std::string& shaderName, const std::string& filename, const std::vector<std::string> defines = {});
	std::shared_ptr<WebGLFragmentShader> CreateFragmentShader(const std::string& shaderName, const std::string& filename, const std::vector<std::string> defines = {});
	void CompileGlsl(WebGLShader* shader, const std::string& filename, const std::vector<std::string> defines = {});
	void BindShaderResources(const std::string& programName, GLuint program);

	vec4 ApplyInverseGamma(vec4 color);

	WebGLPresentPushConstants GetPresentPushConstants();
	int GetPresentShaderIndex(const WebGLPresentPushConstants& pushconstants);

	bool VerticesMapped() const { return SceneVertices && SceneIndexes; }
	void MapVertices(bool nextBuffer);
	void UnmapVertices();

	// Every GL call crosses from wasm into the browser, so the state setters below skip
	// calls that would not change anything. The cache only knows about state set through
	// them; code that touches the same state directly must call ResetStateCache afterwards.
	struct GLStateCache
	{
		static const int MaxUnits = 16;
		static const GLuint Unknown = ~0u;
		GLuint Program = Unknown;
		GLuint Textures[MaxUnits];
		GLuint Samplers[MaxUnits];
		const WebGLBlendState* BlendState = nullptr;
		bool HasBlendConstants = false;
		float BlendConstants[4] = {};
		const WebGLDepthStencilState* DepthStencilState = nullptr;
		const WebGLRasterizerState* RasterizerState = nullptr;
	};
	GLStateCache StateCache;
	void ResetStateCache();
	void UseProgram(GLuint program);

	void SetViewport(const WebGLViewport& viewport);
	void SetBufferData(GLenum target, GLenum usage, WebGLBuffer* buffer, const void* data, size_t size);
	void SetBlendState(WebGLBlendState* blendState, const float* BlendConstants = nullptr);
	void SetDepthStencilState(WebGLDepthStencilState* depthStencilState);
	void SetRasterizerState(WebGLRasterizerState* rasterizerState);
	void SetUniformBuffers(int start, int count, WebGLBuffer** buffers);
	void SetTextures(int start, int count, WebGLTexture** textures);
	void SetSamplers(int start, int count, WebGLSampler** samplers);

	bool UsePrecache = true;
	vec4 FlashScale = vec4(0.0f);
	vec4 FlashFog = vec4(0.0f);
	SceneNode* CurrentFrame = nullptr;
	float Aspect = 0.0f;
	float RProjZ = 0.0f;
	float RFX2 = 0.0f;
	float RFY2 = 0.0f;
	WebGLScenePushConstants SceneConstants = {};
	WebGLViewport SceneViewport = {};

	struct HitQuery
	{
		int Start = 0;
		int Count = 0;
	};

	uint8_t* HitData = nullptr;
	int* HitSize = nullptr;
	std::vector<uint8_t> HitQueryStack;
	std::vector<HitQuery> HitQueries;
	std::vector<uint8_t> HitBuffer;

	int ForceHitIndex = -1;

	bool IsLocked = false;
	bool ActiveHdr = false;

	bool HasAnisotropicFiltering = false;
	bool HasColorBufferFloat = false;

	// glGetError is a synchronous round trip to the browser's GPU process, so the per-frame
	// paths only check when SURREAL_WEBGL_VALIDATE is set in the environment. Without it,
	// Unlock polls the (sticky) error state every few dozen frames instead.
	bool ValidateEveryCall = false;
	uint32_t FramesSinceErrorCheck = 0;

	bool CurrentFullscreen = 0;
	int CurrentSizeX = 0;
	int CurrentSizeY = 0;
};

inline void WebGLRenderDevice::SetPipeline(ScenePipelineState* pipeline)
{
	if (pipeline != Batch.Pipeline)
	{
		AddDrawBatch();
		Batch.Pipeline = pipeline;
	}
}

inline void WebGLRenderDevice::SetPipeline(uint32_t PolyFlags)
{
	auto pipeline = GetPipeline(PolyFlags);
	if (pipeline != Batch.Pipeline)
	{
		AddDrawBatch();
		Batch.Pipeline = pipeline;
	}
}

inline void WebGLRenderDevice::SetDescriptorSet(uint32_t PolyFlags, WebGLCachedTexture* tex, bool clamp)
{
	if (!tex) tex = nulltex;

	uint32_t samplermode = 0;
	if (PolyFlags & PF_NoSmooth) samplermode |= 1;
	if (clamp) samplermode |= 2;
	samplermode |= (tex->DummyMipmapCount << 2);

	if (Batch.Tex != tex || Batch.TexSamplerMode != samplermode || Batch.Lightmap != nulltex || Batch.Detailtex != nulltex || Batch.DetailtexSamplerMode != 0 || Batch.Macrotex != nulltex || Batch.MacrotexSamplerMode != 0)
	{
		AddDrawBatch();
		Batch.Tex = tex;
		Batch.Lightmap = nulltex;
		Batch.Detailtex = nulltex;
		Batch.Macrotex = nulltex;
		Batch.TexSamplerMode = samplermode;
		Batch.DetailtexSamplerMode = 0;
		Batch.MacrotexSamplerMode = 0;
	}
}

inline void WebGLRenderDevice::SetDescriptorSet(uint32_t PolyFlags, const ComplexSurfaceInfo& info)
{
	uint32_t samplermode = 0;
	if (PolyFlags & PF_NoSmooth) samplermode |= 1;
	samplermode |= (info.tex->DummyMipmapCount << 2);

	int detailsamplermode = info.detailtex->DummyMipmapCount << 2;
	int macrosamplermode = info.macrotex->DummyMipmapCount << 2;

	if (Batch.Tex != info.tex || Batch.TexSamplerMode != samplermode || Batch.Lightmap != info.lightmap || Batch.Detailtex != info.detailtex || Batch.DetailtexSamplerMode != detailsamplermode || Batch.Macrotex != info.macrotex || Batch.MacrotexSamplerMode != macrosamplermode)
	{
		AddDrawBatch();
		Batch.Tex = info.tex;
		Batch.Lightmap = info.lightmap;
		Batch.Detailtex = info.detailtex;
		Batch.Macrotex = info.macrotex;
		Batch.TexSamplerMode = samplermode;
		Batch.DetailtexSamplerMode = detailsamplermode;
		Batch.MacrotexSamplerMode = macrosamplermode;
	}
}
