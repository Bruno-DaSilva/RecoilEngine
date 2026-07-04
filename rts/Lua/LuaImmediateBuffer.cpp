/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "Lua/LuaImmediateBuffer.h"

#include "Rendering/GL/myGL.h"
#include "Rendering/GL/RenderBuffers.h"
#include "Rendering/GlobalRendering.h"
#include "Rendering/Shaders/ShaderHandler.h"
#include "Rendering/Shaders/Shader.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace {
	// uniform-MVP shader for the modern backend: transforms by a uniform mat4,
	// NOT gl_ModelViewProjectionMatrix, so the path makes no fixed-function
	// matrix calls. Attribute locations match VA_TYPE_C::attributeDefs (pos=0,
	// color=1) so it consumes the TypedRenderBuffer VAO directly.
	// uColor: whole-draw color multiplier. For single-color streams the exact
	// float color goes here (vertex colors white) so animated fade alphas match
	// the legacy float pipeline bit-exactly instead of the 8-bit attribute
	// quantization; multi-color streams use per-vertex colors with uColor=1.
	const char* fsSrc =
		"#version 150\n"
		"in vec4 vcolor;\n"
		"uniform vec4 uColor;\n"
		"out vec4 outColor;\n"
		"void main() { outColor = vcolor * uColor; }\n";

	std::string MakeVertexSrc(bool explicitAttribLoc)
	{
		std::string s = "#version 150 compatibility\n";
		if (explicitAttribLoc) {
			s += "#extension GL_ARB_explicit_attrib_location : require\n";
			s += "layout(location = 0) in vec3 apos;\n";
			s += "layout(location = 1) in vec4 acolor;\n";
		} else {
			s += "in vec3 apos;\n";
			s += "in vec4 acolor;\n";
		}
		s += "uniform mat4 uMVP;\n";
		s += "out vec4 vcolor;\n";
		s += "void main() { vcolor = acolor; gl_Position = uMVP * vec4(apos, 1.0); }\n";
		return s;
	}

	// GL_QUADS / GL_QUAD_STRIP / GL_POLYGON are not in the core profile, so the
	// modern path triangulates them; other modes pass through unchanged. For a
	// planar convex quad/polygon the triangle union (hence coverage) is the same
	// as the fixed-function decomposition, so flat-colored fills stay bit-exact;
	// only smooth-shaded fills can differ by the diagonal choice.
	template<typename V>
	std::pair<std::vector<V>, uint32_t> TriangulateForModern(uint32_t mode, const std::vector<V>& in)
	{
		std::vector<V> out;

		switch (mode) {
			case GL_QUADS: {
				out.reserve((in.size() / 4) * 6);
				for (size_t i = 0; i + 3 < in.size(); i += 4) {
					out.push_back(in[i + 0]); out.push_back(in[i + 1]); out.push_back(in[i + 2]);
					out.push_back(in[i + 0]); out.push_back(in[i + 2]); out.push_back(in[i + 3]);
				}
				return {std::move(out), GL_TRIANGLES};
			}
			case GL_QUAD_STRIP: {
				// quad k spans verts {2k, 2k+1, 2k+3, 2k+2}
				for (size_t i = 0; i + 3 < in.size(); i += 2) {
					out.push_back(in[i + 0]); out.push_back(in[i + 1]); out.push_back(in[i + 3]);
					out.push_back(in[i + 0]); out.push_back(in[i + 3]); out.push_back(in[i + 2]);
				}
				return {std::move(out), GL_TRIANGLES};
			}
			case GL_POLYGON: {
				// triangle fan from the first vertex (matches FF for convex polys)
				for (size_t i = 1; i + 1 < in.size(); ++i) {
					out.push_back(in[0]); out.push_back(in[i]); out.push_back(in[i + 1]);
				}
				return {std::move(out), GL_TRIANGLES};
			}
			default:
				return {in, mode};
		}
	}

	Shader::IProgramObject* GetModernShader()
	{
		Shader::IProgramObject* shader = shaderHandler->GetProgramObject("[LuaImmediateBuffer]", "VA_TYPE_C");
		if (shader != nullptr && shader->IsValid())
			return shader;

		const bool eal = globalRendering->supportExplicitAttribLoc;

		shader = shaderHandler->CreateProgramObject("[LuaImmediateBuffer]", "VA_TYPE_C");
		shader->AttachShaderObject(shaderHandler->CreateShaderObject(MakeVertexSrc(eal), "", GL_VERTEX_SHADER));
		shader->AttachShaderObject(shaderHandler->CreateShaderObject(fsSrc, "", GL_FRAGMENT_SHADER));

		if (!eal) {
			shader->BindAttribLocation("apos", 0);
			shader->BindAttribLocation("acolor", 1);
		}

		shader->Link();
		return shader;
	}

	// textured (VA_TYPE_TC) shader: MODULATE = texture * color (uColor as in
	// the untextured shader: exact float whole-draw color, vertex colors white).
	const char* fsTexSrc =
		"#version 150\n"
		"uniform sampler2D tex;\n"
		"uniform vec4 uColor;\n"
		"in vec2 vuv;\n"
		"in vec4 vcolor;\n"
		"out vec4 outColor;\n"
		"void main() { outColor = texture(tex, vuv) * vcolor * uColor; }\n";

	std::string MakeTexVertexSrc(bool explicitAttribLoc)
	{
		std::string s = "#version 150 compatibility\n";
		if (explicitAttribLoc) {
			s += "#extension GL_ARB_explicit_attrib_location : require\n";
			s += "layout(location = 0) in vec3 apos;\n";
			s += "layout(location = 1) in vec2 auv;\n";
			s += "layout(location = 2) in vec4 acolor;\n";
		} else {
			s += "in vec3 apos;\n";
			s += "in vec2 auv;\n";
			s += "in vec4 acolor;\n";
		}
		s += "uniform mat4 uMVP;\n";
		s += "out vec2 vuv;\n";
		s += "out vec4 vcolor;\n";
		s += "void main() { vuv = auv; vcolor = acolor; gl_Position = uMVP * vec4(apos, 1.0); }\n";
		return s;
	}

	Shader::IProgramObject* GetModernTexShader()
	{
		Shader::IProgramObject* shader = shaderHandler->GetProgramObject("[LuaImmediateBuffer]", "VA_TYPE_TC");
		if (shader != nullptr && shader->IsValid())
			return shader;

		const bool eal = globalRendering->supportExplicitAttribLoc;

		shader = shaderHandler->CreateProgramObject("[LuaImmediateBuffer]", "VA_TYPE_TC");
		shader->AttachShaderObject(shaderHandler->CreateShaderObject(MakeTexVertexSrc(eal), "", GL_VERTEX_SHADER));
		shader->AttachShaderObject(shaderHandler->CreateShaderObject(fsTexSrc, "", GL_FRAGMENT_SHADER));

		if (!eal) {
			shader->BindAttribLocation("apos", 0);
			shader->BindAttribLocation("auv", 1);
			shader->BindAttribLocation("acolor", 2);
		}

		shader->Link();
		return shader;
	}

	// The FF texturing configurations the modern textured shader reproduces
	// EXACTLY: active unit 0 with plain GL_TEXTURE_2D sampling (no
	// higher-priority target enabled, no texgen, no other enabled units),
	// MODULATE env, identity texture matrix, and a texture whose base format
	// modulates like GLSL sampling does. Anything else falls back to the exact
	// legacy replay: legacy is the parity oracle, so unsupported state costs
	// only modern coverage, never correctness. This gate is why the earlier
	// ungated attempt diverged on gui_pip's textured overlays (255-class).
	bool PlainModulateTexturing()
	{
		GLint activeUnit = 0;
		glGetIntegerv(GL_ACTIVE_TEXTURE, &activeUnit);
		if (activeUnit != GL_TEXTURE0)
			return false;

		if (glIsEnabled(GL_TEXTURE_2D) != GL_TRUE)
			return false;

		// FF target priority: an enabled cube/rect/3D target overrides 2D
		// (1D is BELOW 2D, so an enabled 1D loses and is fine)
		if (glIsEnabled(GL_TEXTURE_CUBE_MAP) == GL_TRUE ||
		    glIsEnabled(GL_TEXTURE_RECTANGLE) == GL_TRUE ||
		    glIsEnabled(GL_TEXTURE_3D) == GL_TRUE)
			return false;

		// texgen replaces the captured per-vertex texcoords
		if (glIsEnabled(GL_TEXTURE_GEN_S) == GL_TRUE || glIsEnabled(GL_TEXTURE_GEN_T) == GL_TRUE ||
		    glIsEnabled(GL_TEXTURE_GEN_R) == GL_TRUE || glIsEnabled(GL_TEXTURE_GEN_Q) == GL_TRUE)
			return false;

		GLint envMode = 0;
		glGetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &envMode);
		if (envMode != GL_MODULATE)
			return false;

		// FF transforms texcoords by the texture matrix; the shader does not
		static const CMatrix44f identity;
		CMatrix44f texMat;
		glGetFloatv(GL_TEXTURE_MATRIX, static_cast<float*>(texMat));
		for (int i = 0; i < 16; ++i) {
			if (texMat.m[i] != identity.m[i])
				return false;
		}

		// GL_ALPHA-format MODULATE passes the fragment RGB through untouched,
		// while GLSL texture() samples (0,0,0,A) and would zero it
		GLint intFormat = 0;
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &intFormat);
		switch (intFormat) {
			case GL_ALPHA: case GL_ALPHA4: case GL_ALPHA8:
			case GL_ALPHA12: case GL_ALPHA16: case GL_COMPRESSED_ALPHA:
				return false;
			default:
				break;
		}

		// any other enabled unit engages FF multitexture combining that the
		// single-sampler shader does not implement (GL_MAX_TEXTURE_UNITS is the
		// FF unit count, typically 4)
		GLint maxFFUnits = 0;
		glGetIntegerv(GL_MAX_TEXTURE_UNITS, &maxFFUnits);
		maxFFUnits = std::min(maxFFUnits, GLint(8));

		bool otherUnitEnabled = false;
		for (GLint u = 1; u < maxFFUnits; ++u) {
			glActiveTexture(GL_TEXTURE0 + u);
			otherUnitEnabled = otherUnitEnabled ||
				(glIsEnabled(GL_TEXTURE_2D) == GL_TRUE) ||
				(glIsEnabled(GL_TEXTURE_1D) == GL_TRUE) ||
				(glIsEnabled(GL_TEXTURE_3D) == GL_TRUE) ||
				(glIsEnabled(GL_TEXTURE_CUBE_MAP) == GL_TRUE) ||
				(glIsEnabled(GL_TEXTURE_RECTANGLE) == GL_TRUE);
		}
		glActiveTexture(GL_TEXTURE0);

		return !otherUnitEnabled;
	}
}

void LuaImmediateBuffer::FlushLegacy() const
{
	if (verts.empty())
		return;

	// transforms via the fixed-function matrix the caller has already set;
	// replays texcoords when the stream was textured, and the ORIGINAL float
	// colors (not the quantized SColor), so it is an exact legacy equivalent
	// either way.
	assert(vertColorsF.size() == verts.size() * 4);
	glBegin(mode);
	for (size_t i = 0; i < verts.size(); ++i) {
		const VA_TYPE_TC& v = verts[i];
		if (textured)
			glTexCoord2f(v.s, v.t);
		glColor4fv(&vertColorsF[i * 4]);
		glVertex3f(v.pos.x, v.pos.y, v.pos.z);
	}
	glEnd();

	// exact legacy end-state: current color = last body glColor, or UNCHANGED
	// (= the inherited seed) when the body never called one -- the per-vertex
	// glColor replay above would otherwise leave the last vertex's quantized
	// color. Float precision so unclamped/overbright current colors round-trip.
	glColor4fv(sawColor ? lastColorF : seedColorF);
}

void LuaImmediateBuffer::FlushModern() const
{
	if (verts.empty())
		return;

	// single-color streams route the exact float color through uColor (vertex
	// colors white) -- the 8-bit vertex attribute cannot represent animated
	// fade alphas bit-exactly; multi-color streams keep per-vertex colors
	static constexpr SColor white = SColor(uint8_t(255), uint8_t(255), uint8_t(255), uint8_t(255));
	const auto c01 = [](float v) { return std::clamp(v, 0.0f, 1.0f); };

	// Textured streams flush through the MODULATE shader when the FF texture
	// state is one it reproduces exactly (see PlainModulateTexturing); any
	// other texture-unit/texenv setup falls back to the exact legacy replay
	// (which carries the texcoords).
	if (textured) {
		if (!PlainModulateTexturing()) {
			FlushLegacy();
			return;
		}

		std::vector<VA_TYPE_TC> tcVerts;
		tcVerts.reserve(verts.size());
		for (const VA_TYPE_TC& v : verts)
			tcVerts.push_back(VA_TYPE_TC{v.pos, v.s, v.t, multiColor ? v.c : white});

		auto [drawVerts, drawMode] = TriangulateForModern<VA_TYPE_TC>(mode, tcVerts);
		if (drawVerts.empty())
			return;

		auto& rb = RenderBuffer::GetTypedRenderBuffer<VA_TYPE_TC>();
		for (const VA_TYPE_TC& v : drawVerts)
			rb.AddVertex(VA_TYPE_TC{v});

		Shader::IProgramObject* shader = GetModernTexShader();
		shader->Enable();
		shader->SetUniformMatrix4x4("uMVP", false, static_cast<const float*>(mvp));
		shader->SetUniform("tex", 0);
		if (multiColor)
			shader->SetUniform("uColor", 1.0f, 1.0f, 1.0f, 1.0f);
		else // clamped as the FF pipeline does at rasterization
			shader->SetUniform("uColor", c01(singleColorF[0]), c01(singleColorF[1]), c01(singleColorF[2]), c01(singleColorF[3]));
		rb.DrawArrays(drawMode);
		shader->Disable();

		// same FF current-color end-state contract as the untextured flush below
		glColor4fv(sawColor ? lastColorF : seedColorF);
		return;
	}

	std::vector<VA_TYPE_C> colorVerts;
	colorVerts.reserve(verts.size());
	for (const VA_TYPE_TC& v : verts)
		colorVerts.push_back(VA_TYPE_C{v.pos, multiColor ? v.c : white});

	auto [drawVerts, drawMode] = TriangulateForModern<VA_TYPE_C>(mode, colorVerts);
	if (drawVerts.empty())
		return;

	auto& rb = RenderBuffer::GetTypedRenderBuffer<VA_TYPE_C>();
	for (const VA_TYPE_C& v : drawVerts)
		rb.AddVertex(VA_TYPE_C{v});

	Shader::IProgramObject* shader = GetModernShader();
	shader->Enable();
	shader->SetUniformMatrix4x4("uMVP", false, static_cast<const float*>(mvp));
	if (multiColor)
		shader->SetUniform("uColor", 1.0f, 1.0f, 1.0f, 1.0f);
	else // clamped as the FF pipeline does at rasterization
		shader->SetUniform("uColor", c01(singleColorF[0]), c01(singleColorF[1]), c01(singleColorF[2]), c01(singleColorF[3]));
	rb.DrawArrays(drawMode);
	shader->Disable();

	// Legacy glBegin/glEnd leaves the FF current color at the body's last glColor
	// (or UNCHANGED when the body issued none), and the compatibility profile
	// exposes that as gl_Color to LATER shader draws -- BAR's gui_pip minimap
	// shader reads its alpha, display-list replays without recorded glColor
	// inherit it. FlushModern draws via a shader and never touches glColor, so
	// replicate the exact legacy side effect (apitrace-confirmed classes: the
	// gui_pip minimap wash, the minimap camera-box blue-channel divergence).
	// Float precision so unclamped/overbright current colors round-trip.
	glColor4fv(sawColor ? lastColorF : seedColorF);
}

void LuaImmediateBuffer::FlushTexRectLegacy() const
{
	if (!texRect.set)
		return;

	// caller has bound the texture and enabled GL_TEXTURE_2D; FF MODULATE
	// gives texture * glColor (exact float color).
	glColor4fv(texRect.cf);
	glBegin(GL_QUADS);
		glTexCoord2f(texRect.s0, texRect.t0); glVertex2f(texRect.x0, texRect.y0);
		glTexCoord2f(texRect.s1, texRect.t0); glVertex2f(texRect.x1, texRect.y0);
		glTexCoord2f(texRect.s1, texRect.t1); glVertex2f(texRect.x1, texRect.y1);
		glTexCoord2f(texRect.s0, texRect.t1); glVertex2f(texRect.x0, texRect.y1);
	glEnd();
}

void LuaImmediateBuffer::FlushTexRectModern() const
{
	if (!texRect.set)
		return;

	// caller has bound the texture to unit 0; the shader samples it (no
	// GL_TEXTURE_2D enable needed). Quad as two CCW triangles; the exact float
	// modulation color goes through uColor (vertex colors white).
	static constexpr SColor white = SColor(uint8_t(255), uint8_t(255), uint8_t(255), uint8_t(255));
	const VA_TYPE_TC bl{ {texRect.x0, texRect.y0, 0.0f}, texRect.s0, texRect.t0, white };
	const VA_TYPE_TC br{ {texRect.x1, texRect.y0, 0.0f}, texRect.s1, texRect.t0, white };
	const VA_TYPE_TC tr{ {texRect.x1, texRect.y1, 0.0f}, texRect.s1, texRect.t1, white };
	const VA_TYPE_TC tl{ {texRect.x0, texRect.y1, 0.0f}, texRect.s0, texRect.t1, white };

	auto& rb = RenderBuffer::GetTypedRenderBuffer<VA_TYPE_TC>();
	rb.AddVertex(VA_TYPE_TC{bl}); rb.AddVertex(VA_TYPE_TC{br}); rb.AddVertex(VA_TYPE_TC{tr});
	rb.AddVertex(VA_TYPE_TC{bl}); rb.AddVertex(VA_TYPE_TC{tr}); rb.AddVertex(VA_TYPE_TC{tl});

	const auto c01 = [](float v) { return std::clamp(v, 0.0f, 1.0f); };

	Shader::IProgramObject* shader = GetModernTexShader();
	shader->Enable();
	shader->SetUniformMatrix4x4("uMVP", false, static_cast<const float*>(mvp));
	shader->SetUniform("tex", 0);
	// clamped as the FF pipeline does at rasterization
	shader->SetUniform("uColor", c01(texRect.cf[0]), c01(texRect.cf[1]), c01(texRect.cf[2]), c01(texRect.cf[3]));
	rb.DrawArrays(GL_TRIANGLES);
	shader->Disable();

	// FlushTexRectLegacy sets glColor(texRect.cf); the compatibility profile
	// carries that FF current color into LATER draws (gl_Color). Replicate it so a
	// modern gl.TexRect is state-identical to legacy -- otherwise a following text
	// draw inherits a stale color (apitrace class: the minimap wash, here on the
	// countdown text).
	glColor4fv(texRect.cf);
}


namespace LuaGLCompare {
	static GLuint fboA = 0, texA = 0, fboB = 0, texB = 0, depthRB = 0;
	static int fbW = 0, fbH = 0;

	static void DeleteFBOs()
	{
		if (fboA)    glDeleteFramebuffers(1, &fboA);
		if (fboB)    glDeleteFramebuffers(1, &fboB);
		if (texA)    glDeleteTextures(1, &texA);
		if (texB)    glDeleteTextures(1, &texB);
		if (depthRB) glDeleteRenderbuffers(1, &depthRB);
		fboA = texA = fboB = texB = depthRB = 0;
	}

	static GLuint MakeColorTex(int w, int h)
	{
		GLuint t = 0;
		glGenTextures(1, &t);
		glBindTexture(GL_TEXTURE_2D, t);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		return t;
	}

	static bool EnsureFBOs(int w, int h)
	{
		if (w == fbW && h == fbH && fboA != 0)
			return true;

		DeleteFBOs();
		if (w <= 0 || h <= 0)
			return false;

		texA = MakeColorTex(w, h);
		texB = MakeColorTex(w, h);

		glGenRenderbuffers(1, &depthRB);
		glBindRenderbuffer(GL_RENDERBUFFER, depthRB);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);

		const auto makeFBO = [&](GLuint tex) {
			GLuint f = 0;
			glGenFramebuffers(1, &f);
			glBindFramebuffer(GL_FRAMEBUFFER, f);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
			glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depthRB);
			return f;
		};
		fboA = makeFBO(texA);
		fboB = makeFBO(texB);

		const bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		fbW = w; fbH = h;

		if (!ok) {
			DeleteFBOs();
			fbW = fbH = 0;
			return false;
		}
		return true;
	}

	int CompareDraws(int w, int h, const std::function<void()>& drawLegacy, const std::function<void()>& drawModern)
	{
		if (!EnsureFBOs(w, h))
			return -1;

		GLint prevFBO = 0;
		GLint prevVP[4] = {0, 0, 0, 0};
		GLfloat prevClear[4] = {0, 0, 0, 0};
		glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);
		glGetIntegerv(GL_VIEWPORT, prevVP);
		glGetFloatv(GL_COLOR_CLEAR_VALUE, prevClear);

		std::vector<uint8_t> a(static_cast<size_t>(w) * h * 4);
		std::vector<uint8_t> b(static_cast<size_t>(w) * h * 4);

		glViewport(0, 0, w, h);
		glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
		glPixelStorei(GL_PACK_ALIGNMENT, 1);

		glBindFramebuffer(GL_FRAMEBUFFER, fboA);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		drawLegacy();
		glFinish(); // ensure the draw completes before readback (threaded sw rasterizers)
		glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, a.data());

		glBindFramebuffer(GL_FRAMEBUFFER, fboB);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		drawModern();
		glFinish(); // ensure the draw completes before readback (threaded sw rasterizers)
		glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, b.data());

		glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFBO));
		glViewport(prevVP[0], prevVP[1], prevVP[2], prevVP[3]);
		glClearColor(prevClear[0], prevClear[1], prevClear[2], prevClear[3]);

		int maxDelta = 0;
		for (size_t i = 0; i < a.size(); ++i)
			maxDelta = std::max(maxDelta, std::abs(int(a[i]) - int(b[i])));
		return maxDelta;
	}
}
