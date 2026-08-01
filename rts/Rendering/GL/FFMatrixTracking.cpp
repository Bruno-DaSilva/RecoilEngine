/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "FFMatrixTracking.h"

#include "Rendering/GL/myGL.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>

#include "Rendering/GL/FFShaderRewrite.h"
#include "Rendering/GL/MatrixStateTracker.h"
#include "System/Config/ConfigHandler.h"
#include "System/Log/ILog.h"

namespace {
	bool installed = false;
	bool suppressed = false;

	// while set, the calls below are OUR OWN restore of the mirrored state: they
	// must reach GL and must not be mirrored back, or the mirror would drift by
	// its own replay every time a draw fell back
	bool materializing = false;

	GLenum ModeMatrixEnum(GLenum mode)
	{
		switch (mode) {
			case GL_PROJECTION: return GL_PROJECTION_MATRIX;
			case GL_TEXTURE:    return GL_TEXTURE_MATRIX;
			default:            return GL_MODELVIEW_MATRIX;
		}
	}

	// A gl.CreateList body's matrix ops are recorded rather than executed, so the
	// mirror must not see them -- the same reason FFMirrorOps() stood down there.
	bool Tracking() { return !GL::ffListBodyOpen && !materializing; }

	// With the family suppressed the call is mirrored and then NOT made. A list
	// body is the exception: there the call is being recorded for a replay that
	// still runs through fixed function, so it has to reach GL.
	bool IssueToGL() { return !suppressed || GL::ffListBodyOpen || materializing; }

	#define FFMAT_ENTRIES(X) \
		X(glMatrixMode,   (GLenum a), (a)) \
		X(glPushMatrix,   (), ()) \
		X(glPopMatrix,    (), ()) \
		X(glLoadIdentity, (), ()) \
		X(glLoadMatrixf,  (const GLfloat* m), (m)) \
		X(glMultMatrixf,  (const GLfloat* m), (m)) \
		X(glLoadMatrixd,  (const GLdouble* m), (m)) \
		X(glMultMatrixd,  (const GLdouble* m), (m)) \
		X(glTranslatef,   (GLfloat x, GLfloat y, GLfloat z), (x, y, z)) \
		X(glScalef,       (GLfloat x, GLfloat y, GLfloat z), (x, y, z)) \
		X(glRotatef,      (GLfloat a, GLfloat x, GLfloat y, GLfloat z), (a, x, y, z)) \
		X(glTranslated,   (GLdouble x, GLdouble y, GLdouble z), (x, y, z)) \
		X(glScaled,       (GLdouble x, GLdouble y, GLdouble z), (x, y, z)) \
		X(glRotated,      (GLdouble a, GLdouble x, GLdouble y, GLdouble z), (a, x, y, z)) \
		X(glOrtho,        (GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f), (l, r, b, t, n, f)) \
		X(glFrustum,      (GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f), (l, r, b, t, n, f))

	#define FFMAT_DECL_ORIG(name, params, args) decltype(glad_##name) orig_##name = nullptr;
	FFMAT_ENTRIES(FFMAT_DECL_ORIG)
	#undef FFMAT_DECL_ORIG

	// CMatrix44f has NO constructor from a float pointer, and passing one
	// compiles anyway: the pointer converts to float3 and picks the
	// position-only constructor, so a 4x4 load silently becomes a TRANSLATION
	// built from the first three floats. That is what the verifier's first gross
	// mismatches were, and it is the same conversion trap float3's
	// array-assignment operator sets elsewhere.
	CMatrix44f FromFloats(const GLfloat* m)
	{
		CMatrix44f out;
		std::copy(m, m + 16, out.m);
		return out;
	}

	CMatrix44f FromDoubles(const GLdouble* m)
	{
		CMatrix44f out;
		for (int i = 0; i < 16; ++i)
			out.m[i] = static_cast<float>(m[i]);
		return out;
	}

	// Every wrapper below runs the real call FIRST and mirrors after, so a GL
	// error (an over-deep pop, say) leaves the mirror describing what GL did
	// rather than what the caller asked for.
	void APIENTRY Trk_glMatrixMode(GLenum mode)
	{
		if (IssueToGL())
			orig_glMatrixMode(mode);

		if (!Tracking())
			return;
		GL::ffMirror.tracker.SetMatrixMode(mode);
	}

	void APIENTRY Trk_glPushMatrix()
	{
		if (IssueToGL())
			orig_glPushMatrix();

		if (Tracking()) { GL::ffMirror.tracker.PushMatrix(); GL::VerifyFFMatrixMirror("glPushMatrix"); }
	}

	void APIENTRY Trk_glPopMatrix()
	{
		if (IssueToGL())
			orig_glPopMatrix();

		if (Tracking()) { GL::ffMirror.tracker.PopMatrix(); GL::VerifyFFMatrixMirror("glPopMatrix"); }
	}

	void APIENTRY Trk_glLoadIdentity()
	{
		if (IssueToGL())
			orig_glLoadIdentity();

		if (Tracking()) { GL::ffMirror.tracker.LoadIdentity(); GL::VerifyFFMatrixMirror("glLoadIdentity"); }
	}

	void APIENTRY Trk_glLoadMatrixf(const GLfloat* m)
	{
		if (IssueToGL())
			orig_glLoadMatrixf(m);

		if (Tracking()) { GL::ffMirror.tracker.LoadMatrix(FromFloats(m)); GL::VerifyFFMatrixMirror("glLoadMatrixf"); }
	}

	void APIENTRY Trk_glMultMatrixf(const GLfloat* m)
	{
		if (IssueToGL())
			orig_glMultMatrixf(m);

		if (Tracking()) { GL::ffMirror.tracker.MultMatrix(FromFloats(m)); GL::VerifyFFMatrixMirror("glMultMatrixf"); }
	}

	void APIENTRY Trk_glLoadMatrixd(const GLdouble* m)
	{
		if (IssueToGL())
			orig_glLoadMatrixd(m);

		if (Tracking()) { GL::ffMirror.tracker.LoadMatrix(FromDoubles(m)); GL::VerifyFFMatrixMirror("glLoadMatrixd"); }
	}

	void APIENTRY Trk_glMultMatrixd(const GLdouble* m)
	{
		if (IssueToGL())
			orig_glMultMatrixd(m);

		if (Tracking()) { GL::ffMirror.tracker.MultMatrix(FromDoubles(m)); GL::VerifyFFMatrixMirror("glMultMatrixd"); }
	}

	void APIENTRY Trk_glTranslatef(GLfloat x, GLfloat y, GLfloat z)
	{
		if (IssueToGL())
			orig_glTranslatef(x, y, z);

		if (Tracking()) { GL::ffMirror.tracker.Translate(x, y, z); GL::VerifyFFMatrixMirror("glTranslatef"); }
	}

	void APIENTRY Trk_glScalef(GLfloat x, GLfloat y, GLfloat z)
	{
		if (IssueToGL())
			orig_glScalef(x, y, z);

		if (Tracking()) { GL::ffMirror.tracker.Scale(x, y, z); GL::VerifyFFMatrixMirror("glScalef"); }
	}

	void APIENTRY Trk_glRotatef(GLfloat a, GLfloat x, GLfloat y, GLfloat z)
	{
		if (IssueToGL())
			orig_glRotatef(a, x, y, z);

		if (Tracking()) { GL::ffMirror.tracker.Rotate(a, x, y, z); GL::VerifyFFMatrixMirror("glRotatef"); }
	}

	void APIENTRY Trk_glTranslated(GLdouble x, GLdouble y, GLdouble z)
	{
		if (IssueToGL())
			orig_glTranslated(x, y, z);

		if (Tracking()) { GL::ffMirror.tracker.Translate(x, y, z); GL::VerifyFFMatrixMirror("glTranslated"); }
	}

	void APIENTRY Trk_glScaled(GLdouble x, GLdouble y, GLdouble z)
	{
		if (IssueToGL())
			orig_glScaled(x, y, z);

		if (Tracking()) { GL::ffMirror.tracker.Scale(x, y, z); GL::VerifyFFMatrixMirror("glScaled"); }
	}

	void APIENTRY Trk_glRotated(GLdouble a, GLdouble x, GLdouble y, GLdouble z)
	{
		if (IssueToGL())
			orig_glRotated(a, x, y, z);

		if (Tracking()) { GL::ffMirror.tracker.Rotate(a, x, y, z); GL::VerifyFFMatrixMirror("glRotated"); }
	}

	void APIENTRY Trk_glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f)
	{
		if (IssueToGL())
			orig_glOrtho(l, r, b, t, n, f);

		if (Tracking()) { GL::ffMirror.tracker.Ortho(l, r, b, t, n, f, false); GL::VerifyFFMatrixMirror("glOrtho"); }
	}

	void APIENTRY Trk_glFrustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f)
	{
		if (IssueToGL())
			orig_glFrustum(l, r, b, t, n, f);

		if (Tracking()) { GL::ffMirror.tracker.Frustum(l, r, b, t, n, f, false); GL::VerifyFFMatrixMirror("glFrustum"); }
	}
}

CONFIG(bool, FFMatrixSuppress).defaultValue(true).safemodeValue(false)
	.description("Stop issuing the fixed-function matrix calls (glMatrixMode, glPushMatrix, glLoadMatrixf, ...) and serve every consumer from the CPU mirror instead. These ten are the last RenderDoc-unsupported functions a BAR frame makes, so with this and FFVertexAttribRewrite on the count is zero. Defaults ON because it is INERT without FFVertexAttribRewrite -- the tracking that feeds the mirror is not even installed then -- so a game that has not enabled the migration is untouched either way.");

void GL::InstallFFMatrixTracking()
{
	if (installed || !FFRewriteEnabled())
		return;

	installed = true;
	suppressed = configHandler->GetBool("FFMatrixSuppress");

	#define FFMAT_SWAP(name, params, args) \
		orig_##name = glad_##name; \
		glad_##name = &Trk_##name;
	FFMAT_ENTRIES(FFMAT_SWAP)
	#undef FFMAT_SWAP

	LOG_L(L_WARNING, "[FFMatrixTracking] ACTIVE: GL::ffMirror is fed from the glad entry points%s",
		suppressed ? ", and the calls are NOT issued" : "");
}

bool GL::FFMatrixTrackingInstalled()
{
	return installed;
}

void GL::MaterializeFFMatrices()
{
	if (!suppressed)
		return;

	materializing = true;

	const int savedMode = ffMirror.tracker.GetMatrixState().mode;
	for (const GLenum mode : { GL_PROJECTION, GL_MODELVIEW, GL_TEXTURE }) {
		glMatrixMode(mode);
		glLoadMatrixf(static_cast<const float*>(ffMirror.tracker.GetMatrix(mode)));
	}
	glMatrixMode(static_cast<GLenum>(savedMode));

	materializing = false;
}

void GL::ReadFFMatrix(unsigned int mode, CMatrix44f& out)
{
	if (FFMatrixSuppressed()) {
		out = ffMirror.tracker.GetMatrix(mode);
		return;
	}

	glGetFloatv(ModeMatrixEnum(static_cast<GLenum>(mode)), static_cast<float*>(out));
}

void GL::ReadFFMatrices(CMatrix44f& proj, CMatrix44f& modelView)
{
	ReadFFMatrix(GL_PROJECTION, proj);
	ReadFFMatrix(GL_MODELVIEW, modelView);
}

bool GL::FFMatrixSuppressed()
{
	return suppressed;
}

void GL::VerifyFFMatrixMirror(const char* op)
{
	// Once the calls are suppressed GL holds an identity and is no longer an
	// oracle for anything; the mirror IS the state.
	if (!ffMirror.shadowCompare || !installed || suppressed)
		return;

	static bool reported = false;
	if (!reported) {
		reported = true;
		LOG_L(L_WARNING, "[FFMatrixTracking] VERIFY ACTIVE: mirror vs glGetFloatv, exact compare");
	}

	// The mode comes from the tracker, not from a copy kept here: FinishSeed()
	// sets it too, and a private copy silently reads back the wrong matrix from
	// that point on -- which is what the first run of this verifier reported, as
	// four ops "mismatching" by whole matrices.
	const GLenum mode = static_cast<GLenum>(ffMirror.tracker.GetMatrixState().mode);

	float live[16] = {};
	glGetFloatv(ModeMatrixEnum(mode), live);

	const CMatrix44f& mirrored = ffMirror.tracker.GetMatrix(mode);
	if (std::memcmp(live, mirrored.m, sizeof(live)) == 0)
		return;

	// Per OP rather than per occurrence: which call the tracker models wrongly is
	// the whole content of the report, and one of them fires 130k times a run.
	static std::unordered_map<std::string, int> reportedOps;
	if (int& n = reportedOps[op]; n++ > 0)
		return;

	float maxAbs = 0.0f;
	float maxRel = 0.0f;
	for (int i = 0; i < 16; ++i) {
		const float d = std::fabs(live[i] - mirrored.m[i]);
		maxAbs = std::max(maxAbs, d);
		maxRel = std::max(maxRel, d / std::max(1.0f, std::fabs(live[i])));
	}

	// GL's own mode as well as the mirror's: a mode desync and a value desync
	// look identical in the numbers and need opposite fixes.
	GLint glMode = 0;
	glGetIntegerv(GL_MATRIX_MODE, &glMode);

	LOG_L(L_WARNING, "[FFMatrixTracking] MISMATCH after %s (mirror mode 0x%x, GL mode 0x%x): max abs %g, max rel %g",
		op, mode, glMode, maxAbs, maxRel);
}
