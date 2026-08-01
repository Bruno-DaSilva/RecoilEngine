/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "3DModelVAO.hpp"

#include <algorithm>
#include <array>
#include <iterator>
#include <string>
#include <unordered_set>

#include "3DModel.hpp"
#include "3DModelPiece.hpp"
#include "IModelParser.h"
#include "Rendering/GlobalRendering.h"
#include "Rendering/ModelsDataUploader.h"
#include "Rendering/GL/FFStateTracker.h"
#include "Rendering/Shaders/Shader.h"
#include "Rendering/Shaders/ShaderHandler.h"
#include "Sim/Units/Unit.h"
#include "System/Config/ConfigHandler.h"
#include "Sim/Units/UnitDef.h"
#include "Sim/Features/Feature.h"

#include "System/Log/ILog.h"

#include "System/Misc/TracyDefs.h"


CONFIG(bool, BindLegacyModelTexUnits).defaultValue(false)
	.description("Bind fixed-function texture-coordinate units 1/5/6 (uv1, tangent, bitangent) "
	             "on the legacy model draw path (gl.UnitShape, ghosted buildings, AI units, model "
	             "projectiles). Off by default: BAR does not read those channels there, and binding "
	             "them costs the process its RenderDoc capture. Enable if a game's model shaders "
	             "read gl_MultiTexCoord1/5/6 on that path without guarding for absent tangents.");

// Function-local so the read happens on first draw, not during static init --
// configHandler does not exist yet at namespace-scope initialisation time, and
// reading it there segfaults before the window is even up.
CONFIG(bool, ModernModelAttribs).defaultValue(false)
	.description("Feed the legacy model draw path (gl.UnitShape, ghosted buildings, AI units, "
	             "model projectiles) from the modern VAO's generic vertex attributes instead of "
	             "fixed-function client arrays, and compile the engine model shader to read them. "
	             "Off by default: a game whose own model shaders read gl_Vertex/gl_Normal/"
	             "gl_MultiTexCoord0 needs those channels. On, it removes five unsupported "
	             "functions from the frame.");

bool GL::ModernModelAttribs()
{
	// function-local: configHandler does not exist during static init
	static const bool b = configHandler->GetBool("ModernModelAttribs");
	return b;
}

static bool BindLegacyTexUnits()
{
	static const bool b = configHandler->GetBool("BindLegacyModelTexUnits");
	return b;
}

CONFIG(bool, ModernModelFFShader).defaultValue(false)
	.description("On the legacy model draw path, when the caller bound no program of its own, "
	             "draw through an engine fixed-function-equivalent shader and the modern VAO "
	             "instead of fixed-function client arrays. Requires ModernModelAttribs. Off by "
	             "default; on, it removes the last four client-array functions from the frame. "
	             "Only taken when the live fixed-function state is one the shader reproduces "
	             "exactly -- anything else still falls back, per draw.");

namespace {
	bool FFModelShaderEnabled()
	{
		static const bool b = configHandler->GetBool("ModernModelFFShader");
		return b;
	}

	// A model draw with no program bound is drawn by fixed function, and the
	// ONLY reason the client-array family is still in a BAR frame is to feed it
	// (gl.UnitShape's rawState defaults to true; the reachable site is
	// unit_icongenerator's offscreen atlas). Fixed function with lighting off is
	// just `texture * current colour`, which is a four-line shader -- so the
	// draw can move to the modern VAO and the client arrays go with it.
	//
	// Deliberately NOT a general fixed-function emulator. It reproduces one
	// state, the caller checks that state is the live one (FFStateReproducible),
	// and everything else keeps the old path. That is what makes it safe to
	// enable for a game nobody has updated: the fallback is per DRAW, not per
	// run, so the worst case is that a game keeps exactly today's behaviour.
	std::string MakeFFModelVertexSrc(bool explicitAttribLoc, bool fogged)
	{
		// gl_ModelViewProjectionMatrix, not a CPU-composed uniform: the driver
		// composes P*MV itself so the transform is exact for any matrices, and
		// S3DModelPiece::DrawStaticLegacy pushes a fresh matrix per piece
		// between draws. Reading a builtin is not a GL call and costs nothing at
		// the capture gate -- only the FF matrix SET-calls do, and they are a
		// separate group. Attribute locations are the modern model VAO's:
		// 0 = position, 4 = texCoords[0] as a vec4 (uv0 in .xy, uv1 in .zw).
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
		s += "gl_Position = gl_ModelViewProjectionMatrix * vec4(apos, 1.0); }\n";
		return s;
	}

	std::string MakeFFModelFragmentSrc(bool fogged)
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

	Shader::IProgramObject* GetFFModelShader(bool fogged)
	{
		const char* poName = fogged ? "FFModelFog" : "FFModel";

		Shader::IProgramObject* shader = shaderHandler->GetProgramObject("[S3DModelVAO]", poName);
		if (shader != nullptr && shader->IsValid())
			return shader;

		const bool eal = globalRendering->supportExplicitAttribLoc;

		shader = shaderHandler->CreateProgramObject("[S3DModelVAO]", poName);
		shader->AttachShaderObject(shaderHandler->CreateShaderObject(MakeFFModelVertexSrc(eal, fogged), "", GL_VERTEX_SHADER));
		shader->AttachShaderObject(shaderHandler->CreateShaderObject(MakeFFModelFragmentSrc(fogged), "", GL_FRAGMENT_SHADER));

		if (!eal) {
			shader->BindAttribLocation("apos", 0);
			shader->BindAttribLocation("auv", 4);
		}

		shader->Link();
		return shader;
	}

	// Is the live fixed-function state one the program above reproduces exactly?
	// Every query here is a glGet/glIsEnabled, all of which RenderDoc supports;
	// the calls being replaced are the ones it does not.
	// Returns nullptr when reproducible, else the reason it is not -- a rejected
	// draw falls back silently and reads exactly like a converted one, so the
	// reason has to be reportable rather than inferable.
	const char* FFStateReproducible(bool& textured, bool& fogged)
	{
		// Lighting is computed by fixed function for a fixed-function draw and
		// by nobody at all for a shader-bound one, so the shader would silently
		// drop it.
		if (glIsEnabled(GL_LIGHTING) == GL_TRUE)
			return "GL_LIGHTING enabled";

		// Fixed function fogs its own draws; a shader-bound draw gets none
		// unless the shader computes it. The fogged variant does, for LINEAR
		// only -- the mode the engine sets -- and reads GL_FOG_COORD_SRC rather
		// than assuming it, since an explicit fog coordinate is not |eye z|.
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

		// Any other enabled target on unit 0 outranks or joins GL_TEXTURE_2D and
		// the single sampler cannot stand in for it.
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
			// GL_MODULATE against the same current colour still differs from
			// fixed function by a little: measured under FFExperiment 6 with
			// ab_unitshape_driver.lua's rawState row texturing half its shapes,
			// 15 px a frame at delta 3, every frame. Small and consistent, which
			// suggests a per-edge or per-texel effect rather than a wrong
			// transform -- but the bar here is zero, and the untextured case
			// below is at zero, so ship that and leave this measured.
			return "textured fixed-function draw (substitute not yet pixel-exact)";
		}

		// A second enabled unit would combine into the fragment as well. Units
		// 1/5/6 are the ones this path ever binds (see BindLegacyTexUnits), and
		// checking a few beyond them costs nothing.
		bool otherUnitEnabled = false;
		for (GLenum unit = GL_TEXTURE1; unit <= GL_TEXTURE7 && !otherUnitEnabled; ++unit) {
			glActiveTexture(unit);
			otherUnitEnabled = (glIsEnabled(GL_TEXTURE_2D)       == GL_TRUE ||
			                    glIsEnabled(GL_TEXTURE_1D)       == GL_TRUE ||
			                    glIsEnabled(GL_TEXTURE_3D)       == GL_TRUE ||
			                    glIsEnabled(GL_TEXTURE_CUBE_MAP) == GL_TRUE ||
			                    glIsEnabled(GL_TEXTURE_RECTANGLE) == GL_TRUE);
		}
		glActiveTexture(GL_TEXTURE0);

		if (otherUnitEnabled)
			return "a second texture unit is enabled";

		return nullptr;
	}
}

void S3DModelVAO::EnableAttribs(bool inst) const
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (!inst) {
		for (int i = 0; i <= 5; ++i) {
			glEnableVertexAttribArray(i);
			glVertexAttribDivisor(i, 0);
		}

		glVertexAttribPointer (0, 3, GL_FLOAT       , false, sizeof(SVertexData), (const void*)offsetof(SVertexData, pos         ));
		glVertexAttribPointer (1, 3, GL_FLOAT       , false, sizeof(SVertexData), (const void*)offsetof(SVertexData, normal      ));
		glVertexAttribPointer (2, 3, GL_FLOAT       , false, sizeof(SVertexData), (const void*)offsetof(SVertexData, sTangent    ));
		glVertexAttribPointer (3, 3, GL_FLOAT       , false, sizeof(SVertexData), (const void*)offsetof(SVertexData, tTangent    ));
		glVertexAttribPointer (4, 4, GL_FLOAT       , false, sizeof(SVertexData), (const void*)offsetof(SVertexData, texCoords[0]));
		glVertexAttribIPointer(5, 3, GL_UNSIGNED_INT,        sizeof(SVertexData), (const void*)offsetof(SVertexData, boneIDsLow  ));
	}
	else {
		for (int i = 6; i <= 6; ++i) {
			glEnableVertexAttribArray(i);
			glVertexAttribDivisor(i, 1);
		}

		// covers all 4 uints of SInstanceData
		glVertexAttribIPointer(6, 4, GL_UNSIGNED_INT, sizeof(SInstanceData), (const void*)offsetof(SInstanceData, matOffset));
	}
}

void S3DModelVAO::DisableAttribs() const
{
	RECOIL_DETAILED_TRACY_ZONE;
	for (int i = 0; i <= 6; ++i) {
		glDisableVertexAttribArray(i);
		glVertexAttribDivisor(i, 0);
	}
}

S3DModelVAO::S3DModelVAO()
{
	RECOIL_DETAILED_TRACY_ZONE;
	vertData.reserve(VERT_SIZE0);
	indxData.reserve(INDX_SIZE0);

	vertVBO = VBO{ GL_ARRAY_BUFFER        , false };
	indxVBO = VBO{ GL_ELEMENT_ARRAY_BUFFER, false };
	instVBO = VBO{ GL_ARRAY_BUFFER        , false };

	//no better place to init it
	instVBO.Bind();
	instVBO.New(S3DModelVAO::INSTANCE_BUFFER_NUM_ELEMS * sizeof(SInstanceData), GL_STREAM_DRAW);
	instVBO.Unbind();
}

std::unique_ptr<S3DModelVAO> S3DModelVAO::instance = nullptr;

void S3DModelVAO::ProcessVertices(const S3DModel* model)
{
	RECOIL_DETAILED_TRACY_ZONE;
	assert(model);
	assert(model->loadStatus == S3DModel::LoadStatus::LOADING);

	if (const auto* root = model->GetRootPiece(); root->vertIndex != ~0u)
		return;

	uint32_t vertIndex = static_cast<uint32_t>(vertData.size());
	for (auto* modelPiece : model->pieceObjects) {
		modelPiece->vertIndex = vertIndex;
		const auto& modelPieceVerts = modelPiece->GetVerticesVec();
		vertIndex += modelPieceVerts.size();
		vertData.insert(vertData.end(), modelPieceVerts.begin(), modelPieceVerts.end()); //append
	}
}

void S3DModelVAO::ProcessIndicies(S3DModel* model)
{
	RECOIL_DETAILED_TRACY_ZONE;
	assert(model);
	if (model->indxStart != ~0u)
		return;

	//models should know their index offset
	model->indxStart = static_cast<uint32_t>(std::distance(indxData.cbegin(), indxData.cend()));

	for (auto* modelPiece : model->pieceObjects) {
		if (!modelPiece->HasGeometryData()) {
			modelPiece->indxStart = static_cast<uint32_t>(indxData.size());
			modelPiece->indxCount = 0;
			continue;
		}

		const auto& modelPieceIndcs = modelPiece->GetIndicesVec();
		indxData.insert(indxData.end(), modelPieceIndcs.begin(), modelPieceIndcs.end()); //append

		const auto endIdx = indxData.end();
		const auto begIdx = endIdx - modelPieceIndcs.size();

		std::for_each(begIdx, endIdx, [offset = modelPiece->vertIndex](uint32_t& indx) { indx += offset; }); // add per piece vertex offset to indices

		//model pieces should know their index offset
		modelPiece->indxStart = static_cast<uint32_t>(std::distance(indxData.begin(), begIdx));

		//model pieces should know their index count
		modelPiece->indxCount = static_cast<uint32_t>(modelPieceIndcs.size());
	}
	//models should know their index count
	model->indxCount = static_cast<uint32_t>(indxData.size() - model->indxStart);

	//add shatter indices to the end of indxData
	for (const auto* modelPiece : model->pieceObjects) {
		if (!modelPiece->HasGeometryData())
			continue;

		const auto& mdlPcsShatIndcs = modelPiece->GetShatterIndicesVec();

		indxData.insert(indxData.end(), mdlPcsShatIndcs.begin(), mdlPcsShatIndcs.end()); //append

		const auto endIdx = indxData.end();
		const auto begIdx = endIdx - mdlPcsShatIndcs.size();

		std::for_each(begIdx, endIdx, [offset = modelPiece->vertIndex](uint32_t& indx) { indx += offset; }); // add per piece vertex offset to indices
	}
}

void S3DModelVAO::CreateVAO()
{
	RECOIL_DETAILED_TRACY_ZONE;
	vao = VAO{};
	vao.Bind();

	vertVBO.Bind();
	indxVBO.Bind();
	EnableAttribs(false); // vertex attribs
	vertVBO.Unbind();

	instVBO.Bind();
	EnableAttribs(true); // instance attribs

	vao.Unbind();
	DisableAttribs();

	indxVBO.Unbind();
	instVBO.Unbind();
}

void S3DModelVAO::UploadVBOs()
{
	RECOIL_DETAILED_TRACY_ZONE;
	static constexpr size_t MEM_STEP = 8 * 1024 * 1024;
	bool reinitVAO = (vao.GetIdRaw() == 0);

	if (vertData.size() > vertUploadIndex) {
		assert(!safeToDeleteVectors);
		vertVBO.Bind();
		const size_t reqSize = AlignUp(std::max(vertData.size(), S3DModelVAO::VERT_SIZE0) * sizeof(SVertexData), MEM_STEP);
		reinitVAO |= (reqSize > vertVBO.GetSize());
		vertVBO.Resize(reqSize, GL_STATIC_DRAW); //noop if size hasn't changed, will copy data if changed
		vertVBO.SetBufferSubData(vertUploadIndex * sizeof(SVertexData), (vertData.size() - vertUploadIndex) * sizeof(SVertexData), vertData.data() + vertUploadIndex);
		vertVBO.Unbind();
		vertUploadIndex = vertData.size();
		vertUploadSize = vertUploadIndex;
	}

	if (indxData.size() > indxUploadIndex) {
		assert(!safeToDeleteVectors);
		indxVBO.Bind();
		const size_t reqSize = AlignUp(std::max(indxData.size(), S3DModelVAO::INDX_SIZE0) * sizeof(   uint32_t), MEM_STEP);
		reinitVAO |= (reqSize > indxVBO.GetSize());
		indxVBO.Resize(reqSize, GL_STATIC_DRAW); //noop if size hasn't changed, will copy data if changed
		indxVBO.SetBufferSubData(indxUploadIndex * sizeof(   uint32_t), (indxData.size() - indxUploadIndex) * sizeof(   uint32_t), indxData.data() + indxUploadIndex);
		indxVBO.Unbind();
		indxUploadIndex = indxData.size();
		indxUploadSize = indxUploadIndex;
	}

	if (reinitVAO)
		CreateVAO();

	if (safeToDeleteVectors && !vertData.empty()) {
		// all models have been uploaded in the calls above
		// safe to clear CPU copy of the data
		vertData.clear();
		indxData.clear();
		vertUploadIndex = 0;
		indxUploadIndex = 0;
	}
}

void S3DModelVAO::Init()
{
	RECOIL_DETAILED_TRACY_ZONE;
	Kill();
	instance = std::make_unique<S3DModelVAO>();
}

void S3DModelVAO::Kill()
{
	RECOIL_DETAILED_TRACY_ZONE;
	instance = nullptr;
}

void S3DModelVAO::Bind() const
{
	RECOIL_DETAILED_TRACY_ZONE;
	assert(vao.GetIdRaw() > 0);
	vao.Bind();
}

void S3DModelVAO::Unbind() const
{
	RECOIL_DETAILED_TRACY_ZONE;
	assert(vao.GetIdRaw() > 0);
	vao.Unbind();
}

void S3DModelVAO::BindLegacyVertexAttribsAndVBOs() const
{
	RECOIL_DETAILED_TRACY_ZONE;

	// Modern path: the VAO carries generic attribs 0..5 AND the element buffer,
	// so S3DModelPiece::DrawElements needs no change. Retires the five
	// client-array functions -- but only together with the shader define, since a
	// shader reading gl_Vertex gets nothing from generic attributes.
	//
	// ...and only when a program is actually bound. gl.UnitShape's rawState arg
	// defaults to TRUE, and CUnitDrawerGLSL::DrawIndividualDefOpaque then skips
	// PushIndividualOpaqueState entirely, so the caller owns the state and there
	// may be no shader at all. Generic attributes feed nothing in that case and
	// the draw would render blank -- which would break unupdated games the
	// moment this mode is enabled. Fall back per draw instead of per run: a
	// caller that binds its own generic-attrib shader gets the modern path, one
	// that relies on fixed-function keeps working exactly as before. The query
	// is supported by RenderDoc; the client-array calls below are not.
	GLint boundProgram = 0;
	if (GL::ModernModelAttribs()) {
		glGetIntegerv(GL_CURRENT_PROGRAM, &boundProgram);
		if (boundProgram != 0) {
			legacyAttribsBound = false;
			vao.Bind();
			return;
		}

		// No program of the caller's own, so fixed function would draw this.
		// Stand in for it where the state allows (see FFStateReproducible) and
		// the client arrays below go with it; where it does not, fall through.
		if (FFModelShaderEnabled() || GL::ffExperiment.Active(GL::FFExperiment::ModelFFShader)) {
			bool textured = false;
			bool fogged = false;
			const char* reject = FFStateReproducible(textured, fogged);

			// A rejected state falls back silently and reads exactly like a
			// converted one, so report which way it went, and for a rejection
			// report WHY -- the whole question this path has to answer is
			// whether ANY draw still needs the client arrays. Once per distinct
			// outcome: this runs per draw.
			static std::array<bool, 4> reportedTaken = {};
			static std::unordered_set<std::string> reportedRejects;
			if (reject == nullptr) {
				bool& seen = reportedTaken[size_t(textured) * 2 + size_t(fogged)];
				if (!seen) {
					seen = true;
					LOG_L(L_INFO, "[S3DModelVAO] FF-equivalent model shader ACTIVE (textured=%d fogged=%d)", int(textured), int(fogged));
				}
			} else if (reportedRejects.insert(reject).second) {
				LOG_L(L_INFO, "[S3DModelVAO] FF-equivalent model shader REJECTED (%s); client arrays retained", reject);
			}

			if (reject == nullptr) {
				Shader::IProgramObject* shader = GetFFModelShader(fogged);
				if (shader->IsValid()) {
					float curColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
					glGetFloatv(GL_CURRENT_COLOR, curColor);
					for (float& c : curColor)
						c = std::clamp(c, 0.0f, 1.0f);

					shader->Enable();
					shader->SetUniform("tex", 0);
					shader->SetUniform("uTextured", textured ? 1 : 0);
					shader->SetUniform4v("uColor", curColor);

					legacyAttribsBound = false;
					ffShaderBound = true;
					ffShaderFogged = fogged;
					vao.Bind();
					return;
				}
			}
		}
	}

	legacyAttribsBound = true;
	vertVBO.Bind();
	indxVBO.Bind();

	glEnableClientState(GL_VERTEX_ARRAY);
	glVertexPointer(3, GL_FLOAT, sizeof(SVertexData), vertVBO.GetPtr(offsetof(SVertexData, pos)));

	glEnableClientState(GL_NORMAL_ARRAY);
	glNormalPointer(GL_FLOAT, sizeof(SVertexData), vertVBO.GetPtr(offsetof(SVertexData, normal)));

	// Only unit 0 is bound by default. GL_TEXTURE0 is the default active client
	// texture, so dropping units 1/5/6 drops every glClientActiveTexture with
	// them -- and glClientActiveTexture was reachable ONLY here, so this is what
	// retires it.
	//
	// Whether uv1 and the sTangent/tTangent channels are read on this path is a
	// question about the shaders bound during it, so it was measured rather than
	// argued. gl.UnitShape is the only thing in BAR that reaches this path, and
	// driving it every frame (test/gl-ab-compare/ab_unitshape_driver.lua, 6845
	// binds/run vs ~31 without) gave 688 compared frames at 0 pixels difference,
	// with useLuaMat=true so BAR's own material shaders were bound -- the very
	// ones that read gl_MultiTexCoord5/6. They guard it
	// (`if (dot(T,T) < 0.1) T = vec3(1,0,0)`), and BAR's main unit paths bind the
	// MODERN VAO rather than this one.
	//
	// A game whose model shaders read those channels here WITHOUT such a guard
	// would lose its tangent frame, hence the config escape hatch.
	if (BindLegacyTexUnits())
		glClientActiveTexture(GL_TEXTURE0);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glTexCoordPointer(2, GL_FLOAT, sizeof(SVertexData), vertVBO.GetPtr(offsetof(SVertexData, texCoords[0])));

	if (!BindLegacyTexUnits())
		return;

	glClientActiveTexture(GL_TEXTURE1);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glTexCoordPointer(2, GL_FLOAT, sizeof(SVertexData), vertVBO.GetPtr(offsetof(SVertexData, texCoords[1])));

	glClientActiveTexture(GL_TEXTURE5);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glTexCoordPointer(3, GL_FLOAT, sizeof(SVertexData), vertVBO.GetPtr(offsetof(SVertexData, sTangent)));

	glClientActiveTexture(GL_TEXTURE6);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glTexCoordPointer(3, GL_FLOAT, sizeof(SVertexData), vertVBO.GetPtr(offsetof(SVertexData, tTangent)));
}

void S3DModelVAO::UnbindLegacyVertexAttribsAndVBOs() const
{
	RECOIL_DETAILED_TRACY_ZONE;

	if (!legacyAttribsBound) {
		vao.Unbind();

		if (ffShaderBound) {
			GetFFModelShader(ffShaderFogged)->Disable();
			ffShaderBound = false;
		}
		return;
	}
	if (BindLegacyTexUnits()) {
		glClientActiveTexture(GL_TEXTURE6);
		glDisableClientState(GL_TEXTURE_COORD_ARRAY);

		glClientActiveTexture(GL_TEXTURE5);
		glDisableClientState(GL_TEXTURE_COORD_ARRAY);

		glClientActiveTexture(GL_TEXTURE1);
		glDisableClientState(GL_TEXTURE_COORD_ARRAY);

		glClientActiveTexture(GL_TEXTURE0);
	}
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);

	glDisableClientState(GL_VERTEX_ARRAY);
	glDisableClientState(GL_NORMAL_ARRAY);

	indxVBO.Unbind();
	vertVBO.Unbind();
}

void S3DModelVAO::DrawElements(GLenum prim, uint32_t vboIndxStart, uint32_t vboIndxCount) const
{
	RECOIL_DETAILED_TRACY_ZONE;
	glDrawElements(prim, vboIndxCount, GL_UNSIGNED_INT, indxVBO.GetPtr(vboIndxStart * sizeof(uint32_t)));
}

template<typename TObj>
bool S3DModelVAO::AddToSubmissionImpl(const TObj* obj, uint32_t indexStart, uint32_t indexCount, uint16_t paletteIndex)
{
	RECOIL_DETAILED_TRACY_ZONE;
	const auto traIndex = transformsUploader.GetElemOffset(obj);
	if (traIndex == TransformsMemStorage::INVALID_INDEX)
		return false;

	const auto uniIndex = modelUniformsStorage.GetObjOffset(obj); //doesn't need to exist for defs and models. Don't check for validity

	uint16_t numPieces = 0;
	size_t bposeIndex = 0;
	if constexpr (std::is_same<TObj, S3DModel>::value) {
		numPieces = static_cast<uint16_t>(obj->numPieces);
		bposeIndex = transformsUploader.GetElemOffset(obj);
	}
	else {
		numPieces = static_cast<uint16_t>(obj->model->numPieces);
		bposeIndex = transformsUploader.GetElemOffset(obj->model);
	}

	if (bposeIndex == TransformsMemStorage::INVALID_INDEX)
		return false;

	auto& modelInstanceData = modelDataToInstance[SIndexAndCount{ indexStart, indexCount }];
	modelInstanceData.emplace_back(SInstanceData(
		static_cast<uint32_t>(traIndex),
		paletteIndex,
		numPieces,
		static_cast<uint32_t>(uniIndex),
		static_cast<uint32_t>(bposeIndex)
	));

	return true;
}

bool S3DModelVAO::AddToSubmission(const S3DModel* model, uint16_t paletteIndex)
{
	RECOIL_DETAILED_TRACY_ZONE;
	assert(model);

	return AddToSubmissionImpl(model, model->indxStart, model->indxCount, paletteIndex);
}

bool S3DModelVAO::AddToSubmission(const CUnit* unit)
{
	RECOIL_DETAILED_TRACY_ZONE;
	assert(unit);

	const S3DModel* model = unit->model;
	assert(model);

	return AddToSubmissionImpl(unit, model->indxStart, model->indxCount, unit->paletteIndex);
}

bool S3DModelVAO::AddToSubmission(const CFeature* feature)
{
	RECOIL_DETAILED_TRACY_ZONE;
	assert(feature);

	const S3DModel* model = feature->model;
	assert(model);

	return AddToSubmissionImpl(feature, model->indxStart, model->indxCount, feature->paletteIndex);
}

bool S3DModelVAO::AddToSubmission(const UnitDef* unitDef, uint16_t paletteIndex)
{
	RECOIL_DETAILED_TRACY_ZONE;
	assert(unitDef);

	const S3DModel* model = unitDef->model;
	assert(model);

	return AddToSubmissionImpl(unitDef, model->indxStart, model->indxCount, paletteIndex);
}


void S3DModelVAO::Submit(GLenum mode, bool bindUnbind)
{
	RECOIL_DETAILED_TRACY_ZONE;
	static std::vector<SDrawElementsIndirectCommand> submitCmds;
	submitCmds.clear();

	batchedBaseInstance = 0u;

	static std::vector<SInstanceData> allRenderModelData;
	allRenderModelData.reserve(INSTANCE_BUFFER_NUM_BATCHED);
	allRenderModelData.clear();

	for (const auto& [indxCount, renderModelData] : modelDataToInstance) {
		if (allRenderModelData.size() + renderModelData.size() >= INSTANCE_BUFFER_NUM_BATCHED)
			continue;

		SDrawElementsIndirectCommand scmd{
			indxCount.count,
			static_cast<uint32_t>(renderModelData.size()),
			indxCount.index,
			0u,
			batchedBaseInstance
		};

		submitCmds.emplace_back(scmd);

		allRenderModelData.insert(allRenderModelData.end(), renderModelData.cbegin(), renderModelData.cend());
		batchedBaseInstance += renderModelData.size();
	}

	if (submitCmds.empty())
		return;

	instVBO.Bind();
	instVBO.SetBufferSubData(allRenderModelData);
	instVBO.Unbind();

	if (bindUnbind)
		Bind();

	glMultiDrawElementsIndirect(mode, GL_UNSIGNED_INT, submitCmds.data(), submitCmds.size(), sizeof(SDrawElementsIndirectCommand));

	if (bindUnbind)
		Unbind();

	modelDataToInstance.clear();
}

template<typename TObj>
bool S3DModelVAO::SubmitImmediatelyImpl(const TObj* obj, uint32_t indexStart, uint32_t indexCount, uint16_t paletteIndex, GLenum mode, bool bindUnbind)
{
	RECOIL_DETAILED_TRACY_ZONE;
	std::size_t traIndex = transformsUploader.GetElemOffset(obj);
	if (traIndex == TransformsMemStorage::INVALID_INDEX)
		return false;

	const auto uniIndex = modelUniformsStorage.GetObjOffset(obj); //doesn't need to exist for defs. Don't check for validity

	uint16_t numPieces = 0;
	size_t bposeIndex = 0;
	if constexpr (std::is_same<TObj, S3DModel>::value) {
		numPieces = static_cast<uint16_t>(obj->numPieces);
		bposeIndex = transformsUploader.GetElemOffset(obj);
	}
	else {
		numPieces = static_cast<uint16_t>(obj->model->numPieces);
		bposeIndex = transformsUploader.GetElemOffset(obj->model);
	}

	SInstanceData instanceData(static_cast<uint32_t>(traIndex), paletteIndex, numPieces, uniIndex, bposeIndex);
	const uint32_t immediateBaseInstanceAbs = INSTANCE_BUFFER_NUM_BATCHED + immediateBaseInstance;

	static SDrawElementsIndirectCommand scmd;
	scmd = {
		indexCount,
		1,
		indexStart,
		0u,
		immediateBaseInstanceAbs
	};

	instVBO.Bind();
	instVBO.SetBufferSubData(immediateBaseInstanceAbs * sizeof(SInstanceData), sizeof(SInstanceData), &instanceData);
	instVBO.Unbind();

	immediateBaseInstance = (immediateBaseInstance + 1) % INSTANCE_BUFFER_NUM_IMMEDIATE;

	if (bindUnbind)
		Bind();

	// As of 01.05.2023 AMD Windows drivers do not support baseInstance field of SDrawElementsIndirectCommand
	// therefore can't use glDrawElementsIndirect
	// At the same time AMD Windows drivers sometimes crash on glDrawElementsInstancedBaseInstance
	// can't use it either
	// Revert to glMultiDrawElementsIndirect as it works reliably

	glMultiDrawElementsIndirect(mode, GL_UNSIGNED_INT, &scmd, 1u, sizeof(SDrawElementsIndirectCommand));

	if (bindUnbind)
		Unbind();

	return true;
}

bool S3DModelVAO::SubmitImmediately(const S3DModel* model, uint16_t paletteIndex, GLenum mode, bool bindUnbind)
{
	RECOIL_DETAILED_TRACY_ZONE;
	assert(model);
	return SubmitImmediatelyImpl(model, model->indxStart, model->indxCount, paletteIndex, mode, bindUnbind);
}

bool S3DModelVAO::SubmitImmediately(const CUnit* unit, const GLenum mode, bool bindUnbind)
{
	RECOIL_DETAILED_TRACY_ZONE;
	assert(unit);

	const S3DModel* model = unit->model;
	assert(model);

	return SubmitImmediatelyImpl(unit, model->indxStart, model->indxCount, unit->paletteIndex, mode, bindUnbind);
}

bool S3DModelVAO::SubmitImmediately(const CFeature* feature, GLenum mode, bool bindUnbind)
{
	RECOIL_DETAILED_TRACY_ZONE;
	assert(feature);

	const S3DModel* model = feature->model;
	assert(model);

	return SubmitImmediatelyImpl(feature, model->indxStart, model->indxCount, feature->paletteIndex, mode, bindUnbind);
}

bool S3DModelVAO::SubmitImmediately(const UnitDef* unitDef, int teamID, GLenum mode, bool bindUnbind)
{
	RECOIL_DETAILED_TRACY_ZONE;
	assert(unitDef);

	const S3DModel* model = unitDef->model;
	assert(model);

	return SubmitImmediatelyImpl(unitDef, model->indxStart, model->indxCount, static_cast<uint16_t>(teamID), mode, bindUnbind);
}
