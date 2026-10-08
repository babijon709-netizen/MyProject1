#pragma once
// Звуки меню и попаданий (OpenSLES). Реализация в audio.cpp; без media/audio.h — заглушки.

enum SoundId { SND_CLICK = 0, SND_SUCCESS, SND_HIT, SND_COUNT };
void AudioInit();
void PlaySound(SoundId id);
void AudioFree();
