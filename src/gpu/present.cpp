/**
 * @file    gpu/present.cpp
 * @brief   End of frame: the deferred clear, the swapchain
 * acquire/blit/present, and the pre-Runtime overlay-only present the installer
 * uses.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/frame.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <mutex>
#include <thread>

#include <plume_render_interface.h>
#if !defined(REBLUE_D3D12)
#include <plume_vulkan.h>
#endif

#include "gpu/gpu_profiling.h"

#include "core/logging.h"
#include "engine/engine.h"
#include "gpu/backend.h"
#include "gpu/constant_buffers.h"
#include "gpu/output.h"
#include "gpu/settings.h"
#include "platform/android_lifecycle.h"
#include "platform/android_perf.h"

namespace bd::gpu {

namespace {

// setVsyncEnabled() only records the *required* present mode; createdPresentMode
// follows on the next resize, and VulkanSwapChain::needsResize() compares the
// two. Calling it every frame therefore made needsResize() permanently true and
// rebuilt the swap chain on each present: a device idle, a fresh set of images
// and a full pipeline flush per frame, which is the "present" time and the
// multi-second worst frames in the logs. Only push it when the setting moves.
void ApplyVsync(VideoState &s) {
  if (!s.swap_chain)
    return;
  static bool applied = false;
  static bool last = false;
  const bool want = Settings::Get().Vsync();
  if (applied && want == last)
    return;
  applied = true;
  last = want;
  s.swap_chain->setVsyncEnabled(want);
}

constexpr i32 kIdleFPS = 30;

// Only Android takes the window surface away from under a live device.
#if defined(__ANDROID__)
constexpr bool kSurfaceCanVanish = true;
#else
constexpr bool kSurfaceCanVanish = false;
#endif

void PaceFrame(bool idle = false) {
  using Clock = std::chrono::steady_clock;
  static Clock::time_point next{};
  i32 fps = bd::engine::TargetFPS();
  if (bd::engine::SofdecPlayer::Playing())
    fps = 30;
  // A held cutscene is a still image by definition, so the target rate is
  // wasted frames and heat. Only ever *lowers* the target, so a tuned cap the
  // user already set below 30 still wins.
  if (bd::engine::CutscenePause::Get().Frozen() && (fps <= 0 || fps > kIdleFPS))
    fps = kIdleFPS;
  if (idle && (fps <= 0 || fps > kIdleFPS))
    fps = kIdleFPS;
  // Not while idle: there is no surface then, or only the installer on it.
  if (!idle)
    bd::platform::android::ApplyDisplayFrameRate(fps);
  if (fps <= 0) {
    next = {};
    return;
  }
  const auto period = std::chrono::duration_cast<Clock::duration>(
      std::chrono::duration<double, std::milli>(1000.0 / fps));
  const auto now = Clock::now();
  if (next.time_since_epoch().count() != 0 && now < next) {
    std::this_thread::sleep_until(next);
    next += period;
  } else {
    next = now + period;
  }
}

// Where each presented frame's time went, summed over the perf window: the
// wait for the swapchain image (display/vsync bound), for the frame slot the
// GPU still holds (GPU bound), and in vkQueuePresent. The rest of the frame
// is the guest and the draw recording on the CPU.
struct FrameWaits {
  double acquire_ms = 0, gpu_ms = 0, present_ms = 0;
  // The fps cap's sleep (and the 30 fps one during FMVs), which is not CPU
  // work either.
  double pace_ms = 0;
};
FrameWaits g_waits;

double TotalWaitsMs() {
  return g_waits.acquire_ms + g_waits.gpu_ms + g_waits.present_ms;
}

// When the current frame's work began: the end of the previous frame's pacing
// sleep. Zero until a frame has been presented, and again after the
// background, so the first frame back does not report the whole trip.
std::chrono::steady_clock::time_point g_frame_begin{};

class WaitTimer {
public:
  explicit WaitTimer(double &sum)
      : sum_(sum), start_(std::chrono::steady_clock::now()) {}
  ~WaitTimer() {
    sum_ += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start_)
                .count();
  }

private:
  double &sum_;
  std::chrono::steady_clock::time_point start_;
};

// How many render passes the frames began and the pixels they covered: on a
// tiler every pass loads and stores its attachments, so this is the memory
// traffic the GPU pays before it shades anything.
std::string PassSummary(u32 frames) {
#if defined(REBLUE_D3D12)
  (void)frames;
  return {};
#else
  const plume::VulkanPassStats stats = plume::takeVulkanPassStats();
  return std::format(" | per frame {:.0f} passes, {:.1f} Mpix, {:.0f} draws",
                     double(stats.passes) / frames,
                     double(stats.pixels) / frames / 1e6,
                     double(stats.draws) / frames);
#endif
}

// One log line every kPerfWindow with the presented frame rate and the worst
// frame, so a slow device leaves numbers behind instead of an impression.
void LogFrameRate() {
  using Clock = std::chrono::steady_clock;
  constexpr auto kPerfWindow = std::chrono::seconds(10);
  static Clock::time_point window_start = Clock::now();
  static Clock::time_point last = window_start;
  static u32 frames = 0;
  static double worst_ms = 0.0;
  const auto now = Clock::now();
  worst_ms = std::max(
      worst_ms, std::chrono::duration<double, std::milli>(now - last).count());
  last = now;
  ++frames;
  if (now - window_start < kPerfWindow)
    return;
  const double seconds =
      std::chrono::duration<double>(now - window_start).count();
  const double frame_ms = seconds * 1000.0 / frames;
  const double acquire = g_waits.acquire_ms / frames;
  const double gpu = g_waits.gpu_ms / frames;
  const double present = g_waits.present_ms / frames;
  const double pace = g_waits.pace_ms / frames;
  // The GPU's own time per frame from the timestamps. Next to cpu it says
  // whether the two overlap (frame ~ max of them) or run back to back
  // (frame ~ their sum).
  auto &s = state();
  const double gpu_busy =
      s.gpu_busy_frames ? s.gpu_busy_ms / s.gpu_busy_frames : 0.0;
  s.gpu_busy_ms = 0.0;
  s.gpu_busy_frames = 0;
  BD_INFO("[perf] {:.1f} fps over {:.0f}s, worst frame {:.1f} ms | per frame "
          "{:.1f} ms: cpu {:.1f}, gpu busy {:.1f}, gpu wait {:.1f}, image "
          "wait {:.1f}, present {:.1f}, pacing {:.1f} (fps cap {}, vsync "
          "{}){}{}",
          frames / seconds, seconds, worst_ms, frame_ms,
          frame_ms - acquire - gpu - present - pace, gpu_busy, gpu, acquire,
          present, pace,
          bd::engine::TargetFPS(), Settings::Get().Vsync(),
          PassSummary(frames), bd::platform::android::ThermalSummary());
  g_waits = {};
  window_start = now;
  frames = 0;
  worst_ms = 0.0;
}

void AbandonFrame(VideoState &s, std::unique_lock<std::mutex> &lock) {
  s.frame_present_committed = true;
  if (!s.command_list_open)
    return;
  SubmitOpenListLocked(s);
  AdvanceAndWaitReused(s);
  const u32 reclaimed = s.frame.load(std::memory_order_relaxed);
  lock.unlock();
  DrainSlot(s, reclaimed);
}

void RebuildSwapChain(VideoState &s) {
  SubmitOpenListLocked(s);
  for (u32 i = 0; i < kNumFrames; ++i) {
    if (s.command_list_submitted[i]) {
      s.queue->waitForCommandFence(s.fences[i].get());
      s.command_list_submitted[i] = false;
    }
  }
  s.framebuffers.clear();
  if (!s.swap_chain->resize() || !BuildFramebuffers(s) ||
      !BuildPresentSemaphores(s)) {
    if (s.swap_chain->getWidth() && s.swap_chain->getHeight())
      BD_ERROR("Swap chain resize failed");
    return;
  }
  LogSwapChain(s);
  if (static_cast<AspectMode>(Settings::Get().AspectRatio()) ==
      AspectMode::Auto)
    Output::Recompute();
}

GuestTexture *SelectPresentSource(VideoState &s, GuestTexture *frontBuffer) {
  GuestTexture *last_rt = s.last_drawn_rt;
  GuestTexture *rt = (s.back_buffer_surface && last_rt == s.back_buffer_surface)
                         ? s.back_buffer_surface
                     : (frontBuffer && frontBuffer->texture) ? frontBuffer
                     : last_rt                               ? last_rt
                     : s.last_resolved_dst ? s.last_resolved_dst
                                           : s.back_buffer_surface;
  if (rt && rt->sourceSurface && rt->sourceSurface != rt &&
      rt->sourceSurface->texture &&
      rt->sourceSurface->sampleCount == plume::RenderSampleCount::COUNT_1 &&
      rt->sourceSurface->descriptorIndex != kInvalidDescriptorIndex) {
    rt = rt->sourceSurface;
  }
  return rt;
}

void RecordPresentPass(VideoState &s, GuestTexture *rt,
                       plume::RenderTexture *back,
                       plume::RenderFramebuffer *back_fb) {
  BD_GPU_ZONE("RecordPresentPass");
  if (rt->layout != plume::RenderTextureLayout::SHADER_READ) {
    const bool needs_discard =
        (rt->layout == plume::RenderTextureLayout::UNKNOWN);
    if (needs_discard) {
      s.command_list->barriers(
          plume::RenderBarrierStage::GRAPHICS,
          plume::RenderTextureBarrier(rt->texture,
                                      plume::RenderTextureLayout::COLOR_WRITE));
      s.command_list->discardTexture(rt->texture);
    }
    s.command_list->barriers(
        plume::RenderBarrierStage::GRAPHICS,
        plume::RenderTextureBarrier(rt->texture,
                                    plume::RenderTextureLayout::SHADER_READ));
    rt->layout = plume::RenderTextureLayout::SHADER_READ;
  }

  const u32 swap_w = s.swap_chain->getWidth();
  const u32 swap_h = s.swap_chain->getHeight();
  const u32 gamma_src_desc = rt->descriptorIndex;

  s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS,
                           plume::RenderTextureBarrier(
                               back, plume::RenderTextureLayout::COLOR_WRITE));
  s.command_list->setFramebuffer(back_fb);

  const double present_aspect =
      (bd::engine::SofdecPlayer::Playing() && !Output::StretchToFill())
          ? kDesignCanvasAspect
          : Output::RenderAspect();
  u32 fit_w = swap_w, fit_h = swap_h;
  i32 off_x = 0, off_y = 0;
  Output::ComputeFit(swap_w, swap_h, present_aspect, fit_w, fit_h, off_x,
                          off_y);
  if (fit_w != swap_w || fit_h != swap_h) {
    s.command_list->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 1.0f));
  }
  s.command_list->setViewports(plume::RenderViewport(
      static_cast<float>(off_x), static_cast<float>(off_y),
      static_cast<float>(fit_w), static_cast<float>(fit_h)));
  s.command_list->setScissors(
      plume::RenderRect(off_x, off_y, off_x + static_cast<i32>(fit_w),
                        off_y + static_cast<i32>(fit_h)));
  constexpr float kPresentGamma = 1.0f;
  constexpr float kPresentDisplayCorrection = 1.0f;
  s.command_list->setPipeline(s.gamma_correction_pipeline.get());
  struct PresentPushConstants {
    u32 descriptor_index;
    u32 descriptor_index_2;
    float gamma;
    float display_correction;
  } pc{gamma_src_desc, 0, s.guest_gamma * kPresentGamma,
       kPresentDisplayCorrection};
  s.command_list->setGraphicsPushConstants(kCopyPushConstantRangeIndex, &pc,
                                           kCopyPushConstantByteOffset,
                                           sizeof(pc));
  s.command_list->drawInstanced(3, 1, 0, 0);

  s.command_list->setViewports(plume::RenderViewport(
      0.0f, 0.0f, static_cast<float>(swap_w), static_cast<float>(swap_h)));
  s.command_list->setScissors(plume::RenderRect(0, 0, static_cast<i32>(swap_w),
                                                static_cast<i32>(swap_h)));

  if (g_overlay_draw_hook) {
    g_overlay_draw_hook(s.command_list, back_fb, swap_w, swap_h);
  }

  s.command_list->setFramebuffer(nullptr);
  s.command_list->barriers(
      plume::RenderBarrierStage::GRAPHICS,
      plume::RenderTextureBarrier(back, plume::RenderTextureLayout::PRESENT));
}

} // namespace

void Video::Clear(u32 flags, u32 color_argb, float depth, u32 stencil) {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  s.frame_present_committed = false;
  BeginCommandList(s);
  if (!s.command_list_open)
    return;
  GuestTexture *rt = nullptr;
  GuestTexture *ds = nullptr;
  ResolveEffectiveTargets(s, rt, ds);
  const bool clear_color = (flags & kClearTarget) != 0 && rt != nullptr;
  const bool clear_depth = (flags & kClearZBuffer) != 0 && ds != nullptr;
  const bool clear_stencil = (flags & kClearStencil) != 0 && ds != nullptr;
  if (!clear_color && !clear_depth && !clear_stencil)
    return;
  if (!BindDrawFramebufferLocked(clear_color))
    return;
  if (clear_color)
    s.command_list->clearColor(0, ArgbToRenderColor(color_argb));
  if (clear_depth || clear_stencil) {
    s.command_list->clearDepthStencil(clear_depth, clear_stencil, depth,
                                      stencil);
  }
}

void Video::RequestResize() {
  state().resize_requested.store(true, std::memory_order_release);
}

void Video::Present(GuestTexture *frontBuffer) {
  auto &s = state();
  if (s.shutting_down.load(std::memory_order_acquire))
    return;
  std::unique_lock lock(s.mutex);
  if (!s.ready || s.shutting_down.load(std::memory_order_acquire)) {
    return;
  }
  if (s.frame_present_committed) {
    return;
  }

  // Backgrounded Android activity: no surface to present to, so keep the
  // guest ticking at the idle rate until it comes back.
  // The process may not come back from the background, so each trip there is
  // when the session's pipelines are kept for the next run.
  static bool cache_saved = false;
  if (!bd::platform::android::SurfaceAvailable()) {
    if (!cache_saved) {
      SavePipelineCache(s);
      cache_saved = true;
    }
    g_frame_begin = {};
    AbandonFrame(s, lock);
    if (lock.owns_lock())
      lock.unlock();
    PaceFrame(true);
    return;
  }
  cache_saved = false;
  // A failed recreation leaves no swap chain, so the next frame tries again.
  if (bd::platform::android::ConsumeSurfaceChanged() ||
      (kSurfaceCanVanish && !s.swap_chain)) {
    SubmitOpenListLocked(s);
    RecreateSwapChainLocked(s);
    bd::platform::android::InvalidateDisplayFrameRate();
  }

  const bool resize_requested =
      s.resize_requested.exchange(false, std::memory_order_acq_rel);
  if (s.swap_chain && (s.swap_chain->needsResize() || resize_requested)) {
    RebuildSwapChain(s);
  }

  if (s.framebuffers.empty()) {
    AbandonFrame(s, lock);
    if (lock.owns_lock())
      lock.unlock();
    PaceFrame(true);
    return;
  }

  const double waits_before = TotalWaitsMs();
  u32 texture_index = 0;
  {
    BD_CPU_ZONE("AcquireTexture");
    WaitTimer timer(g_waits.acquire_ms);
    if (!s.swap_chain->acquireTexture(
            s.acquire_semaphores[s.frame.load(std::memory_order_relaxed)].get(),
            &texture_index)) {
      RebuildSwapChain(s);
      if (s.framebuffers.empty() ||
          !s.swap_chain->acquireTexture(
              s.acquire_semaphores[s.frame.load(std::memory_order_relaxed)]
                  .get(),
              &texture_index)) {
        CheckDeviceRemoved("swapchain acquire");
        AbandonFrame(s, lock);
        return;
      }
    }
  }

  plume::RenderTexture *back = s.swap_chain->getTexture(texture_index);
  plume::RenderFramebuffer *back_fb = s.framebuffers[texture_index].get();

  GuestTexture *rt = SelectPresentSource(s, frontBuffer);

  const bool have_rt_blit =
      rt && rt->texture && rt->descriptorIndex != kInvalidDescriptorIndex &&
      rt->sampleCount == plume::RenderSampleCount::COUNT_1;
  if (!have_rt_blit) {
    static std::atomic<u32> s_log{0};
    const u32 n = s_log.fetch_add(1, std::memory_order_relaxed);
    if (n < 5) {
      BD_ERROR("Present #{} skipped: no drawable RT", n);
    }
    AbandonFrame(s, lock);
    return;
  }

  BeginCommandList(s);
  RecordPresentPass(s, rt, back, back_fb);

  const u32 cur = s.frame.load(std::memory_order_relaxed);
  if (s.gpu_timer_started[cur]) {
    s.command_list->writeTimestamp(s.gpu_timers[cur].get(), 1);
    s.gpu_timer_started[cur] = false;
    s.gpu_timer_pending[cur] = true;
  }
  s.command_lists[cur]->end();
  s.command_list_open = false;

  const plume::RenderCommandList *lists[] = {s.command_lists[cur].get()};
  plume::RenderCommandSemaphore *waits[] = {s.acquire_semaphores[cur].get()};
  plume::RenderCommandSemaphore *signals[] = {
      s.render_semaphores[texture_index].get()};
  {
    BD_CPU_ZONE("Submit");
    s.queue->executeCommandLists(lists, 1, waits, 1, signals, 1,
                                 s.fences[cur].get());
  }
  s.command_list_submitted[cur] = true;
  ApplyVsync(s);
  {
    BD_CPU_ZONE("PresentSwap");
    WaitTimer timer(g_waits.present_ms);
    if (!s.swap_chain->present(texture_index, signals, 1)) {
      if (!CheckDeviceRemoved("swapchain present"))
        s.resize_requested.store(true, std::memory_order_release);
    }
  }
  {
    BD_CPU_ZONE("WaitFence");
    WaitTimer timer(g_waits.gpu_ms);
    AdvanceAndWaitReused(s);
  }
  const double frame_waits_ms = TotalWaitsMs() - waits_before;
  const u32 reclaimed = s.frame.load(std::memory_order_relaxed);
  s.frame_present_committed = true;
  BD_FRAME_MARK();
  LogFrameRate();
  Video::SyncBackBufferSizeLocked();
  lock.unlock();
  {
    BD_CPU_ZONE("DrainSlot");
    DrainSlot(s, reclaimed);
  }
  // The guest and the draw recording, without the waits on the swap chain and
  // the GPU: what the CPU governor has to keep up with.
  if (g_frame_begin.time_since_epoch().count() != 0) {
    const auto work =
        std::chrono::steady_clock::now() - g_frame_begin -
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double, std::milli>(frame_waits_ms));
    bd::platform::android::ReportFrameWork(
        std::chrono::duration_cast<std::chrono::nanoseconds>(work),
        bd::engine::TargetFPS());
  }
  // Between frames, so interpolation never flips halfway through one.
  bd::platform::android::UpdatePowerSaving();
  {
    BD_CPU_ZONE("PaceFrame");
    WaitTimer timer(g_waits.pace_ms);
    PaceFrame();
  }
  g_frame_begin = std::chrono::steady_clock::now();
}

void Video::SkipPresent() {
  auto &s = state();
  if (s.shutting_down.load(std::memory_order_acquire))
    return;
  std::unique_lock lock(s.mutex);
  if (!s.ready || s.shutting_down.load(std::memory_order_acquire) ||
      s.frame_present_committed) {
    return;
  }
  AbandonFrame(s, lock);
}

void Video::PresentOverlayFrame() {
  auto &s = state();
  if (s.shutting_down.load(std::memory_order_acquire))
    return;
  std::unique_lock lock(s.mutex);
  if (s.ready || !s.device || (!s.swap_chain && !kSurfaceCanVanish))
    return;
  if (!bd::platform::android::SurfaceAvailable())
    return;
  // A failed recreation leaves no swap chain, so the next frame tries again.
  if ((bd::platform::android::ConsumeSurfaceChanged() || !s.swap_chain) &&
      !RecreateSwapChainLocked(s))
    return;

  const bool resize_requested =
      s.resize_requested.exchange(false, std::memory_order_acq_rel);
  if (s.swap_chain->needsResize() || resize_requested) {
    for (u32 i = 0; i < kNumFrames; ++i) {
      if (s.command_list_submitted[i]) {
        s.queue->waitForCommandFence(s.fences[i].get());
        s.command_list_submitted[i] = false;
      }
    }
    s.framebuffers.clear();
    if (!s.swap_chain->resize() || !BuildFramebuffers(s) ||
        !BuildPresentSemaphores(s)) {
      if (s.swap_chain->getWidth() && s.swap_chain->getHeight())
        BD_ERROR("Swap chain resize failed");
    }
  }
  if (s.framebuffers.empty())
    return;

  const u32 cur = s.frame.load(std::memory_order_relaxed);
  u32 texture_index = 0;
  if (!s.swap_chain->acquireTexture(s.acquire_semaphores[cur].get(),
                                    &texture_index)) {
    s.resize_requested.store(true, std::memory_order_release);
    return;
  }
  plume::RenderTexture *back = s.swap_chain->getTexture(texture_index);
  plume::RenderFramebuffer *back_fb = s.framebuffers[texture_index].get();
  const u32 swap_w = s.swap_chain->getWidth();
  const u32 swap_h = s.swap_chain->getHeight();

  BeginCommandList(s);
  if (!s.command_list_open)
    return;

  s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS,
                           plume::RenderTextureBarrier(
                               back, plume::RenderTextureLayout::COLOR_WRITE));
  s.command_list->setFramebuffer(back_fb);
  s.command_list->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 1.0f));
  s.command_list->setViewports(plume::RenderViewport(
      0.0f, 0.0f, static_cast<float>(swap_w), static_cast<float>(swap_h)));
  s.command_list->setScissors(plume::RenderRect(0, 0, static_cast<i32>(swap_w),
                                                static_cast<i32>(swap_h)));
  if (g_overlay_draw_hook) {
    g_overlay_draw_hook(s.command_list, back_fb, swap_w, swap_h);
  }
  s.command_list->setFramebuffer(nullptr);
  s.command_list->barriers(
      plume::RenderBarrierStage::GRAPHICS,
      plume::RenderTextureBarrier(back, plume::RenderTextureLayout::PRESENT));

  s.command_lists[cur]->end();
  s.command_list_open = false;
  const plume::RenderCommandList *lists[] = {s.command_lists[cur].get()};
  plume::RenderCommandSemaphore *waits[] = {s.acquire_semaphores[cur].get()};
  plume::RenderCommandSemaphore *signals[] = {
      s.render_semaphores[texture_index].get()};
  s.queue->executeCommandLists(lists, 1, waits, 1, signals, 1,
                               s.fences[cur].get());
  s.command_list_submitted[cur] = true;
  ApplyVsync(s);
  if (!s.swap_chain->present(texture_index, signals, 1)) {
    CheckDeviceRemoved("swapchain present (overlay)");
  }
  AdvanceAndWaitReused(s);
  const u32 reclaimed = s.frame.load(std::memory_order_relaxed);
  lock.unlock();
  DrainSlot(s, reclaimed);
}

} // namespace bd::gpu
