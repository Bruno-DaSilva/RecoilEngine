/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "Lua/LuaImmediateBuffer.h"

#include "Rendering/GL/myGL.h"
#include "Rendering/GL/RenderBuffers.h"
#include "Rendering/GlobalRendering.h"
#include "Rendering/Shaders/ShaderHandler.h"
#include "Rendering/Shaders/Shader.h"

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
	std::pair<std::vector<VA_TYPE_C>, uint32_t> TriangulateForModern(uint32_t mode, const std::vector<VA_TYPE_C>& in)
	{
		std::vector<VA_TYPE_C> out;

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
}

void LuaImmediateBuffer::FlushLegacy() const
{
	if (verts.empty())
		return;

	// transforms via the fixed-function matrix the caller has already set.
	glBegin(mode);
	for (const VA_TYPE_C& v : verts) {
		glColor4ub(v.c.r, v.c.g, v.c.b, v.c.a);
		glVertex3f(v.pos.x, v.pos.y, v.pos.z);
	}
	glEnd();
}

void LuaImmediateBuffer::FlushModern() const
{
	if (verts.empty())
		return;

	auto [drawVerts, drawMode] = TriangulateForModern(mode, verts);
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
}
