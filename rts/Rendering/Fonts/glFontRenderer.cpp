#include "glFontRenderer.h"

#include "glFont.h"
#include "glFontRendererShaders.h"
#include "Rendering/GlobalRendering.h"
#include "Rendering/Shaders/Shader.h"
#include "System/Config/ConfigHandler.h"
#include "System/Log/ILog.h"
#include "System/Matrix44f.h"
#include "System/SafeUtil.h"

#include "System/Misc/TracyDefs.h"

#include <algorithm>
#include <cstdint>
#include <vector>


CONFIG(bool, FontUseMVPUniform).defaultValue(false).headlessValue(false).safemodeValue(false)
	.description("Font renderer transforms glyphs via a uniform mat4 (uMVP) sourced from the "
	             "fixed-function matrix bridge instead of gl_ModelViewProjectionMatrix. "
	             "Phase-1 modern-GL migration; off => legacy builtin path, byte-identical.");

CONFIG(bool, FontShaderMVPCompare).defaultValue(false).headlessValue(false).safemodeValue(false)
	.description("Validation: each font draw renders the glyph buffer both ways (uMVP vs "
	             "gl_ModelViewProjectionMatrix) into two FBOs in the SAME frame and logs the max "
	             "byte delta. Same-process A/B, immune to cross-run noise. Default off.");


////////////////////////////////////////
// Vertex-shader sources (vsFont330 / vsFont130) live in glFontRendererShaders.h
// so the modern-GL A/B test compiles the exact same source. The fragment shaders
// stay here.

static constexpr const char* fsFont330 = R"(
#version 150

uniform sampler2D tex;

in Data{
	vec4 vCol;
	vec2 vUV;
};

out vec4 outColor;

void main() {
	vec2 texSize = vec2(textureSize(tex, 0));

	float alpha = texture(tex, vUV / texSize).x;
	outColor = vec4(vCol.r, vCol.g, vCol.b, vCol.a * alpha);
}
)";

static constexpr const char* fsFontColor330 = R"(
#version 150

uniform sampler2D tex;

in Data{
	vec4 vCol;
	vec2 vUV;
};

out vec4 outColor;

void main() {
	vec2 texSize = vec2(textureSize(tex, 0));

	outColor = texture(tex, vUV / texSize);
	outColor = outColor*vCol;
}
)";


////////////////////////////////////////////

static constexpr const char* fsFont130 = R"(
#version 130

uniform sampler2D tex;

in vec4 vCol;
in vec2 vUV;

void main() {
	vec2 texSize = vec2(textureSize(tex, 0));

	float alpha = texture(tex, vUV / texSize).x;
	gl_FragColor = vec4(vCol.r, vCol.g, vCol.b, vCol.a * alpha);
}
)";
static constexpr const char* fsFontColor130 = R"(
#version 130

uniform sampler2D tex;

in vec4 vCol;
in vec2 vUV;

void main() {
	vec2 texSize = vec2(textureSize(tex, 0));

	float4 col = texture(tex, vUV / texSize);
	gl_FragColor = vCol*col;
}
)";

////////////////////////////////////////////

CglShaderFontRenderer::CglShaderFontRenderer()
{
	RECOIL_DETAILED_TRACY_ZONE;
	primaryBufferTC = TypedRenderBuffer<VA_TYPE_TC>(NUM_TRI_BUFFER_VERTS, NUM_TRI_BUFFER_ELEMS, IStreamBufferConcept::SB_BUFFERSUBDATA);
	outlineBufferTC = TypedRenderBuffer<VA_TYPE_TC>(NUM_TRI_BUFFER_VERTS, NUM_TRI_BUFFER_ELEMS, IStreamBufferConcept::SB_BUFFERSUBDATA);

	useMVPUniform = configHandler->GetBool("FontUseMVPUniform");
	mvpCompare    = configHandler->GetBool("FontShaderMVPCompare");

	++fontShaderRefs;

	if (fontShaderRefs > 1)
		return;

	// can't use shaderHandler here because it invalidates the objects on reload
	// but fonts are expected to be available all the time
	fontShader = std::make_unique<Shader::GLSLProgramObject>("[GL-Font]");
	fontShaderColor = std::make_unique<Shader::GLSLProgramObject>("[GL-Font]");

	LOG("[CglFont::%s] Creating Font shaders: GLAD_GL_ARB_explicit_attrib_location = %s", __func__, globalRendering->supportExplicitAttribLoc ? "true" : "false");
	if (globalRendering->supportExplicitAttribLoc) {
		fontShader->AttachShaderObject(new Shader::GLSLShaderObject(GL_VERTEX_SHADER  , vsFont330));
		fontShader->AttachShaderObject(new Shader::GLSLShaderObject(GL_FRAGMENT_SHADER, fsFont330));
		fontShaderColor->AttachShaderObject(new Shader::GLSLShaderObject(GL_VERTEX_SHADER  , vsFont330));
		fontShaderColor->AttachShaderObject(new Shader::GLSLShaderObject(GL_FRAGMENT_SHADER, fsFontColor330));
	}
	else {
		fontShader->AttachShaderObject(new Shader::GLSLShaderObject(GL_VERTEX_SHADER  , vsFont130));
		fontShader->AttachShaderObject(new Shader::GLSLShaderObject(GL_FRAGMENT_SHADER, fsFont130));
		fontShader->BindAttribLocation("pos", 0);
		fontShader->BindAttribLocation("uv" , 1);
		fontShader->BindAttribLocation("col", 2);
		fontShaderColor->AttachShaderObject(new Shader::GLSLShaderObject(GL_VERTEX_SHADER  , vsFont130));
		fontShaderColor->AttachShaderObject(new Shader::GLSLShaderObject(GL_FRAGMENT_SHADER, fsFontColor130));
		fontShaderColor->BindAttribLocation("pos", 0);
		fontShaderColor->BindAttribLocation("uv" , 1);
		fontShaderColor->BindAttribLocation("col", 2);

	}
	fontShader->Link();
	fontShader->Enable();
	fontShader->SetUniform("tex", 0);
	fontShader->SetUniform("uUseMVP", useMVPUniform ? 1 : 0);
	fontShader->Disable();
	fontShader->Validate();
	assert(fontShader->IsValid());

	fontShaderColor->Link();
	fontShaderColor->Enable();
	fontShaderColor->SetUniform("tex", 0);
	fontShaderColor->SetUniform("uUseMVP", useMVPUniform ? 1 : 0);
	fontShaderColor->Disable();
	fontShaderColor->Validate();
	assert(fontShaderColor->IsValid());
}

CglShaderFontRenderer::~CglShaderFontRenderer()
{
	RECOIL_DETAILED_TRACY_ZONE;
	--fontShaderRefs;
	if (fontShaderRefs > 0)
		return;

	fontShader = nullptr; // fontShader->Release() is called implicitly
	fontShaderColor = nullptr; // fontShader->Release() is called implicitly
}

void CglShaderFontRenderer::AddQuadTrianglesPB(VA_TYPE_TC&& tl, VA_TYPE_TC&& tr, VA_TYPE_TC&& br, VA_TYPE_TC&& bl)
{
	RECOIL_DETAILED_TRACY_ZONE;
	primaryBufferTC.AddQuadTriangles(std::move(tl), std::move(tr), std::move(br), std::move(bl));
}

void CglShaderFontRenderer::AddQuadTrianglesOB(VA_TYPE_TC&& tl, VA_TYPE_TC&& tr, VA_TYPE_TC&& br, VA_TYPE_TC&& bl)
{
	RECOIL_DETAILED_TRACY_ZONE;
	outlineBufferTC.AddQuadTriangles(std::move(tl), std::move(tr), std::move(br), std::move(bl));
}

void CglShaderFontRenderer::DrawTraingleElements()
{
	RECOIL_DETAILED_TRACY_ZONE;

	if (mvpCompare && boundFontShader != nullptr &&
	    (primaryBufferTC.SumIndcs() + outlineBufferTC.SumIndcs()) > 0)
		CompareMVPDraws();

	outlineBufferTC.DrawElements(GL_TRIANGLES);
	primaryBufferTC.DrawElements(GL_TRIANGLES);
}

void CglShaderFontRenderer::HandleTextureUpdate(CFontTexture& fnt, bool onlyUpload)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (!onlyUpload)
		fnt.UpdateGlyphAtlasTexture();

	GLint dl = 0;
	glGetIntegerv(GL_LIST_INDEX, &dl);
	if (dl == 0) {
		fnt.UploadGlyphAtlasTextureImpl();
	}
}

// Phase-0 bridge: read the current MVP straight off the fixed-function stack (a
// query, not a deprecated set-call), mirroring LuaOpenGL's GetCurrentFixedFunctionMVP.
// At PushGLState time every transform is already composed on the FF stack -- screen
// (SetupScreenMatrices), world (DrawWorldBuffered's GetBillBoardMatrix multiply),
// minimap, plus any Lua gl.Translate/Scale around gl.Text -- so this is the exact MVP
// the gl_ModelViewProjectionMatrix builtin would use, i.e. byte-identical by
// construction. (Swapping this for a CPU/UBO source is the follow-up that makes the
// font FF-independent and unblocks Phase 0.)
static CMatrix44f GetCurrentFixedFunctionFontMVP()
{
	CMatrix44f proj;
	CMatrix44f modelView;
	glGetFloatv(GL_PROJECTION_MATRIX, static_cast<float*>(proj));
	glGetFloatv(GL_MODELVIEW_MATRIX, static_cast<float*>(modelView));
	return proj * modelView;
}

void CglShaderFontRenderer::PushGLState(const CglFont& fnt)
{
	RECOIL_DETAILED_TRACY_ZONE;
	glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_ALPHA_TEST); //just in case
	glEnable(GL_BLEND);
	if (!userDefinedBlending)
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	glBindTexture(GL_TEXTURE_2D, fnt.GetTexture());

	glGetIntegerv(GL_CURRENT_PROGRAM, &currProgID);

	Shader::IProgramObject* shader = fnt.HasColor() ? fontShaderColor.get() : fontShader.get();
	shader->Enable();
	boundFontShader = shader;

	// set the branch toggle every draw (not just at ctor) so useMVPUniform can be
	// flipped at runtime, e.g. by the whole-frame A/B compare.
	shader->SetUniform("uUseMVP", useMVPUniform ? 1 : 0);
	if (useMVPUniform) {
		const CMatrix44f mvp = GetCurrentFixedFunctionFontMVP();
		shader->SetUniformMatrix4x4("uMVP", false, static_cast<const float*>(mvp));
	}
}

// Same-frame A/B (config FontShaderMVPCompare): render the accumulated glyph
// buffers two ways IN ONE FRAME -- the legacy gl_ModelViewProjectionMatrix branch
// vs the uMVP branch fed by the same bridge matrix -- into two FBOs and log the max
// byte delta. Immune to the cross-run/temporal noise that defeats a screenshot A/B,
// since only the shader branch differs. Verified: a builtin-vs-builtin control gives
// the same (near-zero) result, so any reported delta is a real branch difference.
//
// Three things are required for a trustworthy result, each learned from a control
// that diverged without them:
//   1. Each branch draws through its OWN scratch buffer objects (cmpA*/cmpB*), filled
//      fresh from the production glyph buffers -- drawing one engine streaming buffer
//      twice per frame does not reproduce.
//   2. Fresh FBOs per compare (not reused across the many font draws per frame).
//   3. The FULL view size (not the current scissored viewport), so the full-screen
//      glyph geometry isn't squished into heavy translucent overlap whose blend a
//      threaded rasterizer renders non-deterministically.
// The shader is already Enabled and the font texture bound by PushGLState.
void CglShaderFontRenderer::CompareMVPDraws()
{
	RECOIL_DETAILED_TRACY_ZONE;

	// Use the full view size, NOT the current (possibly scissored/clamped) GL
	// viewport: the glyph geometry is in the MVP's full coordinate space, so a small
	// viewport would squish all of it into a few rows -> heavy translucent overlap
	// whose blended result a threaded rasterizer renders non-deterministically
	// (a builtin-vs-builtin control diverged at viewport 893x29).
	const int w = globalRendering->viewSizeX;
	const int h = globalRendering->viewSizeY;
	if (w <= 0 || h <= 0)
		return;

	const CMatrix44f mvp = GetCurrentFixedFunctionFontMVP();
	Shader::IProgramObject* sh = boundFontShader;

	// one outline + one primary scratch buffer PER branch, each drawn once per
	// frame; sized like the real font buffers, lazily built on first use.
	static TypedRenderBuffer<VA_TYPE_TC> cmpA_OL(NUM_TRI_BUFFER_VERTS, NUM_TRI_BUFFER_ELEMS, IStreamBufferConcept::SB_BUFFERSUBDATA);
	static TypedRenderBuffer<VA_TYPE_TC> cmpA_PM(NUM_TRI_BUFFER_VERTS, NUM_TRI_BUFFER_ELEMS, IStreamBufferConcept::SB_BUFFERSUBDATA);
	static TypedRenderBuffer<VA_TYPE_TC> cmpB_OL(NUM_TRI_BUFFER_VERTS, NUM_TRI_BUFFER_ELEMS, IStreamBufferConcept::SB_BUFFERSUBDATA);
	static TypedRenderBuffer<VA_TYPE_TC> cmpB_PM(NUM_TRI_BUFFER_VERTS, NUM_TRI_BUFFER_ELEMS, IStreamBufferConcept::SB_BUFFERSUBDATA);

	const auto drawBranch = [&](TypedRenderBuffer<VA_TYPE_TC>& ol, TypedRenderBuffer<VA_TYPE_TC>& pm, bool useMVP) {
		ol.Clear();
		ol.AddVertices(outlineBufferTC.GetElems());
		ol.AddIndices(outlineBufferTC.GetIndcs());
		pm.Clear();
		pm.AddVertices(primaryBufferTC.GetElems());
		pm.AddIndices(primaryBufferTC.GetIndcs());

		sh->SetUniform("uUseMVP", useMVP ? 1 : 0);
		if (useMVP)
			sh->SetUniformMatrix4x4("uMVP", false, static_cast<const float*>(mvp));

		ol.DrawElements(GL_TRIANGLES);
		pm.DrawElements(GL_TRIANGLES);
	};

	// Render each branch into its OWN fresh FBO (created+destroyed per compare).
	// Reusing static FBOs across the many font draws per frame gave false
	// divergences here (a clean-FBO control showed the two branches byte-identical);
	// fresh FBOs + glFinish are deterministic.
	GLuint fbos[2] = {0, 0}, texs[2] = {0, 0}, drb = 0;
	glGenRenderbuffers(1, &drb);
	glBindRenderbuffer(GL_RENDERBUFFER, drb);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
	for (int i = 0; i < 2; ++i) {
		glGenTextures(1, &texs[i]);
		glBindTexture(GL_TEXTURE_2D, texs[i]);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glGenFramebuffers(1, &fbos[i]);
		glBindFramebuffer(GL_FRAMEBUFFER, fbos[i]);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texs[i], 0);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, drb);
	}

	int maxDelta = -1;
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
		GLint prevFBO = 0; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);
		GLint prevVP[4]; glGetIntegerv(GL_VIEWPORT, prevVP);
		glViewport(0, 0, w, h);
		glPixelStorei(GL_PACK_ALIGNMENT, 1);

		std::vector<uint8_t> px[2] = { std::vector<uint8_t>(size_t(w) * h * 4), std::vector<uint8_t>(size_t(w) * h * 4) };
		for (int i = 0; i < 2; ++i) {
			glBindFramebuffer(GL_FRAMEBUFFER, fbos[i]);
			glClearColor(0, 0, 0, 0);
			glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
			drawBranch(i == 0 ? cmpA_OL : cmpB_OL, i == 0 ? cmpA_PM : cmpB_PM, i != 0);
			glFinish();
			glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px[i].data());
		}
		glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFBO));
		glViewport(prevVP[0], prevVP[1], prevVP[2], prevVP[3]);

		maxDelta = 0;
		for (size_t k = 0; k < px[0].size(); ++k)
			maxDelta = std::max(maxDelta, std::abs(int(px[0][k]) - int(px[1][k])));
	}
	glDeleteFramebuffers(2, fbos); glDeleteTextures(2, texs); glDeleteRenderbuffers(1, &drb);

	// restore the uniform state the real (visible) draw expects
	sh->SetUniform("uUseMVP", useMVPUniform ? 1 : 0);
	if (useMVPUniform)
		sh->SetUniformMatrix4x4("uMVP", false, static_cast<const float*>(mvp));

	// log once on success; always on FBO failure or a real divergence (>1 LSB)
	static int logged = 0;
	if (maxDelta < 0) {
		if (logged++ == 0)
			LOG_L(L_WARNING, "[Font MVP compare] compare FBO unavailable");
	} else if (maxDelta > 1) {
		LOG_L(L_WARNING, "[Font MVP compare] uMVP path diverged from builtin: max byte delta = %d", maxDelta);
	} else if (logged++ == 0) {
		LOG("[Font MVP compare] uMVP path matches builtin: max byte delta = %d", maxDelta);
	}
}

void CglShaderFontRenderer::PopGLState(const CglFont& fnt)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (fnt.HasColor())
		fontShaderColor->Disable();
	else
		fontShader->Disable();

	if (currProgID > 0)
		glUseProgram(currProgID);

	glBindTexture(GL_TEXTURE_2D, 0);

	glPopAttrib();
}

void CglShaderFontRenderer::GetStats(std::array<size_t, 8>& stats) const
{
	RECOIL_DETAILED_TRACY_ZONE;
	stats[0 + 0] = primaryBufferTC.SumElems();
	stats[0 + 1] = primaryBufferTC.SumIndcs();
	stats[0 + 2] = primaryBufferTC.NumSubmits(false);
	stats[0 + 3] = primaryBufferTC.NumSubmits(true);

	stats[4 + 0] = outlineBufferTC.SumElems();
	stats[4 + 1] = outlineBufferTC.SumIndcs();
	stats[4 + 2] = outlineBufferTC.NumSubmits(false);
	stats[4 + 3] = outlineBufferTC.NumSubmits(true);
}

CglNoShaderFontRenderer::CglNoShaderFontRenderer()
{
	RECOIL_DETAILED_TRACY_ZONE;
	for (auto& v : verts)
		v.reserve(NUM_TRI_BUFFER_VERTS);
	for (auto& i : indcs)
		i.reserve(NUM_TRI_BUFFER_ELEMS);

	textureSpaceMatrix = glGenLists(1);
	glNewList(textureSpaceMatrix, GL_COMPILE);
	glEndList();
}

CglNoShaderFontRenderer::~CglNoShaderFontRenderer()
{
	RECOIL_DETAILED_TRACY_ZONE;
	glDeleteLists(textureSpaceMatrix, 1);
}

void CglNoShaderFontRenderer::AddQuadTrianglesImpl(bool primary, VA_TYPE_TC&& tl, VA_TYPE_TC&& tr, VA_TYPE_TC&& br, VA_TYPE_TC&& bl)
{
	RECOIL_DETAILED_TRACY_ZONE;
	auto& v = verts[primary];
	auto& i = indcs[primary];

	const uint16_t baseIndex = static_cast<uint16_t>(v.size());

	v.emplace_back(std::move(tl)); //0
	v.emplace_back(std::move(tr)); //1
	v.emplace_back(std::move(br)); //2
	v.emplace_back(std::move(bl)); //3

	//triangle 1 {tl, tr, bl}
	i.emplace_back(baseIndex + 3);
	i.emplace_back(baseIndex + 0);
	i.emplace_back(baseIndex + 1);

	//triangle 2 {bl, tr, br}
	i.emplace_back(baseIndex + 3);
	i.emplace_back(baseIndex + 1);
	i.emplace_back(baseIndex + 2);
}

void CglNoShaderFontRenderer::AddQuadTrianglesPB(VA_TYPE_TC&& tl, VA_TYPE_TC&& tr, VA_TYPE_TC&& br, VA_TYPE_TC&& bl)
{
	RECOIL_DETAILED_TRACY_ZONE;
	AddQuadTrianglesImpl(true , std::move(tl), std::move(tr), std::move(br), std::move(bl));
}

void CglNoShaderFontRenderer::AddQuadTrianglesOB(VA_TYPE_TC&& tl, VA_TYPE_TC&& tr, VA_TYPE_TC&& br, VA_TYPE_TC&& bl)
{
	RECOIL_DETAILED_TRACY_ZONE;
	AddQuadTrianglesImpl(false, std::move(tl), std::move(tr), std::move(br), std::move(bl));
}

void CglNoShaderFontRenderer::DrawTraingleElements()
{
	RECOIL_DETAILED_TRACY_ZONE;
	static constexpr GLsizei stride = sizeof(VA_TYPE_TC);

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

	for (size_t idx = 0; idx < 2; ++idx) {
		glVertexPointer(3, GL_FLOAT, stride, &verts[idx].data()->pos);
		glTexCoordPointer(2, GL_FLOAT, stride, &verts[idx].data()->s);
		glColorPointer(4, GL_UNSIGNED_BYTE, stride, &verts[idx].data()->c.r);
		glDrawRangeElements(GL_TRIANGLES, 0, verts[idx].size() - 1, indcs[idx].size(), GL_UNSIGNED_SHORT, indcs[idx].data());
	};

	for (auto& v : verts)
		v.clear();
	for (auto& i : indcs)
		i.clear();
}

void CglNoShaderFontRenderer::HandleTextureUpdate(CFontTexture& fnt, bool onlyUpload)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (!onlyUpload)
		fnt.UpdateGlyphAtlasTexture();

	GLint dl = 0;
	glGetIntegerv(GL_LIST_INDEX, &dl);
	if (dl == 0) {
		fnt.UploadGlyphAtlasTextureImpl();

		// update texture space dlist (this affects already compiled dlists too!)
		glNewList(textureSpaceMatrix, GL_COMPILE);
		glScalef(1.0f / fnt.GetTextureWidth(), 1.0f / fnt.GetTextureHeight(), 1.0f);
		glEndList();
	}
}

void CglNoShaderFontRenderer::PushGLState(const CglFont& fnt)
{
	RECOIL_DETAILED_TRACY_ZONE;
	glPushAttrib(GL_ENABLE_BIT | GL_CURRENT_BIT);
	glDisable(GL_LIGHTING);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_ALPHA_TEST);
	glEnable(GL_BLEND);
	if (!userDefinedBlending)
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glEnable(GL_TEXTURE_2D);

	glMatrixMode(GL_TEXTURE);
	glPushMatrix();
	glCallList(textureSpaceMatrix);

	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glEnableClientState(GL_COLOR_ARRAY);

	glBindTexture(GL_TEXTURE_2D, fnt.GetTexture());
}

void CglNoShaderFontRenderer::PopGLState(const CglFont& fnt)
{
	RECOIL_DETAILED_TRACY_ZONE;
	glBindTexture(GL_TEXTURE_2D, 0);

	glDisableClientState(GL_VERTEX_ARRAY);
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	glDisableClientState(GL_COLOR_ARRAY);

	glPopMatrix();
	glMatrixMode(GL_MODELVIEW);

	glDisable(GL_TEXTURE_2D);
	glPopAttrib();
}

void CglNoShaderFontRenderer::GetStats(std::array<size_t, 8>& stats) const
{
	RECOIL_DETAILED_TRACY_ZONE;
	/// placeholder
	std::fill(stats.begin(), stats.end(), 0);
}


std::unique_ptr<CglFontRenderer> CglFontRenderer::CreateInstance()
{
	RECOIL_DETAILED_TRACY_ZONE;
#ifndef HEADLESS
	//return std::make_unique<CglNoShaderFontRenderer>();
	if (globalRendering->amdHacks)
		return std::make_unique<CglNoShaderFontRenderer>();

	auto fr = std::make_unique<CglShaderFontRenderer>();
	if (fr->IsValid())
		return fr;

	fr = nullptr;
	return std::make_unique<CglNoShaderFontRenderer>();
#else
	return std::make_unique<CglNullFontRenderer>();
#endif
}

void CglFontRenderer::DeleteInstance(std::unique_ptr<CglFontRenderer>& instance)
{
	RECOIL_DETAILED_TRACY_ZONE;
	instance = nullptr;
}

void CglNullFontRenderer::GetStats(std::array<size_t, 8>& stats) const
{
	RECOIL_DETAILED_TRACY_ZONE;
	std::fill(stats.begin(), stats.end(), 0u);
}
