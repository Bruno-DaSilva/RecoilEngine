/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

// Minimal stubs so the engine rendering path (RenderBuffers/Shader/VAO/VBO/
// StreamBuffer) can be linked into an offscreen unit test WITHOUT dragging in
// the GL window system, the VFS, or the config-file machinery. Only the
// handful of globals/symbols the render+shader-compile path actually touches
// are provided here, with conservative values.

#include "Rendering/GlobalRendering.h"
#include "System/Config/ConfigHandler.h"
#include "System/FileSystem/FileHandler.h"

#include <string>

// Never dereferenced on the RenderBuffer path: the only use in the shader
// compile path (Shader.cpp "UseShaderCache") is guarded by `oldValid &&`,
// which is false on a first-time compile. Providing the symbol is enough.
ConfigHandler* configHandler = nullptr;

// glClearErrors lives in myGL.cpp, which drags in CONFIG vars + CVertexArray;
// VBO.cpp only calls it to drain the GL error queue, so a no-op stub is fine.
void glClearErrors(const char* cls, const char* fnc, bool verbose) {}

// Shader.cpp's GetShaderSource (file/#include loading) is statically reachable
// from CreateShaderObject but never *called* for the RenderBuffer inline
// shaders. Stub the CFileHandler symbols it references (incl. the virtuals, so
// the vtable is emitted here) with inert bodies; none execute at runtime.
CFileHandler::CFileHandler(const std::string& fileName, const std::string& modes) {}
void CFileHandler::Close() {}
int CFileHandler::Read(void* buf, int length) { return 0; }
bool CFileHandler::TryReadFromPWD(const std::string& fileName) { return false; }
bool CFileHandler::TryReadFromRawFS(const std::string& fileName) { return false; }
bool CFileHandler::TryReadFromVFS(const std::string& fileName, int section) { return false; }

// A zeroed CGlobalRendering with just the fields the RenderBuffer/Shader path
// reads. Zero-init selects the conservative branch everywhere (no GL4, no
// persistent mapping, not AMD/Intel, etc.); we flip on only what's needed.
static CGlobalRendering* MakeStubGlobalRendering()
{
	alignas(CGlobalRendering) static unsigned char mem[sizeof(CGlobalRendering)] = {};
	auto* gr = reinterpret_cast<CGlobalRendering*>(mem);
	gr->supportExplicitAttribLoc = true; // shader uses explicit attrib locations
	gr->active = true;
	return gr;
}

CGlobalRendering* globalRendering = MakeStubGlobalRendering();
