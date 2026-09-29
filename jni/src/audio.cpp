#include "audio.h"
#include <cstring>

#if __has_include("media/audio.h")
#  include "media/audio.h"
#  define AUDIO_AVAILABLE
#endif
#ifdef AUDIO_AVAILABLE
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

// enum SoundId — в audio.h (раньше был и здесь, в ветке AUDIO_AVAILABLE:
// после выноса в заголовок повторное определение ломало NDK-сборку).

struct AudioState {
    SLObjectItf engineObj  = nullptr;
    SLEngineItf engineItf  = nullptr;
    SLObjectItf outputMix  = nullptr;
    struct Player {
        SLObjectItf                   obj  = nullptr;
        SLPlayItf                     play = nullptr;
        SLAndroidSimpleBufferQueueItf bq   = nullptr;
    } players[SND_COUNT];
};
static AudioState g_audio;

void AudioInit() {
    SLEngineOption opt[] = {{ SL_ENGINEOPTION_THREADSAFE, SL_BOOLEAN_TRUE }};
    if (slCreateEngine(&g_audio.engineObj, 1, opt, 0, nullptr, nullptr) != SL_RESULT_SUCCESS) return;
    if ((*g_audio.engineObj)->Realize(g_audio.engineObj, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) return;
    if ((*g_audio.engineObj)->GetInterface(g_audio.engineObj, SL_IID_ENGINE, &g_audio.engineItf) != SL_RESULT_SUCCESS) return;
    if ((*g_audio.engineItf)->CreateOutputMix(g_audio.engineItf, &g_audio.outputMix, 0, nullptr, nullptr) != SL_RESULT_SUCCESS) return;
    if ((*g_audio.outputMix)->Realize(g_audio.outputMix, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) return;

    const struct { const uint8_t* data; size_t len; } srcs[SND_COUNT] = {
        { click_wav,   click_wav_len   },
        { success_wav, success_wav_len },
    };

    for (int i = 0; i < SND_COUNT; i++) {
        const uint8_t* wav = srcs[i].data;
        uint16_t channels = 0; uint32_t sampleRate = 0; uint16_t bps = 0;
        memcpy(&channels,   wav + 22, 2);
        memcpy(&sampleRate, wav + 24, 4);
        memcpy(&bps,        wav + 34, 2);

        SLDataLocator_AndroidSimpleBufferQueue loc = { SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 1 };
        SLDataFormat_PCM fmt = {
            SL_DATAFORMAT_PCM, (SLuint32)channels, (SLuint32)sampleRate * 1000,
            (SLuint32)bps, (SLuint32)bps,
            channels == 2 ? SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT : SL_SPEAKER_FRONT_CENTER,
            SL_BYTEORDER_LITTLEENDIAN
        };
        SLDataSource src  = { &loc, &fmt };
        SLDataLocator_OutputMix outLoc = { SL_DATALOCATOR_OUTPUTMIX, g_audio.outputMix };
        SLDataSink   sink = { &outLoc, nullptr };

        const SLInterfaceID ids[] = { SL_IID_BUFFERQUEUE };
        const SLboolean     req[] = { SL_BOOLEAN_TRUE };
        if ((*g_audio.engineItf)->CreateAudioPlayer(g_audio.engineItf, &g_audio.players[i].obj, &src, &sink, 1, ids, req) != SL_RESULT_SUCCESS) continue;
        if ((*g_audio.players[i].obj)->Realize(g_audio.players[i].obj, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) continue;
        (*g_audio.players[i].obj)->GetInterface(g_audio.players[i].obj, SL_IID_PLAY,        &g_audio.players[i].play);
        (*g_audio.players[i].obj)->GetInterface(g_audio.players[i].obj, SL_IID_BUFFERQUEUE, &g_audio.players[i].bq);
        (*g_audio.players[i].play)->SetPlayState(g_audio.players[i].play, SL_PLAYSTATE_PLAYING);
    }
}

void PlaySound(SoundId id) {
    static const struct { const uint8_t* data; size_t len; } srcs[SND_COUNT] = {
        { click_wav,   click_wav_len   },
        { success_wav, success_wav_len },
    };
    auto& p = g_audio.players[id];
    if (!p.bq || !p.play) return;
    (*p.play)->SetPlayState(p.play, SL_PLAYSTATE_STOPPED);
    (*p.bq)->Clear(p.bq);
    (*p.bq)->Enqueue(p.bq, (void*)(srcs[id].data + 44), (SLuint32)(srcs[id].len - 44));
    (*p.play)->SetPlayState(p.play, SL_PLAYSTATE_PLAYING);
}

void AudioFree() {
    for (int i = 0; i < SND_COUNT; i++) {
        if (g_audio.players[i].obj) { (*g_audio.players[i].obj)->Destroy(g_audio.players[i].obj); g_audio.players[i].obj = nullptr; }
    }
    if (g_audio.outputMix) { (*g_audio.outputMix)->Destroy(g_audio.outputMix); g_audio.outputMix = nullptr; }
    if (g_audio.engineObj) { (*g_audio.engineObj)->Destroy(g_audio.engineObj); g_audio.engineObj = nullptr; }
}
#else
void AudioInit() {}
void PlaySound(SoundId) {}
void AudioFree() {}
#endif