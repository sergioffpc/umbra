// Generates `halton_values.rs`: the values pbrt-v4 itself computes for the
// low-discrepancy routines and the `HaltonSampler` that `umbra-sampling`
// ports, on fixed inputs. The tests compare Umbra's results with them
// (ADR 0004, ADR 0022: a seed must give pbrt-v4's numbers).
//
// `HaltonSampler` lives in `src/pbrt/samplers.h` and `samplers.cpp`, next to
// samplers that need the rest of pbrt, and `ComputeRadicalInversePermutations`
// in `src/pbrt/util/lowdiscrepancy.cpp`, next to `ToString` methods that need
// `print.cpp`; the build below copies them out. `primes.cpp` and `pstd.cpp`
// compile as they are. From the repository root, with `mmp/pbrt-v4` checked
// out at the pinned commit in `$PBRT`:
//
//   T=crates/umbra-sampling/tests/umbra_sampling/pbrt_v4
//   { echo '#include <pbrt/util/lowdiscrepancy.h>'
//     echo 'namespace pbrt {'
//     sed -n '/^pstd::vector<DigitPermutation> \*ComputeRadicalInversePermutations/,/^}/p' \
//       $PBRT/src/pbrt/util/lowdiscrepancy.cpp
//     sed -n '/^class HaltonSampler {/,/^};/p' $PBRT/src/pbrt/samplers.h \
//       | sed '/static HaltonSampler \*Create/,/Allocator alloc);/d; /Sampler Clone/d; /std::string ToString/d'
//     sed -n '/^HaltonSampler::HaltonSampler/,/^}/p' $PBRT/src/pbrt/samplers.cpp
//     echo '}'; } > /tmp/halton.h
//   g++ -std=c++17 -O3 -ffp-contract=off -DPBRT_HAVE_POSIX_MEMALIGN -I$PBRT/src -I$PBRT/src/ext \
//     -I/tmp $T/halton_values.cpp $PBRT/src/pbrt/util/primes.cpp \
//     $PBRT/src/pbrt/util/pstd.cpp -o /tmp/halton_values
//   /tmp/halton_values > $T/halton_values.rs && rustfmt --edition 2024 $T/halton_values.rs
//
// `-ffp-contract=off` keeps the compiler from fusing multiplies and adds,
// which Umbra never does implicitly. No transcendental is involved, so every
// value is exact.

#include <pbrt/util/lowdiscrepancy.h>

#include <halton.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace pbrt;

// What the `CHECK` macros of the headers call; never reached on these inputs.
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

static const int kBaseIndices[] = {0, 1, 2, 3, 10, 57, 500, 999};
static const uint64_t kIndices[] = {0, 1, 2, 7, 12345, 0x7fffffffull, 0x0123456789abcdefull};

static void PrimeTable() {
    Begin("PRIMES", "(usize, u32)", "pbrt-v4 `Primes[i]`: `(i, prime)`.");
    for (int i : {0, 1, 2, 10, 100, 500, 998, 999})
        printf("(%d, %d),\n", i, pbrt::Primes[i]);
    End();
}

static void RadicalInverses() {
    Begin("RADICAL_INVERSE", "(usize, u64, u32)",
            "pbrt-v4 `RadicalInverse(baseIndex, a)`: `(baseIndex, a, bits)`.");
    for (int b : kBaseIndices)
        for (uint64_t a : kIndices)
            printf("(%d, 0x%016llx, 0x%08x),\n", b, (unsigned long long)a,
                    Bits(RadicalInverse(b, a)));
    End();

    Begin("INVERSE_RADICAL_INVERSE", "(u64, u32, u32, u64)",
            "pbrt-v4 `InverseRadicalInverse(inverse, base, nDigits)`: "
            "`(inverse, base, nDigits, result)`.");
    for (uint64_t inverse : {0ull, 1ull, 5ull, 100ull, 127ull, 242ull})
        for (int base : {2, 3})
            for (int nDigits : {0, 1, 5, 7})
                printf("(%llu, %d, %d, %llu),\n", (unsigned long long)inverse, base, nDigits,
                        (unsigned long long)InverseRadicalInverse(inverse, base, nDigits));
    End();

    Begin("PERMUTATION_ELEMENT", "(u32, u32, u32, u32)",
            "pbrt-v4 `PermutationElement(i, l, p)`: `(i, l, p, result)`.");
    for (uint32_t l : {1u, 2u, 3u, 17u, 7919u, 65536u})
        for (uint32_t p : {0u, 1u, 0xdeadbeefu, 0xffffffffu})
            for (uint32_t i : {0u, 1u, 2u, 16u})
                if (i < l)
                    printf("(%u, %u, 0x%08x, %d),\n", i, l, p, PermutationElement(i, l, p));
    End();

    Begin("OWEN_SCRAMBLED_RADICAL_INVERSE", "(usize, u64, u32, u32)",
            "pbrt-v4 `OwenScrambledRadicalInverse(baseIndex, a, hash)`: "
            "`(baseIndex, a, hash, bits)`.");
    for (int b : kBaseIndices)
        for (uint64_t a : kIndices)
            for (uint32_t hash : {0u, 0x9e3779b9u})
                printf("(%d, 0x%016llx, 0x%08x, 0x%08x),\n", b, (unsigned long long)a, hash,
                        Bits(OwenScrambledRadicalInverse(b, a, hash)));
    End();
}

static void ScrambledRadicalInverses() {
    Begin("SCRAMBLED_RADICAL_INVERSE", "(u32, usize, u64, u32)",
            "pbrt-v4 `ScrambledRadicalInverse(baseIndex, a, "
            "(*ComputeRadicalInversePermutations(seed))[baseIndex])`: "
            "`(seed, baseIndex, a, bits)`.");
    for (uint32_t seed : {0u, 7u, 0xffffffffu}) {
        pstd::vector<DigitPermutation> *perms = ComputeRadicalInversePermutations(seed);
        for (int b : kBaseIndices)
            for (uint64_t a : kIndices)
                printf("(0x%08x, %d, 0x%016llx, 0x%08x),\n", seed, b, (unsigned long long)a,
                        Bits(ScrambledRadicalInverse(b, a, (*perms)[b])));
    }
    End();
}

// One Pixel Sample's draws in the order `RayIntegrator::EvaluatePixelSample`
// makes them (wavelengths, filter, time, lens), then three more.
static void Draws(HaltonSampler &sampler, Point2i p, int sampleIndex) {
    sampler.StartPixelSample(p, sampleIndex, 0);
    float v[10];
    v[0] = sampler.Get1D();
    Point2f pixel = sampler.GetPixel2D();
    v[1] = pixel.x;
    v[2] = pixel.y;
    v[3] = sampler.Get1D();
    Point2f lens = sampler.Get2D();
    v[4] = lens.x;
    v[5] = lens.y;
    v[6] = sampler.Get1D();
    Point2f u = sampler.Get2D();
    v[7] = u.x;
    v[8] = u.y;
    v[9] = sampler.Get1D();
    printf("(%d, %d, %d, [", p.x, p.y, sampleIndex);
    for (float f : v)
        printf("0x%08x, ", Bits(f));
    printf("]),\n");
}

static void Haltons() {
    struct Case {
        const char *name;
        RandomizeStrategy randomize;
        const char *randomizeName;
        int seed;
        Point2i resolution;
    };
    const Case cases[] = {
        {"HALTON_NONE", RandomizeStrategy::None, "None", 0, {64, 48}},
        {"HALTON_PERMUTE_DIGITS", RandomizeStrategy::PermuteDigits, "PermuteDigits", 0, {1280, 720}},
        {"HALTON_PERMUTE_DIGITS_SEED_7", RandomizeStrategy::PermuteDigits, "PermuteDigits", 7, {16, 16}},
        {"HALTON_OWEN", RandomizeStrategy::Owen, "Owen", 0, {200, 1}},
    };
    for (const Case &c : cases) {
        std::string doc =
            std::string("pbrt-v4 `HaltonSampler(16, Point2i(") +
            std::to_string(c.resolution.x) + ", " + std::to_string(c.resolution.y) +
            "), " + c.randomizeName + ", " + std::to_string(c.seed) +
            ")`: `(x, y, sampleIndex, draws)` after `StartPixelSample(Point2i(x, y), "
            "sampleIndex, 0)`, the draws being `Get1D`, `GetPixel2D`, `Get1D`, `Get2D`, "
            "`Get1D`, `Get2D`, `Get1D`.";
        Begin(c.name, "(i32, i32, u32, [u32; 10])", doc.c_str());
        HaltonSampler sampler(16, c.resolution, c.randomize, c.seed);
        for (Point2i p : {Point2i(0, 0), Point2i(1, 0), Point2i(5, 3), Point2i(127, 127),
                            Point2i(130, 260), Point2i(1279, 719)})
            for (int sampleIndex : {0, 1, 15, 1000})
                Draws(sampler, p, sampleIndex);
        End();
    }

    Begin("HALTON_DIMENSION_WRAP", "u32",
            "pbrt-v4 `HaltonSampler(16, Point2i(8, 8), PermuteDigits, 0)`: 1100 `Get1D` "
            "draws after `StartPixelSample(Point2i(3, 4), 2, 0)`, every hundredth, past "
            "`PrimeTableSize` where the dimension wraps to 2.");
    HaltonSampler sampler(16, Point2i(8, 8), RandomizeStrategy::PermuteDigits, 0);
    sampler.StartPixelSample(Point2i(3, 4), 2, 0);
    for (int i = 0; i < 1100; ++i) {
        float f = sampler.Get1D();
        if (i % 100 == 99)
            printf("0x%08x,\n", Bits(f));
    }
    End();
}

int main() {
    printf("//! Generated by `halton_values.cpp` from pbrt-v4 at the pinned commit; do not "
            "edit.\n//! Floats are their bit patterns.\n\n#![expect(\n    "
            "clippy::unreadable_literal,\n    reason = \"generated tables of bit "
            "patterns\"\n)]\n\n");
    PrimeTable();
    RadicalInverses();
    ScrambledRadicalInverses();
    Haltons();
}
