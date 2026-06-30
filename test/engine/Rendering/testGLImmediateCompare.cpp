/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

// Tier-2 offscreen A/B harness self-validation for the BAR modern-GL
// migration (see doc/bar-gl4-immediate-mode-inventory.md).
//
// Per the doc ("validate the harness before trusting it"), a comparator that
// gates rendering equivalence must be proven to BOTH confirm and deny before
// we rely on it:
//   - positive control: the same legacy primitive rendered twice is
//     byte-identical (proves the offscreen context + FBO + glReadPixels are
//     deterministic),
//   - negative control: two deliberately-different renders compare UNEQUAL,
//     and a single flipped texel is still detected (the comparator is not a
//     no-op that always passes).
//
// Shared harness (context/FBO/comparator/legacy helpers) lives in
// GLTestHarness.h. SDL2 + glad only; SKIPs if no GL context (headless CI box).
//
// CI recipe: xvfb-run -a -s "-screen 0 64x64x24" env LIBGL_ALWAYS_SOFTWARE=1
//            ctest -R testGLImmediateCompare   (Mesa llvmpipe, deterministic).

#include "GLTestHarness.h"

using namespace gltest;


TEST_CASE("GLImmediateCompare: offscreen context + FBO readback works")
{
	if (!EnsureGL())
		SKIP("no usable OpenGL context (headless box); skipping GL harness");

	// clear to exact bytes (255,0,0,255) -> no rounding ambiguity
	const auto buf = RenderToBuffer([] { ClearTo(1.0f, 0.0f, 0.0f, 1.0f); });

	REQUIRE(buf.size() == size_t(kSize) * kSize * 4);
	// sample the center texel
	const size_t c = (size_t(kSize / 2) * kSize + kSize / 2) * 4;
	CHECK(buf[c + 0] == 255);
	CHECK(buf[c + 1] == 0);
	CHECK(buf[c + 2] == 0);
	CHECK(buf[c + 3] == 255);
}


TEST_CASE("GLImmediateCompare: positive control — identical renders are byte-identical")
{
	if (!EnsureGL())
		SKIP("no usable OpenGL context (headless box); skipping GL harness");

	const auto scene = [] {
		ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
		SetupOrtho(kSize, kSize);
		DrawRectLegacy(32, 32, 200, 160, 1.0f, 0.5f, 0.25f, 1.0f);
	};

	const auto a = RenderToBuffer(scene);
	const auto b = RenderToBuffer(scene);

	const DiffResult d = Compare(a, b);
	INFO("firstDiffByte=" << d.firstDiffByte << " maxAbsDelta=" << d.maxAbsDelta << " diffBytes=" << d.diffBytes);
	CHECK(d.equal);
	CHECK(d.maxAbsDelta == 0);
	CHECK(d.diffBytes == 0);
}


TEST_CASE("GLImmediateCompare: negative control — different renders compare UNEQUAL")
{
	if (!EnsureGL())
		SKIP("no usable OpenGL context (headless box); skipping GL harness");

	const auto sceneA = [] {
		ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
		SetupOrtho(kSize, kSize);
		DrawRectLegacy(32, 32, 200, 160, 1.0f, 0.5f, 0.25f, 1.0f);
	};
	// same geometry, shifted right + recolored: must register as different.
	const auto sceneB = [] {
		ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
		SetupOrtho(kSize, kSize);
		DrawRectLegacy(48, 32, 216, 160, 0.25f, 1.0f, 0.5f, 1.0f);
	};

	const auto a = RenderToBuffer(sceneA);
	const auto b = RenderToBuffer(sceneB);

	const DiffResult d = Compare(a, b);
	INFO("maxAbsDelta=" << d.maxAbsDelta << " diffBytes=" << d.diffBytes);
	CHECK_FALSE(d.equal);    // the comparator MUST be able to deny
	CHECK(d.maxAbsDelta > 0);
	CHECK(d.diffBytes > 0);
}


TEST_CASE("GLImmediateCompare: comparator denies a single-pixel difference")
{
	if (!EnsureGL())
		SKIP("no usable OpenGL context (headless box); skipping GL harness");

	// guard against a comparator that only catches large/global differences:
	// a 1-texel delta must still register.
	auto a = RenderToBuffer([] { ClearTo(0.0f, 0.0f, 0.0f, 1.0f); });
	auto b = a;
	b[(size_t(10) * kSize + 10) * 4 + 1] ^= 0xFF; // flip one green byte

	const DiffResult d = Compare(a, b);
	CHECK_FALSE(d.equal);
	CHECK(d.diffBytes == 1);
	CHECK(d.maxAbsDelta == 255);
}
