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
	const char* fsSrc =
		"#version 150\n"
		"in vec4 vcolor;\n"
		"out vec4 outColor;\n"
		"void main() { outColor = vcolor; }\n";

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

	// textured (VA_TYPE_TC) shader: MODULATE = texture * vertex color.
	const char* fsTexSrc =
		"#version 150\n"
		"uniform sampler2D tex;\n"
		"in vec2 vuv;\n"
		"in vec4 vcolor;\n"
		"out vec4 outColor;\n"
		"void main() { outColor = texture(tex, vuv) * vcolor; }\n";

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
}

void LuaImmediateBuffer::FlushLegacy() const
{
	if (verts.empty())
		return;

	// transforms via the fixed-function matrix the caller has already set;
	// replays texcoords when the stream was textured so it is an exact legacy
	// equivalent either way.
	glBegin(mode);
	for (const VA_TYPE_TC& v : verts) {
		if (textured)
			glTexCoord2f(v.s, v.t);
		glColor4ub(v.c.r, v.c.g, v.c.b, v.c.a);
		glVertex3f(v.pos.x, v.pos.y, v.pos.z);
	}
	glEnd();

	// exact legacy end-state: current color = last body glColor, or UNCHANGED
	// (= the inherited seed) when the body never called one -- the per-vertex
	// glColor replay above would otherwise leave the last vertex's color
	glColor4ub(sawColor ? lastColor.r : seedColor.r, sawColor ? lastColor.g : seedColor.g,
	           sawColor ? lastColor.b : seedColor.b, sawColor ? lastColor.a : seedColor.a);
}

void LuaImmediateBuffer::FlushModern() const
{
	if (verts.empty())
		return;

	// Textured BeginEnd streams fall back to the exact legacy replay (which
	// carries the texcoords). A single MODULATE/unit-0 shader can't reproduce
	// the per-widget texture-unit / texenv variety real widgets use -- the
	// in-engine compare caught gui_pip's textured overlays diverging (255) when
	// this path used a modern shader. General textured BeginEnd is deferred;
	// gl.TexRect (a controlled single MODULATE quad) is modernized separately.
	if (textured) {
		FlushLegacy();
		return;
	}

	std::vector<VA_TYPE_C> colorVerts;
	colorVerts.reserve(verts.size());
	for (const VA_TYPE_TC& v : verts)
		colorVerts.push_back(VA_TYPE_C{v.pos, v.c});

	auto [drawVerts, drawMode] = TriangulateForModern<VA_TYPE_C>(mode, colorVerts);
	if (drawVerts.empty())
		return;

	auto& rb = RenderBuffer::GetTypedRenderBuffer<VA_TYPE_C>();
	for (const VA_TYPE_C& v : drawVerts)
		rb.AddVertex(VA_TYPE_C{v});

	Shader::IProgramObject* shader = GetModernShader();
	shader->Enable();
	shader->SetUniformMatrix4x4("uMVP", false, static_cast<const float*>(mvp));
	rb.DrawArrays(drawMode);
	shader->Disable();

	// Legacy glBegin/glEnd leaves the FF current color at the body's last glColor
	// (or UNCHANGED when the body issued none), and the compatibility profile
	// exposes that as gl_Color to LATER shader draws -- BAR's gui_pip minimap
	// shader reads its alpha, display-list replays without recorded glColor
	// inherit it. FlushModern draws via a shader and never touches glColor, so
	// replicate the exact legacy side effect (apitrace-confirmed classes: the
	// gui_pip minimap wash, the minimap camera-box blue-channel divergence).
	const SColor& lc = sawColor ? lastColor : seedColor;
	glColor4ub(lc.r, lc.g, lc.b, lc.a);
}

void LuaImmediateBuffer::FlushTexRectLegacy() const
{
	if (!texRect.set)
		return;

	// caller has bound the texture and enabled GL_TEXTURE_2D; FF MODULATE
	// gives texture * glColor.
	glColor4ub(texRect.c.r, texRect.c.g, texRect.c.b, texRect.c.a);
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
	// GL_TEXTURE_2D enable needed). Quad as two CCW triangles.
	const SColor c = texRect.c;
	const VA_TYPE_TC bl{ {texRect.x0, texRect.y0, 0.0f}, texRect.s0, texRect.t0, c };
	const VA_TYPE_TC br{ {texRect.x1, texRect.y0, 0.0f}, texRect.s1, texRect.t0, c };
	const VA_TYPE_TC tr{ {texRect.x1, texRect.y1, 0.0f}, texRect.s1, texRect.t1, c };
	const VA_TYPE_TC tl{ {texRect.x0, texRect.y1, 0.0f}, texRect.s0, texRect.t1, c };

	auto& rb = RenderBuffer::GetTypedRenderBuffer<VA_TYPE_TC>();
	rb.AddVertex(VA_TYPE_TC{bl}); rb.AddVertex(VA_TYPE_TC{br}); rb.AddVertex(VA_TYPE_TC{tr});
	rb.AddVertex(VA_TYPE_TC{bl}); rb.AddVertex(VA_TYPE_TC{tr}); rb.AddVertex(VA_TYPE_TC{tl});

	Shader::IProgramObject* shader = GetModernTexShader();
	shader->Enable();
	shader->SetUniformMatrix4x4("uMVP", false, static_cast<const float*>(mvp));
	shader->SetUniform("tex", 0);
	rb.DrawArrays(GL_TRIANGLES);
	shader->Disable();

	// FlushTexRectLegacy sets glColor4ub(texRect.c); the compatibility profile
	// carries that FF current color into LATER draws (gl_Color). Replicate it so a
	// modern gl.TexRect is state-identical to legacy -- otherwise a following text
	// draw inherits a stale color (apitrace class: the minimap wash, here on the
	// countdown text).
	glColor4ub(c.r, c.g, c.b, c.a);
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
