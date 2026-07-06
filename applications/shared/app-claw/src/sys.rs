//! Low-level app-claw FFI bindings.

#[cfg(target_os = "espidf")]
mod imp {
    #![allow(dead_code)]
    #![allow(unsafe_code)]

    use std::ffi::{CStr, CString, NulError};
    use std::fmt::Write as _;

    use core::ffi::{c_char, c_int};
    use debug_console::{Error, Result};

    pub(crate) const ESP_OK: c_int = 0;
    pub(crate) const ESP_FAIL: c_int = -1;

    const CLAW_CAP_CALLER_CONSOLE: c_int = 2;
    const CAP_OUTPUT_SIZE: usize = 16384;
    const CAP_UNLOAD_TIMEOUT_MS: u32 = 1000;

    #[repr(C)]
    struct ClawCapCallContext {
        request_id: u32,
        session_id: *const c_char,
        agent_id: *const c_char,
        agent_type: *const c_char,
        parent_agent_id: *const c_char,
        parent_session_id: *const c_char,
        channel: *const c_char,
        chat_id: *const c_char,
        target_channel: *const c_char,
        target_chat_id: *const c_char,
        source_cap: *const c_char,
        correlation_id: *const c_char,
        caller: c_int,
    }

    impl Default for ClawCapCallContext {
        fn default() -> Self {
            Self {
                request_id: 0,
                session_id: core::ptr::null(),
                agent_id: core::ptr::null(),
                agent_type: core::ptr::null(),
                parent_agent_id: core::ptr::null(),
                parent_session_id: core::ptr::null(),
                channel: core::ptr::null(),
                chat_id: core::ptr::null(),
                target_channel: core::ptr::null(),
                target_chat_id: core::ptr::null(),
                source_cap: core::ptr::null(),
                correlation_id: core::ptr::null(),
                caller: CLAW_CAP_CALLER_CONSOLE,
            }
        }
    }

    #[repr(C)]
    struct ClawCapDescriptor {
        id: *const c_char,
        name: *const c_char,
        family: *const c_char,
        description: *const c_char,
        kind: c_int,
        cap_flags: u32,
        input_schema_json: *const c_char,
        init: Option<unsafe extern "C" fn() -> c_int>,
        start: Option<unsafe extern "C" fn() -> c_int>,
        stop: Option<unsafe extern "C" fn() -> c_int>,
        execute: Option<
            unsafe extern "C" fn(
                *const c_char,
                *const ClawCapCallContext,
                *mut c_char,
                usize,
            ) -> c_int,
        >,
    }

    #[repr(C)]
    struct ClawCapList {
        items: *const ClawCapDescriptor,
        count: usize,
    }

    #[repr(C)]
    struct ClawCapGroupInfo {
        group_id: *const c_char,
        plugin_name: *const c_char,
        version: *const c_char,
        state: c_int,
        descriptor_count: usize,
    }

    #[repr(C)]
    struct ClawCapGroupList {
        items: *const ClawCapGroupInfo,
        count: usize,
    }

    #[repr(C)]
    struct ClawEventRouterResult {
        matched: bool,
        matched_rules: c_int,
        action_count: c_int,
        failed_actions: c_int,
        handled_at_ms: i64,
        first_rule_id: [c_char; 64],
        ack: [c_char; 256],
        route: c_int,
        last_error: c_int,
    }

    #[repr(C)]
    struct ClawAgentResponse {
        status: c_int,
        text: *mut c_char,
        error_message: *mut c_char,
    }

    extern "C" {
        fn claw_cap_list() -> ClawCapList;
        fn claw_cap_list_groups() -> ClawCapGroupList;
        fn claw_cap_enable_group(group_id: *const c_char) -> c_int;
        fn claw_cap_disable_group(group_id: *const c_char) -> c_int;
        fn claw_cap_unregister_group(group_id: *const c_char, timeout_ms: u32) -> c_int;
        fn claw_cap_call(
            id_or_name: *const c_char,
            input_json: *const c_char,
            ctx: *const ClawCapCallContext,
            output: *mut c_char,
            output_size: usize,
        ) -> c_int;
        fn claw_cap_state_to_string(state: c_int) -> *const c_char;
        fn claw_event_router_get_last_result(out_result: *mut ClawEventRouterResult) -> c_int;
        fn claw_event_router_publish_message(
            source_cap: *const c_char,
            channel: *const c_char,
            chat_id: *const c_char,
            text: *const c_char,
            sender_id: *const c_char,
            message_id: *const c_char,
        ) -> c_int;
        fn claw_event_router_publish_trigger(
            source_cap: *const c_char,
            event_type: *const c_char,
            event_key: *const c_char,
            payload_json: *const c_char,
        ) -> c_int;
        fn claw_agent_session_submit(
            session_id: u32,
            text: *const c_char,
            out_request_id: *mut u32,
        ) -> c_int;
        fn claw_agent_session_receive(
            session_id: u32,
            request_id: u32,
            out_response: *mut ClawAgentResponse,
            timeout_ms: u32,
        ) -> c_int;
        fn claw_agent_response_free(response: *mut ClawAgentResponse);
        fn esp_err_to_name(error: c_int) -> *const c_char;
    }

    pub(crate) fn cap_list() -> Result<String> {
        let list = unsafe { claw_cap_list() };
        let items = unsafe { slice_from_raw_parts(list.items, list.count)? };
        if items.is_empty() {
            return Ok("no capabilities\n".to_owned());
        }

        let mut output = String::new();
        for item in items {
            let _ = writeln!(
                output,
                "{}\t{}\t{}",
                cstr_to_string(item.id),
                cstr_to_string(item.family),
                cstr_to_string(item.description)
            );
        }
        Ok(output)
    }

    pub(crate) fn cap_groups() -> Result<String> {
        let list = unsafe { claw_cap_list_groups() };
        let items = unsafe { slice_from_raw_parts(list.items, list.count)? };
        if items.is_empty() {
            return Ok("no capability groups\n".to_owned());
        }

        let mut output = String::new();
        for item in items {
            let _ = writeln!(
                output,
                "{}\t{}\tdescriptors={}\tplugin={}\tversion={}",
                cstr_to_string(item.group_id),
                cap_state_name(item.state),
                item.descriptor_count,
                cstr_or_dash(item.plugin_name),
                cstr_or_dash(item.version)
            );
        }
        Ok(output)
    }

    pub(crate) fn cap_call(name: &str, input_json: &str) -> Result<String> {
        let name = cstring(name, "capability name")?;
        let input_json = cstring(input_json, "capability input")?;
        let ctx = ClawCapCallContext::default();
        let mut output = vec![0_u8; CAP_OUTPUT_SIZE];
        let err = unsafe {
            claw_cap_call(
                name.as_ptr(),
                input_json.as_ptr(),
                &ctx,
                output.as_mut_ptr().cast::<c_char>(),
                output.len(),
            )
        };
        let text = c_buffer_to_string(&output);
        if err == ESP_OK {
            return Ok(ensure_trailing_newline(text));
        }
        if text.trim().is_empty() {
            return Err(ffi_error("cap call", err));
        }
        Err(Error::Stream(text))
    }

    pub(crate) fn cap_enable_group(group_id: &str) -> Result<String> {
        let group_id_c = cstring(group_id, "group id")?;
        let err = unsafe { claw_cap_enable_group(group_id_c.as_ptr()) };
        if err == ESP_OK {
            return Ok(format!("enabled {group_id}\n"));
        }
        Err(ffi_error("cap enable", err))
    }

    pub(crate) fn cap_disable_group(group_id: &str) -> Result<String> {
        let group_id_c = cstring(group_id, "group id")?;
        let err = unsafe { claw_cap_disable_group(group_id_c.as_ptr()) };
        if err == ESP_OK {
            return Ok(format!("disabled {group_id}\n"));
        }
        Err(ffi_error("cap disable", err))
    }

    pub(crate) fn cap_unload_group(group_id: &str) -> Result<String> {
        let group_id_c = cstring(group_id, "group id")?;
        let err = unsafe { claw_cap_unregister_group(group_id_c.as_ptr(), CAP_UNLOAD_TIMEOUT_MS) };
        if err == ESP_OK {
            return Ok(format!("unloaded {group_id}\n"));
        }
        Err(ffi_error("cap unload", err))
    }

    pub(crate) fn event_router_last() -> Result<String> {
        let mut result = ClawEventRouterResult {
            matched: false,
            matched_rules: 0,
            action_count: 0,
            failed_actions: 0,
            handled_at_ms: 0,
            first_rule_id: [0; 64],
            ack: [0; 256],
            route: 0,
            last_error: ESP_OK,
        };
        let err = unsafe { claw_event_router_get_last_result(&mut result) };
        if err != ESP_OK {
            return Err(ffi_error("event router last", err));
        }

        Ok(format!(
            "matched={} matched_rules={} action_count={} failed_actions={} route={} handled_at_ms={}\nfirst_rule_id={}\nack={}\nlast_error={}\n",
            result.matched,
            result.matched_rules,
            result.action_count,
            result.failed_actions,
            result.route,
            result.handled_at_ms,
            c_array_to_string(&result.first_rule_id),
            c_array_to_string(&result.ack),
            esp_err_name(result.last_error)
        ))
    }

    pub(crate) fn event_router_publish_message(
        source_cap: &str,
        channel: &str,
        chat_id: &str,
        text: &str,
    ) -> Result<String> {
        let source_cap_c = cstring(source_cap, "source cap")?;
        let channel_c = cstring(channel, "channel")?;
        let chat_id_c = cstring(chat_id, "chat id")?;
        let text_c = cstring(text, "text")?;
        let sender_id = cstring("console", "sender id")?;
        let message_id = cstring("debug-console", "message id")?;
        let err = unsafe {
            claw_event_router_publish_message(
                source_cap_c.as_ptr(),
                channel_c.as_ptr(),
                chat_id_c.as_ptr(),
                text_c.as_ptr(),
                sender_id.as_ptr(),
                message_id.as_ptr(),
            )
        };
        if err == ESP_OK {
            return Ok(format!(
                "message event published via {source_cap} to {channel}:{chat_id}\n"
            ));
        }
        Err(ffi_error("event router emit message", err))
    }

    pub(crate) fn event_router_publish_trigger(
        source_cap: &str,
        event_type: &str,
        event_key: &str,
        payload_json: &str,
    ) -> Result<String> {
        let source_cap_c = cstring(source_cap, "source cap")?;
        let event_type_c = cstring(event_type, "event type")?;
        let event_key_c = cstring(event_key, "event key")?;
        let payload_json_c = cstring(payload_json, "payload json")?;
        let err = unsafe {
            claw_event_router_publish_trigger(
                source_cap_c.as_ptr(),
                event_type_c.as_ptr(),
                event_key_c.as_ptr(),
                payload_json_c.as_ptr(),
            )
        };
        if err == ESP_OK {
            return Ok(format!(
                "trigger event published via {source_cap} type={event_type} key={event_key}\n"
            ));
        }
        Err(ffi_error("event router emit trigger", err))
    }

    #[allow(dead_code)]
    pub(crate) fn agent_submit_session(
        session_id: u32,
        text: &str,
        timeout_ms: u32,
    ) -> Result<String> {
        if session_id == 0 {
            return Err(Error::Stream("session id must be non-zero".to_owned()));
        }

        let text_c = cstring(text, "agent text")?;
        let mut request_id = 0_u32;
        let err =
            unsafe { claw_agent_session_submit(session_id, text_c.as_ptr(), &mut request_id) };
        if err != ESP_OK {
            return Err(ffi_error("agent submit", err));
        }

        let mut response = ClawAgentResponse {
            status: 0,
            text: core::ptr::null_mut(),
            error_message: core::ptr::null_mut(),
        };
        let err = unsafe {
            claw_agent_session_receive(session_id, request_id, &mut response, timeout_ms)
        };
        if err != ESP_OK {
            return Err(ffi_error("agent receive", err));
        }

        let output = if response.status == 0 {
            ensure_trailing_newline(cstr_to_string(response.text))
        } else {
            cstr_to_string(response.error_message)
        };
        unsafe { claw_agent_response_free(&mut response) };

        if response.status == 0 {
            Ok(output)
        } else {
            Err(Error::Stream(output))
        }
    }

    unsafe fn slice_from_raw_parts<'a, T>(ptr: *const T, count: usize) -> Result<&'a [T]> {
        if count == 0 {
            return Ok(&[]);
        }
        if ptr.is_null() {
            return Err(Error::Stream(
                "ffi returned null list with non-zero count".to_owned(),
            ));
        }
        Ok(std::slice::from_raw_parts(ptr, count))
    }

    fn cstring(value: &str, name: &'static str) -> Result<CString> {
        CString::new(value).map_err(|error| nul_error(name, error))
    }

    fn nul_error(name: &'static str, _error: NulError) -> Error {
        Error::Stream(format!("{name} contains an embedded nul byte"))
    }

    fn cstr_to_string(ptr: *const c_char) -> String {
        if ptr.is_null() {
            return String::new();
        }
        unsafe { CStr::from_ptr(ptr) }
            .to_string_lossy()
            .into_owned()
    }

    fn cstr_or_dash(ptr: *const c_char) -> String {
        let value = cstr_to_string(ptr);
        if value.is_empty() {
            "-".to_owned()
        } else {
            value
        }
    }

    fn c_array_to_string<const N: usize>(bytes: &[c_char; N]) -> String {
        cstr_to_string(bytes.as_ptr())
    }

    fn c_buffer_to_string(bytes: &[u8]) -> String {
        let ptr = bytes.as_ptr().cast::<c_char>();
        cstr_to_string(ptr)
    }

    fn ensure_trailing_newline(mut text: String) -> String {
        if !text.ends_with('\n') {
            text.push('\n');
        }
        text
    }

    fn cap_state_name(state: c_int) -> String {
        let ptr = unsafe { claw_cap_state_to_string(state) };
        let value = cstr_to_string(ptr);
        if value.is_empty() {
            state.to_string()
        } else {
            value
        }
    }

    fn esp_err_name(error: c_int) -> String {
        let ptr = unsafe { esp_err_to_name(error) };
        let value = cstr_to_string(ptr);
        if value.is_empty() {
            error.to_string()
        } else {
            value
        }
    }

    fn ffi_error(operation: &'static str, error: c_int) -> Error {
        Error::Stream(format!("{operation} failed: {}", esp_err_name(error)))
    }
}

#[cfg(not(target_os = "espidf"))]
mod imp {
    #![allow(dead_code)]

    use debug_console::{Error, Result};

    pub(crate) const ESP_OK: i32 = 0;
    pub(crate) const ESP_FAIL: i32 = -1;

    pub(crate) fn cap_list() -> Result<String> {
        unsupported("cap list")
    }

    pub(crate) fn cap_groups() -> Result<String> {
        unsupported("cap groups")
    }

    pub(crate) fn cap_call(_name: &str, _input_json: &str) -> Result<String> {
        unsupported("cap call")
    }

    pub(crate) fn cap_enable_group(_group_id: &str) -> Result<String> {
        unsupported("cap enable")
    }

    pub(crate) fn cap_disable_group(_group_id: &str) -> Result<String> {
        unsupported("cap disable")
    }

    pub(crate) fn cap_unload_group(_group_id: &str) -> Result<String> {
        unsupported("cap unload")
    }

    pub(crate) fn event_router_last() -> Result<String> {
        unsupported("auto last")
    }

    pub(crate) fn event_router_publish_message(
        _source_cap: &str,
        _channel: &str,
        _chat_id: &str,
        _text: &str,
    ) -> Result<String> {
        unsupported("auto emit_message")
    }

    pub(crate) fn event_router_publish_trigger(
        _source_cap: &str,
        _event_type: &str,
        _event_key: &str,
        _payload_json: &str,
    ) -> Result<String> {
        unsupported("auto emit_trigger")
    }

    pub(crate) fn agent_submit_session(
        _session_id: u32,
        _text: &str,
        _timeout_ms: u32,
    ) -> Result<String> {
        unsupported("agent submit")
    }

    fn unsupported(operation: &'static str) -> Result<String> {
        Err(Error::Stream(format!("{operation} requires ESP-IDF FFI")))
    }
}

pub(crate) use imp::*;
