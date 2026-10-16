
#include "Precomp.h"
#include "WebGLTextureManager.h"
#include "WebGLRenderDevice.h"
#include "WebGLCachedTexture.h"

WebGLTextureManager::WebGLTextureManager(WebGLRenderDevice* renderer) : renderer(renderer)
{
}

WebGLTextureManager::~WebGLTextureManager()
{
	ClearCache();
}

WebGLCachedTexture* WebGLTextureManager::GetFromCache(int masked, uint64_t cacheID)
{
	if (LastTextureResult[masked].first == cacheID && LastTextureResult[masked].second)
		return LastTextureResult[masked].second;

	LastTextureResult[masked].first = cacheID;
	LastTextureResult[masked].second = TextureCache[masked][cacheID].get();

	return LastTextureResult[masked].second;
}

void WebGLTextureManager::UpdateTextureRect(TextureInfo* info, int x, int y, int w, int h)
{
	WebGLCachedTexture* tex = GetFromCache(0, info->CacheID);
	if (tex)
	{
		renderer->Uploads->UploadTextureRect(tex, *info, x, y, w, h);
		info->bRealtimeChanged = 0;
	}
}

WebGLCachedTexture* WebGLTextureManager::GetTexture(TextureInfo* info, bool masked)
{
	if (!info)
		return GetNullTexture();

	if (info->Texture && (info->Texture->PolyFlags() & PF_Masked))
		masked = true;

	if (info->Format != TextureFormat::P8)
		masked = false;

	WebGLCachedTexture* tex = GetFromCache((int)masked, info->CacheID);
	if (!tex)
	{
		std::unique_ptr<WebGLCachedTexture>& tex2 = TextureCache[(int)masked][info->CacheID];
		tex2.reset(new WebGLCachedTexture());
		tex = tex2.get();

		renderer->Uploads->UploadTexture(tex, *info, masked);
	}
	else
	{
		if (info->bRealtimeChanged)
			UploadTexture(info, masked, tex);
	}

	float uscale = info->UScale;
	float vscale = info->VScale;
	tex->UScale = uscale;
	tex->VScale = vscale;
	tex->PanX = info->Pan.x;
	tex->PanY = info->Pan.y;
	tex->UMult = 1.0f / (uscale * info->USize);
	tex->VMult = 1.0f / (vscale * info->VSize);

	return tex;
}

void WebGLTextureManager::UploadTexture(TextureInfo* info, bool masked, WebGLCachedTexture* tex)
{
	if (info->bRealtimeChanged)
	{
		info->bRealtimeChanged = 0;
		renderer->Uploads->UploadTexture(tex, *info, masked);
	}
}

void WebGLTextureManager::ClearCache()
{
	for (auto& cache : TextureCache)
	{
		cache.clear();
	}
	for (auto& texture : LastTextureResult)
		texture = {};
}

WebGLCachedTexture* WebGLTextureManager::CreateNullTexture()
{
	NullTexture.reset(new WebGLCachedTexture());
	// GL_UNSIGNED_INT_8_8_8_8 does not exist in GLES; four 0xff bytes is the same white pixel.
	uint8_t white[4] = { 255, 255, 255, 255 };
	NullTexture->Texture = std::make_shared<WebGLTexture2D>();
	renderer->BindTexture2D(NullTexture->Texture->Handle);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
	ThrowIfWebGLError("CreateNullTexture failed");
	return NullTexture.get();
}
