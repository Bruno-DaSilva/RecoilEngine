#pragma once

#include <memory>

#include "Rendering/GL/VertexArrayTypes.h"
#include "Rendering/GL/RenderBuffers.h"

class CglFont;
class CFontTexture;
class CglFontRenderer {
public:
	virtual ~CglFontRenderer() = default;

	virtual void AddQuadTrianglesPB(VA_TYPE_TC&& tl, VA_TYPE_TC&& tr, VA_TYPE_TC&& br, VA_TYPE_TC&& bl) = 0;
	virtual void AddQuadTrianglesOB(VA_TYPE_TC&& tl, VA_TYPE_TC&& tr, VA_TYPE_TC&& br, VA_TYPE_TC&& bl) = 0;
	virtual void DrawTraingleElements() = 0;
	virtual void HandleTextureUpdate(CFontTexture& font, bool onlyUpload) = 0;
	virtual void PushGLState(const CglFont& font) = 0;
	virtual void PopGLState(const CglFont& font) = 0;

	virtual bool IsLegacy() const = 0;
	virtual bool IsValid() const = 0;
	virtual void GetStats(std::array<size_t, 8>& stats) const = 0;

	void SetUserDefinedBlending(bool enableUserDefinedBlending) { userDefinedBlending = enableUserDefinedBlending; };

	static std::unique_ptr<CglFontRenderer> CreateInstance();
	static void DeleteInstance(std::unique_ptr<CglFontRenderer>& instance);
protected:
	GLint currProgID = 0;
	bool userDefinedBlending = false;

	// Saved by PushGLState in place of glPushAttrib(GL_ENABLE_BIT |
	// GL_COLOR_BUFFER_BIT), which RenderDoc rejects. Only the states the bracket
	// actually modifies need saving; the queries that read them
	// (glIsEnabled/glGetIntegerv) are not on the unsupported list.
	struct SavedGLState {
		GLboolean depthTest = GL_FALSE;
		GLboolean texture2D = GL_FALSE;
		GLboolean alphaTest = GL_FALSE;
		GLboolean blend = GL_FALSE;
		GLint blendSrcRGB = GL_ONE, blendDstRGB = GL_ZERO;
		GLint blendSrcAlpha = GL_ONE, blendDstAlpha = GL_ZERO;
	} savedState;

	// should be enough to hold all data for a given frame
	static constexpr size_t NUM_BUFFER_ELEMS = (1 << 14);
	static constexpr size_t NUM_TRI_BUFFER_VERTS = (4 * NUM_BUFFER_ELEMS);
	static constexpr size_t NUM_TRI_BUFFER_ELEMS = (6 * NUM_BUFFER_ELEMS);
};

namespace Shader {
	struct IProgramObject;
};
class CglShaderFontRenderer final: public CglFontRenderer {
public:
	CglShaderFontRenderer();
	~CglShaderFontRenderer() override;

	void AddQuadTrianglesPB(VA_TYPE_TC&& tl, VA_TYPE_TC&& tr, VA_TYPE_TC&& br, VA_TYPE_TC&& bl) override;
	void AddQuadTrianglesOB(VA_TYPE_TC&& tl, VA_TYPE_TC&& tr, VA_TYPE_TC&& br, VA_TYPE_TC&& bl) override;
	void DrawTraingleElements() override;
	void HandleTextureUpdate(CFontTexture& font, bool onlyUpload) override;
	void PushGLState(const CglFont& font) override;
	void PopGLState(const CglFont& font) override;

	bool IsLegacy() const override { return false; }
	bool IsValid() const override { return fontShader->IsValid(); }
	void GetStats(std::array<size_t, 8>& stats) const override;

	// Runtime toggle of the uMVP branch (default from config FontUseMVPUniform);
	// used by the whole-frame A/B compare to render text both ways. Read per-draw
	// in PushGLState, so flipping it takes effect on the next draw.
	static void SetUseMVPUniform(bool b) { useMVPUniform = b; }
	static bool GetUseMVPUniform() { return useMVPUniform; }
private:
	// Flush used while a display list is being COMPILED (gl.CreateList bodies with
	// font:Print / gl.Text inside -- a common BAR widget pattern). The normal
	// RenderBuffer flush is not list-safe on two counts: (1) the recorded
	// glDrawElements aliases the streaming font VBO at fixed offsets, so the replay
	// shows whatever glyph data occupies those offsets later; (2) glUseProgram /
	// glUniform* get RECORDED into the list instead of executing, so they stomp
	// shader state at replay time and desync the program's CPU-side uniform cache
	// from the GPU (PushGLState therefore skips the whole program path during a
	// compile). Instead emit plain fixed-function immediate mode, which the list
	// captures by value and replays against the replay-time FF matrices -- the
	// exact semantics widgets expect. Texel->UV scaling uses the same shared
	// texture-space-matrix display list trick as CglNoShaderFontRenderer, so
	// already-recorded lists survive atlas resizes.
	void DrawTraingleElementsRecordable();
	// set by PushGLState when called during a display-list compile; PopGLState
	// then skips the (never-executed) shader disable/restore as well
	bool inListCompile = false;

	TypedRenderBuffer<VA_TYPE_TC> primaryBufferTC;
	TypedRenderBuffer<VA_TYPE_TC> outlineBufferTC;

	// 0 until the recordable flush is first needed. That flush only runs inside
	// an open display-list compile, so a run that compiles none -- the target
	// state for RenderDoc capturability, where one glGenLists anywhere is enough
	// to keep the whole display-list family on the unsupported list -- must not
	// pay for this list at all.
	uint32_t ffTextureSpaceMatrix = 0u;
	bool wantTextureSpaceMatrix = false;
	// atlas size the list was last compiled for: recompiling it on EVERY
	// HandleTextureUpdate was ~190 glNewList compiles per frame of BAR UI.
	// Tracked even while the list does not exist, so the first recordable flush
	// has a scale to fall back on.
	int texMatListW = 0, texMatListH = 0;

	static inline size_t fontShaderRefs = 0;
	static inline std::unique_ptr<Shader::IProgramObject> fontShader = nullptr;
	static inline size_t fontShaderColorRefs = 0;
	static inline std::unique_ptr<Shader::IProgramObject> fontShaderColor = nullptr;

	// Phase-1 modern-GL migration: when set (config FontUseMVPUniform), glyphs
	// transform by the uMVP uniform fed from the fixed-function bridge instead of
	// gl_ModelViewProjectionMatrix. Latched at first construction (shaders are
	// built once); off => byte-identical legacy path.
	static inline bool useMVPUniform = false;
};

class CglNoShaderFontRenderer final: public CglFontRenderer {
public:
	CglNoShaderFontRenderer();
	~CglNoShaderFontRenderer() override;

	void AddQuadTrianglesPB(VA_TYPE_TC&& tl, VA_TYPE_TC&& tr, VA_TYPE_TC&& br, VA_TYPE_TC&& bl) override;
	void AddQuadTrianglesOB(VA_TYPE_TC&& tl, VA_TYPE_TC&& tr, VA_TYPE_TC&& br, VA_TYPE_TC&& bl) override;
	void DrawTraingleElements() override;
	void HandleTextureUpdate(CFontTexture& font, bool onlyUpload) override;
	void PushGLState(const CglFont& font) override;
	void PopGLState(const CglFont& font) override;

	bool IsLegacy() const override { return true; }
	bool IsValid() const override { return true; }
	void GetStats(std::array<size_t, 8>& stats) const override;
private:
	void AddQuadTrianglesImpl(bool primary, VA_TYPE_TC&& tl, VA_TYPE_TC&& tr, VA_TYPE_TC&& br, VA_TYPE_TC&& bl);

	std::array<std::vector<VA_TYPE_TC>, 2> verts; // OL, PM
	std::array<std::vector<uint16_t  >, 2> indcs; // OL, PM

	uint32_t textureSpaceMatrix = 0u;
	// see CglShaderFontRenderer::texMatListW
	int texMatListW = 0, texMatListH = 0;
};

class CglNullFontRenderer final : public CglFontRenderer {
	// Inherited via CglFontRenderer
	void AddQuadTrianglesPB(VA_TYPE_TC&& tl, VA_TYPE_TC&& tr, VA_TYPE_TC&& br, VA_TYPE_TC&& bl) override {}
	void AddQuadTrianglesOB(VA_TYPE_TC&& tl, VA_TYPE_TC&& tr, VA_TYPE_TC&& br, VA_TYPE_TC&& bl) override {}
	void DrawTraingleElements() override {}
	void HandleTextureUpdate(CFontTexture& font, bool onlyUpload) override {}
	void PushGLState(const CglFont& font) override {}
	void PopGLState(const CglFont& font) override {}
	bool IsLegacy() const override { return true; }
	bool IsValid() const override { return true; }
	void GetStats(std::array<size_t, 8>& stats) const override;
};
