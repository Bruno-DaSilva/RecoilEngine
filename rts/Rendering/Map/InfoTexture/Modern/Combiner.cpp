/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "Combiner.h"
#include "Game/GlobalUnsynced.h"
#include "Rendering/GlobalRendering.h"
#include "Rendering/Shaders/ShaderHandler.h"
#include "Rendering/Shaders/Shader.h"
#include "Rendering/GL/SubState.h"
#include "Rendering/GL/FFShaderRewrite.h"
#include "Map/ReadMap.h"
#include "System/Exceptions.h"
#include "System/Config/ConfigHandler.h"
#include "System/Log/ILog.h"

#include "System/Misc/TracyDefs.h"


// set HighResInfoTexture, because games read that value
CONFIG(bool, HighResInfoTexture).defaultValue(true).deprecated(true);

CInfoTextureCombiner::CInfoTextureCombiner()
: CModernInfoTexture("info")
, disabled(true)
{
	texSize = int2(mapDims.pwr2mapx, mapDims.pwr2mapy);

	GL::TextureCreationParams tcp{
		.reqNumLevels = -1,
		.linearMipMapFilter = false,
		.linearTextureFilter = true,
		.wrapMirror = false
	};

	texture = GL::Texture2D(texSize, GL_RGB10_A2, tcp, false);

	if (FBO::IsSupported()) {
		fbo.Bind();
		fbo.AttachTexture(texture.GetId());
		/*bool status =*/ fbo.CheckStatus("CInfoTextureCombiner");
		glClearColor(0.5f, 0.5f, 0.5f, 1.0f);
		if (fbo.IsValid()) glClear(GL_COLOR_BUFFER_BIT);
		FBO::Unbind();

		// create mipmaps
		auto binding = texture.ScopedBind();
		texture.ProduceMipmaps();
	}

	shader = shaderHandler->CreateProgramObject("[CInfoTextureCombiner]", "CInfoTextureCombiner");

	if (!fbo.IsValid() /*|| !shader->IsValid()*/) { // don't check shader (it gets created/switched at runtime)
		throw opengl_error("");
	}
}

CInfoTextureCombiner::~CInfoTextureCombiner()
{
	RECOIL_DETAILED_TRACY_ZONE;
	shaderHandler->ReleaseProgramObject("[CInfoTextureCombiner]", "CInfoTextureCombiner");
}


void CInfoTextureCombiner::SwitchMode(const std::string& name)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (name.empty()) {
		disabled = true;

		CreateShader(curMode = "", true);
		return;
	}

	// WTF? fully reloaded from disk on every switch?
	// TODO: allow "info:myluainfotex"
	// pathDrawer->UpdateExtraTexture(texDrawMode, starty, endy, offset, infoTexMem);
	switch (hashString(name.c_str())) {
		case hashString("los"     ): { disabled = !CreateShader("shaders/GLSL/infoLOS.lua"   , true, float4(0.5f, 0.5f, 0.5f, 1.0f)); } break;
		case hashString("metal"   ): { disabled = !CreateShader("shaders/GLSL/infoMetal.lua" , true, float4(0.0f, 0.0f, 0.0f, 1.0f)); } break;
		case hashString("height"  ): { disabled = !CreateShader("shaders/GLSL/infoHeight.lua"                                      ); } break;
		case hashString("path"    ): { disabled = !CreateShader("shaders/GLSL/infoPath.lua"                                        ); } break;
		case hashString("heat"    ): [[fallthrough]];/*TODO ?*/
		case hashString("flow"    ): [[fallthrough]];/*TODO ?*/
		case hashString("pathcost"): [[fallthrough]];/*TODO ?*/
		default                    : { disabled = !CreateShader(name                                                               ); } break;
	}

	curMode = (disabled) ? "" : name;
}


bool CInfoTextureCombiner::CreateShader(const std::string& filename, const bool clear, const float4 clearColor)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (clear) {
		// clear
		fbo.Bind();
		glClearColor(clearColor.r, clearColor.g, clearColor.b, clearColor.a);
		if (fbo.IsValid()) glClear(GL_COLOR_BUFFER_BIT);
		FBO::Unbind();

		// create mipmaps
		auto binding = texture.ScopedBind();
		texture.ProduceMipmaps();
	}

	if (filename.empty())
		return false;

	shader->Release();
	return shader->LoadFromLua(filename);
}


void CInfoTextureCombiner::Update()
{
	RECOIL_DETAILED_TRACY_ZONE;

	using namespace GL::State;
	auto state = GL::SubState(
		Blending(GL_TRUE),
		ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE)
	);

	fbo.Bind();
	glViewport(0,0, texSize.x, texSize.y);

	shader->BindTextures();
	shader->Enable();
	shader->SetUniform("time", gu->gameTime);

	const float isx = 2.0f * (mapDims.mapx / float(mapDims.pwr2mapx)) - 1.0f;
	const float isy = 2.0f * (mapDims.mapy / float(mapDims.pwr2mapy)) - 1.0f;

	// The Lua-supplied combiner shader reads gl_Vertex and gl_MultiTexCoord0, so
	// immediate mode used to be the only way to deliver them. Where the source
	// rewrite reached it those come from generic attributes instead; where it
	// did not -- a shader reading a builtin with no attribute channel -- the
	// fixed-function quad still has to draw, which is what keeps an unchanged
	// game rendering. Corner texcoords are axis-aligned, so the two triangles
	// interpolate exactly as the quad did.
	const float corners[4][9] = {
		{ -1.f, -1.f, 0.f, 0.f, 0.f, 1.f, 1.f, 1.f, 1.f },
		{ -1.f, +isy, 0.f, 0.f, 1.f, 1.f, 1.f, 1.f, 1.f },
		{ +isx, +isy, 0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f },
		{ +isx, -1.f, 0.f, 1.f, 0.f, 1.f, 1.f, 1.f, 1.f },
	};

	if (const GL::FFAttribBinding* b = GL::GetFFAttribBinding(GL::CurrentProgram()); b != nullptr) {
		float tris[6][9];
		int n = 0;
		for (const int k : {0, 1, 2, 0, 2, 3})
			std::copy(corners[k], corners[k] + 9, tris[n++]);

		GL::DrawFFAttribStream(GL_TRIANGLES, &tris[0][0], 6, *b);
	} else {
		glBegin(GL_QUADS);
			for (const float(&c)[9] : corners) {
				glTexCoord2f(c[3], c[4]);
				glVertex2f(c[0], c[1]);
			}
		glEnd();
	}

	shader->Disable();
	shader->UnbindTextures();

	globalRendering->LoadViewport();
	FBO::Unbind();

	state.pop();
	// create mipmaps
	auto binding = texture.ScopedBind();
	texture.ProduceMipmaps();
}
