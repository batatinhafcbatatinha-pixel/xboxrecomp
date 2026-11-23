#version 450
// AMD FidelityFX Super Resolution 1.0, RCAS (robust contrast-adaptive sharpening), after EASU or DLSS.
#extension GL_GOOGLE_include_directive : require
#define A_GPU 1
#define A_GLSL 1
#include "ffx_a.h"
layout(set = 0, binding = 0) uniform sampler2D source;
layout(push_constant) uniform Parameters { vec2 sourceSize; vec2 outputSize; vec2 outputOffset; float sharpness; } p;
layout(location = 0) out vec4 color;
AF4 FsrRcasLoadF(ASU2 q) { return texelFetch(source, q, 0); }
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
#define FSR_RCAS_F 1
#include "ffx_fsr1.h"
void main() {
    AU4 con;
    FsrRcasCon(con, p.sharpness);   // stops: 0 = sharpest
    AF3 c;
    FsrRcasF(c.r, c.g, c.b, AU2(gl_FragCoord.xy - p.outputOffset), con);
    color = vec4(c, 1.0);
}
