#pragma once

// OpenGL 3.3 entry points the viewport loads through GLFW, shared by the scene
// renderer and the overlay renderer.

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// clang-format off
// windows.h must precede the headers below; they depend on its declarations.
#include <windows.h>
// clang-format on
#endif

#include <GLFW/glfw3.h>

#include <cstddef>

namespace rwsman {

using GlSizePtr = std::ptrdiff_t;
using CreateShaderProc = GLuint(APIENTRY*)(GLenum);
using ShaderSourceProc = void(APIENTRY*)(GLuint, GLsizei, const char* const*, const GLint*);
using CompileShaderProc = void(APIENTRY*)(GLuint);
using GetShaderIvProc = void(APIENTRY*)(GLuint, GLenum, GLint*);
using GetShaderInfoLogProc = void(APIENTRY*)(GLuint, GLsizei, GLsizei*, char*);
using DeleteShaderProc = void(APIENTRY*)(GLuint);
using CreateProgramProc = GLuint(APIENTRY*)();
using AttachShaderProc = void(APIENTRY*)(GLuint, GLuint);
using LinkProgramProc = void(APIENTRY*)(GLuint);
using GetProgramIvProc = void(APIENTRY*)(GLuint, GLenum, GLint*);
using GetProgramInfoLogProc = void(APIENTRY*)(GLuint, GLsizei, GLsizei*, char*);
using DeleteProgramProc = void(APIENTRY*)(GLuint);
using UseProgramProc = void(APIENTRY*)(GLuint);
using GenVertexArraysProc = void(APIENTRY*)(GLsizei, GLuint*);
using BindVertexArrayProc = void(APIENTRY*)(GLuint);
using DeleteVertexArraysProc = void(APIENTRY*)(GLsizei, const GLuint*);
using GenBuffersProc = void(APIENTRY*)(GLsizei, GLuint*);
using BindBufferProc = void(APIENTRY*)(GLenum, GLuint);
using BufferDataProc = void(APIENTRY*)(GLenum, GlSizePtr, const void*, GLenum);
using BufferSubDataProc = void(APIENTRY*)(GLenum, GlSizePtr, GlSizePtr, const void*);
using DeleteBuffersProc = void(APIENTRY*)(GLsizei, const GLuint*);
using EnableVertexAttribArrayProc = void(APIENTRY*)(GLuint);
using VertexAttribPointerProc = void(APIENTRY*)(GLuint, GLint, GLenum, GLboolean, GLsizei,
                                                const void*);
using GetUniformLocationProc = GLint(APIENTRY*)(GLuint, const char*);
using Uniform1iProc = void(APIENTRY*)(GLint, GLint);
using Uniform1fProc = void(APIENTRY*)(GLint, GLfloat);
using Uniform2fProc = void(APIENTRY*)(GLint, GLfloat, GLfloat);
using Uniform3fProc = void(APIENTRY*)(GLint, GLfloat, GLfloat, GLfloat);
using Uniform4fProc = void(APIENTRY*)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
using ActiveTextureProc = void(APIENTRY*)(GLenum);
using GenerateMipmapProc = void(APIENTRY*)(GLenum);
using GenQueriesProc = void(APIENTRY*)(GLsizei, GLuint*);
using DeleteQueriesProc = void(APIENTRY*)(GLsizei, const GLuint*);
using BeginQueryProc = void(APIENTRY*)(GLenum, GLuint);
using EndQueryProc = void(APIENTRY*)(GLenum);
using GetQueryObjectIvProc = void(APIENTRY*)(GLuint, GLenum, GLint*);
using GetQueryObjectUi64vProc = void(APIENTRY*)(GLuint, GLenum, unsigned long long*);

inline constexpr GLenum gl_vertex_shader = 0x8B31;
inline constexpr GLenum gl_fragment_shader = 0x8B30;
inline constexpr GLenum gl_compile_status = 0x8B81;
inline constexpr GLenum gl_link_status = 0x8B82;
inline constexpr GLenum gl_array_buffer = 0x8892;
inline constexpr GLenum gl_static_draw = 0x88E4;
inline constexpr GLenum gl_texture0 = 0x84C0;
inline constexpr GLenum gl_texture1 = gl_texture0 + 1;
inline constexpr GLenum gl_texture_max_anisotropy = 0x84FE;
inline constexpr GLenum gl_max_texture_max_anisotropy = 0x84FF;
inline constexpr GLenum gl_stream_draw = 0x88E0;
inline constexpr GLenum gl_time_elapsed = 0x88BF;
inline constexpr GLenum gl_query_result = 0x8866;
inline constexpr GLenum gl_query_result_available = 0x8867;

struct GlApi {
    CreateShaderProc create_shader{};
    ShaderSourceProc shader_source{};
    CompileShaderProc compile_shader{};
    GetShaderIvProc get_shader_iv{};
    GetShaderInfoLogProc get_shader_log{};
    DeleteShaderProc delete_shader{};
    CreateProgramProc create_program{};
    AttachShaderProc attach_shader{};
    LinkProgramProc link_program{};
    GetProgramIvProc get_program_iv{};
    GetProgramInfoLogProc get_program_log{};
    DeleteProgramProc delete_program{};
    UseProgramProc use_program{};
    GenVertexArraysProc gen_vertex_arrays{};
    BindVertexArrayProc bind_vertex_array{};
    DeleteVertexArraysProc delete_vertex_arrays{};
    GenBuffersProc gen_buffers{};
    BindBufferProc bind_buffer{};
    BufferDataProc buffer_data{};
    BufferSubDataProc buffer_sub_data{};
    DeleteBuffersProc delete_buffers{};
    EnableVertexAttribArrayProc enable_vertex_attrib_array{};
    VertexAttribPointerProc vertex_attrib_pointer{};
    GetUniformLocationProc get_uniform_location{};
    Uniform1iProc uniform_1i{};
    Uniform1fProc uniform_1f{};
    Uniform2fProc uniform_2f{};
    Uniform3fProc uniform_3f{};
    Uniform4fProc uniform_4f{};
    ActiveTextureProc active_texture{};
    GenerateMipmapProc generate_mipmap{};
    // Optional: GPU timer queries for the frame statistics.
    GenQueriesProc gen_queries{};
    DeleteQueriesProc delete_queries{};
    BeginQueryProc begin_query{};
    EndQueryProc end_query{};
    GetQueryObjectIvProc get_query_object_iv{};
    GetQueryObjectUi64vProc get_query_object_ui64v{};

    [[nodiscard]] bool has_timer_queries() const noexcept {
        return gen_queries && delete_queries && begin_query && end_query && get_query_object_iv &&
               get_query_object_ui64v;
    }

    bool load() {
#define LOAD_GL(member, name)                                                                      \
    member = reinterpret_cast<decltype(member)>(glfwGetProcAddress(name));                         \
    if (!member) return false
        LOAD_GL(create_shader, "glCreateShader");
        LOAD_GL(shader_source, "glShaderSource");
        LOAD_GL(compile_shader, "glCompileShader");
        LOAD_GL(get_shader_iv, "glGetShaderiv");
        LOAD_GL(get_shader_log, "glGetShaderInfoLog");
        LOAD_GL(delete_shader, "glDeleteShader");
        LOAD_GL(create_program, "glCreateProgram");
        LOAD_GL(attach_shader, "glAttachShader");
        LOAD_GL(link_program, "glLinkProgram");
        LOAD_GL(get_program_iv, "glGetProgramiv");
        LOAD_GL(get_program_log, "glGetProgramInfoLog");
        LOAD_GL(delete_program, "glDeleteProgram");
        LOAD_GL(use_program, "glUseProgram");
        LOAD_GL(gen_vertex_arrays, "glGenVertexArrays");
        LOAD_GL(bind_vertex_array, "glBindVertexArray");
        LOAD_GL(delete_vertex_arrays, "glDeleteVertexArrays");
        LOAD_GL(gen_buffers, "glGenBuffers");
        LOAD_GL(bind_buffer, "glBindBuffer");
        LOAD_GL(buffer_data, "glBufferData");
        LOAD_GL(buffer_sub_data, "glBufferSubData");
        LOAD_GL(delete_buffers, "glDeleteBuffers");
        LOAD_GL(enable_vertex_attrib_array, "glEnableVertexAttribArray");
        LOAD_GL(vertex_attrib_pointer, "glVertexAttribPointer");
        LOAD_GL(get_uniform_location, "glGetUniformLocation");
        LOAD_GL(uniform_1i, "glUniform1i");
        LOAD_GL(uniform_1f, "glUniform1f");
        LOAD_GL(uniform_2f, "glUniform2f");
        LOAD_GL(uniform_3f, "glUniform3f");
        LOAD_GL(uniform_4f, "glUniform4f");
        LOAD_GL(active_texture, "glActiveTexture");
        LOAD_GL(generate_mipmap, "glGenerateMipmap");
#undef LOAD_GL
#define LOAD_OPTIONAL_GL(member, name) member = reinterpret_cast<decltype(member)>(glfwGetProcAddress(name))
        LOAD_OPTIONAL_GL(gen_queries, "glGenQueries");
        LOAD_OPTIONAL_GL(delete_queries, "glDeleteQueries");
        LOAD_OPTIONAL_GL(begin_query, "glBeginQuery");
        LOAD_OPTIONAL_GL(end_query, "glEndQuery");
        LOAD_OPTIONAL_GL(get_query_object_iv, "glGetQueryObjectiv");
        LOAD_OPTIONAL_GL(get_query_object_ui64v, "glGetQueryObjectui64v");
#undef LOAD_OPTIONAL_GL
        return true;
    }
};

inline GlApi& gl_api() {
    static GlApi api;
    static const bool loaded = api.load();
    (void)loaded;
    return api;
}

} // namespace rwsman
