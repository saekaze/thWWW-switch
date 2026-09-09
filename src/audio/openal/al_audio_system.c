// On Windows, include windows.h first so its headers are processed before stb_vorbis
// defines single-letter macros (L, C, R) that conflict with winnt.h struct field names.
#ifdef _WIN32
#include <windows.h>
#endif

#include "stb_vorbis.c"
#include "al_audio_system.h"
#include "binary_utils.h"
#include "data_win.h"
#include "utils.h"

#include "stdio_compat.h"
#include <stdlib.h>
#include "string_compat.h"
#include "stb_ds.h"

// ===[ Helpers ]===

static bool isValidSoundInstanceId(int32_t instanceId) {
    return AUDIO_STREAM_INDEX_BASE > instanceId && instanceId >= SOUND_INSTANCE_ID_BASE;
}

static bool alSourceIsPlaying(ALuint source) {
    ALint state;
    alGetSourcei(source, AL_SOURCE_STATE, &state);
    return state == AL_PLAYING;
}

static bool alSourceHasStopped(ALuint source) {
    ALint state;
    alGetSourcei(source, AL_SOURCE_STATE, &state);
    return state == AL_STOPPED;
}

static bool alSourceIsLooping(ALuint source) {
    ALint state;
    alGetSourcei(source, AL_LOOPING, &state);
    return state != AL_FALSE;
}

// Source - https://stackoverflow.com/a/7995655
// Posted by Karl
// Retrieved 2026-05-05, License - CC BY-SA 3.0
static void alGetSourceLengthSec(ALuint buffer, float* out) {
    ALint sizeInBytes;
    ALint channels;
    ALint bits;

    alGetBufferi(buffer, AL_SIZE, &sizeInBytes);
    alGetBufferi(buffer, AL_CHANNELS, &channels);
    alGetBufferi(buffer, AL_BITS, &bits);

    int lengthInSamples = sizeInBytes * 8 / (channels * bits);
    ALint frequency;

    alGetBufferi(buffer, AL_FREQUENCY, &frequency);

    *out = (float)lengthInSamples / (float)frequency;
}

// Tears down whatever AL state is attached to a slot and marks it inactive.
static void releaseInstance(SoundInstance* inst) {
    if (!inst->active)
        return;

    alSourceStop(inst->alSource);

    if (inst->streaming) {
        // Drain anything still queued so the buffer names are detachable.
        ALint queued = 0;
        alGetSourcei(inst->alSource, AL_BUFFERS_QUEUED, &queued);
        repeat(queued, i) {
            ALuint b;
            alSourceUnqueueBuffers(inst->alSource, 1, &b);
        }
        alDeleteSources(1, &inst->alSource);
        alDeleteBuffers(AL_STREAM_BUFFER_COUNT, inst->streamBuffers);
        if (inst->vorbis != nullptr) {
            stb_vorbis_close((stb_vorbis*) inst->vorbis);
            inst->vorbis = nullptr;
        }
        free(inst->decodeScratch);
        inst->decodeScratch = nullptr;
        inst->streamPcmData = nullptr;
        inst->streamPcmSize = 0;
        inst->streamPcmCursor = 0;
        inst->streamIsPcm = false;
        inst->streaming = false;
    } else {
        alDeleteSources(1, &inst->alSource);
        alDeleteBuffers(1, &inst->alBuffer);
    }

    inst->active = false;
}

// Upload the next queued chunk for either a decoded Vorbis stream or a
// memory-backed runtime PCM sound. Wraps at EOF when looping is enabled.
static bool streamFillBuffer(SoundInstance* inst, ALuint buf) {
    if (inst->streamIsPcm) {
        int32_t bytesPerFrame = inst->streamChannels * (inst->streamBitsPerSample / 8);
        if (bytesPerFrame <= 0 || inst->streamPcmData == nullptr || inst->streamPcmSize <= 0)
            return false;
        if (inst->streamPcmCursor >= inst->streamPcmSize) {
            if (!inst->loop) return false;
            inst->streamPcmCursor = 0;
        }
        int32_t bytes = inst->streamPcmSize - inst->streamPcmCursor;
        int32_t maxBytes = AL_STREAM_BUFFER_SAMPLES * bytesPerFrame;
        if (bytes > maxBytes) bytes = maxBytes;
        bytes -= bytes % bytesPerFrame;
        if (bytes <= 0) return false;
        alBufferData(buf, inst->streamFormat,
                     inst->streamPcmData + inst->streamPcmCursor,
                     bytes, inst->streamSampleRate);
        if (alGetError() != AL_NO_ERROR) return false;
        inst->streamPcmCursor += bytes;
        return true;
    }

    stb_vorbis* v = (stb_vorbis*) inst->vorbis;
    int samples = stb_vorbis_get_samples_short_interleaved(v, inst->streamChannels, inst->decodeScratch, AL_STREAM_BUFFER_SAMPLES * inst->streamChannels);
    if (0 >= samples) {
        if (!inst->loop) return false;
        stb_vorbis_seek_start(v);
        samples = stb_vorbis_get_samples_short_interleaved(v, inst->streamChannels, inst->decodeScratch, AL_STREAM_BUFFER_SAMPLES * inst->streamChannels);
        if (0 >= samples) return false;
    }
    alBufferData(buf, inst->streamFormat, inst->decodeScratch, samples * inst->streamChannels * (ALsizei) sizeof(int16_t), inst->streamSampleRate);
    return alGetError() == AL_NO_ERROR;
}

static SoundInstance* findFreeSlot(AlAudioSystem* ma) {
    // First pass: find an inactive slot
    repeat(MAX_SOUND_INSTANCES, i) {
        if (!ma->instances[i].active) {
            return &ma->instances[i];
        }
    }

    // Second pass: evict the lowest-priority ended sound.
    // Streaming instances can briefly report AL_STOPPED during an underrun, so exclude them from eviction to keep music alive across SFX bursts.
    SoundInstance* best = nullptr;
    repeat(MAX_SOUND_INSTANCES, i) {
        SoundInstance* inst = &ma->instances[i];
        if (inst->streaming)
            continue;

        if (!alSourceIsPlaying(inst->alSource)) {
            if (best == nullptr || best->priority > inst->priority) {
                best = inst;
            }
        }
    }

    if (best != nullptr) {
        releaseInstance(best);
    }

    return best;
}

static SoundInstance* findInstanceById(AlAudioSystem* ma, int32_t instanceId) {
    int32_t slotIndex = instanceId - SOUND_INSTANCE_ID_BASE;
    if (0 > slotIndex || slotIndex >= MAX_SOUND_INSTANCES)
        return nullptr;

    SoundInstance* inst = &ma->instances[slotIndex];
    if (!inst->active || inst->instanceId != instanceId)
        return nullptr;

    return inst;
}

// Helper: resolve external audio file path from Sound entry
static char* resolveExternalPath(AlAudioSystem* ma, Sound* sound) {
    const char* file = sound->file;
    if (file == nullptr || file[0] == '\0') return nullptr;

    // If the filename has no extension, append ".ogg"
    bool hasExtension = (strchr(file, '.') != nullptr);

    char filename[512];
    if (hasExtension) {
        snprintf(filename, sizeof(filename), "%s", file);
    } else {
        snprintf(filename, sizeof(filename), "%s.ogg", file);
    }

    return ma->fileSystem->vtable->resolvePath(ma->fileSystem, filename);
}

// ===[ Vtable Implementations ]===

static void maInit(AudioSystem* audio, DataWin* dataWin, FileSystem* fileSystem) {
    AlAudioSystem* ma = (AlAudioSystem*) audio;
    ma->base.dw = dataWin;
    arrput(ma->base.audioGroups, dataWin);
    ma->fileSystem = fileSystem;

    ma->alDevice = alcOpenDevice(nullptr);
    if (ma->alDevice == nullptr) {
        logWarn("Audio: Failed to open OpenAL device (error %d)\n", alGetError());
        return;
    }

    ma->alContext = alcCreateContext(ma->alDevice, nullptr);
    if (ma->alContext == nullptr) {
        logWarn("Audio: Failed to create OpenAL context (error %d)\n", alGetError());
        alcCloseDevice(ma->alDevice);
        ma->alDevice = nullptr;
        return;
    }

    if (!alcMakeContextCurrent(ma->alContext)) {
        logWarn("Audio: Failed to make OpenAL context current (error %d)\n", alGetError());
        alcDestroyContext(ma->alContext);
        alcCloseDevice(ma->alDevice);
        ma->alContext = nullptr;
        ma->alDevice = nullptr;
        return;
    }

    memset(ma->instances, 0, sizeof(ma->instances));
    ma->nextInstanceCounter = 0;

    logInfo("Audio: OpenAL engine initialized\n");
}

static void maDestroy(AudioSystem* audio) {
    AlAudioSystem* ma = (AlAudioSystem*) audio;

    // Uninit all active sound instances
    repeat(MAX_SOUND_INSTANCES, i) {
        releaseInstance(&ma->instances[i]);
    }

    // Free stream entries
    repeat(MAX_AUDIO_STREAMS, i) {
        if (ma->streams[i].active) {
            free(ma->streams[i].filePath);
        }
    }

    // Free runtime PCM entries.
    repeat(MAX_AUDIO_BUFFER_SOUNDS, i) {
        free(ma->bufferSounds[i].data);
        ma->bufferSounds[i].data = nullptr;
        ma->bufferSounds[i].active = false;
    }

    // Free loaded audio groups. The main data.win is owned by the caller, so skip index 0.
    if (arrlen(ma->base.audioGroups) > 1) {
        for (int32_t i = 1; i < (int32_t) arrlen(ma->base.audioGroups); i++) {
            DataWin_free(ma->base.audioGroups[i]);
        }
    }
    arrfree(ma->base.audioGroups);

    alcMakeContextCurrent(nullptr);
    alcDestroyContext(ma->alContext);
    alcCloseDevice(ma->alDevice);
    free(ma);
}

static void maUpdate(AudioSystem* audio, float deltaTime) {
    AlAudioSystem* ma = (AlAudioSystem*) audio;

    repeat(MAX_SOUND_INSTANCES, i) {
        SoundInstance* inst = &ma->instances[i];
        if (!inst->active) continue;

        // Handle gain fading (for cases where we do manual fading)
        if (inst->fadeTimeRemaining > 0.0f) {
            inst->fadeTimeRemaining -= deltaTime;
            if (0.0f >= inst->fadeTimeRemaining) {
                inst->fadeTimeRemaining = 0.0f;
                inst->currentGain = inst->targetGain;
            } else {
                float t = 1.0f - (inst->fadeTimeRemaining / inst->fadeTotalTime);
                inst->currentGain = inst->startGain + (inst->targetGain - inst->startGain) * t;
            }
            alSourcef(inst->alSource, AL_GAIN, inst->currentGain);
        }

        if (inst->streaming) {
            // Recycle any buffers AL has finished with: count their samples toward the play position, then refill from the decoder and re-queue at the tail.
            ALint processed = 0;
            alGetSourcei(inst->alSource, AL_BUFFERS_PROCESSED, &processed);
            while (processed > 0) {
                ALuint buf;
                alSourceUnqueueBuffers(inst->alSource, 1, &buf);
                processed--;

                ALint sizeBytes = 0, bits = 0, channels = 0;
                alGetBufferi(buf, AL_SIZE, &sizeBytes);
                alGetBufferi(buf, AL_BITS, &bits);
                alGetBufferi(buf, AL_CHANNELS, &channels);
                if (bits > 0 && channels > 0) {
                    inst->playedSamples += (uint64_t) (sizeBytes * 8 / (bits * channels));
                }

                if (!inst->streamEnded) {
                    if (streamFillBuffer(inst, buf)) {
                        alSourceQueueBuffers(inst->alSource, 1, &buf);
                    } else {
                        inst->streamEnded = true;
                    }
                }
            }

            // Reap once the queue has fully drained on a non-looping track.
            ALint queued = 0;
            alGetSourcei(inst->alSource, AL_BUFFERS_QUEUED, &queued);
            if (inst->streamEnded && queued == 0) {
                releaseInstance(inst);
                continue;
            }

            // Underrun recovery: AL goes to AL_STOPPED if the queue runs dry.
            // Kick it back on as soon as we have buffers queued again.
            if (alSourceHasStopped(inst->alSource) && queued > 0) {
                alSourcePlay(inst->alSource);
            }
            continue;
        }

        // Clean up ended non-looping sounds (ma_sound_at_end avoids reaping still-loading async sounds)
        if (alSourceHasStopped(inst->alSource) && !alSourceIsLooping(inst->alSource)) {
            releaseInstance(inst);
        }
    }
}

// Walk RIFF chunks in a WAV container to find the 'data' chunk payload offset and length.
// Returns pointer to the first byte of audio data, or nullptr on failure.
// audioDataLenOut receives the chunk's data length.
static const uint8_t* findWavDataChunk(const uint8_t* data, uint32_t dataSize, uint32_t* audioDataLenOut) {
    if (data == nullptr || dataSize < 12) return nullptr;
    if (data[0] != 'R' || data[1] != 'I' || data[2] != 'F' || data[3] != 'F') return nullptr;
    uint32_t offset = 12;
    while (offset + 8 <= dataSize) {
        uint32_t chunkLen = data[offset+4] | (data[offset+5] << 8) | (data[offset+6] << 16) | (data[offset+7] << 24);
        uint32_t chunkDataOffset = offset + 8;
        uint32_t available = dataSize - chunkDataOffset;
        if (chunkLen > available) chunkLen = available;
        if (data[offset] == 'd' && data[offset+1] == 'a' && data[offset+2] == 't' && data[offset+3] == 'a') {
            *audioDataLenOut = chunkLen;
            return data + chunkDataOffset;
        }
        offset = chunkDataOffset + chunkLen;
        if (offset & 1) offset++;
    }
    return nullptr;
}

// Parse WAV fmt chunk fields from a RIFF/WAV header.
// Returns true if the data starts with "RIFF" and the fmt fields decode to non-zero values.
// outAudioData may be nullptr if the caller only needs the data length.
static bool parseWavHeader(const uint8_t* data, uint32_t dataSize,
                           uint32_t* outChannels, uint32_t* outSampleRate,
                           uint32_t* outBitsPerSample, const uint8_t** outAudioData,
                           uint32_t* outAudioDataLen) {
    if (data == nullptr || dataSize < 36) return false;
    if (data[0] != 'R' || data[1] != 'I' || data[2] != 'F' || data[3] != 'F') return false;
    *outChannels = data[22] | (data[23] << 8);
    *outSampleRate = data[24] | (data[25] << 8) | (data[26] << 16) | (data[27] << 24);
    *outBitsPerSample = data[34] | (data[35] << 8);
    uint32_t audioDataLen = 0;
    const uint8_t* found = findWavDataChunk(data, dataSize, &audioDataLen);
    if (outAudioData != nullptr) *outAudioData = found;
    *outAudioDataLen = audioDataLen;
    return found != nullptr && audioDataLen > 0
        && *outChannels > 0 && *outSampleRate > 0 && *outBitsPerSample > 0;
}

static int32_t maPlaySound(AudioSystem* audio, int32_t soundIndex, int32_t priority, bool loop) {
    AlAudioSystem* ma = (AlAudioSystem*) audio;

    // Check dynamic stream and runtime PCM index spaces before regular SOND assets.
    bool isStream = (soundIndex >= AUDIO_STREAM_INDEX_BASE);
    bool isBufferSound = (soundIndex >= AUDIO_BUFFER_SOUND_INDEX_BASE && soundIndex < AUDIO_STREAM_INDEX_BASE);
    Sound* sound = nullptr;
    AudioBufferSoundEntry* bufferSound = nullptr;
    char* streamPath = nullptr;
    float streamPitch = 1.0f;
    float streamGain = 1.0f;

    if (isStream) {
        int32_t streamSlot = soundIndex - AUDIO_STREAM_INDEX_BASE;
        if (0 > streamSlot || streamSlot >= MAX_AUDIO_STREAMS || !ma->streams[streamSlot].active) {
            logWarn("Audio: Invalid stream index %d\n", soundIndex);
            return -1;
        }
        AudioStreamEntry* stream = &ma->streams[streamSlot];
        streamPath = stream->filePath;
        streamPitch = stream->initialPitch;
        streamGain = stream->initialGain;
    } else if (isBufferSound) {
        int32_t bufferSlot = soundIndex - AUDIO_BUFFER_SOUND_INDEX_BASE;
        if (0 > bufferSlot || bufferSlot >= MAX_AUDIO_BUFFER_SOUNDS || !ma->bufferSounds[bufferSlot].active) {
            logWarn("Audio: Invalid buffer sound index %d\n", soundIndex);
            return -1;
        }
        bufferSound = &ma->bufferSounds[bufferSlot];
    } else {
        DataWin* dw = ma->base.audioGroups[0]; // Audio Group 0 should always be data.win
        if (0 > soundIndex || (uint32_t) soundIndex >= dw->sond.count) {
            logWarn("Audio: Invalid sound index %d\n", soundIndex);
            return -1;
        }
        sound = &dw->sond.sounds[soundIndex];
    }

    SoundInstance* slot = findFreeSlot(ma);
    if (slot == nullptr) {
        logWarn("Audio: No free sound slots for sound %d\n", soundIndex);
        return -1;
    }

    int32_t slotIndex = (int32_t) (slot - ma->instances);

    slot->streaming = false;
    slot->streamIsPcm = false;
    slot->vorbis = nullptr;
    slot->decodeScratch = nullptr;
    slot->streamPcmData = nullptr;
    slot->streamPcmSize = 0;
    slot->streamPcmCursor = 0;
    slot->streamBitsPerSample = 0;
    slot->streamEnded = false;
    slot->playedSamples = 0;

    if (isBufferSound) {
        // Runtime PCM (thWWW's WAV BGM): queue small buffers instead of asking
        // OpenAL to allocate one 16-42 MiB static buffer. This is both less
        // memory hungry and reliable on the Switch OpenAL backend.
        slot->streaming = true;
        slot->streamIsPcm = true;
        slot->loop = loop;
        slot->streamPcmData = bufferSound->data;
        slot->streamPcmSize = bufferSound->size;
        slot->streamPcmCursor = 0;
        slot->streamBitsPerSample = bufferSound->bitsPerSample;
        slot->streamChannels = bufferSound->channels;
        slot->streamSampleRate = bufferSound->sampleRate;
        if (bufferSound->channels == 1)
            slot->streamFormat = bufferSound->bitsPerSample == 8 ? AL_FORMAT_MONO8 : AL_FORMAT_MONO16;
        else
            slot->streamFormat = bufferSound->bitsPerSample == 8 ? AL_FORMAT_STEREO8 : AL_FORMAT_STEREO16;
        int32_t bytesPerFrame = bufferSound->channels * (bufferSound->bitsPerSample / 8);
        slot->streamLengthSeconds = bytesPerFrame > 0
            ? (float)bufferSound->size / (float)(bytesPerFrame * bufferSound->sampleRate)
            : 0.0f;

        alGenSources(1, &slot->alSource);
        alGenBuffers(AL_STREAM_BUFFER_COUNT, slot->streamBuffers);
        if (alGetError() != AL_NO_ERROR) {
            logWarn("Audio: alGenSources/alGenBuffers failed for runtime PCM sound %d\n", soundIndex);
            slot->streaming = false;
            slot->streamIsPcm = false;
            return -1;
        }

        int primed = 0;
        for (int i = 0; AL_STREAM_BUFFER_COUNT > i; i++) {
            if (!streamFillBuffer(slot, slot->streamBuffers[i])) break;
            alSourceQueueBuffers(slot->alSource, 1, &slot->streamBuffers[i]);
            if (alGetError() != AL_NO_ERROR) break;
            primed++;
        }
        if (primed == 0) {
            alDeleteSources(1, &slot->alSource);
            alDeleteBuffers(AL_STREAM_BUFFER_COUNT, slot->streamBuffers);
            slot->streaming = false;
            slot->streamIsPcm = false;
            logWarn("Audio: Could not prime runtime PCM stream %d\n", soundIndex);
            return -1;
        }
    } else if (isStream) {
        // Streaming path: open the decoder, queue a few small buffers, and let maUpdate() top them up.
        // This avoids the multi-hundred-millisecond hang of decoding a whole song into PCM on the main thread.
        int err = 0;
        stb_vorbis* v = stb_vorbis_open_filename(streamPath, &err, nullptr);
        if (v == nullptr) {
            logWarn("Audio: Failed to open stream '%s' (stb_vorbis err %d)\n", streamPath, err);
            return -1;
        }
        stb_vorbis_info info = stb_vorbis_get_info(v);

        slot->streaming = true;
        slot->loop = loop;
        slot->vorbis = v;
        slot->streamChannels = info.channels;
        slot->streamSampleRate = (int) info.sample_rate;
        slot->streamFormat = (info.channels == 2) ? AL_FORMAT_STEREO16 : AL_FORMAT_MONO16;
        slot->streamLengthSeconds = stb_vorbis_stream_length_in_seconds(v);
        slot->decodeScratch = (int16_t *)safeMalloc(AL_STREAM_BUFFER_SAMPLES * info.channels * sizeof(int16_t));

        alGenSources(1, &slot->alSource);
        alGenBuffers(AL_STREAM_BUFFER_COUNT, slot->streamBuffers);
        if (alGetError() != AL_NO_ERROR) {
            logWarn("Audio: alGenSources/alGenBuffers failed for stream\n");
            stb_vorbis_close(v);
            free(slot->decodeScratch);
            slot->streaming = false;
            slot->vorbis = nullptr;
            slot->decodeScratch = nullptr;
            return -1;
        }

        int primed = 0;
        for (int i = 0; AL_STREAM_BUFFER_COUNT > i; i++) {
            if (!streamFillBuffer(slot, slot->streamBuffers[i])) break;
            alSourceQueueBuffers(slot->alSource, 1, &slot->streamBuffers[i]);
            primed++;
        }

        if (primed == 0) {
            // Empty file or decode failure: tear everything down cleanly.
            alDeleteSources(1, &slot->alSource);
            alDeleteBuffers(AL_STREAM_BUFFER_COUNT, slot->streamBuffers);
            stb_vorbis_close(v);
            free(slot->decodeScratch);
            slot->streaming = false;
            slot->vorbis = nullptr;
            slot->decodeScratch = nullptr;
            return -1;
        }
    } else {
        alGenSources(1, &slot->alSource);
        alGenBuffers(1, &slot->alBuffer);
        if (alGetError() != AL_NO_ERROR) {
            logWarn("Audio: alGenSources/alGenBuffers failed for sound %d\n", soundIndex);
            return -1;
        }
        if (isBufferSound) {
            ALenum format = AL_FORMAT_STEREO16;
            if (bufferSound->channels == 1)
                format = bufferSound->bitsPerSample == 8 ? AL_FORMAT_MONO8 : AL_FORMAT_MONO16;
            else
                format = bufferSound->bitsPerSample == 8 ? AL_FORMAT_STEREO8 : AL_FORMAT_STEREO16;
            alBufferData(slot->alBuffer, format, bufferSound->data, bufferSound->size, bufferSound->sampleRate);
            if (alGetError() != AL_NO_ERROR) {
                logWarn("Audio: alBufferData failed for runtime sound %d\n", soundIndex);
                return -1;
            }
            alSourcei(slot->alSource, AL_BUFFER, slot->alBuffer);
        } else {
        bool isRegular = (sound->flags & AUDIO_ENTRY_FLAG_REGULAR) == AUDIO_ENTRY_FLAG_REGULAR;
        bool isEmbedded = (sound->flags & AUDIO_ENTRY_FLAG_IS_EMBEDDED) != 0;
        bool isCompressed = (sound->flags & AUDIO_ENTRY_FLAG_IS_COMPRESSED) != 0;
        bool inAudo = !isRegular || isEmbedded || isCompressed;

        if (inAudo) {
           // Embedded audio: decode from AUDO chunk memory
            if (0 > sound->audioFile || (uint32_t) sound->audioFile >= ma->base.audioGroups[sound->audioGroup]->audo.count) {
                logWarn("Audio: Invalid audio file index %d for sound '%s'\n", sound->audioFile, sound->name);
                return -1;
            }

            DataWin_loadAudoIfNeeded(ma->base.audioGroups[sound->audioGroup], (uint32_t)sound->audioFile);
            AudioEntry* entry = &ma->base.audioGroups[sound->audioGroup]->audo.entries[sound->audioFile];

            uint32_t channels = 0;
            uint32_t sampleRate = 0;
            uint32_t bitsPerSample = 0;
            const uint8_t* audioData = nullptr;
            uint32_t audioDataLen = 0;

            if (!parseWavHeader(entry->data, entry->dataSize, &channels, &sampleRate, &bitsPerSample, &audioData, &audioDataLen)) {
                channels = 2;
                sampleRate = 44100;
                bitsPerSample = 16;
                audioData = entry->data;
                audioDataLen = entry->dataSize;
            }

            if (audioData == nullptr || audioDataLen == 0) {
                logWarn("Audio: No audio data for '%s'\n", sound->name);
                return -1;
            }

            uint32_t format;
            if (channels == 1)
            {
                if (bitsPerSample == 8)
                    format = AL_FORMAT_MONO8;
                else
                    format = AL_FORMAT_MONO16;
            }
            else {
                if (bitsPerSample == 8)
                    format = AL_FORMAT_STEREO8;
                else
                    format = AL_FORMAT_STEREO16;
            }
#if defined(IS_BIG_ENDIAN)
            if (bitsPerSample == 16) {
                int16_t* swapped = (int16_t*)safeMalloc(audioDataLen);
                memcpy(swapped, audioData, audioDataLen);
                for (uint32_t i = 0, n = audioDataLen / sizeof(int16_t); i < n; i++) {
                    swapped[i] = (int16_t)BinaryUtils_bswap16((uint16_t)swapped[i]);
                }
                alBufferData(slot->alBuffer, format, swapped, audioDataLen, sampleRate);
                free(swapped);
            } else {
                alBufferData(slot->alBuffer, format, audioData, audioDataLen, sampleRate);
            }
#else
            alBufferData(slot->alBuffer, format, audioData, audioDataLen, sampleRate);
#endif
            ALenum alErr = alGetError();
            if (alErr != AL_NO_ERROR) {
                logWarn("Audio: alBufferData failed for '%s' format=0x%x len=%u rate=%u err=%d\n",
                    sound->name, format, audioDataLen, sampleRate, alErr);
                return -1;
            }
            alSourcei(slot->alSource, AL_BUFFER, slot->alBuffer);
        } else {
            // External audio: load from file
            char* path = resolveExternalPath(ma, sound);
            if (path == nullptr) {
                logWarn("Audio: Could not resolve path for sound '%s'\n", sound->name);
                return -1;
            }

            int channels;
            int sample_rate;
            short* data = NULL;
            int len = stb_vorbis_decode_filename(path, &channels, &sample_rate, &data);
            if (len <= 0 || data == nullptr) {
                logWarn("Audio: stb_vorbis_decode failed for '%s' path='%s' len=%d\n", sound->name, path, len);
                free(path);
                return -1;
            }
            alBufferData(
                slot->alBuffer,
                (channels == 2) ? AL_FORMAT_STEREO16 : AL_FORMAT_MONO16,
                (void*)data,
                len*channels*sizeof(uint16_t),
                sample_rate
            );
            if (alGetError() != AL_NO_ERROR) {
                logWarn("Audio: alBufferData failed for external '%s'\n", sound->name);
                free(data);
                free(path);
                return -1;
            }
            alSourcei(slot->alSource, AL_BUFFER, slot->alBuffer);
            if(data != NULL) free(data);
            free(path);
        }
        }
    }

    // Apply properties
    float volume = isStream ? streamGain : (isBufferSound ? 1.0f : sound->volume);
    float pitch = isStream ? streamPitch : (isBufferSound ? 1.0f : sound->pitch);
    alSourcef(slot->alSource, AL_GAIN, volume);

    if (pitch != 1.0f) {
        alSourcef(slot->alSource, AL_PITCH, pitch != 0.0f ? pitch : 1.0f);
    }
    // AL_LOOPING on a streaming source only loops the currently-playing buffer, not the whole queue,
    // so all queued streams loop in streamFillBuffer instead.
    if (!slot->streaming)
        alSourcei(slot->alSource, AL_LOOPING, loop ? AL_TRUE : AL_FALSE);

    // Set up instance tracking
    slot->active = true;
    slot->soundIndex = soundIndex;
    slot->instanceId = SOUND_INSTANCE_ID_BASE + slotIndex;
    slot->currentGain = volume;
    slot->targetGain = volume;
    slot->fadeTimeRemaining = 0.0f;
    slot->fadeTotalTime = 0.0f;
    slot->startGain = volume;
    slot->priority = priority;

    // Track unique IDs for disambiguation
    ma->nextInstanceCounter++;

    alSourcePlay(slot->alSource);
    if (alGetError() != AL_NO_ERROR) {
        logWarn("Audio: alSourcePlay failed for sound %d (stream=%d)\n", soundIndex, isStream);
    } else if (isBufferSound) {
        logInfo("Audio: Playing runtime PCM sound %d as instance %d (loop=%d)\n",
                soundIndex, slot->instanceId, loop);
    }

    return slot->instanceId;
}

static void maStopSound(AudioSystem* audio, int32_t soundOrInstance) {
    AlAudioSystem* ma = (AlAudioSystem*) audio;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        // Stop specific instance
        SoundInstance* inst = findInstanceById(ma, soundOrInstance);
        if (inst != nullptr) releaseInstance(inst);
    } else {
        // Stop all instances of this sound resource
        repeat(MAX_SOUND_INSTANCES, i) {
            SoundInstance* inst = &ma->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) {
                releaseInstance(inst);
            }
        }
    }
}

static void maStopAll(AudioSystem* audio) {
    AlAudioSystem* ma = (AlAudioSystem*) audio;

    repeat(MAX_SOUND_INSTANCES, i) {
        releaseInstance(&ma->instances[i]);
    }
}

static bool maIsPlaying(AudioSystem* audio, int32_t soundOrInstance) {
    AlAudioSystem* ma = (AlAudioSystem*) audio;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        SoundInstance* inst = findInstanceById(ma, soundOrInstance);
        if (inst == nullptr)
            return false;

        // Streaming sources can flip to AL_STOPPED for a frame during underrun, so trust the active flag instead (cleared by maUpdate when fully drained).
        if (inst->streaming)
            return inst->active;

        return alSourceIsPlaying(inst->alSource);
    } else {
        // Check if any instance of this sound resource is playing
        repeat(MAX_SOUND_INSTANCES, i) {
            SoundInstance* inst = &ma->instances[i];
            if (!inst->active || inst->soundIndex != soundOrInstance) continue;
            if (inst->streaming) return true;
            if (alSourceIsPlaying(inst->alSource)) return true;
        }
        return false;
    }
}

static void pauseInstanceIfPlaying(SoundInstance* inst) {
    ALint state = AL_INITIAL;
    alGetSourcei(inst->alSource, AL_SOURCE_STATE, &state);
    if (state == AL_PLAYING)
        alSourcePause(inst->alSource);
}

static void resumeInstanceIfPaused(SoundInstance* inst) {
    ALint state = AL_INITIAL;
    alGetSourcei(inst->alSource, AL_SOURCE_STATE, &state);
    // alSourcePlay on an already-playing OpenAL source restarts it from the
    // beginning. GameMaker games commonly call audio_resume_sound every step,
    // so only issue the OpenAL command for an actually paused source.
    if (state == AL_PAUSED)
        alSourcePlay(inst->alSource);
}

static void maPauseSound(AudioSystem* audio, int32_t soundOrInstance) {
    AlAudioSystem* ma = (AlAudioSystem*) audio;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        SoundInstance* inst = findInstanceById(ma, soundOrInstance);
        if (inst != nullptr)
            pauseInstanceIfPlaying(inst);
    } else {
        repeat(MAX_SOUND_INSTANCES, i) {
            SoundInstance* inst = &ma->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance)
                pauseInstanceIfPlaying(inst);
        }
    }
}

static void maResumeSound(AudioSystem* audio, int32_t soundOrInstance) {
    AlAudioSystem* ma = (AlAudioSystem*) audio;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        SoundInstance* inst = findInstanceById(ma, soundOrInstance);
        if (inst != nullptr)
            resumeInstanceIfPaused(inst);
    } else {
        repeat(MAX_SOUND_INSTANCES, i) {
            SoundInstance* inst = &ma->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance)
                resumeInstanceIfPaused(inst);
        }
    }
}

static void maPauseAll(AudioSystem* audio) {
    AlAudioSystem* ma = (AlAudioSystem*) audio;

    repeat(MAX_SOUND_INSTANCES, i) {
        SoundInstance* inst = &ma->instances[i];
        if (inst->active && alSourceIsPlaying(inst->alSource)) {
            alSourcePause(inst->alSource);
        }
    }
}

static void maResumeAll(AudioSystem* audio) {
    AlAudioSystem* ma = (AlAudioSystem*) audio;

    repeat(MAX_SOUND_INSTANCES, i) {
        SoundInstance* inst = &ma->instances[i];
        if (inst->active) {
            alSourcePlay(inst->alSource);
        }
    }
}

static void alSuspend(AudioSystem* audio) {
    AlAudioSystem* ma = (AlAudioSystem*) audio;

    repeat(MAX_SOUND_INSTANCES, i) {
        SoundInstance* inst = &ma->instances[i];
        if (inst->active && alSourceIsPlaying(inst->alSource)) {
            alSourcePause(inst->alSource);
        }
    }
}

static void alResume(AudioSystem* audio) {
    AlAudioSystem* ma = (AlAudioSystem*) audio;

    repeat(MAX_SOUND_INSTANCES, i) {
        SoundInstance* inst = &ma->instances[i];
        if (!inst->active) continue;
        ALint state = 0;
        alGetSourcei(inst->alSource, AL_SOURCE_STATE, &state);
        if (state == AL_PAUSED) {
            alSourcePlay(inst->alSource);
        }
    }
}

static void maSetSoundGain(AudioSystem* audio, int32_t soundOrInstance, float gain, uint32_t timeMs) {
    AlAudioSystem* ma = (AlAudioSystem*) audio;

    if (soundOrInstance >= AUDIO_STREAM_INDEX_BASE) {
        int32_t streamSlot = soundOrInstance - AUDIO_STREAM_INDEX_BASE;
        AudioStreamEntry* stream = &ma->streams[streamSlot];

        if (stream != nullptr) {
            stream->initialGain = gain;
        }
        // We want it to "fallthrough" to the check below so that any playing instances are updated
    }

    if (isValidSoundInstanceId(soundOrInstance)) {
        SoundInstance* inst = findInstanceById(ma, soundOrInstance);
        if (inst != nullptr) {
            if (timeMs == 0) {
                inst->currentGain = gain;
                inst->targetGain = gain;
                inst->fadeTimeRemaining = 0.0f;
                alSourcef(inst->alSource, AL_GAIN, gain);
            } else {
                inst->startGain = inst->currentGain;
                inst->targetGain = gain;
                inst->fadeTotalTime = (float) timeMs / 1000.0f;
                inst->fadeTimeRemaining = inst->fadeTotalTime;
            }
        }
    } else {
        if (AUDIO_STREAM_INDEX_BASE > soundOrInstance || DataWin_isVersionAtLeast(audio->dw, 2024, 11, 0, 0)) {
            repeat(MAX_SOUND_INSTANCES, i) {
                SoundInstance* inst = &ma->instances[i];
                if (inst->active && inst->soundIndex == soundOrInstance) {
                    if (timeMs == 0) {
                        inst->currentGain = gain;
                        inst->targetGain = gain;
                        inst->fadeTimeRemaining = 0.0f;
                        alSourcef(inst->alSource, AL_GAIN, gain);
                    } else {
                        inst->startGain = inst->currentGain;
                        inst->targetGain = gain;
                        inst->fadeTotalTime = (float) timeMs / 1000.0f;
                        inst->fadeTimeRemaining = inst->fadeTotalTime;
                    }
                }
            }
        }
    }
}

static float maGetSoundGain(AudioSystem* audio, int32_t soundOrInstance) {
    AlAudioSystem* ma = (AlAudioSystem*) audio;

    if (isValidSoundInstanceId(soundOrInstance)) {
        SoundInstance* inst = findInstanceById(ma, soundOrInstance);
        if (inst != nullptr) return inst->currentGain;
    } else {
        repeat(MAX_SOUND_INSTANCES, i) {
            SoundInstance* inst = &ma->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) {
                return inst->currentGain;
            }
        }
    }
    return 0.0f;
}

static void maSetSoundPitch(AudioSystem* audio, int32_t soundOrInstance, float pitch) {
    AlAudioSystem* ma = (AlAudioSystem*) audio;

    if (soundOrInstance >= AUDIO_STREAM_INDEX_BASE) {
        int32_t streamSlot = soundOrInstance - AUDIO_STREAM_INDEX_BASE;
        AudioStreamEntry* stream = &ma->streams[streamSlot];

        if (stream != nullptr) {
            stream->initialPitch = pitch;
        }
        // We want it to "fallthrough" to the check below so that any playing instances are updated
    }

    if (isValidSoundInstanceId(soundOrInstance)) {
        SoundInstance* inst = findInstanceById(ma, soundOrInstance);
        if (inst != nullptr) {
            alSourcef(inst->alSource, AL_PITCH, pitch);
        }
    } else {
        repeat(MAX_SOUND_INSTANCES, i) {
            SoundInstance* inst = &ma->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) {
                alSourcef(inst->alSource, AL_PITCH, pitch);
            }
        }
    }
}

static float maGetSoundPitch(AudioSystem* audio, int32_t soundOrInstance) {
    AlAudioSystem* ma = (AlAudioSystem*) audio;

    float pitch = 1.0f;
    if (isValidSoundInstanceId(soundOrInstance)) {
        SoundInstance* inst = findInstanceById(ma, soundOrInstance);
        if (inst != nullptr) alGetSourcef(inst->alSource, AL_PITCH, &pitch);
    } else {
        repeat(MAX_SOUND_INSTANCES, i) {
            SoundInstance* inst = &ma->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) {
                alGetSourcef(inst->alSource, AL_PITCH, &pitch);
            }
        }
    }
    return pitch;
}

// For streaming instances AL_SEC_OFFSET resets per buffer in the queue, so we combine the dequeued-sample tally with the offset into the currently-playing buffer to report a position over the whole track.
static float streamCursorSeconds(SoundInstance* inst) {
    if (0 >= inst->streamSampleRate)
        return 0.0f;

    ALint sampleOffset = 0;
    alGetSourcei(inst->alSource, AL_SAMPLE_OFFSET, &sampleOffset);
    uint64_t total = inst->playedSamples + (uint64_t) sampleOffset;
    return (float) total / (float) inst->streamSampleRate;
}

// Rebuild a queued stream at an absolute track position. AL_SEC_OFFSET only
// addresses the current queued buffer, so it cannot implement GameMaker's
// audio_sound_set_track_position for streamed BGM by itself.
static bool streamSeekSeconds(SoundInstance* inst, float positionSeconds) {
    if (!inst->streaming || inst->streamSampleRate <= 0) return false;
    if (positionSeconds < 0.0f) positionSeconds = 0.0f;
    if (inst->streamLengthSeconds > 0.0f && positionSeconds > inst->streamLengthSeconds)
        positionSeconds = inst->streamLengthSeconds;

    ALint oldState = AL_STOPPED;
    alGetSourcei(inst->alSource, AL_SOURCE_STATE, &oldState);
    alSourceStop(inst->alSource);

    ALint queued = 0;
    alGetSourcei(inst->alSource, AL_BUFFERS_QUEUED, &queued);
    repeat(queued, i) {
        ALuint buffer;
        alSourceUnqueueBuffers(inst->alSource, 1, &buffer);
    }

    uint64_t targetSample = (uint64_t)(positionSeconds * (float)inst->streamSampleRate);
    if (inst->streamIsPcm) {
        int32_t bytesPerFrame = inst->streamChannels * (inst->streamBitsPerSample / 8);
        uint64_t byteOffset = targetSample * (uint64_t)bytesPerFrame;
        if (byteOffset > (uint64_t)inst->streamPcmSize) byteOffset = (uint64_t)inst->streamPcmSize;
        inst->streamPcmCursor = (int32_t)byteOffset;
    } else {
        if (!stb_vorbis_seek((stb_vorbis*)inst->vorbis, (unsigned int)targetSample))
            return false;
    }

    inst->playedSamples = targetSample;
    inst->streamEnded = false;
    int primed = 0;
    for (int i = 0; AL_STREAM_BUFFER_COUNT > i; i++) {
        if (!streamFillBuffer(inst, inst->streamBuffers[i])) break;
        alSourceQueueBuffers(inst->alSource, 1, &inst->streamBuffers[i]);
        if (alGetError() != AL_NO_ERROR) break;
        primed++;
    }
    if (primed == 0) {
        inst->streamEnded = true;
        return false;
    }

    if (oldState == AL_PLAYING) {
        alSourcePlay(inst->alSource);
    } else if (oldState == AL_PAUSED) {
        alSourcePlay(inst->alSource);
        alSourcePause(inst->alSource);
    }
    bool ok = alGetError() == AL_NO_ERROR;
    if (ok) logInfo("Audio: Sought queued stream to %.3f seconds\n", positionSeconds);
    return ok;
}

static float maGetTrackPosition(AudioSystem* audio, int32_t soundOrInstance) {
    AlAudioSystem* ma = (AlAudioSystem*) audio;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        SoundInstance* inst = findInstanceById(ma, soundOrInstance);
        if (inst != nullptr) {
            if (inst->streaming) return streamCursorSeconds(inst);
            float cursor;
            alGetSourcef(inst->alSource, AL_SEC_OFFSET, &cursor);
            return cursor;
        }
    } else {
        repeat(MAX_SOUND_INSTANCES, i) {
            SoundInstance* inst = &ma->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) {
                if (inst->streaming) return streamCursorSeconds(inst);
                float cursor;
                alGetSourcef(inst->alSource, AL_SEC_OFFSET, &cursor);
                return cursor;
            }
        }
    }
    return 0.0f;
}

static void maSetTrackPosition(AudioSystem* audio, int32_t soundOrInstance, float positionSeconds) {
    AlAudioSystem* ma = (AlAudioSystem*) audio;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        SoundInstance* inst = findInstanceById(ma, soundOrInstance);
        if (inst != nullptr) {
            if (inst->streaming) streamSeekSeconds(inst, positionSeconds);
            else alSourcef(inst->alSource, AL_SEC_OFFSET, positionSeconds);
        }
    } else {
        repeat(MAX_SOUND_INSTANCES, i) {
            SoundInstance* inst = &ma->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) {
                if (inst->streaming) streamSeekSeconds(inst, positionSeconds);
                else alSourcef(inst->alSource, AL_SEC_OFFSET, positionSeconds);
            }
        }
    }
}

// Total length of a loaded sound. Works on both SOND index and active instance ids.
static float maGetSoundLength(AudioSystem* audio, int32_t soundOrInstance) {
    AlAudioSystem* ma = (AlAudioSystem*) audio;

    SoundInstance* match = nullptr;
    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        match = findInstanceById(ma, soundOrInstance);
    } else {
        repeat(MAX_SOUND_INSTANCES, i) {
            SoundInstance* inst = &ma->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) {
                match = inst;
                break;
            }
        }
    }
    if (match != nullptr) {
        if (match->streaming) return match->streamLengthSeconds;
        float seconds = 0.0f;
        alGetSourceLengthSec(match->alBuffer, &seconds);
        return seconds;
    }

    // No active instance: GMS audio_sound_length(soundIndex) must still return the asset's duration.
    if (soundOrInstance >= AUDIO_BUFFER_SOUND_INDEX_BASE && soundOrInstance < AUDIO_STREAM_INDEX_BASE) {
        int32_t slot = soundOrInstance - AUDIO_BUFFER_SOUND_INDEX_BASE;
        if (slot >= 0 && slot < MAX_AUDIO_BUFFER_SOUNDS && ma->bufferSounds[slot].active) {
            AudioBufferSoundEntry* entry = &ma->bufferSounds[slot];
            int32_t bytesPerFrame = entry->channels * (entry->bitsPerSample / 8);
            if (bytesPerFrame > 0 && entry->sampleRate > 0)
                return (float) entry->size / (float) (bytesPerFrame * entry->sampleRate);
        }
        return 0.0f;
    }
    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE || soundOrInstance >= AUDIO_STREAM_INDEX_BASE)
        return 0.0f;

    DataWin* dw = ma->base.audioGroups[0];
    if (dw == nullptr || 0 > soundOrInstance || (uint32_t) soundOrInstance >= dw->sond.count)
        return 0.0f;

    Sound* sound = &dw->sond.sounds[soundOrInstance];

    bool isRegular = (sound->flags & AUDIO_ENTRY_FLAG_REGULAR) == AUDIO_ENTRY_FLAG_REGULAR;
    bool isEmbedded = (sound->flags & AUDIO_ENTRY_FLAG_IS_EMBEDDED) != 0;
    bool isCompressed = (sound->flags & AUDIO_ENTRY_FLAG_IS_COMPRESSED) != 0;
    bool inAudo = !isRegular || isEmbedded || isCompressed;
    if (inAudo) {
        if (0 > sound->audioFile || (uint32_t) sound->audioFile >= ma->base.audioGroups[sound->audioGroup]->audo.count) return 0.0f;
        DataWin_loadAudoIfNeeded(ma->base.audioGroups[sound->audioGroup], (uint32_t)sound->audioFile);
        AudioEntry* entry = &ma->base.audioGroups[sound->audioGroup]->audo.entries[sound->audioFile];

        uint32_t channels, sampleRate, bitsPerSample, audioDataLen;
        if (parseWavHeader(entry->data, entry->dataSize, &channels, &sampleRate, &bitsPerSample, nullptr, &audioDataLen)) {
            uint32_t bytesPerSample = channels * (bitsPerSample / 8);
            if (bytesPerSample > 0)
                return (float) audioDataLen / (float)(sampleRate * bytesPerSample);
        } else {
            // Raw PCM: assume stereo 16-bit 44100 Hz
            return (float) entry->dataSize / (float)(44100 * 2 * 2);
        }
        return 0.0f;
    }

    char* path = resolveExternalPath(ma, sound);
    if (path == nullptr) return 0.0f;
    int err = 0;
    stb_vorbis* v = stb_vorbis_open_filename(path, &err, nullptr);
    free(path);
    if (v == nullptr) return 0.0f;
    float seconds = stb_vorbis_stream_length_in_seconds(v);
    stb_vorbis_close(v);
    return seconds;
}

static void maSetMasterGainForListener(AudioSystem* audio, float gain, int32_t id) {
    (void)audio;
    (void)id;
    alListenerf(AL_GAIN, gain);
}

static void maSetMasterGain(AudioSystem* audio, float gain) {
    (void)audio;
    alListenerf(AL_GAIN, gain);
}

static void maSetChannelCount(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED int32_t count) {
    // miniaudio handles channel management internally, this is a no-op
}

static void maGroupLoad(AudioSystem* audio, int32_t groupIndex) {
    if (groupIndex > 0) {
        int sz = snprintf(nullptr, 0, "audiogroup%d.dat", groupIndex);
        char *buf = (char *)safeMalloc(sz + 1);
        snprintf(buf, sz + 1, "audiogroup%d.dat", groupIndex);

        // The original runner does not care if the file doesn't exist (this may happen if someone uses "audio_group_load" on a non-existent group)
        FileSystem* fileSystem = ((AlAudioSystem*)audio)->fileSystem;
        char* resolvedPath = (((AlAudioSystem*)audio)->fileSystem->vtable->resolvePath(((AlAudioSystem*)audio)->fileSystem, buf));
        if (!fileSystem->vtable->fileExists(fileSystem, resolvedPath)) {
            logWarn("Audio: Wanted to load Audio Group %d, but Audio Group %d does not exist!\n", groupIndex, groupIndex);
            free(buf);
            return;
        }

        DataWinParserOptions options = {0};
        options.parseAudo = true;
        options.lazyLoadAudio = audio->dw->lazyLoadAudio;
        if (audio->dw->mappedFile)
            options.loadType = DATAWINLOADTYPE_MAP_FILE;
        DataWin *audioGroup = DataWin_parse(((AlAudioSystem*)audio)->fileSystem->vtable->resolvePath(((AlAudioSystem*)audio)->fileSystem, buf), options);
        arrput(audio->audioGroups, audioGroup);
        free(buf);
    }
}

static bool maGroupIsLoaded(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED int32_t groupIndex) {
    return (arrlen(audio->audioGroups) > groupIndex);
}

// ===[ Audio Streams ]===

static int32_t maCreateStream(AudioSystem* audio, const char* filename) {
    AlAudioSystem* ma = (AlAudioSystem*) audio;

    // Find a free stream slot
    int32_t freeSlot = -1;
    repeat(MAX_AUDIO_STREAMS, i) {
        if (!ma->streams[i].active) {
            freeSlot = (int32_t) i;
            break;
        }
    }

    if (0 > freeSlot) {
        logWarn("Audio: No free stream slots for '%s'\n", filename);
        return -1;
    }

    char* resolved = ma->fileSystem->vtable->resolvePath(ma->fileSystem, filename);
    if (resolved == nullptr) {
        logWarn("Audio: Could not resolve path for stream '%s'\n", filename);
        return -1;
    }

    ma->streams[freeSlot].active = true;
    ma->streams[freeSlot].filePath = resolved;
    ma->streams[freeSlot].initialGain = 1.0f;
    ma->streams[freeSlot].initialPitch = 1.0f;

    int32_t streamIndex = AUDIO_STREAM_INDEX_BASE + freeSlot;
    logInfo("Audio: Created stream %d for '%s' -> '%s'\n", streamIndex, filename, resolved);
    return streamIndex;
}

static bool maDestroyStream(AudioSystem* audio, int32_t streamIndex) {
    AlAudioSystem* ma = (AlAudioSystem*) audio;

    int32_t slotIndex = streamIndex - AUDIO_STREAM_INDEX_BASE;
    if (0 > slotIndex || slotIndex >= MAX_AUDIO_STREAMS) {
        logWarn("Audio: Invalid stream index %d for destroy\n", streamIndex);
        return false;
    }

    AudioStreamEntry* entry = &ma->streams[slotIndex];
    if (!entry->active) return false;

    // Stop all sound instances that were playing this stream
    repeat(MAX_SOUND_INSTANCES, i) {
        SoundInstance* inst = &ma->instances[i];
        if (inst->active && inst->soundIndex == streamIndex) {
            releaseInstance(inst);
        }
    }

    free(entry->filePath);
    entry->filePath = nullptr;
    entry->active = false;
    logInfo("Audio: Destroyed stream %d\n", streamIndex);
    return true;
}

static int32_t maCreateBufferSound(AudioSystem* audio, const uint8_t* data, int32_t size,
                                   int32_t bitsPerSample, int32_t sampleRate, int32_t channels) {
    AlAudioSystem* ma = (AlAudioSystem*) audio;
    if (data == nullptr || size <= 0 || (bitsPerSample != 8 && bitsPerSample != 16)
        || sampleRate <= 0 || (channels != 1 && channels != 2)) return -1;

    repeat(MAX_AUDIO_BUFFER_SOUNDS, i) {
        AudioBufferSoundEntry* entry = &ma->bufferSounds[i];
        if (entry->active) continue;
        entry->data = (uint8_t*) safeMalloc((size_t) size);
        memcpy(entry->data, data, (size_t) size);
        entry->size = size;
        entry->bitsPerSample = bitsPerSample;
        entry->sampleRate = sampleRate;
        entry->channels = channels;
        entry->active = true;
        int32_t soundIndex = AUDIO_BUFFER_SOUND_INDEX_BASE + (int32_t) i;
        logInfo("Audio: Created runtime PCM sound %d (%d bytes, %d-bit, %d Hz, %d channel(s))\n",
                soundIndex, size, bitsPerSample, sampleRate, channels);
        return soundIndex;
    }
    logWarn("Audio: No free runtime buffer-sound slots\n");
    return -1;
}

static bool maDestroyBufferSound(AudioSystem* audio, int32_t bufferSoundIndex) {
    AlAudioSystem* ma = (AlAudioSystem*) audio;
    int32_t slot = bufferSoundIndex - AUDIO_BUFFER_SOUND_INDEX_BASE;
    if (slot < 0 || slot >= MAX_AUDIO_BUFFER_SOUNDS || !ma->bufferSounds[slot].active) return false;

    repeat(MAX_SOUND_INSTANCES, i) {
        if (ma->instances[i].active && ma->instances[i].soundIndex == bufferSoundIndex)
            releaseInstance(&ma->instances[i]);
    }
    free(ma->bufferSounds[slot].data);
    ZERO_STRUCT(ma->bufferSounds[slot]);
    logInfo("Audio: Destroyed runtime PCM sound %d\n", bufferSoundIndex);
    return true;
}

// ===[ Vtable ]===

static AudioSystemVtable AlAudioSystemVtable;

// ===[ Lifecycle ]===

AlAudioSystem* AlAudioSystem_create(void) {
    AlAudioSystem* ma = (AlAudioSystem *)safeCalloc(1, sizeof(AlAudioSystem));
    AlAudioSystemVtable.init = maInit;
    AlAudioSystemVtable.destroy = maDestroy;
    AlAudioSystemVtable.update = maUpdate;
    AlAudioSystemVtable.playSound = maPlaySound;
    AlAudioSystemVtable.stopSound = maStopSound;
    AlAudioSystemVtable.stopAll = maStopAll;
    AlAudioSystemVtable.isPlaying = maIsPlaying;
    AlAudioSystemVtable.pauseSound = maPauseSound;
    AlAudioSystemVtable.resumeSound = maResumeSound;
    AlAudioSystemVtable.pauseAll = maPauseAll;
    AlAudioSystemVtable.resumeAll = maResumeAll;
    AlAudioSystemVtable.suspend = alSuspend;
    AlAudioSystemVtable.resume = alResume;
    AlAudioSystemVtable.setSoundGain = maSetSoundGain;
    AlAudioSystemVtable.getSoundGain = maGetSoundGain;
    AlAudioSystemVtable.setSoundPitch = maSetSoundPitch;
    AlAudioSystemVtable.getSoundPitch = maGetSoundPitch;
    AlAudioSystemVtable.getTrackPosition = maGetTrackPosition;
    AlAudioSystemVtable.setTrackPosition = maSetTrackPosition;
    AlAudioSystemVtable.getSoundLength = maGetSoundLength;
    AlAudioSystemVtable.setMasterGain = maSetMasterGain;
    AlAudioSystemVtable.setMasterGainForListener = maSetMasterGainForListener;
    AlAudioSystemVtable.setChannelCount = maSetChannelCount;
    AlAudioSystemVtable.groupLoad = maGroupLoad;
    AlAudioSystemVtable.groupIsLoaded = maGroupIsLoaded;
    AlAudioSystemVtable.createStream = maCreateStream;
    AlAudioSystemVtable.destroyStream = maDestroyStream;
    AlAudioSystemVtable.createBufferSound = maCreateBufferSound;
    AlAudioSystemVtable.destroyBufferSound = maDestroyBufferSound;
    ma->base.vtable = &AlAudioSystemVtable;
    return ma;
}
