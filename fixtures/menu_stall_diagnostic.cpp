#include "../headless/switch_menu_view.h"
#include <cassert>
using namespace emuhub_switch_menu;
int main() {
    State menu;
    assert(menu.consume(touchpad | options, false) == Action::opened);
    const auto generation = Eden::SwitchMenu::Publish(menu.page(), menu.choice());
    assert(!Eden::SwitchMenu::Visible(generation));
    assert(menu.consume(0, false) == Action::none);
    assert(menu.consume(down, false) == Action::none);
    assert(menu.choice() == Choice::resume);
    assert(menu.consume(cross, false) == Action::none);
    assert(menu.page() == Page::choices);
    Eden::SwitchMenu::MarkPresented(generation);
    assert(Eden::SwitchMenu::Visible(generation));
    menu.consume(0, true);
    menu.consume(down, true);
    assert(menu.choice() == Choice::return_to_emuhub);
    menu.consume(0, true);
    assert(menu.consume(cross, true) == Action::none);
    assert(menu.page() == Page::confirm_return);
    menu.consume(0, true);
    assert(menu.consume(circle, true) == Action::none);
    assert(menu.page() == Page::choices);
    assert(menu.choice() == Choice::resume);
    menu.consume(0, true);
    assert(menu.consume(circle, true) == Action::resumed);
    assert(menu.page() == Page::closed);
}
