/*
 * Copyright 2021 Google LLC.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef GrPathTessellateOp_DEFINED
#define GrPathTessellateOp_DEFINED

#include "src/gpu/ops/GrDrawOp.h"
#include "src/gpu/tessellate/shaders/GrTessellationShader.h"

class GrPathTessellator;

// Tessellates a path directly to the color buffer, using one single render pass. This currently
// only works for convex paths.
class GrPathTessellateOp : public GrDrawOp {
private:
    DEFINE_OP_CLASS_ID

    GrPathTessellateOp(const SkMatrix& viewMatrix, const SkPath& path, GrPaint&& paint,
                       GrAAType aaType, const GrUserStencilSettings* stencil,
                       const SkRect& devBounds)
            : GrDrawOp(ClassID())
            , fViewMatrix(viewMatrix)
            , fPath(path)
            , fAAType(aaType)
            , fStencil(stencil)
            , fColor(paint.getColor4f())
            , fProcessors(std::move(paint)) {
        this->setBounds(devBounds, HasAABloat::kNo, IsHairline::kNo);
    }

    const char* name() const override { return "GrPathTessellateOp"; }
    bool usesMSAA() const override { return fAAType == GrAAType::kMSAA; }
    void visitProxies(const GrVisitProxyFunc&) const override;
    GrProcessorSet::Analysis finalize(const GrCaps&, const GrAppliedClip*, GrClampType) override;
    bool usesStencil() const override { return !fStencil->isUnused(); }

    void prepareTessellator(const GrTessellationShader::ProgramArgs&, GrAppliedClip&& clip);

    void onPrePrepare(GrRecordingContext*, const GrSurfaceProxyView&, GrAppliedClip*,
                      const GrDstProxyView&, GrXferBarrierFlags, GrLoadOp colorLoadOp) override;
    void onPrepare(GrOpFlushState*) override;
    void onExecute(GrOpFlushState*, const SkRect& chainBounds) override;

    const SkMatrix fViewMatrix;
    const SkPath fPath;
    const GrAAType fAAType;
    const GrUserStencilSettings* const fStencil;
    SkPMColor4f fColor;
    GrProcessorSet fProcessors;

    // Decided during prepareTessellator.
    GrPathTessellator* fTessellator = nullptr;
    const GrProgramInfo* fTessellationProgram = nullptr;

    friend class GrOp;  // For ctor.
};

#endif
