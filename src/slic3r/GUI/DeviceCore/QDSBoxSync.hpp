#ifndef slic3r_GUI_DeviceCore_QDSBoxSync_hpp_
#define slic3r_GUI_DeviceCore_QDSBoxSync_hpp_

#include <array>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r::GUI::QDSBoxSync {

constexpr int slots_per_box = 4;
constexpr int max_box_count = 4;
constexpr int max_box_slots = slots_per_box * max_box_count;

struct PrinterMetadata
{
    std::optional<std::string> configured_model;
    // Empty means the connected printer did not advertise a model.
    std::vector<std::string> reported_models;
    // nullopt means absent; an engaged empty vector means present but invalid.
    std::optional<std::vector<double>> reported_nozzles;
};

struct CompatibilityResult
{
    bool        compatible{false};
    std::string effective_model;
    double      effective_nozzle{0.0};
    bool        used_selected_model_fallback{false};
    bool        used_selected_nozzle_fallback{false};
    std::string reason;
};

struct RawSlot
{
    int                        slot_index{-1};
    bool                       occupied{false};
    bool                       occupancy_known{false};
    int                        vendor_index{-1};
    int                        filament_index{-1};
    std::string                material_name;
    std::string                material_type;
    std::optional<std::string> colour;
    std::optional<int>         remaining_percent;
};

// Moonraker object subscriptions deliver an initial object snapshot followed by
// partial updates.  Presence is tracked per field so an omitted value never
// clears a previously validated Box slot.
struct RawSlotPatch
{
    int                        slot_index{-1};
    std::optional<bool>        occupied;
    std::optional<int>         vendor_index;
    std::optional<int>         filament_index;
    std::optional<std::string> material_name;
    std::optional<std::string> material_type;
    bool                       colour_present{false};
    std::optional<std::string> colour;
    bool                       remaining_present{false};
    std::optional<int>         remaining_percent;
};

struct BoxSnapshotPatch
{
    std::optional<int>       box_count;
    std::vector<RawSlotPatch> slots;
    bool                     loaded_slot_present{false};
    std::optional<int>       loaded_slot;
};

struct BoxSlotSnapshot
{
    int                        slot_index{-1};
    bool                       occupied{false};
    int                        vendor_index{-1};
    int                        filament_index{-1};
    std::optional<std::string> material_name;
    std::optional<std::string> material_type;
    std::optional<std::string> filament_preset_id;
    std::optional<std::string> colour;
    std::optional<int>         remaining_percent;
};

struct BoxSnapshotInput
{
    int                        box_count{0};
    std::string                box_id;
    std::vector<RawSlot>       slots;
    std::optional<RawSlot>     external_spool;
    std::optional<int>         loaded_slot;
};

struct BoxSnapshot
{
    int                            box_count{0};
    std::vector<BoxSlotSnapshot>   slots;
    std::optional<BoxSlotSnapshot> external_spool;
    std::optional<int>             loaded_slot;
    std::vector<std::string>       diagnostics;
};

struct MappingContext
{
    std::string printer_profile;
    std::string device_id;
};

struct MappingPreference
{
    int         project_filament{-1};
    std::string project_preset;
    int         slot_index{-1};
    std::string slot_preset_id;
};

using MappingPreferences = std::map<int, MappingPreference>;

std::string normalize_model_name(const std::string &model);
std::string mapping_device_identity(const std::string &host, const std::string &runtime_id);
std::string mapping_storage_key(const std::string &printer_profile);
bool valid_catalog_index(int index, std::size_t catalog_size);
bool qidi_filament_ids_compatible(const std::string &project_preset_id, const std::string &slot_preset_id);
bool filament_selection_compatible(const std::string &project_material,
                                   const std::string &project_preset_id,
                                   const std::string &slot_material,
                                   const std::string &slot_preset_id,
                                   bool enforce_material);
bool prefer_filament_match(bool candidate_exact, double candidate_colour_distance,
                           bool current_exact, double current_colour_distance);
std::optional<std::string> normalize_colour(const std::optional<std::string> &colour);
std::optional<std::string> make_filament_preset_id(const std::string &box_id, int vendor_index, int filament_index);

CompatibilityResult resolve_compatibility(const PrinterMetadata &metadata,
                                          const std::string &selected_model,
                                          double selected_nozzle,
                                          double nozzle_tolerance = 0.001);

BoxSnapshot normalize_snapshot(const BoxSnapshotInput &input);
bool snapshot_ready_for_sync(const BoxSnapshotInput &input,
                             bool box_count_seen,
                             const std::array<bool, max_box_slots> &occupancy_seen);
BoxSnapshotInput merge_snapshot_patch(BoxSnapshotInput input, const BoxSnapshotPatch &patch);
bool mapping_is_current(const BoxSnapshot &snapshot, int slot_index, const std::string &filament_preset_id);
bool mapping_preference_is_current(const BoxSnapshot &snapshot,
                                   const MappingPreference &preference,
                                   const std::string &project_preset_id);
std::string serialize_mapping_preferences(const MappingContext &context, const MappingPreferences &preferences);
MappingPreferences deserialize_mapping_preferences(const std::string &stored,
                                                    const MappingContext &context,
                                                    std::vector<std::string> *diagnostics = nullptr);

} // namespace Slic3r::GUI::QDSBoxSync

#endif
