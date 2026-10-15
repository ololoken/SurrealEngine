#pragma once

#include "WebGLHandles.h"

struct TextureInfo;
class UnrealMipmap;
struct TextureColor;
enum class TextureFormat : uint32_t;

class WebGLTextureUploader
{
public:
	WebGLTextureUploader(GLint internalformat, GLenum format, GLenum type) : Internalformat(internalformat), Format(format), Type(type) { }
	virtual ~WebGLTextureUploader() = default;

	virtual int GetUploadSize(int x, int y, int w, int h) = 0;
	virtual void UploadRect(void* dst, UnrealMipmap* mip, int x, int y, int w, int h, TextureColor* palette, bool masked) = 0;

	GLint GetInternalformat() const { return Internalformat; }
	GLenum GetFormat() const { return Format; }
	GLenum GetType() const { return Type; }
	bool IsCompressed() const { return Format == 0; }

	static WebGLTextureUploader* GetUploader(TextureFormat format);

	// Set by WebGLRenderDevice::Init before the first upload, based on the
	// WEBGL_compressed_texture_s3tc extension being present.
	static bool SupportsS3TC;

private:
	GLint Internalformat;
	GLenum Format;
	GLenum Type;
};

class WebGLTextureUploader_P8 : public WebGLTextureUploader
{
public:
	WebGLTextureUploader_P8() : WebGLTextureUploader(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE) { }

	int GetUploadSize(int x, int y, int w, int h) override;
	void UploadRect(void* dst, UnrealMipmap* mip, int x, int y, int w, int h, TextureColor* palette, bool masked) override;
};

class WebGLTextureUploader_RGB8 : public WebGLTextureUploader
{
public:
	WebGLTextureUploader_RGB8() : WebGLTextureUploader(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE) { }

	int GetUploadSize(int x, int y, int w, int h) override;
	void UploadRect(void* dst, UnrealMipmap* mip, int x, int y, int w, int h, TextureColor* palette, bool masked) override;
};

// WebGL has no GL_BGRA pixel format, so BGRA8 is swizzled to RGBA on the CPU.
class WebGLTextureUploader_BGRA8 : public WebGLTextureUploader
{
public:
	WebGLTextureUploader_BGRA8() : WebGLTextureUploader(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE) { }

	int GetUploadSize(int x, int y, int w, int h) override;
	void UploadRect(void* dst, UnrealMipmap* mip, int x, int y, int w, int h, TextureColor* palette, bool masked) override;
};

class WebGLTextureUploader_BGRA8_LM : public WebGLTextureUploader
{
public:
	WebGLTextureUploader_BGRA8_LM() : WebGLTextureUploader(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE) { }

	int GetUploadSize(int x, int y, int w, int h) override;
	void UploadRect(void* dst, UnrealMipmap* mip, int x, int y, int w, int h, TextureColor* palette, bool masked) override;
};

class WebGLTextureUploader_Simple : public WebGLTextureUploader
{
public:
	WebGLTextureUploader_Simple(GLint internalformat, GLenum format, GLenum type, int bytesPerPixel) : WebGLTextureUploader(internalformat, format, type), BytesPerPixel(bytesPerPixel) { }

	int GetUploadSize(int x, int y, int w, int h) override;
	void UploadRect(void* dst, UnrealMipmap* mip, int x, int y, int w, int h, TextureColor* palette, bool masked) override;

private:
	int BytesPerPixel;
};

class WebGLTextureUploader_4x4Block : public WebGLTextureUploader
{
public:
	WebGLTextureUploader_4x4Block(GLint internalformat, int bytesPerBlock) : WebGLTextureUploader(internalformat, 0, 0), BytesPerBlock(bytesPerBlock) { }

	int GetUploadSize(int x, int y, int w, int h) override;
	void UploadRect(void* dst, UnrealMipmap* mip, int x, int y, int w, int h, TextureColor* palette, bool masked) override;

private:
	int BytesPerBlock;
};
