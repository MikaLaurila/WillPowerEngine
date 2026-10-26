#include "renderer/shaders.h"

static void wpe__vertex_color_vs(const void* uniforms, const void* vertex, WPE_Vec4* out_position,
                                 float* out_varyings) {
    const WPE_VertexColorUniforms* u = uniforms;
    const WPE_VertexColored* v = vertex;
    *out_position = wpe_mat4_mul_vec4(&u->mvp, wpe_vec4(v->position.x, v->position.y, v->position.z, 1.0f));
    out_varyings[0] = v->color[0] * u->tint.x;
    out_varyings[1] = v->color[1] * u->tint.y;
    out_varyings[2] = v->color[2] * u->tint.z;
    out_varyings[3] = v->color[3] * u->tint.w;
}

static bool wpe__vertex_color_fs(const void* uniforms, const WPE_FragmentInput* input, WPE_Vec4* out_color) {
    (void)uniforms;
    *out_color = wpe_vec4(input->varyings[0], input->varyings[1], input->varyings[2], input->varyings[3]);
    return true;
}

static const WPE_ShaderProgram wpe__vertex_color_program = {
    .vertex = wpe__vertex_color_vs,
    .fragment = wpe__vertex_color_fs,
    .varying_count = 4,
};

const WPE_ShaderProgram* wpe_shader_vertex_color(void) {
    return &wpe__vertex_color_program;
}
