#version 120

// note: gl_ModelViewMatrix actually only contains the
// model matrix, view matrix is on the projection stack

varying vec4 vertexWorldPos;
varying vec3 cameraDir;
varying float fogFactor;
varying vec3 normalv;

#if (USE_SHADOWS == 1)
	uniform mat4 shadowMatrix;
	varying vec4 shadowVertexPos;
#endif

// Generic-attribute variant. The fixed-function vertex channels (gl_Vertex,
// gl_Normal, gl_MultiTexCoord0) can only be fed by immediate mode or client
// arrays, both of which RenderDoc rejects -- so a shader reading them forces the
// engine to keep those calls. These locations match S3DModelVAO's modern VAO
// exactly (0=pos, 1=normal, 4=uv), which is the same layout the GL4 template
// already uses; the engine binds that VAO instead of client arrays when
// ModernModelAttribs is on.
#if (GENERIC_ATTRIBS == 1)
	attribute vec3 aPos;
	attribute vec3 aNormal;
	attribute vec4 aUV;
	#define MDL_VERTEX vec4(aPos, 1.0)
	#define MDL_NORMAL aNormal
	#define MDL_TEXCOORD0 vec4(aUV.xy, 0.0, 1.0)
#else
	#define MDL_VERTEX gl_Vertex
	#define MDL_NORMAL gl_Normal
	#define MDL_TEXCOORD0 gl_MultiTexCoord0
#endif

void main(void)
{
	normalv = gl_NormalMatrix * MDL_NORMAL;

	gl_ClipVertex  = gl_ModelViewMatrix * MDL_VERTEX; // M (!)
	gl_Position    = gl_ProjectionMatrix * gl_ClipVertex;

	vertexWorldPos = gl_ClipVertex;

	vec4 cameraPos = gl_ProjectionMatrixInverse * vec4(0, 0, 0, 1); cameraPos.xyz /= cameraPos.w;

	cameraDir      = vertexWorldPos.xyz - cameraPos.xyz;

#if (USE_SHADOWS == 1)
	shadowVertexPos = shadowMatrix * vertexWorldPos;
	shadowVertexPos.xy += vec2(0.5);
#endif

	gl_TexCoord[0].st = MDL_TEXCOORD0.st;

#if (DEFERRED_MODE == 0)
	float fogCoord = length(cameraDir.xyz);
	fogFactor = (gl_Fog.end - fogCoord) * gl_Fog.scale; //gl_Fog.scale := 1.0 / (gl_Fog.end - gl_Fog.start)
	fogFactor = clamp(fogFactor, 0.0, 1.0);
#endif
}
