#include "vrf/research/image_metrics.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

namespace vrf::research
{
    namespace
    {
        struct Plane
        {
            std::vector<double> v;
            uint32_t            w {0};
            uint32_t            h {0};

            double  At(uint32_t x, uint32_t y) const { return v[static_cast<size_t>(y) * w + x]; }
            double& At(uint32_t x, uint32_t y) { return v[static_cast<size_t>(y) * w + x]; }
        };

        uint32_t RowPitch(const ImageView& img)
        {
            if (img.rowPitchBytes != 0)
                return img.rowPitchBytes;
            return img.width * img.channels * (img.isFloat ? 4u : 1u);
        }

        // Normalised to [0,1] so a uint8 capture and a float readback of the same image agree.
        double Sample(const ImageView& img, uint32_t x, uint32_t y, uint32_t c)
        {
            const auto*  base  = static_cast<const uint8_t*>(img.data) + static_cast<size_t>(y) * RowPitch(img);
            const size_t index = static_cast<size_t>(x) * img.channels + c;
            if (img.isFloat)
                return static_cast<double>(reinterpret_cast<const float*>(base)[index]);
            return static_cast<double>(base[index]) / 255.0;
        }

        // Rec.709 luma. SSIM is defined on a single channel and the literature computes it on
        // luminance, so a colour image must be reduced the same way on both sides or the number
        // is not comparable with published ones.
        double Luminance(const ImageView& img, uint32_t x, uint32_t y)
        {
            if (img.channels < 3)
                return Sample(img, x, y, 0);
            const uint32_t r = img.bgraOrder ? 2u : 0u;
            const uint32_t b = img.bgraOrder ? 0u : 2u;
            return 0.2126 * Sample(img, x, y, r) + 0.7152 * Sample(img, x, y, 1) + 0.0722 * Sample(img, x, y, b);
        }

        Plane MakePlane(const ImageView& img, bool luminance, uint32_t channel)
        {
            Plane p;
            p.w = img.width;
            p.h = img.height;
            p.v.resize(static_cast<size_t>(p.w) * p.h);
            for (uint32_t y = 0; y < p.h; ++y)
                for (uint32_t x = 0; x < p.w; ++x)
                    p.At(x, y) = luminance ? Luminance(img, x, y) : Sample(img, x, y, channel);
            return p;
        }

        std::vector<double> GaussianKernel(uint32_t size, double sigma)
        {
            std::vector<double> k(size);
            const double        centre = 0.5 * static_cast<double>(size - 1);
            double              sum    = 0.0;
            for (uint32_t i = 0; i < size; ++i)
            {
                const double d = static_cast<double>(i) - centre;
                k[i]           = std::exp(-(d * d) / (2.0 * sigma * sigma));
                sum += k[i];
            }
            for (double& value : k)
                value /= sum;
            return k;
        }

        // Separable, clamp-to-edge. The 11x11 circular-symmetric Gaussian SSIM specifies is
        // separable, so two 1D passes are exact, not an approximation. Edge windows are biased
        // by the clamp, which is why the SSIM mean below excludes a border of one radius.
        Plane Blur(const Plane& src, const std::vector<double>& kernel)
        {
            const size_t taps   = kernel.size();
            const int    radius = static_cast<int>(taps / 2);
            const int    maxX   = static_cast<int>(src.w) - 1;
            const int    maxY   = static_cast<int>(src.h) - 1;

            Plane tmp;
            tmp.w = src.w;
            tmp.h = src.h;
            tmp.v.resize(src.v.size());
            for (uint32_t y = 0; y < src.h; ++y)
                for (uint32_t x = 0; x < src.w; ++x)
                {
                    double acc = 0.0;
                    for (size_t t = 0; t < taps; ++t)
                    {
                        const int sx = std::min(std::max(static_cast<int>(x) + static_cast<int>(t) - radius, 0), maxX);
                        acc += kernel[t] * src.At(static_cast<uint32_t>(sx), y);
                    }
                    tmp.At(x, y) = acc;
                }

            Plane out;
            out.w = src.w;
            out.h = src.h;
            out.v.resize(src.v.size());
            for (uint32_t y = 0; y < src.h; ++y)
                for (uint32_t x = 0; x < src.w; ++x)
                {
                    double acc = 0.0;
                    for (size_t t = 0; t < taps; ++t)
                    {
                        const int sy = std::min(std::max(static_cast<int>(y) + static_cast<int>(t) - radius, 0), maxY);
                        acc += kernel[t] * tmp.At(x, static_cast<uint32_t>(sy));
                    }
                    out.At(x, y) = acc;
                }
            return out;
        }

        Plane Multiply(const Plane& a, const Plane& b)
        {
            Plane out = a;
            for (size_t i = 0; i < out.v.size(); ++i)
                out.v[i] = a.v[i] * b.v[i];
            return out;
        }

        // Wang et al. 2004, eq. 13, evaluated from Gaussian-weighted local statistics.
        double MeanSsim(const Plane& x, const Plane& y, const MetricOptions& o, uint64_t& windows)
        {
            windows             = 0;
            const uint32_t size = o.ssimWindow | 1u; // force odd so the window has a centre
            if (x.w < size || x.h < size)
                return 0.0;

            const std::vector<double> kernel = GaussianKernel(size, o.ssimSigma);
            const Plane               muX    = Blur(x, kernel);
            const Plane               muY    = Blur(y, kernel);
            const Plane               xx     = Blur(Multiply(x, x), kernel);
            const Plane               yy     = Blur(Multiply(y, y), kernel);
            const Plane               xy     = Blur(Multiply(x, y), kernel);

            const double c1 = (o.ssimK1 * o.dynamicRange) * (o.ssimK1 * o.dynamicRange);
            const double c2 = (o.ssimK2 * o.dynamicRange) * (o.ssimK2 * o.dynamicRange);

            const uint32_t radius = size / 2;
            double         sum    = 0.0;
            for (uint32_t py = radius; py + radius < x.h; ++py)
                for (uint32_t px = radius; px + radius < x.w; ++px)
                {
                    const double mx  = muX.At(px, py);
                    const double my  = muY.At(px, py);
                    const double vx  = xx.At(px, py) - mx * mx;
                    const double vy  = yy.At(px, py) - my * my;
                    const double vxy = xy.At(px, py) - mx * my;

                    const double numerator   = (2.0 * mx * my + c1) * (2.0 * vxy + c2);
                    const double denominator = (mx * mx + my * my + c1) * (vx + vy + c2);
                    sum += denominator > 0.0 ? numerator / denominator : 1.0;
                    ++windows;
                }
            return windows > 0 ? sum / static_cast<double>(windows) : 0.0;
        }
    } // namespace

    MetricResult Compare(const ImageView& reference, const ImageView& test, const MetricOptions& options)
    {
        MetricResult result;
        if (reference.data == nullptr || test.data == nullptr)
        {
            result.error = "null image data";
            return result;
        }
        if (reference.width != test.width || reference.height != test.height)
        {
            result.error = "size mismatch";
            return result;
        }
        if (reference.channels != test.channels || reference.channels == 0 || reference.channels > 4)
        {
            result.error = "channel-count mismatch or unsupported channel count";
            return result;
        }
        if (reference.width == 0 || reference.height == 0)
        {
            result.error = "empty image";
            return result;
        }

        MetricOptions o = options;
        if (!(o.dynamicRange > 0.0))
            o.dynamicRange = 1.0;

        // Error metrics run over the compared channels; SSIM runs on one plane.
        const uint32_t colourChannels = reference.channels >= 3 ? 3u : reference.channels;
        const uint32_t lastChannel    = (o.includeAlpha && reference.channels == 4) ? 4u : colourChannels;

        double   squared  = 0.0;
        double   absolute = 0.0;
        double   worst    = 0.0;
        uint64_t samples  = 0;
        for (uint32_t y = 0; y < reference.height; ++y)
            for (uint32_t x = 0; x < reference.width; ++x)
                for (uint32_t c = 0; c < lastChannel; ++c)
                {
                    const double d = Sample(reference, x, y, c) - Sample(test, x, y, c);
                    squared += d * d;
                    absolute += std::abs(d);
                    worst = std::max(worst, std::abs(d));
                    ++samples;
                }

        result.comparedPixels = static_cast<uint64_t>(reference.width) * reference.height;
        result.mse            = samples > 0 ? squared / static_cast<double>(samples) : 0.0;
        result.rmse           = std::sqrt(result.mse);
        result.mae            = samples > 0 ? absolute / static_cast<double>(samples) : 0.0;
        result.maxAbsError    = worst;
        result.psnr           = result.mse > 0.0 ? 10.0 * std::log10((o.dynamicRange * o.dynamicRange) / result.mse) :
                                                   std::numeric_limits<double>::infinity();

        if (o.luminanceOnly || reference.channels < 3)
        {
            const Plane a = MakePlane(reference, o.luminanceOnly && reference.channels >= 3, 0);
            const Plane b = MakePlane(test, o.luminanceOnly && reference.channels >= 3, 0);
            result.ssim   = MeanSsim(a, b, o, result.ssimWindows);
        }
        else
        {
            // Per-channel SSIM averaged, for callers that opted out of luminance reduction.
            double   total   = 0.0;
            uint64_t windows = 0;
            uint32_t counted = 0;
            for (uint32_t c = 0; c < colourChannels; ++c)
            {
                const Plane a = MakePlane(reference, false, c);
                const Plane b = MakePlane(test, false, c);
                total += MeanSsim(a, b, o, windows);
                ++counted;
            }
            result.ssim        = counted > 0 ? total / counted : 0.0;
            result.ssimWindows = windows;
        }

        result.valid = true;
        return result;
    }

    uint32_t FormatSummary(const MetricResult& r, char* buffer, uint32_t capacity)
    {
        if (buffer == nullptr || capacity == 0)
            return 0;
        if (!r.valid)
        {
            const int written =
                std::snprintf(buffer, capacity, "invalid (%s)", r.error != nullptr ? r.error : "unknown");
            return written > 0 ? static_cast<uint32_t>(written) : 0u;
        }
        const int written = std::snprintf(
            buffer, capacity, "rmse %.5f  psnr %.2f dB  ssim %.5f  maxErr %.5f", r.rmse, r.psnr, r.ssim, r.maxAbsError);
        return written > 0 ? static_cast<uint32_t>(written) : 0u;
    }
} // namespace vrf::research
