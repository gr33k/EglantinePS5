#pragma once
#include <cstdint>

// Input-only state machine for the PS5 Switch provider's EmuHub in-game menu.
// The caller renders the state and captures guest inputs while open. The
// GPU producer keeps running; no chord directly requests game shutdown.
namespace emuhub_switch_menu {
enum Button : std::uint32_t {
    touchpad = 1u << 0, options = 1u << 1,
    up = 1u << 2, down = 1u << 3,
    cross = 1u << 4, circle = 1u << 5,
};
enum class Page { closed, choices, confirm_return };
enum class Choice { resume, return_to_emuhub };
enum class Action { none, opened, resumed, exit_confirmed };
class State {
public:
    Page page() const { return page_; }
    Choice choice() const { return choice_; }
    bool captures_game_input() const { return page_ != Page::closed; }
    void reset() { page_ = Page::closed; choice_ = Choice::resume; prior_ = 0; }
    Action consume(std::uint32_t buttons, bool visible = true) {
        const std::uint32_t rising = buttons & ~prior_;
        const bool chord = (buttons & (touchpad | options)) == (touchpad | options);
        const bool prior_chord = (prior_ & (touchpad | options)) == (touchpad | options);
        prior_ = buttons;
        if (chord && !prior_chord && page_ == Page::closed) {
            page_ = Page::choices;
            choice_ = Choice::resume;
            return Action::opened;
        }
        if (page_ == Page::closed || chord || !visible) return Action::none;
        if (rising & circle) {
            if (page_ == Page::confirm_return) {
                page_ = Page::choices;
                choice_ = Choice::resume;
                return Action::none;
            }
            page_ = Page::closed;
            return Action::resumed;
        }
        if (page_ == Page::confirm_return) {
            if (rising & cross) {
                page_ = Page::closed;
                return Action::exit_confirmed;
            }
            return Action::none;
        }
        if (rising & (up | down))
            choice_ = choice_ == Choice::resume ? Choice::return_to_emuhub : Choice::resume;
        if (rising & cross) {
            if (choice_ == Choice::resume) {
                page_ = Page::closed;
                return Action::resumed;
            }
            page_ = Page::confirm_return;
        }
        return Action::none;
    }
private:
    Page page_ = Page::closed;
    Choice choice_ = Choice::resume;
    std::uint32_t prior_ = 0;
};
} // namespace emuhub_switch_menu
