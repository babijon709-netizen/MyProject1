#pragma once
#include <cstdint>
#include <cstddef>
typedef uint32_t SLuint32; typedef int32_t SLint32; typedef uint8_t SLboolean;
typedef uint32_t SLresult; typedef const void* SLInterfaceID;
#define SL_RESULT_SUCCESS ((SLresult)0)
#define SL_BOOLEAN_FALSE ((SLboolean)0)
#define SL_BOOLEAN_TRUE  ((SLboolean)1)
#define SL_ENGINEOPTION_THREADSAFE ((SLuint32)1)
#define SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE ((SLuint32)0x80000701)
#define SL_DATALOCATOR_OUTPUTMIX ((SLuint32)3)
#define SL_DATAFORMAT_PCM ((SLuint32)1)
#define SL_SPEAKER_FRONT_LEFT ((SLuint32)0x1)
#define SL_SPEAKER_FRONT_RIGHT ((SLuint32)0x2)
#define SL_SPEAKER_FRONT_CENTER ((SLuint32)0x4)
#define SL_BYTEORDER_LITTLEENDIAN ((SLuint32)2)
#define SL_PLAYSTATE_STOPPED ((SLuint32)1)
#define SL_PLAYSTATE_PAUSED  ((SLuint32)2)
#define SL_PLAYSTATE_PLAYING ((SLuint32)3)
extern const SLInterfaceID SL_IID_ENGINE, SL_IID_PLAY, SL_IID_BUFFERQUEUE;
const SLInterfaceID SL_IID_ENGINE = (const void*)0x1, SL_IID_PLAY = (const void*)0x2,
                    SL_IID_BUFFERQUEUE = (const void*)0x3;
struct SLObjectItf_ {
    SLresult (*Realize)(const void*, SLboolean);
    SLresult (*GetInterface)(const void*, const void*, void*);
    void     (*Destroy)(const void*);
};
typedef const struct SLObjectItf_ *const *SLObjectItf;
struct SLEngineItf_ {
    SLresult (*CreateOutputMix)(const void*, SLObjectItf*, SLuint32, const SLInterfaceID*, const SLboolean*);
    SLresult (*CreateAudioPlayer)(const void*, SLObjectItf*, struct SLDataSource_*, struct SLDataSink_*,
                                  SLuint32, const SLInterfaceID*, const SLboolean*);
};
typedef const struct SLEngineItf_ *const *SLEngineItf;
struct SLPlayItf_ { SLresult (*SetPlayState)(const void*, SLuint32); };
typedef const struct SLPlayItf_ *const *SLPlayItf;
struct SLAndroidSimpleBufferQueueItf_ {
    SLresult (*Enqueue)(const void*, const void*, SLuint32);
    SLresult (*Clear)(const void*);
};
typedef const struct SLAndroidSimpleBufferQueueItf_ *const *SLAndroidSimpleBufferQueueItf;
struct SLEngineOption { SLuint32 feature; SLuint32 setting; };
struct SLDataLocator_AndroidSimpleBufferQueue { SLuint32 locatorType; SLuint32 numBuffers; };
struct SLDataLocator_OutputMix { SLuint32 locatorType; SLObjectItf outputMix; };
struct SLDataFormat_PCM {
    SLuint32 formatType, numChannels, samplesPerSec, bitsPerSample,
             containerSize, channelMask, endianness;
};
struct SLDataSource_ { void* pLocator; void* pFormat; };
struct SLDataSink_ { void* pLocator; void* pConfiguration; };
typedef struct SLDataSource_ SLDataSource; typedef struct SLDataSink_ SLDataSink;
inline SLresult slCreateEngine(SLObjectItf*, SLuint32, const SLEngineOption*, SLuint32,
                               const SLInterfaceID*, const SLboolean*) { return SL_RESULT_SUCCESS; }
