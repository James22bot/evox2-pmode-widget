#include "evox2/overlay_model.hpp"

#include <algorithm>
#include <utility>

namespace evox2::overlay {
namespace {

constexpr Rgb kQuietColor {80, 210, 120};
constexpr Rgb kBalancedColor {245, 180, 55};
constexpr Rgb kPerformanceColor {245, 85, 85};
constexpr Rgb kUnavailableColor {150, 155, 165};

Rgb color_for_mode(PMode mode)
{
    switch (mode) {
    case PMode::Quiet:
        return kQuietColor;
    case PMode::Balanced:
        return kBalancedColor;
    case PMode::Performance:
        return kPerformanceColor;
    }
    throw UnsupportedHardwareError("Ungueltiger P-MODE fuer Overlay-Farbe.");
}

} // namespace

Position top_right_position(const Rectangle& work_area, const Dimensions& overlay, int margin)
{
    const int safe_margin = std::max(0, margin);
    const int width = std::max(0, overlay.width);
    const int minimum_x = work_area.left + safe_margin;
    const int minimum_y = work_area.top + safe_margin;
    return Position {
        .x = std::max(minimum_x, work_area.right - width - safe_margin),
        .y = minimum_y,
    };
}

OverlayModel::OverlayModel()
    : text_("P-MODE: --"), color_(kUnavailableColor), detail_("Noch kein erfolgreicher EC-Read.")
{
}

bool OverlayModel::set_mode(PMode mode)
{
    const bool changed = !available_ || mode_ != mode;
    available_ = true;
    mode_ = mode;
    text_ = "P-MODE: " + std::string(mode_name(mode));
    color_ = color_for_mode(mode);
    detail_.clear();
    return changed;
}

bool OverlayModel::set_unavailable(std::string reason)
{
    const bool changed = available_ || detail_ != reason;
    available_ = false;
    text_ = "P-MODE: --";
    color_ = kUnavailableColor;
    detail_ = std::move(reason);
    return changed;
}

bool OverlayModel::available() const noexcept
{
    return available_;
}

std::optional<PMode> OverlayModel::observed_mode() const noexcept
{
    return available_ ? std::optional<PMode>(mode_) : std::nullopt;
}

std::string_view OverlayModel::text() const noexcept
{
    return text_;
}

Rgb OverlayModel::color() const noexcept
{
    return color_;
}

const std::string& OverlayModel::detail() const noexcept
{
    return detail_;
}

std::string OverlayModel::tray_tooltip() const
{
    return available_ ? "EVO-X2 P-MODE: " + std::string(mode_name(mode_)) : "EVO-X2 P-MODE: unavailable";
}

} // namespace evox2::overlay
