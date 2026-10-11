#define GL_VERSION 0x1F02
#define GL_RENDERER 0x1F01
#define GL_VENDOR 0x1F00
#define GL_MAJOR_VERSION 0x821B
#define GL_MINOR_VERSION 0x821C
#define GL_MAX_TEXTURE_SIZE 0x0D33
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_RGBA8 0x8058
#define GL_BGRA 0x80E1
#define GL_UNSIGNED_BYTE 0x1401
#define GL_DEPTH_TEST 0x0B71
#define GL_BLEND 0x0BE2
#define GL_SCISSOR_TEST 0x0C11
#define GL_CULL_FACE 0x0B44
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_TEXTURE0 0x84C0
#define GL_ARRAY_BUFFER 0x8892
#define GL_STATIC_DRAW 0x88E4
#define GL_DYNAMIC_DRAW 0x88E8
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_MAX_VERTEX_ATTRIBS 0x8869
#define GL_SAMPLER_BINDING 0x8919
#define GL_ARRAY_BUFFER_BINDING 0x8894
#define GL_VERTEX_ARRAY_BINDING 0x85B5
#define GL_INFO_LOG_LENGTH 0x8B84
#define GL_LINK_STATUS 0x8B82
#define GL_COMPILE_STATUS 0x8B81
#define GL_ACTIVE_UNIFORMS 0x8B86
#define GL_ACTIVE_ATTRIBUTES 0x8B89
#define GL_CURRENT_PROGRAM 0x8B8D
#define GL_NUM_EXTENSIONS 0x821D
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C
#define GL_EXTENSIONS 0x1F03
#define GL_DEPTH_FUNC 0x0B74
#define GL_FRONT_FACE 0x0B46
#define GL_CW 0x0900
#define GL_CCW 0x0901
#define GL_TEXTURE_BINDING_2D 0x8069
#define GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS 0x8B4D
#define GL_TEXTURE_SWIZZLE_RGBA 0x8E46
#define GL_RESET_NOTIFICATION_STRATEGY 0x8256
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#define GL_UNPACK_SKIP_ROWS 0x0CF3
#define GL_UNPACK_SKIP_PIXELS 0x0CF4
#define GL_SRGB8_ALPHA8 0x8C43
#define GL_LINEAR_MIPMAP_LINEAR 0x2703
#define GL_REPEAT 0x2901
#define GL_DEPTH_BUFFER_BIT 0x00000100
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_STENCIL_BUFFER_BIT 0x00000400
#define GL_MAX_SAMPLES 0x8D57
#define GL_FRAMEBUFFER_SRGB 0x8DB9
#define GL_POLYGON_MODE 0x0B40
#define GL_FILL 0x1B02
#define GL_LINE 0x1B01
#define GL_TRIANGLES 0x0004
#define GL_TRIANGLE_STRIP 0x0005
#define GL_FLOAT 0x1406
#define GL_INT 0x1404
#define GL_UNSIGNED_INT 0x1405
#define GL_UNSIGNED_SHORT 0x1403
#define GL_TRUE 1
#define GL_FALSE 0
#define GL_NO_ERROR 0
void glGetFloatv(GLenum, GLfloat*);
void glGetBooleanv(GLenum, GLboolean*);
void glFramebufferTexture2D(GLenum, GLenum, GLenum, GLuint, GLint);
void glBlitFramebuffer(GLint,GLint,GLint,GLint,GLint,GLint,GLint,GLint,GLbitfield,GLenum);
void glBindSampler(GLuint, GLuint);
void glGenSamplers(GLsizei, GLuint*);
void glDeleteSamplers(GLsizei, const GLuint*);
void glSamplerParameteri(GLuint, GLenum, GLint);
void glTexParameterf(GLenum, GLenum, GLfloat);
void glDetachShader(GLuint, GLuint);
GLboolean glIsProgram(GLuint);
GLboolean glIsTexture(GLuint);
GLboolean glIsBuffer(GLuint);
GLboolean glIsFramebuffer(GLuint);
GLboolean glIsVertexArray(GLuint);
void glUniform1f(GLint, GLfloat);
void glFrontFace(GLenum);
const GLubyte* glGetStringi(GLenum, GLuint);
void glGetActiveUniform(GLuint, GLuint, GLsizei, GLsizei*, GLint*, GLenum*, GLchar*);
void glGetActiveAttrib(GLuint, GLuint, GLsizei, GLsizei*, GLint*, GLenum*, GLchar*);
void glVertexAttribDivisor(GLuint, GLuint);
void glReadBuffer(GLenum);

#define GL_PIXEL_UNPACK_BUFFER_BINDING 0x88EF
#define GL_PIXEL_PACK_BUFFER_BINDING 0x88ED
#define GL_UNIFORM_BUFFER_BINDING 0x8A28
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
