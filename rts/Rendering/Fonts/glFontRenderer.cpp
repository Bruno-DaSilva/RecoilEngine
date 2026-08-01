#include "glFontRenderer.h"
#include "Rendering/GL/FFMatrixTracking.h"

#include "glFont.h"
#include "glFontRendererShaders.h"
#include "Rendering/GlobalRendering.h"
#include "Rendering/GL/AttribStateVerify.h"
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

	// The texel->UV texture-space matrix list is NOT created here: see the
	// declaration. It is only reachable from the recordable flush, and creating
	// it up front spends a glGenLists/glNewList/glEndList in every run including
	// the ones that compile no display list at all.

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
	if (ffTextureSpaceMatrix != 0)
		glDeleteLists(ffTextureSpaceMatrix, 1);

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

	// inside a display-list compile the RenderBuffer flush is not list-safe
	// (stream-VBO offset aliasing; glUseProgram/glUniform get recorded into the
	// list instead of executing) -- emit recordable fixed-function immediate mode
	GLint dl = 0;
	glGetIntegerv(GL_LIST_INDEX, &dl);
	if (dl != 0) {
		DrawTraingleElementsRecordable();
		return;
	}

	outlineBufferTC.DrawElements(GL_TRIANGLES);
	primaryBufferTC.DrawElements(GL_TRIANGLES);
}

void CglShaderFontRenderer::DrawTraingleElementsRecordable()
{
	RECOIL_DETAILED_TRACY_ZONE;

	// glyph verts store texel coords (the shader divides by textureSize()); scale
	// through the shared texture-space-matrix list so atlas resizes keep working
	glMatrixMode(GL_TEXTURE);
	glPushMatrix();
	wantTextureSpaceMatrix = true;
	if (ffTextureSpaceMatrix != 0) {
		glCallList(ffTextureSpaceMatrix);
	} else if (texMatListW > 0 && texMatListH > 0) {
		// First recordable flush of the run: the shared list cannot be created
		// from here (glGenLists/glNewList are illegal inside the compile this
		// runs in), so bake the scale into THIS list and let the next
		// HandleTextureUpdate build the shared one for every flush after. Only
		// this one list stops tracking atlas resizes, and only until whatever
		// rebuilds it does so.
		glScalef(1.0f / texMatListW, 1.0f / texMatListH, 1.0f);
	}
	glMatrixMode(GL_MODELVIEW);

	// FF sampling; inside PushGLState's glPushAttrib(GL_ENABLE_BIT) bracket
	glEnable(GL_TEXTURE_2D);

	// AddQuadTriangles stores each quad as 4 consecutive verts {tl, tr, br, bl}
	for (auto* buffer : { &outlineBufferTC, &primaryBufferTC }) {
		const auto& verts = buffer->GetElems();

		if (!verts.empty()) {
			glBegin(GL_QUADS);
			for (const auto& v : verts) {
				GL::ffColor.SetUB(v.c.r, v.c.g, v.c.b, v.c.a);
				glTexCoord2f(v.s, v.t);
				glVertex3f(v.pos.x, v.pos.y, v.pos.z);
			}
			glEnd();
		}

		buffer->Clear();
	}

	glDisable(GL_TEXTURE_2D);

	glMatrixMode(GL_TEXTURE);
	glPopMatrix();
	glMatrixMode(GL_MODELVIEW);
}

void CglShaderFontRenderer::HandleTextureUpdate(CFontTexture& fnt, bool onlyUpload)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (!onlyUpload)
		fnt.UpdateGlyphAtlasTexture();

	GLint dl = 0;
	glGetIntegerv(GL_LIST_INDEX, &dl);
	if (dl != 0)
		return;

	fnt.UploadGlyphAtlasTextureImpl();

	const int w = (int)fnt.GetTextureWidth();
	const int h = (int)fnt.GetTextureHeight();
	const bool resized = (texMatListW != w || texMatListH != h);

	texMatListW = w;
	texMatListH = h;

	// Nothing has needed the recordable flush yet, so the list does not exist
	// and must not be brought into being: this is the only place that CAN create
	// it (outside any compile), but doing so unconditionally is what kept
	// glGenLists/glNewList/glEndList alive in runs with no display lists at all.
	if (!wantTextureSpaceMatrix)
		return;

	const bool created = (ffTextureSpaceMatrix == 0);
	if (created)
		ffTextureSpaceMatrix = glGenLists(1);

	// keep the recordable-flush texture-space matrix in sync with the atlas size
	// (this affects already compiled dlists too, like the no-shader renderer's);
	// only recompile when the size actually changed -- unconditional recompiles
	// were ~190 glNewList calls per frame of BAR UI
	if (resized || created) {
		glNewList(ffTextureSpaceMatrix, GL_COMPILE);
		glScalef(1.0f / w, 1.0f / h, 1.0f);
		glEndList();
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
	GL::ReadFFMatrices(proj, modelView);
	return proj * modelView;
}

void CglShaderFontRenderer::PushGLState(const CglFont& fnt)
{
	RECOIL_DETAILED_TRACY_ZONE;
	// Explicit save/restore instead of glPushAttrib(GL_ENABLE_BIT |
	// GL_COLOR_BUFFER_BIT): the attrib stack is on RenderDoc's unsupported list
	// and this bracket was its single busiest user (~66k pairs/run). The
	// queries below are not on that list, so reading the state costs nothing.
	//
	// Saving only what the bracket writes is equivalent to the wide push only if
	// nothing between here and PopGLState leaves another enable or colour-buffer
	// state changed. GL_TEXTURE_2D is exactly such a case and cost a gate
	// failure: DrawTraingleElementsRecordable enables it and ends by DISABLING
	// it, so the wide pop used to put back whatever the caller had, while a
	// narrower save left texturing off for the next draw (1 frame per run, max
	// delta 73 -- intermittent because it needs a display-list compile).
	GL::ShadowPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT);
	savedState.depthTest = glIsEnabled(GL_DEPTH_TEST);
	savedState.texture2D = glIsEnabled(GL_TEXTURE_2D);
	savedState.alphaTest = glIsEnabled(GL_ALPHA_TEST);
	savedState.blend     = glIsEnabled(GL_BLEND);
	glGetIntegerv(GL_BLEND_SRC_RGB,   &savedState.blendSrcRGB);
	glGetIntegerv(GL_BLEND_DST_RGB,   &savedState.blendDstRGB);
	glGetIntegerv(GL_BLEND_SRC_ALPHA, &savedState.blendSrcAlpha);
	glGetIntegerv(GL_BLEND_DST_ALPHA, &savedState.blendDstAlpha);

	glDisable(GL_DEPTH_TEST);
	glDisable(GL_ALPHA_TEST); //just in case
	glEnable(GL_BLEND);
	if (!userDefinedBlending)
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	glBindTexture(GL_TEXTURE_2D, fnt.GetTexture());

	// inside a display-list compile glUseProgram/glUniform* are RECORDED into the
	// list, not executed. Issuing them here would (a) stomp shader state whenever
	// the list is replayed and (b) desync the program's CPU-side uniform cache from
	// the GPU: the cache updates but the GL call never executes, so later
	// "redundant" uUseMVP writes are silently skipped and direct legacy text draws
	// run the stale uMVP branch with another draw's matrix (text vanishes; found by
	// the whole-frame A/B gate on a BAR replay pregame). The list-compile flush is
	// recordable fixed-function (DrawTraingleElementsRecordable) and needs no
	// shader at all, so skip the entire program path.
	GLint dl = 0;
	glGetIntegerv(GL_LIST_INDEX, &dl);
	inListCompile = (dl != 0);
	if (inListCompile)
		return;

	glGetIntegerv(GL_CURRENT_PROGRAM, &currProgID);

	Shader::IProgramObject* shader = fnt.HasColor() ? fontShaderColor.get() : fontShader.get();
	shader->Enable();

	// set the branch toggle every draw (not just at ctor) so useMVPUniform can be
	// flipped at runtime, e.g. by the whole-frame A/B compare.
	shader->SetUniform("uUseMVP", useMVPUniform ? 1 : 0);
	if (useMVPUniform) {
		const CMatrix44f mvp = GetCurrentFixedFunctionFontMVP();
		shader->SetUniformMatrix4x4("uMVP", false, static_cast<const float*>(mvp));
	}
}

void CglShaderFontRenderer::PopGLState(const CglFont& fnt)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (!inListCompile) {
		if (fnt.HasColor())
			fontShaderColor->Disable();
		else
			fontShader->Disable();

		if (currProgID > 0)
			glUseProgram(currProgID);
	}
	inListCompile = false;

	glBindTexture(GL_TEXTURE_2D, 0);

	// mirror of the save in PushGLState
	if (savedState.depthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
	if (savedState.texture2D) glEnable(GL_TEXTURE_2D); else glDisable(GL_TEXTURE_2D);
	if (savedState.alphaTest) glEnable(GL_ALPHA_TEST); else glDisable(GL_ALPHA_TEST);
	if (savedState.blend)     glEnable(GL_BLEND);      else glDisable(GL_BLEND);
	glBlendFuncSeparate(savedState.blendSrcRGB, savedState.blendDstRGB,
	                    savedState.blendSrcAlpha, savedState.blendDstAlpha);
	GL::VerifyAttribRestore("CglShaderFontRenderer::PopGLState");
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

		// update texture space dlist (this affects already compiled dlists too!);
		// only when the atlas size changed, see CglShaderFontRenderer
		if (texMatListW != (int)fnt.GetTextureWidth() || texMatListH != (int)fnt.GetTextureHeight()) {
			texMatListW = (int)fnt.GetTextureWidth();
			texMatListH = (int)fnt.GetTextureHeight();
			glNewList(textureSpaceMatrix, GL_COMPILE);
			glScalef(1.0f / fnt.GetTextureWidth(), 1.0f / fnt.GetTextureHeight(), 1.0f);
			glEndList();
		}
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
