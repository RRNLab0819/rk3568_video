/* GPU shaders - NV12 YUV→RGB, OpenGL ES 2.0 */
#ifndef SHADER_YUV_H
#define SHADER_YUV_H

static const char vert_src[] =
    "#version 100\n"
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_tex;\n"
    "varying vec2 v_tex;\n"
    "void main() {\n"
    "  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "  v_tex = a_tex;\n"
    "}\n";

/* YUV→RGB via BT.601 (copied from AVM texture_y_uv.frag) */
static const char frag_src[] =
    "#version 100\n"
    "precision mediump float;\n"
    "uniform sampler2D u_texY;\n"
    "uniform sampler2D u_texUV;\n"
    "varying vec2 v_tex;\n"
    "const mat3 yuv2rgb = mat3("
    "  1.0, 1.0, 1.0,"
    "  0.0, -0.39465, 2.03211,"
    "  1.13983, -0.58060, 0.0);\n"
    "void main() {\n"
    "  vec3 yuv;\n"
    "  yuv.x = texture2D(u_texY, v_tex).r;\n"
    "  yuv.y = texture2D(u_texUV, v_tex).g - 0.5;\n"
    "  yuv.z = texture2D(u_texUV, v_tex).a - 0.5;\n"
    "  yuv.x -= 0.0625;\n"
    "  gl_FragColor = vec4(yuv2rgb * yuv, 1.0);\n"
    "}\n";

#endif
