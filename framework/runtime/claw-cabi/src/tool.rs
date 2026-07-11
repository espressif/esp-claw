use core::ffi::{c_char, CStr};
use std::collections::BTreeMap;
use std::ffi::CString;

use claw_tool::{
    RetryCount, SyncToolHandler, Tool, ToolError, ToolGroup, ToolInvocation, ToolInvokeError,
    ToolOutput, ToolResult, ToolSpec,
};
use serde_json::json;

use crate::abi::{
    claw_cap_call, claw_cap_get_descriptor_state, claw_cap_list, ClawCapCallContext,
    ClawCapDescriptor, ClawCapDescriptorInfo, CLAW_CAP_FLAG_CALLABLE_BY_LLM,
    CLAW_CAP_FLAG_ROOT_AGENT_ONLY, CLAW_CAP_KIND_CALLABLE, CLAW_CAP_KIND_HYBRID, ESP_OK,
    TOOL_OUTPUT_CAPACITY,
};

#[derive(Debug, thiserror::Error)]
pub(crate) enum CapToolError {
    #[error("invalid capability registry list")]
    InvalidList,
    #[error("invalid capability descriptor")]
    InvalidDescriptor,
    #[error("invalid capability schema: {0}")]
    InvalidSchema(String),
    #[error("tool registry failed: {0}")]
    Registry(#[from] claw_tool::ToolRegistryError),
}

pub(crate) fn register_capability_tools(
    registry: &claw_tool::ToolRegistry,
) -> Result<(), CapToolError> {
    let list = unsafe { claw_cap_list() };
    if list.count > 0 && list.items.is_null() {
        return Err(CapToolError::InvalidList);
    }

    let mut groups = BTreeMap::<String, Vec<Tool>>::new();
    for index in 0..list.count {
        let descriptor =
            unsafe { list.items.add(index).as_ref() }.ok_or(CapToolError::InvalidDescriptor)?;
        if !is_llm_tool(descriptor) {
            continue;
        }
        let group_id = descriptor_group_id(descriptor)?;
        groups
            .entry(group_id)
            .or_default()
            .push(Tool::from_sync(CapTool::try_from(descriptor)?));
    }
    for (group_id, tools) in groups {
        registry.register_group(ToolGroup::new(group_id, false, tools))?;
    }
    Ok(())
}

fn is_llm_tool(descriptor: &ClawCapDescriptor) -> bool {
    matches!(
        descriptor.kind,
        CLAW_CAP_KIND_CALLABLE | CLAW_CAP_KIND_HYBRID
    ) && descriptor.execute.is_some()
        && descriptor.cap_flags & CLAW_CAP_FLAG_CALLABLE_BY_LLM != 0
        && descriptor.cap_flags & CLAW_CAP_FLAG_ROOT_AGENT_ONLY == 0
}

fn descriptor_group_id(descriptor: &ClawCapDescriptor) -> Result<String, CapToolError> {
    let name = c_string(descriptor.name)
        .or_else(|| c_string(descriptor.id))
        .ok_or(CapToolError::InvalidDescriptor)?;
    let c_name = CString::new(name).map_err(|_| CapToolError::InvalidDescriptor)?;
    let mut info = ClawCapDescriptorInfo {
        id: core::ptr::null(),
        name: core::ptr::null(),
        group_id: core::ptr::null(),
        state: 0,
        active_calls: 0,
    };
    let err = unsafe { claw_cap_get_descriptor_state(c_name.as_ptr(), &mut info) };
    if err != ESP_OK {
        return Err(CapToolError::InvalidDescriptor);
    }
    c_string(info.group_id)
        .filter(|group_id| !group_id.is_empty())
        .ok_or(CapToolError::InvalidDescriptor)
}

struct CapTool {
    name: String,
    schema: String,
    usage: Option<String>,
}

impl CapTool {
    fn try_from(descriptor: &ClawCapDescriptor) -> Result<Self, CapToolError> {
        let name = c_string(descriptor.name)
            .or_else(|| c_string(descriptor.id))
            .ok_or(CapToolError::InvalidDescriptor)?;
        let input_schema =
            c_string(descriptor.input_schema_json).ok_or(CapToolError::InvalidDescriptor)?;
        let description = c_string(descriptor.description);
        let description_text = match description.as_deref() {
            Some(description) => description,
            None => "",
        };

        let parameters = serde_json::from_str::<serde_json::Value>(&input_schema)
            .map_err(|error| CapToolError::InvalidSchema(error.to_string()))?;
        let schema = json!({
            "type": "function",
            "function": {
                "name": &name,
                "description": description_text,
                "parameters": parameters,
            }
        })
        .to_string();

        Ok(Self {
            name,
            schema,
            usage: description,
        })
    }
}

impl ToolSpec for CapTool {
    fn name(&self) -> &str {
        &self.name
    }

    fn schema(&self) -> &str {
        &self.schema
    }

    fn usage(&self) -> Option<&str> {
        self.usage.as_deref()
    }

    fn retry_count(&self) -> RetryCount {
        RetryCount::none()
    }
}

impl SyncToolHandler for CapTool {
    fn invoke(&self, call: &ToolInvocation<'_>) -> ToolResult<ToolOutput> {
        if call.name() != self.name {
            return Err(ToolError::NotFound(call.name().to_owned()).into());
        }
        call_capability(&self.name, call.arguments_json())
    }
}

pub(crate) fn call_capability(name: &str, arguments_json: &str) -> ToolResult<ToolOutput> {
    let name = cstring(name)?;
    let arguments_json = cstring(arguments_json)?;
    let mut output = vec![0u8; TOOL_OUTPUT_CAPACITY];
    let ctx = ClawCapCallContext::default();
    let err = unsafe {
        claw_cap_call(
            name.as_ptr(),
            arguments_json.as_ptr(),
            &ctx,
            output.as_mut_ptr().cast::<c_char>(),
            output.len(),
        )
    };
    let output = c_buffer_to_string(&output);
    if err == ESP_OK {
        Ok(ToolOutput { output, ok: true })
    } else {
        Err(ToolError::InvokeRejected(output).into())
    }
}

fn c_string(ptr: *const c_char) -> Option<String> {
    if ptr.is_null() {
        return None;
    }
    unsafe { CStr::from_ptr(ptr).to_str().ok().map(str::to_owned) }
}

fn cstring(value: &str) -> Result<CString, ToolInvokeError> {
    CString::new(value)
        .map_err(|_| ToolError::InvalidArguments("string contains nul".into()).into())
}

fn c_buffer_to_string(buffer: &[u8]) -> String {
    let len = match buffer.iter().position(|byte| *byte == 0) {
        Some(len) => len,
        None => buffer.len(),
    };
    let payload = match buffer.get(..len) {
        Some(payload) => payload,
        None => buffer,
    };
    String::from_utf8_lossy(payload).into_owned()
}
