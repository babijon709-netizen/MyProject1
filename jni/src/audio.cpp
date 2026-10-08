#include "audio.h"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#if __has_include("media/audio.h")
#  include "media/audio.h"
#  define AUDIO_AVAILABLE
#endif

#ifdef AUDIO_AVAILABLE
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

namespace {

constexpr std::uint32_t kHitSampleRate = 22050;
constexpr std::size_t kHitFrameCount = 1764;
constexpr float kPi = 3.14159265358979323846f;

struct AudioClip {
    const std::uint8_t* data = nullptr;
    std::size_t length = 0;
    std::uint32_t sample_rate = 0;
    std::uint16_t channels = 0;
    std::uint16_t bits_per_sample = 0;
};

std::uint16_t read_u16le(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0]) |
           static_cast<std::uint16_t>(p[1] << 8);
}

std::uint32_t read_u32le(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

bool parse_wav(const std::uint8_t* wav, std::size_t length, AudioClip& out) {
    if (!wav || length < 12 || std::memcmp(wav, "RIFF", 4) != 0 ||
        std::memcmp(wav + 8, "WAVE", 4) != 0)
        return false;

    std::uint16_t format = 0;
    bool have_format = false;
    bool have_data = false;
    std::size_t offset = 12;
    while (offset <= length && length - offset >= 8) {
        const std::uint8_t* chunk = wav + offset;
        const std::uint32_t chunk_length = read_u32le(chunk + 4);
        const std::size_t payload = offset + 8;
        if (static_cast<std::size_t>(chunk_length) > length - payload)
            return false;

        if (std::memcmp(chunk, "fmt ", 4) == 0 && chunk_length >= 16) {
            format = read_u16le(wav + payload);
            out.channels = read_u16le(wav + payload + 2);
            out.sample_rate = read_u32le(wav + payload + 4);
            out.bits_per_sample = read_u16le(wav + payload + 14);
            have_format = true;
        } else if (std::memcmp(chunk, "data", 4) == 0) {
            out.data = wav + payload;
            out.length = chunk_length;
            have_data = true;
        }

        const std::size_t next = payload + static_cast<std::size_t>(chunk_length) + (chunk_length & 1u);
        if (next <= offset || next > length) break;
        offset = next;
    }

    return have_format && have_data && format == 1 &&
           (out.channels == 1 || out.channels == 2) &&
           out.sample_rate >= 8000 && out.sample_rate <= 192000 &&
           out.bits_per_sample == 16 && out.length > 0;
}

struct AudioState {
    SLObjectItf engine_obj = nullptr;
    SLEngineItf engine = nullptr;
    SLObjectItf output_mix = nullptr;
    struct Player {
        SLObjectItf object = nullptr;
        SLPlayItf play = nullptr;
        SLAndroidSimpleBufferQueueItf queue = nullptr;
    } players[SND_COUNT];
    AudioClip clips[SND_COUNT];
    std::int16_t hit_pcm[kHitFrameCount] = {};
};

AudioState g_audio;

void build_hit_sound() {
    float phase = 0.0f;
    for (std::size_t i = 0; i < kHitFrameCount; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kHitSampleRate);
        const float progress = static_cast<float>(i) / static_cast<float>(kHitFrameCount - 1);
        const float frequency = 1480.0f - 620.0f * progress;
        phase += 2.0f * kPi * frequency / static_cast<float>(kHitSampleRate);
        const float envelope = std::exp(-34.0f * t) * (1.0f - 0.16f * progress);
        const float wave = std::sin(phase) + 0.22f * std::sin(phase * 2.03f);
        long sample = std::lround(wave * envelope * 14500.0f);
        if (sample < -32768) sample = -32768;
        if (sample > 32767) sample = 32767;
        g_audio.hit_pcm[i] = static_cast<std::int16_t>(sample);
    }
}

bool create_player(const AudioClip& clip, AudioState::Player& player) {
    if (!clip.data || !clip.length || !clip.sample_rate ||
        clip.length > 0xFFFFFFFFu || !g_audio.engine || !g_audio.output_mix)
        return false;

    SLDataLocator_AndroidSimpleBufferQueue queue_locator = {
        SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 1
    };
    SLDataFormat_PCM pcm_format = {
        SL_DATAFORMAT_PCM,
        static_cast<SLuint32>(clip.channels),
        static_cast<SLuint32>(clip.sample_rate * 1000u),
        static_cast<SLuint32>(clip.bits_per_sample),
        static_cast<SLuint32>(clip.bits_per_sample),
        clip.channels == 2
            ? SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT
            : SL_SPEAKER_FRONT_CENTER,
        SL_BYTEORDER_LITTLEENDIAN
    };
    SLDataSource source = {&queue_locator, &pcm_format};
    SLDataLocator_OutputMix output_locator = {
        SL_DATALOCATOR_OUTPUTMIX, g_audio.output_mix
    };
    SLDataSink sink = {&output_locator, nullptr};
    const SLInterfaceID interfaces[] = {SL_IID_BUFFERQUEUE};
    const SLboolean required[] = {SL_BOOLEAN_TRUE};

    if ((*g_audio.engine)->CreateAudioPlayer(
            g_audio.engine, &player.object, &source, &sink,
            1, interfaces, required) != SL_RESULT_SUCCESS)
        return false;
    if ((*player.object)->Realize(player.object, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) {
        (*player.object)->Destroy(player.object);
        player = {};
        return false;
    }
    if ((*player.object)->GetInterface(player.object, SL_IID_PLAY, &player.play) != SL_RESULT_SUCCESS ||
        (*player.object)->GetInterface(player.object, SL_IID_BUFFERQUEUE, &player.queue) != SL_RESULT_SUCCESS) {
        (*player.object)->Destroy(player.object);
        player = {};
        return false;
    }
    (*player.play)->SetPlayState(player.play, SL_PLAYSTATE_PLAYING);
    return true;
}

}  // namespace

void AudioInit() {
    AudioFree();
    g_audio = AudioState{};

    SLEngineOption options[] = {{SL_ENGINEOPTION_THREADSAFE, SL_BOOLEAN_TRUE}};
    if (slCreateEngine(&g_audio.engine_obj, 1, options, 0, nullptr, nullptr) != SL_RESULT_SUCCESS) return;
    if ((*g_audio.engine_obj)->Realize(g_audio.engine_obj, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) return;
    if ((*g_audio.engine_obj)->GetInterface(g_audio.engine_obj, SL_IID_ENGINE, &g_audio.engine) != SL_RESULT_SUCCESS) return;
    if ((*g_audio.engine)->CreateOutputMix(g_audio.engine, &g_audio.output_mix, 0, nullptr, nullptr) != SL_RESULT_SUCCESS) return;
    if ((*g_audio.output_mix)->Realize(g_audio.output_mix, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) return;

    parse_wav(click_wav, click_wav_len, g_audio.clips[SND_CLICK]);
    parse_wav(success_wav, success_wav_len, g_audio.clips[SND_SUCCESS]);
    build_hit_sound();
    g_audio.clips[SND_HIT] = {
        reinterpret_cast<const std::uint8_t*>(g_audio.hit_pcm),
        sizeof(g_audio.hit_pcm), kHitSampleRate, 1, 16
    };

    for (int i = 0; i < SND_COUNT; ++i)
        create_player(g_audio.clips[i], g_audio.players[i]);
}

void PlaySound(SoundId id) {
    const int index = static_cast<int>(id);
    if (index < 0 || index >= SND_COUNT) return;

    static auto last_hit = std::chrono::steady_clock::time_point{};
    if (id == SND_HIT) {
        const auto now = std::chrono::steady_clock::now();
        if (last_hit.time_since_epoch().count() != 0 &&
            now - last_hit < std::chrono::milliseconds(55))
            return;
        last_hit = now;
    }

    auto& player = g_audio.players[index];
    const AudioClip& clip = g_audio.clips[index];
    if (!player.queue || !player.play || !clip.data || !clip.length) return;

    (*player.play)->SetPlayState(player.play, SL_PLAYSTATE_STOPPED);
    (*player.queue)->Clear(player.queue);
    if ((*player.queue)->Enqueue(player.queue, const_cast<std::uint8_t*>(clip.data),
                                 static_cast<SLuint32>(clip.length)) != SL_RESULT_SUCCESS)
        return;
    (*player.play)->SetPlayState(player.play, SL_PLAYSTATE_PLAYING);
}

void AudioFree() {
    for (int i = 0; i < SND_COUNT; ++i) {
        if (g_audio.players[i].object) {
            (*g_audio.players[i].object)->Destroy(g_audio.players[i].object);
            g_audio.players[i] = {};
        }
        g_audio.clips[i] = {};
    }
    if (g_audio.output_mix) {
        (*g_audio.output_mix)->Destroy(g_audio.output_mix);
        g_audio.output_mix = nullptr;
    }
    if (g_audio.engine_obj) {
        (*g_audio.engine_obj)->Destroy(g_audio.engine_obj);
        g_audio.engine_obj = nullptr;
        g_audio.engine = nullptr;
    }
}

#else
void AudioInit() {}
void PlaySound(SoundId) {}
void AudioFree() {}
#endif
