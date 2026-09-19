#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

namespace coopstory::bridge {

// Missing means not sampled yet; an engaged empty list means no shop gear.
using HorseComponents = std::optional<std::vector<std::uint32_t>>;
inline constexpr std::size_t kMaximumHorseComponents = 64U;

// Only tack categories may be reconciled; body, mane and tail items are excluded.
[[nodiscard]] inline constexpr bool IsHorseGearCategory(const std::uint32_t category) noexcept {
    switch (category) {
    case 0xBAA7E618U: // Saddles
    case 0xEFB31921U: // Bedrolls
    case 0x17CEB41AU: // Blankets
    case 0xDA6DADCAU: // Stirrups
    case 0x05447332U: // Saddle horns
    case 0x80451C25U: // Saddlebags
    case 0xFACFC3C0U: // Shoes
    case 0xD3500E5DU: // Accessories (masks)
    case 0x94B2E3AFU: // Bridles
    case 0x1530BE1CU: // Saddle lanterns
    case 0xAC106B30U: // Saddle holsters
        return true;
    default:
        return false;
    }
}

[[nodiscard]] inline bool ValidHorseComponents(const HorseComponents& components) {
    if (!components) return true;
    if (components->size() > kMaximumHorseComponents) return false;
    for (auto it = components->begin(); it != components->end(); ++it) {
        if (*it == 0U || std::find(components->begin(), it, *it) != it) return false;
    }
    return true;
}

struct HorseComponentChanges final {
    std::vector<std::uint32_t> remove{};
    std::vector<std::uint32_t> apply{};
};

[[nodiscard]] inline HorseComponentChanges PlanHorseComponentChanges(
    const std::vector<std::uint32_t>& current, const HorseComponents& desired) {
    HorseComponentChanges changes;
    if (!desired || !ValidHorseComponents(desired)) return changes;
    for (const auto component : current) {
        if (std::find(desired->begin(), desired->end(), component) == desired->end())
            changes.remove.push_back(component);
    }
    for (const auto component : *desired) {
        if (std::find(current.begin(), current.end(), component) == current.end())
            changes.apply.push_back(component);
    }
    return changes;
}

} // namespace coopstory::bridge
