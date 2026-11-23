#version 450
// AMD FidelityFX Super Resolution 1.0, EASU (edge-adaptive upscale): the frame enlarged to the output size.
#extension GL_GOOGLE_include_directive : require
#define A_GPU 1
#define A_GLSL 1
#include "ffx_a.h"
layout(set = 0, binding = 0) uniform sampler2D source;
layout(push_constant) uniform Parameters { vec2 sourceSize; vec2 outputSize; vec2 outputOffset; float sharpness; } p;
layout(location = 0) out vec4 color;
AF4 FsrEasuRF(AF2 q) { return textureGather(source, q, 0); }
AF4 FsrEasuGF(AF2 q) { return textureGather(source, q, 1); }
AF4 FsrEasuBF(AF2 q) { return textureGather(source, q, 2); }
#define FSR_EASU_F 1
#include "ffx_fsr1.h"
void main() {
    AU4 con0, con1, con2, con3;
    FsrEasuCon(con0, con1, con2, con3, p.sourceSize.x, p.sourceSize.y, p.sourceSize.x, p.sourceSize.y, p.outputSize.x, p.outputSize.y);
    AF3 c;
    FsrEasuF(c, AU2(gl_FragCoord.xy - p.outputOffset), con0, con1, con2, con3);
    color = vec4(c, 1.0);
}
