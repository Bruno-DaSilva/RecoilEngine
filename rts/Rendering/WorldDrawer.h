/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

class CWorldDrawer
{
public:
	void InitPre() const;
	void InitPost() const;
	void Kill();

	void Update(bool newSimFrame);
	void Draw() const;

	void CreateShadowTextures() const;
	void GenerateIBLTextures() const;
	void ResetMVPMatrices() const;

private:
	void DrawOpaqueObjects() const;
	void DrawAlphaObjects() const;
	void DrawMiscObjects() const;
	void DrawBelowWaterOverlay() const;

private:
	unsigned int numUpdates = 0;
};
