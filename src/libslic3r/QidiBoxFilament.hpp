#ifndef slic3r_QidiBoxFilament_hpp_
#define slic3r_QidiBoxFilament_hpp_

#include <algorithm>
#include <charconv>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Slic3r::QidiBoxFilament {

enum class IdentityKind
{
    NotBox,
    Malformed,
    Generic,
    Branded
};

struct Identity
{
    IdentityKind kind{IdentityKind::NotBox};
    int          box_index{-1};
    int          vendor_index{-1};
    int          filament_index{-1};
};

enum class ResolutionKind
{
    None,
    BrandedExact,
    RetainedProject,
    GenericSystem
};

struct PresetCandidate
{
    std::string name;
    std::string id;
    std::string material;
    bool        compatible{false};
    bool        system{false};
    bool        exact_identity_base{false};
};

struct Resolution
{
    ResolutionKind kind{ResolutionKind::None};
    std::size_t    candidate_index{0};

    explicit operator bool() const { return kind != ResolutionKind::None; }
};

struct SlotRequest
{
    std::string                id;
    std::string                material;
    std::optional<std::size_t> current_candidate;
};

inline bool parse_nonnegative_int(std::string_view text, int &value)
{
    if (text.empty())
        return false;

    const char *first = text.data();
    const char *last  = first + text.size();
    const auto  parsed = std::from_chars(first, last, value);
    return parsed.ec == std::errc{} && parsed.ptr == last && value >= 0;
}

// QIDI Box identifiers use QD_<box>_<vendor>_<filament>. The packaged
// officiall_filas_list.cfg catalog reserves vendor 0 for Generic materials;
// every other vendor is treated as branded and therefore requires an exact
// preset match. Unknown future vendors consequently fail closed.
inline Identity classify(std::string_view preset_id)
{
    if (preset_id.rfind("QD_", 0) != 0)
        return {};

    Identity result;
    result.kind = IdentityKind::Malformed;

    preset_id.remove_prefix(3);
    const auto first_separator = preset_id.find('_');
    if (first_separator == std::string_view::npos)
        return result;
    const auto second_separator = preset_id.find('_', first_separator + 1);
    if (second_separator == std::string_view::npos ||
        preset_id.find('_', second_separator + 1) != std::string_view::npos)
        return result;

    if (!parse_nonnegative_int(preset_id.substr(0, first_separator), result.box_index) ||
        !parse_nonnegative_int(preset_id.substr(first_separator + 1,
                                                second_separator - first_separator - 1),
                               result.vendor_index) ||
        !parse_nonnegative_int(preset_id.substr(second_separator + 1), result.filament_index) ||
        result.filament_index == 0)
        return result;

    result.kind = result.vendor_index == 0 ? IdentityKind::Generic : IdentityKind::Branded;
    return result;
}

inline std::string normalize_material_family(std::string value)
{
    const auto first = std::find_if_not(value.begin(), value.end(),
                                        [](unsigned char ch) { return std::isspace(ch); });
    const auto last = std::find_if_not(value.rbegin(), value.rend(),
                                       [](unsigned char ch) { return std::isspace(ch); }).base();
    if (first >= last)
        return {};

    std::string normalized(first, last);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return normalized;
}

inline bool material_families_compatible(const std::string &project_material,
                                         const std::string &slot_material)
{
    const std::string project = normalize_material_family(project_material);
    const std::string slot    = normalize_material_family(slot_material);
    return !project.empty() && project == slot;
}

inline bool can_use_for_generic_slot(const std::string &preset_id,
                                     const std::string &preset_material,
                                     const std::string &slot_material)
{
    const Identity preset = classify(preset_id);
    return preset.kind != IdentityKind::Malformed &&
           preset.kind != IdentityKind::Branded &&
           material_families_compatible(preset_material, slot_material);
}

inline bool is_generic_system_candidate(const std::string &preset_name,
                                        const std::string &preset_id,
                                        const std::string &preset_material,
                                        const std::string &slot_material)
{
    return preset_name.rfind("Generic ", 0) == 0 &&
           can_use_for_generic_slot(preset_id, preset_material, slot_material);
}

inline Resolution resolve_preset(const SlotRequest &request,
                                 const std::vector<PresetCandidate> &candidates)
{
    const Identity slot = classify(request.id);
    if (slot.kind == IdentityKind::NotBox || slot.kind == IdentityKind::Malformed)
        return {};

    if (slot.kind == IdentityKind::Branded) {
        for (std::size_t index = 0; index < candidates.size(); ++index) {
            const PresetCandidate &candidate = candidates[index];
            if (candidate.compatible && candidate.exact_identity_base &&
                candidate.id == request.id)
                return {ResolutionKind::BrandedExact, index};
        }
        return {};
    }

    if (request.current_candidate && *request.current_candidate < candidates.size()) {
        const PresetCandidate &current = candidates[*request.current_candidate];
        if (current.compatible &&
            can_use_for_generic_slot(current.id, current.material, request.material))
            return {ResolutionKind::RetainedProject, *request.current_candidate};
    }

    for (std::size_t index = 0; index < candidates.size(); ++index) {
        const PresetCandidate &candidate = candidates[index];
        if (candidate.compatible && candidate.system &&
            is_generic_system_candidate(candidate.name, candidate.id,
                                        candidate.material, request.material))
            return {ResolutionKind::GenericSystem, index};
    }
    return {};
}

// Resolve a complete snapshot without exposing a partial result. Callers may
// safely commit the returned plan only when this function succeeds.
inline std::optional<std::vector<Resolution>> resolve_all(
    const std::vector<SlotRequest> &requests,
    const std::vector<PresetCandidate> &candidates)
{
    std::vector<Resolution> plan;
    plan.reserve(requests.size());
    for (const SlotRequest &request : requests) {
        Resolution resolution = resolve_preset(request, candidates);
        if (!resolution)
            return std::nullopt;
        plan.push_back(resolution);
    }
    return plan;
}

inline bool preset_ids_compatible(const std::string &project_preset_id,
                                  const std::string &slot_preset_id)
{
    const Identity slot = classify(slot_preset_id);
    if (slot.kind == IdentityKind::Malformed)
        return false;
    if (slot.kind == IdentityKind::NotBox)
        return classify(project_preset_id).kind == IdentityKind::NotBox;
    if (slot.kind == IdentityKind::Branded) {
        const Identity project = classify(project_preset_id);
        return project.kind == IdentityKind::NotBox || project_preset_id == slot_preset_id;
    }

    const Identity project = classify(project_preset_id);
    return project.kind == IdentityKind::NotBox || project.kind == IdentityKind::Generic;
}

} // namespace Slic3r::QidiBoxFilament

#endif
