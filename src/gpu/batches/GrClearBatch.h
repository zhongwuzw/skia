/*
 * Copyright 2015 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef GrClearBatch_DEFINED
#define GrClearBatch_DEFINED

#include "GrFixedClip.h"
#include "GrGpu.h"
#include "GrGpuCommandBuffer.h"
#include "GrOp.h"
#include "GrOpFlushState.h"
#include "GrRenderTarget.h"

class GrClearBatch final : public GrOp {
public:
    DEFINE_OP_CLASS_ID

    static sk_sp<GrClearBatch> Make(const GrFixedClip& clip, GrColor color, GrRenderTarget* rt) {
        sk_sp<GrClearBatch> batch(new GrClearBatch(clip, color, rt));
        if (!batch->fRenderTarget) {
            return nullptr; // The clip did not contain any pixels within the render target.
        }
        return batch;
    }

    static sk_sp<GrClearBatch> Make(const SkIRect& rect, GrColor color, GrRenderTarget* rt,
                                    bool fullScreen) {
        sk_sp<GrClearBatch> batch(new GrClearBatch(rect, color, rt, fullScreen));
        return batch;
    }

    const char* name() const override { return "Clear"; }

    // TODO: this needs to be updated to return GrSurfaceProxy::UniqueID
    GrGpuResource::UniqueID renderTargetUniqueID() const override {
        return fRenderTarget.get()->uniqueID();
    }

    SkString dumpInfo() const override {
        SkString string("Scissor [");
        if (fClip.scissorEnabled()) {
            const SkIRect& r = fClip.scissorRect();
            string.appendf("L: %d, T: %d, R: %d, B: %d", r.fLeft, r.fTop, r.fRight, r.fBottom);
        }
        string.appendf("], Color: 0x%08x, RT: %d", fColor,
                                                   fRenderTarget.get()->uniqueID().asUInt());
        string.append(INHERITED::dumpInfo());
        return string;
    }

    void setColor(GrColor color) { fColor = color; }

private:
    GrClearBatch(const GrFixedClip& clip, GrColor color, GrRenderTarget* rt)
        : INHERITED(ClassID())
        , fClip(clip)
        , fColor(color) {
        SkIRect rtRect = SkIRect::MakeWH(rt->width(), rt->height());
        if (fClip.scissorEnabled()) {
            // Don't let scissors extend outside the RT. This may improve batching.
            if (!fClip.intersect(rtRect)) {
                return;
            }
            if (fClip.scissorRect() == rtRect) {
                fClip.disableScissor();
            }
        }
        this->setBounds(SkRect::Make(fClip.scissorEnabled() ? fClip.scissorRect() : rtRect),
                        HasAABloat::kNo, IsZeroArea::kNo);
        fRenderTarget.reset(rt);
    }

    GrClearBatch(const SkIRect& rect, GrColor color, GrRenderTarget* rt, bool fullScreen)
        : INHERITED(ClassID())
        , fClip(GrFixedClip(rect))
        , fColor(color)
        , fRenderTarget(rt) {
        if (fullScreen) {
            fClip.disableScissor();
        }
        this->setBounds(SkRect::Make(rect), HasAABloat::kNo, IsZeroArea::kNo);
    }

    bool onCombineIfPossible(GrOp* t, const GrCaps& caps) override {
        // This could be much more complicated. Currently we look at cases where the new clear
        // contains the old clear, or when the new clear is a subset of the old clear and is the
        // same color.
        GrClearBatch* cb = t->cast<GrClearBatch>();
        SkASSERT(cb->fRenderTarget == fRenderTarget);
        if (!fClip.windowRectsState().cheapEqualTo(cb->fClip.windowRectsState())) {
            return false;
        }
        if (cb->contains(this)) {
            fClip = cb->fClip;
            this->replaceBounds(*t);
            fColor = cb->fColor;
            return true;
        } else if (cb->fColor == fColor && this->contains(cb)) {
            return true;
        }
        return false;
    }

    bool contains(const GrClearBatch* that) const {
        // The constructor ensures that scissor gets disabled on any clip that fills the entire RT.
        return !fClip.scissorEnabled() ||
               (that->fClip.scissorEnabled() &&
                fClip.scissorRect().contains(that->fClip.scissorRect()));
    }

    void onPrepare(GrOpFlushState*) override {}

    void onDraw(GrOpFlushState* state, const SkRect& /*bounds*/) override {
        state->commandBuffer()->clear(fRenderTarget.get(), fClip, fColor);
    }

    GrFixedClip                                             fClip;
    GrColor                                                 fColor;
    GrPendingIOResource<GrRenderTarget, kWrite_GrIOType>    fRenderTarget;

    typedef GrOp INHERITED;
};

#endif
