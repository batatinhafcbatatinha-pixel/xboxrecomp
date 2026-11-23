#version 450
// A triangle covering the target, from the vertex index; uv 0..1 across the drawn viewport.
layout(location = 0) out vec2 uv;
void main() {
    vec2 q = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    uv = q;
    gl_Position = vec4(q * 2.0 - 1.0, 0.0, 1.0);
}
