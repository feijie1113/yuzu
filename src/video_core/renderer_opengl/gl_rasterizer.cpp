// Copyright 2015 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <glad/glad.h>
#include "common/alignment.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "common/math_util.h"
#include "common/microprofile.h"
#include "common/scope_exit.h"
#include "common/vector_math.h"
#include "core/settings.h"
#include "video_core/renderer_opengl/gl_rasterizer.h"
#include "video_core/renderer_opengl/gl_shader_gen.h"
#include "video_core/renderer_opengl/renderer_opengl.h"

using PixelFormat = SurfaceParams::PixelFormat;
using SurfaceType = SurfaceParams::SurfaceType;

MICROPROFILE_DEFINE(OpenGL_VAO, "OpenGL", "Vertex Array Setup", MP_RGB(128, 128, 192));
MICROPROFILE_DEFINE(OpenGL_VS, "OpenGL", "Vertex Shader Setup", MP_RGB(128, 128, 192));
MICROPROFILE_DEFINE(OpenGL_FS, "OpenGL", "Fragment Shader Setup", MP_RGB(128, 128, 192));
MICROPROFILE_DEFINE(OpenGL_Drawing, "OpenGL", "Drawing", MP_RGB(128, 128, 192));
MICROPROFILE_DEFINE(OpenGL_Blits, "OpenGL", "Blits", MP_RGB(100, 100, 255));
MICROPROFILE_DEFINE(OpenGL_CacheManagement, "OpenGL", "Cache Mgmt", MP_RGB(100, 255, 100));

enum class UniformBindings : GLuint { Common, VS, FS };

static void SetShaderUniformBlockBinding(GLuint shader, const char* name, UniformBindings binding,
                                         size_t expected_size) {
    GLuint ub_index = glGetUniformBlockIndex(shader, name);
    if (ub_index != GL_INVALID_INDEX) {
        GLint ub_size = 0;
        glGetActiveUniformBlockiv(shader, ub_index, GL_UNIFORM_BLOCK_DATA_SIZE, &ub_size);
        ASSERT_MSG(ub_size == expected_size,
                   "Uniform block size did not match! Got %d, expected %zu",
                   static_cast<int>(ub_size), expected_size);
        glUniformBlockBinding(shader, ub_index, static_cast<GLuint>(binding));
    }
}

static void SetShaderUniformBlockBindings(GLuint shader) {
    SetShaderUniformBlockBinding(shader, "shader_data", UniformBindings::Common,
                                 sizeof(RasterizerOpenGL::UniformData));
    SetShaderUniformBlockBinding(shader, "vs_config", UniformBindings::VS,
                                 sizeof(RasterizerOpenGL::VSUniformData));
    SetShaderUniformBlockBinding(shader, "fs_config", UniformBindings::FS,
                                 sizeof(RasterizerOpenGL::FSUniformData));
}

RasterizerOpenGL::RasterizerOpenGL() {
    has_ARB_buffer_storage = false;
    has_ARB_direct_state_access = false;
    has_ARB_separate_shader_objects = false;
    has_ARB_vertex_attrib_binding = false;

    GLint ext_num;
    glGetIntegerv(GL_NUM_EXTENSIONS, &ext_num);
    for (GLint i = 0; i < ext_num; i++) {
        std::string extension{reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, i))};

        if (extension == "GL_ARB_buffer_storage") {
            has_ARB_buffer_storage = true;
        } else if (extension == "GL_ARB_direct_state_access") {
            has_ARB_direct_state_access = true;
        } else if (extension == "GL_ARB_separate_shader_objects") {
            has_ARB_separate_shader_objects = true;
        } else if (extension == "GL_ARB_vertex_attrib_binding") {
            has_ARB_vertex_attrib_binding = true;
        }
    }

    // Clipping plane 0 is always enabled for PICA fixed clip plane z <= 0
    state.clip_distance[0] = true;

    // Generate VBO, VAO and UBO
    vertex_buffer = OGLStreamBuffer::MakeBuffer(GLAD_GL_ARB_buffer_storage, GL_ARRAY_BUFFER);
    vertex_buffer->Create(VERTEX_BUFFER_SIZE, VERTEX_BUFFER_SIZE / 2);
    sw_vao.Create();
    uniform_buffer.Create();

    state.draw.vertex_array = sw_vao.handle;
    state.draw.vertex_buffer = vertex_buffer->GetHandle();
    state.draw.uniform_buffer = uniform_buffer.handle;
    state.Apply();

    glBufferData(GL_UNIFORM_BUFFER, sizeof(UniformData), nullptr, GL_STATIC_DRAW);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, uniform_buffer.handle);

    uniform_block_data.dirty = true;

    // Create render framebuffer
    framebuffer.Create();

    if (has_ARB_separate_shader_objects) {
        hw_vao.Create();
        hw_vao_enabled_attributes.fill(false);

        stream_buffer = OGLStreamBuffer::MakeBuffer(has_ARB_buffer_storage, GL_ARRAY_BUFFER);
        stream_buffer->Create(STREAM_BUFFER_SIZE, STREAM_BUFFER_SIZE / 2);
        state.draw.vertex_buffer = stream_buffer->GetHandle();

        pipeline.Create();
        vs_input_index_min = 0;
        vs_input_index_max = 0;
        state.draw.program_pipeline = pipeline.handle;
        state.draw.shader_program = 0;
        state.draw.vertex_array = hw_vao.handle;
        state.Apply();

        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, stream_buffer->GetHandle());

        vs_uniform_buffer.Create();
        glBindBuffer(GL_UNIFORM_BUFFER, vs_uniform_buffer.handle);
        glBufferData(GL_UNIFORM_BUFFER, sizeof(VSUniformData), nullptr, GL_STREAM_COPY);
        glBindBufferBase(GL_UNIFORM_BUFFER, 1, vs_uniform_buffer.handle);
    } else {
        UNIMPLEMENTED();
    }

    accelerate_draw = AccelDraw::Disabled;

    glEnable(GL_BLEND);

    // Sync fixed function OpenGL state
    SyncClipEnabled();
    SyncClipCoef();
    SyncCullMode();
    SyncBlendEnabled();
    SyncBlendFuncs();
    SyncBlendColor();
}

RasterizerOpenGL::~RasterizerOpenGL() {
    if (stream_buffer != nullptr) {
        state.draw.vertex_buffer = stream_buffer->GetHandle();
        state.Apply();
        stream_buffer->Release();
    }
}

static constexpr std::array<GLenum, 4> vs_attrib_types{
    GL_BYTE,          // VertexAttributeFormat::BYTE
    GL_UNSIGNED_BYTE, // VertexAttributeFormat::UBYTE
    GL_SHORT,         // VertexAttributeFormat::SHORT
    GL_FLOAT          // VertexAttributeFormat::FLOAT
};

void RasterizerOpenGL::AnalyzeVertexArray(bool is_indexed) {
    UNIMPLEMENTED();
}

void RasterizerOpenGL::SetupVertexArray(u8* array_ptr, GLintptr buffer_offset) {
    MICROPROFILE_SCOPE(OpenGL_VAO);
    UNIMPLEMENTED();
}

void RasterizerOpenGL::SetupVertexShader(VSUniformData* ub_ptr, GLintptr buffer_offset) {
    MICROPROFILE_SCOPE(OpenGL_VS);
    UNIMPLEMENTED();
}

void RasterizerOpenGL::SetupFragmentShader(FSUniformData* ub_ptr, GLintptr buffer_offset) {
    MICROPROFILE_SCOPE(OpenGL_FS);
    UNIMPLEMENTED();
}

bool RasterizerOpenGL::AccelerateDrawBatch(bool is_indexed) {
    if (!has_ARB_separate_shader_objects) {
        UNIMPLEMENTED();
        return false;
    }

    accelerate_draw = is_indexed ? AccelDraw::Indexed : AccelDraw::Arrays;
    DrawTriangles();

    return true;
}

void RasterizerOpenGL::DrawTriangles() {
    MICROPROFILE_SCOPE(OpenGL_Drawing);
    UNIMPLEMENTED();
}

void RasterizerOpenGL::NotifyMaxwellRegisterChanged(u32 id) {}

void RasterizerOpenGL::FlushAll() {
    MICROPROFILE_SCOPE(OpenGL_CacheManagement);
    res_cache.FlushAll();
}

void RasterizerOpenGL::FlushRegion(PAddr addr, u32 size) {
    MICROPROFILE_SCOPE(OpenGL_CacheManagement);
    res_cache.FlushRegion(addr, size);
}

void RasterizerOpenGL::InvalidateRegion(PAddr addr, u32 size) {
    MICROPROFILE_SCOPE(OpenGL_CacheManagement);
    res_cache.InvalidateRegion(addr, size, nullptr);
}

void RasterizerOpenGL::FlushAndInvalidateRegion(PAddr addr, u32 size) {
    MICROPROFILE_SCOPE(OpenGL_CacheManagement);
    res_cache.FlushRegion(addr, size);
    res_cache.InvalidateRegion(addr, size, nullptr);
}

bool RasterizerOpenGL::AccelerateDisplayTransfer(const void* config) {
    MICROPROFILE_SCOPE(OpenGL_Blits);
    UNIMPLEMENTED();
    return true;
}

bool RasterizerOpenGL::AccelerateTextureCopy(const void* config) {
    UNIMPLEMENTED();
    return true;
}

bool RasterizerOpenGL::AccelerateFill(const void* config) {
    UNIMPLEMENTED();
    return true;
}

bool RasterizerOpenGL::AccelerateDisplay(const void* config, PAddr framebuffer_addr,
                                         u32 pixel_stride, ScreenInfo& screen_info) {
    UNIMPLEMENTED();
    return true;
}

void RasterizerOpenGL::SetShader() {
    UNIMPLEMENTED();
}

void RasterizerOpenGL::SyncClipEnabled() {
    UNIMPLEMENTED();
}

void RasterizerOpenGL::SyncClipCoef() {
    UNIMPLEMENTED();
}

void RasterizerOpenGL::SyncCullMode() {
    UNIMPLEMENTED();
}

void RasterizerOpenGL::SyncDepthScale() {
    UNIMPLEMENTED();
}

void RasterizerOpenGL::SyncDepthOffset() {
    UNIMPLEMENTED();
}

void RasterizerOpenGL::SyncBlendEnabled() {
    UNIMPLEMENTED();
}

void RasterizerOpenGL::SyncBlendFuncs() {
    UNIMPLEMENTED();
}

void RasterizerOpenGL::SyncBlendColor() {
    UNIMPLEMENTED();
}
