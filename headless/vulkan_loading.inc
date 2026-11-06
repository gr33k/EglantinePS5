// Runs on the existing GPU worker, before the first guest Composite. Reuse its
// swapchain and composition pass without guest captures or FPS notifications.
void RendererVulkan::PresentLoading() {
    const auto menu_state = Eden::SwitchMenu::view.load(std::memory_order_acquire);
    const auto menu_generation =
        static_cast<Eden::SwitchMenu::Page>(menu_state & 0xffu) == Eden::SwitchMenu::Page::closed
            ? 0u : static_cast<std::uint32_t>(menu_state >> 32);
    Frame* frame = present_manager.GetRenderFrame();
    const auto menu_ticket = Eden::SwitchMenu::BeginPresentation(frame, menu_generation);
    scheduler.RequestOutsideRenderPassOperationContext();
    blit_swapchain.DrawToFrame(device, rasterizer, frame, {},
                              render_window.GetFramebufferLayout(), swapchain.GetImageCount(),
                              swapchain.GetImageViewFormat(), true);
    scheduler.Flush(*frame->render_ready);
    present_manager.Present(frame);
    if (Eden::SwitchMenu::FullyDrawn(menu_ticket, menu_generation))
        Eden::SwitchMenu::MarkPresented(menu_generation);
    Eden::SwitchMenu::EndPresentation(menu_ticket);
}
