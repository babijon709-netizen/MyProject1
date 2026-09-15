// Сгенерировано tools/video/gen_avatar.py — не править руками.
#ifndef MEDIA_AVATAR_VIDEO_H
#define MEDIA_AVATAR_VIDEO_H

// Кадры аватарки: 253 JPEG 176x176, 12 кадр/с, 590380 Б на всё видео (ролик НЕ вшит целиком, см. tools/video/gen_avatar.py).
static const int vid_avatar_w = 176;
static const int vid_avatar_h = 176;
static const int vid_avatar_fps = 12;
static const int vid_avatar_count = 253;
static const unsigned int vid_avatar_bytes = 590380;
extern const unsigned int vid_avatar_frames;
extern const unsigned int vid_avatar_off[254];
extern const unsigned char vid_avatar_data[590380];

#endif  // MEDIA_AVATAR_VIDEO_H
