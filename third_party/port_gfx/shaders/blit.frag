#version 450
// The frame onto the viewport, filtered by the sampler (nearest or linear).
layout(set = 0, binding = 0) uniform sampler2D source;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
void main() { color = vec4(texture(source, uv).rgb, 1.0); }
