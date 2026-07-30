#include "QDSBoxSync.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>

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

} // namespace

std::string normalize_model_name(const std::string &model)
{
    std::string normalized;
    normalized.reserve(model.size());
    for (const unsigned char ch : model) {
        if (std::isalnum(ch))
            normalized.push_back(static_cast<char>(std::tolower(ch)));
    }
    return normalized;
}

bool valid_catalog_index(int index, std::size_t catalog_size)
{
    return index >= 0 && static_cast<std::size_t>(index) < catalog_size;
}

std::optional<std::string> normalize_colour(const std::optional<std::string> &colour)
{
    if (!colour || (colour->size() != 7 && colour->size() != 9) || colour->front() != '#')
        return std::nullopt;

    std::string normalized = colour->substr(0, 7);
    for (size_t i = 1; i < normalized.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(normalized[i]);
        if (!std::isxdigit(ch))
            return std::nullopt;
        normalized[i] = static_cast<char>(std::toupper(ch));
    }
    return normalized;
}

std::optional<std::string> make_filament_preset_id(const std::string &box_id, int vendor_index, int filament_index)
{
    if (box_id.empty() || vendor_index < 0 || filament_index <= 0)
        return std::nullopt;
    return "QD_" + box_id + "_" + std::to_string(vendor_index) + "_" + std::to_string(filament_index);
}

CompatibilityResult resolve_compatibility(const PrinterMetadata &metadata,
                                          const std::string &selected_model,
                                          double selected_nozzle,
                                          double nozzle_tolerance)
{
    CompatibilityResult result;
    const std::string selected = normalize_model_name(selected_model);
    if (selected.empty() || selected_nozzle <= 0.0) {
        result.reason = "selected printer profile is incomplete";
        return result;
    }

    const std::string configured = metadata.configured_model ? normalize_model_name(*metadata.configured_model) : std::string{};
    const std::string reported   = metadata.reported_model ? normalize_model_name(*metadata.reported_model) : std::string{};
    if (!configured.empty() && !reported.empty() && configured != reported) {
        result.reason = "configured and reported printer models contradict each other";
        return result;
    }

    const std::string connected = !reported.empty() ? reported : configured;
    if (!connected.empty() && connected != selected) {
        result.reason = "connected printer model contradicts the selected profile";
        return result;
    }

    result.effective_model = selected_model;
    result.used_selected_model_fallback = connected.empty();

    if (metadata.reported_nozzles.empty()) {
        result.effective_nozzle = selected_nozzle;
        result.used_selected_nozzle_fallback = true;
    } else {
        const auto match = std::find_if(metadata.reported_nozzles.begin(), metadata.reported_nozzles.end(),
                                        [selected_nozzle, nozzle_tolerance](double nozzle) {
                                            return std::fabs(nozzle - selected_nozzle) <= nozzle_tolerance;
                                        });
        if (match == metadata.reported_nozzles.end()) {
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
    std::sort(ordered.begin(), ordered.end(), [](const RawSlot &lhs, const RawSlot &rhs) {
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
        if (!slot.filament_preset_id)
            snapshot.diagnostics.emplace_back("occupied slot has no valid exact filament preset id");
        if (!slot.colour)
            snapshot.diagnostics.emplace_back("occupied slot has no valid colour");
        snapshot.slots.emplace_back(std::move(slot));
    }

    if (input.external_spool && input.external_spool->occupied &&
        (input.external_spool->filament_index > 0 || !input.external_spool->material_name.empty() || !input.external_spool->material_type.empty())) {
        snapshot.external_spool = normalize_slot(*input.external_spool, input.box_id);
    }

    if (input.loaded_slot && *input.loaded_slot >= 0 && *input.loaded_slot < max_box_slots)
        snapshot.loaded_slot = input.loaded_slot;
    return snapshot;
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

        if (slot_patch.occupied)
            slot->occupied = *slot_patch.occupied;
        if (slot_patch.vendor_index)
            slot->vendor_index = *slot_patch.vendor_index;
        if (slot_patch.filament_index)
            slot->filament_index = *slot_patch.filament_index;
        if (slot_patch.material_name)
            slot->material_name = *slot_patch.material_name;
        if (slot_patch.material_type)
            slot->material_type = *slot_patch.material_type;
        if (slot_patch.colour_present)
            slot->colour = slot_patch.colour;
        if (slot_patch.remaining_present)
            slot->remaining_percent = slot_patch.remaining_percent;
    }

    std::sort(input.slots.begin(), input.slots.end(), [](const RawSlot &lhs, const RawSlot &rhs) {
        return lhs.slot_index < rhs.slot_index;
    });
    return input;
}

bool mapping_is_current(const BoxSnapshot &snapshot, int slot_index, const std::string &filament_preset_id)
{
    const auto iter = std::find_if(snapshot.slots.begin(), snapshot.slots.end(),
                                   [slot_index](const BoxSlotSnapshot &slot) { return slot.slot_index == slot_index; });
    return iter != snapshot.slots.end() && iter->filament_preset_id && *iter->filament_preset_id == filament_preset_id;
}

} // namespace Slic3r::GUI::QDSBoxSync
