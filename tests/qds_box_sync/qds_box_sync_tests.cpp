#include <catch_main.hpp>

#include <sstream>

#include "slic3r/GUI/DeviceCore/QDSBoxSync.hpp"
#include "slic3r/GUI/DeviceCore/QDSPrintOptions.hpp"
#include "libslic3r/ProjectTask.hpp"
#include "libslic3r/ResultFilePath.hpp"

using namespace Slic3r::GUI::QDSBoxSync;

namespace {

RawSlot qidi_basic(int slot, const std::string &colour)
{
    RawSlot raw;
    raw.slot_index      = slot;
    raw.occupied        = true;
    raw.occupancy_known = true;
    raw.vendor_index    = 1;
    raw.filament_index  = 7;
    raw.material_name   = "PLA Basic";
    raw.material_type   = "PLA";
    raw.colour          = colour;
    return raw;
}

BoxSnapshot live_snapshot(std::optional<int> loaded_slot = std::nullopt)
{
    BoxSnapshotInput input;
    input.box_count   = 1;
    input.box_id      = "0";
    input.loaded_slot = loaded_slot;
    input.slots = {
        qidi_basic(0, "#228332"),
        qidi_basic(1, "#FF362D"),
        qidi_basic(2, "#DFD628"),
        qidi_basic(3, "#FAFAFA")
    };
    return normalize_snapshot(input);
}

MappingSelection qidi_selection(int tray_id, const std::string &colour)
{
    MappingSelection selection;
    selection.tray_id = tray_id;
    selection.ams_id = std::to_string(tray_id / slots_per_box + 1);
    selection.slot_id = std::to_string(tray_id);
    selection.displayed_preset_id = "QD_0_1_7";
    selection.displayed_material = "PLA";
    selection.displayed_colour = colour;
    selection.project_preset_id = "QD_0_1_7";
    selection.project_material = "PLA";
    return selection;
}

MappingSelection external_selection(const std::string &colour)
{
    auto selection = qidi_selection(0, colour);
    selection.ams_id = "255";
    selection.slot_id = "255";
    return selection;
}

} // namespace

TEST_CASE("packaged QIDI catalog restores the X-Plus 4 Box indexes", "[qds_box_sync][catalog][fallback]")
{
    const std::string path = std::string{TEST_DATA_DIR} + "/../../resources/profiles/officiall_filas_list.cfg";
    const auto catalog = load_packaged_filament_catalog(path);

    REQUIRE(catalog.usable);
    REQUIRE(catalog.entries.size() == 100);
    REQUIRE(catalog.entries[7].name == "PLA Basic");
    REQUIRE(catalog.entries[7].type == "PLA");
    REQUIRE(catalog.entries[1].vendor == "QIDI");
    REQUIRE(catalog.entries[9].colour == "#228332");
    REQUIRE(catalog.entries[18].colour == "#FF362D");
    REQUIRE(catalog.entries[8].colour == "#DFD628");
    REQUIRE(catalog.entries[1].colour == "#FAFAFA");
}

TEST_CASE("packaged catalog parser rejects malformed and incomplete data safely", "[qds_box_sync][catalog][bounds]")
{
    std::istringstream malformed{
        "[fila7]\n"
        "filament=PLA Basic\n"
        "type=PLA\n"
        "[fila100]\n"
        "filament=Out of range\n"
        "type=PLA\n"
        "[colordict]\n"
        "9=#228332\n"
        "100=#FFFFFF\n"
        "[vendor_list]\n"
        "1=QIDI\n"};
    const auto catalog = parse_packaged_filament_catalog(malformed, 100);

    REQUIRE(catalog.usable);
    REQUIRE(catalog.entries[7].name == "PLA Basic");
    REQUIRE(catalog.entries[9].colour == "#228332");
    REQUIRE(catalog.entries[1].vendor == "QIDI");
    REQUIRE_FALSE(catalog.diagnostics.empty());

    std::istringstream incomplete{
        "[fila7]\n"
        "filament=PLA Basic\n"
        "type=PLA\n"};
    REQUIRE_FALSE(parse_packaged_filament_catalog(incomplete).usable);
    REQUIRE_FALSE(load_packaged_filament_catalog("/path/that/does/not/exist").usable);
}

TEST_CASE("selected Plus 4 nozzle profiles supply missing connected metadata", "[qds_box_sync][compatibility]")
{
    for (const double nozzle : {0.2, 0.4, 0.6, 0.8}) {
        INFO("nozzle=" << nozzle);
        const auto result = resolve_compatibility({}, "X-Plus 4", nozzle);
        REQUIRE(result.compatible);
        REQUIRE(result.used_selected_model_fallback);
        REQUIRE(result.used_selected_nozzle_fallback);
        REQUIRE(result.effective_nozzle == Approx(nozzle));
    }
}

TEST_CASE("equivalent Plus 4 spelling and reported 0.4 nozzle are accepted", "[qds_box_sync][compatibility]")
{
    PrinterMetadata metadata;
    metadata.configured_model = "X Plus 4";
    metadata.reported_models = {"XPlus4", "QIDI X-Plus 4", "QIDI TECH X Plus 4"};
    metadata.reported_nozzles = std::vector<double>{0.4};
    const auto result = resolve_compatibility(metadata, "X-Plus 4", 0.4);
    REQUIRE(result.compatible);
    REQUIRE_FALSE(result.used_selected_model_fallback);
    REQUIRE_FALSE(result.used_selected_nozzle_fallback);
}

TEST_CASE("contradictory printer metadata remains blocked", "[qds_box_sync][compatibility]")
{
    PrinterMetadata wrong_model;
    wrong_model.configured_model = "X-Max 4";
    REQUIRE_FALSE(resolve_compatibility(wrong_model, "X-Plus 4", 0.4).compatible);

    PrinterMetadata wrong_reported_model;
    wrong_reported_model.configured_model = "X-Plus 4";
    wrong_reported_model.reported_models = {"X-Plus 4", "QIDI X-Max 4"};
    REQUIRE_FALSE(resolve_compatibility(wrong_reported_model, "X-Plus 4", 0.4).compatible);

    PrinterMetadata wrong_nozzle;
    wrong_nozzle.configured_model = "X-Plus 4";
    wrong_nozzle.reported_nozzles = std::vector<double>{0.6};
    REQUIRE_FALSE(resolve_compatibility(wrong_nozzle, "X-Plus 4", 0.4).compatible);

    PrinterMetadata invalid_nozzle;
    invalid_nozzle.configured_model = "X-Plus 4";
    invalid_nozzle.reported_nozzles = std::vector<double>{};
    REQUIRE_FALSE(resolve_compatibility(invalid_nozzle, "X-Plus 4", 0.4).compatible);
}

TEST_CASE("a fresh connection can fall back after an earlier contradictory nozzle report",
          "[qds_box_sync][compatibility][reconnect]")
{
    PrinterMetadata previous_session;
    previous_session.configured_model = "X-Plus 4";
    previous_session.reported_nozzles = std::vector<double>{0.6};
    REQUIRE_FALSE(resolve_compatibility(previous_session, "X-Plus 4", 0.4).compatible);

    PrinterMetadata fresh_session;
    fresh_session.configured_model = "X-Plus 4";
    const auto result = resolve_compatibility(fresh_session, "X-Plus 4", 0.4);
    REQUIRE(result.compatible);
    REQUIRE(result.used_selected_nozzle_fallback);
}

TEST_CASE("missing metadata fallback is limited to supported Plus 4 nozzle profiles", "[qds_box_sync][compatibility]")
{
    REQUIRE_FALSE(resolve_compatibility({}, "X-Max 4", 0.4).compatible);
    REQUIRE_FALSE(resolve_compatibility({}, "X-Plus 4", 0.3).compatible);

    PrinterMetadata plus4_without_nozzle;
    plus4_without_nozzle.configured_model = "X-Plus 4";
    const auto supported = resolve_compatibility(plus4_without_nozzle, "X-Plus 4", 0.6);
    REQUIRE(supported.compatible);
    REQUIRE(supported.used_selected_model_fallback);
    REQUIRE(supported.used_selected_nozzle_fallback);
}

TEST_CASE("one Box normalizes all four QIDI PLA Basic slots exactly", "[qds_box_sync][snapshot]")
{
    const auto snapshot = live_snapshot(3);
    REQUIRE(snapshot.box_count == 1);
    REQUIRE(snapshot.slots.size() == 4);
    REQUIRE(snapshot.loaded_slot == 3);

    const std::vector<std::string> colours{"#228332", "#FF362D", "#DFD628", "#FAFAFA"};
    for (size_t i = 0; i < snapshot.slots.size(); ++i) {
        REQUIRE(snapshot.slots[i].slot_index == static_cast<int>(i));
        REQUIRE(snapshot.slots[i].material_name == "PLA Basic");
        REQUIRE(snapshot.slots[i].material_type == "PLA");
        REQUIRE(snapshot.slots[i].filament_preset_id == "QD_0_1_7");
        REQUIRE(snapshot.slots[i].colour == colours[i]);
    }
}

TEST_CASE("slots do not depend on a currently loaded filament", "[qds_box_sync][snapshot]")
{
    const auto snapshot = live_snapshot();
    REQUIRE_FALSE(snapshot.loaded_slot.has_value());
    REQUIRE(snapshot.slots.size() == 4);
}

TEST_CASE("empty or incomplete external spool is omitted", "[qds_box_sync][external]")
{
    BoxSnapshotInput input;
    input.box_count = 1;
    input.box_id = "0";
    input.slots = {qidi_basic(0, "#228332")};
    input.external_spool = RawSlot{};
    input.external_spool->occupied = true;
    REQUIRE_FALSE(normalize_snapshot(input).external_spool.has_value());

    input.external_spool.reset();
    REQUIRE_FALSE(normalize_snapshot(input).external_spool.has_value());

    RawSlot incomplete;
    incomplete.slot_index = 16;
    incomplete.occupied = true;
    incomplete.material_type = "PLA";
    input.external_spool = incomplete;
    REQUIRE_FALSE(normalize_snapshot(input).external_spool.has_value());

    incomplete.vendor_index = 1;
    incomplete.filament_index = 7;
    incomplete.material_type.clear();
    input.external_spool = incomplete;
    REQUIRE_FALSE(normalize_snapshot(input).external_spool.has_value());
}

TEST_CASE("missing colour and remaining estimate preserve exact material", "[qds_box_sync][missing]")
{
    BoxSnapshotInput input;
    input.box_count = 1;
    input.box_id = "0";
    RawSlot raw = qidi_basic(0, "invalid");
    raw.remaining_percent = 101;
    input.slots = {raw};
    const auto snapshot = normalize_snapshot(input);
    REQUIRE(snapshot.slots.size() == 1);
    REQUIRE_FALSE(snapshot.slots[0].colour.has_value());
    REQUIRE_FALSE(snapshot.slots[0].remaining_percent.has_value());
    REQUIRE(snapshot.slots[0].material_name == "PLA Basic");
    REQUIRE(snapshot.slots[0].filament_preset_id == "QD_0_1_7");

    REQUIRE(normalize_colour(std::string{"#aabbcc80"}) == "#AABBCC");
    REQUIRE_FALSE(normalize_colour(std::string{"#aabbcczz"}).has_value());
}

TEST_CASE("catalog indexes are range checked before lookup", "[qds_box_sync][bounds][catalog]")
{
    REQUIRE(valid_catalog_index(0, 1));
    REQUIRE(valid_catalog_index(18, 19));
    REQUIRE_FALSE(valid_catalog_index(-1, 19));
    REQUIRE_FALSE(valid_catalog_index(19, 19));
    REQUIRE_FALSE(valid_catalog_index(999, 19));
    REQUIRE_FALSE(valid_catalog_index(0, 0));

    BoxSnapshotInput input;
    input.box_count = 1;
    input.box_id = "0";
    RawSlot invalid_vendor = qidi_basic(0, "#228332");
    invalid_vendor.vendor_index = -1;
    RawSlot invalid_filament = qidi_basic(1, "#FF362D");
    invalid_filament.filament_index = -1;
    input.slots = {invalid_vendor, invalid_filament};

    const auto snapshot = normalize_snapshot(input);
    REQUIRE(snapshot.slots.empty());
    REQUIRE(snapshot.diagnostics.size() == 2);
}

TEST_CASE("invalid and repeated slots are deterministic and safe", "[qds_box_sync][bounds][repeat]")
{
    BoxSnapshotInput input;
    input.box_count = 1;
    input.box_id = "0";
    input.slots = {qidi_basic(3, "#FAFAFA"), qidi_basic(0, "#228332"), qidi_basic(0, "#FFFFFF"), qidi_basic(16, "#FFFFFF")};
    const auto first = normalize_snapshot(input);
    const auto second = normalize_snapshot(input);
    REQUIRE(first.slots.size() == 2);
    REQUIRE(first.slots[0].slot_index == 0);
    REQUIRE(first.slots[1].slot_index == 3);
    REQUIRE(second.slots[0].colour == first.slots[0].colour);
    REQUIRE(second.diagnostics == first.diagnostics);
}

TEST_CASE("partial Moonraker updates preserve omitted Box slot fields", "[qds_box_sync][partial]")
{
    BoxSnapshotInput input;
    input.box_count = 1;
    input.box_id = "0";
    input.slots = {
        qidi_basic(0, "#228332"),
        qidi_basic(1, "#FF362D"),
        qidi_basic(2, "#DFD628"),
        qidi_basic(3, "#FAFAFA")
    };

    BoxSnapshotPatch unrelated_delta;
    input = merge_snapshot_patch(std::move(input), unrelated_delta);
    auto snapshot = normalize_snapshot(input);
    REQUIRE(snapshot.slots.size() == 4);
    REQUIRE(snapshot.slots[0].filament_preset_id == "QD_0_1_7");
    REQUIRE(snapshot.slots[3].colour == "#FAFAFA");

    BoxSnapshotPatch runout_delta;
    RawSlotPatch slot_two;
    slot_two.slot_index = 2;
    slot_two.occupied = false;
    runout_delta.slots.push_back(slot_two);
    snapshot = normalize_snapshot(merge_snapshot_patch(std::move(input), runout_delta));
    REQUIRE(snapshot.slots.size() == 3);
    REQUIRE(snapshot.slots[0].colour == "#228332");
    REQUIRE(snapshot.slots[1].colour == "#FF362D");
    REQUIRE(snapshot.slots[2].slot_index == 3);
    REQUIRE(snapshot.slots[2].colour == "#FAFAFA");
}

TEST_CASE("initial identity and occupancy updates may arrive in either order", "[qds_box_sync][partial][ordering]")
{
    BoxSnapshotInput input;
    input.box_count = 1;
    input.box_id = "0";
    input.slots = {
        qidi_basic(1, "#FF362D"),
        qidi_basic(2, "#DFD628"),
        qidi_basic(3, "#FAFAFA")
    };

    BoxSnapshotPatch identity_delta;
    RawSlotPatch identity;
    identity.slot_index = 0;
    identity.vendor_index = 1;
    identity.filament_index = 7;
    identity.material_name = "PLA Basic";
    identity.material_type = "PLA";
    identity.colour_present = true;
    identity.colour = "#228332";
    identity_delta.slots.push_back(identity);
    input = merge_snapshot_patch(std::move(input), identity_delta);
    REQUIRE_FALSE(input.slots[0].occupancy_known);
    REQUIRE(input.slots[0].filament_index == 7);
    REQUIRE(normalize_snapshot(input).slots.size() == 3);

    BoxSnapshotPatch occupied_delta;
    RawSlotPatch occupied;
    occupied.slot_index = 0;
    occupied.occupied = true;
    occupied_delta.slots.push_back(occupied);
    input = merge_snapshot_patch(std::move(input), occupied_delta);
    REQUIRE(input.slots[0].occupancy_known);
    REQUIRE(normalize_snapshot(input).slots.size() == 4);
    REQUIRE(normalize_snapshot(input).slots[0].filament_preset_id == "QD_0_1_7");
    std::array<bool, max_box_slots> occupancy_seen{};
    occupancy_seen[0] = occupancy_seen[1] = occupancy_seen[2] = occupancy_seen[3] = true;
    REQUIRE(snapshot_ready_for_sync(input, true, occupancy_seen));
}

TEST_CASE("an emptied slot cannot reuse the previous spool identity", "[qds_box_sync][partial][identity]")
{
    BoxSnapshotInput input;
    input.box_count = 1;
    input.box_id = "0";
    input.slots = {
        qidi_basic(0, "#228332"),
        qidi_basic(1, "#FF362D"),
        qidi_basic(2, "#DFD628"),
        qidi_basic(3, "#FAFAFA")
    };

    BoxSnapshotPatch empty_delta;
    RawSlotPatch emptied;
    emptied.slot_index = 0;
    emptied.occupied = false;
    emptied.vendor_index = -1;
    emptied.filament_index = -1;
    emptied.material_name = std::string{};
    emptied.material_type = std::string{};
    emptied.colour_present = true;
    emptied.colour.reset();
    empty_delta.slots.push_back(emptied);
    input = merge_snapshot_patch(std::move(input), empty_delta);
    REQUIRE(normalize_snapshot(input).slots.size() == 3);

    BoxSnapshotPatch stale_identity_delta;
    RawSlotPatch stale_identity;
    stale_identity.slot_index = 0;
    stale_identity.vendor_index = 1;
    stale_identity.filament_index = 7;
    stale_identity.material_name = "PLA Basic";
    stale_identity.material_type = "PLA";
    stale_identity.colour_present = true;
    stale_identity.colour = "#228332";
    stale_identity_delta.slots.push_back(stale_identity);
    input = merge_snapshot_patch(std::move(input), stale_identity_delta);
    REQUIRE(input.slots[0].vendor_index == -1);
    REQUIRE(input.slots[0].filament_index == -1);
    REQUIRE(input.slots[0].material_name.empty());
    REQUIRE(input.slots[0].material_type.empty());
    REQUIRE_FALSE(input.slots[0].colour.has_value());

    BoxSnapshotPatch combined_insertion_delta;
    RawSlotPatch combined_insertion;
    combined_insertion.slot_index = 0;
    combined_insertion.occupied = true;
    combined_insertion.vendor_index = 1;
    combined_insertion.filament_index = 7;
    combined_insertion.material_name = "PLA Basic";
    combined_insertion.material_type = "PLA";
    combined_insertion.colour_present = true;
    combined_insertion.colour = "#112233";
    combined_insertion_delta.slots.push_back(combined_insertion);
    const auto combined_input = merge_snapshot_patch(input, combined_insertion_delta);
    const auto combined_snapshot = normalize_snapshot(combined_input);
    REQUIRE(combined_snapshot.slots.size() == 4);
    REQUIRE(combined_snapshot.slots[0].filament_preset_id == "QD_0_1_7");
    REQUIRE(combined_snapshot.slots[0].colour == "#112233");
    std::array<bool, max_box_slots> combined_occupancy_seen{};
    combined_occupancy_seen[0] = combined_occupancy_seen[1] = combined_occupancy_seen[2] = combined_occupancy_seen[3] = true;
    REQUIRE(snapshot_ready_for_sync(combined_input, true, combined_occupancy_seen));

    BoxSnapshotPatch occupied_only_delta;
    RawSlotPatch occupied_only;
    occupied_only.slot_index = 0;
    occupied_only.occupied = true;
    occupied_only_delta.slots.push_back(occupied_only);
    input = merge_snapshot_patch(std::move(input), occupied_only_delta);
    REQUIRE(normalize_snapshot(input).slots.size() == 3);

    std::array<bool, max_box_slots> occupancy_seen{};
    occupancy_seen[0] = occupancy_seen[1] = occupancy_seen[2] = occupancy_seen[3] = true;
    REQUIRE_FALSE(snapshot_ready_for_sync(input, true, occupancy_seen));
}

TEST_CASE("Box synchronization waits for a complete snapshot after reconnect",
          "[qds_box_sync][snapshot][reconnect]")
{
    BoxSnapshotInput input;
    std::array<bool, max_box_slots> occupancy_seen{};
    REQUIRE_FALSE(snapshot_ready_for_sync(input, false, occupancy_seen));

    input.box_count = 1;
    REQUIRE_FALSE(snapshot_ready_for_sync(input, true, occupancy_seen));

    input.box_count = -1;
    REQUIRE_FALSE(snapshot_ready_for_sync(input, true, occupancy_seen));
    input.box_count = max_box_count + 1;
    REQUIRE_FALSE(snapshot_ready_for_sync(input, true, occupancy_seen));
    input.box_count = 1;

    input.slots = {
        qidi_basic(0, "#228332"),
        qidi_basic(1, "#FF362D"),
        qidi_basic(2, "#DFD628"),
        qidi_basic(3, "#FAFAFA")
    };
    occupancy_seen[0] = occupancy_seen[1] = occupancy_seen[2] = true;
    REQUIRE_FALSE(snapshot_ready_for_sync(input, true, occupancy_seen));

    occupancy_seen[3] = true;
    REQUIRE(snapshot_ready_for_sync(input, true, occupancy_seen));

    input.slots[2].vendor_index = -1;
    REQUIRE_FALSE(snapshot_ready_for_sync(input, true, occupancy_seen));

    input.slots[2].occupied = false;
    REQUIRE(snapshot_ready_for_sync(input, true, occupancy_seen));

    input.box_count = 0;
    occupancy_seen.fill(false);
    REQUIRE(snapshot_ready_for_sync(input, true, occupancy_seen));
}

TEST_CASE("stale persisted mapping is rejected", "[qds_box_sync][mapping]")
{
    auto snapshot = live_snapshot();
    REQUIRE(mapping_is_current(snapshot, 0, "QD_0_1_7"));
    REQUIRE_FALSE(mapping_is_current(snapshot, 0, "QD_0_1_6"));
    REQUIRE_FALSE(mapping_is_current(snapshot, 9, "QD_0_1_7"));

    const MappingPreference preference{0, "QIDI PLA Basic", 0, "QD_0_1_7"};
    REQUIRE(mapping_preference_is_current(snapshot, preference, "QD_0_1_7"));
    REQUIRE_FALSE(mapping_preference_is_current(snapshot, preference, "QD_0_1_6"));
}

TEST_CASE("persisted mapping restoration has one deterministic representation",
          "[qds_box_sync][mapping][persistence]")
{
    auto snapshot = live_snapshot();
    MappingPreference preference{0, "QIDI PLA Basic @Qidi X-Plus 4 0.4 nozzle", 3, "QD_0_1_7"};
    Slic3r::FilamentInfo mapping;
    mapping.id = 0;
    mapping.color = "228332FF";
    mapping.colors = {"112233FF"};
    mapping.ctype = 2;

    REQUIRE(apply_mapping_preference(mapping, snapshot, preference,
                                     preference.project_preset, "QD_0_1_7"));
    REQUIRE(mapping.tray_id == 3);
    REQUIRE(mapping.ams_id == "1");
    REQUIRE(mapping.slot_id == "3");
    REQUIRE(mapping.filament_id == "QD_0_1_7");
    REQUIRE(mapping.type == "PLA");
    REQUIRE(mapping.color == "FAFAFAFF");
    REQUIRE(mapping.distance == 0.0f);
    REQUIRE(mapping.mapping_result == Slic3r::MAPPING_RESULT_DEFAULT);
    REQUIRE(mapping.ctype == 0);
    REQUIRE(mapping.colors.empty());

    REQUIRE_FALSE(apply_mapping_preference(mapping, snapshot, preference,
                                            "changed project preset", "QD_0_1_7"));
    REQUIRE_FALSE(apply_mapping_preference(mapping, snapshot, preference,
                                            preference.project_preset, "QD_0_1_6"));

    BoxSnapshotInput missing_colour;
    missing_colour.box_count = 1;
    missing_colour.box_id = "0";
    RawSlot slot = qidi_basic(0, "#228332");
    slot.colour.reset();
    missing_colour.slots.push_back(slot);
    auto neutral_snapshot = normalize_snapshot(missing_colour);
    preference.slot_index = 0;
    REQUIRE(apply_mapping_preference(mapping, neutral_snapshot, preference,
                                     preference.project_preset, "QD_0_1_7"));
    REQUIRE(mapping.color == "CECECEFF");
}

TEST_CASE("final send validation accepts every current Box slot and reports Box use",
          "[qds_box_sync][mapping][send]")
{
    const auto snapshot = live_snapshot();
    for (int slot = 0; slot < 4; ++slot) {
        Slic3r::FilamentInfo mapping;
        mapping.id          = 0;
        mapping.tray_id     = slot;
        mapping.ams_id      = "1";
        mapping.slot_id     = std::to_string(slot);
        mapping.filament_id = "QD_0_1_7";
        mapping.type        = "PLA";
        mapping.color       = std::array<std::string, 4>{"228332FF", "FF362DFF", "DFD628FF", "FAFAFAFF"}[slot];

        const auto validation = validate_mapping_result(snapshot, {mapping});
        REQUIRE(validation.valid);
        REQUIRE(validation.uses_box);
    }
}

TEST_CASE("final send validation rejects unmapped, stale, and inconsistent QDS mappings",
          "[qds_box_sync][mapping][send]")
{
    const auto snapshot = live_snapshot();
    Slic3r::FilamentInfo mapping;
    mapping.id = 0;
    REQUIRE_FALSE(validate_mapping_result(snapshot, {mapping}).valid);
    REQUIRE_FALSE(validate_mapping_result(snapshot, {}).valid);

    mapping.tray_id     = 3;
    mapping.ams_id      = "1";
    mapping.slot_id     = "3";
    mapping.filament_id = "QD_0_1_6";
    mapping.type        = "PLA";
    mapping.color       = "FAFAFAFF";
    REQUIRE_FALSE(validate_mapping_result(snapshot, {mapping}).valid);

    mapping.filament_id = "QD_0_1_7";
    mapping.ams_id      = "2";
    REQUIRE_FALSE(validate_mapping_result(snapshot, {mapping}).valid);
    mapping.ams_id  = "1";
    mapping.slot_id = "2";
    REQUIRE_FALSE(validate_mapping_result(snapshot, {mapping}).valid);
}

TEST_CASE("final send validation accepts only the current optional external spool",
          "[qds_box_sync][mapping][send][external]")
{
    BoxSnapshotInput input;
    input.box_count = 1;
    input.box_id = "0";
    input.slots = {qidi_basic(0, "#228332")};
    RawSlot external = qidi_basic(16, "#FAFAFA");
    input.external_spool = external;
    const auto snapshot = normalize_snapshot(input);
    REQUIRE(snapshot.external_spool.has_value());

    Slic3r::FilamentInfo mapping;
    mapping.id          = 0;
    mapping.filament_id = "project-preset-before-selection";
    REQUIRE(apply_mapping_selection(mapping, snapshot, external_selection("#FAFAFA")));
    REQUIRE(mapping.tray_id == 255);
    REQUIRE(mapping.ams_id == "255");
    REQUIRE(mapping.slot_id == "255");
    REQUIRE(mapping.filament_id == "QD_0_1_7");
    REQUIRE(mapping.type == "PLA");
    REQUIRE(mapping.color == "FAFAFAFF");
    const auto validation = validate_mapping_result(snapshot, {mapping});
    REQUIRE(validation.valid);
    REQUIRE_FALSE(validation.uses_box);

    mapping.filament_id = "QD_0_1_6";
    REQUIRE_FALSE(validate_mapping_result(snapshot, {mapping}).valid);
    REQUIRE_FALSE(validate_mapping_result(live_snapshot(), {mapping}).valid);

    mapping.tray_id     = -1;
    mapping.ams_id.clear();
    mapping.slot_id     = "16";
    mapping.filament_id = "QD_0_1_7";
    mapping.type        = "PLA";
    mapping.color       = "FAFAFAFF";
    REQUIRE(validate_mapping_result(snapshot, {mapping}).valid);
}

TEST_CASE("manual Box selection refreshes exact physical preset and material identity",
          "[qds_box_sync][mapping][selection]")
{
    const auto snapshot = live_snapshot();
    Slic3r::FilamentInfo mapping;
    mapping.id          = 0;
    mapping.filament_id = "generic-project-preset";
    mapping.type        = "generic-pla";
    mapping.color       = "000000FF";

    REQUIRE(apply_mapping_selection(mapping, snapshot, qidi_selection(3, "#FAFAFA")));
    REQUIRE(mapping.tray_id == 3);
    REQUIRE(mapping.ams_id == "1");
    REQUIRE(mapping.slot_id == "3");
    REQUIRE(mapping.filament_id == "QD_0_1_7");
    REQUIRE(mapping.type == "PLA");
    REQUIRE(mapping.color == "FAFAFAFF");
    REQUIRE(validate_mapping_result(snapshot, {mapping}).valid);

    const auto accepted_mapping = mapping;
    auto wrong_ams = qidi_selection(3, "#FAFAFA");
    wrong_ams.ams_id = "2";
    REQUIRE_FALSE(apply_mapping_selection(mapping, snapshot, wrong_ams));
    REQUIRE(mapping.filament_id == accepted_mapping.filament_id);
    REQUIRE(mapping.slot_id == accepted_mapping.slot_id);

    auto wrong_slot = qidi_selection(3, "#FAFAFA");
    wrong_slot.slot_id = "2";
    REQUIRE_FALSE(apply_mapping_selection(mapping, snapshot, wrong_slot));
    REQUIRE_FALSE(apply_mapping_selection(mapping, live_snapshot(), external_selection("#FAFAFA")));

    auto stale_preset = qidi_selection(3, "#FAFAFA");
    stale_preset.displayed_preset_id = "QD_0_1_6";
    REQUIRE_FALSE(apply_mapping_selection(mapping, snapshot, stale_preset));

    auto stale_colour = qidi_selection(3, "#228332");
    REQUIRE_FALSE(apply_mapping_selection(mapping, snapshot, stale_colour));

    auto incompatible_project = qidi_selection(3, "#FAFAFA");
    incompatible_project.project_material = "PETG";
    REQUIRE_FALSE(apply_mapping_selection(mapping, snapshot, incompatible_project));
}

TEST_CASE("same-preset colour changes invalidate a serialized mapping", "[qds_box_sync][mapping][send][colour]")
{
    Slic3r::FilamentInfo mapping;
    mapping.id = 0;
    REQUIRE(apply_mapping_selection(mapping, live_snapshot(), qidi_selection(0, "#228332")));
    REQUIRE(validate_mapping_result(live_snapshot(), {mapping}).valid);

    BoxSnapshotInput changed;
    changed.box_count = 1;
    changed.box_id = "0";
    changed.slots = {qidi_basic(0, "#FF362D")};
    REQUIRE_FALSE(validate_mapping_result(normalize_snapshot(changed), {mapping}).valid);
}

TEST_CASE("exact QIDI identity is required before colour chooses among equivalent slots", "[qds_box_sync][mapping]")
{
    REQUIRE(qidi_filament_ids_compatible("QD_0_1_7", "QD_0_1_7"));
    REQUIRE_FALSE(qidi_filament_ids_compatible("QD_0_1_6", "QD_0_1_7"));
    REQUIRE(qidi_filament_ids_compatible("", "QD_0_1_7"));

    REQUIRE(filament_selection_compatible("PLA", "QD_0_1_7", "pla", "QD_0_1_7", true));
    REQUIRE_FALSE(filament_selection_compatible("PLA", "QD_0_1_6", "PLA", "QD_0_1_7", true));
    REQUIRE_FALSE(filament_selection_compatible("PLA", "QD_0_1_6", "PLA", "QD_0_1_7", false));
    REQUIRE(filament_selection_compatible("PLA", "", "pla", "QD_0_1_7", true));
    REQUIRE_FALSE(filament_selection_compatible("PETG", "", "PLA", "QD_0_1_7", true));

    REQUIRE(prefer_filament_match(true, 120.0, false, 1.0));
    REQUIRE(prefer_filament_match(true, 5.0, true, 20.0));
    REQUIRE_FALSE(prefer_filament_match(true, 20.0, true, 5.0));
    REQUIRE_FALSE(prefer_filament_match(false, 1.0, true, 120.0));
}

TEST_CASE("mapping preferences are scoped to printer, device, and project preset", "[qds_box_sync][mapping][persistence]")
{
    const MappingContext context{"X-Plus 4 0.4 nozzle", "printer-a"};
    MappingPreferences preferences;
    preferences.emplace(0, MappingPreference{0, "QIDI PLA Basic @Qidi X-Plus 4 0.4 nozzle", 3, "QD_0_1_7"});

    const std::string stored = serialize_mapping_preferences(context, preferences);
    const auto restored = deserialize_mapping_preferences(stored, context);
    REQUIRE(restored.size() == 1);
    REQUIRE(restored.at(0).project_preset == "QIDI PLA Basic @Qidi X-Plus 4 0.4 nozzle");
    REQUIRE(restored.at(0).slot_index == 3);
    REQUIRE(restored.at(0).slot_preset_id == "QD_0_1_7");

    REQUIRE(deserialize_mapping_preferences(stored, {"X-Plus 4 0.6 nozzle", "printer-a"}).empty());
    REQUIRE(deserialize_mapping_preferences(stored, {"X-Plus 4 0.4 nozzle", "printer-b"}).empty());
}

TEST_CASE("external-spool mapping preferences persist and restore canonically",
          "[qds_box_sync][mapping][persistence][external]")
{
    BoxSnapshotInput input;
    input.box_count = 1;
    input.box_id = "0";
    input.slots = {qidi_basic(0, "#228332")};
    input.external_spool = qidi_basic(external_spool_slot, "#FAFAFA");
    const auto snapshot = normalize_snapshot(input);

    const MappingContext context{"X-Plus 4 0.4 nozzle", "printer-a"};
    MappingPreferences preferences;
    preferences.emplace(0, MappingPreference{0, "QIDI PLA Basic", external_spool_slot, "QD_0_1_7"});
    const auto restored = deserialize_mapping_preferences(serialize_mapping_preferences(context, preferences), context);
    REQUIRE(restored.size() == 1);
    REQUIRE(restored.at(0).slot_index == external_spool_slot);

    Slic3r::FilamentInfo mapping;
    mapping.id = 0;
    REQUIRE(apply_mapping_preference(mapping, snapshot, restored.at(0), "QIDI PLA Basic", "QD_0_1_7"));
    REQUIRE(mapping.tray_id == 255);
    REQUIRE(mapping.ams_id == "255");
    REQUIRE(mapping.slot_id == "255");
    REQUIRE_FALSE(validate_mapping_result(snapshot, {mapping}).uses_box);
}

TEST_CASE("local mapping identity survives regenerated runtime ids", "[qds_box_sync][mapping][persistence]")
{
    REQUIRE(mapping_device_identity(" 192.0.2.1 ", "1234") == "192.0.2.1");
    REQUIRE(mapping_device_identity("192.0.2.1", "9876") == "192.0.2.1");
    REQUIRE(mapping_device_identity("", "DEVICE-A") == "device-a");
    REQUIRE(mapping_device_identity("   ", "DEVICE-B") == "device-b");
    REQUIRE(mapping_storage_key("X-Plus 4 0.4 nozzle") == "qds_box:X-Plus 4 0.4 nozzle");
    REQUIRE(mapping_storage_key("X-Plus 4 0.4 nozzle") != "X-Plus 4 0.4 nozzle");
    REQUIRE(mapping_storage_key("").empty());
}

TEST_CASE("empty mappings clear safely and legacy or malformed data is ignored", "[qds_box_sync][mapping][persistence]")
{
    const MappingContext context{"X-Plus 4 0.4 nozzle", "printer-a"};
    const std::string empty = serialize_mapping_preferences(context, {});
    REQUIRE(deserialize_mapping_preferences(empty, context).empty());

    std::vector<std::string> diagnostics;
    REQUIRE(deserialize_mapping_preferences(R"({"version":1,"mappings":[]})", context, &diagnostics).empty());
    REQUIRE_FALSE(diagnostics.empty());
    diagnostics.clear();
    REQUIRE(deserialize_mapping_preferences("not-json", context, &diagnostics).empty());
    REQUIRE_FALSE(diagnostics.empty());
}

TEST_CASE("mapping confirmation rejects unavailable or changed snapshot generations",
          "[qds_box_sync][mapping][generation]")
{
    REQUIRE(mapping_generation_is_current(true, 7, 7));
    REQUIRE_FALSE(mapping_generation_is_current(false, 7, 7));
    REQUIRE_FALSE(mapping_generation_is_current(true, 0, 0));
    REQUIRE_FALSE(mapping_generation_is_current(true, 7, 8));
}

TEST_CASE("empty-project direct synchronization captures the current QDS generation",
          "[qds_box_sync][mapping][generation][direct]")
{
    const auto ready = prepare_direct_sync(true, true, 11);
    REQUIRE(ready.can_skip_dialog);
    REQUIRE(ready.generation == 11);
    REQUIRE(mapping_generation_is_current(true, ready.generation, 11));

    REQUIRE_FALSE(prepare_direct_sync(false, true, 11).can_skip_dialog);
    REQUIRE_FALSE(prepare_direct_sync(true, false, 11).can_skip_dialog);
    REQUIRE_FALSE(prepare_direct_sync(true, true, 0).can_skip_dialog);
}

TEST_CASE("external-spool material policy does not weaken exact QIDI identity",
          "[qds_box_sync][mapping][external]")
{
    BoxSnapshotInput input;
    input.box_count = 1;
    input.box_id = "0";
    input.slots = {qidi_basic(0, "#228332")};
    input.external_spool = qidi_basic(external_spool_slot, "#FAFAFA");
    const auto snapshot = normalize_snapshot(input);

    Slic3r::FilamentInfo mapping;
    mapping.id = 0;
    auto relaxed = external_selection("#FAFAFA");
    relaxed.project_preset_id.clear();
    relaxed.project_material = "PETG";
    relaxed.enforce_material = false;
    REQUIRE(apply_mapping_selection(mapping, snapshot, relaxed));
    REQUIRE(mapping.tray_id == 255);
    REQUIRE(mapping.type == "PLA");

    auto strict = relaxed;
    strict.enforce_material = true;
    REQUIRE_FALSE(apply_mapping_selection(mapping, snapshot, strict));

    auto wrong_qidi_identity = relaxed;
    wrong_qidi_identity.project_preset_id = "QD_0_1_6";
    REQUIRE_FALSE(apply_mapping_selection(mapping, snapshot, wrong_qidi_identity));
}

TEST_CASE("result diagnostics never default inside a signed app bundle",
          "[qds_box_sync][packaging][result_path]")
{
    namespace fs = boost::filesystem;
    const fs::path resources = "/Applications/QIDIStudio.app/Contents/Resources";
    const fs::path app_data = "/Users/test/Library/Application Support/QIDIStudioInternal";

    REQUIRE(Slic3r::resolve_result_file_path(
                "", resources, resources, app_data) == app_data / "result.json");
    REQUIRE(Slic3r::resolve_result_file_path(
                "", resources / "profiles", resources, app_data) == app_data / "result.json");
    REQUIRE(Slic3r::resolve_result_file_path(
                "", "/Users/test/Jobs", resources, app_data) == fs::path("/Users/test/Jobs/result.json"));
    REQUIRE(Slic3r::resolve_result_file_path(
                "", "/Applications/QIDIStudio.app/Contents/Resources-copy", resources, app_data) ==
            fs::path("/Applications/QIDIStudio.app/Contents/Resources-copy/result.json"));
    REQUIRE(Slic3r::resolve_result_file_path(
                "/tmp/qidi-output", resources, resources, app_data) ==
            fs::path("/tmp/qidi-output/result.json"));
}

TEST_CASE("QIDI bed-leveling choice is embedded in the print job",
          "[qds_print_options][bed_leveling]")
{
    using Slic3r::GUI::QDSPrintOptions::prepare_bed_leveling;

    const auto enabled = prepare_bed_leveling("on");
    REQUIRE(enabled.gcode_preamble == "G31\n");
    REQUIRE(enabled.estimated_overhead_seconds ==
            Slic3r::GUI::QDSPrintOptions::full_bed_leveling_estimate_seconds);

    const auto disabled = prepare_bed_leveling("off");
    REQUIRE(disabled.gcode_preamble == "G32\n");
    REQUIRE(disabled.estimated_overhead_seconds == 0);

    const auto automatic = prepare_bed_leveling("auto");
    REQUIRE(automatic.gcode_preamble.empty());
    REQUIRE(automatic.estimated_overhead_seconds == 0);

    const auto malformed = prepare_bed_leveling("OFF");
    REQUIRE(malformed.gcode_preamble.empty());
    REQUIRE(malformed.estimated_overhead_seconds == 0);
}
