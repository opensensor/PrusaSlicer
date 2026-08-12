#include <catch2/catch_test_macros.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/Format/3mf.hpp"
#include "libslic3r/Format/STL.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <limits>
#include <vector>

using namespace Slic3r;

namespace {

class ScopedTemporary3mf
{
public:
    ScopedTemporary3mf()
        : m_path(boost::filesystem::temp_directory_path() /
            boost::filesystem::unique_path("prusaslicer-security-%%%%-%%%%.3mf"))
    {}

    ~ScopedTemporary3mf()
    {
        boost::system::error_code error;
        boost::filesystem::remove(m_path, error);
    }

    const boost::filesystem::path& path() const { return m_path; }

private:
    boost::filesystem::path m_path;
};

uint16_t read_zip_uint16(const std::vector<char> &bytes, size_t offset)
{
    return uint16_t(static_cast<unsigned char>(bytes[offset])) |
        (uint16_t(static_cast<unsigned char>(bytes[offset + 1])) << 8);
}

void write_zip_uint32(std::vector<char> &bytes, size_t offset, uint32_t value)
{
    for (size_t byte = 0; byte < 4; ++ byte)
        bytes[offset + byte] = static_cast<char>((value >> (byte * 8)) & 0xff);
}

bool set_central_directory_uncompressed_size(
    const boost::filesystem::path &archive, const std::string &entry_name, uint32_t size)
{
    boost::nowide::ifstream input(archive.string(), std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    input.close();

    constexpr unsigned char central_header[] = {0x50, 0x4b, 0x01, 0x02};
    for (size_t offset = 0; offset + 46 <= bytes.size(); ++ offset) {
        if (! std::equal(std::begin(central_header), std::end(central_header), bytes.begin() + offset,
                [](unsigned char expected, char actual) { return expected == static_cast<unsigned char>(actual); }))
            continue;

        const size_t filename_size = read_zip_uint16(bytes, offset + 28);
        const size_t extra_size    = read_zip_uint16(bytes, offset + 30);
        const size_t comment_size  = read_zip_uint16(bytes, offset + 32);
        if (offset + 46 + filename_size + extra_size + comment_size > bytes.size())
            return false;

        const std::string filename(bytes.data() + offset + 46, filename_size);
        if (filename == entry_name) {
            write_zip_uint32(bytes, offset + 24, size);
            boost::nowide::ofstream output(archive.string(), std::ios::binary | std::ios::trunc);
            output.write(bytes.data(), std::streamsize(bytes.size()));
            return output.good();
        }

        offset += 45 + filename_size + extra_size + comment_size;
    }
    return false;
}

} // namespace

SCENARIO("Reading 3mf file", "[3mf]") {
    GIVEN("umlauts in the path of the file") {
        Model model;
        WHEN("3mf model is read") {
        	std::string path = std::string(TEST_DATA_DIR) + "/test_3mf/Geräte/Büchse.3mf";
        	DynamicPrintConfig config;
            ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Disable };
            boost::optional<Semver> version;
            bool ret = load_3mf(path.c_str(), config, ctxt, &model, false, version);
            THEN("load should succeed") {
                REQUIRE(ret);
            }
        }
    }
}

SCENARIO("Export+Import geometry to/from 3mf file cycle", "[3mf]") {
    GIVEN("world vertices coordinates before save") {
        // load a model from stl file
        Model src_model;
        std::string src_file = std::string(TEST_DATA_DIR) + "/test_3mf/Prusa.stl";
        load_stl(src_file.c_str(), &src_model);
        src_model.add_default_instances();

        ModelObject* src_object = src_model.objects.front();

        // apply generic transformation to the 1st volume
        Geometry::Transformation src_volume_transform;
        src_volume_transform.set_offset({ 10.0, 20.0, 0.0 });
        src_volume_transform.set_rotation({ Geometry::deg2rad(25.0), Geometry::deg2rad(35.0), Geometry::deg2rad(45.0) });
        src_volume_transform.set_scaling_factor({ 1.1, 1.2, 1.3 });
        src_volume_transform.set_mirror({ -1.0, 1.0, -1.0 });
        src_object->volumes.front()->set_transformation(src_volume_transform);

        // apply generic transformation to the 1st instance
        Geometry::Transformation src_instance_transform;
        src_instance_transform.set_offset({ 5.0, 10.0, 0.0 });
        src_instance_transform.set_rotation({ Geometry::deg2rad(12.0), Geometry::deg2rad(13.0), Geometry::deg2rad(14.0) });
        src_instance_transform.set_scaling_factor({ 0.9, 0.8, 0.7 });
        src_instance_transform.set_mirror({ 1.0, -1.0, -1.0 });
        src_object->instances.front()->set_transformation(src_instance_transform);

        WHEN("model is saved+loaded to/from 3mf file") {
            // save the model to 3mf file
            std::string test_file = std::string(TEST_DATA_DIR) + "/test_3mf/prusa.3mf";
            store_3mf(test_file.c_str(), &src_model, nullptr, false);

            // load back the model from the 3mf file
            Model dst_model;
            DynamicPrintConfig dst_config;
            {
                ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Disable };
                boost::optional<Semver> version;
                load_3mf(test_file.c_str(), dst_config, ctxt, &dst_model, false, version);
            }
            boost::filesystem::remove(test_file);

            // compare meshes
            TriangleMesh src_mesh = src_model.mesh();
            TriangleMesh dst_mesh = dst_model.mesh();

            bool res = src_mesh.its.vertices.size() == dst_mesh.its.vertices.size();
            if (res) {
                for (size_t i = 0; i < dst_mesh.its.vertices.size(); ++i) {
                    res &= dst_mesh.its.vertices[i].isApprox(src_mesh.its.vertices[i]);
                }
            }
            THEN("world vertices coordinates after load match") {
                REQUIRE(res);
            }
        }
    }
}

SCENARIO("2D convex hull of sinking object", "[3mf]") {
    GIVEN("model") {
        // load a model
        Model model;
        std::string src_file = std::string(TEST_DATA_DIR) + "/test_3mf/Prusa.stl";
        load_stl(src_file.c_str(), &model);
        model.add_default_instances();

        WHEN("model is rotated, scaled and set as sinking") {
            ModelObject* object = model.objects.front();
            object->center_around_origin(false);

            // set instance's attitude so that it is rotated, scaled and sinking
            ModelInstance* instance = object->instances.front();
            instance->set_rotation(X, -M_PI / 4.0);
            instance->set_offset(Vec3d::Zero());
            instance->set_scaling_factor({ 2.0, 2.0, 2.0 });

            // calculate 2D convex hull
            Polygon hull_2d = object->convex_hull_2d(instance->get_transformation().get_matrix());

            // verify result
            Points result = {
                { -91501496, -15914144 },
                { 91501496, -15914144 },
                { 91501496, 4243 },
                { 78229680, 4246883 },
                { 56898100, 4246883 },
                { -85501496, 4242641 },
                { -91501496, 4243 }
            };

            // Allow 1um error due to floating point rounding.
            bool res = hull_2d.points.size() == result.size();
            if (res)
                for (size_t i = 0; i < result.size(); ++ i) {
                    const Point &p1 = result[i];
                    const Point &p2 = hull_2d.points[i];
                    if (std::abs(p1.x() - p2.x()) > 1 || std::abs(p1.y() - p2.y()) > 1) {
                        res = false;
                        break;
                    }
                }

            THEN("2D convex hull should match with reference") {
                REQUIRE(res);
            }
        }
    }
}

TEST_CASE("Reject oversized 3MF model configuration before allocation", "[3mf][security]")
{
    Model source_model;
    const std::string source_file = std::string(TEST_DATA_DIR) + "/test_3mf/Prusa.stl";
    load_stl(source_file.c_str(), &source_model);
    source_model.add_default_instances();

    const ScopedTemporary3mf archive;
    DynamicPrintConfig source_config;
    REQUIRE(store_3mf(archive.path().string().c_str(), &source_model, &source_config, false, nullptr, false));

    // 0xfffffffe is representable in a ZIP32 central directory and exceeds
    // both the metadata limit and XML_GetBuffer's signed-int parameter.
    REQUIRE(set_central_directory_uncompressed_size(
        archive.path(), "Metadata/Slic3r_PE_model.config", std::numeric_limits<uint32_t>::max() - 1));

    Model loaded_model;
    DynamicPrintConfig loaded_config;
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Disable};
    boost::optional<Semver> generator_version;
    REQUIRE_FALSE(load_3mf(
        archive.path().string().c_str(), loaded_config, substitutions, &loaded_model, false, generator_version));
}
