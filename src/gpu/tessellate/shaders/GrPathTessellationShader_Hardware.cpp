/*
 * Copyright 2019 Google LLC.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "src/gpu/tessellate/shaders/GrPathTessellationShader.h"

#include "src/gpu/geometry/GrWangsFormula.h"
#include "src/gpu/glsl/GrGLSLGeometryProcessor.h"
#include "src/gpu/glsl/GrGLSLVertexGeoBuilder.h"
#include "src/gpu/tessellate/GrPathTessellator.h"

namespace {

// Converts keywords from shared SkSL strings to native GLSL keywords.
constexpr static char kSkSLTypeDefs[] = R"(
#define float4x3 mat4x3
#define float4x2 mat4x2
#define float3x2 mat3x2
#define float2x2 mat2
#define float2 vec2
#define float3 vec3
#define float4 vec4
)";

// Uses GPU tessellation shaders to linearize, triangulate, and render cubic "wedge" patches. A
// wedge is a 5-point patch consisting of 4 cubic control points, plus an anchor point fanning from
// the center of the curve's resident contour.
// TODO: Eventually we want to use rational cubic wedges in order to support perspective and conics.
class HardwareWedgeShader : public GrPathTessellationShader {
public:
    HardwareWedgeShader(const SkMatrix& viewMatrix, const SkPMColor4f& color)
            : GrPathTessellationShader(kTessellate_HardwareWedgeShader_ClassID,
                                       GrPrimitiveType::kPatches, 5, viewMatrix, color) {
        constexpr static Attribute kInputPointAttrib{"inputPoint", kFloat2_GrVertexAttribType,
                                                     kFloat2_GrSLType};
        this->setVertexAttributes(&kInputPointAttrib, 1);
    }

private:
    const char* name() const final { return "tessellate_HardwareWedgeShader"; }
    void getGLSLProcessorKey(const GrShaderCaps&, GrProcessorKeyBuilder*) const final {}
    GrGLSLGeometryProcessor* createGLSLInstance(const GrShaderCaps&) const final;
};

GrGLSLGeometryProcessor* HardwareWedgeShader::createGLSLInstance(const GrShaderCaps&) const {
    class Impl : public GrPathTessellationShader::Impl {
        void emitVertexCode(const GrPathTessellationShader&, GrGLSLVertexBuilder* v,
                            GrGPArgs*) override {
            v->declareGlobal(GrShaderVar("vsPt", kFloat2_GrSLType, GrShaderVar::TypeModifier::Out));
            v->codeAppend(R"(
            // If y is infinity then x is a conic weight. Don't transform.
            vsPt = (isinf(inputPoint.y)) ? inputPoint : AFFINE_MATRIX * inputPoint + TRANSLATE;)");
        }
        SkString getTessControlShaderGLSL(const GrGeometryProcessor&,
                                          const char* versionAndExtensionDecls,
                                          const GrGLSLUniformHandler&,
                                          const GrShaderCaps& shaderCaps) const override {
            SkString code(versionAndExtensionDecls);
            code.appendf(R"(
            #define MAX_TESSELLATION_SEGMENTS %i)", shaderCaps.maxTessellationSegments());
            code.appendf(R"(
            #define PRECISION %f)", GrPathTessellator::kLinearizationPrecision);
            code.append(kSkSLTypeDefs);
            code.append(GrWangsFormula::as_sksl());
            code.append(kUnpackRationalCubicFn);
            code.append(R"(
            layout(vertices = 1) out;

            in vec2 vsPt[];
            patch out mat4x2 rationalCubicXY;
            patch out float rationalCubicW;
            patch out vec2 fanpoint;

            void main() {
                mat4x2 P = mat4x2(vsPt[0], vsPt[1], vsPt[2], vsPt[3]);
                float numSegments;
                if (isinf(P[3].y)) {
                    // This is a conic.
                    float w = P[3].x;
                    numSegments = wangs_formula_conic(PRECISION, mat3x2(P), w);
                    // Convert to a rational cubic in projected form.
                    rationalCubicXY = mat4x2(P[0],
                                             mix(vec4(P[0], P[2]), (P[1] * w).xyxy, 2.0/3.0),
                                             P[2]);
                    rationalCubicW = fma(w, 2.0/3.0, 1.0/3.0);
                } else {
                    // This is a cubic.
                    numSegments = wangs_formula_cubic(PRECISION, P, mat2(1));
                    rationalCubicXY = P;
                    rationalCubicW = 1;
                }
                fanpoint = vsPt[4];

                // Tessellate the first side of the patch into numSegments triangles.
                gl_TessLevelOuter[0] = min(numSegments, MAX_TESSELLATION_SEGMENTS);

                // Leave the other two sides of the patch as single segments.
                gl_TessLevelOuter[1] = 1.0;
                gl_TessLevelOuter[2] = 1.0;

                // Changing the inner level to 1 when numSegments == 1 collapses the entire
                // patch to a single triangle. Otherwise, we need an inner level of 2 so our curve
                // triangles have an interior point to originate from.
                gl_TessLevelInner[0] = min(numSegments, 2.0);
            })");

            return code;
        }
        SkString getTessEvaluationShaderGLSL(const GrGeometryProcessor&,
                                             const char* versionAndExtensionDecls,
                                             const GrGLSLUniformHandler&,
                                             const GrShaderCaps&) const override {
            SkString code(versionAndExtensionDecls);
            code.append(kSkSLTypeDefs);
            code.append(kEvalRationalCubicFn);
            code.append(R"(
            layout(triangles, equal_spacing, ccw) in;

            uniform vec4 sk_RTAdjust;

            patch in mat4x2 rationalCubicXY;
            patch in float rationalCubicW;
            patch in vec2 fanpoint;

            void main() {
                // Locate our parametric point of interest. It is equal to the barycentric
                // y-coordinate if we are a vertex on the tessellated edge of the triangle patch,
                // 0.5 if we are the patch's interior vertex, or N/A if we are the fan point.
                // NOTE: We are on the tessellated edge when the barycentric x-coordinate == 0.
                float T = (gl_TessCoord.x == 0.0) ? gl_TessCoord.y : 0.5;

                mat4x3 P = mat4x3(rationalCubicXY[0], 1,
                                  rationalCubicXY[1], rationalCubicW,
                                  rationalCubicXY[2], rationalCubicW,
                                  rationalCubicXY[3], 1);
                vec2 vertexpos = eval_rational_cubic(P, T);

                if (gl_TessCoord.x == 1.0) {
                    // We are the anchor point that fans from the center of the curve's contour.
                    vertexpos = fanpoint;
                } else if (gl_TessCoord.x != 0.0) {
                    // We are the interior point of the patch; center it inside [C(0), C(.5), C(1)].
                    vertexpos = (P[0].xy + vertexpos + P[3].xy) / 3.0;
                }

                gl_Position = vec4(vertexpos * sk_RTAdjust.xz + sk_RTAdjust.yw, 0.0, 1.0);
            })");

            return code;
        }
    };
    return new Impl;
}

// Uses GPU tessellation shaders to linearize, triangulate, and render standalone closed cubics.
// TODO: Eventually we want to use rational cubic wedges in order to support perspective and conics.
class HardwareCurveShader : public GrPathTessellationShader {
public:
    HardwareCurveShader(const SkMatrix& viewMatrix, const SkPMColor4f& color)
            : GrPathTessellationShader(kTessellate_HardwareCurveShader_ClassID,
                                       GrPrimitiveType::kPatches, 4, viewMatrix, color) {
        constexpr static Attribute kInputPointAttrib{"inputPoint", kFloat2_GrVertexAttribType,
                                                     kFloat2_GrSLType};
        this->setVertexAttributes(&kInputPointAttrib, 1);
    }

private:
    const char* name() const final { return "tessellate_HardwareCurveShader"; }
    void getGLSLProcessorKey(const GrShaderCaps&, GrProcessorKeyBuilder*) const final {}
    GrGLSLGeometryProcessor* createGLSLInstance(const GrShaderCaps&) const final;
};

GrGLSLGeometryProcessor* HardwareCurveShader::createGLSLInstance(const GrShaderCaps&) const {
    class Impl : public GrPathTessellationShader::Impl {
        void emitVertexCode(const GrPathTessellationShader&, GrGLSLVertexBuilder* v,
                            GrGPArgs*) override {
            v->declareGlobal(GrShaderVar("P", kFloat2_GrSLType, GrShaderVar::TypeModifier::Out));
            v->codeAppend(R"(
            // If y is infinity then x is a conic weight. Don't transform.
            P = (isinf(inputPoint.y)) ? inputPoint : AFFINE_MATRIX * inputPoint + TRANSLATE;)");
        }
        SkString getTessControlShaderGLSL(const GrGeometryProcessor&,
                                          const char* versionAndExtensionDecls,
                                          const GrGLSLUniformHandler&,
                                          const GrShaderCaps& shaderCaps) const override {
            SkString code(versionAndExtensionDecls);
            code.appendf(R"(
            #define MAX_TESSELLATION_SEGMENTS %i)", shaderCaps.maxTessellationSegments());
            code.appendf(R"(
            #define PRECISION %f)", GrPathTessellator::kLinearizationPrecision);
            code.append(kSkSLTypeDefs);
            code.append(GrWangsFormula::as_sksl());
            code.append(kUnpackRationalCubicFn);
            code.append(R"(
            layout(vertices = 1) out;

            in vec2 P[];
            patch out mat4x2 rationalCubicXY;
            patch out float rationalCubicW;

            void main() {
                float w = -1;  // w<0 means a cubic.
                vec2 p1w = P[1];
                if (isinf(P[3].y)) {
                    // This patch is actually a conic. Project to homogeneous space.
                    w = P[3].x;
                    p1w *= w;
                }

                // Chop the curve at T=1/2.
                vec2 ab = (P[0] + p1w) * .5;
                vec2 bc = (p1w + P[2]) * .5;
                vec2 cd = (P[2] + P[3]) * .5;
                vec2 abc = (ab + bc) * .5;
                vec2 bcd = (bc + cd) * .5;
                vec2 abcd = (abc + bcd) * .5;

                float n0, n1;
                if (w < 0 || isinf(w)) {
                    if (w < 0) {
                        // The patch is a cubic. Calculate how many segments are required to
                        // linearize each half of the curve.
                        n0 = wangs_formula_cubic(PRECISION, mat4x2(P[0], ab, abc, abcd), mat2(1));
                        n1 = wangs_formula_cubic(PRECISION, mat4x2(abcd, bcd, cd, P[3]), mat2(1));
                        rationalCubicW = 1;
                    } else {
                        // The patch is a triangle (a conic with infinite weight).
                        n0 = n1 = 1;
                        rationalCubicW = -1;  // In the next stage, rationalCubicW<0 means triangle.
                    }
                    rationalCubicXY = mat4x2(P[0], P[1], P[2], P[3]);
                } else {
                    // The patch is a conic. Unproject p0..5. w1 == w2 == w3 when chopping at .5.
                    // (See SkConic::chopAt().)
                    float r = 2.0 / (1.0 + w);
                    ab *= r, bc *= r, abc *= r;
                    // Put in "standard form" where w0 == w2 == w4 == 1.
                    float w_ = inversesqrt(r);  // Both halves have the same w' when chopping at .5.
                    // Calculate how many segments are needed to linearize each half of the curve.
                    n0 = wangs_formula_conic(PRECISION, mat3x2(P[0], ab, abc), w_);
                    n1 = wangs_formula_conic(PRECISION, mat3x2(abc, bc, P[2]), w_);
                    // Covert the conic to a rational cubic in projected form.
                    rationalCubicXY = mat4x2(P[0],
                                             mix(float4(P[0],P[2]), p1w.xyxy, 2.0/3.0),
                                             P[2]);
                    rationalCubicW = fma(w, 2.0/3.0, 1.0/3.0);
                }

                gl_TessLevelOuter[0] = min(n1, MAX_TESSELLATION_SEGMENTS);
                gl_TessLevelOuter[1] = 1.0;
                gl_TessLevelOuter[2] = min(n0, MAX_TESSELLATION_SEGMENTS);

                // Changing the inner level to 1 when n0 == n1 == 1 collapses the entire patch to a
                // single triangle. Otherwise, we need an inner level of 2 so our curve triangles
                // have an interior point to originate from.
                gl_TessLevelInner[0] = min(max(n0, n1), 2.0);
            })");

            return code;
        }
        SkString getTessEvaluationShaderGLSL(const GrGeometryProcessor&,
                                             const char* versionAndExtensionDecls,
                                             const GrGLSLUniformHandler&,
                                             const GrShaderCaps&) const override {
            SkString code(versionAndExtensionDecls);
            code.append(kSkSLTypeDefs);
            code.append(kEvalRationalCubicFn);
            code.append(R"(
            layout(triangles, equal_spacing, ccw) in;

            uniform vec4 sk_RTAdjust;

            patch in mat4x2 rationalCubicXY;
            patch in float rationalCubicW;

            void main() {
                vec2 vertexpos;
                if (rationalCubicW < 0) {  // rationalCubicW < 0 means a triangle now.
                    vertexpos = (gl_TessCoord.x != 0) ? rationalCubicXY[0]
                              : (gl_TessCoord.y != 0) ? rationalCubicXY[1]
                                                      : rationalCubicXY[2];
                } else {
                    // Locate our parametric point of interest. T ramps from [0..1/2] on the left
                    // edge of the triangle, and [1/2..1] on the right. If we are the patch's
                    // interior vertex, then we want T=1/2. Since the barycentric coords are
                    // (1/3, 1/3, 1/3) at the interior vertex, the below fma() works in all 3
                    // scenarios.
                    float T = fma(.5, gl_TessCoord.y, gl_TessCoord.z);

                    mat4x3 P = mat4x3(rationalCubicXY[0], 1,
                                      rationalCubicXY[1], rationalCubicW,
                                      rationalCubicXY[2], rationalCubicW,
                                      rationalCubicXY[3], 1);
                    vertexpos = eval_rational_cubic(P, T);
                    if (all(notEqual(gl_TessCoord.xz, vec2(0)))) {
                        // We are the interior point of the patch; center it inside
                        // [C(0), C(.5), C(1)].
                        vertexpos = (P[0].xy + vertexpos + P[3].xy) / 3.0;
                    }
                }

                gl_Position = vec4(vertexpos * sk_RTAdjust.xz + sk_RTAdjust.yw, 0.0, 1.0);
            })");

            return code;
        }
    };
    return new Impl;
}

}  // namespace

GrPathTessellationShader* GrPathTessellationShader::MakeHardwareTessellationShader(
        SkArenaAlloc* arena, const SkMatrix& viewMatrix, const SkPMColor4f& color,
        PatchType patchType) {
    switch (patchType) {
        case PatchType::kWedges:
            return arena->make<HardwareWedgeShader>(viewMatrix, color);
        case PatchType::kCurves:
            return arena->make<HardwareCurveShader>(viewMatrix, color);
    }
    SkUNREACHABLE;
}
