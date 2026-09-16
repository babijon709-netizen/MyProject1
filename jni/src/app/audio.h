#pragma once
// Звуки меню: щелчок по строке и подтверждение действия.
//
// Было: блок внутри jni/src/main.cpp под #ifdef AUDIO_AVAILABLE, вместе с
// дублирующими пустышками в #else. Стало: отдельный модуль — сам звук в
// app/audio.cpp, а сюда вынесено только то, что зовут другие модули.
//
// Если media/audio.h нет (хостовые проверки, сборка без вшитых сэмплов),
// AUDIO_AVAILABLE не определяется и вызовы ниже просто ничего не делают: код
// меню об этом не думает.
enum SoundId { SND_CLICK = 0, SND_SUCCESS, SND_COUNT };

void AudioInit();
void PlaySound(SoundId id);
void AudioFree();
