#include "QDSBoxSync.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <set>

#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <nlohmann/json.hpp>

#include "libslic3r/ProjectTask.hpp"
#include "libslic3r/QidiBoxFilament.hpp"
#include "DevDefs.h"

namespace Slic3r::GUI::QDSBoxSync {

namespace {

std::optional<std::string> non_empty(const std::string &value)
{
    return value.empty() ? std::nullopt : std::optional<std::string>{value};
}

BoxSlotSnapshot normalize_slot(const RawSlot &raw, const std::string &box_id)
{
    BoxSlotSnapshot slot;
    slot.slot_index         = raw.slot_index;
    slot.occupied           = raw.occupied;
    slot.vendor_index       = raw.vendor_index;
    slot.filament_index     = raw.filament_index;
    slot.material_name      = non_empty(raw.material_name);
    slot.material_type      = non_empty(raw.material_type);
    slot.filament_preset_id = make_filament_preset_id(box_id, raw.vendor_index, raw.filament_index);
    slot.colour             = normalize_colour(raw.colour);
    if (raw.remaining_percent && *raw.remaining_percent >= 0 && *raw.remaining_percent <= 100)
        slot.remaining_percent = raw.remaining_percent;
    return slot;
}

void assign_mapping_target(FilamentInfo &mapping, const BoxSlotSnapshot &slot,
                           std::optional<int> external_virtual_id = std::nullopt)
{
    if (external_virtual_id) {
        mapping.tray_id = *external_virtual_id;
        mapping.ams_id  = std::to_string(*external_virtual_id);
        mapping.slot_id = std::to_string(*external_virtual_id);
    } else {
        mapping.tray_id = slot.slot_index;
        mapping.ams_id  = std::to_string(slot.slot_index / slots_per_box + 1);
        mapping.slot_id = std::to_string(slot.slot_index);
    }
    mapping.filament_id    = *slot.filament_preset_id;
    mapping.type           = slot.material_type.value_or("");
    mapping.color          = slot.colour.value_or("#CECECE").substr(1) + "FF";
    mapping.distance       = 0.0f;
    mapping.mapping_result = MAPPING_RESULT_DEFAULT;
    mapping.ctype          = 0;
    mapping.colors.clear();
}

const BoxSlotSnapshot *find_snapshot_slot(const BoxSnapshot &snapshot, int slot_index)
{
    if (slot_index == external_spool_slot)
        return snapshot.external_spool ? &*snapshot.external_spool : nullptr;

    const auto slot = std::find_if(snapshot.slots.begin(), snapshot.slots.end(),
                                   [slot_index](const BoxSlotSnapshot &candidate) {
                                       return candidate.slot_index == slot_index;
                                   });
    return slot == snapshot.slots.end() ? nullptr : &*slot;
}

bool mapping_target_matches(const FilamentInfo &mapping, const BoxSlotSnapshot &slot)
{
    if (!slot.filament_preset_id || mapping.filament_id != *slot.filament_preset_id)
        return false;

    const std::string expected_colour = slot.colour.value_or("#CECECE").substr(1) + "FF";
    return mapping.type == slot.material_type.value_or("") && mapping.color == expected_colour;
}

std::optional<std::size_t> parse_catalog_index(const std::string &value, std::size_t catalog_size)
{
    try {
        std::size_t parsed = 0;
        const long index = std::stol(value, &parsed);
        if (parsed == value.size() && index >= 0 && static_cast<std::size_t>(index) < catalog_size)
            return static_cast<std::size_t>(index);
    } catch (const std::exception &) {
    }
    return std::nullopt;
}

int optional_temperature(const boost::property_tree::ptree &section, const char *key)
{
    return section.get_optional<int>(key).value_or(0);
}

} // namespace

std::string normalize_model_name(const std::string &model)
{
    std::string normalized;
    normalized.reserve(model.size());
    for (const unsigned char ch : model) {
        if (std::isalnum(ch))
            normalized.push_back(static_cast<char>(std::tolower(ch)));
    }

    // Discovery services may prepend QIDI's vendor name. Strip only these
    // explicit prefixes; the remaining model still has to match exactly.
    if (normalized.rfind("qiditech", 0) == 0)
        normalized.erase(0, 8);
    else if (normalized.rfind("qidi", 0) == 0)
        normalized.erase(0, 4);
    return normalized;
}

std::string mapping_device_identity(const std::string &host, const std::string &runtime_id)
{
    const auto normalize = [](const std::string &value) {
        const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) { return std::isspace(ch); });
        const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) { return std::isspace(ch); }).base();
        if (first >= last)
            return std::string{};

        std::string identity(first, last);
        std::transform(identity.begin(), identity.end(), identity.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return identity;
    };

    std::string identity = normalize(host);
    return identity.empty() ? normalize(runtime_id) : identity;
}

std::string mapping_storage_key(const std::string &printer_profile)
{
    return printer_profile.empty() ? std::string{} : "qds_box:" + printer_profile;
}

bool valid_catalog_index(int index, std::size_t catalog_size)
{
    return index >= 0 && static_cast<std::size_t>(index) < catalog_size;
}

bool qidi_filament_ids_compatible(const std::string &project_preset_id, const std::string &slot_preset_id)
{
    return QidiBoxFilament::preset_ids_compatible(project_preset_id, slot_preset_id);
}

bool filament_selection_compatible(const std::string &project_material,
                                   const std::string &project_preset_id,
                                   const std::string &slot_material,
                                   const std::string &slot_preset_id,
                                   bool enforce_material)
{
    if (!qidi_filament_ids_compatible(project_preset_id, slot_preset_id))
        return false;
    const auto slot_identity = QidiBoxFilament::classify(slot_preset_id);
    if (!enforce_material && slot_identity.kind != QidiBoxFilament::IdentityKind::Generic)
        return true;
    return QidiBoxFilament::material_families_compatible(project_material, slot_material);
}

bool prefer_filament_match(bool candidate_exact, double candidate_colour_distance,
                           bool current_exact, double current_colour_distance)
{
    if (candidate_exact != current_exact)
        return candidate_exact;
    return candidate_colour_distance < current_colour_distance;
}

std::optional<std::string> normalize_colour(const std::optional<std::string> &colour)
{
    if (!colour || (colour->size() != 7 && colour->size() != 9) || colour->front() != '#')
        return std::nullopt;

    std::string normalized = *colour;
    for (size_t i = 1; i < normalized.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(normalized[i]);
        if (!std::isxdigit(ch))
            return std::nullopt;
        normalized[i] = static_cast<char>(std::toupper(ch));
    }
    normalized.resize(7);
    return normalized;
}

std::optional<std::string> make_filament_preset_id(const std::string &box_id, int vendor_index, int filament_index)
{
    if (box_id.empty() || vendor_index < 0 || filament_index <= 0)
        return std::nullopt;
    return "QD_" + box_id + "_" + std::to_string(vendor_index) + "_" + std::to_string(filament_index);
}

FilamentCatalogResult parse_packaged_filament_catalog(std::istream &input, std::size_t catalog_size)
{
    FilamentCatalogResult result;
    result.entries.resize(catalog_size);
    if (catalog_size == 0) {
        result.diagnostics.emplace_back("packaged filament catalog has no addressable entries");
        return result;
    }

    boost::property_tree::ptree tree;
    try {
        boost::property_tree::ini_parser::read_ini(input, tree);
    } catch (const std::exception &error) {
        result.diagnostics.emplace_back(std::string{"packaged filament catalog could not be parsed: "} + error.what());
        return result;
    }

    std::size_t material_count = 0;
    std::size_t colour_count = 0;
    std::size_t vendor_count = 0;
    for (const auto &section : tree) {
        if (section.first == "colordict" || section.first == "vendor_list") {
            for (const auto &item : section.second) {
                const auto index = parse_catalog_index(item.first, result.entries.size());
                if (!index) {
                    result.diagnostics.emplace_back("ignored out-of-range " + section.first + " index " + item.first);
                    continue;
                }

                if (section.first == "colordict") {
                    const auto colour = normalize_colour(item.second.get_value<std::string>());
                    if (!colour) {
                        result.diagnostics.emplace_back("ignored invalid colour at index " + item.first);
                        continue;
                    }
                    result.entries[*index].colour = *colour;
                    ++colour_count;
                } else {
                    const std::string vendor = item.second.get_value<std::string>();
                    if (vendor.empty()) {
                        result.diagnostics.emplace_back("ignored empty vendor at index " + item.first);
                        continue;
                    }
                    result.entries[*index].vendor = vendor;
                    ++vendor_count;
                }
            }
            continue;
        }

        if (section.first.rfind("fila", 0) != 0)
            continue;
        const auto index = parse_catalog_index(section.first.substr(4), result.entries.size());
        if (!index) {
            result.diagnostics.emplace_back("ignored out-of-range filament section " + section.first);
            continue;
        }

        const std::string name = section.second.get<std::string>("filament", "");
        const std::string type = section.second.get<std::string>("type", "");
        if (name.empty() && type.empty())
            continue;
        if (name.empty() || type.empty()) {
            result.diagnostics.emplace_back("ignored incomplete filament section " + section.first);
            continue;
        }

        auto &entry = result.entries[*index];
        entry.name = name;
        entry.type = type;
        entry.min_temperature = optional_temperature(section.second, "min_temp");
        entry.max_temperature = optional_temperature(section.second, "max_temp");
        entry.box_min_temperature = optional_temperature(section.second, "box_min_temp");
        entry.box_max_temperature = optional_temperature(section.second, "box_max_temp");
        ++material_count;
    }

    result.usable = material_count > 0 && colour_count > 0 && vendor_count > 0;
    if (!result.usable)
        result.diagnostics.emplace_back("packaged filament catalog omitted material, colour, or vendor data");
    return result;
}

FilamentCatalogResult load_packaged_filament_catalog(const std::string &path, std::size_t catalog_size)
{
    std::ifstream input(path);
    if (!input) {
        FilamentCatalogResult result;
        result.diagnostics.emplace_back("packaged filament catalog could not be opened");
        return result;
    }
    return parse_packaged_filament_catalog(input, catalog_size);
}

CompatibilityResult resolve_compatibility(const PrinterMetadata &metadata,
                                          const std::string &selected_model,
                                          double selected_nozzle,
                                          double nozzle_tolerance)
{
    CompatibilityResult result;
    const std::string selected = normalize_model_name(selected_model);
    if (selected.empty() || !std::isfinite(selected_nozzle) || selected_nozzle <= 0.0 ||
        !std::isfinite(nozzle_tolerance) || nozzle_tolerance < 0.0) {
        result.reason = "selected printer profile is incomplete";
        return result;
    }
    // The Plus 4 firmware currently omits model and nozzle metadata from its
    // Box snapshot.  In that case the selected Plus 4 machine profile is the
    // only available source of nozzle information.  Accept each nozzle size
    // that QIDI ships a Plus 4 profile for, while continuing to reject an
    // unsupported diameter or any contradictory connected metadata below.
    static constexpr std::array<double, 4> supported_plus4_nozzles{0.2, 0.4, 0.6, 0.8};
    const bool supported_fallback = selected == "xplus4" &&
        std::any_of(supported_plus4_nozzles.begin(), supported_plus4_nozzles.end(),
                    [selected_nozzle, nozzle_tolerance](double nozzle) {
                        return std::fabs(selected_nozzle - nozzle) <= nozzle_tolerance;
                    });

    const std::string configured = metadata.configured_model ?
        normalize_model_name(*metadata.configured_model) : std::string{};
    if (!configured.empty() && configured != selected) {
        result.reason = "configured physical printer contradicts the selected profile";
        return result;
    }

    bool has_reported_model = false;
    for (const std::string &reported_model : metadata.reported_models) {
        const std::string reported = normalize_model_name(reported_model);
        if (reported.empty())
            continue;
        has_reported_model = true;
        if (reported != selected) {
            result.reason = "reported printer model contradicts the selected profile";
            return result;
        }
    }
    if (!has_reported_model && !supported_fallback) {
        result.reason = "missing model metadata fallback requires a supported X-Plus 4 nozzle profile";
        return result;
    }

    result.effective_model = selected_model;
    result.used_selected_model_fallback = !has_reported_model;

    if (!metadata.reported_nozzles) {
        if (!supported_fallback) {
            result.reason = "missing nozzle metadata fallback requires a supported X-Plus 4 nozzle profile";
            return result;
        }
        result.effective_nozzle = selected_nozzle;
        result.used_selected_nozzle_fallback = true;
    } else {
        if (metadata.reported_nozzles->empty()) {
            result.reason = "reported nozzle metadata is present but invalid";
            return result;
        }
        const auto match = std::find_if(metadata.reported_nozzles->begin(), metadata.reported_nozzles->end(),
                                        [selected_nozzle, nozzle_tolerance](double nozzle) {
                                            return std::fabs(nozzle - selected_nozzle) <= nozzle_tolerance;
                                        });
        if (match == metadata.reported_nozzles->end()) {
            result.reason = "reported nozzle diameter contradicts the selected profile";
            return result;
        }
        result.effective_nozzle = *match;
    }

    result.compatible = true;
    if (result.used_selected_model_fallback || result.used_selected_nozzle_fallback)
        result.reason = "selected printer profile supplied missing connected metadata";
    else
        result.reason = "connected printer metadata matches the selected profile";
    return result;
}

BoxSnapshot normalize_snapshot(const BoxSnapshotInput &input)
{
    BoxSnapshot snapshot;
    snapshot.box_count = std::clamp(input.box_count, 0, max_box_count);
    if (snapshot.box_count != input.box_count)
        snapshot.diagnostics.emplace_back("box count was outside the supported range");

    const int usable_slots = snapshot.box_count * slots_per_box;
    std::set<int> seen;
    std::vector<RawSlot> ordered = input.slots;
    std::stable_sort(ordered.begin(), ordered.end(), [](const RawSlot &lhs, const RawSlot &rhs) {
        return lhs.slot_index < rhs.slot_index;
    });

    for (const RawSlot &raw : ordered) {
        if (raw.slot_index < 0 || raw.slot_index >= usable_slots) {
            if (raw.occupied)
                snapshot.diagnostics.emplace_back("ignored occupied slot outside the attached Box range");
            continue;
        }
        if (!seen.insert(raw.slot_index).second) {
            snapshot.diagnostics.emplace_back("ignored duplicate Box slot");
            continue;
        }
        if (!raw.occupied)
            continue;

        BoxSlotSnapshot slot = normalize_slot(raw, input.box_id);
        if (!slot.filament_preset_id) {
            snapshot.diagnostics.emplace_back("occupied slot has no valid exact filament preset id");
            continue;
        }
        if (!slot.colour)
            snapshot.diagnostics.emplace_back("occupied slot has no valid colour");
        snapshot.slots.emplace_back(std::move(slot));
    }

    if (input.external_spool && input.external_spool->occupied &&
        (input.external_spool->filament_index > 0 || !input.external_spool->material_name.empty() || !input.external_spool->material_type.empty())) {
        BoxSlotSnapshot external = normalize_slot(*input.external_spool, input.box_id);
        if (external.filament_preset_id && (external.material_name || external.material_type))
            snapshot.external_spool = std::move(external);
        else
            snapshot.diagnostics.emplace_back("ignored incomplete external spool record");
    }

    if (input.loaded_slot && *input.loaded_slot >= 0 && *input.loaded_slot < usable_slots)
        snapshot.loaded_slot = input.loaded_slot;
    return snapshot;
}

bool snapshot_ready_for_sync(const BoxSnapshotInput &input,
                             bool box_count_seen,
                             const std::array<bool, max_box_slots> &occupancy_seen)
{
    if (!box_count_seen || input.box_count < 0 || input.box_count > max_box_count)
        return false;

    const int usable_slots = input.box_count * slots_per_box;
    for (int slot_index = 0; slot_index < usable_slots; ++slot_index) {
        if (!occupancy_seen[slot_index])
            return false;

        const auto slot = std::find_if(input.slots.begin(), input.slots.end(),
                                       [slot_index](const RawSlot &candidate) {
                                           return candidate.slot_index == slot_index;
                                       });
        if (slot == input.slots.end())
            return false;
        if (slot->occupied && (slot->vendor_index < 0 || slot->filament_index <= 0))
            return false;
    }
    return true;
}

BoxSnapshotInput merge_snapshot_patch(BoxSnapshotInput input, const BoxSnapshotPatch &patch)
{
    if (patch.box_count)
        input.box_count = *patch.box_count;
    if (patch.loaded_slot_present)
        input.loaded_slot = patch.loaded_slot;

    for (const RawSlotPatch &slot_patch : patch.slots) {
        if (slot_patch.slot_index < 0 || slot_patch.slot_index >= max_box_slots)
            continue;

        auto slot = std::find_if(input.slots.begin(), input.slots.end(),
                                 [&slot_patch](const RawSlot &candidate) {
                                     return candidate.slot_index == slot_patch.slot_index;
                                 });
        if (slot == input.slots.end()) {
            RawSlot raw;
            raw.slot_index = slot_patch.slot_index;
            input.slots.emplace_back(std::move(raw));
            slot = input.slots.end() - 1;
        }

        if (slot_patch.occupied) {
            slot->occupancy_known = true;
            slot->occupied = *slot_patch.occupied;
            if (!slot->occupied) {
                slot->vendor_index = -1;
                slot->filament_index = -1;
                slot->material_name.clear();
                slot->material_type.clear();
                slot->colour.reset();
                slot->remaining_percent.reset();
            }
        }

        // Once a slot is known empty, do not let delayed save_variables
        // metadata revive the previous spool. Before the first occupancy event,
        // identity is retained so Moonraker's independent object updates may
        // arrive in either order.
        const bool identity_update_allowed = !slot->occupancy_known || slot->occupied;
        if (slot_patch.vendor_index && (*slot_patch.vendor_index < 0 || identity_update_allowed))
            slot->vendor_index = *slot_patch.vendor_index;
        if (slot_patch.filament_index && (*slot_patch.filament_index <= 0 || identity_update_allowed))
            slot->filament_index = *slot_patch.filament_index;
        if (slot_patch.material_name && (slot_patch.material_name->empty() || identity_update_allowed))
            slot->material_name = *slot_patch.material_name;
        if (slot_patch.material_type && (slot_patch.material_type->empty() || identity_update_allowed))
            slot->material_type = *slot_patch.material_type;
        if (slot_patch.colour_present && (!slot_patch.colour || identity_update_allowed))
            slot->colour = slot_patch.colour;
        if (slot_patch.remaining_present && (!slot_patch.remaining_percent || identity_update_allowed))
            slot->remaining_percent = slot_patch.remaining_percent;
    }

    std::sort(input.slots.begin(), input.slots.end(), [](const RawSlot &lhs, const RawSlot &rhs) {
        return lhs.slot_index < rhs.slot_index;
    });
    return input;
}

DirectSyncState prepare_direct_sync(bool snapshot_ready, bool profile_compatible, std::uint64_t generation)
{
    const bool can_skip_dialog = snapshot_ready && profile_compatible && generation != 0;
    return {can_skip_dialog, can_skip_dialog ? generation : 0};
}

bool mapping_generation_is_current(bool ready, std::uint64_t expected_generation, std::uint64_t current_generation)
{
    return ready && expected_generation != 0 && expected_generation == current_generation;
}

bool mapping_is_current(const BoxSnapshot &snapshot, int slot_index, const std::string &filament_preset_id)
{
    const BoxSlotSnapshot *slot = find_snapshot_slot(snapshot, slot_index);
    return slot && slot->filament_preset_id && *slot->filament_preset_id == filament_preset_id;
}

bool mapping_preference_is_current(const BoxSnapshot &snapshot,
                                   const MappingPreference &preference,
                                   const std::string &project_preset_id)
{
    return mapping_is_current(snapshot, preference.slot_index, preference.slot_preset_id) &&
           qidi_filament_ids_compatible(project_preset_id, preference.slot_preset_id);
}

bool apply_mapping_preference(FilamentInfo &mapping,
                              const BoxSnapshot &snapshot,
                              const MappingPreference &preference,
                              const std::string &project_preset_name,
                              const std::string &project_preset_id)
{
    if (mapping.id != preference.project_filament ||
        project_preset_name != preference.project_preset ||
        !mapping_preference_is_current(snapshot, preference, project_preset_id))
        return false;

    const BoxSlotSnapshot *slot = find_snapshot_slot(snapshot, preference.slot_index);
    if (!slot)
        return false;

    if (preference.slot_index == external_spool_slot)
        assign_mapping_target(mapping, *slot, VIRTUAL_TRAY_MAIN_ID);
    else
        assign_mapping_target(mapping, *slot);
    return true;
}

bool apply_mapping_selection(FilamentInfo &mapping,
                             const BoxSnapshot &snapshot,
                             const MappingSelection &selection)
{
    const std::string main_virtual = std::to_string(VIRTUAL_TRAY_MAIN_ID);
    const std::string deputy_virtual = std::to_string(VIRTUAL_TRAY_DEPUTY_ID);
    const bool external = (selection.ams_id == main_virtual || selection.ams_id == deputy_virtual) &&
                          selection.slot_id == selection.ams_id;
    const BoxSlotSnapshot *slot = external
        ? find_snapshot_slot(snapshot, external_spool_slot)
        : find_snapshot_slot(snapshot, selection.tray_id);
    if (!slot || !slot->filament_preset_id ||
        selection.displayed_preset_id != *slot->filament_preset_id ||
        selection.displayed_material != slot->material_type.value_or("") ||
        selection.displayed_colour != slot->colour.value_or("#CECECE") ||
        !filament_selection_compatible(selection.project_material,
                                       selection.project_preset_id,
                                       slot->material_type.value_or(""),
                                       *slot->filament_preset_id,
                                       selection.enforce_material))
        return false;

    if (external) {
        const int virtual_id = selection.ams_id == main_virtual
            ? VIRTUAL_TRAY_MAIN_ID : VIRTUAL_TRAY_DEPUTY_ID;
        assign_mapping_target(mapping, *slot, virtual_id);
        return true;
    }

    if (selection.ams_id != std::to_string(selection.tray_id / slots_per_box + 1) ||
        selection.slot_id != std::to_string(selection.tray_id))
            return false;

    assign_mapping_target(mapping, *slot);
    return true;
}

MappingValidationResult validate_mapping_result(const BoxSnapshot &snapshot,
                                                const std::vector<FilamentInfo> &mappings)
{
    MappingValidationResult result;
    if (mappings.empty()) {
        result.reason = "mapping list is empty";
        return result;
    }

    for (const FilamentInfo &mapping : mappings) {
        const bool canonical_external =
            ((mapping.ams_id == std::to_string(VIRTUAL_TRAY_MAIN_ID) &&
              mapping.tray_id == VIRTUAL_TRAY_MAIN_ID) ||
             (mapping.ams_id == std::to_string(VIRTUAL_TRAY_DEPUTY_ID) &&
              mapping.tray_id == VIRTUAL_TRAY_DEPUTY_ID)) &&
            mapping.slot_id == mapping.ams_id;
        const bool legacy_external = mapping.tray_id < 0 && mapping.ams_id.empty() &&
            snapshot.external_spool &&
            mapping.slot_id == std::to_string(snapshot.external_spool->slot_index);
        if (canonical_external || legacy_external) {
            if (!snapshot.external_spool ||
                !mapping_target_matches(mapping, *snapshot.external_spool)) {
                result.reason = "project filament is not mapped to the current external spool";
                return result;
            }
            continue;
        }

        const std::string expected_ams = std::to_string(mapping.tray_id / slots_per_box + 1);
        const BoxSlotSnapshot *slot = find_snapshot_slot(snapshot, mapping.tray_id);
        if (mapping.slot_id != std::to_string(mapping.tray_id) || mapping.ams_id != expected_ams || !slot ||
            !mapping_target_matches(mapping, *slot)) {
            result.reason = "project filament is not mapped to a current occupied Box slot";
            return result;
        }
        result.uses_box = true;
    }

    result.valid = true;
    result.reason = "all project filaments map to the current QDS snapshot";
    return result;
}

std::string serialize_mapping_preferences(const MappingContext &context, const MappingPreferences &preferences)
{
    nlohmann::json stored = {
        {"kind", "qds_box_mapping"},
        {"version", 2},
        {"printer_profile", context.printer_profile},
        {"device_id", context.device_id},
        {"mappings", nlohmann::json::array()}
    };
    for (const auto &entry : preferences) {
        const MappingPreference &preference = entry.second;
        stored["mappings"].push_back({
            {"project_filament", preference.project_filament},
            {"project_preset", preference.project_preset},
            {"slot", preference.slot_index},
            {"preset_id", preference.slot_preset_id}
        });
    }
    return stored.dump();
}

MappingPreferences deserialize_mapping_preferences(const std::string &stored,
                                                    const MappingContext &context,
                                                    std::vector<std::string> *diagnostics)
{
    auto diagnose = [diagnostics](const std::string &message) {
        if (diagnostics)
            diagnostics->push_back(message);
    };

    MappingPreferences preferences;
    if (stored.empty())
        return preferences;

    try {
        const nlohmann::json data = nlohmann::json::parse(stored);
        if (!data.is_object() || data.value("kind", "") != "qds_box_mapping" || data.value("version", 0) != 2) {
            diagnose("ignored unsupported QDS mapping schema");
            return preferences;
        }
        if (data.value("printer_profile", "") != context.printer_profile || data.value("device_id", "") != context.device_id) {
            diagnose("ignored QDS mappings saved for another printer context");
            return preferences;
        }
        if (!data.contains("mappings") || !data["mappings"].is_array()) {
            diagnose("ignored QDS mappings with no mapping array");
            return preferences;
        }

        for (const auto &entry : data["mappings"]) {
            if (!entry.is_object() || !entry.contains("project_filament") || !entry.contains("project_preset") ||
                !entry.contains("slot") || !entry.contains("preset_id") ||
                !entry["project_filament"].is_number_integer() || !entry["project_preset"].is_string() ||
                !entry["slot"].is_number_integer() || !entry["preset_id"].is_string()) {
                diagnose("ignored malformed QDS mapping entry");
                continue;
            }

            MappingPreference preference;
            preference.project_filament = entry["project_filament"].get<int>();
            preference.project_preset   = entry["project_preset"].get<std::string>();
            preference.slot_index       = entry["slot"].get<int>();
            preference.slot_preset_id   = entry["preset_id"].get<std::string>();
            if (preference.project_filament < 0 || preference.project_preset.empty() ||
                preference.slot_index < 0 || preference.slot_index > external_spool_slot || preference.slot_preset_id.empty()) {
                diagnose("ignored invalid QDS mapping entry");
                continue;
            }
            if (!preferences.emplace(preference.project_filament, std::move(preference)).second)
                diagnose("ignored duplicate QDS project-filament mapping");
        }
    } catch (const std::exception &) {
        diagnose("ignored malformed QDS mapping JSON");
    }
    return preferences;
}

} // namespace Slic3r::GUI::QDSBoxSync
