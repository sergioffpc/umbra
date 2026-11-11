// Generates `known_values.rs`: the values pbrt-v4 itself computes for the
// hashing, RNG and sampling routines `umbra-sampling` ports, on fixed inputs.
// The tests compare Umbra's results with them (ADR 0004: a seed must give
// pbrt-v4's numbers).
//
// pbrt-v4 at the pinned commit (arc42 §8.11) is header-only for everything
// used here except the allocator (`src/pbrt/util/pstd.cpp`, compiled as is)
// and the spherical-triangle and Henyey–Greenstein sampling functions, which
// live in `src/pbrt/util/sampling.cpp` next to code that needs the rest of
// pbrt; the build below copies them out of it. From the repository root, with
// `mmp/pbrt-v4` checked out at the pinned commit in `$PBRT`:
//
//   T=crates/umbra-sampling/tests/umbra_sampling/pbrt_v4
//   { echo '#include <pbrt/util/sampling.h>'; echo '#include <pbrt/util/scattering.h>'
//     echo 'namespace pbrt {'
//     sed -n '/^PBRT_CPU_GPU pstd::array<Float, 3> SampleSphericalTriangle/,/^PBRT_CPU_GPU Point3f SampleSphericalRectangle/p' \
//       $PBRT/src/pbrt/util/sampling.cpp | sed '$d' \
//       | sed 's/CHECK_RARE([^;]*;//; s/CHECK_GT([^;]*;//; s/CHECK(!IsNaN(cosBp));//'
//     sed -n '/^PBRT_CPU_GPU Vector3f SampleHenyeyGreenstein/,/^Point2f RejectionSampleDisk/p' \
//       $PBRT/src/pbrt/util/sampling.cpp | sed '$d'
//     echo '}'; } > /tmp/sampling_functions.cpp
//   g++ -std=c++17 -O3 -ffp-contract=off -DPBRT_HAVE_POSIX_MEMALIGN -I$PBRT/src -I$PBRT/src/ext \
//     $T/known_values.cpp /tmp/sampling_functions.cpp $PBRT/src/pbrt/util/pstd.cpp -o /tmp/known_values
//   /tmp/known_values > $T/known_values.rs && rustfmt --edition 2024 $T/known_values.rs
//
// `-ffp-contract=off` keeps the compiler from fusing multiplies and adds,
// which Umbra never does implicitly (ADR 0002). Transcendentals come from the
// platform libm (glibc), not Umbra's own kernels or the glibc of its image,
// so values computed through them can differ in the last bits; the tests say how many ulps they allow.

#include <pbrt/util/hash.h>
#include <pbrt/util/rng.h>
#include <pbrt/util/sampling.h>
#include <pbrt/util/scattering.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <cstring>
#include <vector>

using namespace pbrt;

// What the `CHECK` macros of the headers call; never reached on these inputs.
// They stand in for `log.cpp` and `print.cpp`, which need the rest of pbrt.
namespace pbrt {
void LogFatal(LogLevel, const char *file, int line, const char *message) {
    fprintf(stderr, "%s:%d: %s\n", file, line, message);
    abort();
}
namespace detail {
void stringPrintfRecursive(std::string *s, const char *fmt) { *s += fmt; }
std::string copyToFormatString(const char **, std::string *) { abort(); }
}  // namespace detail
}  // namespace pbrt

static uint32_t Bits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}

static void Begin(const char *name, const char *type, const char *doc) {
    printf("/// %s\n#[rustfmt::skip]\npub(crate) const %s: &[%s] = &[\n", doc, name, type);
}

static void End() { printf("];\n\n"); }

static void P2(Point2f p) { printf("[0x%08x, 0x%08x]", Bits(p.x), Bits(p.y)); }
static void V3(Vector3f v) {
    printf("[0x%08x, 0x%08x, 0x%08x]", Bits(v.x), Bits(v.y), Bits(v.z));
}
static void F(float f) { printf("0x%08x", Bits(f)); }

// The sample points every 2D warp is evaluated at: a few edge cases, then
// pbrt-v4's default `RNG` stream.
static std::vector<Point2f> SamplePoints() {
    std::vector<Point2f> u = {{0, 0}, {0.5f, 0.5f}, {0.25f, 0.75f},
                                {OneMinusEpsilon, OneMinusEpsilon}, {0, OneMinusEpsilon}};
    RNG rng;
    for (int i = 0; i < 27; ++i) {
        Float x = rng.Uniform<Float>();
        u.push_back({x, rng.Uniform<Float>()});
    }
    return u;
}

static void Hashing() {
    Begin("MIX_BITS", "(u64, u64)", "pbrt-v4 `MixBits(v)`: `(v, result)`.");
    for (uint64_t v : {0ull, 1ull, 2ull, 0xdeadbeefull, 0x0123456789abcdefull, ~0ull})
        printf("(0x%016llx, 0x%016llx),\n", (unsigned long long)v,
                (unsigned long long)MixBits(v));
    End();

    Begin("MURMUR_HASH_64A", "(usize, u64, u64)",
            "pbrt-v4 `MurmurHash64A` of the first `len` bytes `(37 i + 11) mod 256`: "
            "`(len, seed, result)`.");
    unsigned char key[32];
    for (int i = 0; i < 32; ++i)
        key[i] = (unsigned char)(37 * i + 11);
    for (uint64_t seed : {0ull, 0x9e3779b97f4a7c15ull})
        for (size_t len = 0; len <= 17; ++len)
            printf("(%zu, 0x%016llx, 0x%016llx),\n", len, (unsigned long long)seed,
                    (unsigned long long)MurmurHash64A(key, len, seed));
    End();

    printf("/// pbrt-v4 `Hash(args...)` on the argument lists named in each comment.\n");
    printf("#[rustfmt::skip]\npub(crate) const HASH: &[u64] = &[\n");
    printf("0x%016llx, // (0_i32)\n", (unsigned long long)Hash(int32_t(0)));
    printf("0x%016llx, // (37_i32)\n", (unsigned long long)Hash(int32_t(37)));
    printf("0x%016llx, // (-12511_i64)\n", (unsigned long long)Hash(int64_t(-12511)));
    printf("0x%016llx, // (0xfacebeef_u64)\n",
            (unsigned long long)Hash(uint64_t(0xfacebeef)));
    printf("0x%016llx, // (Point2i(3, 7), 42_i32)\n",
            (unsigned long long)Hash(Point2i(3, 7), int32_t(42)));
    printf("0x%016llx, // (0.25_f32, Point2f(0.5, 0.75))\n",
            (unsigned long long)Hash(0.25f, Point2f(0.5f, 0.75f)));
    printf("0x%016llx, // (0_i32, Vector3f(0.1, -0.2, 0.3))\n",
            (unsigned long long)Hash(int32_t(0), Vector3f(0.1f, -0.2f, 0.3f)));
    printf("0x%016llx, // (Point2i(-1, 5), 9_i64, 3_i32)\n",
            (unsigned long long)Hash(Point2i(-1, 5), int64_t(9), int32_t(3)));
    printf("0x%016llx, // (Point3f(1, 2, 3), Normal3f(0, 0, 1), 7_u32)\n",
            (unsigned long long)Hash(Point3f(1, 2, 3), Normal3f(0, 0, 1), uint32_t(7)));
    End();

    Begin("HASH_FLOAT", "u32",
            "pbrt-v4 `HashFloat(args...)`, as bits, on the argument lists named in each "
            "comment.");
    printf("0x%08x, // (0_i32)\n", Bits(HashFloat(int32_t(0))));
    printf("0x%08x, // (Point3f(1, 2, 3), Vector3f(0.1, -0.2, 0.3))\n",
            Bits(HashFloat(Point3f(1, 2, 3), Vector3f(0.1f, -0.2f, 0.3f))));
    End();
}

static void Rngs() {
    Begin("RNG_DEFAULT_U32", "u32", "pbrt-v4 `RNG()`: the first `Uniform<uint32_t>()` values.");
    {
        RNG rng;
        for (int i = 0; i < 8; ++i)
            printf("0x%08x,\n", rng.Uniform<uint32_t>());
    }
    End();

    Begin("RNG_SEQUENCE_U32", "(u64, [u32; 4])",
            "pbrt-v4 `RNG(sequenceIndex)`: `(sequenceIndex, first four Uniform<uint32_t>())`.");
    for (uint64_t seq : {0ull, 1ull, 1234ull, 0xdeadbeefcafeull}) {
        RNG rng(seq);
        printf("(0x%llx, [", (unsigned long long)seq);
        for (int i = 0; i < 4; ++i)
            printf("0x%08x, ", rng.Uniform<uint32_t>());
        printf("]),\n");
    }
    End();

    Begin("RNG_OFFSET_U32", "u32",
            "pbrt-v4 `RNG(1234, 6502)`: the first `Uniform<uint32_t>()` values.");
    {
        RNG rng(1234, 6502);
        for (int i = 0; i < 4; ++i)
            printf("0x%08x,\n", rng.Uniform<uint32_t>());
    }
    End();

    Begin("RNG_DEFAULT_F32", "u32", "pbrt-v4 `RNG()`: the first `Uniform<float>()`, as bits.");
    {
        RNG rng;
        for (int i = 0; i < 4; ++i)
            F(rng.Uniform<float>()), printf(",\n");
    }
    End();

    Begin("RNG_DEFAULT_U64", "u64", "pbrt-v4 `RNG()`: the first `Uniform<uint64_t>()` values.");
    {
        RNG rng;
        for (int i = 0; i < 3; ++i)
            printf("0x%016llx,\n", (unsigned long long)rng.Uniform<uint64_t>());
    }
    End();

    Begin("RNG_DEFAULT_I32", "i32", "pbrt-v4 `RNG()`: the first `Uniform<int32_t>()` values.");
    {
        RNG rng;
        for (int i = 0; i < 6; ++i)
            printf("%d,\n", rng.Uniform<int32_t>());
    }
    End();

    Begin("RNG_DEFAULT_I64", "i64", "pbrt-v4 `RNG()`: the first `Uniform<int64_t>()` values.");
    {
        RNG rng;
        for (int i = 0; i < 4; ++i)
            printf("%lld,\n", (long long)rng.Uniform<int64_t>());
    }
    End();

    Begin("RNG_BELOW", "(u32, [u32; 4])",
            "pbrt-v4 `RNG(7)`, then `Uniform<uint32_t>(b)` four times per bound: `(b, values)`.");
    {
        RNG rng(7);
        for (uint32_t b : {1u, 7u, 1000u, 0x80000001u}) {
            printf("(0x%08x, [", b);
            for (int i = 0; i < 4; ++i)
                printf("0x%08x, ", rng.Uniform<uint32_t>(b));
            printf("]),\n");
        }
    }
    End();

    Begin("RNG_ADVANCE", "(i64, u32)",
            "pbrt-v4 `RNG(77)`, `Advance(delta)` cumulatively, then one `Uniform<uint32_t>()` "
            "after each: `(delta, value)`.");
    {
        RNG rng(77);
        for (int64_t delta : {int64_t(1000), int64_t(-37), int64_t(0), int64_t(1) << 40}) {
            rng.Advance(delta);
            printf("(%lld, 0x%08x),\n", (long long)delta, rng.Uniform<uint32_t>());
        }
    }
    End();
}

static void SphereWarps(const std::vector<Point2f> &us) {
    Begin("SAMPLE_UNIFORM_DISK_CONCENTRIC", "([u32; 2], [u32; 2], [u32; 2])",
            "pbrt-v4 `SampleUniformDiskConcentric(u)` and its inverse: `(u, p, inverse)`.");
    for (Point2f u : us) {
        Point2f p = SampleUniformDiskConcentric(u);
        printf("("), P2(u), printf(", "), P2(p), printf(", "),
            P2(InvertUniformDiskConcentricSample(p)), printf("),\n");
    }
    End();

    Begin("SAMPLE_UNIFORM_DISK_POLAR", "([u32; 2], [u32; 2], [u32; 2])",
            "pbrt-v4 `SampleUniformDiskPolar(u)` and its inverse: `(u, p, inverse)`.");
    for (Point2f u : us) {
        Point2f p = SampleUniformDiskPolar(u);
        printf("("), P2(u), printf(", "), P2(p), printf(", "),
            P2(InvertUniformDiskPolarSample(p)), printf("),\n");
    }
    End();

    struct {
        const char *name, *doc;
        Vector3f (*sample)(Point2f);
        Point2f (*invert)(Vector3f);
    } warps[] = {
        {"SAMPLE_UNIFORM_SPHERE", "`SampleUniformSphere(u)`", SampleUniformSphere,
            InvertUniformSphereSample},
        {"SAMPLE_UNIFORM_HEMISPHERE", "`SampleUniformHemisphere(u)`",
            SampleUniformHemisphere, InvertUniformHemisphereSample},
        {"SAMPLE_COSINE_HEMISPHERE", "`SampleCosineHemisphere(u)`", SampleCosineHemisphere,
            InvertCosineHemisphereSample},
    };
    for (auto &w : warps) {
        char doc[200];
        snprintf(doc, sizeof(doc), "pbrt-v4 %s and its inverse: `(u, w, inverse)`.", w.doc);
        Begin(w.name, "([u32; 2], [u32; 3], [u32; 2])", doc);
        for (Point2f u : us) {
            Vector3f v = w.sample(u);
            printf("("), P2(u), printf(", "), V3(v), printf(", "), P2(w.invert(v)),
                printf("),\n");
        }
        End();
    }

    Begin("SAMPLE_UNIFORM_CONE", "([u32; 2], [u32; 3], [u32; 2])",
            "pbrt-v4 `SampleUniformCone(u, 0.3)` and its inverse: `(u, w, inverse)`.");
    for (Point2f u : us) {
        Vector3f v = SampleUniformCone(u, 0.3f);
        printf("("), P2(u), printf(", "), V3(v), printf(", "),
            P2(InvertUniformConeSample(v, 0.3f)), printf("),\n");
    }
    End();

    Begin("WARP_PDFS", "u32",
            "pbrt-v4 `UniformSpherePDF()`, `UniformHemispherePDF()`, "
            "`CosineHemispherePDF(0.6)`, `UniformConePDF(0.3)`, as bits.");
    F(UniformSpherePDF()), printf(",\n");
    F(UniformHemispherePDF()), printf(",\n");
    F(CosineHemispherePDF(0.6f)), printf(",\n");
    F(UniformConePDF(0.3f)), printf(",\n");
    End();
}

static void TriangleWarps(const std::vector<Point2f> &us) {
    Begin("SAMPLE_UNIFORM_TRIANGLE", "([u32; 2], [u32; 3], [u32; 2])",
            "pbrt-v4 `SampleUniformTriangle(u)` and its inverse: `(u, b, inverse)`.");
    for (Point2f u : us) {
        pstd::array<Float, 3> b = SampleUniformTriangle(u);
        printf("("), P2(u), printf(", "), V3(Vector3f(b[0], b[1], b[2])), printf(", "),
            P2(InvertUniformTriangleSample(b)), printf("),\n");
    }
    End();

    pstd::array<Point3f, 3> v = {Point3f(4, 1, 1), Point3f(-10, 3, 3), Point3f(-2, -8, 10)};
    Point3f p(.5f, -.4f, .7f);
    Begin("SAMPLE_SPHERICAL_TRIANGLE", "([u32; 2], [u32; 3], u32, [u32; 2])",
            "pbrt-v4 `SampleSphericalTriangle(v, p, u, &pdf)` for the vertices and point of "
            "`TEST(Sampling, SphericalTriangle)`, and `InvertSphericalTriangleSample` of the "
            "direction to the sampled point: `(u, b, pdf, inverse)`.");
    for (Point2f u : us) {
        Float pdf;
        pstd::array<Float, 3> b = SampleSphericalTriangle(v, p, u, &pdf);
        Point3f pTri = b[0] * v[0] + b[1] * v[1] + b[2] * v[2];
        printf("("), P2(u), printf(", "), V3(Vector3f(b[0], b[1], b[2])), printf(", "),
            F(pdf), printf(", "),
            P2(InvertSphericalTriangleSample(v, p, Normalize(pTri - p))), printf("),\n");
    }
    End();
}

static void PlanarWarps(const std::vector<Point2f> &us) {
    Begin("SAMPLE_LINEAR", "(u32, u32, u32, u32)",
            "pbrt-v4 `SampleLinear(u, 0.5, 2)`, `LinearPDF(x, 0.5, 2)` and "
            "`InvertLinearSample(x, 0.5, 2)`: `(u, x, pdf, inverse)`.");
    for (Point2f u : us) {
        Float x = SampleLinear(u.x, 0.5f, 2);
        printf("("), F(u.x), printf(", "), F(x), printf(", "), F(LinearPDF(x, 0.5f, 2)),
            printf(", "), F(InvertLinearSample(x, 0.5f, 2)), printf("),\n");
    }
    End();

    Float w[4] = {0.1f, 0.5f, 1.3f, 4.7f};
    Begin("SAMPLE_BILINEAR", "([u32; 2], [u32; 2], u32, [u32; 2])",
            "pbrt-v4 `SampleBilinear(u, {0.1, 0.5, 1.3, 4.7})`, `BilinearPDF` and "
            "`InvertBilinearSample` there: `(u, p, pdf, inverse)`.");
    for (Point2f u : us) {
        Point2f q = SampleBilinear(u, w);
        printf("("), P2(u), printf(", "), P2(q), printf(", "), F(BilinearPDF(q, w)),
            printf(", "), P2(InvertBilinearSample(q, w)), printf("),\n");
    }
    End();

    Begin("SAMPLE_VISIBLE_WAVELENGTHS", "(u32, u32, u32)",
            "pbrt-v4 `SampleVisibleWavelengths(u)` and `VisibleWavelengthsPDF` there: "
            "`(u, lambda, pdf)`.");
    for (Point2f u : us) {
        Float lambda = SampleVisibleWavelengths(u.x);
        printf("("), F(u.x), printf(", "), F(lambda), printf(", "),
            F(VisibleWavelengthsPDF(lambda)), printf("),\n");
    }
    End();

    Begin("SAMPLE_EXPONENTIAL", "(u32, u32, u32, u32)",
            "pbrt-v4 `SampleExponential(u, 1.7)`, `ExponentialPDF` and "
            "`InvertExponentialSample` there: `(u, x, pdf, inverse)`.");
    for (Point2f u : us) {
        Float x = SampleExponential(u.x, 1.7f);
        printf("("), F(u.x), printf(", "), F(x), printf(", "), F(ExponentialPDF(x, 1.7f)),
            printf(", "), F(InvertExponentialSample(x, 1.7f)), printf("),\n");
    }
    End();

    Float weights[] = {0.1f, 0.4f, 0, 0.25f, 0.25f};
    Begin("SAMPLE_DISCRETE", "(u32, usize, u32, u32)",
            "pbrt-v4 `SampleDiscrete({0.1, 0.4, 0, 0.25, 0.25}, u, &pmf, &uRemapped)`: "
            "`(u, index, pmf, uRemapped)`.");
    for (Point2f u : us) {
        Float pmf, uRemapped;
        int index = SampleDiscrete(weights, u.x, &pmf, &uRemapped);
        printf("("), F(u.x), printf(", %d, ", index), F(pmf), printf(", "), F(uRemapped),
            printf("),\n");
    }
    End();

    Begin("HEURISTICS", "(i32, u32, i32, u32, u32, u32)",
            "pbrt-v4 `BalanceHeuristic` and `PowerHeuristic`: "
            "`(nf, fPdf, ng, gPdf, balance, power)`.");
    struct {
        int nf;
        Float fPdf;
        int ng;
        Float gPdf;
    } hs[] = {{1, 0.5f, 1, 0.25f}, {1, 3.7f, 2, 0.1f}, {4, 0.01f, 1, 10}, {1, 1e30f, 1, 1}};
    for (auto h : hs) {
        printf("(%d, ", h.nf), F(h.fPdf), printf(", %d, ", h.ng), F(h.gPdf), printf(", "),
            F(BalanceHeuristic(h.nf, h.fPdf, h.ng, h.gPdf)), printf(", "),
            F(PowerHeuristic(h.nf, h.fPdf, h.ng, h.gPdf)), printf("),\n");
    }
    End();
}

static void HenyeyGreensteins(const std::vector<Point2f> &us) {
    Vector3f wo = Normalize(Vector3f(0.3f, -0.5f, 0.8f));
    Begin("SAMPLE_HENYEY_GREENSTEIN", "(u32, [u32; 2], [u32; 3], u32)",
            "pbrt-v4 `SampleHenyeyGreenstein(Normalize(Vector3f(0.3, -0.5, 0.8)), g, u, &pdf)`: "
            "`(g, u, wi, pdf)`.");
    for (Float g : {-0.7f, 0.f, 0.0005f, 0.4f, 0.999f})
        for (Point2f u : us) {
            Float pdf;
            Vector3f wi = SampleHenyeyGreenstein(wo, g, u, &pdf);
            printf("("), F(g), printf(", "), P2(u), printf(", "), V3(wi), printf(", "), F(pdf),
                printf("),\n");
        }
    End();

    Begin("HENYEY_GREENSTEIN", "(u32, u32, u32)",
            "pbrt-v4 `HenyeyGreenstein(cosTheta, g)`: `(cosTheta, g, value)`.");
    for (Float g : {-0.999f, -0.3f, 0.f, 0.6f, 1.f})
        for (Float cosTheta : {-1.f, -0.25f, 0.f, 0.5f, 1.f})
            printf("("), F(cosTheta), printf(", "), F(g), printf(", "),
                F(HenyeyGreenstein(cosTheta, g)), printf("),\n");
    End();
}

static void Distributions(const std::vector<Point2f> &us) {
    std::vector<Float> f1 = {0, 1.25f, 0.5f, 0.25f, 3.7f};
    PiecewiseConstant1D d1(f1, -1, 3);
    Begin("PIECEWISE_CONSTANT_1D_CDF", "u32",
            "pbrt-v4 `PiecewiseConstant1D({0, 1.25, 0.5, 0.25, 3.7}, -1, 3)`: its CDF, as "
            "bits, then its integral.");
    for (Float c : d1.cdf)
        F(c), printf(",\n");
    F(d1.Integral()), printf(",\n");
    End();

    Begin("PIECEWISE_CONSTANT_1D", "(u32, u32, u32, usize, u32)",
            "pbrt-v4 `Sample(u, &pdf, &offset)` of the distribution above, and `Invert` of the "
            "result: `(u, x, pdf, offset, inverse)`.");
    for (Point2f u : us) {
        Float pdf;
        int offset;
        Float x = d1.Sample(u.x, &pdf, &offset);
        printf("("), F(u.x), printf(", "), F(x), printf(", "), F(pdf),
            printf(", %d, ", offset), F(*d1.Invert(x)), printf("),\n");
    }
    End();

    std::vector<Float> f2;
    RNG rng;
    for (int i = 0; i < 4 * 5; ++i)
        f2.push_back(rng.Uniform<Float>());
    PiecewiseConstant2D d2(f2, 4, 5, Bounds2f(Point2f(-1, -0.5f), Point2f(3, 1.5f)));
    Begin("PIECEWISE_CONSTANT_2D", "([u32; 2], [u32; 2], u32, [usize; 2], u32, [u32; 2])",
            "pbrt-v4 `PiecewiseConstant2D` of 4 × 5 values from `RNG()` over "
            "[-1, 3] × [-0.5, 1.5]: `Sample(u, &pdf, &offset)`, `PDF` and `Invert` of the "
            "result: `(u, p, pdf, offset, PDF(p), inverse)`.");
    for (Point2f u : us) {
        Float pdf;
        Point2i offset;
        Point2f p = d2.Sample(u, &pdf, &offset);
        printf("("), P2(u), printf(", "), P2(p), printf(", "), F(pdf),
            printf(", [%d, %d], ", offset.x, offset.y), F(d2.PDF(p)), printf(", "),
            P2(*d2.Invert(p)), printf("),\n");
    }
    End();
    printf("/// The integral of the 2D distribution above, as bits.\n");
    printf("pub(crate) const PIECEWISE_CONSTANT_2D_INTEGRAL: u32 = 0x%08x;\n",
            Bits(d2.Integral()));
}

int main() {
    printf("//! Generated by `known_values.cpp` from pbrt-v4 at the pinned commit; do not "
            "edit.\n//! Floats are their bit patterns.\n\n#![expect(\n    clippy::unreadable_literal,\n    "
            "clippy::type_complexity,\n    reason = \"generated tables of bit patterns\"\n)]\n\n");
    std::vector<Point2f> us = SamplePoints();
    Hashing();
    Rngs();
    SphereWarps(us);
    TriangleWarps(us);
    PlanarWarps(us);
    HenyeyGreensteins(us);
    Distributions(us);
}
