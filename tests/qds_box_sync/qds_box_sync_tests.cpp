#include <catch_main.hpp>

#include "slic3r/GUI/DeviceCore/QDSBoxSync.hpp"

using namespace Slic3r::GUI::QDSBoxSync;

namespace {

RawSlot qidi_basic(int slot, const std::string &colour)
{
    RawSlot raw;
    raw.slot_index     = slot;
    raw.occupied       = true;
    raw.vendor_index   = 1;
    raw.filament_index = 7;
    raw.material_name  = "PLA Basic";
    raw.material_type  = "PLA";
    raw.colour         = colour;
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

} // namespace

TEST_CASE("selected Plus 4 profile supplies missing connected metadata", "[qds_box_sync][compatibility]")
{
    PrinterMetadata metadata;
    const auto result = resolve_compatibility(metadata, "X-Plus 4", 0.4);
    REQUIRE(result.compatible);
    REQUIRE(result.used_selected_model_fallback);
    REQUIRE(result.used_selected_nozzle_fallback);
    REQUIRE(result.effective_nozzle == Approx(0.4));
}

TEST_CASE("equivalent Plus 4 spelling and reported 0.4 nozzle are accepted", "[qds_box_sync][compatibility]")
{
    PrinterMetadata metadata;
    metadata.configured_model = "X Plus 4";
    metadata.reported_nozzles = {0.4};
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

    PrinterMetadata wrong_nozzle;
    wrong_nozzle.configured_model = "X-Plus 4";
    wrong_nozzle.reported_nozzles = {0.6};
    REQUIRE_FALSE(resolve_compatibility(wrong_nozzle, "X-Plus 4", 0.4).compatible);
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
    REQUIRE(snapshot.slots.size() == 2);
    REQUIRE_FALSE(snapshot.slots[0].filament_preset_id.has_value());
    REQUIRE_FALSE(snapshot.slots[1].filament_preset_id.has_value());
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

TEST_CASE("stale persisted mapping is rejected", "[qds_box_sync][mapping]")
{
    auto snapshot = live_snapshot();
    REQUIRE(mapping_is_current(snapshot, 0, "QD_0_1_7"));
    REQUIRE_FALSE(mapping_is_current(snapshot, 0, "QD_0_1_6"));
    REQUIRE_FALSE(mapping_is_current(snapshot, 9, "QD_0_1_7"));
}
