#pragma once
#include <cstddef>
typedef unsigned int GLuint; typedef int GLint; typedef unsigned int GLenum;
typedef int GLsizei; typedef float GLfloat; typedef unsigned char GLubyte;
typedef void GLvoid; typedef unsigned int GLuint64; typedef long long GLint64;
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_LINEAR 0x2601
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_BLEND 0x0BE2
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_COLOR_BUFFER_BIT 0x4000
inline void glGenTextures(GLsizei n, GLuint* t){ (void)n; (void)t; }
inline void glBindTexture(GLenum target, GLuint t){ (void)target; (void)t; }
inline void glTexImage2D(GLenum a, GLint b, GLint c, GLsizei d, GLsizei e, GLint f, GLenum g, GLenum h, const GLvoid* i){ (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;(void)i; }
inline void glTexParameteri(GLenum a, GLenum b, GLint c){ (void)a;(void)b;(void)c; }
inline void glEnable(GLenum a){ (void)a; }
inline void glBlendFunc(GLenum a, GLenum b){ (void)a;(void)b; }
inline void glClear(GLenum a){ (void)a; }
inline void glFinish(){}
