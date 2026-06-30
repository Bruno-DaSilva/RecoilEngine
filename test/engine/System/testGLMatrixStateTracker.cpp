/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

// Tier-1 unit tests for the CPU-side matrix tracker that mirrors the
// fixed-function GL matrix stack (Phase 1 of the BAR modern-GL migration,
// see doc/bar-gl4-immediate-mode-inventory.md).
//
// These are pure CPU math: no GL context, no lua_State. The oracle is a
// hand-built CMatrix44f, exactly the engine's own matrix type whose GL
// convention correctness is already pinned by testMatrix44f /
// testRotationMatrix44f. What is under test here is that GLMatrixStateTracker
// *drives* CMatrix44f with the right OpenGL semantics:
//   - the fixed-function ops post-multiply (M' = M * Op, vertex = M*Op*v),
//   - push/pop is a bit-exact save/restore,
//   - the three matrix modes are independent stacks,
//   - depth over/underflow and balance match HasMatrixStateError().
//
// Contract dimensions covered: C1 (transform values) and C5 (matrix edge
// behavior) from the doc's compatibility contract.

#include "Rendering/GL/MatrixStateTracker.h"
#include "System/Matrix44f.h"
#include "System/float3.h"
#include "System/MathConstants.h"

#include <array>
#include <random>
#include <vector>

#include <catch_amalgamated.hpp>


// A generic, non-trivial, non-orthonormal modelview-like matrix with the
// m[3]==m[7]==0 invariant the engine's SSE matrix-multiply assumes for its
// right-hand operand. Built independently of the tracker.
static CMatrix44f MakeKnownMatrix()
{
	CMatrix44f m;
	m.Translate(1.0f, 2.0f, 3.0f);
	m.RotateZ(0.5f);
	m.Scale(float3(2.0f, 1.5f, 0.5f));
	return m;
}

// Explicit translation matrix (identity with a translation column).
static CMatrix44f MakeTranslateMatrix(float x, float y, float z)
{
	CMatrix44f t;
	t.SetPos(float3(x, y, z));
	return t;
}

// Explicit diagonal scale matrix.
static CMatrix44f MakeScaleMatrix(float x, float y, float z)
{
	CMatrix44f s;
	s[ 0] = x;
	s[ 5] = y;
	s[10] = z;
	return s;
}

// Pure rotation matrix about a (raw, unnormalized) axis, glRotatef-style:
// degrees in, axis normalized, R = rot * I.
static CMatrix44f MakeRotateMatrix(float angleDeg, float x, float y, float z)
{
	float3 axis(x, y, z);
	axis.Normalize();

	CMatrix44f r;
	r.Rotate(angleDeg * (math::PI / 180.0f), axis);
	return r;
}


TEST_CASE("GLMatrixTracker: default state is identity in MODELVIEW")
{
	GLMatrixStateTracker tr;

	CHECK(tr.GetMode() == GL_MODELVIEW);
	CHECK(tr.GetMatrix() == CMatrix44f());
	CHECK(tr.GetMatrix(GL_MODELVIEW)  == CMatrix44f());
	CHECK(tr.GetMatrix(GL_PROJECTION) == CMatrix44f());
	CHECK(tr.GetMatrix(GL_TEXTURE)    == CMatrix44f());
	CHECK_FALSE(tr.HasMatrixStateError());
}


TEST_CASE("GLMatrixTracker: LoadIdentity and LoadMatrix are bit-exact")
{
	GLMatrixStateTracker tr;
	const CMatrix44f m = MakeKnownMatrix();

	tr.LoadMatrix(m);
	CHECK(tr.GetMatrix() == m);

	tr.LoadIdentity();
	CHECK(tr.GetMatrix() == CMatrix44f());
}


TEST_CASE("GLMatrixTracker: Translate post-multiplies (order/side)")
{
	GLMatrixStateTracker tr;
	const CMatrix44f m = MakeKnownMatrix();

	tr.LoadMatrix(m);
	tr.Translate(2.0f, 3.0f, 4.0f);

	const CMatrix44f tmat   = MakeTranslateMatrix(2.0f, 3.0f, 4.0f);
	const CMatrix44f postMul = m * tmat; // glTranslatef: M' = M * T
	const CMatrix44f preMul  = tmat * m; // the wrong side

	CHECK(tr.GetMatrix().equals(postMul));
	CHECK_FALSE(tr.GetMatrix().equals(preMul));
}


TEST_CASE("GLMatrixTracker: Scale post-multiplies")
{
	GLMatrixStateTracker tr;
	const CMatrix44f m = MakeKnownMatrix();

	tr.LoadMatrix(m);
	tr.Scale(2.0f, 0.5f, 3.0f);

	const CMatrix44f smat = MakeScaleMatrix(2.0f, 0.5f, 3.0f);
	CHECK(tr.GetMatrix().equals(m * smat));
}


TEST_CASE("GLMatrixTracker: Rotate post-multiplies")
{
	GLMatrixStateTracker tr;
	const CMatrix44f m = MakeKnownMatrix();

	tr.LoadMatrix(m);
	tr.Rotate(37.0f, 0.0f, 0.0f, 1.0f);

	const CMatrix44f rmat = MakeRotateMatrix(37.0f, 0.0f, 0.0f, 1.0f);
	CHECK(tr.GetMatrix().equals(m * rmat));
}


TEST_CASE("GLMatrixTracker: Rotate normalizes its axis like glRotatef")
{
	GLMatrixStateTracker tr;
	tr.Rotate(37.0f, 0.0f, 0.0f, 5.0f); // unnormalized z-axis

	// rotation about +Z is identical whether the axis is (0,0,1) or (0,0,5)
	const CMatrix44f rmat = MakeRotateMatrix(37.0f, 0.0f, 0.0f, 1.0f);
	CHECK(tr.GetMatrix().equals(rmat));
}


TEST_CASE("GLMatrixTracker: MultMatrix post-multiplies")
{
	GLMatrixStateTracker tr;
	const CMatrix44f m = MakeKnownMatrix();
	const CMatrix44f n = MakeTranslateMatrix(-1.0f, 4.0f, 2.0f) * MakeScaleMatrix(1.5f, 1.5f, 1.5f);

	tr.LoadMatrix(m);
	tr.MultMatrix(n);

	CHECK(tr.GetMatrix().equals(m * n));
}


TEST_CASE("GLMatrixTracker: op sequence composes in GL order")
{
	GLMatrixStateTracker tr;

	tr.LoadIdentity();
	tr.Translate(10.0f, 0.0f, 0.0f);
	tr.Rotate(90.0f, 0.0f, 0.0f, 1.0f);
	tr.Scale(2.0f, 2.0f, 2.0f);

	CMatrix44f golden;
	golden = golden * MakeTranslateMatrix(10.0f, 0.0f, 0.0f);
	golden = golden * MakeRotateMatrix(90.0f, 0.0f, 0.0f, 1.0f);
	golden = golden * MakeScaleMatrix(2.0f, 2.0f, 2.0f);

	CHECK(tr.GetMatrix().equals(golden));

	// independent spot-check: a local-space point should map identically.
	const float4 p(1.0f, 0.0f, 0.0f, 1.0f);
	const float4 a = tr.GetMatrix() * p;
	const float4 b = golden * p;
	CHECK(a.x == Catch::Approx(b.x));
	CHECK(a.y == Catch::Approx(b.y));
	CHECK(a.z == Catch::Approx(b.z));
	CHECK(a.w == Catch::Approx(b.w));
}


TEST_CASE("GLMatrixTracker: push/pop round-trip is bit-exact")
{
	GLMatrixStateTracker tr;
	const CMatrix44f m = MakeKnownMatrix();

	tr.LoadMatrix(m);
	const CMatrix44f saved = tr.GetMatrix();

	REQUIRE(tr.PushMatrix());
	tr.Translate(5.0f, 6.0f, 7.0f);
	tr.Rotate(45.0f, 1.0f, 0.0f, 0.0f);
	CHECK(tr.GetMatrix() != saved);

	REQUIRE(tr.PopMatrix());
	CHECK(tr.GetMatrix() == saved);
}


TEST_CASE("GLMatrixTracker: nested push/pop restores each level")
{
	GLMatrixStateTracker tr;

	tr.LoadIdentity();
	tr.Translate(1.0f, 0.0f, 0.0f);
	const CMatrix44f l0 = tr.GetMatrix();

	REQUIRE(tr.PushMatrix());
	tr.Translate(0.0f, 2.0f, 0.0f);
	const CMatrix44f l1 = tr.GetMatrix();

	REQUIRE(tr.PushMatrix());
	tr.Scale(3.0f, 3.0f, 3.0f);
	CHECK(tr.GetMatrix() != l1);

	REQUIRE(tr.PopMatrix());
	CHECK(tr.GetMatrix() == l1);
	REQUIRE(tr.PopMatrix());
	CHECK(tr.GetMatrix() == l0);
}


TEST_CASE("GLMatrixTracker: matrix modes are independent stacks")
{
	GLMatrixStateTracker tr;

	const CMatrix44f mv  = MakeTranslateMatrix(1.0f, 0.0f, 0.0f);
	const CMatrix44f prj = MakeTranslateMatrix(0.0f, 2.0f, 0.0f);
	const CMatrix44f tex = MakeTranslateMatrix(0.0f, 0.0f, 3.0f);

	REQUIRE(tr.SetMatrixMode(GL_MODELVIEW));
	tr.LoadMatrix(mv);
	REQUIRE(tr.SetMatrixMode(GL_PROJECTION));
	tr.LoadMatrix(prj);
	REQUIRE(tr.SetMatrixMode(GL_TEXTURE));
	tr.LoadMatrix(tex);

	// mutating + push/pop one mode must not perturb the others
	REQUIRE(tr.PushMatrix());
	tr.Scale(9.0f, 9.0f, 9.0f);
	REQUIRE(tr.PopMatrix());

	CHECK(tr.GetMatrix(GL_MODELVIEW)  == mv);
	CHECK(tr.GetMatrix(GL_PROJECTION) == prj);
	CHECK(tr.GetMatrix(GL_TEXTURE)    == tex);
}


TEST_CASE("GLMatrixTracker: Ortho matches CMatrix44f::OrthoProj golden")
{
	GLMatrixStateTracker tr;
	tr.LoadIdentity();
	tr.Ortho(0.0, 1024.0, 768.0, 0.0, -1.0, 1.0);

	const CMatrix44f golden = CMatrix44f::OrthoProj(0.0f, 1024.0f, 768.0f, 0.0f, -1.0f, 1.0f);
	CHECK(tr.GetMatrix().equals(golden));
}


TEST_CASE("GLMatrixTracker: Frustum matches CMatrix44f::PerspProj golden")
{
	GLMatrixStateTracker tr;
	tr.LoadIdentity();
	tr.Frustum(-1.0, 1.0, -1.0, 1.0, 1.0, 100.0);

	const CMatrix44f golden = CMatrix44f::PerspProj(-1.0f, 1.0f, -1.0f, 1.0f, 1.0f, 100.0f);
	CHECK(tr.GetMatrix().equals(golden));
}


TEST_CASE("GLMatrixTracker: pop underflow is rejected")
{
	GLMatrixStateTracker tr;
	CHECK_FALSE(tr.PopMatrix());           // nothing pushed yet
	CHECK(tr.GetMatrix() == CMatrix44f()); // unchanged
}


TEST_CASE("GLMatrixTracker: push overflow is rejected at the depth cap")
{
	GLMatrixStateTracker tr;

	int pushed = 0;
	while (tr.PushMatrix())
		++pushed;

	CHECK(pushed == 255); // matches the legacy GLMatrixStateTracker cap

	// stack stays balanced afterwards
	for (int i = 0; i < pushed; ++i)
		CHECK(tr.PopMatrix());
	CHECK_FALSE(tr.PopMatrix());
}


TEST_CASE("GLMatrixTracker: HasMatrixStateError flags unbalanced state")
{
	GLMatrixStateTracker tr;
	CHECK_FALSE(tr.HasMatrixStateError());

	// unbalanced push => error
	REQUIRE(tr.PushMatrix());
	CHECK(tr.HasMatrixStateError());
	REQUIRE(tr.PopMatrix());
	CHECK_FALSE(tr.HasMatrixStateError());

	// left in the wrong mode => error
	REQUIRE(tr.SetMatrixMode(GL_PROJECTION));
	CHECK(tr.HasMatrixStateError());
	REQUIRE(tr.SetMatrixMode(GL_MODELVIEW));
	CHECK_FALSE(tr.HasMatrixStateError());
}


TEST_CASE("GLMatrixTracker: randomized op sequence matches an independent stack")
{
	// Drive the tracker through a long random sequence of ops/push/pop/mode
	// switches, mirroring each into an independent per-mode std::vector stack,
	// and assert the active matrix matches after every op. This exercises the
	// stack/mode bookkeeping far beyond the fixed cases above. Fixed seed =>
	// reproducible.
	std::mt19937 rng(0xC0FFEE);
	const auto rf = [&](float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(rng); };

	const unsigned modes[3] = {GL_MODELVIEW, GL_PROJECTION, GL_TEXTURE};

	GLMatrixStateTracker tr;
	std::array<std::vector<CMatrix44f>, 3> ref;
	for (auto& s : ref)
		s.emplace_back(); // identity "current" per mode
	int cur = 0;

	for (int i = 0; i < 3000; ++i) {
		switch (rng() % 6) {
			case 0: { // Translate
				const float x = rf(-5, 5), y = rf(-5, 5), z = rf(-5, 5);
				tr.Translate(x, y, z);
				ref[cur].back().Translate(x, y, z);
			} break;
			case 1: { // Scale
				const float s = rf(0.2f, 3.0f);
				tr.Scale(s, s, s);
				ref[cur].back().Scale(float3(s, s, s));
			} break;
			case 2: { // Rotate (tracker normalizes axis + deg->rad; mirror it)
				const float a = rf(-180.0f, 180.0f);
				float3 axis(rf(-1, 1), rf(-1, 1), rf(-1, 1));
				if (axis.SqLength() < 1e-4f)
					axis = float3(0.0f, 0.0f, 1.0f);
				float3 axisN = axis;
				axisN.Normalize();
				CMatrix44f rot;
				rot.Rotate(a * (math::PI / 180.0f), axisN);
				tr.Rotate(a, axis.x, axis.y, axis.z);
				ref[cur].back() = ref[cur].back() * rot;
			} break;
			case 3: { // Push
				if (tr.PushMatrix())
					ref[cur].push_back(ref[cur].back());
			} break;
			case 4: { // Pop (only when something is pushed)
				if (ref[cur].size() > 1) {
					REQUIRE(tr.PopMatrix());
					ref[cur].pop_back();
				}
			} break;
			case 5: { // switch matrix mode
				cur = rng() % 3;
				REQUIRE(tr.SetMatrixMode(modes[cur]));
			} break;
		}

		REQUIRE(tr.GetMatrix(modes[cur]).equals(ref[cur].back()));
	}
}


TEST_CASE("GLMatrixTracker: SetMatrixMode rejects invalid modes")
{
	GLMatrixStateTracker tr;
	CHECK(tr.SetMatrixMode(GL_PROJECTION));
	CHECK_FALSE(tr.SetMatrixMode(0x1800));   // GL_COLOR: not a tracked matrix stack
	CHECK(tr.GetMode() == GL_PROJECTION);    // mode unchanged on reject
}
