// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string_view>
#include "ps5_switch_safe_menu.hpp"

namespace Eden::SwitchMenu {
using emuhub_switch_menu::Page;
using emuhub_switch_menu::Choice;
// One writer (the controller poller), one or more renderer readers.
inline std::atomic<std::uint64_t> view{0};
inline std::atomic<std::uint32_t> presented{0};
inline std::uint32_t generation = 0;
inline std::uint32_t Publish(Page page, Choice choice) {
    const auto next = ++generation;
    const auto bits = (std::uint64_t(next) << 32) |
                      (std::uint64_t(choice == Choice::return_to_emuhub) << 8) |
                      std::uint64_t(page);
    view.store(bits, std::memory_order_release);
    return next;
}
inline std::uint32_t Generation() {
    return static_cast<std::uint32_t>(view.load(std::memory_order_acquire) >> 32);
}
inline Page CurrentPage() {
    return static_cast<Page>(view.load(std::memory_order_acquire) & 0xffu);
}
inline bool Open() { return CurrentPage() != Page::closed; }
inline bool Visible(std::uint32_t wanted) {
    return wanted != 0 && presented.load(std::memory_order_acquire) == wanted;
}
inline void MarkPresented(std::uint32_t drawn) {
    if (drawn != 0 && Generation() == drawn)
        presented.store(drawn, std::memory_order_release);
}
inline std::string_view Line(std::uint64_t state, unsigned row) {
    if (static_cast<Page>(state & 0xffu) == Page::confirm_return) {
        constexpr std::string_view lines[] = {
            "EMUHUB  RETURN TO EMUHUB",
            "UNSAVED PROGRESS MAY BE LOST",
            "CROSS  CONFIRM RETURN",
            "CIRCLE  CANCEL",
        };
        return row < 4 ? lines[row] : std::string_view{};
    }
    const bool returning = (state & 0x100u) != 0;
    switch (row) {
    case 0: return "EMUHUB  GAME CONTINUES";
    case 1: return returning ? "RESUME" : "RESUME  SELECTED";
    case 2: return returning ? "RETURN TO WHEEL  SELECTED" : "RETURN TO WHEEL";
    case 3: return "CROSS  SELECT       CIRCLE  RESUME";
    default: return {};
    }
}
inline constexpr unsigned kRows = 4;
inline constexpr unsigned kWidth = 800;
inline constexpr unsigned kHeight = 40;
inline constexpr unsigned kX = 560;
inline constexpr unsigned kY = 440;
inline constexpr unsigned kStep = 46;

// OpenGL returns the generation only if every native row was actually drawn.
// The returned generation is checked again after the surface swap.
template <class DrawRow>
inline std::uint32_t DrawAllRows(std::uint64_t state, DrawRow draw_row) {
    if (static_cast<Page>(state & 0xffu) == Page::closed) return 0;
    for (unsigned row = 0; row < kRows; ++row)
        if (!draw_row(row, Line(state, row))) return 0;
    return static_cast<std::uint32_t>(state >> 32);
}

// Vulkan uses the same height-based integer scaling as vulkan_hud_draw.inc.
// Reject a missing allocator, a zero-sized scissor, or any clipped row before
// issuing a draw. The OpenGL path separately checks each actual DrawHud result.
inline bool FitsVulkanMenu(std::uint32_t width, std::uint32_t height, bool hud_memory) {
    if (!hud_memory || width == 0 || height == 0) return false;
    const auto units = [height](std::uint32_t value) {
        return std::uint64_t(value) * height / 1080u;
    };
    const auto x = units(kX), y = units(kY + (kRows - 1) * kStep);
    const auto w = units(kWidth), h = units(kHeight);
    return w > 0 && h > 0 && x + w <= width && y + h <= height;
}

// The Vulkan loading presenter issues a unique ticket for its exact frame
// before DrawToFrame. WindowAdaptPass captures that ticket while scheduling the
// draw; only its complete four-row command buffer may redeem it. Recycled
// Frame pointers and late callbacks from older presentations cannot do so.
struct PresentationTicket {
    const void* frame = nullptr;
    std::uint32_t generation = 0;
    std::uint64_t id = 0;
};
inline std::mutex draw_receipt_mutex;
inline PresentationTicket pending_presentation;
inline std::uint64_t next_ticket = 0;
inline std::uint64_t full_draw_ticket = 0;
inline std::uint64_t BeginPresentation(const void* frame, std::uint32_t generation) {
    if (!frame || !generation || Generation() != generation) return 0;
    const std::scoped_lock lock(draw_receipt_mutex);
    if (Generation() != generation) return 0;
    pending_presentation = {frame, generation, ++next_ticket};
    return next_ticket;
}
inline std::uint64_t TicketFor(const void* frame, std::uint32_t generation) {
    const std::scoped_lock lock(draw_receipt_mutex);
    return frame && generation && pending_presentation.frame == frame &&
                   pending_presentation.generation == generation ? pending_presentation.id : 0;
}
inline void RecordFullDraw(std::uint64_t ticket, std::uint32_t generation) {
    if (!ticket || !generation || Generation() != generation) return;
    const std::scoped_lock lock(draw_receipt_mutex);
    if (Generation() == generation && pending_presentation.id == ticket &&
        pending_presentation.generation == generation)
        full_draw_ticket = ticket;
}
inline bool FullyDrawn(std::uint64_t ticket, std::uint32_t generation) {
    const std::scoped_lock lock(draw_receipt_mutex);
    return ticket && generation && Generation() == generation &&
           pending_presentation.id == ticket && full_draw_ticket == ticket &&
           pending_presentation.generation == generation;
}
inline void EndPresentation(std::uint64_t ticket) {
    const std::scoped_lock lock(draw_receipt_mutex);
    if (ticket && pending_presentation.id == ticket) pending_presentation = {};
}
} // namespace Eden::SwitchMenu
