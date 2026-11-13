#if !defined(__PROSPERO__) // not PS5
#elif defined(EDEN_DEV_PROFILE) // PS5 development build
#include "headless/performance.h"
namespace VideoCore { class RendererBase; }
namespace Eden { bool LoadingTick(VideoCore::RendererBase&, bool idle); }
#else // PS5
namespace VideoCore { class RendererBase; }
namespace Eden { bool LoadingTick(VideoCore::RendererBase&, bool idle); }
#endif // PS5
// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <thread>

#ifdef __linux__
#include <sys/prctl.h>
#elif defined(_WIN32)
#include "common/windows/timer_resolution.h"
#endif

#include "common/assert.h"
#include "common/dkb_cpu_trace.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "common/thread.h"
#include "core/core.h"
#include "core/frontend/graphics_context.h"
#include "video_core/control/scheduler.h"
#include "video_core/dma_pusher.h"
#include "video_core/gpu.h"
#include "video_core/gpu_thread.h"
#include "video_core/gpu_split.h"
#include "video_core/nx2_profile.h"
#include "video_core/host1x/host1x.h"
#include "video_core/renderer_base.h"

namespace VideoCommon::GPUThread {

namespace {

// Tells a CPU thread waiting in PushCommand that its command ran.
void SignalCommand(SynchState& state, u64 fence, bool block) {
    state.signaled_fence.store(fence);
    if (block) {
        // We have to lock the write_lock to ensure that the condition_variable wait not get a
        // race between the check and the lock itself.
        std::scoped_lock lk{state.write_lock};
        state.cv.notify_all();
    }
}

} // Anonymous namespace

ThreadManager::ThreadManager(Core::System& system_)
    : system{system_}
{}

#if !defined(__PROSPERO__) // not PS5
ThreadManager::~ThreadManager() = default;
#else // PS5
ThreadManager::~ThreadManager() { Stop(); }
#endif // PS5

#if !defined(__PROSPERO__) // not PS5
void ThreadManager::StartThread(VideoCore::RendererBase& renderer, Core::Frontend::GraphicsContext& context, Tegra::Control::Scheduler& scheduler) {
    rasterizer = renderer.ReadRasterizer();
    thread = std::jthread([&](std::stop_token stop_token) {
#else // PS5
void ThreadManager::RunWorker(VideoCore::RendererBase& renderer, Core::Frontend::GraphicsContext& context, Tegra::Control::Scheduler& scheduler) {
    const auto stop_token = stop_source.get_token();
#endif // PS5
        const Common::DkbCpuTrace::Registration cpu_trace{Common::DkbCpuTrace::Role::GpuCommand};
        const VideoCommon::StutterTrace::ThreadScope stutter_thread{
            VideoCommon::StutterTrace::Thread::Front};
        Common::SetCurrentThreadName("GPU");
        VideoCommon::GpuSplit::MarkGpuThread();
#ifdef __linux__
        // The pending-channel polls below sleep 50 us; the default timer slack adds up to 50 more.
        prctl(PR_SET_TIMERSLACK, 1UL, 0, 0, 0);
#endif
        Common::SetCurrentThreadPriority(Common::ThreadPriority::Critical);
        Common::SetCurrentThreadToPerformanceCores();
        if (Settings::values.pin_gpu_threads.GetValue()) {
            Common::PinCurrentThread(Common::PinnedRole::GpuFront);
        }
        system.RegisterHostThread();

        auto current_context = context.Acquire();
#if !defined(__PROSPERO__) // not PS5
#else // PS5
        unsigned dispatches = 0;
#endif // PS5
        CommandDataContainer next;
        // When a pending channel last moved or a command arrived (see the polling below).
        auto last_progress = std::chrono::steady_clock::now();
        namespace Profile = VideoCommon::Nx2Profile;
        auto& split = VideoCommon::GpuSplit::Get();
        while (!stop_token.stop_requested()) {
            Profile::Report();
            split.MaybeStart(system);
            if (VideoCommon::g_acquire_counting) {
                split.ReportAcquire();
            }
            if (scheduler.HasPendingChannels()) {
                // Suspended channels wait on semaphores the CPU releases: poll them meanwhile.
                bool resumed = false;
                {
                    Profile::Scope scope{Profile::Zone::ResumePending};
                    resumed = scheduler.ResumePending(system.GPU());
                }
                if (resumed) {
                    last_progress = std::chrono::steady_clock::now();
                }
                if (!state.queue.TryPop(next)) {
                    if (!scheduler.HasPendingChannels()) {
                        continue;
                    }
                    // The CPU usually releases the semaphore soon after: poll for half a
                    // millisecond, then in short sleeps. Ocarina of Time waits here ~330 times a
                    // second and its present waits for the work behind them: with 1 ms sleeps the
                    // present waited ~17 ms a frame in Kokiri village, with 50 us ~1 ms. A channel
                    // still waiting after 2 s (menus, loading) goes back to 1 ms sleeps.
                    Profile::Scope scope{Profile::Zone::PendingSleep};
                    split.SettleIdleBack();
                    const auto waited = std::chrono::steady_clock::now() - last_progress;
                    if (waited < std::chrono::microseconds{500}) {
                        std::this_thread::yield();
                    } else if (waited < std::chrono::seconds{2}) {
#ifdef _WIN32
                        // sleep_for(50us) rounds up to Sleep(1) (>= 1 ms) on Windows;
                        // SleepForOneTick uses NtDelayExecution(-1) for one high-res timer tick.
                        Common::Windows::SleepForOneTick();
#else
                        std::this_thread::sleep_for(std::chrono::microseconds{50});
#endif
                    } else {
                        std::this_thread::sleep_for(std::chrono::milliseconds{1});
                    }
                    continue;
                }
            } else {
#if !defined(__PROSPERO__) // not PS5
#elif defined(EDEN_DEV_PROFILE) // PS5 development build
                if (Eden::LoadingTick(renderer, false)) {
                    if (!state.queue.TryPop(next)) {
                        Eden::LoadingTick(renderer, true);
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        continue;
                    }
                } else {
                { ::Eden::Performance::Timer idle_timer(::Eden::Performance::gpu_queue_wait);
#else // PS5
                if (Eden::LoadingTick(renderer, false)) {
                    if (!state.queue.TryPop(next)) {
                        Eden::LoadingTick(renderer, true);
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        continue;
                    }
                } else {
#endif // PS5
                Profile::Scope scope{Profile::Zone::Idle};
                state.queue.PopWait(next, stop_token);
#if !defined(__PROSPERO__) // not PS5
#elif defined(EDEN_DEV_PROFILE) // PS5 development build
                }
                }
#else // PS5
                }
#endif // PS5
            }
            if (stop_token.stop_requested()) {
                break;
            }
            if (split.Active()) {
                // Only command lists are parsed here; everything else runs on the back thread
                // in order, and so does the signal that the command ran.
                auto* const rasterizer_ptr = renderer.ReadRasterizer();
                if (auto* submit_list = std::get_if<SubmitListCommand>(&next.data)) {
                    Profile::Scope scope{Profile::Zone::Push};
                    scheduler.Push(system.GPU(), submit_list->channel, std::move(submit_list->entries));
                } else if (std::holds_alternative<GPUTickCommand>(next.data)) {
                    // TickWork ends fast draw reuse itself when it runs a request.
                    split.Defer([gpu = &system.GPU()] { gpu->TickWork(); },
                                VideoCommon::GpuSplit::FastDrawNeutral);
                } else if (auto* run = std::get_if<RunCommand>(&next.data)) {
                    if (run->before_later_reads) {
                        // GPU memory maps unmap caches (UnmapMemory downloads): the back's state.
                        split.DeferFunction(std::move(run->func));
                        split.Drain(VideoCommon::DrainSite::Map);
                    } else {
                        split.DeferFunction(std::move(run->func));
                    }
                } else if (const auto* flush = std::get_if<FlushRegionCommand>(&next.data)) {
                    split.Defer([rasterizer_ptr, addr = flush->addr, size = flush->size] {
                        rasterizer_ptr->FlushRegion(addr, size);
                    });
                } else if (const auto* invalidate = std::get_if<InvalidateRegionCommand>(&next.data)) {
                    split.Defer([rasterizer_ptr, addr = invalidate->addr, size = invalidate->size] {
                        rasterizer_ptr->OnCacheInvalidation(addr, size);
                    });
                } else {
                    ASSERT(false);
                }
                split.Defer([state_ptr = &state, fence = next.fence, block = next.block] {
                    SignalCommand(*state_ptr, fence, block);
                }, VideoCommon::GpuSplit::FastDrawNeutral);
                split.Publish();
                last_progress = std::chrono::steady_clock::now();
                continue;
            }
            if (auto* submit_list = std::get_if<SubmitListCommand>(&next.data)) {
#if !defined(__PROSPERO__) // not PS5
#elif defined(EDEN_DEV_PROFILE) // PS5 development build
                const auto count = ++dispatches;
                if (count <= 3 || ::Eden::Performance::BootTrace()) LOG_INFO(Render_OpenGL, "EDEN_GPU_DISPATCH_BEGIN count={}", count);
                { ::Eden::Performance::Timer dispatch_timer(::Eden::Performance::gpu_dispatch);
#else // PS5
                const auto count = ++dispatches;
                if (count <= 3) LOG_INFO(Render_OpenGL, "EDEN_GPU_DISPATCH_BEGIN count={}", count);
#endif // PS5
                Profile::Scope scope{Profile::Zone::Push};
                scheduler.Push(system.GPU(), submit_list->channel, std::move(submit_list->entries));
#if !defined(__PROSPERO__) // not PS5
#elif defined(EDEN_DEV_PROFILE) // PS5 development build
                }
                if (count <= 3 || ::Eden::Performance::BootTrace()) LOG_INFO(Render_OpenGL, "EDEN_GPU_DISPATCH_END count={}", count);
#else // PS5
                if (count <= 3) LOG_INFO(Render_OpenGL, "EDEN_GPU_DISPATCH_END count={}", count);
#endif // PS5
            } else if (std::holds_alternative<GPUTickCommand>(next.data)) {
                Profile::Scope scope{Profile::Zone::Tick};
                system.GPU().TickWork();
            } else if (auto* run = std::get_if<RunCommand>(&next.data)) {
                Profile::Scope scope{Profile::Zone::Run};
                run->func();
            } else if (const auto* flush = std::get_if<FlushRegionCommand>(&next.data)) {
                Profile::Scope scope{Profile::Zone::FlushInvalidate};
                renderer.ReadRasterizer()->FlushRegion(flush->addr, flush->size);
            } else if (const auto* invalidate = std::get_if<InvalidateRegionCommand>(&next.data)) {
                Profile::Scope scope{Profile::Zone::FlushInvalidate};
                renderer.ReadRasterizer()->OnCacheInvalidation(invalidate->addr, invalidate->size);
            } else {
                ASSERT(false);
            }
            SignalCommand(state, next.fence, next.block);
            last_progress = std::chrono::steady_clock::now();
        }
#if !defined(__PROSPERO__) // not PS5
    });
#else // PS5
#endif // PS5
}

void ThreadManager::SubmitList(s32 channel, Tegra::CommandList&& entries, bool is_async) {
    PushCommand(SubmitListCommand(channel, std::move(entries)), false, is_async);
}

void ThreadManager::FlushRegion(DAddr addr, u64 size, bool is_async) {
    if (!is_async) {
        // Always flush with synchronous GPU mode
        PushCommand(FlushRegionCommand(addr, size), false, is_async);
    }
}

void ThreadManager::TickGPU(bool is_async) {
    PushCommand(GPUTickCommand(), false, is_async);
}

void ThreadManager::Run(std::function<void()>&& func, bool is_async, bool before_later_reads) {
    PushCommand(RunCommand{std::move(func), before_later_reads}, false, is_async);
}

void ThreadManager::InvalidateRegion(DAddr addr, u64 size) {
    rasterizer->OnCacheInvalidation(addr, size);
}

void ThreadManager::FlushAndInvalidateRegion(DAddr addr, u64 size, bool is_async) {
    if (Settings::IsGPULevelHigh()) {
        if (!is_async) {
            PushCommand(FlushRegionCommand(addr, size), false, is_async);
        } else {
            auto& gpu = system.GPU();
            const u64 fence = gpu.RequestFlush(addr, size);
            TickGPU(is_async);
            gpu.WaitForSyncOperation(fence);
        }
    }
    rasterizer->OnCacheInvalidation(addr, size);
}

u64 ThreadManager::PushCommand(CommandData&& command_data, bool block, bool is_async) {
    if (!is_async) {
        // In synchronous GPU mode, block the caller until the command has executed
        block = true;
    }

    std::unique_lock lk(state.write_lock);
    const u64 fence{++state.last_fence};
#if !defined(__PROSPERO__) // not PS5
    state.queue.EmplaceWait(std::move(command_data), fence, block);
#elif defined(EDEN_DEV_PROFILE) // PS5 development build
    if (stop_source.stop_requested() ||
        !state.queue.TryEmplace(std::move(command_data), fence, block)) {
        ::Eden::Performance::Timer full_timer(::Eden::Performance::gpu_queue_full);
    while (!stop_source.stop_requested() &&
           !state.queue.TryEmplace(std::move(command_data), fence, block))
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    if (stop_source.stop_requested())
        return state.signaled_fence.load(std::memory_order_relaxed);
#else // PS5
    while (!stop_source.stop_requested() &&
           !state.queue.TryEmplace(std::move(command_data), fence, block))
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    if (stop_source.stop_requested())
        return state.signaled_fence.load(std::memory_order_relaxed);
#endif // PS5

    if (block) {
#if !defined(__PROSPERO__) // not PS5
        state.cv.wait(lk, thread.get_stop_token(), [this, fence] {
#else // PS5
        state.cv.wait(lk, stop_source.get_token(), [this, fence] {
#endif // PS5
            return fence <= state.signaled_fence.load(std::memory_order_relaxed);
        });
    }

    return fence;
}

void ThreadManager::NotifyShutdown() {
#if !defined(__PROSPERO__) // not PS5
    if (thread.joinable()) {
        thread.request_stop();
        thread.join();
    }
#else // PS5
    Stop();
#endif // PS5
    VideoCommon::GpuSplit::Get().Stop();
}

} // namespace VideoCommon::GPUThread

#if !defined(__PROSPERO__) // not PS5
#else // PS5
// SPDX-License-Identifier: GPL-3.0-or-later
// PS5-only ThreadManager lifecycle; RunWorker retains Eden's worker body.
#include <memory>
#include <cstdio>
#include <system_error>
#include "headless/diagnostics.h"
#include "headless/gpu_failure.h"

namespace VideoCommon::GPUThread {
void ThreadManager::StartThread(VideoCore::RendererBase& renderer,
        Core::Frontend::GraphicsContext& context, Tegra::Control::Scheduler& scheduler) {
    Stop();
    (void)Eden::TakeGpuFailure();
    stop_source = std::stop_source{};
    rasterizer = renderer.ReadRasterizer();
    const auto check = [](int result) {
        if (result) throw std::system_error(result, std::generic_category(), "PS5 GPU worker");
    };
    pthread_attr_t attributes;
    check(pthread_attr_init(&attributes));
    struct DestroyAttributes {
        pthread_attr_t* attributes;
        ~DestroyAttributes() {
            if (pthread_attr_destroy(attributes)) std::terminate();
        }
    } cleanup{&attributes};
    check(pthread_attr_setstacksize(&attributes, 1024 * 1024));
    struct StartData {
        ThreadManager* owner;
        VideoCore::RendererBase& renderer;
        Core::Frontend::GraphicsContext& context;
        Tegra::Control::Scheduler& scheduler;
    };
    auto data = std::make_unique<StartData>(StartData{this, renderer, context, scheduler});
    check(pthread_create(&thread, &attributes, [](void* opaque) noexcept -> void* {
        const std::unique_ptr<StartData> start{static_cast<StartData*>(opaque)};
        try {
            start->owner->RunWorker(start->renderer, start->context, start->scheduler);
        } catch (const std::exception& error) {
            Eden::Report("GPU worker", error.what());
            Eden::RecordGpuFailure(std::current_exception());
            start->owner->stop_source.request_stop();
        } catch (...) {
            Eden::Report("GPU worker", "Unknown exception");
            Eden::RecordGpuFailure(std::current_exception());
            start->owner->stop_source.request_stop();
        }
        return nullptr;
    }, data.get()));
    data.release();
    thread_joinable = true;
    std::puts("EDEN_GPU_WORKER_STACK bytes=1048576");
}

void ThreadManager::Stop() {
    stop_source.request_stop();
    if (thread_joinable) {
        if (pthread_join(thread, nullptr)) std::terminate();
        thread_joinable = false;
    }
}
} // namespace VideoCommon::GPUThread

#endif // PS5