#pragma once
#include <cstddef>
#include <cstdint>
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
/* --- расширение заглушки: символы, которые пользуют Blur.cpp / VidAvatar.cpp /
       бэкенды ImGui (только объявления, для -fsyntax-only) --- */
typedef char GLchar; typedef float GLclampf; typedef intptr_t GLintptr; typedef size_t GLsizeiptr;
typedef unsigned short GLushort; typedef short GLshort; typedef unsigned char GLboolean;
#define GL_RGB 0x1907
#define GL_FALSE 0
#define GL_TRUE 1
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_INFO_LOG_LENGTH 0x8B84
#define GL_TEXTURE0 0x84C0
#define GL_FRAMEBUFFER 0x8D40
#define GL_RENDERBUFFER 0x8D41
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_TRIANGLE_STRIP 0x0005
#define GL_TRIANGLES 0x0004
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_STATIC_DRAW 0x88E4
#define GL_DYNAMIC_DRAW 0x88E8
#define GL_FLOAT 0x1406
#define GL_UNSIGNED_SHORT 0x1403
#define GL_DEPTH_TEST 0x0B71
#define GL_SCISSOR_TEST 0x0C11
#define GL_CULL_FACE 0x0B44
#define GL_BACK 0x0405
#define GL_FRONT 0x0404
#define GL_SRC_ALPHA 0x0302
#define GL_ONE 1
#define GL_ZERO 0
#define GL_NEAREST 0x2600
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_TEXTURE_WRAP_R 0x8072
#define GL_TEXTURE_3D 0x806F
inline GLuint glCreateShader(GLenum t){(void)t;return 0;}
inline void glShaderSource(GLuint,GLsizei,const GLchar* const*,const GLint*){}
inline void glCompileShader(GLuint){}
inline void glGetShaderiv(GLuint,GLenum,GLint*){}
inline void glGetShaderInfoLog(GLuint,GLsizei,GLsizei*,GLchar*){}
inline GLuint glCreateProgram(){return 0;}
inline void glAttachShader(GLuint,GLuint){}
inline void glLinkProgram(GLuint){}
inline void glGetProgramiv(GLuint,GLenum,GLint*){}
inline void glGetProgramInfoLog(GLuint,GLsizei,GLsizei*,GLchar*){}
inline void glDeleteShader(GLuint){}
inline void glDeleteProgram(GLuint){}
inline void glDeleteTextures(GLsizei,const GLuint*){}
inline void glUseProgram(GLuint){}
inline GLint glGetUniformLocation(GLuint,const GLchar*){return -1;}
inline void glUniform1i(GLint,GLint){}
inline void glUniform1f(GLint,GLfloat){}
inline void glUniform2f(GLint,GLfloat,GLfloat){}
inline void glUniform3f(GLint,GLfloat,GLfloat,GLfloat){}
inline void glUniform4f(GLint,GLfloat,GLfloat,GLfloat,GLfloat){}
inline void glUniformMatrix4fv(GLint,GLsizei,GLboolean,const GLfloat*){}
inline void glVertexAttribPointer(GLuint,GLint,GLenum,GLboolean,GLsizei,const void*){}
inline void glEnableVertexAttribArray(GLuint){}
inline void glDisableVertexAttribArray(GLuint){}
inline void glActiveTexture(GLenum){}
inline void glGenFramebuffers(GLsizei,GLuint*){}
inline void glBindFramebuffer(GLenum,GLuint){}
inline void glFramebufferTexture2D(GLenum,GLenum,GLenum,GLuint,GLint){}
inline void glDeleteFramebuffers(GLsizei,const GLuint*){}
inline GLenum glCheckFramebufferStatus(GLenum){return GL_FRAMEBUFFER_COMPLETE;}
inline void glGenVertexArrays(GLsizei,GLuint*){}
inline void glBindVertexArray(GLuint){}
inline void glDeleteVertexArrays(GLsizei,const GLuint*){}
inline void glGenBuffers(GLsizei,GLuint*){}
inline void glBindBuffer(GLenum,GLuint){}
inline void glBufferData(GLenum,GLsizeiptr,const void*,GLenum){}
inline void glDeleteBuffers(GLsizei,const GLuint*){}
inline void glDrawArrays(GLenum,GLint,GLsizei){}
inline void glDrawElements(GLenum,GLsizei,GLenum,const void*){}
inline void glViewport(GLint,GLint,GLsizei,GLsizei){}
inline void glScissor(GLint,GLint,GLsizei,GLsizei){}
inline void glDisable(GLenum){}
inline void glPixelStorei(GLenum,GLint){}
inline void glClearDepthf(GLfloat){}
inline void glDepthFunc(GLenum){}
inline void glDepthMask(GLboolean){}
inline void glCullFace(GLenum){}
inline void glBlendEquation(GLenum){}
inline void glBlendFuncSeparate(GLenum,GLenum,GLenum,GLenum){}
inline void glColorMask(GLboolean,GLboolean,GLboolean,GLboolean){}
inline void glLineWidth(GLfloat){}
inline void glTexSubImage2D(GLenum,GLint,GLint,GLint,GLsizei,GLsizei,GLenum,GLenum,const void*){}
inline const GLubyte* glGetString(GLenum){return (const GLubyte*)"";}
inline GLenum glGetError(){return 0;}
inline void glGetIntegerv(GLenum,GLint*){}
inline void glGetFloatv(GLenum,GLfloat*){}
inline void glGenerateMipmap(GLenum){}
inline void glTexStorage2D(GLenum,GLsizei,GLenum,GLsizei,GLsizei){}
inline void* glMapBufferRange(GLenum,GLintptr,GLsizeiptr,GLenum){return nullptr;}
inline GLboolean glUnmapBuffer(GLenum){return GL_TRUE;}
inline void glBindSampler(GLuint,GLuint){}
inline void glGenSamplers(GLsizei,GLuint*){}
inline void glSamplerParameteri(GLuint,GLenum,GLint){}
inline void glBindAttribLocation(GLuint,GLuint,const GLchar*){}
/* --- ещё символы для Blur.cpp (blit-фреймбуферы, состояния) --- */
#define GL_RGBA8 0x8058
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#define GL_VIEWPORT 0x0BA2
#define GL_STENCIL_TEST 0x0B90
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
inline void glGetBooleanv(GLenum,GLboolean*){}
inline void glBlitFramebuffer(GLint,GLint,GLint,GLint,GLint,GLint,GLint,GLint,GLenum,GLenum){}
inline void glReadPixels(GLint,GLint,GLsizei,GLsizei,GLenum,GLenum,void*){}
inline void glCopyTexSubImage2D(GLenum,GLint,GLint,GLint,GLint,GLint,GLsizei,GLsizei){}
inline void glFinish(){}
inline void glFlush(){}
