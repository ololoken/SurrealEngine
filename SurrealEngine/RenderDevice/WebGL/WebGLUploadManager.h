#pragma once

#include "WebGLTextureUploader.h"
#include <vector>

class WebGLRenderDevice;
class WebGLCachedTexture;
struct TextureInfo;

class WebGLUploadManager
{
public:
	WebGLUploadManager(WebGLRenderDevice* renderer);
	~WebGLUploadManager();

	bool SupportsTextureFormat(TextureFormat Format) const;

	void UploadTexture(WebGLCachedTexture* tex, const TextureInfo& Info, bool masked);
	void UploadTextureRect(WebGLCachedTexture* tex, const TextureInfo& Info, int x, int y, int w, int h);

	void UploadWhite(WebGLTexture2D* image);

private:
	void UploadData(WebGLTexture2D* image, const TextureInfo& Info, bool masked, WebGLTextureUploader* uploader, int dummyMipmapCount);

	uint8_t* GetUploadBuffer(size_t size);

	WebGLRenderDevice* renderer = nullptr;
	std::vector<uint32_t> UploadBuffer;
};
