/*
 * Copyright 2021 Google LLC.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef GrPathTessellator_DEFINED
#define GrPathTessellator_DEFINED

#include "src/core/SkPathPriv.h"
#include "src/gpu/GrInnerFanTriangulator.h"
#include "src/gpu/GrVertexWriter.h"
#include "src/gpu/GrVx.h"

class SkPath;
class GrMeshDrawTarget;
class GrOpFlushState;
class GrPathTessellationShader;

// Prepares GPU data for, and then draws a path's tessellated geometry. Depending on the subclass,
// the caller may or may not be required to draw the path's inner fan separately.
class GrPathTessellator {
public:
    using BreadcrumbTriangleList = GrInnerFanTriangulator::BreadcrumbTriangleList;

    // Don't allow linearized segments to be off by more than 1/4th of a pixel from the true curve.
    constexpr static float kLinearizationPrecision = 4;

    // For fixed count tessellators, this is the largest number of segments we can stuff into a
    // single instance before we need to chop.
    constexpr static int kMaxFixedCountSegments = 32;

    const GrPathTessellationShader* shader() const { return fShader; }

    // Called before draw(). Prepares GPU buffers containing the geometry to tessellate. If the
    // given BreadcrumbTriangleList is non-null, then this class will also include the breadcrumb
    // triangles in its draw.
    virtual void prepare(GrMeshDrawTarget*, const SkRect& cullBounds, const SkPath&,
                         const BreadcrumbTriangleList* = nullptr) = 0;

    // Issues draw calls for the tessellated geometry. The caller is responsible for binding its
    // desired pipeline ahead of time.
    virtual void draw(GrOpFlushState*) const = 0;

    // Draws a 4-point instance for each curve. This method is used for drawing convex hulls over
    // each cubic with GrFillCubicHullShader. The caller is responsible for binding its desired
    // pipeline ahead of time. This method is not supported by every subclass.
    virtual void drawHullInstances(GrOpFlushState*) const { SK_ABORT("Not supported."); }

    virtual ~GrPathTessellator() {}

    // Returns an upper bound on the number of segments (lineTo, quadTo, conicTo, cubicTo) in a
    // path, also accounting for any implicit lineTos from closing contours.
    static int MaxSegmentsInPath(const SkPath& path) {
        // There might be an implicit kClose at the end, but the path always begins with kMove. So
        // the max number of segments in the path is equal to the number of verbs.
        SkASSERT(path.countVerbs() == 0 || SkPathPriv::VerbData(path)[0] == SkPath::kMove_Verb);
        return path.countVerbs();
    }

protected:
    GrPathTessellator(GrPathTessellationShader* shader) : fShader(shader) {}

    GrPathTessellationShader* fShader;
};

#endif
