///|/ Copyright (c) Prusa Research 2025
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "MultiMaterialAutoColorization.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <random>

#include "libslic3r/I18N.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/TriangleMesh.hpp"

namespace Slic3r {

namespace {

constexpr float ACL_EPSILON = 1e-6f;

// Perlin noise implementation for the noise pattern
class PerlinNoise {
private:
    std::vector<int> p;

public:
    PerlinNoise(int seed = 0) {
        // Initialize the permutation vector with the reference values
        p = {
            151, 160, 137, 91, 90, 15, 131, 13, 201, 95, 96, 53, 194, 233, 7, 225,
            140, 36, 103, 30, 69, 142, 8, 99, 37, 240, 21, 10, 23, 190, 6, 148,
            247, 120, 234, 75, 0, 26, 197, 62, 94, 252, 219, 203, 117, 35, 11, 32,
            57, 177, 33, 88, 237, 149, 56, 87, 174, 20, 125, 136, 171, 168, 68, 175,
            74, 165, 71, 134, 139, 48, 27, 166, 77, 146, 158, 231, 83, 111, 229, 122,
            60, 211, 133, 230, 220, 105, 92, 41, 55, 46, 245, 40, 244, 102, 143, 54,
            65, 25, 63, 161, 1, 216, 80, 73, 209, 76, 132, 187, 208, 89, 18, 169,
            200, 196, 135, 130, 116, 188, 159, 86, 164, 100, 109, 198, 173, 186, 3, 64,
            52, 217, 226, 250, 124, 123, 5, 202, 38, 147, 118, 126, 255, 82, 85, 212,
            207, 206, 59, 227, 47, 16, 58, 17, 182, 189, 28, 42, 223, 183, 170, 213,
            119, 248, 152, 2, 44, 154, 163, 70, 221, 153, 101, 155, 167, 43, 172, 9,
            129, 22, 39, 253, 19, 98, 108, 110, 79, 113, 224, 232, 178, 185, 112, 104,
            218, 246, 97, 228, 251, 34, 242, 193, 238, 210, 144, 12, 191, 179, 162, 241,
            81, 51, 145, 235, 249, 14, 239, 107, 49, 192, 214, 31, 181, 199, 106, 157,
            184, 84, 204, 176, 115, 121, 50, 45, 127, 4, 150, 254, 138, 236, 205, 93,
            222, 114, 67, 29, 24, 72, 243, 141, 128, 195, 78, 66, 215, 61, 156, 180
        };

        // Duplicate the permutation vector
        p.insert(p.end(), p.begin(), p.end());

        // If a seed is provided, shuffle the permutation vector
        if (seed > 0) {
            std::mt19937 rng(seed);
            std::shuffle(p.begin(), p.end(), rng);
        }
    }

    double noise(double x, double y, double z) const {
        // Find the unit cube that contains the point
        int X = static_cast<int>(std::floor(x)) & 255;
        int Y = static_cast<int>(std::floor(y)) & 255;
        int Z = static_cast<int>(std::floor(z)) & 255;

        // Find relative x, y, z of point in cube
        x -= std::floor(x);
        y -= std::floor(y);
        z -= std::floor(z);

        // Compute fade curves for each of x, y, z
        double u = fade(x);
        double v = fade(y);
        double w = fade(z);

        // Hash coordinates of the 8 cube corners
        int A = p[X] + Y;
        int AA = p[A] + Z;
        int AB = p[A + 1] + Z;
        int B = p[X + 1] + Y;
        int BA = p[B] + Z;
        int BB = p[B + 1] + Z;

        // Add blended results from 8 corners of cube
        double res = lerp(w, lerp(v, lerp(u, grad(p[AA], x, y, z),
                                            grad(p[BA], x-1, y, z)),
                                    lerp(u, grad(p[AB], x, y-1, z),
                                            grad(p[BB], x-1, y-1, z))),
                            lerp(v, lerp(u, grad(p[AA+1], x, y, z-1),
                                            grad(p[BA+1], x-1, y, z-1)),
                                    lerp(u, grad(p[AB+1], x, y-1, z-1),
                                            grad(p[BB+1], x-1, y-1, z-1))));
        return (res + 1.0) / 2.0;
    }

    // Sum of successively finer and weaker noise octaves, which reads as natural turbulence
    // instead of the single smooth blob a plain Perlin lookup produces.
    double fbm(double x, double y, double z, int octaves, double persistence) const {
        double total = 0.0;
        double amplitude = 1.0;
        double frequency = 1.0;
        double max_amplitude = 0.0;

        for (int octave = 0; octave < std::max(1, octaves); ++octave) {
            total += amplitude * (noise(x * frequency, y * frequency, z * frequency) * 2.0 - 1.0);
            max_amplitude += amplitude;
            amplitude *= persistence;
            frequency *= 2.0;
        }

        return max_amplitude > 0.0 ? (total / max_amplitude + 1.0) / 2.0 : 0.5;
    }

private:
    static double fade(double t) {
        return t * t * t * (t * (t * 6 - 15) + 10);
    }

    static double lerp(double t, double a, double b) {
        return a + t * (b - a);
    }

    static double grad(int hash, double x, double y, double z) {
        int h = hash & 15;
        // Convert lower 4 bits of hash into 12 gradient directions
        double u = h < 8 ? x : y,
               v = h < 4 ? y : h == 12 || h == 14 ? x : z;
        return ((h & 1) == 0 ? u : -u) + ((h & 2) == 0 ? v : -v);
    }
};

// Integer hash, so that dithering and the cellular patterns stay identical between runs and between
// the preview and the applied result.
inline uint32_t hash_u32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

inline float hash01(uint32_t x) { return float(hash_u32(x) & 0xFFFFFFU) / float(0x1000000U); }

inline uint32_t hash_cell(int cx, int cy, int cz, int seed)
{
    return hash_u32(uint32_t(cx) * 73856093U ^ uint32_t(cy) * 19349663U ^ uint32_t(cz) * 83492791U ^ uint32_t(seed));
}

// Discrete patterns spread their index evenly over the distribution, so that an even distribution
// gives exactly one color per index while an uneven one still shifts the weighting.
inline float discrete_value(int index, int count)
{
    const int n = std::max(1, count);
    const int i = ((index % n) + n) % n;
    return (float(i) + 0.5f) / float(n);
}

inline float fract(float value) { return value - std::floor(value); }

Vec3f axis_direction(const MMUAutoColorizationParams &params)
{
    switch (params.axis) {
    case MMUAutoColorizationAxis::X: return Vec3f(Vec3f::UnitX());
    case MMUAutoColorizationAxis::Y: return Vec3f(Vec3f::UnitY());
    case MMUAutoColorizationAxis::Custom: {
        const float norm = params.custom_axis.norm();
        return norm > ACL_EPSILON ? Vec3f(params.custom_axis / norm) : Vec3f(Vec3f::UnitZ());
    }
    case MMUAutoColorizationAxis::Z:
    default: return Vec3f(Vec3f::UnitZ());
    }
}

// Two unit vectors spanning the plane perpendicular to the axis, used to turn a point into an angle
// and a distance around that axis.
void axis_frame(const Vec3f &axis, Vec3f &out_u, Vec3f &out_v)
{
    const Vec3f reference = std::abs(axis.z()) < 0.9f ? Vec3f(Vec3f::UnitZ()) : Vec3f(Vec3f::UnitX());
    out_u = axis.cross(reference).normalized();
    out_v = axis.cross(out_u);
}

// Everything the patterns need to know about the object as a whole. Computed once per object, so
// that a gradient spans the whole model instead of restarting inside every volume.
struct ColorizationContext
{
    BoundingBoxf3 bbox;
    Vec3f         center     = Vec3f::Zero();
    Vec3f         axis       = Vec3f::UnitZ();
    Vec3f         axis_u     = Vec3f::UnitX();
    Vec3f         axis_v     = Vec3f::UnitY();
    float         axis_min   = 0.f;
    float         axis_max   = 0.f;
    float         plane_span = 1.f;
    int           volume_count          = 1;
    int           active_extruder_count = 1;
    PerlinNoise   noise;

    // Filled in per volume, only for the patterns that actually need them.
    std::vector<float> face_curvature;
    std::vector<int>   face_shell;
};

struct FacetSample
{
    Vec3f center = Vec3f::Zero();
    Vec3f normal = Vec3f::UnitZ();
    int   facet_idx  = 0;
    int   volume_idx = 0;
};

float gradient_window(float t, float t_min, float t_max, const MMUAutoColorizationParams &params)
{
    const float span  = t_max - t_min;
    const float start = t_min + (params.height_start_percent / 100.0f) * span;
    const float end   = t_min + (params.height_end_percent / 100.0f) * span;
    const float range = end - start;

    if (std::abs(range) < ACL_EPSILON)
        return t < start ? 0.0f : 1.0f;

    return std::clamp((t - start) / range, 0.0f, 1.0f);
}

// Shifts the median of a noise value onto the middle of the distribution, so that the threshold
// controls how much of the model each half of the distribution covers.
float apply_noise_threshold(float value, float threshold)
{
    const float t = std::clamp(threshold, ACL_EPSILON, 1.0f - ACL_EPSILON);
    return value < t ? 0.5f * value / t : 0.5f + 0.5f * (value - t) / (1.0f - t);
}

float pattern_angle(const ColorizationContext &ctx, const Vec3f &relative)
{
    const float a = relative.dot(ctx.axis_u);
    const float b = relative.dot(ctx.axis_v);
    float angle = std::atan2(b, a);
    if (angle < 0.0f)
        angle += 2.0f * float(M_PI);
    return angle;
}

float pattern_radius(const ColorizationContext &ctx, const Vec3f &relative)
{
    const float a = relative.dot(ctx.axis_u);
    const float b = relative.dot(ctx.axis_v);
    return std::sqrt(a * a + b * b);
}

// Distance to the nearest of the randomly jittered cell centers around the sample, which produces
// the irregular patches of a Voronoi diagram without building the diagram itself.
float voronoi_cell_value(const MMUAutoColorizationParams &params, const Vec3f &point)
{
    const float cell_size = std::max(0.1f, params.cell_size);
    const Vec3f scaled    = point / cell_size;
    const int   bx = int(std::floor(scaled.x()));
    const int   by = int(std::floor(scaled.y()));
    const int   bz = int(std::floor(scaled.z()));

    float    best_distance = std::numeric_limits<float>::max();
    uint32_t best_cell     = 0;

    for (int dz = -1; dz <= 1; ++dz) {
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                const int cx = bx + dx, cy = by + dy, cz = bz + dz;
                const uint32_t cell = hash_cell(cx, cy, cz, params.noise_seed);
                const Vec3f feature(float(cx) + hash01(cell), float(cy) + hash01(cell ^ 0x9e3779b9U),
                                    float(cz) + hash01(cell ^ 0x85ebca6bU));
                const float distance = (scaled - feature).squaredNorm();
                if (distance < best_distance) {
                    best_distance = distance;
                    best_cell     = cell;
                }
            }
        }
    }

    return hash01(best_cell);
}

float image_projection_value(const MMUAutoColorizationParams &params, const ColorizationContext &ctx, const Vec3f &relative)
{
    if (!params.image || params.image->empty())
        return 0.0f;

    float u = 0.0f;
    float v = 0.0f;

    switch (params.projection) {
    case MMUAutoColorizationProjection::Cylindrical: {
        const float along = relative.dot(ctx.axis) + ctx.center.dot(ctx.axis);
        const float span  = ctx.axis_max - ctx.axis_min;
        u = pattern_angle(ctx, relative) / (2.0f * float(M_PI));
        v = span > ACL_EPSILON ? 1.0f - (along - ctx.axis_min) / span : 0.5f;
        break;
    }
    case MMUAutoColorizationProjection::Spherical: {
        const float radius = relative.norm();
        u = pattern_angle(ctx, relative) / (2.0f * float(M_PI));
        v = radius > ACL_EPSILON ? std::acos(std::clamp(relative.dot(ctx.axis) / radius, -1.0f, 1.0f)) / float(M_PI) : 0.5f;
        break;
    }
    case MMUAutoColorizationProjection::Planar:
    default: {
        // Fit the image across the model, then let the user scale, rotate and shift it from there.
        const float extent = std::max(ACL_EPSILON, ctx.plane_span * std::max(0.01f, params.image_scale));
        const float angle  = -params.image_rotation_deg * float(M_PI) / 180.0f;
        const float a      = relative.dot(ctx.axis_u) - params.image_offset.x();
        const float b      = relative.dot(ctx.axis_v) - params.image_offset.y();
        const float ra     = a * std::cos(angle) - b * std::sin(angle);
        const float rb     = a * std::sin(angle) + b * std::cos(angle);
        u = ra / extent + 0.5f;
        v = 0.5f - rb / extent;
        break;
    }
    }

    const float luminance = params.image->sample(u, v);
    return params.image_invert ? 1.0f - luminance : luminance;
}

float evaluate_pattern(const MMUAutoColorizationParams &params, const ColorizationContext &ctx, const FacetSample &sample)
{
    const Vec3f relative = sample.center - ctx.center;

    switch (params.pattern_type) {
    case MMUAutoColorizationPattern::HeightGradient:
        return gradient_window(sample.center.z(), float(ctx.bbox.min.z()), float(ctx.bbox.max.z()), params);

    case MMUAutoColorizationPattern::LinearGradient:
        return gradient_window(sample.center.dot(ctx.axis), ctx.axis_min, ctx.axis_max, params);

    case MMUAutoColorizationPattern::RadialGradient:
        return std::clamp(pattern_radius(ctx, relative) / std::max(0.1f, params.radial_radius), 0.0f, 1.0f);

    case MMUAutoColorizationPattern::SphericalGradient:
        return std::clamp(relative.norm() / std::max(0.1f, params.radial_radius), 0.0f, 1.0f);

    case MMUAutoColorizationPattern::AngularSweep: {
        const float start = params.angular_start_deg * float(M_PI) / 180.0f;
        return fract((pattern_angle(ctx, relative) - start) / (2.0f * float(M_PI)) + 1.0f);
    }

    case MMUAutoColorizationPattern::SpiralPattern: {
        const float angle    = pattern_angle(ctx, relative);
        const float distance = pattern_radius(ctx, relative);
        const float turns    = float(std::max(1, params.spiral_turns));
        return fract((angle / (2.0f * float(M_PI)) + distance / std::max(0.1f, params.spiral_pitch)) / turns);
    }

    case MMUAutoColorizationPattern::ConcentricRings:
        return fract(pattern_radius(ctx, relative) / std::max(0.1f, params.period));

    case MMUAutoColorizationPattern::Stripes:
        return fract(relative.dot(ctx.axis) / std::max(0.1f, params.period));

    case MMUAutoColorizationPattern::Checkerboard: {
        const float period = std::max(0.1f, params.period);
        const int   cx = int(std::floor(relative.x() / period));
        const int   cy = int(std::floor(relative.y() / period));
        const int   cz = int(std::floor(relative.z() / period));
        return discrete_value(cx + cy + cz, ctx.active_extruder_count);
    }

    case MMUAutoColorizationPattern::NoisePattern: {
        const float scale = std::max(0.1f, params.noise_scale) / 100.0f;
        const float value = float(ctx.noise.noise(sample.center.x() * scale, sample.center.y() * scale, sample.center.z() * scale));
        return apply_noise_threshold(value, params.noise_threshold);
    }

    case MMUAutoColorizationPattern::Turbulence: {
        const float scale = std::max(0.1f, params.noise_scale) / 100.0f;
        const float value = float(ctx.noise.fbm(sample.center.x() * scale, sample.center.y() * scale,
                                                sample.center.z() * scale, params.octaves, params.persistence));
        return apply_noise_threshold(value, params.noise_threshold);
    }

    case MMUAutoColorizationPattern::Voronoi:
        return voronoi_cell_value(params, sample.center);

    case MMUAutoColorizationPattern::MarbleGrain: {
        const float scale = std::max(0.1f, params.noise_scale) / 100.0f;
        const float turbulence = float(ctx.noise.fbm(sample.center.x() * scale, sample.center.y() * scale,
                                                     sample.center.z() * scale, params.octaves, params.persistence)) - 0.5f;
        const float phase = relative.dot(ctx.axis) / std::max(0.1f, params.period) + params.distortion * turbulence;
        return 0.5f + 0.5f * std::sin(2.0f * float(M_PI) * phase);
    }

    case MMUAutoColorizationPattern::WoodGrain: {
        const float scale = std::max(0.1f, params.noise_scale) / 100.0f;
        const float turbulence = float(ctx.noise.fbm(sample.center.x() * scale, sample.center.y() * scale,
                                                     sample.center.z() * scale, params.octaves, params.persistence)) - 0.5f;
        const float rings = pattern_radius(ctx, relative) / std::max(0.1f, params.period) + params.distortion * turbulence;
        return fract(rings);
    }

    case MMUAutoColorizationPattern::SlopeAngle: {
        const float degrees = std::acos(std::clamp(sample.normal.dot(ctx.axis), -1.0f, 1.0f)) * 180.0f / float(M_PI);
        const float range   = params.slope_max_deg - params.slope_min_deg;
        return std::abs(range) < ACL_EPSILON ? 0.0f : std::clamp((degrees - params.slope_min_deg) / range, 0.0f, 1.0f);
    }

    case MMUAutoColorizationPattern::Curvature: {
        if (sample.facet_idx >= int(ctx.face_curvature.size()))
            return 0.0f;
        return std::clamp(ctx.face_curvature[sample.facet_idx] / std::max(ACL_EPSILON, params.curvature_scale), 0.0f, 1.0f);
    }

    case MMUAutoColorizationPattern::PerVolume:
        return discrete_value(sample.volume_idx, ctx.active_extruder_count);

    case MMUAutoColorizationPattern::PerShell: {
        const int shell = sample.facet_idx < int(ctx.face_shell.size()) ? ctx.face_shell[sample.facet_idx] : 0;
        return discrete_value(shell, ctx.active_extruder_count);
    }

    case MMUAutoColorizationPattern::ImageProjection:
        return image_projection_value(params, ctx, relative);

    case MMUAutoColorizationPattern::OptimizedChanges:
    default:
        // The bands are contiguous along Z, which is what keeps the number of tool changes down.
        // The short ones have already been merged away before the pattern is evaluated.
        return gradient_window(sample.center.z(), float(ctx.bbox.min.z()), float(ctx.bbox.max.z()), params);
    }
}

// Scatters the triangles that sit close to a band boundary into the neighboring band. Filament
// cannot be blended, so a stochastic mix of both colors is the only way to fade one into the other.
float apply_dither(float value, float dither_width, int facet_idx)
{
    if (dither_width <= 0.0f)
        return value;

    const float offset = (hash01(uint32_t(facet_idx) + 0x9e3779b9U) - 0.5f) * dither_width;
    return std::clamp(value + offset, 0.0f, 1.0f);
}

int count_active_extruders(const std::vector<int> &extruders)
{
    return int(std::count_if(extruders.begin(), extruders.end(), [](int extruder) { return extruder > 0; }));
}

// Union of all model part volumes, in object coordinates. A gradient has to span the whole object,
// otherwise every part of a multi-part object restarts the gradient from the beginning.
BoundingBoxf3 object_bounding_box(const ModelObject &model_object)
{
    BoundingBoxf3 bbox;
    for (const ModelVolume *volume : model_object.volumes) {
        if (!volume->is_model_part())
            continue;
        bbox.merge(volume->mesh().bounding_box().transformed(volume->get_matrix()));
    }

    if (!bbox.defined)
        bbox = BoundingBoxf3(Vec3d::Zero(), Vec3d::Zero());

    return bbox;
}

// Groups the faces of a mesh into connected shells, so that separate pieces of one volume can be
// colored apart from each other.
std::vector<int> compute_face_shells(const indexed_triangle_set &its)
{
    std::vector<int> shells(its.indices.size(), -1);
    if (its.indices.empty())
        return shells;

    const std::vector<Vec3i> neighbors = its_face_neighbors(its);
    std::vector<int>         stack;
    int                      shell_idx = 0;

    for (size_t seed = 0; seed < shells.size(); ++seed) {
        if (shells[seed] != -1)
            continue;

        stack.clear();
        stack.push_back(int(seed));
        shells[seed] = shell_idx;

        while (!stack.empty()) {
            const int face = stack.back();
            stack.pop_back();

            for (int edge = 0; edge < 3; ++edge) {
                const int neighbor = neighbors[face][edge];
                if (neighbor >= 0 && neighbor < int(shells.size()) && shells[neighbor] == -1) {
                    shells[neighbor] = shell_idx;
                    stack.push_back(neighbor);
                }
            }
        }

        ++shell_idx;
    }

    return shells;
}

// Rough per face curvature - how much the face is tilted away from the faces it touches.
std::vector<float> compute_face_curvature(const indexed_triangle_set &its, const std::vector<Vec3f> &face_normals)
{
    std::vector<float> curvature(its.indices.size(), 0.0f);
    if (its.indices.empty())
        return curvature;

    const std::vector<Vec3i> neighbors = its_face_neighbors(its);
    for (size_t face = 0; face < curvature.size(); ++face) {
        float total = 0.0f;
        int   count = 0;
        for (int edge = 0; edge < 3; ++edge) {
            const int neighbor = neighbors[face][edge];
            if (neighbor < 0 || neighbor >= int(face_normals.size()))
                continue;
            total += 1.0f - std::clamp(face_normals[face].dot(face_normals[neighbor]), -1.0f, 1.0f);
            ++count;
        }
        curvature[face] = count > 0 ? total / float(count) : 0.0f;
    }

    return curvature;
}

bool pattern_needs_normals(MMUAutoColorizationPattern pattern)
{
    return pattern == MMUAutoColorizationPattern::SlopeAngle || pattern == MMUAutoColorizationPattern::Curvature;
}

void apply_pattern(TriangleSelector &selector, const ModelVolume &volume, int volume_idx,
                   const MMUAutoColorizationParams &params, ColorizationContext &ctx)
{
    const TriangleMesh &mesh             = volume.mesh();
    const Transform3d  &volume_transform = volume.get_matrix();
    const indexed_triangle_set &its      = mesh.its;

    std::vector<Vec3f> face_normals;
    if (pattern_needs_normals(params.pattern_type)) {
        face_normals = its_face_normals(its);
        // The normals come out in mesh coordinates, the patterns work in object coordinates.
        const Matrix3d normal_matrix = volume_transform.matrix().block<3, 3>(0, 0).inverse().transpose();
        for (Vec3f &normal : face_normals)
            normal = (normal_matrix * normal.cast<double>()).cast<float>().normalized();
    }

    ctx.face_curvature.clear();
    if (params.pattern_type == MMUAutoColorizationPattern::Curvature)
        ctx.face_curvature = compute_face_curvature(its, face_normals);

    ctx.face_shell.clear();
    if (params.pattern_type == MMUAutoColorizationPattern::PerShell)
        ctx.face_shell = compute_face_shells(its);

    for (size_t i = 0; i < its.indices.size(); ++i) {
        const stl_triangle_vertex_indices &indices = its.indices[i];

        // Calculate the center of the triangle
        Vec3f center = Vec3f::Zero();
        for (int j = 0; j < 3; ++j)
            center += its.vertices[indices[j]];
        center /= 3.0f;

        FacetSample sample;
        sample.center     = (volume_transform * center.cast<double>()).cast<float>();
        sample.normal     = i < face_normals.size() ? face_normals[i] : Vec3f(Vec3f::UnitZ());
        sample.facet_idx  = int(i);
        sample.volume_idx = volume_idx;

        float value = evaluate_pattern(params, ctx, sample);
        if (params.reverse)
            value = 1.0f - value;
        value = apply_dither(value, params.dither_width, int(i));

        // Set the triangle state
        const int extruder_id = assign_color_from_distribution(value, params.extruders, params.distribution);
        if (extruder_id > 0)
            selector.set_facet(int(i), TriangleStateType(extruder_id));
    }
}

ColorizationContext make_context(const ModelObject &model_object, const MMUAutoColorizationParams &params)
{
    ColorizationContext ctx{};
    ctx.bbox   = object_bounding_box(model_object);
    ctx.center = params.use_custom_center ? params.custom_center : ctx.bbox.center().cast<float>();
    ctx.axis   = axis_direction(params);
    axis_frame(ctx.axis, ctx.axis_u, ctx.axis_v);

    // Extent of the object along the axis and across it, so the patterns can be expressed relative
    // to the model instead of relative to the print bed.
    ctx.axis_min = std::numeric_limits<float>::max();
    ctx.axis_max = std::numeric_limits<float>::lowest();
    float plane_min_u = std::numeric_limits<float>::max(), plane_max_u = std::numeric_limits<float>::lowest();
    float plane_min_v = std::numeric_limits<float>::max(), plane_max_v = std::numeric_limits<float>::lowest();

    for (int corner = 0; corner < 8; ++corner) {
        const Vec3f point(float((corner & 1) ? ctx.bbox.max.x() : ctx.bbox.min.x()),
                          float((corner & 2) ? ctx.bbox.max.y() : ctx.bbox.min.y()),
                          float((corner & 4) ? ctx.bbox.max.z() : ctx.bbox.min.z()));
        const float along = point.dot(ctx.axis);
        ctx.axis_min = std::min(ctx.axis_min, along);
        ctx.axis_max = std::max(ctx.axis_max, along);

        const Vec3f relative = point - ctx.center;
        plane_min_u = std::min(plane_min_u, relative.dot(ctx.axis_u));
        plane_max_u = std::max(plane_max_u, relative.dot(ctx.axis_u));
        plane_min_v = std::min(plane_min_v, relative.dot(ctx.axis_v));
        plane_max_v = std::max(plane_max_v, relative.dot(ctx.axis_v));
    }

    ctx.plane_span = std::max({plane_max_u - plane_min_u, plane_max_v - plane_min_v, ACL_EPSILON});

    ctx.volume_count = int(std::count_if(model_object.volumes.begin(), model_object.volumes.end(),
                                         [](const ModelVolume *volume) { return volume->is_model_part(); }));
    ctx.volume_count = std::max(1, ctx.volume_count);
    ctx.active_extruder_count = std::max(1, count_active_extruders(params.extruders));
    ctx.noise = PerlinNoise(params.noise_seed);

    return ctx;
}

// Merges the bands that would be too short to be worth a tool change into their predecessor.
void merge_short_bands(const std::vector<int> &extruders, std::vector<float> &distribution, float total_height, float min_band_height)
{
    if (min_band_height <= 0.0f || total_height <= 0.0f)
        return;

    const float total_share = std::accumulate(distribution.begin(), distribution.end(), 0.0f);
    if (total_share <= 0.0f)
        return;

    const float min_share = min_band_height / total_height * total_share;

    int previous = -1;
    for (size_t i = 0; i < extruders.size() && i < distribution.size(); ++i) {
        if (extruders[i] <= 0 || distribution[i] <= 0.0f)
            continue;

        if (distribution[i] < min_share && previous >= 0) {
            distribution[previous] += distribution[i];
            distribution[i] = 0.0f;
            continue;
        }

        previous = int(i);
    }
}

// Everything the two entry points share: validate, build the object wide context and fold away the
// bands that are too short to be worth a tool change.
ColorizationContext prepare_colorization(const ModelObject &model_object, MMUAutoColorizationParams &params)
{
    params = validate_auto_colorization_params(params);

    ColorizationContext ctx = make_context(model_object, params);
    if (params.pattern_type == MMUAutoColorizationPattern::OptimizedChanges)
        merge_short_bands(params.extruders, params.distribution, float(ctx.bbox.size().z()), params.min_band_height);

    return ctx;
}

} // namespace

float MMUAutoColorizationImage::sample(float u, float v) const
{
    if (this->empty())
        return 0.0f;

    // Clamp to the edge, so that a projection reaching past the image keeps the border color
    // instead of tiling it back over the model.
    const float x = std::clamp(u, 0.0f, 1.0f) * float(this->width - 1);
    const float y = std::clamp(v, 0.0f, 1.0f) * float(this->height - 1);

    const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
    const int x1 = std::min(x0 + 1, this->width - 1), y1 = std::min(y0 + 1, this->height - 1);
    const float fx = x - float(x0), fy = y - float(y0);

    const float top    = this->luminance[size_t(y0) * size_t(this->width) + size_t(x0)] * (1.0f - fx) +
                         this->luminance[size_t(y0) * size_t(this->width) + size_t(x1)] * fx;
    const float bottom = this->luminance[size_t(y1) * size_t(this->width) + size_t(x0)] * (1.0f - fx) +
                         this->luminance[size_t(y1) * size_t(this->width) + size_t(x1)] * fx;

    return top * (1.0f - fy) + bottom * fy;
}

// Helper function to assign a color (extruder) based on a normalized value and distribution
int assign_color_from_distribution(float normalized_value, const std::vector<int>& extruders, const std::vector<float>& distribution) {
    if (extruders.empty() || distribution.empty())
        return 0;

    // Collect the active extruders together with their cumulative share. Both vectors are built
    // in the same pass so that cumulative_dist[i] always describes active_extruders[i]. Skipping
    // slots in only one of them would shift the bands onto the wrong extruder.
    std::vector<int> active_extruders;
    std::vector<float> cumulative_dist;
    float sum = 0.0f;
    int last_weighted_extruder = 0;
    for (size_t i = 0; i < extruders.size(); ++i) {
        if (extruders[i] <= 0)
            continue;

        // A disabled extruder must not consume any share of the distribution.
        const float weight = (i < distribution.size()) ? std::max(0.0f, distribution[i]) : 0.0f;
        if (weight > 0.0f)
            last_weighted_extruder = extruders[i];

        sum += weight;
        active_extruders.push_back(extruders[i]);
        cumulative_dist.push_back(sum);
    }

    if (active_extruders.empty())
        return 0;

    if (sum <= 0.0f)
        return active_extruders.front(); // Default to the first active extruder if all shares are 0

    // Find the appropriate color based on the normalized value. Zero-width bands repeat the
    // previous cumulative value, so they can never be selected.
    normalized_value = std::clamp(normalized_value, 0.0f, 1.0f);
    for (size_t i = 0; i < cumulative_dist.size(); ++i) {
        if (normalized_value < cumulative_dist[i] / sum)
            return active_extruders[i];
    }

    // normalized_value sits exactly on the upper bound, which belongs to the last band with width.
    return last_weighted_extruder;
}

const char* auto_colorization_pattern_name(MMUAutoColorizationPattern pattern)
{
    switch (pattern) {
    case MMUAutoColorizationPattern::HeightGradient:    return L("Height gradient");
    case MMUAutoColorizationPattern::LinearGradient:    return L("Linear gradient");
    case MMUAutoColorizationPattern::RadialGradient:    return L("Radial gradient");
    case MMUAutoColorizationPattern::SphericalGradient: return L("Spherical gradient");
    case MMUAutoColorizationPattern::AngularSweep:      return L("Angular sweep");
    case MMUAutoColorizationPattern::SpiralPattern:     return L("Spiral pattern");
    case MMUAutoColorizationPattern::ConcentricRings:   return L("Concentric rings");
    case MMUAutoColorizationPattern::Stripes:           return L("Stripes");
    case MMUAutoColorizationPattern::Checkerboard:      return L("Checkerboard");
    case MMUAutoColorizationPattern::NoisePattern:      return L("Noise pattern");
    case MMUAutoColorizationPattern::Turbulence:        return L("Turbulence");
    case MMUAutoColorizationPattern::Voronoi:           return L("Voronoi cells");
    case MMUAutoColorizationPattern::MarbleGrain:       return L("Marble grain");
    case MMUAutoColorizationPattern::WoodGrain:         return L("Wood grain");
    case MMUAutoColorizationPattern::SlopeAngle:        return L("Surface slope");
    case MMUAutoColorizationPattern::Curvature:         return L("Curvature");
    case MMUAutoColorizationPattern::PerVolume:         return L("Per part");
    case MMUAutoColorizationPattern::PerShell:          return L("Per separate shell");
    case MMUAutoColorizationPattern::ImageProjection:   return L("Image projection");
    case MMUAutoColorizationPattern::OptimizedChanges:  return L("Optimized changes");
    default:                                           return L("Height gradient");
    }
}

bool auto_colorization_pattern_uses_center(MMUAutoColorizationPattern pattern)
{
    switch (pattern) {
    case MMUAutoColorizationPattern::RadialGradient:
    case MMUAutoColorizationPattern::SphericalGradient:
    case MMUAutoColorizationPattern::AngularSweep:
    case MMUAutoColorizationPattern::SpiralPattern:
    case MMUAutoColorizationPattern::ConcentricRings:
    case MMUAutoColorizationPattern::Stripes:
    case MMUAutoColorizationPattern::Checkerboard:
    case MMUAutoColorizationPattern::MarbleGrain:
    case MMUAutoColorizationPattern::WoodGrain:
    case MMUAutoColorizationPattern::ImageProjection:
        return true;
    default:
        return false;
    }
}

bool auto_colorization_pattern_uses_axis(MMUAutoColorizationPattern pattern)
{
    switch (pattern) {
    case MMUAutoColorizationPattern::LinearGradient:
    case MMUAutoColorizationPattern::AngularSweep:
    case MMUAutoColorizationPattern::SpiralPattern:
    case MMUAutoColorizationPattern::ConcentricRings:
    case MMUAutoColorizationPattern::Stripes:
    case MMUAutoColorizationPattern::RadialGradient:
    case MMUAutoColorizationPattern::MarbleGrain:
    case MMUAutoColorizationPattern::WoodGrain:
    case MMUAutoColorizationPattern::SlopeAngle:
    case MMUAutoColorizationPattern::ImageProjection:
        return true;
    default:
        return false;
    }
}

// Validate and normalize the auto-colorization parameters
MMUAutoColorizationParams validate_auto_colorization_params(const MMUAutoColorizationParams& params) {
    MMUAutoColorizationParams validated = params;

    // Ensure we have at least one active extruder
    bool has_active_extruder = false;
    for (int e : validated.extruders) {
        if (e > 0) {
            has_active_extruder = true;
            break;
        }
    }

    if (!has_active_extruder && !validated.extruders.empty()) {
        validated.extruders[0] = 1; // Set first extruder as active if none are
    }

    // Keep both vectors the same length, so a slot's share always belongs to the extruder next to it.
    validated.distribution.resize(validated.extruders.size(), 0.0f);

    // Ensure distribution values are valid. Only the active extruders take a share, otherwise a
    // disabled slot would keep consuming part of the model while painting nothing.
    int active_count = 0;
    float total_distribution = 0.0f;
    for (size_t i = 0; i < validated.extruders.size(); ++i) {
        if (validated.extruders[i] <= 0) {
            validated.distribution[i] = 0.0f;
            continue;
        }

        ++active_count;
        validated.distribution[i] = std::max(0.0f, validated.distribution[i]); // Ensure non-negative
        total_distribution += validated.distribution[i];
    }

    // Normalize distribution if needed
    if (total_distribution > 0.0f) {
        for (float& d : validated.distribution) {
            d = (d / total_distribution) * 100.0f;
        }
    } else if (active_count > 0) {
        // If all distributions are 0, set equal distribution for active extruders
        const float equal_value = 100.0f / float(active_count);
        for (size_t i = 0; i < validated.extruders.size(); ++i) {
            validated.distribution[i] = (validated.extruders[i] > 0) ? equal_value : 0.0f;
        }
    }

    // Ensure height gradient parameters are valid
    validated.height_start_percent = std::clamp(validated.height_start_percent, 0.0f, 100.0f);
    validated.height_end_percent = std::clamp(validated.height_end_percent, 0.0f, 100.0f);

    // Ensure radial gradient parameters are valid
    validated.radial_radius = std::max(0.1f, validated.radial_radius);

    // Ensure spiral pattern parameters are valid
    validated.spiral_pitch = std::max(0.1f, validated.spiral_pitch);
    validated.spiral_turns = std::max(1, validated.spiral_turns);

    // Ensure the repeating patterns have a usable period
    validated.period = std::max(0.1f, validated.period);
    validated.cell_size = std::max(0.1f, validated.cell_size);

    // Ensure noise pattern parameters are valid
    validated.noise_scale = std::max(0.1f, validated.noise_scale);
    validated.noise_threshold = std::clamp(validated.noise_threshold, 0.0f, 1.0f);
    validated.octaves = std::clamp(validated.octaves, 1, 8);
    validated.persistence = std::clamp(validated.persistence, 0.05f, 1.0f);
    validated.distortion = std::max(0.0f, validated.distortion);

    // Ensure the surface driven patterns have a usable range
    validated.slope_min_deg = std::clamp(validated.slope_min_deg, 0.0f, 180.0f);
    validated.slope_max_deg = std::clamp(validated.slope_max_deg, 0.0f, 180.0f);
    validated.curvature_scale = std::max(ACL_EPSILON, validated.curvature_scale);

    // Ensure the image projection parameters are valid
    validated.image_scale = std::max(0.01f, validated.image_scale);

    // Ensure the blending and band parameters are valid
    validated.dither_width = std::clamp(validated.dither_width, 0.0f, 1.0f);
    validated.min_band_height = std::max(0.0f, validated.min_band_height);

    // A custom axis of zero length would collapse every axis driven pattern.
    if (validated.custom_axis.norm() < ACL_EPSILON)
        validated.custom_axis = Vec3f::UnitZ();

    return validated;
}

// Apply automatic colorization to a model object
void apply_auto_colorization(ModelObject& model_object, const MMUAutoColorizationParams& params) {
    MMUAutoColorizationParams working_params = params;
    ColorizationContext ctx = prepare_colorization(model_object, working_params);

    // Process each volume in the model object
    int volume_idx = -1;
    for (ModelVolume* volume : model_object.volumes) {
        if (!volume->is_model_part())
            continue;

        ++volume_idx;

        // Create a triangle selector for this volume
        TriangleSelector selector(volume->mesh());
        apply_pattern(selector, *volume, volume_idx, working_params, ctx);

        // Apply the colorization to the volume
        volume->mm_segmentation_facets.set(selector);
    }
}

// Generate a preview of the auto-colorization without modifying the model
std::vector<std::unique_ptr<TriangleSelector>> preview_auto_colorization(
    const ModelObject& model_object,
    const MMUAutoColorizationParams& params)
{
    MMUAutoColorizationParams working_params = params;
    ColorizationContext ctx = prepare_colorization(model_object, working_params);

    // Create a vector to store the triangle selectors
    std::vector<std::unique_ptr<TriangleSelector>> result_selectors;

    // Process each volume in the model object
    int volume_idx = -1;
    for (const ModelVolume* volume : model_object.volumes) {
        if (!volume->is_model_part())
            continue;

        ++volume_idx;

        // Create a triangle selector for this volume
        auto selector = std::make_unique<TriangleSelector>(volume->mesh());
        apply_pattern(*selector, *volume, volume_idx, working_params, ctx);

        // Add the selector to the vector
        result_selectors.push_back(std::move(selector));
    }

    return result_selectors;
}

} // namespace Slic3r
