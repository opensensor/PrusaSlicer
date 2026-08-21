///|/ Copyright (c) Prusa Research 2025
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_MultiMaterialAutoColorization_hpp_
#define slic3r_MultiMaterialAutoColorization_hpp_

#include <vector>
#include <functional>
#include <memory>
#include <optional>

#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"

namespace Slic3r {

class ModelObject;
class ModelVolume;

enum class MMUAutoColorizationPattern : int {
    // Gradients
    HeightGradient,
    LinearGradient,
    RadialGradient,
    SphericalGradient,
    AngularSweep,
    SpiralPattern,
    // Repeating shapes
    ConcentricRings,
    Stripes,
    Checkerboard,
    // Procedural textures
    NoisePattern,
    Turbulence,
    Voronoi,
    MarbleGrain,
    WoodGrain,
    // Driven by the surface itself
    SlopeAngle,
    Curvature,
    PerVolume,
    PerShell,
    // Driven by an external image
    ImageProjection,
    // Tool change minimization
    OptimizedChanges,
    Count
};

// Direction used by the patterns that run along a single axis.
enum class MMUAutoColorizationAxis : int { X, Y, Z, Custom, Count };

// How the image of the image projection pattern is wrapped onto the model.
enum class MMUAutoColorizationProjection : int { Planar, Cylindrical, Spherical, Count };

// Greyscale image consumed by MMUAutoColorizationPattern::ImageProjection. Decoding is left to the
// caller, so libslic3r stays independent of any particular image format. Held through a shared
// pointer, so that passing the parameters around stays cheap.
struct MMUAutoColorizationImage
{
    int width  = 0;
    int height = 0;
    // Row major, first row is the top of the image, values in <0, 1>.
    std::vector<float> luminance;

    bool  empty() const { return this->width <= 0 || this->height <= 0 || this->luminance.empty(); }
    float sample(float u, float v) const;
};

struct MMUAutoColorizationParams {
    MMUAutoColorizationPattern pattern_type = MMUAutoColorizationPattern::HeightGradient;

    // Which extruders to use (1-based indices, 0 means not used)
    std::vector<int> extruders = {1, 2, 0, 0, 0};

    // Percentage-based distribution for each extruder (0-100)
    std::vector<float> distribution = {50.0f, 50.0f, 0.0f, 0.0f, 0.0f};

    // Shared by every pattern

    // Direction of the linear gradient, the stripes and the axis the angular patterns turn around.
    MMUAutoColorizationAxis axis        = MMUAutoColorizationAxis::Z;
    Vec3f                   custom_axis = Vec3f::UnitZ();
    // Center of the radial, spherical, angular, spiral and ring patterns. Unless a custom center is
    // requested, the center of the object bounding box is used, so the patterns stay centered on the
    // model instead of collapsing onto the object origin.
    bool                    use_custom_center = false;
    Vec3f                   custom_center     = Vec3f::Zero();
    // Fraction of a full band blended stochastically into its neighbor, 0 keeps hard edges. Filament
    // cannot be mixed, so scattering triangles across the boundary is the only way to fade colors.
    float                   dither_width      = 0.0f;
    // Flips the direction of whichever pattern is selected.
    bool                    reverse           = false;

    // Pattern-specific parameters

    // For the height and linear gradients
    float height_start_percent = 0.0f;  // Start of the gradient as percentage of the model extent
    float height_end_percent = 100.0f;  // End of the gradient as percentage of the model extent

    // For radial and spherical gradients
    float radial_radius = 50.0f;        // Radius of the gradient in mm

    // For spiral pattern
    float spiral_pitch = 10.0f;         // Distance between spiral turns in mm
    int spiral_turns = 5;               // Number of complete turns

    // For the angular sweep
    float angular_start_deg = 0.0f;     // Angle the sweep starts at

    // For the repeating patterns (rings, stripes, checkerboard)
    float period = 10.0f;               // Length of one repetition in mm

    // For noise pattern
    float noise_scale = 10.0f;          // Scale of the noise pattern
    float noise_threshold = 0.5f;       // Noise value remapped onto the middle of the distribution
    int noise_seed = 1234;              // Seed for the noise generator

    // For turbulence, marble and wood grain
    int   octaves = 4;                  // Number of noise octaves summed together
    float persistence = 0.5f;           // Amplitude falloff between octaves
    float distortion = 1.0f;            // How strongly the noise distorts the grain

    // For the voronoi pattern
    float cell_size = 10.0f;            // Average cell diameter in mm

    // For the slope pattern
    float slope_min_deg = 0.0f;         // Facet angle mapped onto the start of the distribution
    float slope_max_deg = 180.0f;       // Facet angle mapped onto the end of the distribution

    // For the curvature pattern
    float curvature_scale = 1.0f;       // Curvature mapped onto the end of the distribution

    // For the image projection
    std::shared_ptr<const MMUAutoColorizationImage> image;
    MMUAutoColorizationProjection projection = MMUAutoColorizationProjection::Planar;
    float image_scale = 1.0f;           // Size of the image relative to the model
    Vec2f image_offset = Vec2f::Zero(); // Shift of the image in its own plane, in mm
    float image_rotation_deg = 0.0f;    // Rotation of the image in its own plane
    bool  image_invert = false;         // Swap dark and light

    // For optimized changes
    float min_band_height = 1.0f;       // Bands shorter than this (in mm) are merged to save tool changes
};

// Apply automatic colorization to a model object based on the specified parameters
void apply_auto_colorization(ModelObject& model_object, const MMUAutoColorizationParams& params);

// Generate a preview of the auto-colorization without modifying the model
// Returns a vector of triangle selectors with the colorization applied
std::vector<std::unique_ptr<TriangleSelector>> preview_auto_colorization(
    const ModelObject& model_object,
    const MMUAutoColorizationParams& params);

// Helper function to validate and normalize the auto-colorization parameters
MMUAutoColorizationParams validate_auto_colorization_params(const MMUAutoColorizationParams& params);

// Helper function to assign color based on distribution (exposed for testing)
int assign_color_from_distribution(float normalized_value, const std::vector<int>& extruders, const std::vector<float>& distribution);

// Untranslated name of a pattern, in the order the patterns are declared (exposed for the GUI).
const char* auto_colorization_pattern_name(MMUAutoColorizationPattern pattern);

// True when the pattern turns around an axis or radiates from a center, and the center parameters apply.
bool auto_colorization_pattern_uses_center(MMUAutoColorizationPattern pattern);

// True when the pattern runs along an axis, and the axis parameters apply.
bool auto_colorization_pattern_uses_axis(MMUAutoColorizationPattern pattern);

} // namespace Slic3r

#endif // slic3r_MultiMaterialAutoColorization_hpp_
