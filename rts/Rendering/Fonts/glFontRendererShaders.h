/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef GL_FONT_RENDERER_SHADERS_H
#define GL_FONT_RENDERER_SHADERS_H

// Vertex-shader sources for the shader-based font renderer (CglShaderFontRenderer).
// Extracted into a header so the modern-GL A/B test (testGLFontMVPCompare) compiles
// the EXACT same source the engine uses -- no copy drift between test and engine.
//
// Transform select (Phase-1 modern-GL migration, see
// doc/bar-gl4-immediate-mode-inventory.md): when uUseMVP is true, glyphs transform
// by the uniform mat4 uMVP (fed from the fixed-function matrix bridge, so RenderDoc
// no longer sees the deprecated builtin); when false (the default), the legacy
// gl_ModelViewProjectionMatrix path runs -- byte-identical to the pre-migration
// shader. The builtin stays available because both shaders are compatibility/pre-core
// (#version 150 compatibility / #version 130).

inline constexpr const char* vsFont330 = R"(
#version 150 compatibility
#extension GL_ARB_explicit_attrib_location : enable

layout (location = 0) in vec3 pos;
layout (location = 1) in vec2 uv;
layout (location = 2) in vec4 col;

out Data {
	vec4 vCol;
	vec2 vUV;
};

uniform mat4 uMVP;
uniform bool uUseMVP;

void main() {
	vCol = col;
	vUV  = uv;
	gl_Position = (uUseMVP ? uMVP : gl_ModelViewProjectionMatrix) * vec4(pos, 1.0);
}
)";

inline constexpr const char* vsFont130 = R"(
#version 130

in vec3 pos;
in vec2 uv;
in vec4 col;

out vec4 vCol;
out vec2 vUV;

uniform mat4 uMVP;
uniform bool uUseMVP;

void main() {
	vCol = col;
	vUV  = uv;
	gl_Position = (uUseMVP ? uMVP : gl_ModelViewProjectionMatrix) * vec4(pos, 1.0);
}
)";

#endif // GL_FONT_RENDERER_SHADERS_H
