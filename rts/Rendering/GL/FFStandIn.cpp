/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "FFStandIn.h"

#include <string>

#include "Rendering/GL/myGL.h"
#include "Rendering/GlobalRendering.h"
#include "Rendering/Shaders/Shader.h"
#include "Rendering/Shaders/ShaderHandler.h"

namespace {
	std::string MakeVertexSrc(bool explicitAttribLoc, bool fogged)
	{
		// gl_ModelViewProjectionMatrix, not a CPU-composed uniform: the driver
		// composes P*MV itself so the transform is exact for any matrices, and
		// the model path pushes a fresh matrix per piece between draws. Reading a
		// builtin is not a GL call and costs nothing at the capture gate -- only
		// the FF matrix SET-calls do, and they are a separate group.
		std::string s = "#version 150 compatibility\n";
		if (explicitAttribLoc) {
			s += "#extension GL_ARB_explicit_attrib_location : require\n";
			s += "layout(location = 0) in vec3 apos;\n";
			s += "layout(location = 4) in vec4 auv;\n";
		} else {
			s += "in vec3 apos;\n";
			s += "in vec4 auv;\n";
		}
		s += "out vec2 vuv;\n";
		if (fogged)
			s += "out float vFogF;\n";
		s += "void main() { vuv = auv.xy; ";
		// Same linear-fog recipe the modern Lua immediate backend is
		// byte-parity-proven on: coordinate = |eye z|, state read from the
		// compatibility gl_Fog builtin. gl_ModelViewMatrix rather than a uniform
		// for the eye-space position, for the reason gl_ModelViewProjectionMatrix
		// is used above.
		if (fogged)
			s += "vFogF = (gl_Fog.end - abs((gl_ModelViewMatrix * vec4(apos, 1.0)).z)) * gl_Fog.scale; ";
		// User clip planes are applied to a fixed-function draw against eye space
		// automatically; a shader-bound draw in the compatibility profile only
		// gets them if the vertex stage says where the vertex is.
		s += "gl_ClipVertex = gl_ModelViewMatrix * vec4(apos, 1.0); ";
		s += "gl_Position = gl_ModelViewProjectionMatrix * vec4(apos, 1.0); }\n";
		return s;
	}

	std::string MakeFragmentSrc(bool fogged)
	{
		// GL_MODULATE against the fixed-function current colour, which the
		// caller passes in already clamped -- fixed function clamps vertex
		// colours at rasterization and an unclamped uniform would not.
		std::string s = fogged ? "#version 150 compatibility\n" /* gl_Fog */ : "#version 150\n";
		s += "uniform sampler2D tex;\n";
		s += "uniform vec4 uColor;\n";
		s += "uniform bool uTextured;\n";
		s += "in vec2 vuv;\n";
		if (fogged)
			s += "in float vFogF;\n";
		s += "out vec4 outColor;\n";
		s += "void main() { vec4 c = uTextured ? texture(tex, vuv) * uColor : uColor; ";
		s += fogged ? "outColor = vec4(mix(gl_Fog.color.rgb, c.rgb, clamp(vFogF, 0.0, 1.0)), c.a); }\n"
		            : "outColor = c; }\n";
		return s;
	}
}

Shader::IProgramObject* GL::FFStandIn::GetShader(bool fogged)
{
	const char* poName = fogged ? "FFStandInFog" : "FFStandIn";

	Shader::IProgramObject* shader = shaderHandler->GetProgramObject("[GL::FFStandIn]", poName);
	if (shader != nullptr && shader->IsValid())
		return shader;

	const bool eal = globalRendering->supportExplicitAttribLoc;

	shader = shaderHandler->CreateProgramObject("[GL::FFStandIn]", poName);
	shader->AttachShaderObject(shaderHandler->CreateShaderObject(MakeVertexSrc(eal, fogged), "", GL_VERTEX_SHADER));
	shader->AttachShaderObject(shaderHandler->CreateShaderObject(MakeFragmentSrc(fogged), "", GL_FRAGMENT_SHADER));

	if (!eal) {
		shader->BindAttribLocation("apos", 0);
		shader->BindAttribLocation("auv", 4);
	}

	shader->Link();
	return shader;
}

const char* GL::FFStandIn::StateReproducible(bool& textured, bool& fogged)
{
	// Every query here is a glGet/glIsEnabled, all of which RenderDoc supports;
	// the calls being replaced are the ones it does not.

	// Lighting is computed by fixed function for a fixed-function draw and by
	// nobody at all for a shader-bound one, so the shader would silently drop it.
	if (glIsEnabled(GL_LIGHTING) == GL_TRUE)
		return "GL_LIGHTING enabled";

	// Fixed function fogs its own draws; a shader-bound draw gets none unless the
	// shader computes it. The fogged variant does, for LINEAR only -- the mode the
	// engine sets -- and reads GL_FOG_COORD_SRC rather than assuming it, since an
	// explicit fog coordinate is not |eye z|.
	fogged = (glIsEnabled(GL_FOG) == GL_TRUE);

	if (fogged) {
		GLint fogMode = 0, fogCoordSrc = 0;
		glGetIntegerv(GL_FOG_MODE, &fogMode);
		glGetIntegerv(GL_FOG_COORD_SRC, &fogCoordSrc);

		if (fogMode != GL_LINEAR)
			return "fog mode is not GL_LINEAR";
		if (fogCoordSrc != GL_FRAGMENT_DEPTH)
			return "fog coordinate source is not GL_FRAGMENT_DEPTH";
	}

	GLint activeUnit = 0;
	glGetIntegerv(GL_ACTIVE_TEXTURE, &activeUnit);
	if (activeUnit != GL_TEXTURE0)
		return "active texture unit is not 0";

	// Any other enabled target on unit 0 outranks or joins GL_TEXTURE_2D and the
	// single sampler cannot stand in for it.
	if (glIsEnabled(GL_TEXTURE_1D) == GL_TRUE ||
	    glIsEnabled(GL_TEXTURE_3D) == GL_TRUE ||
	    glIsEnabled(GL_TEXTURE_CUBE_MAP) == GL_TRUE ||
	    glIsEnabled(GL_TEXTURE_RECTANGLE) == GL_TRUE)
		return "a non-2D texture target is enabled on unit 0";

	textured = (glIsEnabled(GL_TEXTURE_2D) == GL_TRUE);

	if (textured) {
		GLint envMode = 0;
		glGetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &envMode);
		if (envMode != GL_MODULATE)
			return "texenv mode is not GL_MODULATE";

		// NOT proven, so not taken. Sampling the same texture with the same
		// GL_MODULATE against the same current colour still differs from fixed
		// function by a little: measured under FFExperiment 6 with
		// ab_unitshape_driver.lua's rawState row texturing half its shapes, 15 px
		// a frame at delta 3, every frame. Small and consistent, which suggests a
		// per-edge or per-texel effect rather than a wrong transform -- but the
		// bar here is zero, and the untextured case below is at zero, so ship that
		// and leave this measured.
		return "textured fixed-function draw (substitute not yet pixel-exact)";
	}

	// A second enabled unit would combine into the fragment as well. Units 1/5/6
	// are the ones the model path ever binds (see BindLegacyTexUnits), and
	// checking a few beyond them costs nothing.
	bool otherUnitEnabled = false;
	for (GLenum unit = GL_TEXTURE1; unit <= GL_TEXTURE7 && !otherUnitEnabled; ++unit) {
		glActiveTexture(unit);
		otherUnitEnabled = (glIsEnabled(GL_TEXTURE_2D)        == GL_TRUE ||
		                    glIsEnabled(GL_TEXTURE_1D)        == GL_TRUE ||
		                    glIsEnabled(GL_TEXTURE_3D)        == GL_TRUE ||
		                    glIsEnabled(GL_TEXTURE_CUBE_MAP)  == GL_TRUE ||
		                    glIsEnabled(GL_TEXTURE_RECTANGLE) == GL_TRUE);
	}
	glActiveTexture(GL_TEXTURE0);

	if (otherUnitEnabled)
		return "a second texture unit is enabled";

	return nullptr;
}
