/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "FFShaderRewrite.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <vector>

#include "Rendering/GL/myGL.h"
#include "System/Config/ConfigHandler.h"
#include "System/Log/ILog.h"

CONFIG(bool, FFVertexAttribRewrite).defaultValue(false).safemodeValue(false)
	.description("Rewrite the fixed-function vertex builtins (gl_Vertex, gl_Color, gl_MultiTexCoord0, ftransform) in compiled GLSL to generic attributes, so shader-bound immediate-mode draws can be fed without glBegin/glVertex. Off leaves every shader source byte-identical.");

namespace {
	constexpr const char* ATTR_VERTEX = "recoil_ff_aVertex";
	constexpr const char* ATTR_COLOR  = "recoil_ff_aColor";
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

	// Insert after the #version directive, or at the very top when the caller
	// already stripped it. Returns the byte offset and the source line the next
	// line then carries, so #line can be re-anchored and a compile error in the
	// game's own source still reports the line its author wrote.
	std::pair<size_t, int> PrologueInsertPoint(const std::string& code)
	{
		const size_t v = FindVersionDirective(code);
		if (v == std::string::npos)
			return { 0, 1 };

		const size_t eol = code.find('\n', v);
		if (eol == std::string::npos)
			return { code.size(), 1 };

		int line = 1;
		for (size_t i = 0; i <= eol; ++i)
			line += (code[i] == '\n');

		return { eol + 1, line };
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

bool GL::RewriteFFVertexBuiltins(std::string& src, int glslVersion)
{
	if (!FFRewriteEnabled())
		return false;

	// every decision below reads the MASKED source, so commented-out code neither
	// triggers a rewrite nor gets one
	const std::string code = MaskComments(src);

	const bool usesFtransform = HasIdent(code, "ftransform");
	const bool usesVertex = usesFtransform || HasIdent(code, "gl_Vertex");
	const bool usesColor  = HasIdent(code, "gl_Color");
	const bool usesTexCrd = HasIdent(code, "gl_MultiTexCoord0");

	if (!usesVertex && !usesColor && !usesTexCrd)
		return false;

	for (const char* b : UNFED_BUILTINS) {
		if (!HasIdent(code, b))
			continue;

		LOG_L(L_DEBUG, "[FFRewrite] declining: shader reads %s, which has no attribute channel", b);
		return false;
	}

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

	// the insertion point is taken before any of this, off the same mask; every
	// site is below the #version line, so the offset survives the rewrite
	const auto [at, nextLine] = PrologueInsertPoint(code);

	std::sort(sites.begin(), sites.end(), [](const Site& a, const Site& b) { return a.pos > b.pos; });
	for (const Site& s : sites)
		src.replace(s.pos, s.len, s.repl);

	const char* in = (glslVersion >= 130) ? "in" : "attribute";

	// one physical line, so a compile error in the game's own source still
	// reports the line the author wrote (see the #line re-anchor below)
	std::string decl = "uniform bool " + std::string(UNIFORM_USE) + ";";
	if (usesVertex) {
		decl += std::string(in) + " vec4 " + ATTR_VERTEX + ";";
		decl += " vec4 recoil_ff_Vertex() { return " + std::string(UNIFORM_USE) + " ? " + ATTR_VERTEX + " : gl_Vertex; }";
	}
	if (usesFtransform) {
		// ftransform() is gl_ModelViewProjectionMatrix * gl_Vertex; the matrix
		// builtin is untouched here, only where the position comes from
		decl += " vec4 recoil_ff_Transform() { return gl_ModelViewProjectionMatrix * recoil_ff_Vertex(); }";
	}
	if (usesColor) {
		decl += std::string(in) + " vec4 " + ATTR_COLOR + ";";
		decl += " vec4 recoil_ff_Color() { return " + std::string(UNIFORM_USE) + " ? " + ATTR_COLOR + " : gl_Color; }";
	}
	if (usesTexCrd) {
		decl += std::string(in) + " vec4 " + ATTR_TEXCRD + ";";
		decl += " vec4 recoil_ff_MultiTexCoord0() { return " + std::string(UNIFORM_USE) + " ? " + ATTR_TEXCRD + " : gl_MultiTexCoord0; }";
	}

	src.insert(at, decl + "\n#line " + std::to_string(nextLine) + "\n");
	return true;
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
	b.color     = glGetAttribLocation(prog, ATTR_COLOR);
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
