/*
 * Copyright 2021 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "tests/Test.h"

#include "include/core/SkVertices.h"
#include "src/core/SkBlendModePriv.h"
#include "src/core/SkMatrixProvider.h"
#include "src/core/SkSurfacePriv.h"
#include "src/gpu/GrStyle.h"
#include "src/gpu/GrSurfaceDrawContext.h"

static SkSurfaceProps kDMSAAProps(kDMSAA_SkSurfacePropsPrivateFlag, kUnknown_SkPixelGeometry);
constexpr static SkPMColor4f kTransYellow = {.5f,.5f,.0f,.5f};
constexpr static SkPMColor4f kTransCyan = {.0f,.5f,.5f,.5f};
constexpr static int w=10, h=10;

static void draw_paint_with_dmsaa(GrSurfaceDrawContext* sdc, const SkPMColor4f& color,
                                  SkBlendMode blendMode) {
    // drawVertices should always trigger dmsaa, but draw something non-rectangular just to be 100%
    // certain.
    static const SkPoint kVertices[3] = {{-.5f,-.5f}, {w * 2.1f, 0}, {0, h * 2.1f}};
    SkVertices::Builder builder(SkVertices::kTriangles_VertexMode, 3, 0, 0);
    memcpy(builder.positions(), kVertices, sizeof(kVertices));
    auto vertices = builder.detach();

    GrPaint paint;
    paint.setColor4f(color);
    paint.setXPFactory(SkBlendMode_AsXPFactory(blendMode));
    sdc->drawVertices(nullptr, std::move(paint), SkSimpleMatrixProvider(SkMatrix::I()), vertices);
}

static bool fuzzy_equals(const float a[4], const SkPMColor4f& b) {
    constexpr static float kTolerance = 2.5f / 256;
    for (int i = 0; i < 4; ++i) {
        if (!SkScalarNearlyEqual(a[i], b.vec()[i], kTolerance)) {
            return false;
        }
    }
    return true;
}

static void check_sdc_color(skiatest::Reporter* reporter, GrSurfaceDrawContext* sdc,
                            GrDirectContext* ctx, const SkPMColor4f& color) {
    auto info = SkImageInfo::Make(w, h, kRGBA_F32_SkColorType, kPremul_SkAlphaType);
    GrPixmap pixmap = GrPixmap::Allocate(info);
    sdc->readPixels(ctx, pixmap, {0, 0});
    auto pix = static_cast<const float*>(pixmap.addr());
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (!fuzzy_equals(pix, color)) {
                ERRORF(reporter, "SDC color mismatch.\n"
                                 "Got      [%0.3f, %0.3f, %0.3f, %0.3f]\n"
                                 "Expected [%0.3f, %0.3f, %0.3f, %0.3f]",
                       pix[0], pix[1], pix[2], pix[3], color.fR, color.fG, color.fB, color.fA);
                return;
            }
            pix += 4;
        }
    }
}

DEF_GPUTEST_FOR_CONTEXTS(DMSAA_preserve_contents,
                         &sk_gpu_test::GrContextFactory::IsRenderingContext, reporter, ctxInfo,
                         nullptr) {
    auto ctx = ctxInfo.directContext();
    auto sdc = GrSurfaceDrawContext::Make(ctx, GrColorType::kRGBA_8888, nullptr,
                                          SkBackingFit::kApprox, {w, h}, kDMSAAProps);

    // Initialize the texture and dmsaa attachment with transparent.
    draw_paint_with_dmsaa(sdc.get(), SK_PMColor4fTRANSPARENT, SkBlendMode::kSrc);
    check_sdc_color(reporter, sdc.get(), ctx, SK_PMColor4fTRANSPARENT);

    // Clear the main texture to yellow.
    sdc->clear(kTransYellow);

    // Close the opsTask by doing a readback.
    check_sdc_color(reporter, sdc.get(), ctx, kTransYellow);

    // Now the DMSAA attachment is clear and the texture is yellow. Blend cyan into the DMSAA
    // attachment. This will fail if the yellow from the main texture doesn't get copied into the
    // DMSAA attachment before the renderPass.
    draw_paint_with_dmsaa(sdc.get(), kTransCyan, SkBlendMode::kSrcOver);
    SkPMColor4f dstColor = SkBlendMode_Apply(SkBlendMode::kSrcOver, kTransCyan, kTransYellow);

    check_sdc_color(reporter, sdc.get(), ctx, dstColor);
}
