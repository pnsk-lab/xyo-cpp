// Scratch graphics effects: seven fixed, allocation-free values per Target.
// Unknown extension-defined keys retain the legacy std::map semantics.
#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <cstddef>
#include <map>
#include <string>
#include <string_view>

namespace xyo {

struct GraphicEffects {
    static constexpr std::array<std::string_view, 7> names{
        "color", "fisheye", "whirl", "pixelate",
        "mosaic", "brightness", "ghost"
    };
    std::array<double, names.size()> values{};
    // The standard seven keys start present, like the former std::map.
    // Tests and embedders may erase/reinsert keys; preserve that behaviour.
    uint8_t present = 0x7f;
    // Most targets have no custom keys. Keep the legacy behaviour for
    // embedders that insert a nonstandard effect name.
    std::map<std::string, double, std::less<>> custom;

    static constexpr std::size_t index(std::string_view name) noexcept {
        for (std::size_t i = 0; i < names.size(); ++i)
            if (names[i] == name) return i;
        return names.size();
    }

    double& operator[](std::string_view name) {
        const std::size_t i = index(name);
        if (i < values.size()) {
            present |= uint8_t(1u << i);
            return values[i];
        }
        return custom[std::string(name)];
    }

    double get(std::string_view name) const {
        const std::size_t i = index(name);
        if (i < values.size())
            return (present & uint8_t(1u << i)) ? values[i] : 0.0;
        auto it = custom.find(name);
        return it == custom.end() ? 0.0 : it->second;
    }

    std::size_t count(std::string_view name) const {
        const std::size_t i = index(name);
        return i < names.size()
            ? (present & uint8_t(1u << i) ? 1 : 0)
            : custom.count(name);
    }
    std::size_t erase(std::string_view name) {
        const std::size_t i = index(name);
        if (i < names.size()) {
            const uint8_t bit = uint8_t(1u << i);
            const bool wasPresent = (present & bit) != 0;
            present &= uint8_t(~bit);
            values[i] = 0.0;
            return wasPresent ? 1 : 0;
        }
        return custom.erase(std::string(name));
    }
    std::size_t size() const noexcept {
        return std::popcount(static_cast<unsigned>(present)) + custom.size();
    }

    void clearValues() {
        values.fill(0.0);
        for (auto& [key, value] : custom) value = 0.0;
    }
    bool anyActive() const {
        for (std::size_t i = 0; i < values.size(); ++i)
            if ((present & uint8_t(1u << i)) && values[i] != 0.0)
                return true;
        for (const auto& [key, value] : custom)
            if (value != 0.0) return true;
        return false;
    }
    bool operator==(const GraphicEffects&) const = default;
};

} // namespace xyo
