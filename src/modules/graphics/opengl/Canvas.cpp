/**
 * Copyright (c) 2006-2017 LOVE Development Team
 *
 * This software is provided 'as-is', without any express or implied
 * warranty.  In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 **/

#include "Canvas.h"
#include "graphics/Graphics.h"

#include <algorithm> // For min/max

namespace love
{
namespace graphics
{
namespace opengl
{

static GLenum createFBO(GLuint &framebuffer, TextureType texType, PixelFormat format, GLuint texture, int layers)
{
	// get currently bound fbo to reset to it later
	GLuint current_fbo = gl.getFramebuffer(OpenGL::FRAMEBUFFER_ALL);

	glGenFramebuffers(1, &framebuffer);
	gl.bindFramebuffer(OpenGL::FRAMEBUFFER_ALL, framebuffer);

	if (texture != 0)
	{
		bool unusedSRGB = false;
		OpenGL::TextureFormat fmt = OpenGL::convertPixelFormat(format, false, unusedSRGB);

		int faces = texType == TEXTURE_CUBE ? 6 : 1;

		// Make sure all faces and layers of the texture are initialized to
		// transparent black. This is unfortunately probably pretty slow for
		// 2D-array and 3D textures with a lot of layers...
		for (int layer = layers - 1; layer >= 0; layer--)
		{
			for (int face = faces - 1; face >= 0; face--)
			{
				for (GLenum attachment : fmt.framebufferAttachments)
				{
					if (attachment == GL_NONE)
						continue;

					gl.framebufferTexture(attachment, texType, texture, 0, layer, face);

					if (isPixelFormatDepthStencil(format))
					{
						glClearDepth(1.0);
						glClearStencil(0);
						glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
					}
					else
					{
						glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
						glClear(GL_COLOR_BUFFER_BIT);
					}
				}
			}
		}
	}

	GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);

	gl.bindFramebuffer(OpenGL::FRAMEBUFFER_ALL, current_fbo);
	return status;
}

static bool createRenderbuffer(int width, int height, int &samples, PixelFormat pixelformat, GLuint &buffer)
{
	int reqsamples = samples;
	bool unusedSRGB = false;
	OpenGL::TextureFormat fmt = OpenGL::convertPixelFormat(pixelformat, true, unusedSRGB);

	GLuint current_fbo = gl.getFramebuffer(OpenGL::FRAMEBUFFER_ALL);

	// Temporary FBO used to clear the renderbuffer.
	GLuint fbo = 0;
	glGenFramebuffers(1, &fbo);
	gl.bindFramebuffer(OpenGL::FRAMEBUFFER_ALL, fbo);

	glGenRenderbuffers(1, &buffer);
	glBindRenderbuffer(GL_RENDERBUFFER, buffer);

	if (samples > 1)
		glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, fmt.internalformat, width, height);
	else
		glRenderbufferStorage(GL_RENDERBUFFER, fmt.internalformat, width, height);

	for (GLenum attachment : fmt.framebufferAttachments)
	{
		if (attachment != GL_NONE)
			glFramebufferRenderbuffer(GL_FRAMEBUFFER, attachment, GL_RENDERBUFFER, buffer);
	}

	if (samples > 1)
		glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_SAMPLES, &samples);
	else
		samples = 0;

	glBindRenderbuffer(GL_RENDERBUFFER, 0);

	GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);

	if (status == GL_FRAMEBUFFER_COMPLETE && (reqsamples <= 1 || samples > 1))
	{
		if (isPixelFormatDepthStencil(pixelformat))
		{
			glClearDepth(1.0);
			glClearStencil(0);
			glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
		}
		else
		{
			// Initialize the buffer to transparent black.
			glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
			glClear(GL_COLOR_BUFFER_BIT);
		}
	}
	else
	{
		glDeleteRenderbuffers(1, &buffer);
		buffer = 0;
		samples = 0;
	}

	gl.bindFramebuffer(OpenGL::FRAMEBUFFER_ALL, current_fbo);
	gl.deleteFramebuffer(fbo);

	return status == GL_FRAMEBUFFER_COMPLETE;
}

Canvas::Canvas(const Settings &settings)
	: love::graphics::Canvas(settings.type)
	, fbo(0)
	, texture(0)
    , renderbuffer(0)
	, requestedSamples(settings.msaa)
	, actualSamples(0)
	, textureMemory(0)
{
	width = settings.width;
	height = settings.height;
	pixelWidth = (int) ((width * settings.pixeldensity) + 0.5);
	pixelHeight = (int) ((height * settings.pixeldensity) + 0.5);

	if (texType == TEXTURE_VOLUME)
		depth = settings.layers;
	else if (texType == TEXTURE_2D_ARRAY)
		layers = settings.layers;
	else
		layers = 1;

	if (width <= 0 || height <= 0 || layers <= 0)
		throw love::Exception("Canvas dimensions must be greater than 0.");

	if (texType != TEXTURE_2D && settings.msaa > 1)
		throw love::Exception("MSAA is only supported for Canvases with the 2D texture type.");

	format = getSizedFormat(settings.format);

	if (settings.readable.set)
		readable = settings.readable.value;
	else
		readable = !isPixelFormatDepthStencil(format);

	if (readable && isPixelFormatDepthStencil(format) && settings.msaa > 1)
		throw love::Exception("Readable depth/stencil Canvases with MSAA are not currently supported.");

	initQuad();
	loadVolatile();

	if (status != GL_FRAMEBUFFER_COMPLETE)
		throw love::Exception("Cannot create Canvas: %s", OpenGL::framebufferStatusString(status));
}

Canvas::~Canvas()
{
	unloadVolatile();
}

bool Canvas::loadVolatile()
{
	if (texture != 0)
		return true;

	if (!Canvas::isSupported())
		throw love::Exception("Canvases are not supported by your OpenGL drivers!");

	if (!Canvas::isFormatSupported(format, readable))
	{
		const char *fstr = "rgba8";
		const char *readablestr = "";
		if (readable != !isPixelFormatDepthStencil(format))
			readablestr = readable ? " readable" : " non-readable";
		love::getConstant(Canvas::getSizedFormat(format), fstr);
		throw love::Exception("The %s%s canvas format is not supported by your OpenGL drivers.", fstr, readablestr);
	}

	if (requestedSamples > 1 && texType != TEXTURE_2D)
		throw love::Exception("MSAA is only supported for 2D texture types.");

	if (!readable && texType != TEXTURE_2D)
		throw love::Exception("Non-readable pixel formats are only supported for 2D texture types.");

	if (!gl.isTextureTypeSupported(texType))
	{
		const char *textypestr = "unknown";
		getConstant(texType, textypestr);
		throw love::Exception("%s textures are not supported on this system!", textypestr);
	}

	switch (texType)
	{
	case TEXTURE_2D:
		if (pixelWidth > gl.getMax2DTextureSize())
			throw TextureTooLargeException("width", pixelWidth);
		else if (pixelHeight > gl.getMax2DTextureSize())
			throw TextureTooLargeException("height", pixelHeight);
		break;
	case TEXTURE_VOLUME:
		if (pixelWidth > gl.getMax3DTextureSize())
			throw TextureTooLargeException("width", pixelWidth);
		else if (pixelHeight > gl.getMax3DTextureSize())
			throw TextureTooLargeException("height", pixelHeight);
		else if (depth > gl.getMax3DTextureSize())
			throw TextureTooLargeException("depth", depth);
		break;
	case TEXTURE_2D_ARRAY:
		if (pixelWidth > gl.getMax2DTextureSize())
			throw TextureTooLargeException("width", pixelWidth);
		else if (pixelHeight > gl.getMax2DTextureSize())
			throw TextureTooLargeException("height", pixelHeight);
		else if (layers > gl.getMaxTextureLayers())
			throw TextureTooLargeException("array layer count", layers);
		break;
	case TEXTURE_CUBE:
		if (pixelWidth != pixelHeight)
			throw love::Exception("Cubemap textures must have equal width and height.");
		else if (pixelWidth > gl.getMaxCubeTextureSize())
			throw TextureTooLargeException("width", pixelWidth);
		break;
	default:
		break;
	}

	OpenGL::TempDebugGroup debuggroup("Canvas load");

	fbo = texture = 0;
	renderbuffer = 0;
	status = GL_FRAMEBUFFER_COMPLETE;

	if (isReadable())
	{
		glGenTextures(1, &texture);
		gl.bindTextureToUnit(this, 0, false);

		GLenum gltype = OpenGL::getGLTextureType(texType);

		if (GLAD_ANGLE_texture_usage)
			glTexParameteri(gltype, GL_TEXTURE_USAGE_ANGLE, GL_FRAMEBUFFER_ATTACHMENT_ANGLE);

		setFilter(filter);
		setWrap(wrap);

		while (glGetError() != GL_NO_ERROR)
			/* Clear the error buffer. */;

		bool isSRGB = format == PIXELFORMAT_sRGBA8;
		if (!gl.rawTexStorage(texType, 1, format, isSRGB, pixelWidth, pixelHeight, texType == TEXTURE_VOLUME ? depth : layers))
		{
			status = GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
			return false;
		}

		if (glGetError() != GL_NO_ERROR)
		{
			gl.deleteTexture(texture);
			texture = 0;
			status = GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
			return false;
		}

		// Create a canvas-local FBO used for glReadPixels as well as MSAA blitting.
		status = createFBO(fbo, texType, format, texture, texType == TEXTURE_VOLUME ? depth : layers);

		if (status != GL_FRAMEBUFFER_COMPLETE)
		{
			if (fbo != 0)
			{
				gl.deleteFramebuffer(fbo);
				fbo = 0;
			}
			return false;
		}
	}

	// getMaxRenderbufferSamples will be 0 on systems that don't support
	// multisampled renderbuffers / don't export FBO multisample extensions.
	actualSamples = requestedSamples;
	actualSamples = std::min(actualSamples, gl.getMaxRenderbufferSamples());
	actualSamples = std::max(actualSamples, 0);
	actualSamples = actualSamples == 1 ? 0 : actualSamples;

	if (!isReadable() || actualSamples > 0)
		createRenderbuffer(pixelWidth, pixelHeight, actualSamples, format, renderbuffer);

	size_t prevmemsize = textureMemory;

	textureMemory = getPixelFormatSize(format) * pixelWidth * pixelHeight;

	if (actualSamples > 0 && isReadable())
		textureMemory += (textureMemory * actualSamples);
	else if (actualSamples > 0)
		textureMemory *= actualSamples;

	gl.updateTextureMemorySize(prevmemsize, textureMemory);

	return true;
}

void Canvas::unloadVolatile()
{
	if (fbo != 0)
		gl.deleteFramebuffer(fbo);

	if (renderbuffer != 0)
		glDeleteRenderbuffers(1, &renderbuffer);

	if (texture != 0)
		gl.deleteTexture(texture);

	fbo = 0;
	renderbuffer = 0;
	texture = 0;

	gl.updateTextureMemorySize(textureMemory, 0);
	textureMemory = 0;
}

void Canvas::setFilter(const Texture::Filter &f)
{
	if (!validateFilter(f, false))
		throw love::Exception("Invalid texture filter.");

	filter = f;
	gl.bindTextureToUnit(this, 0, false);
	gl.setTextureFilter(texType, filter);
}

bool Canvas::setWrap(const Texture::Wrap &w)
{
	bool success = true;
	bool forceclamp = texType == TEXTURE_CUBE;
	wrap = w;

	// If we only have limited NPOT support then the wrap mode must be CLAMP.
	if ((GLAD_ES_VERSION_2_0 && !(GLAD_ES_VERSION_3_0 || GLAD_OES_texture_npot))
		&& (pixelWidth != nextP2(pixelWidth) || pixelHeight != nextP2(pixelHeight) || depth != nextP2(depth)))
	{
		forceclamp = true;
	}

	if (forceclamp)
	{
		if (wrap.s != WRAP_CLAMP || wrap.t != WRAP_CLAMP || wrap.r != WRAP_CLAMP)
			success = false;

		wrap.s = wrap.t = wrap.r = WRAP_CLAMP;
	}

	if (!gl.isClampZeroTextureWrapSupported())
	{
		if (wrap.s == WRAP_CLAMP_ZERO) wrap.s = WRAP_CLAMP;
		if (wrap.t == WRAP_CLAMP_ZERO) wrap.t = WRAP_CLAMP;
		if (wrap.r == WRAP_CLAMP_ZERO) wrap.r = WRAP_CLAMP;
	}

	gl.bindTextureToUnit(this, 0, false);
	gl.setTextureWrap(texType, wrap);

	return success;
}

bool Canvas::setMipmapSharpness(float /*sharpness*/)
{
	return false;
}

ptrdiff_t Canvas::getHandle() const
{
	return texture;
}

love::image::ImageData *Canvas::newImageData(love::image::Image *module, int slice, int x, int y, int w, int h)
{
	if (!isReadable())
		throw love::Exception("Canvas:newImageData cannot be called on non-readable Canvases.");

	if (x < 0 || y < 0 || w <= 0 || h <= 0 || (x + w) > getPixelWidth() || (y + h) > getPixelHeight())
		throw love::Exception("Invalid rectangle dimensions.");

	if (slice < 0 || (texType == TEXTURE_VOLUME && slice >= depth)
		|| (texType == TEXTURE_2D_ARRAY && slice >= layers)
		|| (texType == TEXTURE_CUBE && slice >= 6))
	{
		throw love::Exception("Invalid slice index.");
	}

	Graphics *gfx = Module::getInstance<Graphics>(Module::M_GRAPHICS);
	if (gfx != nullptr && gfx->isCanvasActive(this))
		throw love::Exception("Canvas:newImageData cannot be called while that Canvas is currently active.");

	PixelFormat dataformat;
	switch (getPixelFormat())
	{
	case PIXELFORMAT_RGB10A2: // FIXME: Conversions aren't supported in GLES
		dataformat = PIXELFORMAT_RGBA16;
		break;
	case PIXELFORMAT_R16F:
	case PIXELFORMAT_RG16F:
	case PIXELFORMAT_RGBA16F:
	case PIXELFORMAT_RG11B10F: // FIXME: Conversions aren't supported in GLES
		dataformat = PIXELFORMAT_RGBA16F;
		break;
	case PIXELFORMAT_R32F:
	case PIXELFORMAT_RG32F:
	case PIXELFORMAT_RGBA32F:
		dataformat = PIXELFORMAT_RGBA32F;
		break;
	default:
		dataformat = PIXELFORMAT_RGBA8;
		break;
	}

	love::image::ImageData *imagedata = module->newImageData(w, h, dataformat);

	bool isSRGB = false;
	OpenGL::TextureFormat fmt = gl.convertPixelFormat(dataformat, false, isSRGB);

	GLuint current_fbo = gl.getFramebuffer(OpenGL::FRAMEBUFFER_ALL);
	gl.bindFramebuffer(OpenGL::FRAMEBUFFER_ALL, getFBO());

	if (slice > 0)
	{
		int layer = texType == TEXTURE_CUBE ? 0 : slice;
		int face = texType == TEXTURE_CUBE ? slice : 0;
		gl.framebufferTexture(GL_COLOR_ATTACHMENT0, texType, texture, 0, layer, face);
	}

	glReadPixels(x, y, w, h, fmt.externalformat, fmt.type, imagedata->getData());

	if (slice > 0)
		gl.framebufferTexture(GL_COLOR_ATTACHMENT0, texType, texture, 0, 0, 0);

	gl.bindFramebuffer(OpenGL::FRAMEBUFFER_ALL, current_fbo);

	return imagedata;
}

PixelFormat Canvas::getSizedFormat(PixelFormat format)
{
	switch (format)
	{
	case PIXELFORMAT_NORMAL:
		if (isGammaCorrect())
			return PIXELFORMAT_sRGBA8;
		else if (!OpenGL::isPixelFormatSupported(PIXELFORMAT_RGBA8, true, true, false))
			// 32-bit render targets don't have guaranteed support on GLES2.
			return PIXELFORMAT_RGBA4;
		else
			return PIXELFORMAT_RGBA8;
	case PIXELFORMAT_HDR:
		return PIXELFORMAT_RGBA16F;
	default:
		return format;
	}
}

bool Canvas::isSupported()
{
	return GLAD_ES_VERSION_2_0 || GLAD_VERSION_3_0 || GLAD_ARB_framebuffer_object || GLAD_EXT_framebuffer_object;
}

bool Canvas::isMultiFormatMultiCanvasSupported()
{
	return gl.getMaxRenderTargets() > 1 && (GLAD_ES_VERSION_3_0 || GLAD_VERSION_3_0 || GLAD_ARB_framebuffer_object);
}

Canvas::SupportedFormat Canvas::supportedFormats[] = {};
Canvas::SupportedFormat Canvas::checkedFormats[] = {};

bool Canvas::isFormatSupported(PixelFormat format)
{
	return isFormatSupported(format, !isPixelFormatDepthStencil(format));
}

bool Canvas::isFormatSupported(PixelFormat format, bool readable)
{
	if (!isSupported())
		return false;

	const char *fstr = "?";
	love::getConstant(format, fstr);

	bool supported = true;
	format = getSizedFormat(format);

	if (!OpenGL::isPixelFormatSupported(format, true, readable, false))
		return false;

	if (checkedFormats[format].get(readable))
		return supportedFormats[format].get(readable);

	// Even though we might have the necessary OpenGL version or extension,
	// drivers are still allowed to throw FRAMEBUFFER_UNSUPPORTED when attaching
	// a texture to a FBO whose format the driver doesn't like. So we should
	// test with an actual FBO.
	GLuint texture = 0;
	GLuint renderbuffer = 0;

	bool unusedSRGB = false;
	OpenGL::TextureFormat fmt = OpenGL::convertPixelFormat(format, readable, unusedSRGB);

	GLuint current_fbo = gl.getFramebuffer(OpenGL::FRAMEBUFFER_ALL);

	GLuint fbo = 0;
	glGenFramebuffers(1, &fbo);
	gl.bindFramebuffer(OpenGL::FRAMEBUFFER_ALL, fbo);

	// Make sure at least something is bound to a color attachment. I believe
	// this is required on ES2 but I'm not positive.
	if (isPixelFormatDepthStencil(format))
		gl.framebufferTexture(GL_COLOR_ATTACHMENT0, TEXTURE_2D, gl.getDefaultTexture(TEXTURE_2D), 0, 0, 0);

	if (readable)
	{
		glGenTextures(1, &texture);
		gl.bindTextureToUnit(TEXTURE_2D, texture, 0, false);

		Texture::Filter f;
		f.min = f.mag = Texture::FILTER_NEAREST;
		gl.setTextureFilter(TEXTURE_2D, f);

		Texture::Wrap w;
		gl.setTextureWrap(TEXTURE_2D, w);

		unusedSRGB = false;
		gl.rawTexStorage(TEXTURE_2D, 1, format, unusedSRGB, 1, 1);
	}
	else
	{
		glGenRenderbuffers(1, &renderbuffer);
		glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
		glRenderbufferStorage(GL_RENDERBUFFER, fmt.internalformat, 1, 1);
	}

	for (GLenum attachment : fmt.framebufferAttachments)
	{
		if (attachment == GL_NONE)
			continue;

		if (readable)
			gl.framebufferTexture(attachment, TEXTURE_2D, texture, 0, 0, 0);
		else
			glFramebufferRenderbuffer(GL_FRAMEBUFFER, attachment, GL_RENDERBUFFER, renderbuffer);
	}

	supported = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

	gl.bindFramebuffer(OpenGL::FRAMEBUFFER_ALL, current_fbo);
	gl.deleteFramebuffer(fbo);

	if (texture != 0)
		gl.deleteTexture(texture);

	if (renderbuffer != 0)
		glDeleteRenderbuffers(1, &renderbuffer);

	// Cache the result so we don't do this for every isFormatSupported call.
	checkedFormats[format].set(readable, true);
	supportedFormats[format].set(readable, supported);

	return supported;
}

} // opengl
} // graphics
} // love
