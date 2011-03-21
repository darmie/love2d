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

#ifndef LOVE_AUDIO_SDL_SOURCE_H
#define LOVE_AUDIO_SDL_SOURCE_H

// LOVE
#include <common/Object.h>
#include <audio/Source.h>
#include <sound/Decoder.h>
#include <sound/SoundData.h>

// SDL
#include <SDL_mixer.h>

namespace love
{
namespace audio
{
namespace sdl
{
	class Source : public love::audio::Source
	{
	private:

		float pitch;
		float volume;
		bool looping;

		static void music_callback(void *udata, Uint8 * stream, int len);

		Mix_Chunk *snd;
		int sndchannel;

		love::sound::Decoder * decoder;
		love::sound::SoundData * sounddata;

		int bufferpos;

	public:
		Source(love::sound::SoundData * sounddata);
		Source(love::sound::Decoder * decoder);
		virtual ~Source();

		virtual love::audio::Source * copy();
		virtual void play();
		virtual void stop();
		virtual void pause();
		virtual void resume();
		virtual void rewind();
		virtual bool isStopped() const;
		virtual bool isFinished() const;
		virtual bool isPaused() const;
		virtual void update();
		virtual void setPitch(float pitch);
		virtual float getPitch() const;
		virtual void setVolume(float volume);
		virtual float getVolume() const;
		virtual void setPosition(float * v);
		virtual void getPosition(float * v) const;
		virtual void setVelocity(float * v);
		virtual void getVelocity(float * v) const;
		virtual void setDirection(float * v);
		virtual void getDirection(float * v) const;
		void setLooping(bool looping);
		bool isLooping() const;
		bool isStatic() const;
		virtual void seek(float offset, Unit unit);
		virtual float tell(Unit unit) const;

	}; // Source

} // sdl
} // audio
} // love

#endif // LOVE_AUDIO_SDL_SOURCE_H
