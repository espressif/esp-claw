use core::ffi::{c_char, CStr};
use std::ffi::CString;

use claw_tool::{
    RetryCount, SyncToolHandler, Tool, ToolError, ToolInvocation, ToolInvokeError, ToolOutput,
    ToolResult, ToolSpec,
};
use serde_json::json;

use crate::abi::{
    claw_cap_call, claw_cap_list, ClawCapCallContext, ClawCapDescriptor,
    CLAW_CAP_FLAG_CALLABLE_BY_LLM, CLAW_CAP_FLAG_ROOT_AGENT_ONLY, CLAW_CAP_KIND_CALLABLE,
    CLAW_CAP_KIND_HYBRID, ESP_OK, TOOL_OUTPUT_CAPACITY,
};

/// Per-submission context a capability call needs (request id, inbound/target
/// routing, …). It rides through the core drive as a type-erased
/// [`claw_tool::SharedContext`] installed by `submit_with_context`, and a
/// [`CapTool`] reads it back with [`claw_tool::current_context`].
///
/// `Send + Sync + 'static` (plain scalars/strings) so it can be stored in a
/// `SharedContext` and observed on the orchestrator's drive worker thread.
#[derive(Clone, Default)]
pub(crate) struct CapabilityContextData {
    pub request_id: u32,
    pub channel: Option<String>,
    pub chat_id: Option<String>,
    pub target_channel: Option<String>,
    pub target_chat_id: Option<String>,
    pub source_cap: Option<String>,
}

/// The current capability context installed for the in-flight submission, or a
/// default (empty) context when none is installed.
fn current_context() -> CapabilityContextData {
    claw_tool::current_context::<CapabilityContextData>()
        .map(|context| (*context).clone())
        .unwrap_or_default()
}

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

    for index in 0..list.count {
        let descriptor =
            unsafe { list.items.add(index).as_ref() }.ok_or(CapToolError::InvalidDescriptor)?;
        if !is_llm_tool(descriptor) {
            continue;
        }
        registry.register(Tool::from_sync(CapTool::try_from(descriptor)?))?;
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

        let parameters = serde_json::from_str::<serde_json::Value>(&input_schema)
            .map_err(|error| CapToolError::InvalidSchema(error.to_string()))?;
        let schema = json!({
            "type": "function",
            "function": {
                "name": &name,
                "description": description.as_deref().unwrap_or(""),
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
        call_capability(&self.name, call.arguments_json(), &current_context())
    }
}

pub(crate) fn call_capability(
    name: &str,
    arguments_json: &str,
    context: &CapabilityContextData,
) -> ToolResult<ToolOutput> {
    let name = cstring(name)?;
    let arguments_json = cstring(arguments_json)?;
    let channel = optional_cstring(context.channel.as_deref())?;
    let chat_id = optional_cstring(context.chat_id.as_deref())?;
    let target_channel = optional_cstring(context.target_channel.as_deref())?;
    let target_chat_id = optional_cstring(context.target_chat_id.as_deref())?;
    let source_cap = optional_cstring(context.source_cap.as_deref())?;
    let mut output = vec![0u8; TOOL_OUTPUT_CAPACITY];
    let ctx = ClawCapCallContext {
        request_id: context.request_id,
        channel: c_ptr(&channel),
        chat_id: c_ptr(&chat_id),
        target_channel: c_ptr(&target_channel),
        target_chat_id: c_ptr(&target_chat_id),
        source_cap: c_ptr(&source_cap),
        ..ClawCapCallContext::default()
    };
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

fn optional_cstring(value: Option<&str>) -> Result<Option<CString>, ToolInvokeError> {
    value.map(cstring).transpose()
}

fn c_ptr(value: &Option<CString>) -> *const c_char {
    value
        .as_ref()
        .map_or(core::ptr::null(), |value| value.as_ptr())
}

fn c_buffer_to_string(buffer: &[u8]) -> String {
    let len = buffer
        .iter()
        .position(|byte| *byte == 0)
        .unwrap_or(buffer.len());
    let payload = match buffer.get(..len) {
        Some(payload) => payload,
        None => buffer,
    };
    String::from_utf8_lossy(payload).into_owned()
}
