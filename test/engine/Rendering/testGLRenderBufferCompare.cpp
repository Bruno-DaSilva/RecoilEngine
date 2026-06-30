/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

// Tier-2 A/B against the REAL engine renderer: GL::TypedRenderBuffer vs legacy
// glBegin, same MVP. Unlike testGLMatrixDrawCompare (hand-rolled VAO+shader),
// this links the actual engine rendering path (RenderBuffers/Shader/VAO/VBO/
// StreamBuffer) so we validate what the migration will really run.
//
// This test lives in a subdirectory that removes -DUNIT_TEST, because the
// engine GL headers (myGL.h) intentionally #error under UNIT_TEST. The few
// engine globals the render path needs (globalRendering, configHandler) are
// provided as minimal stubs here to avoid dragging in the GL window system /
// VFS / config file machinery — see GLEngineStubs.cpp.
//
// SKIPs with no GL context. Run under xvfb-run + LIBGL_ALWAYS_SOFTWARE=1.

#include "GLTestHarness.h"

#include "Rendering/GL/RenderBuffers.h"
#include "Rendering/GL/VertexArrayTypes.h"
#include "Rendering/Shaders/ShaderHandler.h"
#include "Rendering/Shaders/Shader.h"
#include "System/Matrix44f.h"
#include "System/Color.h"

using namespace gltest;

namespace {

// quad as two CCW triangles, single color
std::vector<VA_TYPE_C> FlatQuadC(float x0, float y0, float x1, float y1, SColor col)
{
	const VA_TYPE_C tl{ {x0, y1, 0}, col };
	const VA_TYPE_C tr{ {x1, y1, 0}, col };
	const VA_TYPE_C br{ {x1, y0, 0}, col };
	const VA_TYPE_C bl{ {x0, y0, 0}, col };
	return { bl, br, tr,  bl, tr, tl };
}

CMatrix44f OrthoMVP()
{
	return CMatrix44f::OrthoProj(0.0f, float(kSize), 0.0f, float(kSize), -1.0f, 1.0f);
}

// The modern backend's shader: a uniform mat4 MVP, no gl_ModelViewProjectionMatrix.
// Attributes use explicit locations matching VA_TYPE_C::attributeDefs (pos=0,
// color=1), so it consumes the engine TypedRenderBuffer's VAO directly.
const char* kUniMVP_VS =
	"#version 150 compatibility\n"
	"#extension GL_ARB_explicit_attrib_location : require\n"
	"layout(location = 0) in vec3 apos;\n"
	"layout(location = 1) in vec4 acolor;\n"
	"uniform mat4 uMVP;\n"
	"out vec4 vcol;\n"
	"void main() { vcol = acolor; gl_Position = uMVP * vec4(apos, 1.0); }\n";

const char* kUniMVP_FS =
	"#version 150\n"
	"in vec4 vcol;\n"
	"out vec4 outColor;\n"
	"void main() { outColor = vcol; }\n";

Shader::IProgramObject* BuildUniformMVPShader()
{
	auto* sh = shaderHandler->CreateProgramObject("[GLRenderBufferCompare]", "uniMVP");
	sh->AttachShaderObject(shaderHandler->CreateShaderObject(kUniMVP_VS, "", GL_VERTEX_SHADER));
	sh->AttachShaderObject(shaderHandler->CreateShaderObject(kUniMVP_FS, "", GL_FRAGMENT_SHADER));
	sh->Link();
	return sh;
}

void LoadFFMatrix(const CMatrix44f& mvp)
{
	glMatrixMode(GL_PROJECTION);
	glLoadMatrixf(static_cast<const float*>(mvp));
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
}

void LegacyDrawC(const CMatrix44f& mvp, const std::vector<VA_TYPE_C>& v)
{
	LoadFFMatrix(mvp);
	glBegin(GL_TRIANGLES);
	for (const VA_TYPE_C& x : v) {
		glColor4ub(x.c.r, x.c.g, x.c.b, x.c.a);
		glVertex3f(x.pos.x, x.pos.y, x.pos.z);
	}
	glEnd();
}

void EngineDrawC(const CMatrix44f& mvp, const std::vector<VA_TYPE_C>& v)
{
	// the stock RenderBuffer shader transforms by gl_ModelViewProjectionMatrix,
	// so feed the same MVP through the fixed-function stack.
	LoadFFMatrix(mvp);

	auto& rb = RenderBuffer::GetTypedRenderBuffer<VA_TYPE_C>();
	for (const VA_TYPE_C& vert : v)
		rb.AddVertex(VA_TYPE_C{vert});

	auto& shader = rb.GetShader();
	shader.Enable();
	rb.DrawArrays(GL_TRIANGLES);
	shader.Disable();
}

} // namespace


TEST_CASE("GLRenderBufferCompare: engine TypedRenderBuffer vs legacy glBegin (flat quad)")
{
	if (!EnsureGL())
		SKIP("no usable OpenGL context (headless box); skipping GL harness");

	RenderBuffer::InitStatic();

	const CMatrix44f mvp = OrthoMVP();
	const auto quad = FlatQuadC(32, 32, 200, 160, SColor(uint8_t(204), uint8_t(102), uint8_t(51), uint8_t(255)));

	const auto legacy = RenderToBuffer([&] {
		ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
		LegacyDrawC(mvp, quad);
	});
	const auto engine = RenderToBuffer([&] {
		ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
		EngineDrawC(mvp, quad);
	});

	const DiffResult d = Compare(legacy, engine);
	INFO("firstDiffByte=" << d.firstDiffByte << " maxAbsDelta=" << d.maxAbsDelta << " diffBytes=" << d.diffBytes);
	CHECK(d.maxAbsDelta <= 1);

	// guard against a false pass where BOTH sides draw nothing: the engine
	// buffer must actually contain the quad color at the FBO center (128,128),
	// which lies inside the [32,200]x[32,160] quad.
	const size_t c = (size_t(kSize / 2) * kSize + kSize / 2) * 4;
	CHECK(engine[c + 0] == 204);
	CHECK(engine[c + 1] == 102);
	CHECK(engine[c + 2] == 51);

	RenderBuffer::KillStatic();
}


TEST_CASE("GLRenderBufferCompare: engine geometry + uniform-MVP shader (no FF matrix) vs legacy")
{
	if (!EnsureGL())
		SKIP("no usable OpenGL context (headless box); skipping GL harness");

	RenderBuffer::InitStatic();

	const CMatrix44f mvp = OrthoMVP();
	const auto quad = FlatQuadC(32, 32, 200, 160, SColor(uint8_t(204), uint8_t(102), uint8_t(51), uint8_t(255)));

	const auto legacy = RenderToBuffer([&] {
		ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
		LegacyDrawC(mvp, quad);
	});

	Shader::IProgramObject* shader = BuildUniformMVPShader();
	REQUIRE(shader->IsValid());

	const auto modern = RenderToBuffer([&] {
		ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
		// the modern path makes NO fixed-function matrix calls; the shader reads
		// only the uMVP uniform (fed here by CMatrix44f, in the engine by the
		// GLMatrixStateTracker) and ignores gl_ModelViewProjectionMatrix.
		auto& rb = RenderBuffer::GetTypedRenderBuffer<VA_TYPE_C>();
		for (const VA_TYPE_C& vert : quad)
			rb.AddVertex(VA_TYPE_C{vert});

		shader->Enable();
		shader->SetUniformMatrix4x4("uMVP", false, static_cast<const float*>(mvp));
		rb.DrawArrays(GL_TRIANGLES);
		shader->Disable();
	});

	const DiffResult d = Compare(legacy, modern);
	INFO("firstDiffByte=" << d.firstDiffByte << " maxAbsDelta=" << d.maxAbsDelta << " diffBytes=" << d.diffBytes);
	CHECK(d.maxAbsDelta <= 1);

	const size_t c = (size_t(kSize / 2) * kSize + kSize / 2) * 4;
	CHECK(modern[c + 0] == 204);
	CHECK(modern[c + 1] == 102);
	CHECK(modern[c + 2] == 51);

	shaderHandler->ReleaseProgramObject("[GLRenderBufferCompare]", "uniMVP");
	RenderBuffer::KillStatic();
}
