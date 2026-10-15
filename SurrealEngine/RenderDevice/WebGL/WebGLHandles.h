#pragma once

// The WebGL backend calls OpenGL ES 3.0 directly. Emscripten resolves these
// symbols at link time (-lGL), so there is no runtime function loader here.
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <utility>
#include <memory>
#include <stdexcept>

class WebGLTexture
{
public:
	WebGLTexture()
	{
		glGenTextures(1, &Handle);
	}

	~WebGLTexture()
	{
		glDeleteTextures(1, &Handle);
		Handle = 0;
	}

	WebGLTexture(const WebGLTexture& other) = delete;
	WebGLTexture& operator=(const WebGLTexture& other) = delete;

	GLuint Handle = 0;
};

typedef WebGLTexture WebGLTexture2D;

class WebGLFramebuffer
{
public:
	WebGLFramebuffer()
	{
		glGenFramebuffers(1, &Handle);
	}

	~WebGLFramebuffer()
	{
		glDeleteFramebuffers(1, &Handle);
		Handle = 0;
	}

	WebGLFramebuffer(const WebGLFramebuffer& other) = delete;
	WebGLFramebuffer& operator=(const WebGLFramebuffer& other) = delete;

	GLuint Handle = 0;
};

class WebGLShader
{
public:
	WebGLShader(GLuint type)
	{
		Handle = glCreateShader(type);
	}

	~WebGLShader()
	{
		glDeleteShader(Handle);
		Handle = 0;
	}

	WebGLShader(const WebGLShader& other) = delete;
	WebGLShader& operator=(const WebGLShader& other) = delete;

	GLuint Handle = 0;
};

class WebGLVertexShader : public WebGLShader
{
public:
	WebGLVertexShader() : WebGLShader(GL_VERTEX_SHADER) {}
};

class WebGLFragmentShader : public WebGLShader
{
public:
	WebGLFragmentShader() : WebGLShader(GL_FRAGMENT_SHADER) {}
};

class WebGLProgram
{
public:
	WebGLProgram()
	{
		Handle = glCreateProgram();
	}

	~WebGLProgram()
	{
		glDeleteProgram(Handle);
		Handle = 0;
	}

	WebGLProgram(const WebGLProgram& other) = delete;
	WebGLProgram& operator=(const WebGLProgram& other) = delete;

	GLuint Handle = 0;
};

class WebGLBuffer
{
public:
	WebGLBuffer()
	{
		glGenBuffers(1, &Handle);
	}

	~WebGLBuffer()
	{
		glDeleteBuffers(1, &Handle);
		Handle = 0;
	}

	WebGLBuffer(const WebGLBuffer& other) = delete;
	WebGLBuffer& operator=(const WebGLBuffer& other) = delete;

	GLuint Handle = 0;
};

class WebGLSampler
{
public:
	WebGLSampler()
	{
		glGenSamplers(1, &Handle);
	}

	~WebGLSampler()
	{
		glDeleteSamplers(1, &Handle);
		Handle = 0;
	}

	WebGLSampler(const WebGLSampler& other) = delete;
	WebGLSampler& operator=(const WebGLSampler& other) = delete;

	GLuint Handle = 0;
};

class WebGLInputLayout
{
public:
	WebGLInputLayout()
	{
		glGenVertexArrays(1, &Handle);
	}

	~WebGLInputLayout()
	{
		glDeleteVertexArrays(1, &Handle);
		Handle = 0;
	}

	WebGLInputLayout(const WebGLInputLayout& other) = delete;
	WebGLInputLayout& operator=(const WebGLInputLayout& other) = delete;

	GLuint Handle = 0;
};

// WebGL 2 has no per-attachment blend state (that needs EXT_draw_buffers_indexed),
// so a blend state describes a single target that applies to all draw buffers.
class WebGLBlendDesc
{
public:
	bool RenderTargetWriteMask; // glColorMask
	bool BlendEnable;
	GLenum BlendOp;
	GLenum BlendOpAlpha;
	GLenum SrcBlend;
	GLenum SrcBlendAlpha;
	GLenum DestBlend;
	GLenum DestBlendAlpha;
};

class WebGLBlendState
{
public:
	WebGLBlendState(const WebGLBlendDesc& desc) : desc(desc) {}
	WebGLBlendDesc desc = {};
};

class WebGLDepthStencilDesc
{
public:
	bool DepthEnable;
	GLenum DepthFunc;
	bool DepthWriteMask;
};

class WebGLDepthStencilState
{
public:
	WebGLDepthStencilState(const WebGLDepthStencilDesc& desc) : desc(desc) {}
	WebGLDepthStencilDesc desc = {};
};

class WebGLRasterizerDesc
{
public:
	bool CullEnable;
	bool FrontCounterClockwise;
};

class WebGLRasterizerState
{
public:
	WebGLRasterizerState(const WebGLRasterizerDesc& desc) : desc(desc) {}
	WebGLRasterizerDesc desc = {};
};

inline void ThrowIfWebGLError(const char* msg)
{
	GLenum error = glGetError();
	if (error != GL_NO_ERROR)
		throw std::runtime_error(msg);
}
