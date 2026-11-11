// Generates `known_values.rs`: the values pbrt-v4 itself computes for the
// spectral routines `umbra-spectrum` ports, on fixed inputs. The tests
// compare Umbra's results with them (ADR 0004: the same inputs must give
// pbrt-v4's numbers).
//
// pbrt-v4 at the pinned commit (arc42 §8.11): `spectrum.cpp`, `color.cpp`,
// `colorspace.cpp` and the allocator (`pstd.cpp`), compiled as they are,
// with the sRGB table that pbrt-v4's own `rgb2spec_opt` generates. The
// other colour spaces' tables are left empty: nothing here reads them. From
// the repository root, with `mmp/pbrt-v4` checked out at the pinned commit
// in `$PBRT`:
//
//   T=crates/umbra-spectrum/tests/umbra_spectrum/pbrt_v4
//   F="-std=c++17 -O3 -ffp-contract=off -DPBRT_HAVE_POSIX_MEMALIGN -I$PBRT/src -I$PBRT/src/ext"
//   g++ $F $PBRT/src/pbrt/cmd/rgb2spec_opt.cpp -o /tmp/rgb2spec_opt -lpthread
//   /tmp/rgb2spec_opt 64 /tmp/rgbspectrum_srgb.cpp sRGB
//   for t in DCI_P3 REC2020 ACES2065_1; do
//     echo "namespace pbrt { extern const int ${t}ToSpectrumTable_Res = 64;
//       extern const float ${t}ToSpectrumTable_Scale[64] = {};
//       extern const float ${t}ToSpectrumTable_Data[3][64][64][64][3] = {}; }"
//   done > /tmp/other_tables.cpp
//   g++ $F $T/known_values.cpp $PBRT/src/pbrt/util/{spectrum,color,colorspace,pstd}.cpp \
//     /tmp/rgbspectrum_srgb.cpp /tmp/other_tables.cpp -o /tmp/known_values
//   /tmp/known_values > $T/known_values.rs && rustfmt --edition 2024 $T/known_values.rs
//
// `-ffp-contract=off` keeps the compiler from fusing multiplies and adds,
// which Umbra never does implicitly (ADR 0002). Transcendentals and the
// cube root of `rgb2spec_opt` come from the platform libm (glibc), not
// Umbra's own kernels or the glibc of its image, so values computed through
// them can differ in their last bits; the tests say how much they allow.

#include <pbrt/util/color.h>
#include <pbrt/util/colorspace.h>
#include <pbrt/util/rng.h>
#include <pbrt/util/spectrum.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace pbrt;

// What `spectrum.cpp`, `color.cpp` and `colorspace.cpp` call outside the
// code used here: logging, errors, printing and files. Never reached on
// these inputs; they stand in for the rest of pbrt.
namespace pbrt {
namespace logging {
LogLevel logLevel = LogLevel::Error;
}
void Log(LogLevel, const char *, int, const char *) {}
void LogFatal(LogLevel, const char *file, int line, const char *message) {
    fprintf(stderr, "%s:%d: %s\n", file, line, message);
    abort();
}
void ErrorExit(const FileLoc *, const char *message) {
    fprintf(stderr, "%s\n", message);
    abort();
}
void Warning(const FileLoc *, const char *) {}
std::vector<Float> ReadFloatFile(std::string) { abort(); }
std::vector<std::string> SplitStringsFromWhitespace(std::string_view) { abort(); }
namespace detail {
void stringPrintfRecursive(std::string *s, const char *fmt) { *s += fmt; }
std::string copyToFormatString(const char **, std::string *) { abort(); }
std::string FloatToString(float) { abort(); }
}  // namespace detail
}  // namespace pbrt

// Two templates `math.cpp` and `print.cpp` define, which the headers
// instantiate before a specialization could replace them: defined here by
// their mangled names, `SquareMatrix<3>::ToString()` and
// `internal::ToString2<float>`.
extern "C" [[noreturn]] void SquareMatrix3ToString() asm("_ZNK4pbrt12SquareMatrixILi3EE8ToStringB5cxx11Ev");
extern "C" void SquareMatrix3ToString() { abort(); }
extern "C" [[noreturn]] void ToString2Float() asm(
    "_ZN4pbrt8internal9ToString2IfEENSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEET_S8_");
extern "C" void ToString2Float() { abort(); }

static uint32_t Bits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}

static void Begin(const char *name, const char *type, const char *doc) {
    printf("/// %s\n#[rustfmt::skip]\npub(crate) const %s: &[%s] = &[\n", doc, name, type);
}

static void End() { printf("];\n\n"); }

static void F(float f) { printf("0x%08x", Bits(f)); }

template <typename T>
static void Fs(const T &v, int n) {
    printf("[");
    for (int i = 0; i < n; ++i) {
        F(v[i]);
        printf(", ");
    }
    printf("]");
}

// The wavelengths every spectrum is evaluated at: the ends, the integers
// and halves the dense spectra round between, and points in between.
static const Float Lambdas[] = {360, 361.5, 412.25, 499.5, 500.5, 555, 633.3, 700, 829.5, 830};
static constexpr int NLambdas = sizeof(Lambdas) / sizeof(Lambdas[0]);

// The RGBs every RGB spectrum is built from: black, white, greys, the
// primaries, the ties `maxc` breaks, dark colours (the cells where the
// table's fit is least stable), and pbrt-v4's default `RNG` stream.
static std::vector<RGB> Colors() {
    std::vector<RGB> c = {
        {0, 0, 0},         {1, 1, 1},          {0.5f, 0.5f, 0.5f},      {0.18f, 0.18f, 0.18f},
        {1, 0, 0},         {0, 1, 0},          {0, 0, 1},               {1, 1, 0},
        {0, 1, 1},         {1, 0, 1},          {0.2f, 0.4f, 0.6f},      {0.9f, 0.1f, 0.05f},
        {0.3f, 0.3f, 0.7f}, {0.6f, 0.6f, 0.2f}, {1e-4f, 2e-4f, 5e-5f},   {1e-3f, 0, 3e-3f},
        {0.01f, 0.004f, 0.002f}, {0.002f, 0.02f, 0.01f}, {0.03f, 0.001f, 0.02f}};
    RNG rng;
    for (int i = 0; i < 24; ++i) {
        Float r = rng.Uniform<Float>(), g = rng.Uniform<Float>();
        c.push_back({r, g, rng.Uniform<Float>()});
    }
    return c;
}

// The `u` every wavelength sample is drawn with: the ends, the strata's
// edges, and pbrt-v4's default `RNG` stream.
static std::vector<Float> Us() {
    std::vector<Float> u = {0, 0.25f, 0.5f, 0.75f, 0.1f, OneMinusEpsilon};
    RNG rng;
    for (int i = 0; i < 18; ++i)
        u.push_back(rng.Uniform<Float>());
    return u;
}

static void Spectra_() {
    Begin("SPECTRA", "[u32; 4]",
            "pbrt-v4 `Spectra::X()`, `Y()`, `Z()` and the sRGB illuminant (`stdillum-D65`) at 360, "
            "361, …, 830 nm.");
    for (int lambda = 360; lambda <= 830; ++lambda) {
        Float v[4] = {Spectra::X()(lambda), Spectra::Y()(lambda), Spectra::Z()(lambda),
                        RGBColorSpace::sRGB->illuminant(lambda)};
        Fs(v, 4);
        printf(",\n");
    }
    End();
}

static void ColorSpace() {
    const RGBColorSpace &cs = *RGBColorSpace::sRGB;
    Begin("SRGB_CHROMATICITIES", "[u32; 2]",
            "pbrt-v4 `RGBColorSpace::sRGB`'s `r`, `g`, `b` and `w`.");
    for (Point2f p : {cs.r, cs.g, cs.b, cs.w}) {
        Fs(p, 2);
        printf(",\n");
    }
    End();
    Begin("SRGB_XYZ_FROM_RGB", "[u32; 3]", "pbrt-v4 `RGBColorSpace::sRGB->XYZFromRGB`, by rows.");
    for (int i = 0; i < 3; ++i) {
        Fs(cs.XYZFromRGB[i], 3);
        printf(",\n");
    }
    End();
    Begin("SRGB_RGB_FROM_XYZ", "[u32; 3]", "pbrt-v4 `RGBColorSpace::sRGB->RGBFromXYZ`, by rows.");
    for (int i = 0; i < 3; ++i) {
        Fs(cs.RGBFromXYZ[i], 3);
        printf(",\n");
    }
    End();
    Begin("SRGB_WHITE_XYZ", "u32",
            "pbrt-v4 `SpectrumToXYZ(&RGBColorSpace::sRGB->illuminant)`, then "
            "`SpectrumToPhotometric` of it.");
    XYZ xyz = SpectrumToXYZ(&cs.illuminant);
    for (int c = 0; c < 3; ++c) {
        F(xyz[c]);
        printf(", ");
    }
    F(SpectrumToPhotometric(&cs.illuminant));
    printf(",\n");
    End();
}

template <typename S>
static void SpectrumRow(RGB rgb, const S &s) {
    printf("(");
    Fs(rgb, 3);
    printf(", ");
    Float v[NLambdas];
    for (int i = 0; i < NLambdas; ++i)
        v[i] = s(Lambdas[i]);
    Fs(v, NLambdas);
    printf(", ");
    F(s.MaxValue());
    printf("),\n");
}

static void RGBSpectra() {
    printf("/// The wavelengths the RGB spectra are evaluated at.\n");
    printf("pub(crate) const LAMBDAS: [u32; %d] = ", NLambdas);
    Fs(Lambdas, NLambdas);
    printf(";\n\n");
    const char *type = "([u32; 3], [u32; 10], u32)";
    const RGBColorSpace &cs = *RGBColorSpace::sRGB;
    Begin("RGB_ALBEDO", type,
            "pbrt-v4 `RGBAlbedoSpectrum(sRGB, rgb)` at `LAMBDAS`, and its `MaxValue`: "
            "`(rgb, values, max)`.");
    for (RGB rgb : Colors())
        SpectrumRow(rgb, RGBAlbedoSpectrum(cs, rgb));
    End();
    Begin("RGB_UNBOUNDED", type,
            "pbrt-v4 `RGBUnboundedSpectrum(sRGB, rgb)` at `LAMBDAS`, and its `MaxValue`: "
            "`(rgb, values, max)`.");
    for (RGB rgb : Colors())
        SpectrumRow(rgb * 10.f, RGBUnboundedSpectrum(cs, rgb * 10.f));
    End();
    Begin("RGB_ILLUMINANT", type,
            "pbrt-v4 `RGBIlluminantSpectrum(sRGB, rgb)` at `LAMBDAS`, and its `MaxValue`: "
            "`(rgb, values, max)`.");
    for (RGB rgb : Colors())
        SpectrumRow(rgb, RGBIlluminantSpectrum(cs, rgb));
    End();
}

static void Wavelengths() {
    const char *type = "(u32, [u32; 4], [u32; 4])";
    Begin("SAMPLE_VISIBLE", type,
            "pbrt-v4 `SampledWavelengths::SampleVisible(u)`: `(u, lambda, pdf)`.");
    for (Float u : Us()) {
        SampledWavelengths swl = SampledWavelengths::SampleVisible(u);
        printf("(");
        F(u);
        printf(", ");
        Fs(swl, 4);
        printf(", ");
        Fs(swl.PDF(), 4);
        printf("),\n");
    }
    End();
    Begin("SAMPLE_UNIFORM", type,
            "pbrt-v4 `SampledWavelengths::SampleUniform(u, 400, 700)`: `(u, lambda, pdf)`.");
    for (Float u : Us()) {
        SampledWavelengths swl = SampledWavelengths::SampleUniform(u, 400, 700);
        printf("(");
        F(u);
        printf(", ");
        Fs(swl, 4);
        printf(", ");
        Fs(swl.PDF(), 4);
        printf("),\n");
    }
    End();
}

// The radiance of `RGBIlluminantSpectrum(sRGB, rgb)` at the wavelengths
// `SampleVisible(u)`, back to XYZ, Y and sRGB.
static void ToXYZ() {
    Begin("TO_XYZ", "(u32, [u32; 3], [u32; 3], u32, [u32; 3])",
            "pbrt-v4 `SampledSpectrum::ToXYZ`, `y` and `ToRGB(sRGB)` of "
            "`RGBIlluminantSpectrum(sRGB, rgb).Sample(SampleVisible(u))`: "
            "`(u, rgb, xyz, y, rgb)`.");
    std::vector<RGB> colors = Colors();
    std::vector<Float> us = Us();
    for (size_t i = 0; i < us.size(); ++i) {
        RGB rgb = colors[i % colors.size()];
        SampledWavelengths lambda = SampledWavelengths::SampleVisible(us[i]);
        SampledSpectrum s = RGBIlluminantSpectrum(*RGBColorSpace::sRGB, rgb).Sample(lambda);
        printf("(");
        F(us[i]);
        printf(", ");
        Fs(rgb, 3);
        printf(", ");
        Fs(s.ToXYZ(lambda), 3);
        printf(", ");
        F(s.y(lambda));
        printf(", ");
        Fs(s.ToRGB(lambda, *RGBColorSpace::sRGB), 3);
        printf("),\n");
    }
    End();
}

int main() {
    Allocator alloc;
    Spectra::Init(alloc);
    RGBToSpectrumTable::Init(alloc);
    RGBColorSpace::Init(alloc);

    printf("//! Generated by `known_values.cpp` from pbrt-v4 at the pinned commit; do not edit.\n");
    printf("//! Floats are their bit patterns.\n\n");
    printf("#![expect(\n    clippy::unreadable_literal,\n    clippy::type_complexity,\n"
            "    reason = \"generated tables of bit patterns\"\n)]\n\n");
    Spectra_();
    ColorSpace();
    RGBSpectra();
    Wavelengths();
    ToXYZ();
}
