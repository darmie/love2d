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

#pragma once

// LOVE
#include "common/Object.h"
#include "common/Data.h"
#include "CompressedSlice.h"

namespace love
{
namespace image
{

/**
 * Base class for all CompressedImageData parser library interfaces.
 * We inherit from love::Object to take advantage of reference counting...
 **/
class CompressedFormatHandler : public love::Object
{
public:

	CompressedFormatHandler() {}
	virtual ~CompressedFormatHandler() {}

	/**
	 * Determines whether a particular FileData can be parsed as
	 * CompressedImageData by this handler.
	 * @param data The data to parse.
	 **/
	virtual bool canParse(const Data *data) = 0;

	/**
	 * Parses compressed image filedata into a list of sub-images and returns
	 * a single block of memory containing all the images.
	 *
	 * @param[in] filedata The data to parse.
	 * @param[out] images The list of sub-images generated. Byte data is a
	 *             pointer to the returned data.
	 * @param[out] format The format of the Compressed Data.
	 * @param[out] sRGB Whether the texture is sRGB-encoded.
	 *
	 * @return The single block of memory containing the parsed images.
	 **/
	virtual StrongRef<CompressedMemory> parse(Data *filedata,
	               std::vector<StrongRef<CompressedSlice>> &images,
	               PixelFormat &format, bool &sRGB) = 0;

}; // CompressedFormatHandler

} // image
} // love
