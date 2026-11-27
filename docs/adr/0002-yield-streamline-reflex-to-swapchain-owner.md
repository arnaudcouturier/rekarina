# Yield Streamline Reflex and Swapchain Hooks to the Active Swapchain Owner

To keep live upscaler and frame-generation backend switching working on `OoT` without restarting, Streamline preparation and AMD FSR SDK 2 graphics queue reservation remain available at startup, **but** Streamline Reflex and swapchain-readiness queries must yield whenever AMD FSR SDK 2 is the active **Swapchain Owner**, and native NGX (`_nvngx.dll`) is not eagerly loaded when Streamline is inactive and DLSS is not requested.

## Considered Options

- **Always-active Streamline swapchain readiness (ShortFuse's approach)**: Keeps `vk_dlssg_swapchain_ready()` returning true even while AMD owns the swapchain, causing Streamline's `slReflexSleep()` to run against an unowned swapchain while suppressing host frame-start pacing, and unconditionally loads `_nvngx.dll` at startup whenever Streamline is absent.
- **Startup-only Streamline initialization**: Avoids all Streamline interposition when `DLAA=off` or `FSR` is selected, preserving native `VK_NV_low_latency2` Reflex and `VK_EXT_PRESENT_TIMING_EXT`, at the cost of breaking live switching to DLSS / DLSS Frame Generation without a restart.
- **Live preparation with strict Swapchain Owner yield (chosen)**: Preserves live switching across Off, FSR, DLSS, DLSS FG, and AMD FSR FG while guaranteeing Streamline never sleeps on an AMD-owned swapchain and avoiding wasted native NGX initialization on non-DLSS runs.

## Consequences

- Callers gating Reflex or frame pacing must check that Streamline is the active **Swapchain Owner**, not merely that Streamline initialized at startup.
- When Streamline is loaded at startup on RTX hardware with `FRAMEGEN=off`, Streamline still interposes the Vulkan swapchain so live DLSS-FG toggles do not require a restart.
