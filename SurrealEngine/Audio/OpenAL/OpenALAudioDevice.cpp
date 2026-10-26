
#include "Precomp.h"
#include "OpenALAudioDevice.h"
#include "Packages/Engine/Actors/UActor.h"
#include "Packages/Engine/Resources/USound.h"
#include "Utils/Exception.h"
#include "Engine.h"
#include <cmath>
#include <chrono>
#include <cstring>

#define UU_PER_METER 43

OpenALAudioDevice::OpenALAudioDevice(int inFrequency, int numVoices, int inMusicBufferCount, int inMusicBufferSize)
{
	frequency = inFrequency;
	musicBufferCount = inMusicBufferCount;
	musicBufferSize = inMusicBufferSize;

	musicBuffer.resize(musicBufferSize * musicBufferCount);
	musicQueue.Resize(musicBufferCount);

	for (int i = 0; i < musicBufferCount; i++)
	{
		musicQueue.Push(&musicBuffer[musicBufferSize * i]);
		musicQueue.Pop();
	}

	// Init OpenAL
	// TODO: Add device enumeration
	alDevice = alcOpenDevice(nullptr);
	if (alDevice == nullptr)
		Exception::Throw("Failed to initialize OpenAL device");

	const ALCint ctxAttribs[] =
	{
		ALC_FREQUENCY, inFrequency,
		ALC_REFRESH, 60,
		ALC_SYNC, ALC_FALSE
	};

	alContext = alcCreateContext(alDevice, nullptr);
	if (alContext == nullptr)
		Exception::Throw("Failed to initialize OpenAL context");

	if (alcMakeContextCurrent(alContext) == ALC_FALSE)
		Exception::Throw("Failed to make OpenAL context current");

	// init listener state
	ALfloat listenerOri[] = { 0.0f, -1.0f, 0.0f, 0.0f, 0.0f, -1.0f };
	alListener3f(AL_POSITION, 0, 0, 0.0f);
	alListener3f(AL_VELOCITY, 0, 0, 0);
	alListenerfv(AL_ORIENTATION, listenerOri);
#ifndef __EMSCRIPTEN__
	// Emscripten OpenAL does not implement this extension and records AL_INVALID_ENUM.
	// That sticky error makes the first alGenSources look like it failed.
	alListenerf(AL_METERS_PER_UNIT, 1.f / UU_PER_METER);
#endif

	alDistanceModel(AL_LINEAR_DISTANCE_CLAMPED);
	alSpeedOfSound(343.3f / (1.0f / UU_PER_METER));

	// Init sound sources
	alcGetIntegerv(alDevice, ALC_MONO_SOURCES, 1, &monoSources);
	alcGetIntegerv(alDevice, ALC_STEREO_SOURCES, 1, &stereoSources);

	// Emscripten OpenAL returns 0x7FFFFFFF for ALC_MONO_SOURCES ("unlimited").
	// Allocating that many ALSoundSource objects throws std::bad_alloc.
	sources.resize(std::min(numVoices, monoSources));

	// init music source/buffer
	alGenSources(1, &alMusicSource);
	alSourcei(alMusicSource, AL_SOURCE_SPATIALIZE_SOFT, AL_FALSE);

	alMusicBuffers.resize(musicBufferCount);
	alGenBuffers(musicBufferCount, &alMusicBuffers[0]);

#ifndef __EMSCRIPTEN__
	// The web build is compiled with USE_PTHREADS=0. std::thread throws there,
	// which aborts device startup. Music is pumped from Update() instead.
	musicThreadData.thread = std::thread([this]() { MusicThreadMain(); });
#endif
}

OpenALAudioDevice::~OpenALAudioDevice()
{
	std::unique_lock lock(musicThreadData.mutex);
	musicThreadData.exitFlag = true;
	lock.unlock();
#ifndef __EMSCRIPTEN__
	musicThreadData.thread.join();
#endif

	alSourceStop(alMusicSource);
	alDeleteSources(1, &alMusicSource);

	alDeleteBuffers((ALsizei)alMusicBuffers.size(), &alMusicBuffers[0]);

	sources.clear();

	for (USound* sound : sounds)
	{
		alDeleteBuffers(1, (ALuint*)&sound->handle);
	}

	alcDestroyContext(alContext);
	alcCloseDevice(alDevice);
}

int OpenALAudioDevice::GetTotalChannels()
{
	return (int)sources.size();
}

void OpenALAudioDevice::AddSound(USound* sound)
{
	sounds.push_back(sound);

	ALenum format = AL_FORMAT_MONO_FLOAT32;
	if (sound->channels == 2)
		format = AL_FORMAT_STEREO_FLOAT32;

	ALuint id;
	alGenBuffers(1, &id);
	alBufferData(id, format, sound->samples.data(), (ALsizei)(sound->samples.size() * sizeof(sound->samples[0])), sound->frequency);
	alError = alGetError();
	if (alError != AL_NO_ERROR)
		Exception::Throw("Failed to buffer sound data for " + sound->Name.ToString());

	sound->handle = (void*)(ptrdiff_t)id;
}

void OpenALAudioDevice::RemoveSound(USound* sound)
{
	auto it = sounds.begin();
	while (it != sounds.end())
	{
		if (*it == sound)
		{
			// TODO: find sources playing this sound and stop them?
			sounds.erase(it);
			alDeleteBuffers(1, reinterpret_cast<const ALuint*>(&sound->handle));
			return;
		}
	}
}

bool OpenALAudioDevice::IsPlaying(int channel)
{
	ALSoundSource& source = sources[channel];
	return source.IsPlaying();
}

void OpenALAudioDevice::PlayMusic(std::unique_ptr<AudioSource> source)
{
	std::unique_lock lock(musicThreadData.mutex);
	musicThreadData.music = std::move(source);
	musicThreadData.musicUpdate = true;
}

void OpenALAudioDevice::PlaySound(int channel, USound* sound, vec3& location, float volume, float radius, float pitch)
{
	if (!std::isfinite(volume) || volume < 0.0f || !std::isfinite(pitch) || channel >= sources.size())
		Exception::Throw("Invalid PlaySound arguments");

	ALSoundSource& source = sources[channel];
	if (source.IsPlaying())
	{
		LogMessage("Attempted to play sound on active channel " + std::to_string(channel));
		return;
	}

	source.SetSound(sound);
	source.SetPosition(location);
	source.SetVolume(volume);
	source.SetRadius(radius);
	source.SetPitch(pitch);
	source.SetSpatial(true);
	source.Play();
}

void OpenALAudioDevice::UpdateSound(int channel, vec3& location, float volume, float radius, float pitch)
{
	if (!std::isfinite(volume) || volume < 0.0f || !std::isfinite(pitch) || channel >= sources.size())
		Exception::Throw("Invalid PlaySound arguments");

	ALSoundSource& source = sources[channel];

	source.SetPosition(location);
	source.SetVolume(volume);
	source.SetRadius(radius);
	source.SetPitch(pitch);

	source.DoLoop();
}

void OpenALAudioDevice::StopSound(int channel)
{
	if (channel >= sources.size())
		Exception::Throw("Invalid StopSound arguments");

	sources[channel].Stop();
}

std::string OpenALAudioDevice::getALErrorString()
{
	switch (alGetError())
	{
	case AL_NO_ERROR:
		return "AL_NO_ERROR";
	case AL_INVALID_NAME:
		return "AL_INVALID_NAME";
	case AL_INVALID_ENUM:
		return "AL_INVALID_ENUM";
	case AL_INVALID_VALUE:
		return "AL_INVALID_VALUE";
	case AL_INVALID_OPERATION:
		return "AL_INVALID_OPERATION";
	case AL_OUT_OF_MEMORY:
		return "AL_OUT_OF_MEMORY";
	default:
		return "AL_UNKNOWN_ERROR";
	}
}

void OpenALAudioDevice::PlayMusicBuffer(AudioSource* music)
{
	int format = (music->GetChannels() == 1) ? AL_FORMAT_MONO_FLOAT32 : AL_FORMAT_STEREO_FLOAT32;
	int freq = music->GetFrequency();

	ALenum error;
	for (int i = 0; i < musicBufferCount; i++)
	{
		alBufferData(alMusicBuffers[i], format, musicQueue.Pop(), musicBufferSize * 4, freq);
		if ((error = alGetError()) != AL_NO_ERROR)
			Exception::Throw("alBufferData failed in PlayMusicBuffer: " + getALErrorString());

		alSourceQueueBuffers(alMusicSource, 1, &alMusicBuffers[i]);
		if (alGetError() != AL_NO_ERROR)
			Exception::Throw("alSourceQueueBuffers failed in PlayMusicBuffer: " + getALErrorString());
	}

	alSourcePlay(alMusicSource);
	if (alGetError() != AL_NO_ERROR)
		Exception::Throw("alSourcePlay failed in PlayMusicBuffer: " + getALErrorString());
}

void OpenALAudioDevice::UpdateMusicBuffer(AudioSource* music)
{
	ALint status;
	alGetSourcei(alMusicSource, AL_BUFFERS_PROCESSED, &status);

	while (status)
	{
		ALuint buffer;
		alSourceUnqueueBuffers(alMusicSource, 1, &buffer);

		int format = (music->GetChannels() == 1) ? AL_FORMAT_MONO_FLOAT32 : AL_FORMAT_STEREO_FLOAT32;
		int freq = music->GetFrequency();

		alBufferData(buffer, format, musicQueue.Pop(), musicBufferSize * 4, freq);
		if (alGetError() != AL_NO_ERROR)
			Exception::Throw("alBufferData failed in UpdateMusicBuffer: " + getALErrorString());

		alSourceQueueBuffers(alMusicSource, 1, &buffer);
		if (alGetError() != AL_NO_ERROR)
			Exception::Throw("alSourceQueueBuffers failed in UpdateMusicBuffer: " + getALErrorString());

		status--;
	}

	alGetSourcei(alMusicSource, AL_SOURCE_STATE, &status);
	if (status == AL_STOPPED)
	{
		alSourcePlay(alMusicSource);
		if (alGetError() != AL_NO_ERROR)
			Exception::Throw("alSourcePlay failed in PlayMusicBuffer: " + getALErrorString());
	}
}

void OpenALAudioDevice::SetMusicVolume(float volume)
{
	alSourcef(alMusicSource, AL_GAIN, volume);
}

void OpenALAudioDevice::Update()
{
	UActor* listener = engine->CameraActor;
	if (listener)
	{
#ifdef __EMSCRIPTEN__
		// Emscripten's OpenAL walks every source on each listener call, so unchanged values stay out of that path.
		vec3& location = listener->Location();
		vec3& velocity = listener->Velocity();

		vec3 at, left, up;
		Coords::Rotation(listener->Rotation()).GetAxes(at, left, up);

		float x = location.x;
		float y = location.y;
		float z = -location.z;
		if (!listenerSet || listenerPosition.x != x || listenerPosition.y != y || listenerPosition.z != z)
		{
			listenerPosition = vec3(x, y, z);
			alListener3f(AL_POSITION, x, y, z);
			listenerSet = true;
		}

		float vx = velocity.x;
		float vy = velocity.y;
		float vz = -velocity.z;
		if (listenerVelocity.x != vx || listenerVelocity.y != vy || listenerVelocity.z != vz)
		{
			listenerVelocity = vec3(vx, vy, vz);
			alListener3f(AL_VELOCITY, vx, vy, vz);
		}

		ALfloat listenerOri[6] = { up.x, up.y, -up.z, -at.x, -at.y, at.z };
		if (memcmp(listenerOrientation, listenerOri, sizeof(listenerOri)) != 0)
		{
			memcpy(listenerOrientation, listenerOri, sizeof(listenerOri));
			alListenerfv(AL_ORIENTATION, listenerOri);
		}
#else
		// Update listener properties
		vec3& location = listener->Location();
		vec3& velocity = listener->Velocity();

		vec3 at, left, up;
		Coords::Rotation(listener->Rotation()).GetAxes(at, left, up);

		ALfloat listenerOri[6] = { up.x, up.y, -up.z, -at.x, -at.y, at.z };
		alListener3f(AL_POSITION, location.x, location.y, -location.z);
		alListener3f(AL_VELOCITY, velocity.x, velocity.y, -velocity.z);
		alListenerfv(AL_ORIENTATION, listenerOri);
#endif
	}

#ifdef __EMSCRIPTEN__
	PumpMusic();
#endif
}

#ifdef __EMSCRIPTEN__
// Move decoded chunks onto the OpenAL stream. Recycled buffers keep playback from
// running dry; fresh buffers grow the queue until the whole pool is in use.
void OpenALAudioDevice::FeedMusicStream(AudioSource* music, int maxChunks)
{
	int format = (music->GetChannels() == 1) ? AL_FORMAT_MONO_FLOAT32 : AL_FORMAT_STEREO_FLOAT32;
	int freq = music->GetFrequency();
	int uploaded = 0;

	ALint processed = 0;
	alGetSourcei(alMusicSource, AL_BUFFERS_PROCESSED, &processed);
	while (processed > 0 && uploaded < maxChunks && musicQueue.Size() > 0)
	{
		ALuint buffer = 0;
		alSourceUnqueueBuffers(alMusicSource, 1, &buffer);
		if (alGetError() != AL_NO_ERROR)
			Exception::Throw("alSourceUnqueueBuffers failed in FeedMusicStream: " + getALErrorString());

		alBufferData(buffer, format, musicQueue.Pop(), musicBufferSize * 4, freq);
		if (alGetError() != AL_NO_ERROR)
			Exception::Throw("alBufferData failed in FeedMusicStream: " + getALErrorString());

		alSourceQueueBuffers(alMusicSource, 1, &buffer);
		if (alGetError() != AL_NO_ERROR)
			Exception::Throw("alSourceQueueBuffers failed in FeedMusicStream: " + getALErrorString());

		processed--;
		uploaded++;
	}

	while (uploaded < maxChunks && musicQueue.Size() > 0 && streamedMusicBuffers < musicBufferCount)
	{
		ALuint buffer = alMusicBuffers[streamedMusicBuffers];
		alBufferData(buffer, format, musicQueue.Pop(), musicBufferSize * 4, freq);
		if (alGetError() != AL_NO_ERROR)
			Exception::Throw("alBufferData failed in FeedMusicStream: " + getALErrorString());

		alSourceQueueBuffers(alMusicSource, 1, &buffer);
		if (alGetError() != AL_NO_ERROR)
			Exception::Throw("alSourceQueueBuffers failed in FeedMusicStream: " + getALErrorString());

		streamedMusicBuffers++;
		uploaded++;
	}

	if (musicPlaying)
	{
		ALint status = 0;
		alGetSourcei(alMusicSource, AL_SOURCE_STATE, &status);
		if (status == AL_STOPPED && streamedMusicBuffers > 0)
		{
			alSourcePlay(alMusicSource);
			if (alGetError() != AL_NO_ERROR)
				Exception::Throw("alSourcePlay failed in FeedMusicStream: " + getALErrorString());
		}
	}
}

// One music-mixing step. Returns false when the device is shutting down.
bool OpenALAudioDevice::PumpMusic()
{
	std::unique_lock lock(musicThreadData.mutex);
	if (musicThreadData.exitFlag)
		return false;
	if (musicThreadData.musicUpdate)
	{
		// Stop marks every queued buffer processed. Unqueue what is actually queued;
		// asking for the whole pool fails when playback had only filled part of it.
		alSourceStop(alMusicSource);
		ALint processed = 0;
		alGetSourcei(alMusicSource, AL_BUFFERS_PROCESSED, &processed);
		while (processed > 0)
		{
			ALuint buffer = 0;
			alSourceUnqueueBuffers(alMusicSource, 1, &buffer);
			processed--;
		}
		musicPlaying = false;
		currentMusic = std::move(musicThreadData.music);
		musicThreadData.musicUpdate = false;
		musicQueue.Clear();
		streamedMusicBuffers = 0;
	}
	lock.unlock();

	if (currentMusic)
	{
		// The mixer runs on the game thread here. Decode and upload one chunk per
		// frame so catching up after a hitch does not stall the frame that presents it.
		constexpr int kMaxChunksPerPump = 1;
		constexpr int kMinChunksToStart = 3;
		int produced = 0;
		while (produced < kMaxChunksPerPump && musicQueue.Size() < (size_t)musicBufferCount)
		{
			currentMusic->ReadSamples(musicQueue.GetNextFree(), musicBufferSize);
			musicQueue.Push(musicQueue.GetNextFree());
			produced++;
		}

		FeedMusicStream(currentMusic.get(), kMaxChunksPerPump);

		if (!musicPlaying && streamedMusicBuffers >= kMinChunksToStart)
		{
			alSourcePlay(alMusicSource);
			if (alGetError() != AL_NO_ERROR)
				Exception::Throw("alSourcePlay failed in PumpMusic: " + getALErrorString());
			musicPlaying = true;
		}
	}
	else
	{
		musicPlaying = false;
	}
	return true;
}
#else
void OpenALAudioDevice::MusicThreadMain()
{
	std::unique_ptr<AudioSource> currentMusic;
	bool musicPlaying = false;

	while (true)
	{
		// Lock the mutex. Grab the data we need from the main thread. Then unlock.
		std::unique_lock lock(musicThreadData.mutex);
		if (musicThreadData.exitFlag)
			break;
		if (musicThreadData.musicUpdate)
		{
			if (musicPlaying)
			{
				alSourceStop(alMusicSource);
				alSourceUnqueueBuffers(alMusicSource, (ALsizei)alMusicBuffers.size(), &alMusicBuffers[0]);
				musicPlaying = false;
			}
			currentMusic = std::move(musicThreadData.music);
			musicThreadData.musicUpdate = false;
		}
		lock.unlock();
		// Never touch anything from musicThreadData after this point.

		if (currentMusic)
		{
			while (musicQueue.Size() < musicBufferCount)
			{
				// render a chunk of music
				currentMusic->ReadSamples(musicQueue.GetNextFree(), musicBufferSize);
				musicQueue.Push(musicQueue.GetNextFree());
			}

			if (!musicPlaying)
			{
				PlayMusicBuffer(currentMusic.get());
				musicPlaying = true;
			}
			else if (musicQueue.Size() > 0)
			{
				UpdateMusicBuffer(currentMusic.get());
			}

			// TODO: music fade in/out
		}
		else
		{
			musicPlaying = false;
		}
		using namespace std::chrono_literals;
		std::this_thread::sleep_for(5ms);
	}
}
#endif
