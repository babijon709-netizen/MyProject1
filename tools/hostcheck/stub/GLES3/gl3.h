// Заглушка GLES3 для hostcheck (на устройстве — настоящий NDK-заголовок).
// Только то, чем реально пользуются draw/Touch/ImGui-backend/first-party модули.
#pragma once
#include <cstdint>
#include <cstddef>
typedef unsigned int   GLenum;
typedef unsigned int   GLuint;
typedef int            GLint;
typedef int            GLsizei;
typedef unsigned char  GLboolean;
typedef unsigned char  GLubyte;
typedef float          GLfloat;
typedef void           GLvoid;
typedef unsigned int   GLbitfield;
typedef int64_t        GLintptr;
typedef int64_t        GLsizeiptr;
typedef char           GLchar;

#define GL_FALSE 0
#define GL_TRUE 1
#define GL_ZERO 0
#define GL_ONE 1
#define GL_NEVER 0x0200
#define GL_FLOAT 0x1406
#define GL_RGBA 0x1908
#define GL_RGB 0x1907
#define GL_RGBA8 0x8058
#define GL_UNSIGNED_BYTE 0x1401
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_LINEAR 0x2601
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_BLEND 0x0BE2
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_DEPTH_BUFFER_BIT 0x0100
#define GL_STENCIL_BUFFER_BIT 0x0400
#define GL_DEPTH_TEST 0x0B71
#define GL_SCISSOR_TEST 0x0C11
#define GL_STENCIL_TEST 0x0B90
#define GL_VIEWPORT 0x0BA2
#define GL_ARRAY_BUFFER 0x8892
#define GL_STATIC_DRAW 0x88E4
#define GL_DYNAMIC_DRAW 0x88E8
#define GL_TRIANGLE_STRIP 0x0005
#define GL_TRIANGLES 0x0004
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_FRAMEBUFFER 0x8D40
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_RED 0x1903
#define GL_GREEN 0x1904
#define GL_BLUE 0x1905
#define GL_ALPHA 0x1906

static inline void glGenTextures(GLsizei n, GLuint* t){ (void)n; if (t) *t = 1; }
static inline void glDeleteTextures(GLsizei n, const GLuint* t){ (void)n; (void)t; }
static inline void glBindTexture(GLenum a, GLuint b){ (void)a; (void)b; }
static inline void glTexParameteri(GLenum a, GLenum b, GLint c){ (void)a; (void)b; (void)c; }
static inline void glTexImage2D(GLenum a, GLint b, GLint c, GLsizei d, GLsizei e, GLint f, GLenum g, GLenum h, const GLvoid* i){ (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;(void)i; }
static inline void glTexSubImage2D(GLenum a, GLint b, GLint c, GLint d, GLsizei e, GLsizei f, GLenum g, GLenum h, const GLvoid* i){ (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;(void)i; }
static inline void glPixelStorei(GLenum a, GLint b){ (void)a; (void)b; }
static inline void glActiveTexture(GLenum a){ (void)a; }
static inline void glViewport(GLint a, GLint b, GLsizei c, GLsizei d){ (void)a;(void)b;(void)c;(void)d; }
static inline void glClear(GLbitfield a){ (void)a; }
static inline void glEnable(GLenum a){ (void)a; }
static inline void glDisable(GLenum a){ (void)a; }
static inline void glBlendFunc(GLenum a, GLenum b){ (void)a; (void)b; }
static inline void glGenBuffers(GLsizei n, GLuint* b){ (void)n; if (b) *b = 1; }
static inline void glDeleteBuffers(GLsizei n, const GLuint* b){ (void)n; (void)b; }
static inline void glBindBuffer(GLenum a, GLuint b){ (void)a; (void)b; }
static inline void glBufferData(GLenum a, GLsizeiptr b, const GLvoid* c, GLenum d){ (void)a;(void)b;(void)c;(void)d; }
static inline void glGenFramebuffers(GLsizei n, GLuint* f){ (void)n; if (f) *f = 1; }
static inline void glDeleteFramebuffers(GLsizei n, const GLuint* f){ (void)n; (void)f; }
static inline void glBindFramebuffer(GLenum a, GLuint b){ (void)a; (void)b; }
static inline void glFramebufferTexture2D(GLenum a, GLenum b, GLenum c, GLuint d, GLint e){ (void)a;(void)b;(void)c;(void)d;(void)e; }
static inline void glBlitFramebuffer(GLint a, GLint b, GLint c, GLint d, GLint e, GLint f, GLint g, GLint h, GLbitfield i, GLenum j){ (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;(void)i;(void)j; }
static inline void glGenVertexArrays(GLsizei n, GLuint* v){ (void)n; if (v) *v = 1; }
static inline void glDeleteVertexArrays(GLsizei n, const GLuint* v){ (void)n; (void)v; }
static inline void glBindVertexArray(GLuint a){ (void)a; }
static inline void glEnableVertexAttribArray(GLuint a){ (void)a; }
static inline void glVertexAttribPointer(GLuint a, GLint b, GLenum c, GLboolean d, GLsizei e, const GLvoid* f){ (void)a;(void)b;(void)c;(void)d;(void)e;(void)f; }
static inline void glDrawArrays(GLenum a, GLint b, GLsizei c){ (void)a; (void)b; (void)c; }
static inline GLuint glCreateShader(GLenum a){ (void)a; return 1; }
static inline void glDeleteShader(GLuint a){ (void)a; }
static inline void glShaderSource(GLuint a, GLsizei b, const GLchar* const* c, const GLint* d){ (void)a;(void)b;(void)c;(void)d; }
static inline void glCompileShader(GLuint a){ (void)a; }
static inline GLuint glCreateProgram(void){ return 1; }
static inline void glDeleteProgram(GLuint a){ (void)a; }
static inline void glAttachShader(GLuint a, GLuint b){ (void)a; (void)b; }
static inline void glLinkProgram(GLuint a){ (void)a; }
static inline GLint glGetUniformLocation(GLuint a, const GLchar* b){ (void)a; (void)b; return -1; }
static inline void glUseProgram(GLuint a){ (void)a; }
static inline void glUniform1i(GLint a, GLint b){ (void)a; (void)b; }
static inline void glUniform1f(GLint a, GLfloat b){ (void)a; (void)b; }
static inline void glGetIntegerv(GLenum a, GLint* b){ (void)a; if (b) *b = 0; }
static inline void glGetBooleanv(GLenum a, GLboolean* b){ (void)a; if (b) *b = 0; }
