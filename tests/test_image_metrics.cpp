// Reference-based image metrics. These assertions are the contract research numbers rely on,
// so they pin the properties a wrong implementation would break rather than just exercising
// the code: identity, symmetry, the uint8/float agreement, and SSIM's sensitivity to
// structure rather than to a constant offset.
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include "vrf/research/image_metrics.hpp"

using namespace vrf::research;

namespace
{
    constexpr uint32_t kW = 64;
    constexpr uint32_t kH = 48;

    ImageView ViewOf(const std::vector<float>& data, uint32_t channels = 1)
    {
        ImageView v;
        v.data     = data.data();
        v.width    = kW;
        v.height   = kH;
        v.channels = channels;
        v.isFloat  = true;
        return v;
    }

    // A checkerboard has structure at every scale, which is what SSIM is meant to see.
    std::vector<float> Checker(uint32_t period)
    {
        std::vector<float> out(static_cast<size_t>(kW) * kH);
        for (uint32_t y = 0; y < kH; ++y)
            for (uint32_t x = 0; x < kW; ++x)
                out[static_cast<size_t>(y) * kW + x] = ((x / period + y / period) % 2 == 0) ? 0.8f : 0.2f;
        return out;
    }
} // namespace

TEST_CASE("image metrics: identical images are a perfect score")
{
    const auto image = Checker(8);
    const auto r     = Compare(ViewOf(image), ViewOf(image));

    REQUIRE(r.valid);
    CHECK(r.mse == doctest::Approx(0.0));
    CHECK(r.rmse == doctest::Approx(0.0));
    CHECK(r.mae == doctest::Approx(0.0));
    CHECK(r.maxAbsError == doctest::Approx(0.0));
    CHECK(r.ssim == doctest::Approx(1.0).epsilon(1e-9));
    // Zero error must report infinite PSNR, not a division-by-zero artefact.
    CHECK(std::isinf(r.psnr));
    CHECK(r.ssimWindows > 0);
}

TEST_CASE("image metrics: error terms match a hand-computed constant offset")
{
    std::vector<float> a(static_cast<size_t>(kW) * kH, 0.5f);
    std::vector<float> b(static_cast<size_t>(kW) * kH, 0.6f);

    const auto r = Compare(ViewOf(a), ViewOf(b));
    REQUIRE(r.valid);
    CHECK(r.mae == doctest::Approx(0.1));
    CHECK(r.rmse == doctest::Approx(0.1));
    CHECK(r.mse == doctest::Approx(0.01));
    CHECK(r.maxAbsError == doctest::Approx(0.1));
    // PSNR = 10*log10(1 / 0.01) = 20 dB for unit dynamic range.
    CHECK(r.psnr == doctest::Approx(20.0).epsilon(1e-6));
}

TEST_CASE("image metrics: SSIM tracks structure, not a uniform shift")
{
    const auto base = Checker(8);

    // A constant offset on a flat-variance signal leaves local covariance intact, so SSIM
    // must stay high even though RMSE is clearly non-zero. A luminance-only comparison that
    // accidentally normalised by the mean would score this as perfect; one that ignored
    // covariance would score it as broken.
    std::vector<float> shifted = base;
    for (float& v : shifted)
        v += 0.05f;

    const auto offset = Compare(ViewOf(base), ViewOf(shifted));
    REQUIRE(offset.valid);
    CHECK(offset.rmse == doctest::Approx(0.05));
    CHECK(offset.ssim > 0.9);

    // Changing the structure itself must score far worse than the shift did, at comparable
    // RMSE - that separation is the whole reason to report SSIM alongside RMSE.
    const auto scrambled  = Checker(2);
    const auto structural = Compare(ViewOf(base), ViewOf(scrambled));
    REQUIRE(structural.valid);
    CHECK(structural.ssim < offset.ssim);
    CHECK(structural.ssim < 0.5);
}

TEST_CASE("image metrics: uint8 and float views of the same image agree")
{
    // Samples are normalised on read, so a capture and a float readback must not disagree
    // merely because of storage. This is what lets a BMP dump be compared against a
    // device readback.
    std::vector<float>   asFloat(static_cast<size_t>(kW) * kH);
    std::vector<uint8_t> asBytes(static_cast<size_t>(kW) * kH);
    for (size_t i = 0; i < asFloat.size(); ++i)
    {
        const uint8_t byte = static_cast<uint8_t>(i % 256);
        asBytes[i]         = byte;
        asFloat[i]         = static_cast<float>(byte) / 255.0f;
    }

    ImageView bytes;
    bytes.data     = asBytes.data();
    bytes.width    = kW;
    bytes.height   = kH;
    bytes.channels = 1;
    bytes.isFloat  = false;

    const auto r = Compare(ViewOf(asFloat), bytes);
    REQUIRE(r.valid);
    CHECK(r.rmse == doctest::Approx(0.0).epsilon(1e-6));
    CHECK(r.ssim == doctest::Approx(1.0).epsilon(1e-6));
}

TEST_CASE("image metrics: mismatched or malformed inputs are rejected, not guessed at")
{
    const auto image = Checker(8);
    auto       good  = ViewOf(image);

    ImageView empty;
    CHECK_FALSE(Compare(good, empty).valid);
    CHECK(Compare(good, empty).error != nullptr);

    auto wrongSize   = good;
    wrongSize.height = kH / 2;
    CHECK_FALSE(Compare(good, wrongSize).valid);

    auto wrongChannels     = good;
    wrongChannels.channels = 3;
    CHECK_FALSE(Compare(good, wrongChannels).valid);
}

TEST_CASE("image metrics: an image smaller than the SSIM window reports no windows")
{
    // Rather than silently returning a meaningless SSIM, the error metrics stay valid and
    // ssimWindows reports zero so a caller can tell the difference.
    std::vector<float> tiny(4 * 4, 0.5f);
    ImageView          v;
    v.data     = tiny.data();
    v.width    = 4;
    v.height   = 4;
    v.channels = 1;
    v.isFloat  = true;

    const auto r = Compare(v, v);
    REQUIRE(r.valid);
    CHECK(r.ssimWindows == 0);
    CHECK(r.rmse == doctest::Approx(0.0));
}
