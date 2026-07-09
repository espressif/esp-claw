//! Worker-thread spawning that mirrors the C `claw_task` behavior.
//!
//! [`EspIdfThread`] is the device implementation of [`claw_interface::ClawThread`]:
//! the C firmware created its long-running worker tasks
//! (`xTaskCreatePinnedToCoreWithCaps`) with a PSRAM-backed stack, and a bare
//! `std::thread` would use the small default pthread stack in internal RAM and
//! overflow under the agent / extraction workloads (LLM, mbedTLS, serde_json).
//! `EspIdfThread` applies the requested stack size, [`Priority`], [`CoreAffinity`],
//! and PSRAM stack caps to the next `pthread_create` (which `std::thread::spawn`
//! uses on ESP-IDF) via `esp_pthread`, then restores the previous config so
//! unrelated spawns are unaffected.
//!
//! This crate owns only the device implementation. The trait
//! `claw_interface::ClawThread` and the platform-neutral `Priority` /
//! `CoreAffinity` types live in `claw-interface` (consumers import them from
//! there, not from here); the host implementation is `claw_interface::StdThread`.
//! Only the concrete FreeRTOS priority numbers and the `tskNO_AFFINITY` sentinel
//! are espidf details, and they stay private inside the `espidf` module below.
//!
//! The wiring layer selects the implementation to inject: `EspIdfThread` on
//! device, `StdThread` on host. Both are zero-sized, so a `T: ClawThread` bound
//! costs nothing.

#[cfg(target_os = "espidf")]
use claw_interface::{ClawThread, CoreAffinity, Priority, WorkerHandle};
#[cfg(target_os = "espidf")]
use std::io;

/// Device implementation of [`ClawThread`] over `esp_pthread`, giving worker
/// threads a PSRAM-backed stack (when PSRAM is available). Zero-sized.
#[cfg(target_os = "espidf")]
#[derive(Clone, Copy, Default)]
pub struct EspIdfThread;

#[cfg(target_os = "espidf")]
impl ClawThread for EspIdfThread {
    fn spawn_worker<F>(
        name: &str,
        stack_size: usize,
        priority: Priority,
        affinity: CoreAffinity,
        f: F,
    ) -> io::Result<WorkerHandle>
    where
        F: FnOnce() + Send + 'static,
    {
        let _restore = espidf::apply_cfg(name, stack_size, priority, affinity);
        // esp_pthread (carried by _restore's cfg) sets the stack size and PSRAM
        // caps; Builder::stack_size pins the pthread attr stack to the same value.
        std::thread::Builder::new()
            .name(name.to_string())
            .stack_size(stack_size)
            .spawn(f)
            .map(WorkerHandle::new)
    }
}

#[cfg(target_os = "espidf")]
mod espidf {
    use std::ffi::CString;
    use std::os::raw::{c_char, c_int};

    use claw_interface::{CoreAffinity, Priority};

    // MALLOC_CAP_* bits from esp_heap_caps.h.
    const MALLOC_CAP_8BIT: u32 = 1 << 2;
    const MALLOC_CAP_SPIRAM: u32 = 1 << 10;

    // FreeRTOS `tskNO_AFFINITY`: let the scheduler pick the core. This is the
    // espidf magic value the cross-platform `CoreAffinity` enum hides.
    const NO_AFFINITY: c_int = 0x7fff_ffff;

    // FreeRTOS task priorities (`0..configMAX_PRIORITIES`, higher = more urgent).
    // The C agent's background workers ran around `PRIO_NORMAL`; the other levels
    // bracket it without touching the high-priority system/timer tasks.
    const PRIO_LOW: usize = 2;
    const PRIO_NORMAL: usize = 5;
    const PRIO_HIGH: usize = 10;

    fn freertos_priority(priority: Priority) -> usize {
        match priority {
            Priority::Low => PRIO_LOW,
            Priority::Normal => PRIO_NORMAL,
            Priority::High => PRIO_HIGH,
        }
    }

    fn freertos_core(affinity: CoreAffinity) -> c_int {
        match affinity {
            CoreAffinity::Any => NO_AFFINITY,
            CoreAffinity::Core(index) => c_int::from(index),
        }
    }

    #[repr(C)]
    #[derive(Clone, Copy)]
    struct EspPthreadCfg {
        stack_size: usize,
        prio: usize,
        inherit_cfg: bool,
        thread_name: *const c_char,
        pin_to_core: c_int,
        stack_alloc_caps: u32,
    }

    extern "C" {
        fn esp_pthread_get_default_config() -> EspPthreadCfg;
        fn esp_pthread_get_cfg(p: *mut EspPthreadCfg) -> c_int;
        fn esp_pthread_set_cfg(cfg: *const EspPthreadCfg) -> c_int;
        fn heap_caps_get_total_size(caps: u32) -> usize;
    }

    /// Restores the prior `esp_pthread` config on drop. Holds the `thread_name`
    /// `CString` alive until then since the config borrows its pointer.
    pub struct CfgGuard {
        previous: EspPthreadCfg,
        had_previous: bool,
        _name: CString,
    }

    impl Drop for CfgGuard {
        fn drop(&mut self) {
            unsafe {
                if self.had_previous {
                    esp_pthread_set_cfg(&self.previous);
                } else {
                    let def = esp_pthread_get_default_config();
                    esp_pthread_set_cfg(&def);
                }
            }
        }
    }

    pub fn apply_cfg(
        name: &str,
        stack_size: usize,
        priority: Priority,
        affinity: CoreAffinity,
    ) -> CfgGuard {
        unsafe {
            let mut previous = esp_pthread_get_default_config();
            let had_previous = esp_pthread_get_cfg(&mut previous) == 0;

            // Prefer a PSRAM stack when PSRAM is present (the build enables
            // CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM); otherwise let
            // esp_pthread choose a valid internal-RAM default (caps == 0).
            let caps = if heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0 {
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
            } else {
                0
            };

            let mut thread_name = name.as_bytes().to_vec();
            for byte in &mut thread_name {
                if *byte == 0 {
                    *byte = b'_';
                }
            }
            // SAFETY: every interior NUL byte was replaced above.
            let cname = CString::from_vec_unchecked(thread_name);
            let mut cfg = esp_pthread_get_default_config();
            cfg.stack_size = stack_size;
            cfg.prio = freertos_priority(priority);
            cfg.inherit_cfg = false;
            cfg.thread_name = cname.as_ptr();
            cfg.pin_to_core = freertos_core(affinity);
            cfg.stack_alloc_caps = caps;
            esp_pthread_set_cfg(&cfg);

            CfgGuard {
                previous,
                had_previous,
                _name: cname,
            }
        }
    }
}
