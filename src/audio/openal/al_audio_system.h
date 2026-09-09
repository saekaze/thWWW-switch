#ifndef _BS_AL_AUDIO_SYSTEM_H_
#define _BS_AL_AUDIO_SYSTEM_H_

#include "common.h"
#include "audio_system.h"
#ifdef __APPLE__
#include <OpenAL/al.h>
#include <OpenAL/alc.h>
#else
#include <AL/al.h>
#include <AL/alc.h>
#endif

#define MAX_SOUND_INSTANCES 128
#define SOUND_INSTANCE_ID_BASE 100000
#define MAX_AUDIO_STREAMS 32
#define MAX_AUDIO_BUFFER_SOUNDS 16
#define AUDIO_BUFFER_SOUND_INDEX_BASE 200000
// This is the index space that the native runner uses
#define AUDIO_STREAM_INDEX_BASE 300000

#define AL_STREAM_BUFFER_COUNT 4
#define AL_STREAM_BUFFER_SAMPLES 4096

struct stb_vorbis;

typedef struct {
    bool active;
    int32_t soundIndex; // SOND resource that spawned this
    int32_t instanceId; // unique ID returned to GML
    ALuint alSource; // OpenAL source object
    ALuint alBuffer; // OpenAL buffer object (only valid when streaming == false)
    float targetGain;
    float currentGain;
    float fadeTimeRemaining;
    float fadeTotalTime;
    float startGain;
    int32_t priority;

    // Streaming state (only valid when streaming == true)
    bool streaming;
    bool streamIsPcm;
    bool loop;
    bool streamEnded; // decoder produced no more samples; waiting for queue to drain
    struct stb_vorbis* vorbis;
    ALuint streamBuffers[AL_STREAM_BUFFER_COUNT];
    int16_t* decodeScratch; // sized for AL_STREAM_BUFFER_SAMPLES * streamChannels shorts
    // Runtime buffer sounds (thWWW BGM) are queued as small PCM chunks instead
    // of one very large OpenAL buffer. The owning AudioBufferSoundEntry remains
    // alive until GML stops the instance and calls audio_free_buffer_sound.
    const uint8_t* streamPcmData;
    int32_t streamPcmSize;
    int32_t streamPcmCursor;
    int streamBitsPerSample;
    int streamChannels;
    int streamSampleRate;
    ALenum streamFormat;
    float streamLengthSeconds;
    uint64_t playedSamples; // cumulative per-channel samples that have left the queue
} SoundInstance;

typedef struct {
    bool active;
    char* filePath; // resolved file path (owned, freed on destroy)
    float initialGain;
    float initialPitch;
} AudioStreamEntry;

typedef struct {
    bool active;
    uint8_t* data;
    int32_t size;
    int32_t bitsPerSample;
    int32_t sampleRate;
    int32_t channels;
} AudioBufferSoundEntry;

typedef struct {
    AudioSystem base;
    ALCdevice* alDevice;
    ALCcontext* alContext;
    SoundInstance instances[MAX_SOUND_INSTANCES];
    int32_t nextInstanceCounter;
    FileSystem* fileSystem;
    AudioStreamEntry streams[MAX_AUDIO_STREAMS];
    AudioBufferSoundEntry bufferSounds[MAX_AUDIO_BUFFER_SOUNDS];
} AlAudioSystem;

AlAudioSystem* AlAudioSystem_create(void);

#endif /* _BS_AL_AUDIO_SYSTEM_H_ */
