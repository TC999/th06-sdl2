#version 450
// Color-only fragment shader (no texture). Outputs vertex diffuse straight.
layout(location = 0) in vec4 v_color;

layout(location = 0) out vec4 out_color;

void main() {
    out_color = v_color;
}
