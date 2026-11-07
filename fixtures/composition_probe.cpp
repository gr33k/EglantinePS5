#include "../headless/hud.h"
#include <cassert>
int main() {
    using namespace Eden::CompositionProbe;
    Observe(1, true, 1920, 1080, 1920);
    Observe(0, false, 0, 0, 0);
    Observe(2, false, 0, 1080, 0);
    const auto first = Drain();
    assert(first.frames == 3 && first.empty == 1 && first.invalid == 1 && first.multi == 1);
    assert(first.last_layers == 2 && first.width == 0 && first.height == 1080 && first.stride == 0);
    const auto second = Drain();
    assert(second.frames == 0 && second.empty == 0 && second.invalid == 0 && second.multi == 0);
    Observe(1, true, 1280, 720, 1280);
    const auto third = Drain();
    assert(third.frames == 1 && third.empty == 0 && third.invalid == 0 && third.multi == 0);
    assert(third.last_layers == 1 && third.width == 1280 && third.height == 720);
}
