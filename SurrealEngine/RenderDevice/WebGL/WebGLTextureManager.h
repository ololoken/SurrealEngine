#pragma once

#include <unordered_map>
#include <memory>

struct TextureInfo;
class WebGLRenderDevice;
class WebGLCachedTexture;

class WebGLTextureManager
{
public:
	WebGLTextureManager(WebGLRenderDevice* renderer);
	~WebGLTextureManager();

	void UpdateTextureRect(TextureInfo* info, int x, int y, int w, int h);
	WebGLCachedTexture* GetTexture(TextureInfo* info, bool masked);

	void ClearCache();
	int GetTexturesInCache() { return (int)(TextureCache[0].size() + TextureCache[1].size()); }

	WebGLCachedTexture* GetNullTexture()
	{
		if (NullTexture)
			return NullTexture.get();
		return CreateNullTexture();
	}

private:
	void UploadTexture(TextureInfo* info, bool masked, WebGLCachedTexture* tex);
	WebGLCachedTexture* CreateNullTexture();

	WebGLCachedTexture* GetFromCache(int masked, uint64_t cacheID);

	WebGLRenderDevice* renderer = nullptr;
	std::unordered_map<uint64_t, std::unique_ptr<WebGLCachedTexture>> TextureCache[2];
	std::unique_ptr<WebGLCachedTexture> NullTexture;

	std::pair<uint64_t, WebGLCachedTexture*> LastTextureResult[2];
};
