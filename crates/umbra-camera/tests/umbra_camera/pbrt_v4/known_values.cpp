// Generates `known_values.rs`: the values pbrt-v4 itself computes for the
// camera, filter and sensor that `umbra-camera` ports, on fixed inputs. The
// tests compare Umbra's results with them.
//
// Cameras and films need most of pbrt, so this links the library of a
// pbrt-v4 build at the pinned commit (arc42 §8.11), which needs OpenEXR 3.1
// (`libopenexr-dev` on Ubuntu). From the repository root, with `mmp/pbrt-v4`
// checked out in `$PBRT` and built in `$BUILD` with its default CMake
// configuration:
//
//   T=crates/umbra-camera/tests/umbra_camera/pbrt_v4
//   g++ -std=c++17 -O3 -DPBRT_HAVE_POSIX_MEMALIGN -I$PBRT/src -I$PBRT/src/ext -I$BUILD \
//     -I$PBRT/src/ext/openvdb/nanovdb $T/known_values.cpp $BUILD/libpbrt_lib.a \
//     $BUILD/src/ext/ptex/src/ptex/libPtex.a $BUILD/src/ext/libdeflate/libdeflate.a \
//     $BUILD/src/ext/double-conversion/libdouble-conversion.a \
//     $BUILD/src/ext/utf8proc/libutf8proc.a $BUILD/src/ext/glfw/src/libglfw3.a \
//     $BUILD/src/ext/glad/libglad.a -lOpenEXR-3_1 -lImath-3_1 -lIlmThread-3_1 -lIex-3_1 \
//     -lz -ldl -o /tmp/known_values
//   /tmp/known_values > $T/known_values.rs && rustfmt --edition 2024 $T/known_values.rs
//
// pbrt-v4's build compiles the library with `-march=native` in GNU C++ mode,
// which lets the compiler fuse multiplies and adds, and its transcendentals
// come from the libm it was built with: the tests compare within a few ulps.

#include <pbrt/cameras.h>
#include <pbrt/film.h>
#include <pbrt/filters.h>
#include <pbrt/options.h>
#include <pbrt/pbrt.h>
#include <pbrt/samplers.h>
#include <pbrt/util/colorspace.h>
#include <pbrt/util/spectrum.h>
#include <pbrt/util/transform.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

using namespace pbrt;

static uint32_t Bits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}

static void Begin(const char *name, const char *type, const char *doc) {
    printf("/// %s\n#[rustfmt::skip]\npub(crate) const %s: &[%s] = &[\n", doc, name, type);
}

static void End() { printf("];\n\n"); }

static const Point2f kUs[] = {{0, 0},           {0.5f, 0.5f},  {0.25f, 0.75f},
                                {0.999f, 0.001f}, {0.1f, 0.9f},  {0.3337f, 0.6129f},
                                {0.71f, 0.05f},   {0.9999f, 0.9999f}};

static void Gaussians() {
    struct Case {
        Vector2f radius;
        Float sigma;
    };
    const Case cases[] = {{{1.5f, 1.5f}, 0.5f}, {{2.f, 1.f}, 0.3f}};

    Begin("GAUSSIAN_EVALUATE", "([u32; 3], [u32; 2], u32)",
            "pbrt-v4 `GaussianFilter(radius, sigma).Evaluate(p)`: "
            "`([radius.x, radius.y, sigma], p, value)`.");
    for (const Case &c : cases) {
        GaussianFilter f(c.radius, c.sigma);
        for (Point2f p : {Point2f(0, 0), Point2f(0.25f, -0.5f), Point2f(1.f, 0.9f),
                            Point2f(-1.4f, 0.1f), Point2f(1.5f, 0.f), Point2f(1.9f, 0.5f)})
            printf("([0x%08x, 0x%08x, 0x%08x], [0x%08x, 0x%08x], 0x%08x),\n", Bits(c.radius.x),
                    Bits(c.radius.y), Bits(c.sigma), Bits(p.x), Bits(p.y),
                    Bits(f.Evaluate(p)));
    }
    End();

    Begin("GAUSSIAN_INTEGRAL", "([u32; 3], u32)",
            "pbrt-v4 `GaussianFilter(radius, sigma).Integral()`: "
            "`([radius.x, radius.y, sigma], integral)`.");
    for (const Case &c : cases) {
        GaussianFilter f(c.radius, c.sigma);
        printf("([0x%08x, 0x%08x, 0x%08x], 0x%08x),\n", Bits(c.radius.x), Bits(c.radius.y),
                Bits(c.sigma), Bits(f.Integral()));
    }
    End();

    Begin("GAUSSIAN_SAMPLE", "([u32; 3], [u32; 2], [u32; 3])",
            "pbrt-v4 `GaussianFilter(radius, sigma).Sample(u)`: "
            "`([radius.x, radius.y, sigma], u, [p.x, p.y, weight])`.");
    for (const Case &c : cases) {
        GaussianFilter f(c.radius, c.sigma);
        for (Point2f u : kUs) {
            FilterSample s = f.Sample(u);
            printf("([0x%08x, 0x%08x, 0x%08x], [0x%08x, 0x%08x], [0x%08x, 0x%08x, 0x%08x]),\n",
                    Bits(c.radius.x), Bits(c.radius.y), Bits(c.sigma), Bits(u.x), Bits(u.y),
                    Bits(s.p.x), Bits(s.p.y), Bits(s.weight));
        }
    }
    End();
}

static RGBFilm *MakeFilm(Point2i resolution, Filter filter) {
    const PixelSensor *sensor = PixelSensor::CreateDefault();
    FilmBaseParameters p(resolution, Bounds2i(Point2i(0, 0), resolution), filter, 35.f,
                            sensor, "unused.exr");
    return new RGBFilm(p, RGBColorSpace::sRGB, Infinity, false);
}

static void PerspectiveRays() {
    struct Case {
        const char *name;
        const char *doc;
        Transform worldFromCamera;
        Point2i resolution;
        Float fov, lensRadius, focalDistance;
    };
    const Case cases[] = {
        {"PERSPECTIVE_PINHOLE",
            "pbrt-v4 `PerspectiveCamera` with the identity `worldFromCamera`, a 16 x 9 film, a "
            "field of view of 90 degrees and no lens",
            Transform(), Point2i(16, 9), 90.f, 0.f, 1e6f},
        {"PERSPECTIVE_THIN_LENS",
            "pbrt-v4 `PerspectiveCamera` with `worldFromCamera` the inverse of `LookAt((1, 2, "
            "3), (0, 0.5, 0), (0, 1, 0))`, a 32 x 64 film, a field of view of 45 degrees and a "
            "lens of radius 0.1 focused at 5",
            Inverse(LookAt(Point3f(1, 2, 3), Point3f(0, 0.5f, 0), Vector3f(0, 1, 0))),
            Point2i(32, 64), 45.f, 0.1f, 5.f},
    };
    for (const Case &c : cases) {
        Filter filter = GaussianFilter::Create(ParameterDictionary(), nullptr, {});
        CameraBaseParameters base;
        base.cameraTransform = CameraTransform(AnimatedTransform(c.worldFromCamera));
        base.film = MakeFilm(c.resolution, filter);
        Float frame = Float(c.resolution.x) / Float(c.resolution.y);
        Bounds2f screen = frame > 1 ? Bounds2f(Point2f(-frame, -1), Point2f(frame, 1))
                                    : Bounds2f(Point2f(-1, -1 / frame), Point2f(1, 1 / frame));
        PerspectiveCamera camera(base, c.fov, screen, c.lensRadius, c.focalDistance);

        std::string doc = std::string(c.doc) +
                            ": `GenerateRay` of a camera sample at `pFilm` and `pLens`: "
                            "`([pFilm, pLens], [o, d])`.";
        Begin(c.name, "([u32; 4], [u32; 6])", doc.c_str());
        SampledWavelengths lambda = SampledWavelengths::SampleVisible(0.5f);
        for (Point2f pFilm : {Point2f(0, 0), Point2f(8, 4.5f), Point2f(0.5f, 0.5f),
                                Point2f(15.25f, 8.75f), Point2f(3.3f, 60.1f)})
            for (Point2f pLens : {Point2f(0.5f, 0.5f), Point2f(0.1f, 0.8f)}) {
                CameraSample sample;
                sample.pFilm = pFilm;
                sample.pLens = pLens;
                Ray r = camera.GenerateRay(sample, lambda)->ray;
                printf("([0x%08x, 0x%08x, 0x%08x, 0x%08x], [0x%08x, 0x%08x, 0x%08x, "
                        "0x%08x, 0x%08x, 0x%08x]),\n",
                        Bits(pFilm.x), Bits(pFilm.y), Bits(pLens.x), Bits(pLens.y),
                        Bits(r.o.x), Bits(r.o.y), Bits(r.o.z), Bits(r.d.x), Bits(r.d.y),
                        Bits(r.d.z));
            }
        End();
    }
}

static void Sensor() {
    const PixelSensor *sensor = PixelSensor::CreateDefault();
    Begin("SENSOR_RGB", "(u32, [u32; 4], [u32; 3])",
            "pbrt-v4 `PixelSensor::CreateDefault()->ToSensorRGB(L, "
            "SampledWavelengths::SampleVisible(u))`: `(u, L, rgb)`.");
    const float Ls[][4] = {{0, 0, 0, 0}, {1, 1, 1, 1}, {0.5f, 2.f, 0.25f, 3.f}};
    for (float u : {0.f, 0.3f, 0.5f, 0.97f})
        for (const float *l : Ls) {
            SampledSpectrum L(pstd::span<const Float>(l, 4));
            SampledWavelengths lambda = SampledWavelengths::SampleVisible(u);
            RGB rgb = sensor->ToSensorRGB(L, lambda);
            printf("(0x%08x, [0x%08x, 0x%08x, 0x%08x, 0x%08x], [0x%08x, 0x%08x, 0x%08x]),\n",
                    Bits(u), Bits(l[0]), Bits(l[1]), Bits(l[2]), Bits(l[3]), Bits(rgb.r),
                    Bits(rgb.g), Bits(rgb.b));
        }
    End();
}

static void CameraSamples() {
    Begin("CAMERA_SAMPLE", "(i32, i32, u32, [u32; 6])",
            "pbrt-v4 `GetCameraSample(sampler, pPixel, filter)` with `HaltonSampler(16, "
            "Point2i(64, 32), PermuteDigits, 0)` after `StartPixelSample(pPixel, sampleIndex)` "
            "and the default `GaussianFilter`: `(x, y, sampleIndex, [pFilm, time, pLens, "
            "filterWeight])`.");
    Filter filter = GaussianFilter::Create(ParameterDictionary(), nullptr, {});
    HaltonSampler sampler(16, Point2i(64, 32), RandomizeStrategy::PermuteDigits, 0);
    for (Point2i p : {Point2i(0, 0), Point2i(7, 3), Point2i(63, 31)})
        for (int sampleIndex : {0, 1, 15}) {
            sampler.StartPixelSample(p, sampleIndex, 0);
            CameraSample cs = GetCameraSample(Sampler(&sampler), p, filter);
            printf("(%d, %d, %d, [0x%08x, 0x%08x, 0x%08x, 0x%08x, 0x%08x, 0x%08x]),\n", p.x,
                    p.y, sampleIndex, Bits(cs.pFilm.x), Bits(cs.pFilm.y), Bits(cs.time),
                    Bits(cs.pLens.x), Bits(cs.pLens.y), Bits(cs.filterWeight));
        }
    End();
}

int main() {
    PBRTOptions options;
    options.quiet = true;
    InitPBRT(options);
    printf("//! Generated by `known_values.cpp` from pbrt-v4 at the pinned commit; do not "
            "edit.\n//! Floats are their bit patterns.\n\n#![expect(\n    "
            "clippy::unreadable_literal,\n    reason = \"generated tables of bit "
            "patterns\"\n)]\n\n");
    Gaussians();
    PerspectiveRays();
    Sensor();
    CameraSamples();
}
