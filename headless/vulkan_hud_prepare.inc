    const auto hud = draw_hud ? Eden::GetVulkanHud() : Eden::HudSnapshot{};
    const auto menu_state = Eden::SwitchMenu::view.load(std::memory_order_acquire);
    const auto menu_generation =
        static_cast<Eden::SwitchMenu::Page>(menu_state & 0xffu) == Eden::SwitchMenu::Page::closed
            ? 0u : static_cast<std::uint32_t>(menu_state >> 32);
    const auto menu_ticket = Eden::SwitchMenu::TicketFor(dst, menu_generation);
    if ((hud.width || menu_generation) && !hud_pipeline) {
        const VkPushConstantRange range{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(hud)};
        hud_layout = device.GetLogical().CreatePipelineLayout(VkPipelineLayoutCreateInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &range,
        });
        auto vertex = BuildShader(device, EDEN_HUD_VERT_SPV);
        auto fragment = BuildShader(device, EDEN_HUD_FRAG_SPV);
        hud_pipeline = CreateWrappedCoverageBlendingPipeline(
            device, render_pass, hud_layout, std::tie(vertex, fragment));
    }
    const VkPipeline hud_graphics_pipeline = *hud_pipeline;
    const VkPipelineLayout hud_pipeline_layout = *hud_layout;
