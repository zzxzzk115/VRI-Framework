/*
 * image_metrics.hpp - reference-based image quality metrics (RMSE, PSNR, SSIM, MAE).
 *
 * For research work that has to defend a claim like "visually lossless" with a number.
 * Differing-pixel counts cannot do that: they answer "did anything change", not "does it
 * look the same", and their threshold is arbitrary.
 *
 * Deliberately CPU-side and dependency-free, over a non-owning view of pixels. Metrics are
 * compared across runs, machines and captures, so they must not depend on the device, the
 * swapchain format, or anything else that varies with where the image came from. Read back
 * with vrf::Screenshot (or load a dump) and hand the bytes here.
 *
 * MEASURE AT NATIVE RESOLUTION. Comparing UI-composited or rescaled captures measures the
 * resampler as much as the renderer, and the resample error can easily exceed the difference
 * under test.
 */
#pragma once

#include <cstdint>

namespace vrf::research
{
    // Non-owning view of tightly-packed-per-row pixel data. Supports 8-bit UNORM and 32-bit
    // float, 1..4 channels, with an explicit row pitch so sub-images and padded readbacks work.
    struct ImageView
    {
        const void* data {nullptr};
        uint32_t    width {0};
        uint32_t    height {0};
        uint32_t    channels {4};
        uint32_t    rowPitchBytes {0}; // 0 => width * channels * (isFloat ? 4 : 1)
        bool        isFloat {false};
        // Channel order only matters for luminance weighting; set when data is BGRA (the
        // usual layout of a swapchain readback or a BMP dump).
        bool bgraOrder {false};
    };

    struct MetricOptions
    {
        // SSIM is defined on a single channel, conventionally luminance. When false, every
        // metric is averaged over the colour channels independently instead.
        bool luminanceOnly {true};

        // L in the SSIM stabilising constants and the PSNR peak. Samples are normalised to
        // [0,1] on read (uint8 divided by 255, float taken as-is) so that a uint8 capture and
        // a float readback of the same image compare equal, hence the default of 1.0. Raise it
        // only for deliberately HDR comparisons, and report that you did.
        double dynamicRange {1.0};

        // Wang et al. 2004: 11x11 circular-symmetric Gaussian, sigma 1.5, K1 = 0.01, K2 = 0.03.
        // These are the values every published SSIM number assumes; change them only if you
        // intend to report a non-standard SSIM, and say so if you do.
        uint32_t ssimWindow {11};
        double   ssimSigma {1.5};
        double   ssimK1 {0.01};
        double   ssimK2 {0.03};

        // Alpha carries an encoding (hole class, sample age) rather than appearance in several
        // of our targets, so it is excluded by default.
        bool includeAlpha {false};
    };

    struct MetricResult
    {
        bool     valid {false};
        double   mse {0.0};
        double   rmse {0.0};
        double   mae {0.0};
        double   maxAbsError {0.0};
        double   psnr {0.0}; // dB; +infinity for identical images
        double   ssim {0.0}; // mean SSIM over valid windows
        uint64_t comparedPixels {0};
        // Windows contributing to the SSIM mean. Zero when the image is smaller than the
        // window, in which case ssim is left at 0 and only the error metrics are meaningful.
        uint64_t ssimWindows {0};
        // Set when the inputs disagree on size/format, or a view is malformed.
        const char* error {nullptr};
    };

    // Compares b against reference a. Both views must have identical width, height and
    // channel count; formats may differ (uint8 vs float) and are normalised to the same
    // range before comparison.
    [[nodiscard]] MetricResult
    Compare(const ImageView& reference, const ImageView& test, const MetricOptions& options = {});

    // Formats a one-line summary for logs/CSV, e.g.
    //   "rmse 0.0031  psnr 50.2 dB  ssim 0.9987  maxErr 0.0412"
    // Writes at most `capacity` bytes including the terminator and returns bytes written.
    uint32_t FormatSummary(const MetricResult&, char* buffer, uint32_t capacity);
} // namespace vrf::research
