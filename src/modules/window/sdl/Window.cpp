/**
 * Copyright (c) 2006-2015 LOVE Development Team
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
#include "graphics/Graphics.h"
#include "Window.h"

// C++
#include <iostream>
#include <vector>
#include <algorithm>

// C
#include <cstdio>

// SDL
#include <SDL_syswm.h>

#if defined(LOVE_WINDOWS)
#include <windows.h>
#elif defined(LOVE_MACOSX)
#include "common/OSX.h"
#endif

#ifndef APIENTRY
#define APIENTRY
#endif

namespace love
{
namespace window
{
namespace sdl
{

Window::Window()
	: open(false)
	, mouseGrabbed(false)
	, window(nullptr)
	, context(nullptr)
	, displayedWindowError(false)
	, displayedContextError(false)
{
	if (SDL_InitSubSystem(SDL_INIT_VIDEO) < 0)
		throw love::Exception("Could not initialize SDL video subsystem (%s)", SDL_GetError());
}

Window::~Window()
{
	close();

	SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

void Window::setGLFramebufferAttributes(int msaa, bool sRGB)
{
	// Set GL window / framebuffer attributes.
	SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 1);
	SDL_GL_SetAttribute(SDL_GL_RETAINED_BACKING, 0);

	SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, (msaa > 0) ? 1 : 0);
	SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, (msaa > 0) ? msaa : 0);

	// SDL or GLX may have bugs with this: https://bugzilla.libsdl.org/show_bug.cgi?id=2897
	// It's fine to leave the attribute disabled on desktops though, because in
	// practice the framebuffer will be sRGB-capable even if it's not requested.
#if !defined(LOVE_LINUX)
	SDL_GL_SetAttribute(SDL_GL_FRAMEBUFFER_SRGB_CAPABLE, sRGB ? 1 : 0);
#endif

#if defined(LOVE_WINDOWS)
	// Avoid the Microsoft OpenGL 1.1 software renderer on Windows. Apparently
	// older Intel drivers like to use it as a fallback when requesting some
	// unsupported framebuffer attribute values, rather than properly failing.
	SDL_GL_SetAttribute(SDL_GL_ACCELERATED_VISUAL, 1);
#endif
}

void Window::setGLContextAttributes(const ContextAttribs &attribs)
{
	int profilemask = 0;
	int contextflags = 0;

	if (attribs.gles)
		profilemask |= SDL_GL_CONTEXT_PROFILE_ES;
	else if (attribs.debug)
		profilemask |= SDL_GL_CONTEXT_PROFILE_COMPATIBILITY;

	if (attribs.debug)
		contextflags |= SDL_GL_CONTEXT_DEBUG_FLAG;

	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, attribs.versionMajor);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, attribs.versionMinor);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, profilemask);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, contextflags);
}

bool Window::checkGLVersion(const ContextAttribs &attribs)
{
	typedef unsigned char GLubyte;
	typedef unsigned int GLenum;
	typedef const GLubyte *(APIENTRY *glGetStringPtr)(GLenum name);
	const GLenum GL_VERSION_ENUM = 0x1F02;

	// We don't have OpenGL headers or an automatic OpenGL function loader in
	// this module, so we have to get the glGetString function pointer ourselves.
	glGetStringPtr glGetStringFunc = (glGetStringPtr) SDL_GL_GetProcAddress("glGetString");
	if (!glGetStringFunc)
		return false;

	const char *glversion = (const char *) glGetStringFunc(GL_VERSION_ENUM);
	if (!glversion)
		return false;

	int glmajor = 0;
	int glminor = 0;

	// glGetString(GL_VERSION) returns a string with the format "major.minor",
	// or "OpenGL ES major.minor" in GLES contexts.
	const char *format = "%d.%d";
	if (attribs.gles)
		format = "OpenGL ES %d.%d";

	if (sscanf(glversion, format, &glmajor, &glminor) != 2)
		return false;

	if (glmajor < attribs.versionMajor
		|| (glmajor == attribs.versionMajor && glminor < attribs.versionMinor))
		return false;

	return true;
}

bool Window::createWindowAndContext(int x, int y, int w, int h, Uint32 windowflags, int msaa, bool sRGB)
{
	bool preferGLES = false;

#ifdef LOVE_GRAPHICS_USE_OPENGLES
	preferGLES = true;
#endif

	const char *curdriver = SDL_GetCurrentVideoDriver();
	const char *glesdrivers[] = {"RPI", "Android", "uikit", "winrt", "emscripten"};

	// We always want to try OpenGL ES first on certain video backends.
	for (const char *glesdriver : glesdrivers)
	{
		if (curdriver && strstr(curdriver, glesdriver) == curdriver)
		{
			preferGLES = true;
			break;
		}
	}

	if (!preferGLES)
	{
		const char *gleshint = SDL_GetHint("LOVE_GRAPHICS_USE_OPENGLES");
		preferGLES = (gleshint != nullptr && gleshint[0] != '0');
	}

	// Do we want a debug context?
	const char *debughint = SDL_GetHint("LOVE_GRAPHICS_DEBUG");
	bool debug = (debughint != nullptr && debughint[0] != '0');

	// Different context attribute profiles to try.
	std::vector<ContextAttribs> attribslist = {
		{2, 1, false, debug}, // OpenGL 2.1.
		{3, 0, true,  debug}, // OpenGL ES 3.
		{2, 0, true,  debug}, // OpenGL ES 2.
	};

	SDL_version sdlversion = {};
	SDL_GetVersion(&sdlversion);

	// OpenGL ES 3+ contexts are only properly supported in SDL 2.0.4+.
	if (sdlversion.major == 2 && sdlversion.minor == 0 && sdlversion.patch <= 3)
		attribslist.erase(attribslist.begin() + 1);

	// Move OpenGL ES to the front of the list if we should prefer GLES.
	if (preferGLES)
		std::rotate(attribslist.begin(), attribslist.begin() + 1, attribslist.end());

	if (context)
	{
		SDL_GL_DeleteContext(context);
		context = nullptr;
	}

	std::string windowerror;

	// Try each context profile in order.
	for (ContextAttribs attribs : attribslist)
	{
		// Unfortunately some OpenGL context settings are part of the internal
		// window state in the Windows and Linux SDL backends, so we have to
		// recreate the window when we want to change those settings...
		if (window)
		{
			SDL_DestroyWindow(window);
			SDL_FlushEvent(SDL_WINDOWEVENT);
			window = nullptr;
		}

		int curMSAA  = msaa;
		bool curSRGB = sRGB;

		setGLFramebufferAttributes(curMSAA, curSRGB);
		setGLContextAttributes(attribs);

		window = SDL_CreateWindow(title.c_str(), x, y, w, h, windowflags);

		if (!window && curMSAA > 0)
		{
			// The MSAA setting could have caused the failure.
			setGLFramebufferAttributes(0, curSRGB);
			window = SDL_CreateWindow(title.c_str(), x, y, w, h, windowflags);
			if (window)
				curMSAA = 0;
		}

		if (!window && curSRGB)
		{
			// same with sRGB.
			setGLFramebufferAttributes(curMSAA, false);
			window = SDL_CreateWindow(title.c_str(), x, y, w, h, windowflags);
			if (window)
				curSRGB = false;
		}

		if (!window && curMSAA > 0 && curSRGB)
		{
			// Or both!
			setGLFramebufferAttributes(0, false);
			window = SDL_CreateWindow(title.c_str(), x, y, w, h, windowflags);
			if (window)
			{
				curMSAA = 0;
				curSRGB = false;
			}
		}

		// Immediately try the next context profile if window creation failed.
		if (!window)
		{
			windowerror = std::string(SDL_GetError());
			continue;
		}

		windowerror.clear();

		context = SDL_GL_CreateContext(window);

		if (!context && curMSAA > 0)
		{
			// MSAA and sRGB settings can also cause CreateContext to fail, on
			// certain SDL backends.
			setGLFramebufferAttributes(0, curSRGB);
			context = SDL_GL_CreateContext(window);
		}

		if (!context && curSRGB)
		{
			setGLFramebufferAttributes(curMSAA, false);
			context = SDL_GL_CreateContext(window);
		}

		if (!context && curMSAA > 0 && curSRGB)
		{
			setGLFramebufferAttributes(0, false);
			context = SDL_GL_CreateContext(window);
		}

		if (!context && attribs.debug)
		{
			attribs.debug = false;
			setGLContextAttributes(attribs);
			context = SDL_GL_CreateContext(window);
		}

		// Make sure the context's version is at least what we requested.
		if (context && !checkGLVersion(attribs))
		{
			SDL_GL_DeleteContext(context);
			context = nullptr;
		}

		if (context)
			break;
	}

	if (!context || !window)
	{
		if (!windowerror.empty())
		{
			std::string title = "Unable to create window";
			std::string message = "SDL error: " + windowerror;

			std::cerr << title << std::endl << message << std::endl;

			// Display a message box with the error, but only once.
			if (!displayedWindowError)
			{
				showMessageBox(title, message, MESSAGEBOX_ERROR, false);
				displayedWindowError = true;
			}
		}
		else if (!context)
		{
			std::string title = "Unable to initialize OpenGL";
			std::string message = "This program requires a graphics card and video drivers which support OpenGL 2.1 or OpenGL ES 2.";

			std::cerr << title << std::endl << message << std::endl;

			// Display a message box with the error, but only once.
			if (!displayedContextError)
			{
				showMessageBox(title, message, MESSAGEBOX_ERROR, true);
				displayedContextError = true;
			}
		}

		close();

		return false;
	}

	open = true;
	return true;
}

bool Window::setWindow(int width, int height, WindowSettings *settings)
{
	WindowSettings f;

	if (settings)
		f = *settings;

	f.minwidth = std::max(f.minwidth, 1);
	f.minheight = std::max(f.minheight, 1);

	f.display = std::min(std::max(f.display, 0), getDisplayCount() - 1);

	// Use the desktop resolution if a width or height of 0 is specified.
	if (width == 0 || height == 0)
	{
		SDL_DisplayMode mode = {};
		SDL_GetDesktopDisplayMode(f.display, &mode);
		width = mode.w;
		height = mode.h;
	}

	Uint32 sdlflags = SDL_WINDOW_OPENGL;

	if (f.fullscreen)
	{
		if (f.fstype == FULLSCREEN_DESKTOP)
			sdlflags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
		else
		{
			sdlflags |= SDL_WINDOW_FULLSCREEN;
			SDL_DisplayMode mode = {0, width, height, 0, 0};

			// Fullscreen window creation will bug out if no mode can be used.
			if (SDL_GetClosestDisplayMode(f.display, &mode, &mode) == nullptr)
			{
				// GetClosestDisplayMode will fail if we request a size larger
				// than the largest available display mode, so we'll try to use
				// the largest (first) mode in that case.
				if (SDL_GetDisplayMode(f.display, 0, &mode) < 0)
					return false;
			}

			width = mode.w;
			height = mode.h;
		}
	}

	if (f.resizable)
		sdlflags |= SDL_WINDOW_RESIZABLE;

	if (f.borderless)
		sdlflags |= SDL_WINDOW_BORDERLESS;

	if (f.highdpi)
		sdlflags |= SDL_WINDOW_ALLOW_HIGHDPI;

	int x = f.x;
	int y = f.y;

	if (f.useposition && !f.fullscreen)
	{
		// The position needs to be in the global coordinate space.
		SDL_Rect displaybounds = {};
		SDL_GetDisplayBounds(f.display, &displaybounds);
		x += displaybounds.x;
		y += displaybounds.y;
	}
	else
	{
		if (f.centered)
			x = y = SDL_WINDOWPOS_CENTERED_DISPLAY(f.display);
		else
			x = y = SDL_WINDOWPOS_UNDEFINED_DISPLAY(f.display);
	}

	close();

	if (!createWindowAndContext(x, y, width, height, sdlflags, f.msaa, f.sRGB))
		return false;

	// Make sure the window keeps any previously set icon.
	setIcon(curMode.icon.get());

	// Make sure the mouse keeps its previous grab setting.
	setMouseGrab(mouseGrabbed);

	// Enforce minimum window dimensions.
	SDL_SetWindowMinimumSize(window, f.minwidth, f.minheight);

	if ((f.useposition || f.centered) && !f.fullscreen)
		SDL_SetWindowPosition(window, x, y);

	SDL_RaiseWindow(window);

	SDL_GL_SetSwapInterval(f.vsync ? 1 : 0);

	updateSettings(f);

	auto gfx = Module::getInstance<graphics::Graphics>(Module::M_GRAPHICS);
	if (gfx != nullptr)
		gfx->setMode(curMode.pixelwidth, curMode.pixelheight, curMode.settings.sRGB);

	return true;
}

bool Window::onSizeChanged(int width, int height)
{
	if (!window)
		return false;

	curMode.width = width;
	curMode.height = height;

	SDL_GL_GetDrawableSize(window, &curMode.pixelwidth, &curMode.pixelheight);

	graphics::Graphics *gfx = Module::getInstance<graphics::Graphics>(Module::M_GRAPHICS);
	if (gfx != nullptr)
		gfx->setViewportSize(curMode.pixelwidth, curMode.pixelheight);

	return true;
}

void Window::updateSettings(const WindowSettings &newsettings)
{
	Uint32 wflags = SDL_GetWindowFlags(window);

	// Set the new display mode as the current display mode.
	SDL_GetWindowSize(window, &curMode.width, &curMode.height);
	SDL_GL_GetDrawableSize(window, &curMode.pixelwidth, &curMode.pixelheight);

	if ((wflags & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN_DESKTOP)
	{
		curMode.settings.fullscreen = true;
		curMode.settings.fstype = FULLSCREEN_DESKTOP;
	}
	else if ((wflags & SDL_WINDOW_FULLSCREEN) == SDL_WINDOW_FULLSCREEN)
	{
		curMode.settings.fullscreen = true;
		curMode.settings.fstype = FULLSCREEN_EXCLUSIVE;
	}
	else
	{
		curMode.settings.fullscreen = false;
		curMode.settings.fstype = newsettings.fstype;
	}

	// The min width/height is set to 0 internally in SDL when in fullscreen.
	if (curMode.settings.fullscreen)
	{
		curMode.settings.minwidth = newsettings.minwidth;
		curMode.settings.minheight = newsettings.minheight;
	}
	else
		SDL_GetWindowMinimumSize(window, &curMode.settings.minwidth, &curMode.settings.minheight);

	curMode.settings.resizable = (wflags & SDL_WINDOW_RESIZABLE) != 0;
	curMode.settings.borderless = (wflags & SDL_WINDOW_BORDERLESS) != 0;
	curMode.settings.centered = newsettings.centered;

	getPosition(curMode.settings.x, curMode.settings.y, curMode.settings.display);

	curMode.settings.highdpi = (wflags & SDL_WINDOW_ALLOW_HIGHDPI) != 0;

	// Only minimize on focus loss if the window is in exclusive-fullscreen
	// mode.
	if (curMode.settings.fullscreen && curMode.settings.fstype == FULLSCREEN_EXCLUSIVE)
		SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "1");
	else
		SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");

	curMode.settings.sRGB = newsettings.sRGB;

	// Verify MSAA setting.
	int buffers = 0;
	int samples = 0;
	SDL_GL_GetAttribute(SDL_GL_MULTISAMPLEBUFFERS, &buffers);
	SDL_GL_GetAttribute(SDL_GL_MULTISAMPLESAMPLES, &samples);

	curMode.settings.msaa = (buffers > 0 ? samples : 0);
	curMode.settings.vsync = SDL_GL_GetSwapInterval() != 0;

	SDL_DisplayMode dmode = {};
	SDL_GetCurrentDisplayMode(curMode.settings.display, &dmode);

	// May be 0 if the refresh rate can't be determined.
	curMode.settings.refreshrate = (double) dmode.refresh_rate;
}

void Window::getWindow(int &width, int &height, WindowSettings &settings)
{
	// The window might have been modified (moved, resized, etc.) by the user.
	if (window)
		updateSettings(curMode.settings);

	width = curMode.width;
	height = curMode.height;
	settings = curMode.settings;
}

void Window::close()
{
	auto gfx = Module::getInstance<graphics::Graphics>(Module::M_GRAPHICS);
	if (gfx != nullptr)
		gfx->unSetMode();

	if (context)
	{
		SDL_GL_DeleteContext(context);
		context = nullptr;
	}

	if (window)
	{
		SDL_DestroyWindow(window);
		window = nullptr;

		// The old window may have generated pending events which are no longer
		// relevant. Destroy them all!
		SDL_FlushEvent(SDL_WINDOWEVENT);
	}

	open = false;
}

bool Window::setFullscreen(bool fullscreen, Window::FullscreenType fstype)
{
	if (!window)
		return false;

	WindowSettings newsettings = curMode.settings;
	newsettings.fullscreen = fullscreen;
	newsettings.fstype = fstype;

	Uint32 sdlflags = 0;

	if (fullscreen)
	{
		if (fstype == FULLSCREEN_DESKTOP)
			sdlflags = SDL_WINDOW_FULLSCREEN_DESKTOP;
		else
		{
			sdlflags = SDL_WINDOW_FULLSCREEN;

			SDL_DisplayMode mode = {};
			mode.w = curMode.width;
			mode.h = curMode.height;

			SDL_GetClosestDisplayMode(SDL_GetWindowDisplayIndex(window), &mode, &mode);
			SDL_SetWindowDisplayMode(window, &mode);
		}
	}

	if (SDL_SetWindowFullscreen(window, sdlflags) == 0)
	{
		SDL_GL_MakeCurrent(window, context);
		updateSettings(newsettings);

		// Update the viewport size now instead of waiting for event polling.
		graphics::Graphics *gfx = Module::getInstance<graphics::Graphics>(Module::M_GRAPHICS);
		if (gfx != nullptr)
			gfx->setViewportSize(curMode.pixelwidth, curMode.pixelheight);

		return true;
	}

	return false;
}

bool Window::setFullscreen(bool fullscreen)
{
	return setFullscreen(fullscreen, curMode.settings.fstype);
}

int Window::getDisplayCount() const
{
	return SDL_GetNumVideoDisplays();
}

const char *Window::getDisplayName(int displayindex) const
{
	const char *name = SDL_GetDisplayName(displayindex);

	if (name == nullptr)
		throw love::Exception("Invalid display index: %d", displayindex + 1);

	return name;
}

typedef Window::WindowSize WindowSize;

std::vector<WindowSize> Window::getFullscreenSizes(int displayindex) const
{
	std::vector<WindowSize> sizes;

	for (int i = 0; i < SDL_GetNumDisplayModes(displayindex); i++)
	{
		SDL_DisplayMode mode = {};
		SDL_GetDisplayMode(displayindex, i, &mode);

		WindowSize w = {mode.w, mode.h};

		// SDL2's display mode list has multiple entries for modes of the same
		// size with different bits per pixel, so we need to filter those out.
		if (std::find(sizes.begin(), sizes.end(), w) == sizes.end())
			sizes.push_back(w);
	}

	return sizes;
}

void Window::getDesktopDimensions(int displayindex, int &width, int &height) const
{
	if (displayindex >= 0 && displayindex < getDisplayCount())
	{
		SDL_DisplayMode mode = {};
		SDL_GetDesktopDisplayMode(displayindex, &mode);
		width = mode.w;
		height = mode.h;
	}
	else
	{
		width = 0;
		height = 0;
	}
}

void Window::setPosition(int x, int y, int displayindex)
{
	if (!window)
		return;

	displayindex = std::min(std::max(displayindex, 0), getDisplayCount() - 1);

	SDL_Rect displaybounds = {};
	SDL_GetDisplayBounds(displayindex, &displaybounds);

	// The position needs to be in the global coordinate space.
	x += displaybounds.x;
	y += displaybounds.y;

	SDL_SetWindowPosition(window, x, y);

	curMode.settings.useposition = true;
}

void Window::getPosition(int &x, int &y, int &displayindex)
{
	if (!window)
	{
		x = y =  0;
		displayindex = 0;
		return;
	}

	displayindex = std::max(SDL_GetWindowDisplayIndex(window), 0);

	SDL_GetWindowPosition(window, &x, &y);

	// SDL always reports 0, 0 for fullscreen windows.
	if (!(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN))
	{
		SDL_Rect displaybounds = {};
		SDL_GetDisplayBounds(displayindex, &displaybounds);

		// The position needs to be in the monitor's coordinate space.
		x -= displaybounds.x;
		y -= displaybounds.y;
	}
}

bool Window::isOpen() const
{
	return open;
}

void Window::setWindowTitle(const std::string &title)
{
	this->title = title;

	if (window)
		SDL_SetWindowTitle(window, title.c_str());
}

const std::string &Window::getWindowTitle() const
{
	return title;
}

bool Window::setIcon(love::image::ImageData *imgd)
{
	if (!imgd)
		return false;

	curMode.icon.set(imgd);

	if (!window)
		return false;

	Uint32 rmask, gmask, bmask, amask;
#ifdef LOVE_BIG_ENDIAN
	rmask = 0xFF000000;
	gmask = 0x00FF0000;
	bmask = 0x0000FF00;
	amask = 0x000000FF;
#else
	rmask = 0x000000FF;
	gmask = 0x0000FF00;
	bmask = 0x00FF0000;
	amask = 0xFF000000;
#endif

	int w = imgd->getWidth();
	int h = imgd->getHeight();
	int pitch = imgd->getWidth() * 4;

	SDL_Surface *sdlicon = 0;

	{
		// We don't want another thread modifying the ImageData mid-copy.
		love::thread::Lock lock(imgd->getMutex());
		sdlicon = SDL_CreateRGBSurfaceFrom(imgd->getData(), w, h, 32, pitch, rmask, gmask, bmask, amask);
	}

	if (!sdlicon)
		return false;

	SDL_SetWindowIcon(window, sdlicon);
	SDL_FreeSurface(sdlicon);

	return true;
}

love::image::ImageData *Window::getIcon()
{
	return curMode.icon.get();
}

void Window::minimize()
{
	if (window != nullptr)
		SDL_MinimizeWindow(window);
}

void Window::maximize()
{
	if (window != nullptr)
		SDL_MaximizeWindow(window);
}

void Window::swapBuffers()
{
	SDL_GL_SwapWindow(window);
}

bool Window::hasFocus() const
{
	return (window && SDL_GetKeyboardFocus() == window);
}

bool Window::hasMouseFocus() const
{
	return (window && SDL_GetMouseFocus() == window);
}

bool Window::isVisible() const
{
	return window && (SDL_GetWindowFlags(window) & SDL_WINDOW_SHOWN) != 0;
}

void Window::setMouseVisible(bool visible)
{
	SDL_ShowCursor(visible ? SDL_ENABLE : SDL_DISABLE);
}

bool Window::getMouseVisible() const
{
	return (SDL_ShowCursor(SDL_QUERY) == SDL_ENABLE);
}

void Window::setMouseGrab(bool grab)
{
	mouseGrabbed = grab;
	if (window)
		SDL_SetWindowGrab(window, (SDL_bool) grab);
}

bool Window::isMouseGrabbed() const
{
	if (window)
		return SDL_GetWindowGrab(window) != SDL_FALSE;
	else
		return mouseGrabbed;
}

void Window::getPixelDimensions(int &w, int &h) const
{
	w = curMode.pixelwidth;
	h = curMode.pixelheight;
}

void Window::windowToPixelCoords(double *x, double *y) const
{
	if (x != nullptr)
		*x = (*x) * ((double) curMode.pixelwidth / (double) curMode.width);
	if (y != nullptr)
		*y = (*y) * ((double) curMode.pixelheight / (double) curMode.height);
}

void Window::pixelToWindowCoords(double *x, double *y) const
{
	if (x != nullptr)
		*x = (*x) * ((double) curMode.width / (double) curMode.pixelwidth);
	if (y != nullptr)
		*y = (*y) * ((double) curMode.height / (double) curMode.pixelheight);
}

double Window::getPixelScale() const
{
	// TODO: Return the density display metric on Android.
	return (double) curMode.pixelheight / (double) curMode.height;
}

double Window::toPixels(double x) const
{
	return x * getPixelScale();
}

void Window::toPixels(double wx, double wy, double &px, double &py) const
{
	double scale = getPixelScale();
	px = wx * scale;
	py = wy * scale;
}

double Window::fromPixels(double x) const
{
	return x / getPixelScale();
}

void Window::fromPixels(double px, double py, double &wx, double &wy) const
{
	double scale = getPixelScale();
	wx = px / scale;
	wy = py / scale;
}

const void *Window::getHandle() const
{
	return window;
}

SDL_MessageBoxFlags Window::convertMessageBoxType(MessageBoxType type) const
{
	switch (type)
	{
	case MESSAGEBOX_ERROR:
		return SDL_MESSAGEBOX_ERROR;
	case MESSAGEBOX_WARNING:
		return SDL_MESSAGEBOX_WARNING;
	case MESSAGEBOX_INFO:
	default:
		return SDL_MESSAGEBOX_INFORMATION;
	}
}

bool Window::showMessageBox(const std::string &title, const std::string &message, MessageBoxType type, bool attachtowindow)
{
	SDL_MessageBoxFlags flags = convertMessageBoxType(type);
	SDL_Window *sdlwindow = attachtowindow ? window : nullptr;

	return SDL_ShowSimpleMessageBox(flags, title.c_str(), message.c_str(), sdlwindow) >= 0;
}

int Window::showMessageBox(const MessageBoxData &data)
{
	SDL_MessageBoxData sdldata = {};

	sdldata.flags = convertMessageBoxType(data.type);
	sdldata.title = data.title.c_str();
	sdldata.message = data.message.c_str();
	sdldata.window = data.attachToWindow ? window : nullptr;

	sdldata.numbuttons = (int) data.buttons.size();

	std::vector<SDL_MessageBoxButtonData> sdlbuttons;

	for (int i = 0; i < (int) data.buttons.size(); i++)
	{
		SDL_MessageBoxButtonData sdlbutton = {};

		sdlbutton.buttonid = i;
		sdlbutton.text = data.buttons[i].c_str();

		if (i == data.enterButtonIndex)
			sdlbutton.flags |= SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT;

		if (i == data.escapeButtonIndex)
			sdlbutton.flags |= SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT;

		sdlbuttons.push_back(sdlbutton);
	}

	sdldata.buttons = &sdlbuttons[0];

	int pressedbutton = -2;
	SDL_ShowMessageBox(&sdldata, &pressedbutton);

	return pressedbutton;
}

void Window::requestAttention(bool continuous)
{
#if defined(LOVE_WINDOWS)

	if (hasFocus())
		return;

	SDL_SysWMinfo wminfo = {};
	SDL_VERSION(&wminfo.version);

	if (SDL_GetWindowWMInfo(window, &wminfo))
	{
		FLASHWINFO flashinfo = {};
		flashinfo.cbSize = sizeof(FLASHWINFO);
		flashinfo.hwnd = wminfo.info.win.window;
		flashinfo.uCount = 1;
		flashinfo.dwFlags = FLASHW_ALL;

		if (continuous)
		{
			flashinfo.uCount = 0;
			flashinfo.dwFlags |= FLASHW_TIMERNOFG;
		}

		FlashWindowEx(&flashinfo);
	}

#elif defined(LOVE_MACOSX)

	love::osx::requestAttention(continuous);

#else

	LOVE_UNUSED(continuous);
	
#endif
	
	// TODO: Linux?
}

const char *Window::getName() const
{
	return "love.window.sdl";
}

} // sdl
} // window
} // love
