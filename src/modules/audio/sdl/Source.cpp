/**
* Copyright (c) 2006-2010 LOVE Development Team
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

#include "Source.h"

#include <iostream>

namespace love
{
namespace audio
{
namespace sdl
{
	void Source::music_callback(void *udata, Uint8 * stream, int len)
	{
		Source *thisS = (Source*) udata;
		for (int i = 0; i < len;)
		{
			int numbytes = thisS->decoder->getSize()-thisS->bufferpos;
			numbytes = len > numbytes ? numbytes : len;
			memcpy(stream+i, ((char*) thisS->decoder->getBuffer())+thisS->bufferpos, numbytes);
			thisS->bufferpos += numbytes;
			i += numbytes;
			if (thisS->bufferpos == thisS->decoder->getSize())
			{
				thisS->decoder->decode();
				thisS->bufferpos = 0;
				if (thisS->isFinished())
				{
					if (thisS->isLooping())
						Mix_HookMusic(music_callback, udata);
					else
						Mix_HookMusic(NULL, NULL);
				}
			}
		}
	}

	Source::Source(love::sound::Decoder * decoder)
		: love::audio::Source(Source::TYPE_STREAM), looping(false), decoder(decoder), bufferpos(0)
	{
		decoder->retain();
	}

	Source::Source(love::sound::SoundData * sounddata)
	: love::audio::Source(Source::TYPE_STATIC), looping(false), sounddata(sounddata), bufferpos(0)
	{
		sounddata->retain();
		snd = Mix_QuickLoad_RAW((Uint8*) sounddata->getData(), (Uint32) sounddata->getSize());
	}

	Source::~Source()
	{
		if (isStatic())
			sounddata->release();
		else
			decoder->release();
	}

	love::audio::Source * Source::copy()
	{
		this->retain();
		return this;
	}

	void Source::play()
	{
		if (isStatic())
			sndchannel = Mix_PlayChannel(-1, snd, isLooping() ? -1 : 0);
		else
			Mix_HookMusic(music_callback, this);
	}

	void Source::stop()
	{
		if (isStatic())
			Mix_HaltChannel(sndchannel);
		else
			Mix_HookMusic(NULL, NULL);
	}

	void Source::pause()
	{
		if (isStatic())
			Mix_Pause(sndchannel);
		else
			Mix_PauseMusic();
	}

	void Source::resume()
	{
		if (isStatic())
			Mix_Resume(sndchannel);
		else
			Mix_ResumeMusic();
	}

	void Source::rewind()
	{
		if (!isStatic())
			Mix_RewindMusic();
	}

	bool Source::isStopped() const
	{
		if (isStatic())
			return !Mix_Playing(sndchannel);
		else
			return decoder->isFinished();
	}

	bool Source::isFinished() const
	{
		if (isStatic())
			return isStopped() && !Mix_Paused(sndchannel);
		else
			return decoder->isFinished();
		return true;
	}
	
	bool Source::isPaused() const
	{
		if (isStatic())
			return Mix_Paused(sndchannel);
		else
			return Mix_PausedMusic();
		return true;
	}

	void Source::update()
	{
	}

	void Source::setPitch(float pitch)
	{
		this->pitch = pitch;
	}

	float Source::getPitch() const
	{
		return pitch;
	}

	void Source::setVolume(float volume)
	{
		this->volume = volume;
	}

	float Source::getVolume() const
	{
		return volume;
	}

	void Source::setPosition(float *)
	{
	}

	void Source::getPosition(float *) const
	{
	}

	void Source::setVelocity(float *)
	{
	}

	void Source::getVelocity(float *) const
	{
	}

	void Source::setDirection(float *)
	{
	}

	void Source::getDirection(float *) const
	{
	}

	void Source::setLooping(bool looping)
	{
		this->looping = looping;
	}

	bool Source::isLooping() const
	{
		return looping;
	}

	bool Source::isStatic() const
	{
		return (type == TYPE_STATIC);
	}
	
	void Source::seek(float offset, Source::Unit unit)
	{
		switch (unit) {
			case Source::UNIT_SAMPLES:
				/* stub */
				break;
			case Source::UNIT_SECONDS:	
			default:
				Mix_RewindMusic();
				Mix_SetMusicPosition(offset);
				break;
			}
	}
	
	float Source::tell(Source::Unit unit) const
	{
		float offset = 0;
		switch (unit) {
			case Source::UNIT_SAMPLES:
				/* stub */
				break;
			case Source::UNIT_SECONDS:
			default:
				/* stub */
				break;
		}
		return offset;
	}

} // sdl
} // audio
} // love
