///|/ Copyright (c) Prusa Research 2019 - 2023 Oleksandra Iushchenko @YuSanka, Lukáš Matěna @lukasmatena, Enrico Turri @enricoturri1966, Filip Sykala @Jony01, Lukáš Hejl @hejllukas, David Kocík @kocikdav, Vojtěch Bubník @bubnikv
///|/ Copyright (c) 2021 Justin Schuh @jschuh
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "GLGizmoMmuSegmentation.hpp"

#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/BitmapCache.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/wxExtensions.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/OpenGLManager.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Model.hpp"
#include "slic3r/Utils/UndoRedo.hpp"

#include <cstring>
#include <locale>
#include <sstream>

#include <wx/filedlg.h>
#include <wx/image.h>
#include <wx/log.h>


#include <GL/glew.h>

namespace Slic3r::GUI {

static inline void show_notification_extruders_limit_exceeded()
{
    wxGetApp()
        .plater()
        ->get_notification_manager()
        ->push_notification(NotificationType::MmSegmentationExceededExtrudersLimit, NotificationManager::NotificationLevel::PrintInfoNotificationLevel,
                            GUI::format(_L("Your printer has more extruders than the multi-material painting gizmo supports. For this reason, only the "
                                           "first %1% extruders will be able to be used for painting."), GLGizmoMmuSegmentation::EXTRUDERS_LIMIT));
}

void GLGizmoMmuSegmentation::on_opening()
{
    const int total_extruders = wxGetApp().extruders_edited_cnt() + wxGetApp().virtual_extruders_cnt();
    if (total_extruders > int(GLGizmoMmuSegmentation::EXTRUDERS_LIMIT)) {
        show_notification_extruders_limit_exceeded();
    }
}

void GLGizmoMmuSegmentation::on_shutdown()
{
    m_parent.use_slope(false);
    m_parent.toggle_model_objects_visibility(true);
}

std::string GLGizmoMmuSegmentation::on_get_name() const
{
    return _u8L("Multimaterial painting");
}

bool GLGizmoMmuSegmentation::on_is_selectable() const
{
    return (wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology() == ptFFF
            && wxGetApp().get_mode() != comSimple && wxGetApp().extruders_edited_cnt() > 1);
}

bool GLGizmoMmuSegmentation::on_is_activable() const
{
    return GLGizmoPainterBase::on_is_activable() && wxGetApp().extruders_edited_cnt() > 1;
}

static std::vector<std::string> get_extruders_names()
{
    const size_t physical_extruders_cnt = wxGetApp().extruders_edited_cnt();
    const size_t virtual_extruders_cnt  = wxGetApp().virtual_extruders_cnt();
    const size_t total_extruders_cnt    = physical_extruders_cnt + virtual_extruders_cnt;

    std::vector<std::string> extruders_out;
    extruders_out.reserve(physical_extruders_cnt + virtual_extruders_cnt);
    for (size_t extruder_idx = 1; extruder_idx <= physical_extruders_cnt; ++extruder_idx) {
        extruders_out.emplace_back(_u8L("Extruder") + " " + std::to_string(extruder_idx));
    }

    const FullSpectrum::VirtualExtruders& virtual_extruders =
        wxGetApp().plater()->model().virtual_extruders;
    for (const FullSpectrum::VirtualExtruder& ve : virtual_extruders) {
        extruders_out.emplace_back("[V] " + _u8L("Extruder") + " " + std::to_string(ve.id));
    }

    return extruders_out;
}

static std::vector<int> get_extruder_id_for_volumes(const ModelObject &model_object)
{
    std::vector<int> extruders_idx;
    extruders_idx.reserve(model_object.volumes.size());
    for (const ModelVolume *model_volume : model_object.volumes) {
        if (!model_volume->is_model_part())
            continue;

        extruders_idx.emplace_back(model_volume->extruder_id());
    }

    return extruders_idx;
}

void GLGizmoMmuSegmentation::init_extruders_data()
{
    m_original_extruders_names     = get_extruders_names();
    m_original_extruders_colors    = wxGetApp().plater()->get_extruder_colors_from_plater_config();
    m_modified_extruders_colors    = m_original_extruders_colors;
    m_first_selected_extruder_idx  = 0;
    m_second_selected_extruder_idx = 1;

    init_auto_colorization_extruders();
}

// The auto-colorization panel offers one slot per extruder known to the painting gizmo (physical
// and virtual ones). on_init() runs only once, on the first render of the 3D canvas, so the slots
// have to be rebuilt from init_extruders_data() instead - otherwise they stay frozen at whatever
// printer happened to be selected at start-up and the panel offers the wrong extruders.
void GLGizmoMmuSegmentation::init_auto_colorization_extruders()
{
    const size_t extruders_cnt = std::min(size_t(EXTRUDERS_LIMIT), m_original_extruders_names.size());
    const size_t prev_cnt      = std::min({m_auto_colorization_params.extruders.size(),
                                           m_auto_colorization_params.distribution.size(), extruders_cnt});

    std::vector<int>   extruders(extruders_cnt, 0);
    std::vector<float> distribution(extruders_cnt, 0.f);

    // Carry over the slots the user has already configured, drop the ones that no longer exist.
    for (size_t idx = 0; idx < prev_cnt; ++idx) {
        extruders[idx]    = m_auto_colorization_params.extruders[idx] > 0 ? int(idx + 1) : 0;
        distribution[idx] = m_auto_colorization_params.distribution[idx];
    }

    // Nothing usable carried over, fall back to an even split between the first two extruders.
    if (std::none_of(extruders.begin(), extruders.end(), [](int extruder) { return extruder > 0; })) {
        const size_t default_cnt = std::min(size_t(2), extruders_cnt);
        for (size_t idx = 0; idx < default_cnt; ++idx) {
            extruders[idx]    = int(idx + 1);
            distribution[idx] = 100.f / float(default_cnt);
        }
    }

    m_auto_colorization_params.extruders    = std::move(extruders);
    m_auto_colorization_params.distribution = std::move(distribution);
}

bool GLGizmoMmuSegmentation::on_init()
{
    m_shortcut_key = WXK_CONTROL_N;

    m_desc["reset_direction"]      = _u8L("Reset direction");
    m_desc["clipping_of_view"]     = _u8L("Clipping of view") + ": ";
    m_desc["cursor_size"]          = _u8L("Brush size") + ": ";
    m_desc["cursor_type"]          = _u8L("Brush shape");
    m_desc["first_color_caption"]  = _u8L("Left mouse button") + ": ";
    m_desc["first_color"]          = _u8L("First color");
    m_desc["second_color_caption"] = _u8L("Right mouse button") + ": ";
    m_desc["second_color"]         = _u8L("Second color");
    m_desc["remove_caption"]       = _u8L("Shift + Left mouse button") + ": ";
    m_desc["remove"]               = _u8L("Remove painted color");

    m_desc["alt_caption"]          = _u8L("Alt + Mouse wheel") + ": ";
    m_desc["alt_brush"]            = _u8L("Change brush size");
    m_desc["alt_fill"]             = _u8L("Change angle");
    m_desc["alt_height_range"]     = _u8L("Change height range");

    m_desc["remove_all"]           = _u8L("Clear all");
    m_desc["circle"]               = _u8L("Circle");
    m_desc["sphere"]               = _u8L("Sphere");
    m_desc["pointer"]              = _u8L("Triangles");

    m_desc["tool_type"]            = _u8L("Tool type");
    m_desc["tool_brush"]           = _u8L("Brush");
    m_desc["tool_smart_fill"]      = _u8L("Smart fill");
    m_desc["tool_bucket_fill"]     = _u8L("Bucket fill");
    m_desc["tool_height_range"]    = _u8L("Height range");

    m_desc["smart_fill_angle"]     = _u8L("Smart fill angle");
    m_desc["bucket_fill_angle"]    = _u8L("Bucket fill angle");

    m_desc["split_triangles"]      = _u8L("Split triangles");

    m_desc["height_range_z_range"] = _u8L("Height range");

    // Auto-colorization related descriptions. The pattern names themselves come from libslic3r,
    // so that adding a pattern there is enough to make it appear in the combo box below.
    m_desc["auto_colorize"]        = _u8L("Auto-colorize");
    m_desc["pattern_type"]         = _u8L("Pattern");
    m_desc["preview"]              = _u8L("Preview");
    m_desc["apply"]                = _u8L("Apply");
    m_desc["live_preview"]         = _u8L("Live preview");
    m_desc["extruder_use"]         = _u8L("Use extruder");
    m_desc["distribution"]         = _u8L("Distribution") + " %";
    m_desc["even_distribution"]    = _u8L("Distribute evenly");
    m_desc["reverse"]              = _u8L("Reverse direction");
    m_desc["dither"]               = _u8L("Blend width");
    m_desc["axis"]                 = _u8L("Axis");
    m_desc["axis_custom"]          = _u8L("Custom axis");
    m_desc["center_custom"]        = _u8L("Custom center");
    m_desc["center"]               = _u8L("Center");
    m_desc["height_start"]         = _u8L("Start") + " %";
    m_desc["height_end"]           = _u8L("End") + " %";
    m_desc["radial_radius"]        = _u8L("Radius") + " " + _u8L("mm");
    m_desc["spiral_pitch"]         = _u8L("Pitch") + " " + _u8L("mm");
    m_desc["spiral_turns"]         = _u8L("Turns");
    m_desc["angular_start"]        = _u8L("Start angle");
    m_desc["period"]               = _u8L("Period") + " " + _u8L("mm");
    m_desc["noise_scale"]          = _u8L("Scale");
    m_desc["noise_seed"]           = _u8L("Seed");
    m_desc["noise_threshold"]      = _u8L("Threshold");
    m_desc["octaves"]              = _u8L("Octaves");
    m_desc["persistence"]          = _u8L("Persistence");
    m_desc["distortion"]           = _u8L("Distortion");
    m_desc["cell_size"]            = _u8L("Cell size") + " " + _u8L("mm");
    m_desc["slope_min"]            = _u8L("Min angle");
    m_desc["slope_max"]            = _u8L("Max angle");
    m_desc["curvature_scale"]      = _u8L("Curvature range");
    m_desc["min_band_height"]      = _u8L("Min band height") + " " + _u8L("mm");
    m_desc["image_load"]           = _u8L("Load image");
    m_desc["image_clear"]          = _u8L("Clear image");
    m_desc["image_none"]           = _u8L("No image loaded");
    m_desc["projection"]           = _u8L("Projection");
    m_desc["projection_planar"]    = _u8L("Planar");
    m_desc["projection_cylinder"]  = _u8L("Cylindrical");
    m_desc["projection_sphere"]    = _u8L("Spherical");
    m_desc["image_scale"]          = _u8L("Image scale");
    m_desc["image_rotation"]       = _u8L("Image rotation");
    m_desc["image_invert"]         = _u8L("Invert image");
    m_desc["presets"]              = _u8L("Presets");
    m_desc["preset_save"]          = _u8L("Save");
    m_desc["preset_load"]          = _u8L("Load");
    m_desc["preset_delete"]        = _u8L("Delete");
    m_desc["randomize"]            = _u8L("Randomize");

    // Also seeds the auto-colorization extruder slots.
    init_extruders_data();

    return true;
}

void GLGizmoMmuSegmentation::render_painter_gizmo()
{
    const Selection& selection = m_parent.get_selection();

    glsafe(::glEnable(GL_BLEND));
    glsafe(::glEnable(GL_DEPTH_TEST));

    render_triangles(selection);

    m_c->object_clipper()->render_cut();
    m_c->instances_hider()->render_cut();
    render_cursor();

    glsafe(::glDisable(GL_BLEND));
}

void GLGizmoMmuSegmentation::data_changed(bool is_serializing)
{
    GLGizmoPainterBase::data_changed(is_serializing);
    if (m_state != On || wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology() != ptFFF || wxGetApp().extruders_edited_cnt() <= 1)
        return;

    ModelObject* model_object = m_c->selection_info()->model_object();
    const std::vector<ColorRGBA> current_colors =
        wxGetApp().plater()->get_extruder_colors_from_plater_config();
    if (int prev_extruders_count = int(m_original_extruders_colors.size());
        prev_extruders_count != int(current_colors.size()) || current_colors != m_original_extruders_colors) {
        if (int(current_colors.size()) > int(GLGizmoMmuSegmentation::EXTRUDERS_LIMIT))
            show_notification_extruders_limit_exceeded();

        this->init_extruders_data();
        // Reinitialize triangle selectors because of change of extruder count need also change the size of GLIndexedVertexArray
        if (prev_extruders_count != int(current_colors.size()))
            this->init_model_triangle_selectors();
    } else if (model_object != nullptr && get_extruder_id_for_volumes(*model_object) != m_original_volumes_extruder_idxs) {
        this->init_model_triangle_selectors();
    }
}

void GLGizmoMmuSegmentation::render_triangles(const Selection &selection) const
{
    ClippingPlaneDataWrapper clp_data = this->get_clipping_plane_data();
    auto                    *shader   = wxGetApp().get_shader("mm_gouraud");
    if (!shader)
        return;
    shader->start_using();
    shader->set_uniform("clipping_plane", clp_data.clp_dataf);
    shader->set_uniform("z_range", clp_data.z_range);
    ScopeGuard guard([shader]() { if (shader) shader->stop_using(); });

    const ModelObject *mo      = m_c->selection_info()->model_object();
    int                mesh_id = -1;
    for (const ModelVolume *mv : mo->volumes) {
        if (!mv->is_model_part())
            continue;

        ++mesh_id;

        const Transform3d trafo_matrix = mo->instances[selection.get_instance_idx()]->get_transformation().get_matrix() * mv->get_matrix();

        const bool is_left_handed = trafo_matrix.matrix().determinant() < 0.0;
        if (is_left_handed)
            glsafe(::glFrontFace(GL_CW));

        const Camera& camera = wxGetApp().plater()->get_camera();
        const Transform3d& view_matrix = camera.get_view_matrix();
        shader->set_uniform("view_model_matrix", view_matrix * trafo_matrix);
        shader->set_uniform("projection_matrix", camera.get_projection_matrix());
        const Matrix3d view_normal_matrix = view_matrix.matrix().block(0, 0, 3, 3) * trafo_matrix.matrix().block(0, 0, 3, 3).inverse().transpose();
        shader->set_uniform("view_normal_matrix", view_normal_matrix);

        shader->set_uniform("volume_world_matrix", trafo_matrix);
        shader->set_uniform("volume_mirrored", is_left_handed);
        m_triangle_selectors[mesh_id]->render(m_imgui, trafo_matrix);

        if (is_left_handed)
            glsafe(::glFrontFace(GL_CCW));
    }
}

static void render_extruders_combo(const std::string& label,
                                   const std::vector<std::string>& extruders,
                                   const std::vector<ColorRGBA>& extruders_colors,
                                   size_t& selection_idx)
{
    assert(!extruders_colors.empty());
    assert(extruders_colors.size() == extruders_colors.size());

    size_t selection_out = selection_idx;
    // It is necessary to use BeginGroup(). Otherwise, when using SameLine() is called, then other items will be drawn inside the combobox.
    ImGui::BeginGroup();
    ImVec2 combo_pos = ImGui::GetCursorScreenPos();
    if (ImGui::BeginCombo(label.c_str(), "")) {
        for (size_t extruder_idx = 0; extruder_idx < std::min(extruders.size(), GLGizmoMmuSegmentation::EXTRUDERS_LIMIT); ++extruder_idx) {
            ImGui::PushID(int(extruder_idx));
            ImVec2 start_position = ImGui::GetCursorScreenPos();

            if (ImGui::Selectable("", extruder_idx == selection_idx))
                selection_out = extruder_idx;

            ImGui::SameLine();
            ImGuiStyle &style  = ImGui::GetStyle();
            float       height = ImGui::GetTextLineHeight();
            ImGui::GetWindowDrawList()->AddRectFilled(start_position, ImVec2(start_position.x + height + height / 2, start_position.y + height), ImGuiPSWrap::to_ImU32(extruders_colors[extruder_idx]));
            ImGui::GetWindowDrawList()->AddRect(start_position, ImVec2(start_position.x + height + height / 2, start_position.y + height), IM_COL32_BLACK);

            ImGui::SetCursorScreenPos(ImVec2(start_position.x + height + height / 2 + style.FramePadding.x, start_position.y));
            ImGui::Text("%s", extruders[extruder_idx].c_str());
            ImGui::PopID();
        }

        ImGui::EndCombo();
    }

    ImVec2      backup_pos = ImGui::GetCursorScreenPos();
    ImGuiStyle &style      = ImGui::GetStyle();

    ImGui::SetCursorScreenPos(ImVec2(combo_pos.x + style.FramePadding.x, combo_pos.y + style.FramePadding.y));
    ImVec2 p      = ImGui::GetCursorScreenPos();
    float  height = ImGui::GetTextLineHeight();

    ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + height + height / 2, p.y + height), ImGuiPSWrap::to_ImU32(extruders_colors[selection_idx]));
    ImGui::GetWindowDrawList()->AddRect(p, ImVec2(p.x + height + height / 2, p.y + height), IM_COL32_BLACK);

    ImGui::SetCursorScreenPos(ImVec2(p.x + height + height / 2 + style.FramePadding.x, p.y));
    ImGui::Text("%s", extruders[selection_out].c_str());
    ImGui::SetCursorScreenPos(backup_pos);
    ImGui::EndGroup();

    selection_idx = selection_out;
}

void GLGizmoMmuSegmentation::on_render_input_window(float x, float y, float bottom_limit)
{
    if (!m_c->selection_info()->model_object())
        return;

    // Increase the approximate height to account for the auto-colorization section
    const float approx_height = m_imgui->scaled(35.0f);
    // Position the window higher up to ensure it fits on screen
    y = std::min(y, bottom_limit - approx_height);
    // Move the window up a bit to make room for the auto-colorization options
    // but not too much to keep it visible
    y -= m_imgui->scaled(20.0f);
    ImGuiPureWrap::set_next_window_pos(x, y, ImGuiCond_Always);

    ImGuiPureWrap::begin(get_name(), ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);

    // First calculate width of all the texts that are could possibly be shown. We will decide set the dialog width based on that:
    const float clipping_slider_left     = std::max(ImGuiPureWrap::calc_text_size(m_desc.at("clipping_of_view")).x,
                                                    ImGuiPureWrap::calc_text_size(m_desc.at("reset_direction")).x) + m_imgui->scaled(1.5f);
    const float cursor_slider_left       = ImGuiPureWrap::calc_text_size(m_desc.at("cursor_size")).x + m_imgui->scaled(1.f);
    const float smart_fill_slider_left   = ImGuiPureWrap::calc_text_size(m_desc.at("smart_fill_angle")).x + m_imgui->scaled(1.f);
    const float bucket_fill_slider_left  = ImGuiPureWrap::calc_text_size(m_desc.at("bucket_fill_angle")).x + m_imgui->scaled(1.f);
    const float height_range_slider_left = ImGuiPureWrap::calc_text_size(m_desc.at("height_range_z_range")).x + m_imgui->scaled(1.f);

    const float cursor_type_radio_circle  = ImGuiPureWrap::calc_text_size(m_desc["circle"]).x + m_imgui->scaled(2.5f);
    const float cursor_type_radio_sphere  = ImGuiPureWrap::calc_text_size(m_desc["sphere"]).x + m_imgui->scaled(2.5f);
    const float cursor_type_radio_pointer = ImGuiPureWrap::calc_text_size(m_desc["pointer"]).x + m_imgui->scaled(2.5f);

    const float button_width             = ImGuiPureWrap::calc_text_size(m_desc.at("remove_all")).x + m_imgui->scaled(1.f);
    const float buttons_width            = m_imgui->scaled(0.5f);
    const float minimal_slider_width     = m_imgui->scaled(4.f);
    const float color_button_width       = m_imgui->scaled(1.75f);
    const float combo_label_width        = std::max(ImGuiPureWrap::calc_text_size(m_desc.at("first_color")).x,
                                                    ImGuiPureWrap::calc_text_size(m_desc.at("second_color")).x) + m_imgui->scaled(1.f);

    const float tool_type_radio_brush        = ImGuiPureWrap::calc_text_size(m_desc["tool_brush"]).x + m_imgui->scaled(2.5f);
    const float tool_type_radio_bucket_fill  = ImGuiPureWrap::calc_text_size(m_desc["tool_bucket_fill"]).x + m_imgui->scaled(2.5f);
    const float tool_type_radio_smart_fill   = ImGuiPureWrap::calc_text_size(m_desc["tool_smart_fill"]).x + m_imgui->scaled(2.5f);
    const float tool_type_radio_height_range = ImGuiPureWrap::calc_text_size(m_desc["tool_height_range"]).x + m_imgui->scaled(2.5f);

    const float tool_type_radio_first_line  = tool_type_radio_brush + tool_type_radio_bucket_fill + tool_type_radio_smart_fill;
    const float tool_type_radio_second_line = tool_type_radio_height_range;
    const float tool_type_radio_max_width   = std::max(tool_type_radio_first_line, tool_type_radio_second_line);

    const float split_triangles_checkbox_width = ImGuiPureWrap::calc_text_size(m_desc["split_triangles"]).x + m_imgui->scaled(2.5f);

    float caption_max = 0.f;
    for (const std::string t : {"first_color", "second_color", "remove", "alt"}) {
        caption_max = std::max(caption_max, ImGuiPureWrap::calc_text_size(m_desc[t + "_caption"]).x);
    }

    float total_text_max = 0.f;
    for (const std::string t : {"first_color", "second_color", "remove", "alt_brush", "alt_fill", "alt_height_range"}) {
        total_text_max = std::max(total_text_max, ImGuiPureWrap::calc_text_size(m_desc[t]).x);
    }

    total_text_max += caption_max + m_imgui->scaled(1.f);
    caption_max    += m_imgui->scaled(1.f);

    const float sliders_left_width = std::max({smart_fill_slider_left, bucket_fill_slider_left, cursor_slider_left, clipping_slider_left, height_range_slider_left});
    const float slider_icon_width  = ImGuiPureWrap::get_slider_icon_size().x;
    float       window_width       = minimal_slider_width + sliders_left_width + slider_icon_width;
    window_width = std::max(window_width, total_text_max);
    window_width = std::max(window_width, button_width);
    window_width = std::max(window_width, split_triangles_checkbox_width);
    window_width = std::max(window_width, cursor_type_radio_circle + cursor_type_radio_sphere + cursor_type_radio_pointer);
    window_width = std::max(window_width, tool_type_radio_max_width);
    window_width = std::max(window_width, 2.f * buttons_width + m_imgui->scaled(1.f));

    auto draw_text_with_caption = [&caption_max](const std::string &caption, const std::string& text) {
        ImGuiPureWrap::text_colored(ImGuiPureWrap::COL_ORANGE_LIGHT, caption);
        ImGui::SameLine(caption_max);
        ImGuiPureWrap::text(text);
    };

    for (const std::string t : {"first_color", "second_color", "remove"}) {
        draw_text_with_caption(m_desc.at(t + "_caption"), m_desc.at(t));
    }

    std::string alt_hint_text = (m_tool_type == ToolType::BRUSH)        ? "alt_brush" :
                                (m_tool_type == ToolType::HEIGHT_RANGE) ? "alt_height_range"
                                                                        : "alt_fill";
    draw_text_with_caption(m_desc.at("alt_caption"), m_desc.at(alt_hint_text));

    ImGui::Separator();

    ImGui::AlignTextToFramePadding();
    ImGuiPureWrap::text(m_desc.at("first_color"));
    ImGui::SameLine(combo_label_width);
    ImGui::PushItemWidth(window_width - combo_label_width - color_button_width);
    render_extruders_combo("##first_color_combo", m_original_extruders_names, m_original_extruders_colors, m_first_selected_extruder_idx);
    ImGui::SameLine();

    const ColorRGBA& select_first_color = m_modified_extruders_colors[m_first_selected_extruder_idx];
    ImVec4           first_color        = ImGuiPSWrap::to_ImVec4(select_first_color);
    const std::string first_label       = into_u8(m_desc.at("first_color")) + "##color_picker";
    if (ImGui::ColorEdit4(first_label.c_str(), (float*)&first_color, ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel,
        // TRN Means "current color"
        _u8L("Current").c_str(),
        // TRN Means "original color"
        _u8L("Original").c_str()))
        m_modified_extruders_colors[m_first_selected_extruder_idx] = ImGuiPSWrap::from_ImVec4(first_color);

    ImGui::AlignTextToFramePadding();
    ImGuiPureWrap::text(m_desc.at("second_color"));
    ImGui::SameLine(combo_label_width);
    ImGui::PushItemWidth(window_width - combo_label_width - color_button_width);
    render_extruders_combo("##second_color_combo", m_original_extruders_names, m_original_extruders_colors, m_second_selected_extruder_idx);
    ImGui::SameLine();

    const ColorRGBA& select_second_color = m_modified_extruders_colors[m_second_selected_extruder_idx];
    ImVec4           second_color        = ImGuiPSWrap::to_ImVec4(select_second_color);
    const std::string second_label       = into_u8(m_desc.at("second_color")) + "##color_picker";
    if (ImGui::ColorEdit4(second_label.c_str(), (float*)&second_color, ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel,
        _u8L("Current").c_str(), _u8L("Original").c_str()))
        m_modified_extruders_colors[m_second_selected_extruder_idx] = ImGuiPSWrap::from_ImVec4(second_color);

    const float max_tooltip_width = ImGui::GetFontSize() * 20.0f;

    ImGui::Separator();

    ImGuiPureWrap::text(m_desc.at("tool_type"));
    ImGui::NewLine();

    const float tool_type_first_line_offset = (window_width - tool_type_radio_first_line + m_imgui->scaled(1.5f)) / 2.f;
    ImGui::SameLine(tool_type_first_line_offset);
    ImGui::PushItemWidth(tool_type_radio_brush);
    if (ImGuiPureWrap::radio_button(m_desc["tool_brush"], m_tool_type == ToolType::BRUSH)) {
        m_tool_type = ToolType::BRUSH;
        for (auto &triangle_selector : m_triangle_selectors) {
            triangle_selector->seed_fill_unselect_all_triangles();
            triangle_selector->request_update_render_data();
        }
    }

    if (ImGui::IsItemHovered())
        ImGuiPureWrap::tooltip(_u8L("Paints facets according to the chosen painting brush."), max_tooltip_width);

    ImGui::SameLine(tool_type_first_line_offset + tool_type_radio_brush);
    ImGui::PushItemWidth(tool_type_radio_smart_fill);
    if (ImGuiPureWrap::radio_button(m_desc["tool_smart_fill"], m_tool_type == ToolType::SMART_FILL)) {
        m_tool_type = ToolType::SMART_FILL;
        for (auto &triangle_selector : m_triangle_selectors) {
            triangle_selector->seed_fill_unselect_all_triangles();
            triangle_selector->request_update_render_data();
        }
    }

    if (ImGui::IsItemHovered())
        ImGuiPureWrap::tooltip(_u8L("Paints neighboring facets whose relative angle is less or equal to set angle."), max_tooltip_width);

    ImGui::SameLine(tool_type_first_line_offset + tool_type_radio_brush + tool_type_radio_smart_fill);
    ImGui::PushItemWidth(tool_type_radio_bucket_fill);
    if (ImGuiPureWrap::radio_button(m_desc["tool_bucket_fill"], m_tool_type == ToolType::BUCKET_FILL)) {
        m_tool_type = ToolType::BUCKET_FILL;
        for (auto &triangle_selector : m_triangle_selectors) {
            triangle_selector->seed_fill_unselect_all_triangles();
            triangle_selector->request_update_render_data();
        }
    }

    if (ImGui::IsItemHovered())
        ImGuiPureWrap::tooltip(_u8L("Paints neighboring facets that have the same color."), max_tooltip_width);

    ImGui::NewLine();

    const float tool_type_second_line_offset = (window_width - tool_type_radio_second_line + m_imgui->scaled(1.5f)) / 2.f;
    ImGui::SameLine(tool_type_second_line_offset);
    ImGui::PushItemWidth(tool_type_radio_height_range);
    if (ImGuiPureWrap::radio_button(m_desc["tool_height_range"], m_tool_type == ToolType::HEIGHT_RANGE)) {
        m_tool_type = ToolType::HEIGHT_RANGE;
        for (auto &triangle_selector : m_triangle_selectors) {
            triangle_selector->seed_fill_unselect_all_triangles();
            triangle_selector->request_update_render_data();
        }
    }

    if (ImGui::IsItemHovered())
        ImGuiPureWrap::tooltip(_u8L("Paints facets within the chosen height range."), max_tooltip_width);

    ImGui::Separator();

    if (m_tool_type == ToolType::BRUSH) {
        ImGuiPureWrap::text(m_desc.at("cursor_type"));
        ImGui::NewLine();

        float cursor_type_offset = (window_width - cursor_type_radio_sphere - cursor_type_radio_circle - cursor_type_radio_pointer + m_imgui->scaled(1.5f)) / 2.f;
        ImGui::SameLine(cursor_type_offset);
        ImGui::PushItemWidth(cursor_type_radio_sphere);
        if (ImGuiPureWrap::radio_button(m_desc["sphere"], m_cursor_type == TriangleSelector::CursorType::SPHERE))
            m_cursor_type = TriangleSelector::CursorType::SPHERE;

        if (ImGui::IsItemHovered())
            ImGuiPureWrap::tooltip(_u8L("Paints all facets inside, regardless of their orientation."), max_tooltip_width);

        ImGui::SameLine(cursor_type_offset + cursor_type_radio_sphere);
        ImGui::PushItemWidth(cursor_type_radio_circle);

        if (ImGuiPureWrap::radio_button(m_desc["circle"], m_cursor_type == TriangleSelector::CursorType::CIRCLE))
            m_cursor_type = TriangleSelector::CursorType::CIRCLE;

        if (ImGui::IsItemHovered())
            ImGuiPureWrap::tooltip(_u8L("Ignores facets facing away from the camera."), max_tooltip_width);

        ImGui::SameLine(cursor_type_offset + cursor_type_radio_sphere + cursor_type_radio_circle);
        ImGui::PushItemWidth(cursor_type_radio_pointer);

        if (ImGuiPureWrap::radio_button(m_desc["pointer"], m_cursor_type == TriangleSelector::CursorType::POINTER))
            m_cursor_type = TriangleSelector::CursorType::POINTER;

        if (ImGui::IsItemHovered())
            ImGuiPureWrap::tooltip(_u8L("Paints only one facet."), max_tooltip_width);

        m_imgui->disabled_begin(m_cursor_type != TriangleSelector::CursorType::SPHERE && m_cursor_type != TriangleSelector::CursorType::CIRCLE);

        ImGui::AlignTextToFramePadding();
        ImGuiPureWrap::text(m_desc.at("cursor_size"));
        ImGui::SameLine(sliders_left_width);
        ImGui::PushItemWidth(window_width - sliders_left_width - slider_icon_width);
        m_imgui->slider_float("##cursor_radius", &m_cursor_radius, CursorRadiusMin, CursorRadiusMax, "%.2f", 1.0f, true, _L("Alt + Mouse wheel"));

        ImGuiPureWrap::checkbox(m_desc["split_triangles"], m_triangle_splitting_enabled);

        if (ImGui::IsItemHovered())
            ImGuiPureWrap::tooltip(_u8L("Split bigger facets into smaller ones while the object is painted."), max_tooltip_width);

        m_imgui->disabled_end();

        ImGui::Separator();
    } else if (m_tool_type == ToolType::SMART_FILL || m_tool_type == ToolType::BUCKET_FILL) {
        ImGui::AlignTextToFramePadding();
        ImGuiPureWrap::text((m_tool_type == ToolType::SMART_FILL ? m_desc["smart_fill_angle"] : m_desc["bucket_fill_angle"])  + ":");
        std::string format_str_angle = std::string("%.f") + I18N::translate_utf8("°", "Degree sign to use in the respective slider in MMU gizmo,"
                                                                                      "placed after the number with no whitespace in between.");
        ImGui::SameLine(sliders_left_width);
        ImGui::PushItemWidth(window_width - sliders_left_width - slider_icon_width);
        float &fill_angle = (m_tool_type == ToolType::SMART_FILL) ? m_smart_fill_angle : m_bucket_fill_angle;
        if (m_imgui->slider_float("##fill_angle", &fill_angle, SmartFillAngleMin, SmartFillAngleMax, format_str_angle.data(), 1.0f, true, _L("Alt + Mouse wheel"))) {
            for (auto &triangle_selector: m_triangle_selectors) {
                triangle_selector->seed_fill_unselect_all_triangles();
                triangle_selector->request_update_render_data();
            }
        }

        ImGui::Separator();
    } else if (m_tool_type == ToolType::HEIGHT_RANGE) {
        ImGui::AlignTextToFramePadding();
        ImGuiPureWrap::text(m_desc["height_range_z_range"] + ":");
        std::string format_str_angle = std::string("%.2f ") + I18N::translate_utf8("mm", "Millimeter sign to use in the respective slider in multi-material painting gizmo,"
                                                                                         "placed after the number with space in between.");
        ImGui::SameLine(sliders_left_width);
        ImGui::PushItemWidth(window_width - sliders_left_width - slider_icon_width);
        if (m_imgui->slider_float("##height_range_z_range", &m_height_range_z_range, HeightRangeZRangeMin, HeightRangeZRangeMax, format_str_angle.data(), 1.0f, true, _L("Alt + Mouse wheel"))) {
            for (auto &triangle_selector: m_triangle_selectors) {
                triangle_selector->seed_fill_unselect_all_triangles();
                triangle_selector->request_update_render_data();
            }
        }

        ImGui::Separator();
    }

    if (m_c->object_clipper()->get_position() == 0.f) {
        ImGui::AlignTextToFramePadding();
        ImGuiPureWrap::text(m_desc.at("clipping_of_view"));
    } else {
        if (ImGuiPureWrap::button(m_desc.at("reset_direction"))) {
            wxGetApp().CallAfter([this]() { m_c->object_clipper()->set_position_by_ratio(-1., false); });
        }
    }

    auto clp_dist = float(m_c->object_clipper()->get_position());
    ImGui::SameLine(sliders_left_width);
    ImGui::PushItemWidth(window_width - sliders_left_width - slider_icon_width);
    if (m_imgui->slider_float("##clp_dist", &clp_dist, 0.f, 1.f, "%.2f", 1.0f, true, from_u8(GUI::shortkey_ctrl_prefix()) + _L("Mouse wheel")))
        m_c->object_clipper()->set_position_by_ratio(clp_dist, true);

    ImGui::Separator();

    // Add auto-colorization section
    if (ImGui::CollapsingHeader(m_desc.at("auto_colorize").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        render_auto_colorization_ui(ImGui::GetContentRegionAvail().x);
    }

    ImGui::Separator();
    if (ImGuiPureWrap::button(m_desc.at("remove_all"))) {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), _L("Reset selection"),
                                      UndoRedo::SnapshotType::GizmoAction);
        ModelObject *        mo  = m_c->selection_info()->model_object();
        int                  idx = -1;
        for (ModelVolume *mv : mo->volumes)
            if (mv->is_model_part()) {
                ++idx;
                m_triangle_selectors[idx]->reset();
                m_triangle_selectors[idx]->request_update_render_data();
            }

        update_model_object();
        m_parent.set_as_dirty();
    }

    ImGuiPureWrap::end();
}

void GLGizmoMmuSegmentation::update_model_object() const
{
    bool updated = false;
    ModelObject* mo = m_c->selection_info()->model_object();
    int idx = -1;
    for (ModelVolume* mv : mo->volumes) {
        if (! mv->is_model_part())
            continue;
        ++idx;
        updated |= mv->mm_segmentation_facets.set(*m_triangle_selectors[idx]);
    }

    if (updated) {
        const ModelObjectPtrs &mos = wxGetApp().model().objects;
        wxGetApp().obj_list()->update_info_items(std::find(mos.begin(), mos.end(), mo) - mos.begin());
        m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
    }
}

void GLGizmoMmuSegmentation::init_model_triangle_selectors()
{
    const int          extruders_count = int(m_original_extruders_colors.size());
    const ModelObject *mo              = m_c->selection_info()->model_object();
    m_triangle_selectors.clear();

    // Don't continue when extruders colors are not initialized
    if(m_original_extruders_colors.empty())
        return;

    for (const ModelVolume *mv : mo->volumes) {
        if (!mv->is_model_part())
            continue;

        // This mesh does not account for the possible Z up SLA offset.
        const TriangleMesh *mesh = &mv->mesh();

        const int physical_cnt = wxGetApp().extruders_edited_cnt();
        const FullSpectrum::VirtualExtruders& virtual_extruders =
            mo->get_model()->virtual_extruders;
        const size_t extruder_idx = get_extruder_color_idx(*mv, physical_cnt, virtual_extruders);
        m_triangle_selectors.emplace_back(std::make_unique<TriangleSelectorMmGui>(*mesh, m_modified_extruders_colors, m_original_extruders_colors[extruder_idx]));
        // Reset of TriangleSelector is done inside TriangleSelectorMmGUI's constructor, so we don't need it to perform it again in deserialize().
        m_triangle_selectors.back()->deserialize(mv->mm_segmentation_facets.get_data(), false);
        m_triangle_selectors.back()->request_update_render_data();
    }
    m_original_volumes_extruder_idxs = get_extruder_id_for_volumes(*mo);
}

void GLGizmoMmuSegmentation::update_from_model_object()
{
    wxBusyCursor wait;

    // Extruder colors need to be reloaded before calling init_model_triangle_selectors to render painted triangles
    // using colors from loaded 3MF and not from printer profile in Slicer.
    if (const std::vector<ColorRGBA> current_colors = wxGetApp().plater()->get_extruder_colors_from_plater_config();
        int(m_original_extruders_colors.size()) != int(current_colors.size()) || current_colors != m_original_extruders_colors)
        this->init_extruders_data();

    this->init_model_triangle_selectors();
}

PainterGizmoType GLGizmoMmuSegmentation::get_painter_type() const
{
    return PainterGizmoType::MM_SEGMENTATION;
}

ColorRGBA GLGizmoMmuSegmentation::get_cursor_sphere_left_button_color() const
{
    ColorRGBA color = m_modified_extruders_colors[m_first_selected_extruder_idx];
    color.a(0.25f);
    return color;
}

ColorRGBA GLGizmoMmuSegmentation::get_cursor_sphere_right_button_color() const
{
    ColorRGBA color = m_modified_extruders_colors[m_second_selected_extruder_idx];
    color.a(0.25f);
    return color;
}

void TriangleSelectorMmGui::render(ImGuiWrapper* imgui, const Transform3d& matrix)
{
    if (m_update_render_data)
        update_render_data();

    auto *shader = wxGetApp().get_current_shader();
    if (!shader)
        return;

    assert(shader->get_name() == "mm_gouraud");

    for (size_t color_idx = 0; color_idx < m_gizmo_scene.triangle_indices.size(); ++color_idx) {
        if (m_gizmo_scene.has_VBOs(color_idx)) {
            if (color_idx > m_colors.size()) // Seed fill VBO
                shader->set_uniform("uniform_color", TriangleSelectorGUI::get_seed_fill_color(color_idx == (m_colors.size() + 1) ? m_default_volume_color : m_colors[color_idx - (m_colors.size() + 1) - 1]));
            else                             // Normal VBO
                shader->set_uniform("uniform_color", color_idx == 0 ? m_default_volume_color : m_colors[color_idx - 1]);

            m_gizmo_scene.render(color_idx);
        }
    }

    render_paint_contour(matrix);
    m_update_render_data = false;
}

void TriangleSelectorMmGui::update_render_data()
{
    m_gizmo_scene.release_geometry();
    m_vertices.reserve(m_vertices.size() * 3);
    for (const Vertex &vr : m_vertices) {
        m_gizmo_scene.vertices.emplace_back(vr.v.x());
        m_gizmo_scene.vertices.emplace_back(vr.v.y());
        m_gizmo_scene.vertices.emplace_back(vr.v.z());
    }
    m_gizmo_scene.finalize_vertices();

    for (const Triangle &tr : m_triangles)
        if (tr.valid() && !tr.is_split()) {
            int               color = int(tr.get_state()) <= int(m_colors.size()) ? int(tr.get_state()) : 0;
            assert(m_colors.size() + 1 + color < m_gizmo_scene.triangle_indices.size());
            std::vector<int> &iva   = m_gizmo_scene.triangle_indices[color + tr.is_selected_by_seed_fill() * (m_colors.size() + 1)];

            if (iva.size() + 3 > iva.capacity())
                iva.reserve(next_highest_power_of_2(iva.size() + 3));

            iva.emplace_back(tr.verts_idxs[0]);
            iva.emplace_back(tr.verts_idxs[1]);
            iva.emplace_back(tr.verts_idxs[2]);
        }

    for (size_t color_idx = 0; color_idx < m_gizmo_scene.triangle_indices.size(); ++color_idx)
        m_gizmo_scene.triangle_indices_sizes[color_idx] = m_gizmo_scene.triangle_indices[color_idx].size();

    m_gizmo_scene.finalize_triangle_indices();
    update_paint_contour();
}

wxString GLGizmoMmuSegmentation::handle_snapshot_action_name(bool shift_down, GLGizmoPainterBase::Button button_down) const
{
    wxString action_name;
    if (shift_down)
        action_name = _L("Remove painted color");
    else {
        size_t extruder_id = (button_down == Button::Left ? m_first_selected_extruder_idx : m_second_selected_extruder_idx) + 1;
        action_name        = GUI::format(_L("Painted using: Extruder %1%"), extruder_id);
    }
    return action_name;
}

void GLMmSegmentationGizmo3DScene::release_geometry() {
    if (this->vertices_VBO_id) {
        glsafe(::glDeleteBuffers(1, &this->vertices_VBO_id));
        this->vertices_VBO_id = 0;
    }
    for(auto &triangle_indices_VBO_id : triangle_indices_VBO_ids) {
        glsafe(::glDeleteBuffers(1, &triangle_indices_VBO_id));
        triangle_indices_VBO_id = 0;
    }
#if !SLIC3R_OPENGL_ES
    if (OpenGLManager::get_gl_info().is_core_profile()) {
#endif // !SLIC3R_OPENGL_ES
        if (this->vertices_VAO_id > 0) {
            glsafe(::glDeleteVertexArrays(1, &this->vertices_VAO_id));
            this->vertices_VAO_id = 0;
        }
#if !SLIC3R_OPENGL_ES
    }
#endif // !SLIC3R_OPENGL_ES

    this->clear();
}

void GLMmSegmentationGizmo3DScene::render(size_t triangle_indices_idx) const
{
    assert(triangle_indices_idx < this->triangle_indices_VBO_ids.size());
    assert(this->triangle_indices_sizes.size() == this->triangle_indices_VBO_ids.size());
#if !SLIC3R_OPENGL_ES
    if (OpenGLManager::get_gl_info().is_core_profile()) {
#endif // !SLIC3R_OPENGL_ES
        assert(this->vertices_VAO_id != 0);
#if !SLIC3R_OPENGL_ES
    }
#endif // !SLIC3R_OPENGL_ES
    assert(this->vertices_VBO_id != 0);
    assert(this->triangle_indices_VBO_ids[triangle_indices_idx] != 0);

    GLShaderProgram* shader = wxGetApp().get_current_shader();
    if (shader == nullptr)
        return;

#if !SLIC3R_OPENGL_ES
    if (OpenGLManager::get_gl_info().is_core_profile()) {
#endif // !SLIC3R_OPENGL_ES
        glsafe(::glBindVertexArray(this->vertices_VAO_id));
#if !SLIC3R_OPENGL_ES
    }
#endif // !SLIC3R_OPENGL_ES
    // the following binding is needed to set the vertex attributes
    glsafe(::glBindBuffer(GL_ARRAY_BUFFER, this->vertices_VBO_id));
    const GLint position_id = shader->get_attrib_location("v_position");
    if (position_id != -1) {
        glsafe(::glVertexAttribPointer(position_id, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (GLvoid*)nullptr));
        glsafe(::glEnableVertexAttribArray(position_id));
    }

    // Render using the Vertex Buffer Objects.
    if (this->triangle_indices_VBO_ids[triangle_indices_idx] != 0 &&
        this->triangle_indices_sizes[triangle_indices_idx] > 0) {
        glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, this->triangle_indices_VBO_ids[triangle_indices_idx]));
        glsafe(::glDrawElements(GL_TRIANGLES, GLsizei(this->triangle_indices_sizes[triangle_indices_idx]), GL_UNSIGNED_INT, nullptr));
        glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0));
    }

    if (position_id != -1)
        glsafe(::glDisableVertexAttribArray(position_id));

    glsafe(::glBindBuffer(GL_ARRAY_BUFFER, 0));
#if !SLIC3R_OPENGL_ES
    if (OpenGLManager::get_gl_info().is_core_profile()) {
#endif // !SLIC3R_OPENGL_ES
        glsafe(::glBindVertexArray(0));
#if !SLIC3R_OPENGL_ES
    }
#endif // !SLIC3R_OPENGL_ES
}

void GLMmSegmentationGizmo3DScene::finalize_vertices()
{
#if !SLIC3R_OPENGL_ES
    if (OpenGLManager::get_gl_info().is_core_profile()) {
#endif // !SLIC3R_OPENGL_ES
        assert(this->vertices_VAO_id == 0);
#if !SLIC3R_OPENGL_ES
    }
#endif // !SLIC3R_OPENGL_ES
    assert(this->vertices_VBO_id == 0);
    if (!this->vertices.empty()) {
#if !SLIC3R_OPENGL_ES
        if (OpenGLManager::get_gl_info().is_core_profile()) {
#endif // !SLIC3R_OPENGL_ES
            glsafe(::glGenVertexArrays(1, &this->vertices_VAO_id));
            glsafe(::glBindVertexArray(this->vertices_VAO_id));
#if !SLIC3R_OPENGL_ES
        }
#endif // !SLIC3R_OPENGL_ES

        glsafe(::glGenBuffers(1, &this->vertices_VBO_id));
        glsafe(::glBindBuffer(GL_ARRAY_BUFFER, this->vertices_VBO_id));
        glsafe(::glBufferData(GL_ARRAY_BUFFER, this->vertices.size() * sizeof(float), this->vertices.data(), GL_STATIC_DRAW));
        glsafe(::glBindBuffer(GL_ARRAY_BUFFER, 0));
        this->vertices.clear();

#if !SLIC3R_OPENGL_ES
        if (OpenGLManager::get_gl_info().is_core_profile()) {
#endif // !SLIC3R_OPENGL_ES
            glsafe(::glBindVertexArray(0));
#if !SLIC3R_OPENGL_ES
        }
#endif // !SLIC3R_OPENGL_ES
    }
}

void GLMmSegmentationGizmo3DScene::finalize_triangle_indices()
{
    assert(std::all_of(triangle_indices_VBO_ids.cbegin(), triangle_indices_VBO_ids.cend(), [](const auto &ti_VBO_id) { return ti_VBO_id == 0; }));

    assert(this->triangle_indices.size() == this->triangle_indices_VBO_ids.size());
    for (size_t buffer_idx = 0; buffer_idx < this->triangle_indices.size(); ++buffer_idx) {
        if (!this->triangle_indices[buffer_idx].empty()) {
            glsafe(::glGenBuffers(1, &this->triangle_indices_VBO_ids[buffer_idx]));
            glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, this->triangle_indices_VBO_ids[buffer_idx]));
            glsafe(::glBufferData(GL_ELEMENT_ARRAY_BUFFER, this->triangle_indices[buffer_idx].size() * sizeof(int), this->triangle_indices[buffer_idx].data(), GL_STATIC_DRAW));
            glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0));
            this->triangle_indices[buffer_idx].clear();
        }
    }
}

// Preview auto-colorization without applying it
void GLGizmoMmuSegmentation::preview_auto_colorization()
{
    ModelObject* mo = m_c->selection_info()->model_object();
    if (!mo)
        return;

    // No undo snapshot here on purpose. The preview only touches the gizmo's own selectors, the
    // model is left alone until Apply, and snapshotting every live preview would flood undo/redo.

    // Generate a preview of the auto-colorization
    auto preview_selectors = Slic3r::preview_auto_colorization(*mo, m_auto_colorization_params);

    // Apply the preview to the triangle selectors
    int idx = -1;
    for (ModelVolume* mv : mo->volumes) {
        if (!mv->is_model_part())
            continue;

        ++idx;
        if (idx < int(preview_selectors.size())) {
            // Copy the preview selector data to the actual selector
            // Unlike in init_model_triangle_selectors(), these selectors already hold state from
            // painting or from an earlier preview, so they have to be reset before deserializing.
            m_triangle_selectors[idx]->deserialize(preview_selectors[idx]->serialize(), true);
            m_triangle_selectors[idx]->request_update_render_data();
        }
    }

    // Mark the parent as dirty to trigger a redraw
    m_parent.set_as_dirty();
}

// Apply auto-colorization to the model
void GLGizmoMmuSegmentation::apply_auto_colorization()
{
    ModelObject* mo = m_c->selection_info()->model_object();
    if (!mo)
        return;

    // Take a snapshot for undo/redo
    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _L("Apply auto-colorization"),
                                  UndoRedo::SnapshotType::GizmoAction);

    // Apply the auto-colorization to the model object
    Slic3r::apply_auto_colorization(*mo, m_auto_colorization_params);

    // Update the triangle selectors from the model
    update_from_model_object();

    // Mark the parent as dirty to trigger a redraw and schedule background processing
    m_parent.set_as_dirty();
    m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
}

// The pattern list is built from libslic3r, so that adding a pattern there is enough to make it
// selectable here and the combo indices can never drift out of sync with the enum.
static std::vector<std::string> auto_colorization_pattern_labels()
{
    std::vector<std::string> labels;
    labels.reserve(size_t(MMUAutoColorizationPattern::Count));
    for (int pattern = 0; pattern < int(MMUAutoColorizationPattern::Count); ++pattern)
        labels.emplace_back(_u8L(auto_colorization_pattern_name(static_cast<MMUAutoColorizationPattern>(pattern))));

    return labels;
}

static const constexpr char *AUTO_COLORIZATION_PRESET_SECTION = "mmu_auto_colorization_presets";

static std::string serialize_auto_colorization_params(const MMUAutoColorizationParams &params, const std::string &image_path)
{
    std::ostringstream out;
    // The presets end up in the application config, which is read back on any machine locale.
    out.imbue(std::locale::classic());

    out << "pattern=" << int(params.pattern_type);
    out << ";axis=" << int(params.axis);
    out << ";custom_axis=" << params.custom_axis.x() << "," << params.custom_axis.y() << "," << params.custom_axis.z();
    out << ";use_custom_center=" << (params.use_custom_center ? 1 : 0);
    out << ";custom_center=" << params.custom_center.x() << "," << params.custom_center.y() << "," << params.custom_center.z();
    out << ";dither=" << params.dither_width;
    out << ";reverse=" << (params.reverse ? 1 : 0);
    out << ";height_start=" << params.height_start_percent;
    out << ";height_end=" << params.height_end_percent;
    out << ";radius=" << params.radial_radius;
    out << ";spiral_pitch=" << params.spiral_pitch;
    out << ";spiral_turns=" << params.spiral_turns;
    out << ";angular_start=" << params.angular_start_deg;
    out << ";period=" << params.period;
    out << ";noise_scale=" << params.noise_scale;
    out << ";noise_threshold=" << params.noise_threshold;
    out << ";noise_seed=" << params.noise_seed;
    out << ";octaves=" << params.octaves;
    out << ";persistence=" << params.persistence;
    out << ";distortion=" << params.distortion;
    out << ";cell_size=" << params.cell_size;
    out << ";slope_min=" << params.slope_min_deg;
    out << ";slope_max=" << params.slope_max_deg;
    out << ";curvature=" << params.curvature_scale;
    out << ";projection=" << int(params.projection);
    out << ";image_scale=" << params.image_scale;
    out << ";image_rotation=" << params.image_rotation_deg;
    out << ";image_invert=" << (params.image_invert ? 1 : 0);
    out << ";min_band_height=" << params.min_band_height;

    out << ";extruders=";
    for (size_t i = 0; i < params.extruders.size(); ++i)
        out << (i == 0 ? "" : ",") << params.extruders[i];

    out << ";distribution=";
    for (size_t i = 0; i < params.distribution.size(); ++i)
        out << (i == 0 ? "" : ",") << params.distribution[i];

    // The image itself is not stored, only where it came from.
    out << ";image_path=" << image_path;

    return out.str();
}

static void deserialize_auto_colorization_params(const std::string &data, MMUAutoColorizationParams &params, std::string &image_path)
{
    const auto to_float = [](const std::string &value, float fallback) {
        std::istringstream in(value);
        in.imbue(std::locale::classic());
        float parsed = 0.f;
        return (in >> parsed) ? parsed : fallback;
    };
    const auto to_int = [](const std::string &value, int fallback) {
        std::istringstream in(value);
        in.imbue(std::locale::classic());
        int parsed = 0;
        return (in >> parsed) ? parsed : fallback;
    };
    const auto split = [](const std::string &value) {
        std::vector<std::string> parts;
        std::istringstream in(value);
        std::string item;
        while (std::getline(in, item, ','))
            parts.push_back(item);
        return parts;
    };

    std::istringstream entries(data);
    std::string entry;
    while (std::getline(entries, entry, ';')) {
        const size_t separator = entry.find('=');
        if (separator == std::string::npos)
            continue;

        const std::string key   = entry.substr(0, separator);
        const std::string value = entry.substr(separator + 1);

        if (key == "pattern") {
            const int pattern = to_int(value, 0);
            if (pattern >= 0 && pattern < int(MMUAutoColorizationPattern::Count))
                params.pattern_type = static_cast<MMUAutoColorizationPattern>(pattern);
        } else if (key == "axis") {
            const int axis = to_int(value, int(MMUAutoColorizationAxis::Z));
            if (axis >= 0 && axis < int(MMUAutoColorizationAxis::Count))
                params.axis = static_cast<MMUAutoColorizationAxis>(axis);
        } else if (key == "projection") {
            const int projection = to_int(value, int(MMUAutoColorizationProjection::Planar));
            if (projection >= 0 && projection < int(MMUAutoColorizationProjection::Count))
                params.projection = static_cast<MMUAutoColorizationProjection>(projection);
        } else if (key == "custom_axis" || key == "custom_center") {
            const std::vector<std::string> parts = split(value);
            if (parts.size() == 3) {
                const Vec3f vector(to_float(parts[0], 0.f), to_float(parts[1], 0.f), to_float(parts[2], 0.f));
                if (key == "custom_axis")
                    params.custom_axis = vector;
                else
                    params.custom_center = vector;
            }
        } else if (key == "extruders") {
            params.extruders.clear();
            for (const std::string &part : split(value))
                params.extruders.push_back(to_int(part, 0));
        } else if (key == "distribution") {
            params.distribution.clear();
            for (const std::string &part : split(value))
                params.distribution.push_back(to_float(part, 0.f));
        } else if (key == "use_custom_center") {
            params.use_custom_center = to_int(value, 0) != 0;
        } else if (key == "reverse") {
            params.reverse = to_int(value, 0) != 0;
        } else if (key == "image_invert") {
            params.image_invert = to_int(value, 0) != 0;
        } else if (key == "dither") {
            params.dither_width = to_float(value, params.dither_width);
        } else if (key == "height_start") {
            params.height_start_percent = to_float(value, params.height_start_percent);
        } else if (key == "height_end") {
            params.height_end_percent = to_float(value, params.height_end_percent);
        } else if (key == "radius") {
            params.radial_radius = to_float(value, params.radial_radius);
        } else if (key == "spiral_pitch") {
            params.spiral_pitch = to_float(value, params.spiral_pitch);
        } else if (key == "spiral_turns") {
            params.spiral_turns = to_int(value, params.spiral_turns);
        } else if (key == "angular_start") {
            params.angular_start_deg = to_float(value, params.angular_start_deg);
        } else if (key == "period") {
            params.period = to_float(value, params.period);
        } else if (key == "noise_scale") {
            params.noise_scale = to_float(value, params.noise_scale);
        } else if (key == "noise_threshold") {
            params.noise_threshold = to_float(value, params.noise_threshold);
        } else if (key == "noise_seed") {
            params.noise_seed = to_int(value, params.noise_seed);
        } else if (key == "octaves") {
            params.octaves = to_int(value, params.octaves);
        } else if (key == "persistence") {
            params.persistence = to_float(value, params.persistence);
        } else if (key == "distortion") {
            params.distortion = to_float(value, params.distortion);
        } else if (key == "cell_size") {
            params.cell_size = to_float(value, params.cell_size);
        } else if (key == "slope_min") {
            params.slope_min_deg = to_float(value, params.slope_min_deg);
        } else if (key == "slope_max") {
            params.slope_max_deg = to_float(value, params.slope_max_deg);
        } else if (key == "curvature") {
            params.curvature_scale = to_float(value, params.curvature_scale);
        } else if (key == "image_scale") {
            params.image_scale = to_float(value, params.image_scale);
        } else if (key == "image_rotation") {
            params.image_rotation_deg = to_float(value, params.image_rotation_deg);
        } else if (key == "min_band_height") {
            params.min_band_height = to_float(value, params.min_band_height);
        } else if (key == "image_path") {
            image_path = value;
        }
    }
}

bool GLGizmoMmuSegmentation::load_auto_colorization_image(const std::string &path)
{
    if (path.empty())
        return false;

    wxImage image;
    {
        // A broken or unsupported file should not pop a wx error dialog in the middle of the gizmo.
        wxLogNull no_log;
        if (!image.LoadFile(from_u8(path)))
            return false;
    }

    if (!image.IsOk() || image.GetWidth() <= 0 || image.GetHeight() <= 0)
        return false;

    // The pattern only samples a luminance value per triangle, so a huge photo buys nothing but
    // memory. Scale anything oversized down to a sane working resolution.
    const int max_dimension = 1024;
    if (image.GetWidth() > max_dimension || image.GetHeight() > max_dimension) {
        const float ratio = float(max_dimension) / float(std::max(image.GetWidth(), image.GetHeight()));
        image = image.Scale(std::max(1, int(float(image.GetWidth()) * ratio)),
                            std::max(1, int(float(image.GetHeight()) * ratio)), wxIMAGE_QUALITY_HIGH);
    }

    auto decoded    = std::make_shared<MMUAutoColorizationImage>();
    decoded->width  = image.GetWidth();
    decoded->height = image.GetHeight();
    decoded->luminance.resize(size_t(decoded->width) * size_t(decoded->height));

    const unsigned char *rgb = image.GetData();
    for (size_t pixel = 0; pixel < decoded->luminance.size(); ++pixel) {
        const float r = float(rgb[pixel * 3 + 0]) / 255.f;
        const float g = float(rgb[pixel * 3 + 1]) / 255.f;
        const float b = float(rgb[pixel * 3 + 2]) / 255.f;
        decoded->luminance[pixel] = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    }

    m_auto_colorization_params.image = decoded;
    m_auto_colorization_image_path   = path;
    return true;
}

std::vector<std::string> GLGizmoMmuSegmentation::auto_colorization_preset_names() const
{
    std::vector<std::string> names;
    const AppConfig *config = wxGetApp().app_config;
    if (config == nullptr || !config->has_section(AUTO_COLORIZATION_PRESET_SECTION))
        return names;

    for (const auto &[name, value] : config->get_section(AUTO_COLORIZATION_PRESET_SECTION))
        names.push_back(name);

    return names;
}

void GLGizmoMmuSegmentation::save_auto_colorization_preset(const std::string &name)
{
    AppConfig *config = wxGetApp().app_config;
    if (config == nullptr || name.empty())
        return;

    config->set(AUTO_COLORIZATION_PRESET_SECTION, name,
                serialize_auto_colorization_params(m_auto_colorization_params, m_auto_colorization_image_path));
}

void GLGizmoMmuSegmentation::load_auto_colorization_preset(const std::string &name)
{
    const AppConfig *config = wxGetApp().app_config;
    if (config == nullptr || name.empty() || !config->has(AUTO_COLORIZATION_PRESET_SECTION, name))
        return;

    std::string image_path;
    deserialize_auto_colorization_params(config->get(AUTO_COLORIZATION_PRESET_SECTION, name),
                                         m_auto_colorization_params, image_path);

    // The extruder slots of the preset were saved for whichever printer was active back then, so
    // they have to be folded back onto the extruders this printer actually has.
    init_auto_colorization_extruders();

    m_auto_colorization_params.image.reset();
    m_auto_colorization_image_path.clear();
    if (!image_path.empty())
        load_auto_colorization_image(image_path);

    m_auto_colorization_dirty = true;
}

void GLGizmoMmuSegmentation::delete_auto_colorization_preset(const std::string &name)
{
    AppConfig *config = wxGetApp().app_config;
    if (config != nullptr && !name.empty())
        config->erase(AUTO_COLORIZATION_PRESET_SECTION, name);
}

void GLGizmoMmuSegmentation::render_auto_colorization_extruders(float window_width)
{
    const float swatch  = ImGui::GetFrameHeight();
    const size_t slots  = m_auto_colorization_params.extruders.size();

    // Setups with many extruders would otherwise grow the panel until Preview and Apply fall off
    // the bottom of the screen, so the list scrolls once it gets long.
    const size_t max_visible_rows = 6;
    const bool   scrolling        = slots > max_visible_rows;
    if (scrolling)
        ImGui::BeginChild("##acl_extruders", ImVec2(0.f, ImGui::GetFrameHeightWithSpacing() * float(max_visible_rows)), false);

    for (size_t i = 0; i < slots && i < m_auto_colorization_params.distribution.size(); ++i) {
        ImGui::PushID(int(i));

        if (i < m_modified_extruders_colors.size()) {
            const ColorRGBA &color = m_modified_extruders_colors[i];
            ImGui::ColorButton("##acl_color", ImVec4(color.r(), color.g(), color.b(), 1.f),
                               ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop, ImVec2(swatch, swatch));
            ImGui::SameLine();
        }

        // Checkbox to enable/disable this extruder
        bool extruder_enabled = m_auto_colorization_params.extruders[i] > 0;
        const std::string label = (i < m_original_extruders_names.size()) ? m_original_extruders_names[i]
                                                                         : m_desc.at("extruder_use") + " " + std::to_string(i + 1);
        if (ImGuiPureWrap::checkbox(label + "##acl_use", extruder_enabled)) {
            m_auto_colorization_params.extruders[i] = extruder_enabled ? int(i + 1) : 0;
            m_auto_colorization_dirty = true;
        }

        // Distribution slider (only show if extruder is enabled)
        if (extruder_enabled) {
            ImGui::SameLine(window_width * 0.5f);
            ImGui::PushItemWidth(window_width * 0.4f);
            float distribution = m_auto_colorization_params.distribution[i];
            if (m_imgui->slider_float("##acl_distribution", &distribution, 0.0f, 100.0f, "%.1f%%")) {
                m_auto_colorization_params.distribution[i] = distribution;
                m_auto_colorization_dirty = true;
            }
        }

        ImGui::PopID();
    }

    if (scrolling)
        ImGui::EndChild();

    if (ImGuiPureWrap::button(m_desc.at("even_distribution"))) {
        // Zero shares make validate_auto_colorization_params() fall back to an even split.
        std::fill(m_auto_colorization_params.distribution.begin(), m_auto_colorization_params.distribution.end(), 0.f);
        m_auto_colorization_params = validate_auto_colorization_params(m_auto_colorization_params);
        m_auto_colorization_dirty = true;
    }
}

void GLGizmoMmuSegmentation::render_auto_colorization_pattern_params(float window_width)
{
    const float label_width  = window_width * 0.45f;
    const float widget_width = window_width * 0.45f;

    const auto slider = [this, label_width, widget_width](const std::string &label, const char *id, float *value,
                                                          float min_value, float max_value, const char *format) {
        ImGui::AlignTextToFramePadding();
        ImGuiPureWrap::text(label);
        ImGui::SameLine(label_width);
        ImGui::PushItemWidth(widget_width);
        if (m_imgui->slider_float(id, value, min_value, max_value, format))
            m_auto_colorization_dirty = true;
    };
    const auto slider_int = [this, label_width, widget_width](const std::string &label, const char *id, int *value,
                                                              int min_value, int max_value) {
        ImGui::AlignTextToFramePadding();
        ImGuiPureWrap::text(label);
        ImGui::SameLine(label_width);
        ImGui::PushItemWidth(widget_width);
        if (ImGui::SliderInt(id, value, min_value, max_value))
            m_auto_colorization_dirty = true;
    };

    MMUAutoColorizationParams &params = m_auto_colorization_params;

    // Controls shared by whole families of patterns.
    if (auto_colorization_pattern_uses_axis(params.pattern_type)) {
        ImGui::AlignTextToFramePadding();
        ImGuiPureWrap::text(m_desc.at("axis"));
        ImGui::SameLine(label_width);
        ImGui::PushItemWidth(widget_width);
        const char *axis_items[] = {"X", "Y", "Z", nullptr};
        const std::string custom = _u8L("Custom");
        axis_items[3] = custom.c_str();
        int axis = int(params.axis);
        if (ImGui::Combo("##acl_axis", &axis, axis_items, IM_ARRAYSIZE(axis_items))) {
            params.axis = static_cast<MMUAutoColorizationAxis>(axis);
            m_auto_colorization_dirty = true;
        }

        if (params.axis == MMUAutoColorizationAxis::Custom) {
            ImGui::AlignTextToFramePadding();
            ImGuiPureWrap::text(m_desc.at("axis_custom"));
            ImGui::SameLine(label_width);
            ImGui::PushItemWidth(widget_width);
            if (ImGui::DragFloat3("##acl_axis_custom", params.custom_axis.data(), 0.01f, -1.f, 1.f, "%.2f"))
                m_auto_colorization_dirty = true;
        }
    }

    if (auto_colorization_pattern_uses_center(params.pattern_type)) {
        bool use_custom_center = params.use_custom_center;
        if (ImGuiPureWrap::checkbox(m_desc.at("center_custom") + "##acl_center_custom", use_custom_center)) {
            params.use_custom_center = use_custom_center;
            m_auto_colorization_dirty = true;
        }

        if (params.use_custom_center) {
            ImGui::AlignTextToFramePadding();
            ImGuiPureWrap::text(m_desc.at("center"));
            ImGui::SameLine(label_width);
            ImGui::PushItemWidth(widget_width);
            if (ImGui::DragFloat3("##acl_center", params.custom_center.data(), 0.5f, -1000.f, 1000.f, "%.1f"))
                m_auto_colorization_dirty = true;
        }
    }

    // Controls specific to the selected pattern.
    switch (params.pattern_type) {
    case MMUAutoColorizationPattern::HeightGradient:
    case MMUAutoColorizationPattern::LinearGradient:
        slider(m_desc.at("height_start"), "##acl_height_start", &params.height_start_percent, 0.f, 100.f, "%.1f%%");
        slider(m_desc.at("height_end"), "##acl_height_end", &params.height_end_percent, 0.f, 100.f, "%.1f%%");
        break;

    case MMUAutoColorizationPattern::RadialGradient:
    case MMUAutoColorizationPattern::SphericalGradient:
        slider(m_desc.at("radial_radius"), "##acl_radius", &params.radial_radius, 1.f, 200.f, "%.1f");
        break;

    case MMUAutoColorizationPattern::AngularSweep:
        slider(m_desc.at("angular_start"), "##acl_angular_start", &params.angular_start_deg, 0.f, 360.f, "%.0f");
        break;

    case MMUAutoColorizationPattern::SpiralPattern:
        slider(m_desc.at("spiral_pitch"), "##acl_spiral_pitch", &params.spiral_pitch, 1.f, 50.f, "%.1f");
        slider_int(m_desc.at("spiral_turns"), "##acl_spiral_turns", &params.spiral_turns, 1, 20);
        break;

    case MMUAutoColorizationPattern::ConcentricRings:
    case MMUAutoColorizationPattern::Stripes:
    case MMUAutoColorizationPattern::Checkerboard:
        slider(m_desc.at("period"), "##acl_period", &params.period, 0.5f, 100.f, "%.1f");
        break;

    case MMUAutoColorizationPattern::NoisePattern:
        slider(m_desc.at("noise_scale"), "##acl_noise_scale", &params.noise_scale, 1.f, 50.f, "%.1f");
        slider(m_desc.at("noise_threshold"), "##acl_noise_threshold", &params.noise_threshold, 0.f, 1.f, "%.2f");
        slider_int(m_desc.at("noise_seed"), "##acl_noise_seed", &params.noise_seed, 1, 10000);
        break;

    case MMUAutoColorizationPattern::Turbulence:
        slider(m_desc.at("noise_scale"), "##acl_noise_scale", &params.noise_scale, 1.f, 50.f, "%.1f");
        slider(m_desc.at("noise_threshold"), "##acl_noise_threshold", &params.noise_threshold, 0.f, 1.f, "%.2f");
        slider_int(m_desc.at("octaves"), "##acl_octaves", &params.octaves, 1, 8);
        slider(m_desc.at("persistence"), "##acl_persistence", &params.persistence, 0.05f, 1.f, "%.2f");
        slider_int(m_desc.at("noise_seed"), "##acl_noise_seed", &params.noise_seed, 1, 10000);
        break;

    case MMUAutoColorizationPattern::Voronoi:
        slider(m_desc.at("cell_size"), "##acl_cell_size", &params.cell_size, 1.f, 100.f, "%.1f");
        slider_int(m_desc.at("noise_seed"), "##acl_noise_seed", &params.noise_seed, 1, 10000);
        break;

    case MMUAutoColorizationPattern::MarbleGrain:
    case MMUAutoColorizationPattern::WoodGrain:
        slider(m_desc.at("period"), "##acl_period", &params.period, 0.5f, 100.f, "%.1f");
        slider(m_desc.at("noise_scale"), "##acl_noise_scale", &params.noise_scale, 1.f, 50.f, "%.1f");
        slider(m_desc.at("distortion"), "##acl_distortion", &params.distortion, 0.f, 5.f, "%.2f");
        slider_int(m_desc.at("octaves"), "##acl_octaves", &params.octaves, 1, 8);
        slider_int(m_desc.at("noise_seed"), "##acl_noise_seed", &params.noise_seed, 1, 10000);
        break;

    case MMUAutoColorizationPattern::SlopeAngle:
        slider(m_desc.at("slope_min"), "##acl_slope_min", &params.slope_min_deg, 0.f, 180.f, "%.0f");
        slider(m_desc.at("slope_max"), "##acl_slope_max", &params.slope_max_deg, 0.f, 180.f, "%.0f");
        break;

    case MMUAutoColorizationPattern::Curvature:
        slider(m_desc.at("curvature_scale"), "##acl_curvature", &params.curvature_scale, 0.01f, 2.f, "%.2f");
        break;

    case MMUAutoColorizationPattern::ImageProjection: {
        const size_t name_start = m_auto_colorization_image_path.find_last_of("/\\");
        ImGuiPureWrap::text(m_auto_colorization_image_path.empty()
                                ? m_desc.at("image_none")
                                : m_auto_colorization_image_path.substr(name_start == std::string::npos ? 0 : name_start + 1));

        if (ImGuiPureWrap::button(m_desc.at("image_load"))) {
            wxFileDialog dialog(wxGetApp().plater(), _L("Select an image"), "", "",
                                _L("Image files") + " (*.png;*.jpg;*.jpeg;*.bmp)|*.png;*.PNG;*.jpg;*.JPG;*.jpeg;*.JPEG;*.bmp;*.BMP",
                                wxFD_OPEN | wxFD_FILE_MUST_EXIST);
            if (dialog.ShowModal() == wxID_OK && load_auto_colorization_image(into_u8(dialog.GetPath())))
                m_auto_colorization_dirty = true;
        }

        if (m_auto_colorization_params.image) {
            ImGui::SameLine();
            if (ImGuiPureWrap::button(m_desc.at("image_clear"))) {
                m_auto_colorization_params.image.reset();
                m_auto_colorization_image_path.clear();
                m_auto_colorization_dirty = true;
            }
        }

        ImGui::AlignTextToFramePadding();
        ImGuiPureWrap::text(m_desc.at("projection"));
        ImGui::SameLine(label_width);
        ImGui::PushItemWidth(widget_width);
        const std::string planar   = m_desc.at("projection_planar");
        const std::string cylinder = m_desc.at("projection_cylinder");
        const std::string sphere   = m_desc.at("projection_sphere");
        const char *projection_items[] = {planar.c_str(), cylinder.c_str(), sphere.c_str()};
        int projection = int(params.projection);
        if (ImGui::Combo("##acl_projection", &projection, projection_items, IM_ARRAYSIZE(projection_items))) {
            params.projection = static_cast<MMUAutoColorizationProjection>(projection);
            m_auto_colorization_dirty = true;
        }

        if (params.projection == MMUAutoColorizationProjection::Planar) {
            slider(m_desc.at("image_scale"), "##acl_image_scale", &params.image_scale, 0.1f, 5.f, "%.2f");
            slider(m_desc.at("image_rotation"), "##acl_image_rotation", &params.image_rotation_deg, 0.f, 360.f, "%.0f");
        }

        bool invert = params.image_invert;
        if (ImGuiPureWrap::checkbox(m_desc.at("image_invert") + "##acl_image_invert", invert)) {
            params.image_invert = invert;
            m_auto_colorization_dirty = true;
        }
        break;
    }

    case MMUAutoColorizationPattern::OptimizedChanges:
        ImGuiPureWrap::text(_u8L("Keeps every color in one contiguous band to minimize tool changes."));
        slider(m_desc.at("min_band_height"), "##acl_min_band", &params.min_band_height, 0.f, 20.f, "%.1f");
        break;

    default:
        break;
    }
}

void GLGizmoMmuSegmentation::render_auto_colorization_presets(float window_width)
{
    const std::vector<std::string> presets = auto_colorization_preset_names();

    char name_buffer[64];
    std::strncpy(name_buffer, m_auto_colorization_preset_name.c_str(), sizeof(name_buffer) - 1);
    name_buffer[sizeof(name_buffer) - 1] = '\0';

    ImGui::PushItemWidth(window_width * 0.45f);
    if (ImGui::InputText("##acl_preset_name", name_buffer, sizeof(name_buffer)))
        m_auto_colorization_preset_name = name_buffer;

    ImGui::SameLine();
    if (ImGuiPureWrap::button(m_desc.at("preset_save")) && !m_auto_colorization_preset_name.empty())
        save_auto_colorization_preset(m_auto_colorization_preset_name);

    if (presets.empty())
        return;

    std::vector<const char *> preset_items;
    preset_items.reserve(presets.size());
    for (const std::string &preset : presets)
        preset_items.push_back(preset.c_str());

    // The selection follows the stored name, so it survives a preset being deleted.
    int selected_preset = 0;
    if (const auto it = std::find(presets.begin(), presets.end(), m_auto_colorization_preset_name); it != presets.end())
        selected_preset = int(it - presets.begin());

    ImGui::PushItemWidth(window_width * 0.45f);
    if (ImGui::Combo("##acl_presets", &selected_preset, preset_items.data(), int(preset_items.size())))
        m_auto_colorization_preset_name = presets[selected_preset];

    ImGui::SameLine();
    if (ImGuiPureWrap::button(m_desc.at("preset_load"))) {
        m_auto_colorization_preset_name = presets[selected_preset];
        load_auto_colorization_preset(m_auto_colorization_preset_name);
    }

    ImGui::SameLine();
    if (ImGuiPureWrap::button(m_desc.at("preset_delete")))
        delete_auto_colorization_preset(presets[selected_preset]);
}

// Render the auto-colorization UI section
void GLGizmoMmuSegmentation::render_auto_colorization_ui(float window_width)
{
    const float label_width  = window_width * 0.45f;
    const float widget_width = window_width * 0.45f;

    // Pattern type selection
    ImGui::AlignTextToFramePadding();
    ImGuiPureWrap::text(m_desc.at("pattern_type"));
    ImGui::SameLine(label_width);
    ImGui::PushItemWidth(widget_width);

    const std::vector<std::string> pattern_labels = auto_colorization_pattern_labels();
    std::vector<const char *>      pattern_items;
    pattern_items.reserve(pattern_labels.size());
    for (const std::string &label : pattern_labels)
        pattern_items.push_back(label.c_str());

    int current_pattern = int(m_auto_colorization_params.pattern_type);
    if (ImGui::Combo("##acl_pattern", &current_pattern, pattern_items.data(), int(pattern_items.size()))) {
        m_auto_colorization_params.pattern_type = static_cast<MMUAutoColorizationPattern>(current_pattern);
        m_auto_colorization_dirty = true;
    }

    ImGui::Separator();

    render_auto_colorization_extruders(window_width);

    ImGui::Separator();

    render_auto_colorization_pattern_params(window_width);

    // Direction and blending apply to every pattern.
    bool reverse = m_auto_colorization_params.reverse;
    if (ImGuiPureWrap::checkbox(m_desc.at("reverse") + "##acl_reverse", reverse)) {
        m_auto_colorization_params.reverse = reverse;
        m_auto_colorization_dirty = true;
    }

    ImGui::AlignTextToFramePadding();
    ImGuiPureWrap::text(m_desc.at("dither"));
    ImGui::SameLine(label_width);
    ImGui::PushItemWidth(widget_width);
    if (m_imgui->slider_float("##acl_dither", &m_auto_colorization_params.dither_width, 0.f, 1.f, "%.2f"))
        m_auto_colorization_dirty = true;

    ImGui::Separator();

    render_auto_colorization_presets(window_width);

    ImGui::Separator();

    bool live_preview = m_auto_colorization_live_preview;
    if (ImGuiPureWrap::checkbox(m_desc.at("live_preview") + "##acl_live", live_preview)) {
        m_auto_colorization_live_preview = live_preview;
        m_auto_colorization_dirty = live_preview;
    }

    // Preview and Apply buttons
    if (ImGuiPureWrap::button(m_desc.at("preview"), window_width * 0.45f, 0.f)) {
        m_auto_colorization_dirty = false;
        preview_auto_colorization();
    }

    ImGui::SameLine();

    if (ImGuiPureWrap::button(m_desc.at("apply"), window_width * 0.45f, 0.f)) {
        m_auto_colorization_dirty = false;
        apply_auto_colorization();
    }

    // Recompute once the control the user is dragging settles, so that live preview stays
    // responsive on large meshes instead of re-colorizing on every frame of a slider drag.
    if (m_auto_colorization_live_preview && m_auto_colorization_dirty && !ImGui::IsAnyItemActive()) {
        m_auto_colorization_dirty = false;
        preview_auto_colorization();
    }
}
} // namespace Slic3r
