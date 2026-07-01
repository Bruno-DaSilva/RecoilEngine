/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

// Tier-2 A/B for the font-renderer uMVP conversion (Phase-1 modern-GL migration,
// see doc/bar-gl4-immediate-mode-inventory.md). The font vertex shader gained a
// runtime toggle:
//     gl_Position = (uUseMVP ? uMVP : gl_ModelViewProjectionMatrix) * vec4(pos,1);
// This test proves the two branches produce the same pixels for the same MVP:
//   - Pass A (oracle):  uUseMVP=0, MVP loaded into the fixed-function stack -> the
//     legacy gl_ModelViewProjectionMatrix branch runs (the pre-migration path).
//   - Pass B (modern):  uUseMVP=1, FF matrices left identity, MVP fed via the uMVP
//     uniform -> the modern branch runs and the FF matrix is NOT consulted.
// Equal pixels => the conversion is byte-(LSB-)faithful and the uniform path is the
// one actually transforming (an identity FF matrix would clip the quad away if the
// builtin branch were wrongly taken, which the non-empty center-pixel check catches).
//
// It compiles the EXACT engine vertex-shader source (glFontRendererShaders.h), so
// there is no copy drift. Driving the real CglShaderFontRenderer is intentionally
// avoided: its PushGLState pulls glFont/CFontTexture/FreeType, far beyond this dir's
// light stub link set.
//
// SKIPs with no GL context. Run under xvfb-run + LIBGL_ALWAYS_SOFTWARE=1.

#include "GLTestHarness.h"

#include "Rendering/Fonts/glFontRendererShaders.h"
#include "Rendering/GL/RenderBuffers.h"
#include "Rendering/GL/VertexArrayTypes.h"
#include "Rendering/Shaders/ShaderHandler.h"
#include "Rendering/Shaders/Shader.h"
#include "System/Matrix44f.h"
#include "System/Color.h"

using namespace gltest;

namespace {

// Fragment shader matching vsFont330's "Data" interface block and the real alpha
// font FS (fsFont330): alpha comes from the texture's red channel, rgb from vCol.
const char* kFontFS330 =
	"#version 150\n"
	"uniform sampler2D tex;\n"
	"in Data { vec4 vCol; vec2 vUV; };\n"
	"out vec4 outColor;\n"
	"void main() {\n"
	"	vec2 texSize = vec2(textureSize(tex, 0));\n"
	"	float alpha = texture(tex, vUV / texSize).x;\n"
	"	outColor = vec4(vCol.rgb, vCol.a * alpha);\n"
	"}\n";

Shader::IProgramObject* BuildFontShader()
{
	auto* sh = shaderHandler->CreateProgramObject("[GLFontMVPCompare]", "font330");
	sh->AttachShaderObject(shaderHandler->CreateShaderObject(vsFont330, "", GL_VERTEX_SHADER));
	sh->AttachShaderObject(shaderHandler->CreateShaderObject(kFontFS330, "", GL_FRAGMENT_SHADER));
	sh->Link();
	return sh;
}

// 2x2 fully-white texture so the FS alpha (sampled .x) is 1 everywhere -> opaque
// glyph color, deterministic and non-empty regardless of uv.
GLuint MakeWhiteTex()
{
	const uint8_t px[2 * 2 * 4] = {
		255,255,255,255,  255,255,255,255,
		255,255,255,255,  255,255,255,255,
	};
	GLuint t = 0;
	glGenTextures(1, &t);
	glBindTexture(GL_TEXTURE_2D, t);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	return t;
}

// quad as two CCW triangles, single color, uv in texel space (s in [0,2]).
std::vector<VA_TYPE_TC> FlatQuadTC(float x0, float y0, float x1, float y1, SColor col)
{
	const VA_TYPE_TC tl{ {x0, y1, 0}, 0.0f, 2.0f, col };
	const VA_TYPE_TC tr{ {x1, y1, 0}, 2.0f, 2.0f, col };
	const VA_TYPE_TC br{ {x1, y0, 0}, 2.0f, 0.0f, col };
	const VA_TYPE_TC bl{ {x0, y0, 0}, 0.0f, 0.0f, col };
	return { bl, br, tr,  bl, tr, tl };
}

CMatrix44f OrthoMVP()
{
	return CMatrix44f::OrthoProj(0.0f, float(kSize), 0.0f, float(kSize), -1.0f, 1.0f);
}

void LoadFFMatrix(const CMatrix44f& mvp)
{
	glMatrixMode(GL_PROJECTION);
	glLoadMatrixf(static_cast<const float*>(mvp));
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
}

// Draw the quad through the font shader with the requested branch. For the legacy
// branch the MVP goes into the FF stack; for the modern branch the FF stack is left
// identity and the MVP is the uniform, so the two only agree if the toggle works.
void DrawFont(Shader::IProgramObject* sh, GLuint tex, bool useMVP,
              const CMatrix44f& mvp, const std::vector<VA_TYPE_TC>& quad)
{
	LoadFFMatrix(useMVP ? CMatrix44f() : mvp); // identity FF when on the uMVP branch

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, tex);

	auto& rb = RenderBuffer::GetTypedRenderBuffer<VA_TYPE_TC>();
	for (const VA_TYPE_TC& v : quad)
		rb.AddVertex(VA_TYPE_TC{v});

	sh->Enable();
	sh->SetUniform("tex", 0);
	sh->SetUniform("uUseMVP", useMVP ? 1 : 0);
	if (useMVP)
		sh->SetUniformMatrix4x4("uMVP", false, static_cast<const float*>(mvp));
	rb.DrawArrays(GL_TRIANGLES);
	sh->Disable();

	glBindTexture(GL_TEXTURE_2D, 0);
}

void RunCase(const CMatrix44f& mvp)
{
	RenderBuffer::InitStatic();

	Shader::IProgramObject* sh = BuildFontShader();
	REQUIRE(sh->IsValid());
	const GLuint tex = MakeWhiteTex();

	const SColor col(uint8_t(204), uint8_t(102), uint8_t(51), uint8_t(255));
	const auto quad = FlatQuadTC(32, 32, 200, 160, col);

	const auto legacy = RenderToBuffer([&] {
		ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
		DrawFont(sh, tex, /*useMVP=*/false, mvp, quad);
	});
	const auto modern = RenderToBuffer([&] {
		ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
		DrawFont(sh, tex, /*useMVP=*/true, mvp, quad);
	});

	const DiffResult d = Compare(legacy, modern);
	INFO("firstDiffByte=" << d.firstDiffByte << " maxAbsDelta=" << d.maxAbsDelta << " diffBytes=" << d.diffBytes);
	CHECK(d.maxAbsDelta <= 1);

	// guard against a both-empty false pass: the quad covers the FBO center
	// (128,128) in [32,200]x[32,160], so BOTH passes must show the glyph color.
	const size_t c = (size_t(kSize / 2) * kSize + kSize / 2) * 4;
	CHECK(legacy[c + 0] == 204); CHECK(legacy[c + 1] == 102); CHECK(legacy[c + 2] == 51);
	CHECK(modern[c + 0] == 204); CHECK(modern[c + 1] == 102); CHECK(modern[c + 2] == 51);

	glDeleteTextures(1, &tex);
	shaderHandler->ReleaseProgramObject("[GLFontMVPCompare]", "font330");
	RenderBuffer::KillStatic();
}

} // namespace


TEST_CASE("GLFontMVPCompare: font uMVP branch == gl_ModelViewProjectionMatrix branch (ortho)")
{
	if (!EnsureGL())
		SKIP("no usable OpenGL context (headless box); skipping GL harness");

	RunCase(OrthoMVP());
}

TEST_CASE("GLFontMVPCompare: font uMVP branch == builtin branch under a transform")
{
	if (!EnsureGL())
		SKIP("no usable OpenGL context (headless box); skipping GL harness");

	// ortho * translate * scale, to exercise the bridge MVP like the world/Lua
	// (gl.Translate/Scale around gl.Text) cases, not just a plain ortho.
	CMatrix44f m = OrthoMVP();
	m.Translate(20.0f, 12.0f, 0.0f);
	m.Scale(float3(1.25f, 0.8f, 1.0f));
	RunCase(m);
}
