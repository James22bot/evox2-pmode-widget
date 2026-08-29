#pragma once

#include "evox2/core.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace evox2::overlay {

inline constexpr unsigned int kRefreshIntervalMilliseconds = 2500;
inline constexpr int kWidgetWidthLogicalPixels = 132;
inline constexpr int kWidgetHeightLogicalPixels = 24;
inline constexpr int kWidgetFontPoints = 10;
inline constexpr int kOverlayMarginLogicalPixels = 6;

struct Rgb {
    std::uint8_t red;
    std::uint8_t green;
    std::uint8_t blue;

    auto operator<=>(const Rgb&) const = default;
};

struct Rectangle {
    int left;
    int top;
    int right;
    int bottom;
};

struct Dimensions {
    int width;
    int height;
};

struct Position {
    int x;
    int y;

    auto operator<=>(const Position&) const = default;
};

[[nodiscard]] Position top_right_position(
    const Rectangle& work_area,
    const Dimensions& overlay,
    int margin);

class OverlayModel final {
public:
    OverlayModel();

    bool set_mode(PMode mode);
    bool set_unavailable(std::string reason);

    [[nodiscard]] bool available() const noexcept;
    [[nodiscard]] std::string_view text() const noexcept;
    [[nodiscard]] Rgb color() const noexcept;
    [[nodiscard]] const std::string& detail() const noexcept;
    [[nodiscard]] std::string tray_tooltip() const;

private:
    bool available_ = false;
    PMode mode_ = PMode::Balanced;
    std::string text_;
    Rgb color_ {};
    std::string detail_;
};

} // namespace evox2::overlay
