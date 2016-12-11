/**
 * Copyright (c) 2006-2016 LOVE Development Team
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

// LOVE
#include "common/config.h"
#include "common/math.h"
#include "common/Vector.h"

#include "Graphics.h"
#include "font/Font.h"
#include "Polyline.h"
#include "math/MathModule.h"
#include "window/Window.h"

#include "libraries/xxHash/xxhash.h"

// C++
#include <vector>
#include <sstream>
#include <algorithm>
#include <iterator>

// C
#include <cmath>
#include <cstdio>

#ifdef LOVE_IOS
#include <SDL_syswm.h>
#endif

namespace love
{
namespace graphics
{
namespace opengl
{

Graphics::Graphics()
	: quadIndices(nullptr)
	, width(0)
	, height(0)
	, created(false)
	, active(true)
	, canCaptureScreenshot(true)
	, currentPass()
	, writingToStencil(false)
	, renderPassCount(0)
{
	gl = OpenGL();

	states.reserve(10);
	states.push_back(DisplayState());

	auto window = getInstance<love::window::Window>(M_WINDOW);

	if (window != nullptr)
	{
		window->setGraphics(this);

		if (window->isOpen())
		{
			int w = 0, h = 0;
			window->getPixelDimensions(w, h);

			setMode(w, h);
		}
	}
}

Graphics::~Graphics()
{
	// We do this manually so the graphics objects are released before the window.
	states.clear();
	defaultFont.set(nullptr);

	if (Shader::defaultShader)
	{
		Shader::defaultShader->release();
		Shader::defaultShader = nullptr;
	}
	if (Shader::defaultVideoShader)
	{
		Shader::defaultVideoShader->release();
		Shader::defaultVideoShader = nullptr;
	}

	if (quadIndices)
		delete quadIndices;
}

const char *Graphics::getName() const
{
	return "love.graphics.opengl";
}

void Graphics::restoreState(const DisplayState &s)
{
	setColor(s.color);
	setBackgroundColor(s.backgroundColor);

	setBlendMode(s.blendMode, s.blendAlphaMode);

	setLineWidth(s.lineWidth);
	setLineStyle(s.lineStyle);
	setLineJoin(s.lineJoin);

	setPointSize(s.pointSize);

	if (s.scissor)
		setScissor(s.scissorRect);
	else
		setScissor();

	setStencilTest(s.stencilCompare, s.stencilTestValue);

	setFont(s.font.get());
	setShader(s.shader.get());

	setColorMask(s.colorMask);
	setWireframe(s.wireframe);

	setDefaultFilter(s.defaultFilter);
	setDefaultMipmapFilter(s.defaultMipmapFilter, s.defaultMipmapSharpness);
}

void Graphics::restoreStateChecked(const DisplayState &s)
{
	const DisplayState &cur = states.back();

	if (s.color != cur.color)
		setColor(s.color);

	setBackgroundColor(s.backgroundColor);

	if (s.blendMode != cur.blendMode || s.blendAlphaMode != cur.blendAlphaMode)
		setBlendMode(s.blendMode, s.blendAlphaMode);

	// These are just simple assignments.
	setLineWidth(s.lineWidth);
	setLineStyle(s.lineStyle);
	setLineJoin(s.lineJoin);

	if (s.pointSize != cur.pointSize)
		setPointSize(s.pointSize);

	if (s.scissor != cur.scissor || (s.scissor && !(s.scissorRect == cur.scissorRect)))
	{
		if (s.scissor)
			setScissor(s.scissorRect);
		else
			setScissor();
	}

	if (s.stencilCompare != cur.stencilCompare || s.stencilTestValue != cur.stencilTestValue)
		setStencilTest(s.stencilCompare, s.stencilTestValue);

	setFont(s.font.get());
	setShader(s.shader.get());

	if (s.colorMask != cur.colorMask)
		setColorMask(s.colorMask);

	if (s.wireframe != cur.wireframe)
		setWireframe(s.wireframe);

	setDefaultFilter(s.defaultFilter);
	setDefaultMipmapFilter(s.defaultMipmapFilter, s.defaultMipmapSharpness);
}

void Graphics::checkSetDefaultFont()
{
	// We don't create or set the default Font if an existing font is in use.
	if (states.back().font.get() != nullptr)
		return;

	// Create a new default font if we don't have one yet.
	if (!defaultFont.get())
	{
		auto fontmodule = Module::getInstance<font::Font>(M_FONT);
		if (!fontmodule)
			throw love::Exception("Font module has not been loaded.");

		auto hinting = font::TrueTypeRasterizer::HINTING_NORMAL;
		StrongRef<font::Rasterizer> r(fontmodule->newTrueTypeRasterizer(12, hinting), Acquire::NORETAIN);

		defaultFont.set(newFont(r.get()), Acquire::NORETAIN);
	}

	states.back().font.set(defaultFont.get());
}

void Graphics::setViewportSize(int width, int height)
{
	this->width = width;
	this->height = height;

	if (currentPass.active && currentPass.info.colorAttachmentCount == 0)
	{
		// Set the viewport to top-left corner.
		gl.setViewport({0, 0, width, height}, false);

		// Set up the projection matrix
		gl.matrices.projection = Matrix4::ortho(0.0, (float) width, (float) height, 0.0);
	}
}

bool Graphics::setMode(int width, int height)
{
	this->width = width;
	this->height = height;

	// Okay, setup OpenGL.
	gl.initContext();
	gl.setupContext();

	created = true;

	// Enable blending
	glEnable(GL_BLEND);

	// Auto-generated mipmaps should be the best quality possible
	glHint(GL_GENERATE_MIPMAP_HINT, GL_NICEST);

	if (!GLAD_ES_VERSION_2_0)
	{
		// Make sure antialiasing works when set elsewhere
		glEnable(GL_MULTISAMPLE);

		// Enable texturing
		glEnable(GL_TEXTURE_2D);
	}

	gl.setTextureUnit(0);

	// Set pixel row alignment
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

	// Set whether drawing converts input from linear -> sRGB colorspace.
	if (GLAD_VERSION_3_0 || GLAD_ARB_framebuffer_sRGB || GLAD_EXT_framebuffer_sRGB
		|| GLAD_ES_VERSION_3_0 || GLAD_EXT_sRGB)
	{
		if (GLAD_VERSION_1_0 || GLAD_EXT_sRGB_write_control)
			gl.setFramebufferSRGB(isGammaCorrect());
	}
	else
		setGammaCorrect(false);

	bool enabledebug = false;

	if (GLAD_VERSION_3_0)
	{
		// Enable OpenGL's debug output if a debug context has been created.
		GLint flags = 0;
		glGetIntegerv(GL_CONTEXT_FLAGS, &flags);
		enabledebug = (flags & GL_CONTEXT_FLAG_DEBUG_BIT) != 0;
	}

	setDebug(enabledebug);

	// Reload all volatile objects.
	if (!Volatile::loadAll())
		::printf("Could not reload all volatile objects.\n");

	// Create a quad indices object owned by love.graphics, so at least one
	// QuadIndices object is alive at all times while love.graphics is alive.
	// This makes sure there aren't too many expensive destruction/creations of
	// index buffer objects, since the shared index buffer used by QuadIndices
	// objects is destroyed when the last object is destroyed.
	if (quadIndices == nullptr)
		quadIndices = new QuadIndices(20);

	setViewportSize(width, height);

	// Restore the graphics state.
	restoreState(states.back());

	pixelScaleStack.clear();
	pixelScaleStack.reserve(5);
	pixelScaleStack.push_back(1);

	int gammacorrect = isGammaCorrect() ? 1 : 0;

	// We always need a default shader.
	if (!Shader::defaultShader)
	{
		Renderer renderer = GLAD_ES_VERSION_2_0 ? RENDERER_OPENGLES : RENDERER_OPENGL;
		Shader::defaultShader = newShader(Shader::defaultCode[renderer][gammacorrect]);
	}

	// and a default video shader.
	if (!Shader::defaultVideoShader)
	{
		Renderer renderer = GLAD_ES_VERSION_2_0 ? RENDERER_OPENGLES : RENDERER_OPENGL;
		Shader::defaultVideoShader = newShader(Shader::defaultVideoCode[renderer][gammacorrect]);
	}

	// A shader should always be active, but the default shader shouldn't be
	// returned by getShader(), so we don't do setShader(defaultShader).
	if (!Shader::current)
		Shader::defaultShader->attach();

	return true;
}

void Graphics::unSetMode()
{
	if (!isCreated())
		return;

	// Unload all volatile objects. These must be reloaded after the display
	// mode change.
	Volatile::unloadAll();

	for (const auto &pair : framebufferObjects)
		gl.deleteFramebuffer(pair.second);

	for (const CachedRenderbuffer &rb : stencilBuffers)
		glDeleteRenderbuffers(1, &rb.renderbuffer);

	framebufferObjects.clear();
	stencilBuffers.clear();

	gl.deInitContext();

	created = false;
}

void Graphics::setActive(bool enable)
{
	// Make sure all pending OpenGL commands have fully executed before
	// returning, when going from active to inactive. This is required on iOS.
	if (isCreated() && this->active && !enable)
		glFinish();

	active = enable;
}

bool Graphics::isActive() const
{
	// The graphics module is only completely 'active' if there's a window, a
	// context, and the active variable is set.
	auto window = getInstance<love::window::Window>(M_WINDOW);
	return active && isCreated() && window != nullptr && window->isOpen();
}

static void APIENTRY debugCB(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei /*len*/, const GLchar *msg, const GLvoid* /*usr*/)
{
	// Human-readable strings for the debug info.
	const char *sourceStr = OpenGL::debugSourceString(source);
	const char *typeStr = OpenGL::debugTypeString(type);
	const char *severityStr = OpenGL::debugSeverityString(severity);

	const char *fmt = "OpenGL: %s [source=%s, type=%s, severity=%s, id=%d]\n";
	printf(fmt, msg, sourceStr, typeStr, severityStr, id);
}

void Graphics::setDebug(bool enable)
{
	// Make sure debug output is supported. The AMD ext. is a bit different
	// so we don't make use of it, since AMD drivers now support KHR_debug.
	if (!(GLAD_VERSION_4_3 || GLAD_KHR_debug || GLAD_ARB_debug_output))
		return;

	// TODO: We don't support GL_KHR_debug in GLES yet.
	if (GLAD_ES_VERSION_2_0)
		return;

	// Ugly hack to reduce code duplication.
	if (GLAD_ARB_debug_output && !(GLAD_VERSION_4_3 || GLAD_KHR_debug))
	{
		fp_glDebugMessageCallback = (pfn_glDebugMessageCallback) fp_glDebugMessageCallbackARB;
		fp_glDebugMessageControl = (pfn_glDebugMessageControl) fp_glDebugMessageControlARB;
	}

	if (!enable)
	{
		// Disable the debug callback function.
		glDebugMessageCallback(nullptr, nullptr);

		// We can disable debug output entirely with KHR_debug.
		if (GLAD_VERSION_4_3 || GLAD_KHR_debug)
			glDisable(GL_DEBUG_OUTPUT);

		return;
	}

	// We don't want asynchronous debug output.
	glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);

	glDebugMessageCallback(debugCB, nullptr);

	// Initially, enable everything.
	glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, 0, GL_TRUE);

	// Disable messages about deprecated OpenGL functionality.
	glDebugMessageControl(GL_DEBUG_SOURCE_API, GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR, GL_DONT_CARE, 0, 0, GL_FALSE);
	glDebugMessageControl(GL_DEBUG_SOURCE_SHADER_COMPILER, GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR, GL_DONT_CARE, 0, 0, GL_FALSE);

	if (GLAD_VERSION_4_3 || GLAD_KHR_debug)
		glEnable(GL_DEBUG_OUTPUT);

	::printf("OpenGL debug output enabled (LOVE_GRAPHICS_DEBUG=1)\n");
}

void Graphics::reset()
{
	DisplayState s;
	stopDrawToStencilBuffer();
	restoreState(s);
	origin();
}

void Graphics::beginPass(PassInfo::BeginAction beginAction, Colorf clearColor)
{
	if (currentPass.active)
		throw love::Exception("Cannot call beginPass while another render pass is active!");

	currentPass.active = true;

	OpenGL::TempDebugGroup debuggroup("Render Pass begin");

	gl.bindFramebuffer(OpenGL::FRAMEBUFFER_ALL, gl.getDefaultFBO());

	gl.setViewport({0, 0, width, height}, false);

	// The projection matrix is flipped compared to rendering to a canvas, due
	// to OpenGL considering (0,0) bottom-left instead of top-left.
	gl.matrices.projection = Matrix4::ortho(0.0, (float) width, (float) height, 0.0);

	if (GLAD_VERSION_1_0 || GLAD_EXT_sRGB_write_control)
	{
		if (isGammaCorrect() && !gl.hasFramebufferSRGB())
			gl.setFramebufferSRGB(true);
		else if (!isGammaCorrect() && gl.hasFramebufferSRGB())
			gl.setFramebufferSRGB(false);
	}

	// Always clear the stencil buffer, for now.
	GLbitfield clearflags = GL_STENCIL_BUFFER_BIT | GL_DEPTH_BUFFER_BIT;

	if (beginAction == PassInfo::BEGIN_CLEAR)
	{
		Colorf c = clearColor;
		gammaCorrectColor(c);

		glClearColor(c.r, c.g, c.b, c.a);
		clearflags |= GL_COLOR_BUFFER_BIT;
	}

	if (clearflags != 0)
		glClear(clearflags);

	PassInfo info;
	info.colorAttachmentCount = 0;
	info.stencil = true;

	currentPass.info = info;

	renderPassCount++;

	canCaptureScreenshot = false;
}

void Graphics::beginPass(const PassInfo &info)
{
	if (info.colorAttachmentCount == 0)
		throw love::Exception("At least one Canvas must be specified for an off-screen render pass.");

	if (currentPass.active)
		throw love::Exception("Cannot call beginPass while another render pass is active!");

	if (info.colorAttachmentCount > gl.getMaxRenderTargets())
		throw love::Exception("This system can't simultaneously render to %d canvases.", info.colorAttachmentCount);

	Canvas *firstcanvas = info.colorAttachments[0].canvas;

	bool multiformatsupported = isSupported(Feature::FEATURE_MULTI_CANVAS_FORMATS);
	PixelFormat firstformat = firstcanvas->getPixelFormat();

	bool hasSRGBcanvas = firstformat == PIXELFORMAT_sRGBA8;

	for (int i = 1; i < info.colorAttachmentCount; i++)
	{
		Canvas *c = info.colorAttachments[i].canvas;

		if (c->getWidth() != firstcanvas->getWidth() || c->getHeight() != firstcanvas->getHeight())
			throw love::Exception("All canvases in a render pass must have the same dimensions.");

		if (!multiformatsupported && c->getPixelFormat() != firstformat)
			throw love::Exception("This system doesn't support multi-canvas rendering with different canvas formats.");

		if (c->getRequestedMSAA() != firstcanvas->getRequestedMSAA())
			throw love::Exception("All Canvases in a render pass must have the same requested MSAA value.");

		if (c->getPixelFormat() == PIXELFORMAT_sRGBA8)
			hasSRGBcanvas = true;
	}

	OpenGL::TempDebugGroup debuggroup("Render Pass begin");

	bindCachedFBOForPass(info);

	currentPass.active = true;

	int w = firstcanvas->getWidth();
	int h = firstcanvas->getHeight();

	gl.setViewport({0, 0, w, h}, true);
	gl.matrices.projection = Matrix4::ortho(0.0, (float) w, 0.0, (float) h);

	// Make sure the correct sRGB setting is used when drawing to the canvases.
	if (GLAD_VERSION_1_0 || GLAD_EXT_sRGB_write_control)
	{
		if (hasSRGBcanvas && !gl.hasFramebufferSRGB())
			gl.setFramebufferSRGB(true);
		else if (!hasSRGBcanvas && gl.hasFramebufferSRGB())
			gl.setFramebufferSRGB(false);
	}

	GLbitfield clearflags = 0;

	// Take a single-color clear codepath if there's only one specified Canvas.
	// The multi-color codepath (in the loop below) assumes MRT functions are
	// available, and also may call more expensive GL functions which are
	// unnecessary if only one Canvas is used.
	if (info.colorAttachmentCount <= 1)
	{
		if (info.colorAttachmentCount > 0 && info.colorAttachments[0].beginAction == PassInfo::BEGIN_CLEAR)
		{
			clearflags |= GL_COLOR_BUFFER_BIT;
			Colorf c = info.colorAttachments[0].clearColor;
			gammaCorrectColor(c);
			glClearColor(c.r, c.g, c.b, c.a);
		}
	}
	else
	{
		bool drawbuffermodified = false;

		for (int i = 0; i < info.colorAttachmentCount; i++)
		{
			if (info.colorAttachments[i].beginAction == PassInfo::BEGIN_CLEAR)
			{
				Colorf c = info.colorAttachments[i].clearColor;
				gammaCorrectColor(c);

				if (GLAD_ES_VERSION_3_0 || GLAD_VERSION_3_0)
				{
					const GLfloat carray[] = {c.r, c.g, c.b, c.a};
					glClearBufferfv(GL_COLOR, i, carray);
				}
				else
				{
					glDrawBuffer(GL_COLOR_ATTACHMENT0 + i);
					glClearColor(c.r, c.g, c.b, c.a);
					glClear(GL_COLOR_BUFFER_BIT);

					drawbuffermodified = true;
				}
			}
		}

		// Revert to the expected draw buffers once we're done, if glClearBuffer
		// wasn't supported.
		if (drawbuffermodified)
		{
			std::vector<GLenum> bufs;

			for (int i = 0; i < info.colorAttachmentCount; i++)
				bufs.push_back(GL_COLOR_ATTACHMENT0 + i);

			glDrawBuffers((int) bufs.size(), &bufs[0]);
		}
	}

	if (info.stencil)
		clearflags |= GL_STENCIL_BUFFER_BIT | GL_DEPTH_BUFFER_BIT;

	if (clearflags != 0)
		glClear(clearflags);

	for (int i = 0; i < info.colorAttachmentCount; i++)
		info.colorAttachments[i].canvas->retain();

	currentPass.info = info;
	renderPassCount++;

	if (gl.bugs.clearRequiresDriverTextureStateUpdate && Shader::current)
	{
		// This seems to be enough to fix the bug for me. Other methods I've
		// tried (e.g. dummy draws) don't work in all cases.
		gl.useProgram(0);
		gl.useProgram(Shader::current->getProgram());
	}
}

void Graphics::endPass()
{
	endPass(0, 0, 0, 0, nullptr, nullptr);
}

void Graphics::endPass(int sX, int sY, int sW, int sH, const ScreenshotInfo *info, void *screenshotCallbackData)
{
	if (!currentPass.active)
		return; // Should this error instead?

	StrongRef<love::image::ImageData> imagedata;

	auto &attachments = currentPass.info.colorAttachments;
	int attachmentcount = currentPass.info.colorAttachmentCount;

	if (info != nullptr)
	{
		if (attachmentcount == 0)
			throw love::Exception("Use captureScreenshot to capture the main screen's contents.");

		if (sX < 0 || sY < 0 || sW <= 0 || sH <= 0 || (sX + sW) > getPassWidth() || (sY + sH) > getPassHeight())
			throw love::Exception("Invalid rectangle dimensions.");

		auto imagemodule = Module::getInstance<image::Image>(M_IMAGE);

		if (imagemodule == nullptr)
			throw love::Exception("The love.image module must be loaded to capture a Canvas' contents.");

		PixelFormat format;
		switch (attachments[0].canvas->getPixelFormat())
		{
		case PIXELFORMAT_RGB10A2: // FIXME: Conversions aren't supported in GLES
			format = PIXELFORMAT_RGBA16;
			break;
		case PIXELFORMAT_R16F:
		case PIXELFORMAT_RG16F:
		case PIXELFORMAT_RGBA16F:
		case PIXELFORMAT_RG11B10F: // FIXME: Conversions aren't supported in GLES
			format = PIXELFORMAT_RGBA16F;
			break;
		case PIXELFORMAT_R32F:
		case PIXELFORMAT_RG32F:
		case PIXELFORMAT_RGBA32F:
			format = PIXELFORMAT_RGBA32F;
			break;
		default:
			format = PIXELFORMAT_RGBA8;
			break;
		}

		imagedata.set(imagemodule->newImageData(sW, sH, format), Acquire::NORETAIN);
	}

	if (currentPass.info.stencil)
		discard(OpenGL::FRAMEBUFFER_ALL, {}, true);

	if (attachmentcount > 0 && attachments[0].canvas->getMSAA() > 1)
	{
		int w = attachments[0].canvas->getWidth();
		int h = attachments[0].canvas->getHeight();

		for (int i = 0; i < attachmentcount; i++)
		{
			glReadBuffer(GL_COLOR_ATTACHMENT0 + i);

			gl.bindFramebuffer(OpenGL::FRAMEBUFFER_DRAW, attachments[i].canvas->getFBO());

			if (GLAD_APPLE_framebuffer_multisample)
				glResolveMultisampleFramebufferAPPLE();
			else
				glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		}
	}

	if (info != nullptr)
	{
		if (attachments[0].canvas->getMSAA() > 1)
			gl.bindFramebuffer(OpenGL::FRAMEBUFFER_READ, attachments[0].canvas->getFBO());
		else if (attachmentcount > 1)
			glReadBuffer(GL_COLOR_ATTACHMENT0);

		GLenum datatype;
		switch (imagedata->getFormat())
		{
		case PIXELFORMAT_RGBA16:
			datatype = GL_UNSIGNED_SHORT;
			break;
		case PIXELFORMAT_RGBA16F:
			datatype = GL_HALF_FLOAT;
			break;
		case PIXELFORMAT_RGBA32F:
			datatype = GL_FLOAT;
			break;
		default:
			datatype = GL_UNSIGNED_BYTE;
			break;
		}

		glReadPixels(sX, sY, sW, sH, GL_RGBA, datatype, imagedata->getData());

		info->callback(imagedata, info->ref, screenshotCallbackData);
	}

	for (int i = 0; i < currentPass.info.colorAttachmentCount; i++)
		currentPass.info.colorAttachments[i].canvas->release();

	currentPass.active = false;
}

const PassInfo &Graphics::getActivePass() const
{
	return currentPass.info;
}

bool Graphics::isPassActive() const
{
	return currentPass.active;
}

void Graphics::discard(OpenGL::FramebufferTarget target, const std::vector<bool> &colorbuffers, bool depthstencil)
{
	if (!(GLAD_VERSION_4_3 || GLAD_ARB_invalidate_subdata || GLAD_ES_VERSION_3_0 || GLAD_EXT_discard_framebuffer))
		return;

	GLenum gltarget = GL_FRAMEBUFFER;
	if (target == OpenGL::FRAMEBUFFER_READ)
		gltarget = GL_READ_FRAMEBUFFER;
	else if (target == OpenGL::FRAMEBUFFER_DRAW)
		gltarget = GL_DRAW_FRAMEBUFFER;

	std::vector<GLenum> attachments;
	attachments.reserve(colorbuffers.size());

	// glDiscardFramebuffer uses different attachment enums for the default FBO.
	if (currentPass.info.colorAttachmentCount == 0 && gl.getDefaultFBO() == 0)
	{
		if (colorbuffers.size() > 0 && colorbuffers[0])
			attachments.push_back(GL_COLOR);

		if (depthstencil)
		{
			attachments.push_back(GL_STENCIL);
			attachments.push_back(GL_DEPTH);
		}
	}
	else
	{
		int rendertargetcount = std::max(currentPass.info.colorAttachmentCount, 1);

		for (int i = 0; i < (int) colorbuffers.size(); i++)
		{
			if (colorbuffers[i] && i < rendertargetcount)
				attachments.push_back(GL_COLOR_ATTACHMENT0 + i);
		}

		if (depthstencil)
		{
			attachments.push_back(GL_STENCIL_ATTACHMENT);
			attachments.push_back(GL_DEPTH_ATTACHMENT);
		}
	}

	// Hint for the driver that it doesn't need to save these buffers.
	if (GLAD_VERSION_4_3 || GLAD_ARB_invalidate_subdata || GLAD_ES_VERSION_3_0)
		glInvalidateFramebuffer(gltarget, (GLint) attachments.size(), &attachments[0]);
	else if (GLAD_EXT_discard_framebuffer)
		glDiscardFramebufferEXT(gltarget, (GLint) attachments.size(), &attachments[0]);
}

void Graphics::bindCachedFBOForPass(const PassInfo &pass)
{
	PassBufferInfo info;
	memset(&info, 0, sizeof(PassBufferInfo));

	info.stencil = pass.stencil;

	int ncanvases = pass.colorAttachmentCount;

	for (int i = 0; i < ncanvases; i++)
		info.canvases[i] = pass.colorAttachments[i].canvas;

	uint32 hash = XXH32(&info, offsetof(PassBufferInfo, canvases) + ncanvases * sizeof(Canvas *), 0);

	GLuint fbo = framebufferObjects[hash];

	if (fbo != 0)
	{
		gl.bindFramebuffer(OpenGL::FRAMEBUFFER_ALL, fbo);
	}
	else
	{
		int w = info.canvases[0]->getWidth();
		int h = info.canvases[0]->getHeight();
		int msaa = std::max(info.canvases[0]->getMSAA(), 1);

		glGenFramebuffers(1, &fbo);
		gl.bindFramebuffer(OpenGL::FRAMEBUFFER_ALL, fbo);

		std::vector<GLenum> drawbuffers;
		drawbuffers.reserve(ncanvases);

		for (int i = 0; i < ncanvases; i++)
		{
			drawbuffers.push_back(GL_COLOR_ATTACHMENT0 + i);

			if (msaa > 1)
			{
				GLuint rbo = (GLuint) info.canvases[i]->getMSAAHandle();
				glFramebufferRenderbuffer(GL_FRAMEBUFFER, drawbuffers[i], GL_RENDERBUFFER, rbo);
			}
			else
			{
				GLuint tex = *(GLuint *) info.canvases[i]->getHandle();
				glFramebufferTexture2D(GL_FRAMEBUFFER, drawbuffers[i], GL_TEXTURE_2D, tex, 0);
			}
		}

		if (drawbuffers.size() > 1)
			glDrawBuffers((int) drawbuffers.size(), &drawbuffers[0]);

		GLuint stencil = attachCachedStencilBuffer(w, h, info.canvases[0]->getRequestedMSAA());

		if (stencil == 0)
		{
			gl.deleteFramebuffer(fbo);
			gl.bindFramebuffer(OpenGL::FRAMEBUFFER_ALL, gl.getDefaultFBO());
			throw love::Exception("Could not create stencil buffer!");
		}

		GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);

		if (status != GL_FRAMEBUFFER_COMPLETE)
		{
			gl.deleteFramebuffer(fbo);
			const char *sstr = OpenGL::framebufferStatusString(status);
			throw love::Exception("Could not create Framebuffer Object! %s", sstr);
		}

		framebufferObjects[hash] = fbo;
	}
}

GLuint Graphics::attachCachedStencilBuffer(int w, int h, int samples)
{
	samples = samples == 1 ? 0 : samples;

	for (const CachedRenderbuffer &rb : stencilBuffers)
	{
		if (rb.w == w && rb.h == h && rb.samples == samples)
		{
			// Attach the buffer to the framebuffer object.
			for (GLenum attachment : rb.attachments)
			{
				if (attachment != GL_NONE)
					glFramebufferRenderbuffer(GL_FRAMEBUFFER, attachment, GL_RENDERBUFFER, rb.renderbuffer);
			}

			return rb.renderbuffer;
		}
	}

	OpenGL::TempDebugGroup debuggroup("Created cached stencil buffer");

	CachedRenderbuffer rb;
	rb.w = w;
	rb.h = h;
	rb.samples = samples;

	rb.attachments[0] = GL_STENCIL_ATTACHMENT;
	rb.attachments[1] = GL_NONE;

	GLenum format = GL_STENCIL_INDEX8;

	// Prefer a combined depth/stencil buffer.
	if (GLAD_ES_VERSION_3_0 || GLAD_VERSION_3_0 || GLAD_ARB_framebuffer_object)
	{
		format = GL_DEPTH24_STENCIL8;
		rb.attachments[0] = GL_DEPTH_STENCIL_ATTACHMENT;
	}
	else if (GLAD_EXT_packed_depth_stencil || GLAD_OES_packed_depth_stencil)
	{
		format = GL_DEPTH24_STENCIL8;
		rb.attachments[0] = GL_DEPTH_ATTACHMENT;
		rb.attachments[1] = GL_STENCIL_ATTACHMENT;
	}

	glGenRenderbuffers(1, &rb.renderbuffer);
	glBindRenderbuffer(GL_RENDERBUFFER, rb.renderbuffer);

	if (rb.samples > 1)
		glRenderbufferStorageMultisample(GL_RENDERBUFFER, rb.samples, format, rb.w, rb.h);
	else
		glRenderbufferStorage(GL_RENDERBUFFER, format, rb.w, rb.h);

	// Attach the buffer to the framebuffer object.
	for (GLenum attachment : rb.attachments)
	{
		if (attachment != GL_NONE)
			glFramebufferRenderbuffer(GL_FRAMEBUFFER, attachment, GL_RENDERBUFFER, rb.renderbuffer);
	}

	glBindRenderbuffer(GL_RENDERBUFFER, 0);

	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
	{
		glDeleteRenderbuffers(1, &rb.renderbuffer);
		rb.renderbuffer = 0;
	}

	if (rb.renderbuffer != 0)
		stencilBuffers.push_back(rb);

	return rb.renderbuffer;
}

void Graphics::captureScreenshot(const ScreenshotInfo &info)
{
	if (!canCaptureScreenshot)
		throw love::Exception("captureScreenshot cannot be called once rendering to the main screen has begun.");

	pendingScreenshotCallbacks.push_back(info);
}

void Graphics::present(void *screenshotCallbackData)
{
	if (!isActive())
		return;

	if (currentPass.active)
		throw love::Exception("present cannot be called while a render pass is active.");

	gl.bindFramebuffer(OpenGL::FRAMEBUFFER_ALL, gl.getDefaultFBO());

	if (!pendingScreenshotCallbacks.empty())
	{
		int w = getWidth();
		int h = getHeight();

		size_t row = 4 * w;
		size_t size = row * h;

		GLubyte *pixels = nullptr;
		GLubyte *screenshot = nullptr;

		try
		{
			pixels = new GLubyte[size];
			screenshot = new GLubyte[size];
		}
		catch (std::exception &)
		{
			delete[] pixels;
			delete[] screenshot;
			throw love::Exception("Out of memory.");
		}

#ifdef LOVE_IOS
		SDL_SysWMinfo info = {};
		SDL_VERSION(&info.version);
		SDL_GetWindowWMInfo(SDL_GL_GetCurrentWindow(), &info);

		if (info.info.uikit.resolveFramebuffer != 0)
		{
			gl.bindFramebuffer(OpenGL::FRAMEBUFFER_DRAW, info.info.uikit.resolveFramebuffer);

			// We need to do an explicit MSAA resolve on iOS, because it uses GLES
			// FBOs rather than a system framebuffer.
			if (GLAD_ES_VERSION_3_0)
				glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
			else if (GLAD_APPLE_framebuffer_multisample)
				glResolveMultisampleFramebufferAPPLE();

			gl.bindFramebuffer(OpenGL::FRAMEBUFFER_READ, info.info.uikit.resolveFramebuffer);
		}
#endif

		glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

		// Replace alpha values with full opacity.
		for (size_t i = 3; i < size; i += 4)
			pixels[i] = 255;

		// OpenGL sucks and reads pixels from the lower-left. Let's fix that.
		GLubyte *src = pixels - row;
		GLubyte *dst = screenshot + size;

		for (int i = 0; i < h; ++i)
			memcpy(dst-=row, src+=row, row);

		delete[] pixels;

		auto imagemodule = Module::getInstance<love::image::Image>(M_IMAGE);

		for (int i = 0; i < (int) pendingScreenshotCallbacks.size(); i++)
		{
			const auto &info = pendingScreenshotCallbacks[i];
			image::ImageData *img = nullptr;

			try
			{
				img = imagemodule->newImageData(w, h, PIXELFORMAT_RGBA8, screenshot);
			}
			catch (love::Exception &)
			{
				delete[] screenshot;
				info.callback(nullptr, info.ref, nullptr);
				for (int j = i + 1; j < (int) pendingScreenshotCallbacks.size(); j++)
				{
					const auto &ninfo = pendingScreenshotCallbacks[j];
					ninfo.callback(nullptr, ninfo.ref, nullptr);
				}
				pendingScreenshotCallbacks.clear();
				throw;
			}

			info.callback(img, info.ref, screenshotCallbackData);
			img->release();
		}

		delete[] screenshot;
		pendingScreenshotCallbacks.clear();
	}

#ifdef LOVE_IOS
	// Hack: SDL's color renderbuffer must be bound when swapBuffers is called.
	SDL_SysWMinfo info = {};
	SDL_VERSION(&info.version);
	SDL_GetWindowWMInfo(SDL_GL_GetCurrentWindow(), &info);
	glBindRenderbuffer(GL_RENDERBUFFER, info.info.uikit.colorbuffer);
#endif

	auto window = getInstance<love::window::Window>(M_WINDOW);
	if (window != nullptr)
		window->swapBuffers();

	// Reset the per-frame stat counts.
	gl.stats.drawCalls = 0;
	gl.stats.shaderSwitches = 0;
	renderPassCount = 0;

	canCaptureScreenshot = true;
}

int Graphics::getWidth() const
{
	return width;
}

int Graphics::getHeight() const
{
	return height;
}

int Graphics::getPassWidth() const
{
	if (currentPass.active && currentPass.info.colorAttachmentCount > 0)
		return currentPass.info.colorAttachments[0].canvas->getWidth();
	else
		return width;
}

int Graphics::getPassHeight() const
{
	if (currentPass.active && currentPass.info.colorAttachmentCount > 0)
		return currentPass.info.colorAttachments[0].canvas->getHeight();
	else
		return height;
}

bool Graphics::isCreated() const
{
	return created;
}

void Graphics::setScissor(const Rect &rect)
{
	glEnable(GL_SCISSOR_TEST);
	// OpenGL's reversed y-coordinate is compensated for in OpenGL::setScissor.
	gl.setScissor({rect.x, rect.y, rect.w, rect.h}, currentPass.info.colorAttachmentCount > 0);

	states.back().scissor = true;
	states.back().scissorRect = rect;
}

void Graphics::intersectScissor(const Rect &rect)
{
	Rect currect = states.back().scissorRect;

	if (!states.back().scissor)
	{
		currect.x = 0;
		currect.y = 0;
		currect.w = std::numeric_limits<int>::max();
		currect.h = std::numeric_limits<int>::max();
	}

	int x1 = std::max(currect.x, rect.x);
	int y1 = std::max(currect.y, rect.y);

	int x2 = std::min(currect.x + currect.w, rect.x + rect.w);
	int y2 = std::min(currect.y + currect.h, rect.y + rect.h);

	Rect newrect = {x1, y1, std::max(0, x2 - x1), std::max(0, y2 - y1)};
	setScissor(newrect);
}

void Graphics::setScissor()
{
	states.back().scissor = false;
	glDisable(GL_SCISSOR_TEST);
}

bool Graphics::getScissor(Rect &rect) const
{
	const DisplayState &state = states.back();
	rect = state.scissorRect;
	return state.scissor;
}

void Graphics::drawToStencilBuffer(StencilAction action, int value)
{
	if (!currentPass.active || !currentPass.info.stencil)
		throw love::Exception("Stenciling must be enabled in the active render pass.");

	writingToStencil = true;

	// Disable color writes but don't save the state for it.
	glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

	GLenum glaction = GL_REPLACE;

	switch (action)
	{
	case STENCIL_REPLACE:
	default:
		glaction = GL_REPLACE;
		break;
	case STENCIL_INCREMENT:
		glaction = GL_INCR;
		break;
	case STENCIL_DECREMENT:
		glaction = GL_DECR;
		break;
	case STENCIL_INCREMENT_WRAP:
		glaction = GL_INCR_WRAP;
		break;
	case STENCIL_DECREMENT_WRAP:
		glaction = GL_DECR_WRAP;
		break;
	case STENCIL_INVERT:
		glaction = GL_INVERT;
		break;
	}

	// The stencil test must be enabled in order to write to the stencil buffer.
	glEnable(GL_STENCIL_TEST);
	glStencilFunc(GL_ALWAYS, value, 0xFFFFFFFF);
	glStencilOp(GL_KEEP, GL_KEEP, glaction);
}

void Graphics::stopDrawToStencilBuffer()
{
	if (!writingToStencil)
		return;

	writingToStencil = false;

	const DisplayState &state = states.back();

	// Revert the color write mask.
	setColorMask(state.colorMask);

	// Use the user-set stencil test state when writes are disabled.
	setStencilTest(state.stencilCompare, state.stencilTestValue);
}

void Graphics::setStencilTest(CompareMode compare, int value)
{
	if (!currentPass.info.stencil && compare != COMPARE_ALWAYS)
		throw love::Exception("Stenciling must be enabled in the active render pass.");

	DisplayState &state = states.back();
	state.stencilCompare = compare;
	state.stencilTestValue = value;

	if (writingToStencil)
		return;

	if (compare == COMPARE_ALWAYS)
	{
		glDisable(GL_STENCIL_TEST);
		return;
	}

	GLenum glcompare = GL_EQUAL;

	/**
	 * Q: Why are some of the compare modes inverted (e.g. COMPARE_LESS becomes
	 * GL_GREATER)?
	 *
	 * A: OpenGL / GPUs do the comparison in the opposite way that makes sense
	 * for this API. For example, if the compare function is GL_GREATER then the
	 * stencil test will pass if the reference value is greater than the value
	 * in the stencil buffer. With our API it's more intuitive to assume that
	 * setStencilTest(COMPARE_GREATER, 4) will make it pass if the stencil
	 * buffer has a value greater than 4.
	 **/

	switch (compare)
	{
	case COMPARE_LESS:
		glcompare = GL_GREATER;
		break;
	case COMPARE_LEQUAL:
		glcompare = GL_GEQUAL;
		break;
	case COMPARE_EQUAL:
	default:
		glcompare = GL_EQUAL;
		break;
	case COMPARE_GEQUAL:
		glcompare = GL_LEQUAL;
		break;
	case COMPARE_GREATER:
		glcompare = GL_LESS;
		break;
	case COMPARE_NOTEQUAL:
		glcompare = GL_NOTEQUAL;
		break;
	case COMPARE_ALWAYS:
		glcompare = GL_ALWAYS;
		break;
	}

	glEnable(GL_STENCIL_TEST);
	glStencilFunc(glcompare, value, 0xFFFFFFFF);
	glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
}

void Graphics::setStencilTest()
{
	setStencilTest(COMPARE_ALWAYS, 0);
}

void Graphics::getStencilTest(CompareMode &compare, int &value)
{
	const DisplayState &state = states.back();
	compare = state.stencilCompare;
	value = state.stencilTestValue;
}

void Graphics::clearStencil()
{
	glClear(GL_STENCIL_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

Image *Graphics::newImage(const std::vector<love::image::ImageData *> &data, const Image::Flags &flags)
{
	return new Image(data, flags);
}

Image *Graphics::newImage(const std::vector<love::image::CompressedImageData *> &cdata, const Image::Flags &flags)
{
	return new Image(cdata, flags);
}

Quad *Graphics::newQuad(Quad::Viewport v, double sw, double sh)
{
	return new Quad(v, sw, sh);
}

Font *Graphics::newFont(love::font::Rasterizer *r, const Texture::Filter &filter)
{
	return new Font(r, filter);
}

SpriteBatch *Graphics::newSpriteBatch(Texture *texture, int size, Mesh::Usage usage)
{
	return new SpriteBatch(texture, size, usage);
}

ParticleSystem *Graphics::newParticleSystem(Texture *texture, int size)
{
	return new ParticleSystem(texture, size);
}

Canvas *Graphics::newCanvas(int width, int height, PixelFormat format, int msaa)
{
	if (!Canvas::isSupported())
		throw love::Exception("Canvases are not supported by your OpenGL drivers!");

	if (!Canvas::isFormatSupported(format))
	{
		const char *fstr = "rgba8";
		love::getConstant(Canvas::getSizedFormat(format), fstr);
		throw love::Exception("The %s canvas format is not supported by your OpenGL drivers.", fstr);
	}

	if (width > gl.getMaxTextureSize())
		throw Exception("Cannot create canvas: width of %d pixels is too large for this system.", width);
	else if (height > gl.getMaxTextureSize())
		throw Exception("Cannot create canvas: height of %d pixels is too large for this system.", height);

	Canvas *canvas = new Canvas(width, height, format, msaa);
	GLenum err = canvas->getStatus();

	// everything ok, return canvas (early out)
	if (err == GL_FRAMEBUFFER_COMPLETE)
		return canvas;

	canvas->release();
	throw love::Exception("Cannot create Canvas: %s", OpenGL::framebufferStatusString(err));
	return nullptr; // never reached
}

Shader *Graphics::newShader(const Shader::ShaderSource &source)
{
	return new Shader(source);
}

Mesh *Graphics::newMesh(const std::vector<Vertex> &vertices, Mesh::DrawMode drawmode, Mesh::Usage usage)
{
	return new Mesh(vertices, drawmode, usage);
}

Mesh *Graphics::newMesh(int vertexcount, Mesh::DrawMode drawmode, Mesh::Usage usage)
{
	return new Mesh(vertexcount, drawmode, usage);
}

Mesh *Graphics::newMesh(const std::vector<Mesh::AttribFormat> &vertexformat, int vertexcount, Mesh::DrawMode drawmode, Mesh::Usage usage)
{
	return new Mesh(vertexformat, vertexcount, drawmode, usage);
}

Mesh *Graphics::newMesh(const std::vector<Mesh::AttribFormat> &vertexformat, const void *data, size_t datasize, Mesh::DrawMode drawmode, Mesh::Usage usage)
{
	return new Mesh(vertexformat, data, datasize, drawmode, usage);
}

Text *Graphics::newText(Font *font, const std::vector<Font::ColoredString> &text)
{
	return new Text(font, text);
}

Video *Graphics::newVideo(love::video::VideoStream *stream)
{
	return new Video(stream);
}

bool Graphics::isGammaCorrect() const
{
	return love::graphics::isGammaCorrect();
}

void Graphics::setColor(Colorf c)
{
	Colorf nc = c;
	gammaCorrectColor(nc);
	glVertexAttrib4f(ATTRIB_CONSTANTCOLOR, nc.r, nc.g, nc.b, nc.a);

	states.back().color = c;
}

Colorf Graphics::getColor() const
{
	return states.back().color;
}

void Graphics::setBackgroundColor(Colorf c)
{
	states.back().backgroundColor = c;
}

Colorf Graphics::getBackgroundColor() const
{
	return states.back().backgroundColor;
}

void Graphics::setFont(Font *font)
{
	// We don't need to set a default font here if null is passed in, since we
	// only care about the default font in getFont and print.

	DisplayState &state = states.back();
	state.font.set(font);
}

Font *Graphics::getFont()
{
	checkSetDefaultFont();
	return states.back().font.get();
}

void Graphics::setShader(Shader *shader)
{
	if (shader == nullptr)
		return setShader();

	DisplayState &state = states.back();

	shader->attach();

	state.shader.set(shader);
}

void Graphics::setShader()
{
	DisplayState &state = states.back();

	// This will activate the default shader.
	Shader::detach();

	state.shader.set(nullptr);
}

Shader *Graphics::getShader() const
{
	return states.back().shader.get();
}

void Graphics::setColorMask(ColorMask mask)
{
	glColorMask(mask.r, mask.g, mask.b, mask.a);
	states.back().colorMask = mask;
}

Graphics::ColorMask Graphics::getColorMask() const
{
	return states.back().colorMask;
}

void Graphics::setBlendMode(BlendMode mode, BlendAlpha alphamode)
{
	GLenum func   = GL_FUNC_ADD;
	GLenum srcRGB = GL_ONE;
	GLenum srcA   = GL_ONE;
	GLenum dstRGB = GL_ZERO;
	GLenum dstA   = GL_ZERO;

	if (mode == BLEND_LIGHTEN || mode == BLEND_DARKEN)
	{
		if (!isSupported(FEATURE_LIGHTEN))
			throw love::Exception("The 'lighten' and 'darken' blend modes are not supported on this system.");
	}

	if (alphamode != BLENDALPHA_PREMULTIPLIED)
	{
		const char *modestr = "unknown";
		switch (mode)
		{
		case BLEND_LIGHTEN:
		case BLEND_DARKEN:
		case BLEND_MULTIPLY:
			getConstant(mode, modestr);
			throw love::Exception("The '%s' blend mode must be used with premultiplied alpha.", modestr);
			break;
		default:
			break;
		}
	}

	switch (mode)
	{
	case BLEND_ALPHA:
		srcRGB = srcA = GL_ONE;
		dstRGB = dstA = GL_ONE_MINUS_SRC_ALPHA;
		break;
	case BLEND_MULTIPLY:
		srcRGB = srcA = GL_DST_COLOR;
		dstRGB = dstA = GL_ZERO;
		break;
	case BLEND_SUBTRACT:
		func = GL_FUNC_REVERSE_SUBTRACT;
	case BLEND_ADD:
		srcRGB = GL_ONE;
		srcA = GL_ZERO;
		dstRGB = dstA = GL_ONE;
		break;
	case BLEND_LIGHTEN:
		func = GL_MAX;
		break;
	case BLEND_DARKEN:
		func = GL_MIN;
		break;
	case BLEND_SCREEN:
		srcRGB = srcA = GL_ONE;
		dstRGB = dstA = GL_ONE_MINUS_SRC_COLOR;
		break;
	case BLEND_REPLACE:
	default:
		srcRGB = srcA = GL_ONE;
		dstRGB = dstA = GL_ZERO;
		break;
	}

	// We can only do alpha-multiplication when srcRGB would have been unmodified.
	if (srcRGB == GL_ONE && alphamode == BLENDALPHA_MULTIPLY)
		srcRGB = GL_SRC_ALPHA;

	glBlendEquation(func);
	glBlendFuncSeparate(srcRGB, dstRGB, srcA, dstA);

	states.back().blendMode = mode;
	states.back().blendAlphaMode = alphamode;
}

Graphics::BlendMode Graphics::getBlendMode(BlendAlpha &alphamode) const
{
	alphamode = states.back().blendAlphaMode;
	return states.back().blendMode;
}

void Graphics::setDefaultFilter(const Texture::Filter &f)
{
	Texture::setDefaultFilter(f);
	states.back().defaultFilter = f;
}

const Texture::Filter &Graphics::getDefaultFilter() const
{
	return Texture::getDefaultFilter();
}

void Graphics::setDefaultMipmapFilter(Texture::FilterMode filter, float sharpness)
{
	Image::setDefaultMipmapFilter(filter);
	Image::setDefaultMipmapSharpness(sharpness);

	states.back().defaultMipmapFilter = filter;
	states.back().defaultMipmapSharpness = sharpness;
}

void Graphics::getDefaultMipmapFilter(Texture::FilterMode *filter, float *sharpness) const
{
	*filter = Image::getDefaultMipmapFilter();
	*sharpness = Image::getDefaultMipmapSharpness();
}

void Graphics::setLineWidth(float width)
{
	states.back().lineWidth = width;
}

void Graphics::setLineStyle(Graphics::LineStyle style)
{
	states.back().lineStyle = style;
}

void Graphics::setLineJoin(Graphics::LineJoin join)
{
	states.back().lineJoin = join;
}

float Graphics::getLineWidth() const
{
	return states.back().lineWidth;
}

Graphics::LineStyle Graphics::getLineStyle() const
{
	return states.back().lineStyle;
}

Graphics::LineJoin Graphics::getLineJoin() const
{
	return states.back().lineJoin;
}

void Graphics::setPointSize(float size)
{
	gl.setPointSize(size);
	states.back().pointSize = size;
}

float Graphics::getPointSize() const
{
	return states.back().pointSize;
}

void Graphics::setWireframe(bool enable)
{
	// Not supported in OpenGL ES.
	if (GLAD_ES_VERSION_2_0)
		return;

	glPolygonMode(GL_FRONT_AND_BACK, enable ? GL_LINE : GL_FILL);
	states.back().wireframe = enable;
}

bool Graphics::isWireframe() const
{
	return states.back().wireframe;
}

void Graphics::draw(Drawable *drawable, const Matrix4 &m)
{
	if (!currentPass.active)
		throw RenderOutsidePassException();

	drawable->draw(m);
}

void Graphics::drawq(Texture *texture, Quad *quad, const Matrix4 &m)
{
	if (!currentPass.active)
		throw RenderOutsidePassException();

	texture->drawq(quad, m);
}

void Graphics::print(const std::vector<Font::ColoredString> &str, const Matrix4 &m)
{
	if (!currentPass.active)
		throw RenderOutsidePassException();

	checkSetDefaultFont();

	DisplayState &state = states.back();

	if (state.font.get() != nullptr)
		state.font->print(str, m);
}

void Graphics::printf(const std::vector<Font::ColoredString> &str, float wrap, Font::AlignMode align, const Matrix4 &m)
{
	if (!currentPass.active)
		throw RenderOutsidePassException();

	checkSetDefaultFont();

	DisplayState &state = states.back();

	if (state.font.get() != nullptr)
		state.font->printf(str, wrap, align, m);
}

/**
 * Primitives
 **/

void Graphics::points(const float *coords, const uint8 *colors, size_t numpoints)
{
	if (!currentPass.active)
		throw RenderOutsidePassException();

	OpenGL::TempDebugGroup debuggroup("Graphics points draw");

	gl.prepareDraw();
	gl.bindTextureToUnit(gl.getDefaultTexture(), 0, false);
	gl.bindBuffer(BUFFER_VERTEX, 0);

	uint32 attribflags = ATTRIBFLAG_POS;
	glVertexAttribPointer(ATTRIB_POS, 2, GL_FLOAT, GL_FALSE, 0, coords);

	if (colors)
	{
		attribflags |= ATTRIBFLAG_COLOR;
		glVertexAttribPointer(ATTRIB_COLOR, 4, GL_UNSIGNED_BYTE, GL_TRUE, 0, colors);
	}

	gl.useVertexAttribArrays(attribflags);
	gl.drawArrays(GL_POINTS, 0, (GLsizei) numpoints);
}

void Graphics::polyline(const float *coords, size_t count)
{
	if (!currentPass.active)
		throw RenderOutsidePassException();

	const DisplayState &state = states.back();
	float pixelsize = 1.0f / std::max((float) pixelScaleStack.back(), 0.000001f);

	if (state.lineJoin == LINE_JOIN_NONE)
	{
		NoneJoinPolyline line;
		line.render(coords, count, state.lineWidth * .5f, pixelsize, state.lineStyle == LINE_SMOOTH);
		line.draw();
	}
	else if (state.lineJoin == LINE_JOIN_BEVEL)
	{
		BevelJoinPolyline line;
		line.render(coords, count, state.lineWidth * .5f, pixelsize, state.lineStyle == LINE_SMOOTH);
		line.draw();
	}
	else // LINE_JOIN_MITER
	{
		MiterJoinPolyline line;
		line.render(coords, count, state.lineWidth * .5f, pixelsize, state.lineStyle == LINE_SMOOTH);
		line.draw();
	}
}

void Graphics::rectangle(DrawMode mode, float x, float y, float w, float h)
{
	float coords[] = {x,y, x,y+h, x+w,y+h, x+w,y, x,y};
	polygon(mode, coords, 5 * 2);
}

void Graphics::rectangle(DrawMode mode, float x, float y, float w, float h, float rx, float ry, int points)
{
	if (!currentPass.active)
		throw RenderOutsidePassException();

	if (rx == 0 || ry == 0)
	{
		rectangle(mode, x, y, w, h);
		return;
	}

	// Radius values that are more than half the rectangle's size aren't handled
	// correctly (for now)...
	if (w >= 0.02f)
		rx = std::min(rx, w / 2.0f - 0.01f);
	if (h >= 0.02f)
		ry = std::min(ry, h / 2.0f - 0.01f);

	points = std::max(points / 4, 1);

	const float half_pi = static_cast<float>(LOVE_M_PI / 2);
	float angle_shift = half_pi / ((float) points + 1.0f);

	int num_coords = (points + 2) * 8;
	float *coords = new float[num_coords + 2];
	float phi = .0f;

	for (int i = 0; i <= points + 2; ++i, phi += angle_shift)
	{
		coords[2 * i + 0] = x + rx * (1 - cosf(phi));
		coords[2 * i + 1] = y + ry * (1 - sinf(phi));
	}

	phi = half_pi;

	for (int i = points + 2; i <= 2 * (points + 2); ++i, phi += angle_shift)
	{
		coords[2 * i + 0] = x + w - rx * (1 + cosf(phi));
		coords[2 * i + 1] = y + ry * (1 - sinf(phi));
	}

	phi = 2 * half_pi;

	for (int i = 2 * (points + 2); i <= 3 * (points + 2); ++i, phi += angle_shift)
	{
		coords[2 * i + 0] = x + w - rx * (1 + cosf(phi));
		coords[2 * i + 1] = y + h - ry * (1 + sinf(phi));
	}

	phi =  3 * half_pi;

	for (int i = 3 * (points + 2); i <= 4 * (points + 2); ++i, phi += angle_shift)
	{
		coords[2 * i + 0] = x + rx * (1 - cosf(phi));
		coords[2 * i + 1] = y + h - ry * (1 + sinf(phi));
	}

	coords[num_coords + 0] = coords[0];
	coords[num_coords + 1] = coords[1];

	polygon(mode, coords, num_coords + 2);

	delete[] coords;
}

int Graphics::calculateEllipsePoints(float rx, float ry) const
{
	int points = (int) sqrtf(((rx + ry) / 2.0f) * 20.0f * (float) pixelScaleStack.back());
	return std::max(points, 8);
}

void Graphics::rectangle(DrawMode mode, float x, float y, float w, float h, float rx, float ry)
{
	rectangle(mode, x, y, w, h, rx, ry, calculateEllipsePoints(rx, ry));
}

void Graphics::circle(DrawMode mode, float x, float y, float radius, int points)
{
	ellipse(mode, x, y, radius, radius, points);
}

void Graphics::circle(DrawMode mode, float x, float y, float radius)
{
	ellipse(mode, x, y, radius, radius);
}

void Graphics::ellipse(DrawMode mode, float x, float y, float a, float b, int points)
{
	if (!currentPass.active)
		throw RenderOutsidePassException();

	float two_pi = static_cast<float>(LOVE_M_PI * 2);
	if (points <= 0) points = 1;
	float angle_shift = (two_pi / points);
	float phi = .0f;

	float *coords = new float[2 * (points + 1)];
	for (int i = 0; i < points; ++i, phi += angle_shift)
	{
		coords[2*i+0] = x + a * cosf(phi);
		coords[2*i+1] = y + b * sinf(phi);
	}

	coords[2*points+0] = coords[0];
	coords[2*points+1] = coords[1];

	polygon(mode, coords, (points + 1) * 2);

	delete[] coords;
}

void Graphics::ellipse(DrawMode mode, float x, float y, float a, float b)
{
	ellipse(mode, x, y, a, b, calculateEllipsePoints(a, b));
}

void Graphics::arc(DrawMode drawmode, ArcMode arcmode, float x, float y, float radius, float angle1, float angle2, int points)
{
	if (!currentPass.active)
		throw RenderOutsidePassException();

	// Nothing to display with no points or equal angles. (Or is there with line mode?)
	if (points <= 0 || angle1 == angle2)
		return;

	// Oh, you want to draw a circle?
	if (fabs(angle1 - angle2) >= 2.0f * (float) LOVE_M_PI)
	{
		circle(drawmode, x, y, radius, points);
		return;
	}

	float angle_shift = (angle2 - angle1) / points;
	// Bail on precision issues.
	if (angle_shift == 0.0)
		return;

	// Prevent the connecting line from being drawn if a closed line arc has a
	// small angle. Avoids some visual issues when connected lines are at sharp
	// angles, due to the miter line join drawing code.
	if (drawmode == DRAW_LINE && arcmode == ARC_CLOSED && fabsf(angle1 - angle2) < LOVE_TORAD(4))
		arcmode = ARC_OPEN;

	// Quick fix for the last part of a filled open arc not being drawn (because
	// polygon(DRAW_FILL, ...) doesn't work without a closed loop of vertices.)
	if (drawmode == DRAW_FILL && arcmode == ARC_OPEN)
		arcmode = ARC_CLOSED;

	float phi = angle1;

	float *coords = nullptr;
	int num_coords = 0;

	const auto createPoints = [&](float *coordinates)
	{
		for (int i = 0; i <= points; ++i, phi += angle_shift)
		{
			coordinates[2 * i + 0] = x + radius * cosf(phi);
			coordinates[2 * i + 1] = y + radius * sinf(phi);
		}
	};

	if (arcmode == ARC_PIE)
	{
		num_coords = (points + 3) * 2;
		coords = new float[num_coords];

		coords[0] = coords[num_coords - 2] = x;
		coords[1] = coords[num_coords - 1] = y;

		createPoints(coords + 2);
	}
	else if (arcmode == ARC_OPEN)
	{
		num_coords = (points + 1) * 2;
		coords = new float[num_coords];

		createPoints(coords);
	}
	else // ARC_CLOSED
	{
		num_coords = (points + 2) * 2;
		coords = new float[num_coords];

		createPoints(coords);

		// Connect the ends of the arc.
		coords[num_coords - 2] = coords[0];
		coords[num_coords - 1] = coords[1];
	}

	// NOTE: We rely on polygon() using GL_TRIANGLE_FAN, when fill mode is used.
	polygon(drawmode, coords, num_coords);

	delete[] coords;
}

void Graphics::arc(DrawMode drawmode, ArcMode arcmode, float x, float y, float radius, float angle1, float angle2)
{
	float points = (float) calculateEllipsePoints(radius, radius);

	// The amount of points is based on the fraction of the circle created by the arc.
	float angle = fabsf(angle1 - angle2);
	if (angle < 2.0f * (float) LOVE_M_PI)
		points *= angle / (2.0f * (float) LOVE_M_PI);

	arc(drawmode, arcmode, x, y, radius, angle1, angle2, (int) (points + 0.5f));
}

/// @param mode    the draw mode
/// @param coords  the coordinate array
/// @param count   the number of coordinates/size of the array
void Graphics::polygon(DrawMode mode, const float *coords, size_t count)
{
	if (!currentPass.active)
		throw RenderOutsidePassException();

	// coords is an array of a closed loop of vertices, i.e.
	// coords[count-2] = coords[0], coords[count-1] = coords[1]
	if (mode == DRAW_LINE)
	{
		polyline(coords, count);
	}
	else
	{
		OpenGL::TempDebugGroup debuggroup("Filled polygon draw");

		gl.prepareDraw();
		gl.bindTextureToUnit(gl.getDefaultTexture(), 0, false);
		gl.bindBuffer(BUFFER_VERTEX, 0);
		gl.useVertexAttribArrays(ATTRIBFLAG_POS);
		glVertexAttribPointer(ATTRIB_POS, 2, GL_FLOAT, GL_FALSE, 0, coords);
		gl.drawArrays(GL_TRIANGLE_FAN, 0, (int)count/2-1); // opengl will close the polygon for us
	}
}

Graphics::RendererInfo Graphics::getRendererInfo() const
{
	RendererInfo info;

	if (GLAD_ES_VERSION_2_0)
		info.name = "OpenGL ES";
	else
		info.name = "OpenGL";

	const char *str = (const char *) glGetString(GL_VERSION);
	if (str)
		info.version = str;
	else
		throw love::Exception("Cannot retrieve renderer version information.");

	str = (const char *) glGetString(GL_VENDOR);
	if (str)
		info.vendor = str;
	else
		throw love::Exception("Cannot retrieve renderer vendor information.");

	str = (const char *) glGetString(GL_RENDERER);
	if (str)
		info.device = str;
	else
		throw love::Exception("Cannot retrieve renderer device information.");

	return info;
}

Graphics::Stats Graphics::getStats() const
{
	Stats stats;

	stats.drawCalls = gl.stats.drawCalls;
	stats.renderPasses = renderPassCount;
	stats.shaderSwitches = gl.stats.shaderSwitches;
	stats.canvases = Canvas::canvasCount;
	stats.images = Image::imageCount;
	stats.fonts = Font::fontCount;
	stats.textureMemory = gl.stats.textureMemory;

	return stats;
}

double Graphics::getSystemLimit(SystemLimit limittype) const
{
	switch (limittype)
	{
	case Graphics::LIMIT_POINT_SIZE:
		return (double) gl.getMaxPointSize();
	case Graphics::LIMIT_TEXTURE_SIZE:
		return (double) gl.getMaxTextureSize();
	case Graphics::LIMIT_MULTI_CANVAS:
		return (double) gl.getMaxRenderTargets();
	case Graphics::LIMIT_CANVAS_MSAA:
		return (double) gl.getMaxRenderbufferSamples();
	case Graphics::LIMIT_ANISOTROPY:
		return (double) gl.getMaxAnisotropy();
	default:
		return 0.0;
	}
}

bool Graphics::isSupported(Feature feature) const
{
	switch (feature)
	{
	case FEATURE_MULTI_CANVAS_FORMATS:
		return Canvas::isMultiFormatMultiCanvasSupported();
	case FEATURE_CLAMP_ZERO:
		return gl.isClampZeroTextureWrapSupported();
	case FEATURE_LIGHTEN:
		return GLAD_VERSION_1_4 || GLAD_ES_VERSION_3_0 || GLAD_EXT_blend_minmax;
	case FEATURE_FULL_NPOT:
		return GLAD_VERSION_2_0 || GLAD_ES_VERSION_3_0 || GLAD_OES_texture_npot;
	case FEATURE_PIXEL_SHADER_HIGHP:
		return gl.isPixelShaderHighpSupported();
	default:
		return false;
	}
}

void Graphics::push(StackType type)
{
	if (stackTypes.size() == MAX_USER_STACK_DEPTH)
		throw Exception("Maximum stack depth reached (more pushes than pops?)");

	gl.pushTransform();

	pixelScaleStack.push_back(pixelScaleStack.back());

	if (type == STACK_ALL)
		states.push_back(states.back());

	stackTypes.push_back(type);
}

void Graphics::pop()
{
	if (stackTypes.size() < 1)
		throw Exception("Minimum stack depth reached (more pops than pushes?)");

	gl.popTransform();
	pixelScaleStack.pop_back();

	if (stackTypes.back() == STACK_ALL)
	{
		DisplayState &newstate = states[states.size() - 2];

		restoreStateChecked(newstate);

		// The last two states in the stack should be equal now.
		states.pop_back();
	}

	stackTypes.pop_back();
}

void Graphics::rotate(float r)
{
	gl.getTransform().rotate(r);
}

void Graphics::scale(float x, float y)
{
	gl.getTransform().scale(x, y);
	pixelScaleStack.back() *= (fabs(x) + fabs(y)) / 2.0;
}

void Graphics::translate(float x, float y)
{
	gl.getTransform().translate(x, y);
}

void Graphics::shear(float kx, float ky)
{
	gl.getTransform().shear(kx, ky);
}

void Graphics::origin()
{
	gl.getTransform().setIdentity();
	pixelScaleStack.back() = 1;
}

void Graphics::applyTransform(love::math::Transform *transform)
{
	Matrix4 &m = gl.getTransform();
	m *= transform->getMatrix();

	float sx, sy;
	m.getApproximateScale(sx, sy);
	pixelScaleStack.back() = (sx + sy) / 2.0;
}

void Graphics::replaceTransform(love::math::Transform *transform)
{
	const Matrix4 &m = transform->getMatrix();
	gl.getTransform() = m;

	float sx, sy;
	m.getApproximateScale(sx, sy);
	pixelScaleStack.back() = (sx + sy) / 2.0;
}

Vector Graphics::transformPoint(Vector point)
{
	Vector p;
	gl.getTransform().transform(&p, &point, 1);
	return p;
}

Vector Graphics::inverseTransformPoint(Vector point)
{
	Vector p;
	// TODO: We should probably cache the inverse transform so we don't have to
	// re-calculate it every time this is called.
	gl.getTransform().inverse().transform(&p, &point, 1);
	return p;
}

} // opengl
} // graphics
} // love
