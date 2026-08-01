/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "FFShaderRewrite.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <cmath>
#include <unordered_map>
#include <vector>

#include "Rendering/GL/FFFog.h"
#include "Rendering/GL/FFRasterState.h"
#include "Rendering/GL/MatrixStateTracker.h"
#include "System/Matrix44f.h"
#include "Rendering/GL/FFStateTracker.h"
#include "Rendering/GL/myGL.h"
#include "System/Config/ConfigHandler.h"
#include "System/Log/ILog.h"

CONFIG(bool, FFVertexAttribRewrite).defaultValue(false).safemodeValue(false)
	.description("Rewrite the fixed-function vertex builtins (gl_Vertex, gl_Color, gl_MultiTexCoord0, ftransform) in compiled GLSL to generic attributes, so shader-bound immediate-mode draws can be fed without glBegin/glVertex. Off leaves every shader source byte-identical.");

namespace {
	bool usesFtransformEarly(const std::string& code);

	constexpr const char* ATTR_VERTEX = "recoil_ff_aVertex";
	constexpr const char* ATTR_TEXCRD = "recoil_ff_aTexCoord0";
	constexpr const char* UNIFORM_USE = "recoil_ff_useAttrs";

	bool IsIdentChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

	// whole-identifier find, so gl_MultiTexCoord0 never matches inside
	// gl_MultiTexCoord07 and gl_Color never matches inside gl_ColorFoo
	size_t FindIdent(const std::string& s, const std::string& id, size_t from)
	{
		for (size_t p = s.find(id, from); p != std::string::npos; p = s.find(id, p + 1)) {
			const bool leftOK  = (p == 0) || !IsIdentChar(s[p - 1]);
			const bool rightOK = (p + id.size() >= s.size()) || !IsIdentChar(s[p + id.size()]);
			if (leftOK && rightOK)
				return p;
		}
		return std::string::npos;
	}

	bool HasIdent(const std::string& s, const std::string& id) { return FindIdent(s, id, 0) != std::string::npos; }

	// ftransform() expands to the same transform, so it counts as a reader of it
	bool usesFtransformEarly(const std::string& code) { return HasIdent(code, "ftransform"); }

	// Same length as `src` with every comment blanked, so identifier positions
	// carry straight over. Commented-out code has to be invisible here or it
	// drives the whole decision: BAR's cus_gl4.vert.glsl mentions
	// gl_MultiTexCoord0 exactly once, on a commented-out line, and that alone was
	// enough to inject a declaration into a shader that reads no builtin at all.
	std::string MaskComments(const std::string& src)
	{
		std::string out = src;
		const size_t n = out.size();

		// newlines survive, so a masked position still sits on the source line it
		// came from and the #line arithmetic below stays right
		for (size_t i = 0; i + 1 < n; ) {
			if (src[i] == '/' && src[i + 1] == '/') {
				while (i < n && src[i] != '\n')
					out[i++] = ' ';
				continue;
			}
			if (src[i] == '/' && src[i + 1] == '*') {
				out[i] = out[i + 1] = ' ';
				for (i += 2; i + 1 < n && !(src[i] == '*' && src[i + 1] == '/'); ++i) {
					if (src[i] != '\n')
						out[i] = ' ';
				}
				if (i + 1 < n) {
					out[i] = out[i + 1] = ' ';
					i += 2;
				} else {
					i = n;
				}
				continue;
			}
			++i;
		}

		return out;
	}

	// Blank the body of every `#if 0` block, for the same reason comments are
	// blanked: dead code otherwise drives the whole decision. BAR's
	// cus_gl4.vert.glsl reads gl_ModelViewMatrix exactly once, inside `#if 0`,
	// and that alone made the generated accessor reference a builtin the live
	// shader never touches -- which compiles fine in a compatibility profile and
	// not at all where the surrounding code had already stopped needing it.
	//
	// Only the literal `#if 0` form, and only up to its own `#else`/`#elif`,
	// whose branch IS live. Evaluating the preprocessor properly is a different
	// project; this is the form people actually use to switch code off.
	std::string MaskDisabledBlocks(const std::string& src)
	{
		std::string out = src;
		size_t i = 0;

		while (i < out.size()) {
			const size_t eol = std::min(out.find('\n', i), out.size());
			size_t p = i;
			while (p < eol && (out[p] == ' ' || out[p] == '\t'))
				++p;

			// "#if 0" with only whitespace after the 0
			bool disabled = (out.compare(p, 3, "#if") == 0);
			if (disabled) {
				size_t q = p + 3;
				while (q < eol && (out[q] == ' ' || out[q] == '\t'))
					++q;
				disabled = (q < eol && out[q] == '0');
				if (disabled) {
					for (++q; q < eol; ++q)
						disabled &= (out[q] == ' ' || out[q] == '\t' || out[q] == '\r');
				}
			}

			if (!disabled) {
				i = eol + 1;
				continue;
			}

			// blank through to the matching #else/#elif/#endif
			int depth = 0;
			size_t j = eol + 1;
			while (j < out.size()) {
				const size_t lineEnd = std::min(out.find('\n', j), out.size());
				size_t k = j;
				while (k < lineEnd && (out[k] == ' ' || out[k] == '\t'))
					++k;

				const bool opens = (out.compare(k, 3, "#if") == 0);
				const bool closes = (out.compare(k, 6, "#endif") == 0);
				const bool alt = (depth == 0) && (out.compare(k, 5, "#else") == 0 || out.compare(k, 5, "#elif") == 0);

				if (alt || (closes && depth == 0))
					break;

				depth += opens;
				depth -= closes;

				for (size_t b = j; b < lineEnd; ++b) {
					if (out[b] != '\n')
						out[b] = ' ';
				}
				j = lineEnd + 1;
			}

			i = j;
		}

		return out;
	}

	// The builtins this does NOT feed. A shader reading one of them cannot take
	// the attribute path at all, because the value it needs has no channel --
	// so it keeps fixed function rather than getting a silent zero.
	const std::array<const char*, 10> UNFED_BUILTINS = {
		"gl_Normal", "gl_SecondaryColor", "gl_FogCoord",
		"gl_MultiTexCoord1", "gl_MultiTexCoord2", "gl_MultiTexCoord3",
		"gl_MultiTexCoord4", "gl_MultiTexCoord5", "gl_MultiTexCoord6",
		"gl_MultiTexCoord7",
	};

	// Offset of a "#version" directive in `code` (already comment-masked), or npos.
	// Only whitespace may precede it on its line -- BAR indents it inside a Lua
	// long string, and treating that as "not a directive" put the declarations
	// above it, which is the one place GLSL forbids anything.
	size_t FindVersionDirective(const std::string& code)
	{
		for (size_t v = code.find("#version"); v != std::string::npos; v = code.find("#version", v + 1)) {
			size_t p = v;
			while (p > 0 && (code[p - 1] == ' ' || code[p - 1] == '\t'))
				--p;
			if (p == 0 || code[p - 1] == '\n')
				return v;
		}
		return std::string::npos;
	}

	// Insert after the #version directive AND after the leading #extension /
	// #pragma block, or at the very top when the caller already stripped the
	// version. #extension is only legal before the first non-preprocessor token,
	// so a declaration placed between #version and it makes the shader
	// uncompilable -- which is how the engine's own fogged stand-in broke.
	//
	// Returns the byte offset and the source line the next line then carries, so
	// #line can be re-anchored and a compile error in the game's own source still
	// reports the line its author wrote.
	std::pair<size_t, int> PrologueInsertPoint(const std::string& code)
	{
		// The caller may have stripped the version already (Shader.cpp extracts it
		// and re-prepends it after compiling), in which case the #extension block
		// is the very first thing in the source and the skip below is the ONLY
		// thing keeping the declarations out from in front of it.
		size_t at = 0;

		if (const size_t v = FindVersionDirective(code); v != std::string::npos) {
			at = code.find('\n', v);
			if (at == std::string::npos)
				return { code.size(), 1 };
			++at;
		}

		// Only blank lines and the two directives that must stay at the top are
		// skipped; anything else (a #define, a #if) could put the declarations
		// inside a conditional or past something that reads them.
		for (;;) {
			size_t p = at;
			while (p < code.size() && (code[p] == ' ' || code[p] == '\t'))
				++p;

			const bool skippable = (p < code.size() && code[p] == '\n') ||
			                       (code.compare(p, 10, "#extension") == 0) ||
			                       (code.compare(p, 7, "#pragma") == 0);
			if (!skippable)
				break;

			const size_t eol = code.find('\n', p);
			if (eol == std::string::npos)
				return { code.size(), 1 };
			at = eol + 1;
		}

		int line = 1;
		for (size_t i = 0; i < at; ++i)
			line += (code[i] == '\n');

		return { at, line };
	}
}

bool GL::FFRewriteEnabled()
{
	static const bool enabled = configHandler->GetBool("FFVertexAttribRewrite");
	return enabled;
}

int GL::ParseGlslVersion(const std::string& text)
{
	const size_t v = FindVersionDirective(MaskComments(text));
	if (v == std::string::npos)
		return 0;

	size_t p = v + 8;
	while (p < text.size() && (text[p] == ' ' || text[p] == '\t'))
		++p;

	int ver = 0;
	for (; p < text.size() && std::isdigit(static_cast<unsigned char>(text[p])); ++p)
		ver = ver * 10 + (text[p] - '0');

	return ver;
}

bool GL::RewriteFFBuiltins(std::string& src, int glslVersion, uint32_t stage)
{
	if (!FFRewriteEnabled())
		return false;

	// every decision below reads the MASKED source, so commented-out code neither
	// triggers a rewrite nor gets one
	const std::string code = MaskDisabledBlocks(MaskComments(src));

	// gl_Fog is fixed-function state read identically in both stages. The vertex
	// channels are vertex-stage only, and gl_Color especially so: in a fragment
	// shader that name is the INTERPOLATED varying, not the attribute, and
	// substituting it there would read a vertex input that does not exist.
	const bool vs = (stage == GL_VERTEX_SHADER);
	const bool usesFog = HasIdent(code, "gl_Fog");
	// The matrix builtins are fixed-function state, readable from either stage
	// like gl_Fog. FindIdent matches whole identifiers, so gl_ModelViewMatrix
	// never matches inside gl_ModelViewMatrixInverse and the order here is free.
	struct MatrixBuiltin { const char* builtin; const char* uniform; const char* accessor; bool used; };
	MatrixBuiltin matrices[] = {
		{ "gl_ModelViewProjectionMatrix",        "recoil_ff_MVPu",    "recoil_ff_MVP()",    false },
		{ "gl_ModelViewMatrix",                  "recoil_ff_MVu",     "recoil_ff_MV()",     false },
		{ "gl_ProjectionMatrix",                 "recoil_ff_Pu",      "recoil_ff_P()",      false },
		{ "gl_ModelViewProjectionMatrixInverse", "recoil_ff_MVPinvU", "recoil_ff_MVPinv()", false },
		{ "gl_ModelViewMatrixInverse",           "recoil_ff_MVinvU",  "recoil_ff_MVinv()",  false },
		{ "gl_ProjectionMatrixInverse",          "recoil_ff_PinvU",   "recoil_ff_Pinv()",   false },
	};

	bool usesAnyMatrix = false;
	for (MatrixBuiltin& mb : matrices) {
		mb.used = HasIdent(code, mb.builtin);
		usesAnyMatrix |= mb.used;
	}

	// ftransform() expands to gl_ModelViewProjectionMatrix * gl_Vertex
	matrices[0].used |= (vs && usesFtransformEarly(code));
	usesAnyMatrix |= matrices[0].used;

	// gl_NormalMatrix is mat3, so it gets its own declaration rather than joining
	// the loop; it is derived from the modelview the same way the builtin is.
	const bool usesNormalMatrix = HasIdent(code, "gl_NormalMatrix");
	usesAnyMatrix |= usesNormalMatrix;

	const bool usesMVP = usesAnyMatrix;
	const bool usesColor = vs && HasIdent(code, "gl_Color");
	bool usesFtransform = vs && HasIdent(code, "ftransform");
	bool usesVertex = usesFtransform || (vs && HasIdent(code, "gl_Vertex"));
	bool usesTexCrd = vs && HasIdent(code, "gl_MultiTexCoord0");

	if (!usesVertex && !usesColor && !usesTexCrd && !usesFog && !usesMVP)
		return false;

	// The stream channels come as a set -- half-feeding one of the builtins this
	// has no channel for would silently zero it -- so a shader that reads one of
	// those keeps fixed function for its position and texcoords. Its colour still
	// converts: that channel is a global current value, complete on its own, and
	// nothing about it depends on a draw supplying vertices.
	for (const char* b : UNFED_BUILTINS) {
		if (!HasIdent(code, b))
			continue;

		LOG_L(L_DEBUG, "[FFRewrite] stream channels declined: shader reads %s, which has no attribute channel", b);
		usesFtransform = usesVertex = usesTexCrd = false;
		break;
	}

	if (!usesVertex && !usesColor && !usesTexCrd && !usesFog && !usesMVP)
		return false;

	// Collect every site against the mask, then rewrite `src` once from the back,
	// so each replacement lands at a position the mask still describes. Doing it
	// before the declarations go in also stops the generated accessors -- which
	// read the builtins on the fixed-function side of the branch -- from being
	// rewritten into infinite recursion.
	struct Site { size_t pos, len; const char* repl; };
	std::vector<Site> sites;

	const auto collect = [&](bool used, const char* id, const char* repl) {
		if (!used)
			return;
		const size_t n = std::strlen(id);
		for (size_t p = FindIdent(code, id, 0); p != std::string::npos; p = FindIdent(code, id, p + n))
			sites.push_back({p, n, repl});
	};
	collect(usesFtransform, "ftransform", "recoil_ff_Transform");
	collect(usesVertex, "gl_Vertex", "recoil_ff_Vertex()");
	collect(usesColor, "gl_Color", "recoil_ff_Color()");
	collect(usesTexCrd, "gl_MultiTexCoord0", "recoil_ff_MultiTexCoord0()");
	// an accessor RETURNING the struct, so member access carries over unchanged
	// (gl_Fog.color becomes recoil_ff_Fog().color) and the builtin stays reachable
	// behind the selector below
	collect(usesFog, "gl_Fog", "recoil_ff_Fog()");
	for (const MatrixBuiltin& mb : matrices)
		collect(mb.used, mb.builtin, mb.accessor);
	collect(usesNormalMatrix, "gl_NormalMatrix", "recoil_ff_N()");

	// the insertion point is taken before any of this, off the same mask; every
	// site is below the #version line, so the offset survives the rewrite
	const auto [at, nextLine] = PrologueInsertPoint(code);

	std::sort(sites.begin(), sites.end(), [](const Site& a, const Site& b) { return a.pos > b.pos; });
	for (const Site& s : sites)
		src.replace(s.pos, s.len, s.repl);

	const char* in = (glslVersion >= 130) ? "in" : "attribute";

	// one physical line, so a compile error in the game's own source still
	// reports the line the author wrote (see the #line re-anchor below)
	std::string decl;
	if (usesVertex || usesTexCrd)
		decl += "uniform bool " + std::string(UNIFORM_USE) + ";";
	if (usesVertex) {
		decl += std::string(in) + " vec4 " + ATTR_VERTEX + ";";
		decl += " vec4 recoil_ff_Vertex() { return " + std::string(UNIFORM_USE) + " ? " + ATTR_VERTEX + " : gl_Vertex; }";
	}
	if (usesFtransform) {
		// ftransform() is gl_ModelViewProjectionMatrix * gl_Vertex, so it takes
		// the same transform source as an explicit read of the builtin does
		decl += " vec4 recoil_ff_Transform() { return recoil_ff_MVP() * recoil_ff_Vertex(); }";
	}
	if (usesColor) {
		// No useAttrs branch, unlike the two above: the pinned slot's CURRENT
		// value is the fixed-function current colour (GL::ffColor writes it there
		// instead of to GL), and a stream draw overrides it per vertex the same
		// way a colour array overrode gl_Color. Both cases are already right, so
		// there is nothing for a per-draw switch to select.
		decl += std::string(in) + " vec4 " + GL::FF_COLOR_ATTRIB_NAME + ";";
		decl += " vec4 recoil_ff_Color() { return " + std::string(GL::FF_COLOR_ATTRIB_NAME) + "; }";
	}
	if (usesTexCrd) {
		decl += std::string(in) + " vec4 " + ATTR_TEXCRD + ";";
		decl += " vec4 recoil_ff_MultiTexCoord0() { return " + std::string(UNIFORM_USE) + " ? " + ATTR_TEXCRD + " : gl_MultiTexCoord0; }";
	}

	if (usesFog) {
		// The member set and its meanings are the builtin's, so a shader reading
		// any of them keeps compiling untouched. scale is 1/(end - start) whichever
		// mode is set, which is why nothing reads the mode.
		//
		// Both sources are compiled in and selected by a uniform, because the
		// whole-frame gate compares Lua backends within ONE build and is otherwise
		// blind to this substitution: a wrong uniform feed would move every pass
		// together and read a clean 0/0. With the selector, the A/B harness flips
		// it per pass and the gate measures "uniform == builtin" directly. The
		// selector costs one bool once the answer is in.
		decl += " struct recoil_ff_FogParameters { vec4 color; float density; float start; float end; float scale; };";
		decl += " uniform recoil_ff_FogParameters " + std::string(FF_FOG_UNIFORM_NAME) + ";";
		decl += " uniform bool " + std::string(FF_FOG_SELECT_NAME) + ";";
		decl += " recoil_ff_FogParameters recoil_ff_Fog() { return " + std::string(FF_FOG_SELECT_NAME) + " ? " +
		        std::string(FF_FOG_UNIFORM_NAME) +
		        " : recoil_ff_FogParameters(gl_Fog.color, gl_Fog.density, gl_Fog.start, gl_Fog.end, gl_Fog.scale); }";
	}

	if (usesAnyMatrix) {
		// One selector for the whole set: they are fed together and there is no
		// case where a shader should take some from the engine and some from the
		// driver.
		decl += " uniform bool " + std::string(FF_MVP_SELECT_NAME) + ";";

		for (const MatrixBuiltin& mb : matrices) {
			if (!mb.used)
				continue;
			decl += " uniform mat4 " + std::string(mb.uniform) + ";";
			decl += " mat4 " + std::string(mb.accessor).substr(0, std::strlen(mb.accessor) - 2) +
			        "() { return " + std::string(FF_MVP_SELECT_NAME) + " ? " + mb.uniform + " : " + mb.builtin + "; }";
		}

		if (usesNormalMatrix) {
			decl += " uniform mat3 recoil_ff_Nu;";
			decl += " mat3 recoil_ff_N() { return " + std::string(FF_MVP_SELECT_NAME) + " ? recoil_ff_Nu : gl_NormalMatrix; }";
		}
	}

	src.insert(at, decl + "\n#line " + std::to_string(nextLine) + "\n");
	return true;
}

namespace {
	struct ProgFFUniforms {
		int32_t fogColor = -1, fogDensity = -1, fogStart = -1, fogEnd = -1, fogScale = -1;
		int32_t fogSelect = -1;
		int32_t mvp = -1, mvpSelect = -1;
		int32_t mv = -1, proj = -1, normal = -1;
		int32_t mvpInv = -1, mvInv = -1, projInv = -1;
		uint32_t fedGeneration = 0; // 0 = never fed
		int32_t fedSelect = -1;     // the selector value this program last saw
		bool anything = false;
	};

	// Keyed by program id, and the ids are RECYCLED -- an entry that outlives its
	// program would hand the next one the previous layout -- so glLinkProgram and
	// glDeleteProgram evict, which is why the feed wraps them too.
	std::unordered_map<uint32_t, ProgFFUniforms> progUniforms;

	decltype(glad_glEnable) origEnable = nullptr;
	decltype(glad_glPopAttrib) origPopAttrib = nullptr;
	decltype(glad_glDisable) origDisable = nullptr;
	decltype(glad_glUseProgram) origUseProgram = nullptr;
	decltype(glad_glLinkProgram) origLinkProgram = nullptr;
	decltype(glad_glDeleteProgram) origDeleteProgram = nullptr;

	// True when the candidate pass of an FF-removal experiment holds `cap` off.
	// Done at the enable rather than per call site: all of them are covered at
	// once, including the ones a grep would miss.
	bool ExperimentHoldsCapOff(GLenum cap)
	{
		switch (cap) {
			case GL_ALPHA_TEST:    return GL::ffExperiment.Active(GL::FFExperiment::AlphaTestOff);
			case GL_LINE_STIPPLE:  return GL::ffExperiment.Active(GL::FFExperiment::LineStippleOff);
			case GL_CLIP_PLANE0:
			case GL_CLIP_PLANE1:
			case GL_CLIP_PLANE2:
			case GL_CLIP_PLANE3:
			case GL_CLIP_PLANE4:
			case GL_CLIP_PLANE5:   return GL::ffExperiment.Active(GL::FFExperiment::ClipPlanesOff);
			default:               return false;
		}
	}

	void APIENTRY HookEnable(GLenum cap)
	{
		// An experiment that holds a capability off reads 0 px both when the
		// operation is inert and when it was never switched on to begin with, so
		// say which caps a run actually reached.
		switch (cap) {
			case GL_ALPHA_TEST:
			case GL_LINE_STIPPLE:
			case GL_CLIP_PLANE0: case GL_CLIP_PLANE1: case GL_CLIP_PLANE2:
			case GL_CLIP_PLANE3: case GL_CLIP_PLANE4: case GL_CLIP_PLANE5: {
				static std::unordered_map<GLenum, bool> reported;
				if (bool& seen = reported[cap]; !seen) {
					seen = true;
					LOG_L(L_WARNING, "[FFRaster] cap 0x%x ENABLED at least once this run", cap);
				}
			} break;
			default: break;
		}

		if (ExperimentHoldsCapOff(cap)) {
			GL::ffRaster.NoteCap(cap, false);
			origDisable(cap);
			return;
		}

		GL::ffRaster.NoteCap(cap, true);
		origEnable(cap);
	}

	void APIENTRY HookDisable(GLenum cap)
	{
		GL::ffRaster.NoteCap(cap, false);
		origDisable(cap);
	}

	void APIENTRY HookPopAttrib()
	{
		origPopAttrib();
		GL::ffRaster.ResyncCaps();
	}

	// Per DRAW rather than per bind, because the fixed-function matrices move
	// while a program stays bound -- CModelDrawerStateGLSL::Enable binds, then
	// PushTransform and DrawStaticLegacy push a transform per object and per
	// piece. Both matrices are read back from GL, so the only thing that differs
	// between the two sides of the selector is who multiplies them.
	void PushMVPUniform()
	{
		const uint32_t prog = GL::CurrentProgram();
		if (prog == 0)
			return;

		auto it = progUniforms.find(prog);

		// A program drawn before it was ever bound through the feed has no cache
		// entry yet; the matrices are needed for THIS draw, so build it here
		// rather than waiting for a bind that already happened.
		if (it == progUniforms.end()) {
			GL::PushFFUniforms(prog);
			it = progUniforms.find(prog);
		}

		if (it == progUniforms.end() || it->second.mvpSelect < 0)
			return;

		const bool fromMirror = GL::ffExperiment.Active(GL::FFExperiment::MatrixUniformFromMirror);
		const int32_t select = (fromMirror || GL::ffExperiment.Active(GL::FFExperiment::MatrixUniform)) ? 1 : 0;
		glUniform1i(it->second.mvpSelect, select);

		if (select == 0 || it->second.mvp < 0)
			return;

		// gl.CallList replays matrix ops inside the driver, where glad cannot see
		// them, so the mirror can be tainted. Falling back keeps the frame
		// rendering, but a tainted push is a phase-C blocker in its own right and
		// has to be counted rather than absorbed.
		const bool useMirror = fromMirror && GL::ffMirror.Valid();

		if (fromMirror && !useMirror) {
			static int tainted = 0;
			if (tainted++ == 0)
				LOG_L(L_WARNING, "[FFUniformFeed] mirror TAINTED at a draw; this push fell back to glGetFloatv");
		}

		CMatrix44f proj;
		CMatrix44f modelView;
		glGetFloatv(GL_PROJECTION_MATRIX, static_cast<float*>(proj));
		glGetFloatv(GL_MODELVIEW_MATRIX, static_cast<float*>(modelView));

		const CMatrix44f mvp = useMirror ? GL::ffMirror.GetMVP() : (proj * modelView);

		const auto upload = [](int32_t loc, const CMatrix44f& m) {
			if (loc >= 0)
				glUniformMatrix4fv(loc, 1, GL_FALSE, static_cast<const float*>(m));
		};
		const ProgFFUniforms& u = it->second;
		upload(u.mv,   useMirror ? GL::ffMirror.tracker.GetMatrix(GL_MODELVIEW) : modelView);
		upload(u.proj, useMirror ? GL::ffMirror.tracker.GetMatrix(GL_PROJECTION) : proj);

		// The inverses are the builtins' own definitions, computed where the
		// matrices are rather than asked of a driver that will not have them.
		if (u.mvInv >= 0)   upload(u.mvInv,  (useMirror ? GL::ffMirror.tracker.GetMatrix(GL_MODELVIEW) : modelView).Invert());
		if (u.projInv >= 0) upload(u.projInv, (useMirror ? GL::ffMirror.tracker.GetMatrix(GL_PROJECTION) : proj).Invert());
		if (u.mvpInv >= 0)  upload(u.mvpInv, mvp.Invert());

		if (u.normal >= 0) {
			// gl_NormalMatrix is the transpose of the inverse of the upper-left
			// 3x3 of the modelview, flattened column-major for a mat3 uniform.
			const CMatrix44f inv = (useMirror ? GL::ffMirror.tracker.GetMatrix(GL_MODELVIEW) : modelView).Invert();
			const float n3[9] = {
				inv.m[0], inv.m[4], inv.m[8],
				inv.m[1], inv.m[5], inv.m[9],
				inv.m[2], inv.m[6], inv.m[10],
			};
			glUniformMatrix3fv(u.normal, 1, GL_FALSE, n3);
		}

		// A mirror that happened to agree with glGetFloatv everywhere would make
		// this experiment a re-run of the last one. Report how far apart they
		// actually got, so a 0-pixel result is a result about a real difference.
		if (useMirror) {
			const CMatrix44f ref = proj * modelView;
			static float worst = 0.0f;
			float d = 0.0f;
			for (int i = 0; i < 16; ++i)
				d = std::max(d, std::fabs(mvp.m[i] - ref.m[i]) / std::max(1.0f, std::fabs(ref.m[i])));

			if (d > worst * 2.0f && d > 0.0f) {
				worst = d;
				LOG_L(L_WARNING, "[FFUniformFeed] mirror MVP differs from the glGetFloatv product by %g relative", d);
			}
		}
		glUniformMatrix4fv(it->second.mvp, 1, GL_FALSE, static_cast<const float*>(mvp));

		// how many distinct programs the measurement actually covered; a 0-pixel
		// result over two of them would say very little
		static std::unordered_map<uint32_t, bool> fedPrograms;
		if (bool& seen = fedPrograms[prog]; !seen) {
			seen = true;
			LOG_L(L_WARNING, "[FFUniformFeed] CPU-composed MVP FED to program %u (%d distinct so far)",
				prog, static_cast<int>(fedPrograms.size()));
		}
	}

	#define FFMVP_DRAWS(X) \
		X(glDrawArrays,             (GLenum a, GLint b, GLsizei c), (a, b, c)) \
		X(glDrawElements,           (GLenum a, GLsizei b, GLenum c, const void* d), (a, b, c, d)) \
		X(glDrawRangeElements,      (GLenum a, GLuint b, GLuint c, GLsizei d, GLenum e, const void* f), (a, b, c, d, e, f)) \
		X(glDrawElementsBaseVertex, (GLenum a, GLsizei b, GLenum c, const void* d, GLint e), (a, b, c, d, e)) \
		X(glDrawArraysInstanced,    (GLenum a, GLint b, GLsizei c, GLsizei d), (a, b, c, d)) \
		X(glDrawElementsInstanced,  (GLenum a, GLsizei b, GLenum c, const void* d, GLsizei e), (a, b, c, d, e))

	#define FFMVP_DECL_ORIG(name, params, args) decltype(glad_##name) origMVP_##name = nullptr;
	FFMVP_DRAWS(FFMVP_DECL_ORIG)
	#undef FFMVP_DECL_ORIG

	#define FFMVP_DEFINE(name, params, args) \
		void APIENTRY MVPHook_##name params { \
			PushMVPUniform(); \
			origMVP_##name args; \
		}
	FFMVP_DRAWS(FFMVP_DEFINE)
	#undef FFMVP_DEFINE

	void APIENTRY FeedUseProgram(GLuint program)
	{
		origUseProgram(program);
		GL::PushFFUniforms(program);
	}

	void APIENTRY FeedLinkProgram(GLuint program)
	{
		progUniforms.erase(program);
		origLinkProgram(program);
	}

	void APIENTRY FeedDeleteProgram(GLuint program)
	{
		progUniforms.erase(program);
		origDeleteProgram(program);
	}
}

void GL::PushFFUniforms(uint32_t prog)
{
	if (prog == 0 || ffListBodyOpen || !FFRewriteEnabled())
		return;

	auto it = progUniforms.find(prog);

	if (it == progUniforms.end()) {
		ProgFFUniforms u;
		const std::string base = FF_FOG_UNIFORM_NAME;
		u.fogColor   = glGetUniformLocation(prog, (base + ".color").c_str());
		u.fogDensity = glGetUniformLocation(prog, (base + ".density").c_str());
		u.fogStart   = glGetUniformLocation(prog, (base + ".start").c_str());
		u.fogEnd     = glGetUniformLocation(prog, (base + ".end").c_str());
		u.fogScale   = glGetUniformLocation(prog, (base + ".scale").c_str());
		u.fogSelect  = glGetUniformLocation(prog, FF_FOG_SELECT_NAME);
		u.mvp        = glGetUniformLocation(prog, FF_MVP_UNIFORM_NAME);
		u.mvpSelect  = glGetUniformLocation(prog, FF_MVP_SELECT_NAME);
		u.mv         = glGetUniformLocation(prog, "recoil_ff_MVu");
		u.proj       = glGetUniformLocation(prog, "recoil_ff_Pu");
		u.normal     = glGetUniformLocation(prog, "recoil_ff_Nu");
		u.mvpInv     = glGetUniformLocation(prog, "recoil_ff_MVPinvU");
		u.mvInv      = glGetUniformLocation(prog, "recoil_ff_MVinvU");
		u.projInv    = glGetUniformLocation(prog, "recoil_ff_PinvU");
		u.anything = (u.fogColor >= 0 || u.fogDensity >= 0 || u.fogStart >= 0 || u.fogEnd >= 0 || u.fogScale >= 0 || u.fogSelect >= 0);
		it = progUniforms.emplace(prog, u).first;
	}

	ProgFFUniforms& u = it->second;

	// The candidate pass of FFExperiment 9 reads the BUILTIN, so the selector is
	// pass-dependent and has to be re-pushed whenever it flips, not only when the
	// values move.
	const int32_t select = ffExperiment.Active(FFExperiment::FogUniform) ? 0 : 1;

	if (!u.anything || (u.fedGeneration == ffFog.Generation() && u.fedSelect == select))
		return;

	u.fedGeneration = ffFog.Generation();
	u.fedSelect = select;

	// A selector that never reaches GL leaves every program on the builtin, which
	// is a clean 0/0 over a substitution that never happened -- the exact vacuous
	// pass this experiment exists to rule out.
	static bool reported = false;
	if (!reported && u.fogSelect >= 0) {
		reported = true;
		LOG_L(L_WARNING, "[FFUniformFeed] fog uniform FED to program %u (select=%d)", prog, select);
	}

	if (u.fogSelect >= 0) glUniform1i(u.fogSelect, select);

	// The linker drops members the shader never reads, so each of these is
	// optional rather than a set.
	if (u.fogColor >= 0)   glUniform4fv(u.fogColor, 1, ffFog.Color());
	if (u.fogDensity >= 0) glUniform1f(u.fogDensity, ffFog.Density());
	if (u.fogStart >= 0)   glUniform1f(u.fogStart, ffFog.Start());
	if (u.fogEnd >= 0)     glUniform1f(u.fogEnd, ffFog.End());
	if (u.fogScale >= 0)   glUniform1f(u.fogScale, ffFog.Scale());
}

void GL::InstallFFUniformFeed()
{
	if (origEnable != nullptr)
		return;

	// Unconditional, unlike the uniform feed below: the rasterization mirror only
	// SKIPS writes to state nothing can observe, which is sound whether or not
	// the rewrite is on, and it has to see every enable to know when that is.
	origEnable = glad_glEnable;
	origDisable = glad_glDisable;
	origPopAttrib = glad_glPopAttrib;
	glad_glEnable = &HookEnable;
	glad_glDisable = &HookDisable;
	glad_glPopAttrib = &HookPopAttrib;

	if (!FFRewriteEnabled())
		return;

	// Only while an experiment is selected: the per-draw readback is a
	// measurement cost, not something a shipping frame should pay.
	if (const int e = configHandler->GetInt("GLFFRemovalExperiment");
	    e == static_cast<int>(FFExperiment::MatrixUniform) || e == static_cast<int>(FFExperiment::MatrixUniformFromMirror)) {
		#define FFMVP_SWAP(name, params, args) \
			origMVP_##name = glad_##name; \
			glad_##name = &MVPHook_##name;
		FFMVP_DRAWS(FFMVP_SWAP)
		#undef FFMVP_SWAP
		LOG_L(L_WARNING, "[FFUniformFeed] MVP experiment armed: composing P*MV per draw");
	}

	origUseProgram = glad_glUseProgram;
	origLinkProgram = glad_glLinkProgram;
	origDeleteProgram = glad_glDeleteProgram;

	glad_glUseProgram = &FeedUseProgram;
	glad_glLinkProgram = &FeedLinkProgram;
	glad_glDeleteProgram = &FeedDeleteProgram;

	LOG_L(L_WARNING, "[FFUniformFeed] ACTIVE: rewritten fixed-function state is fed per program bind");
}

void GL::BindFFColorAttribLocation(uint32_t prog)
{
	if (!FFRewriteEnabled() || prog == 0)
		return;

	glBindAttribLocation(prog, FF_COLOR_ATTRIB_LOC, FF_COLOR_ATTRIB_NAME);
}

uint32_t GL::CurrentProgram()
{
	GLint prog = 0;
	glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
	return static_cast<uint32_t>(prog);
}

const GL::FFAttribBinding* GL::GetFFAttribBinding(uint32_t prog)
{
	// Deliberately not cached by program id: ids are recycled, so a widget that
	// deletes a shader and creates another would inherit the previous one's
	// layout and draw through the wrong attributes. Four introspection queries
	// against a driver hash table, on a path that is already issuing a draw
	// call, is not worth a cache that can be wrong.
	static thread_local FFAttribBinding b;

	if (prog == 0 || !FFRewriteEnabled())
		return nullptr;

	b.useAttrs  = glGetUniformLocation(prog, UNIFORM_USE);
	if (b.useAttrs < 0)
		return nullptr;

	b.vertex    = glGetAttribLocation(prog, ATTR_VERTEX);
	b.color     = glGetAttribLocation(prog, GL::FF_COLOR_ATTRIB_NAME);
	b.texCoord0 = glGetAttribLocation(prog, ATTR_TEXCRD);

	return b.Usable() ? &b : nullptr;
}

void GL::DrawFFAttribStream(uint32_t drawMode, const float* data, size_t vertCount, const FFAttribBinding& b)
{
	static constexpr GLsizei STRIDE = 9 * sizeof(float);

	static GLuint vao = 0;
	static GLuint vbo = 0;
	static int32_t live[3] = { -1, -1, -1 }; // locations the previous draw enabled

	if (vertCount == 0)
		return;

	if (vao == 0) {
		glGenVertexArrays(1, &vao);
		glGenBuffers(1, &vbo);
	}

	// unlike the no-shader flushes, this runs while a game's content owns the
	// state and may be mid-draw with a VAO of its own (gl.VAO)
	GLint prevVAO = 0;
	glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVAO);

	glBindVertexArray(vao);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, vertCount * 9 * sizeof(float), data, GL_STREAM_DRAW);

	for (int32_t& loc : live) {
		if (loc >= 0)
			glDisableVertexAttribArray(loc);
		loc = -1;
	}

	// gl_Vertex and gl_MultiTexCoord0 are vec4; GL fills the components the
	// pointer does not supply with (0,0,0,1), which is what glVertex3f and
	// glTexCoord2f give the fixed-function pipeline
	const auto bind = [&](int32_t loc, GLint size, size_t byteOffset, int slot) {
		if (loc < 0)
			return;
		glEnableVertexAttribArray(loc);
		glVertexAttribPointer(loc, size, GL_FLOAT, GL_FALSE, STRIDE, reinterpret_cast<void*>(byteOffset));
		live[slot] = loc;
	};
	bind(b.vertex,    3,  0, 0);
	bind(b.texCoord0, 2, 12, 1);
	bind(b.color,     4, 20, 2);

	glUniform1i(b.useAttrs, 1);
	glDrawArrays(drawMode, 0, static_cast<GLsizei>(vertCount));
	glUniform1i(b.useAttrs, 0);

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindVertexArray(prevVAO);
}
