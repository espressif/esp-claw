# Tool

```rust
struct CapabilityRegistry {
    pub fn register(capability: Capability) -> Result<()> // central registration; creates a one-member group
    pub fn register_group(group: CapabilityGroup) -> Result<()> // central registration; group is the enable/disable unit
    pub fn enable_group(group_id: &str) -> Result<()> // central enable; affects every projection's next handle
    pub fn disable_group(group_id: &str) -> Result<()> // central disable; overrides every projection-local enable
    pub fn enable(capability_id:CapabilityId) -> Result<()>
    pub fn disable(capability_id:CapabilityId) -> Result<()>
    pub fn start_all() -> Result<()> // starts lifecycle for centrally-enabled groups
    pub fn stop_all() -> Result<()> // stops lifecycle for started groups
    pub fn tool_set() -> ToolSet // ToolSet must come from the registry
    pub fn channel_set() -> ChannelSet // ChannelSet must come from the registry
    pub fn version() -> u64 // increments after every central visibility/catalog mutation
}

struct ToolInvocation<'a> {
    pub id: Option<&'a str> // raw model/tool-call id; transport metadata only
    pub name: &'a str // raw model-emitted name; not guaranteed to exist in the ToolSet
    pub arguments_json: &'a str // raw JSON object text; not validated until dispatch
}

struct ToolOutput {
    pub output: String // tool-message content returned to the model
    pub ok: bool // false means the tool ran but reported a recoverable failure
}

enum ToolError {
    NotFound(ToolName)
    InvalidArgumentsJson(String)
    InvalidArguments(String)
    InvokeRejected(String)
}

struct ToolInvokeError {
    pub error: ToolError
    pub retries: ToolRetryCount
}

struct ToolRetryCount {
    pub fn none() -> ToolRetryCount
    pub fn extra(extra_attempts: u32) -> ToolRetryCount
}

struct ToolGroup {
    pub fn new(name: ToolGroupName, tools: impl IntoIterator<Item = Tool>) -> ToolGroup // local grouping/provenance
    pub fn name() -> ToolGroupName // group label, not a dispatch namespace
    pub fn tools() -> &[Tool] // borrowed tools in this group
}

trait ToolSpec {
    fn name(&self) -> &str // may return static text or borrow metadata owned by the handler
    fn schema(&self) -> &str // may return static text or borrow metadata owned by the handler
    fn usage(&self) -> Option<&str> // may return static text or borrow metadata owned by the handler
    fn concurrent(&self) -> bool // internal scheduling hint; runner uses it, callers do not
    fn classify(&self, call: &ToolInvocation<'_>) -> Action // describes the call; permission decides the Action
}

trait SyncToolHandler: ToolSpec {
    fn invoke(&self, call: &ToolInvocation<'_>) -> Result<ToolOutput>
}

trait AsyncToolHandler: ToolSpec {
    fn invoke<'a>(&'a self, call: &'a ToolInvocation<'_>) -> ToolFuture<'a>
}

struct Tool {
    pub fn from_sync(handler: impl SyncToolHandler + 'static) -> Tool // adapter for C/immediate tools
    pub fn from_async(handler: impl AsyncToolHandler + 'static) -> Tool // adapter for native async tools
    pub fn name(&self) -> &str // stable id used by Capability::from_tool
}

struct ToolSetCache{
    schemas_json: Option<String>,
    tool_context: Option<String>,
    extra_tool_context: Option<String>,
}

enum ToolSource{
    Registry,
    Local,
}

enum ToolState{
    Enabled,    
    Disabled,
    TemporailyEnabled,
    TemporailyDisabled,
}

struct ToolSet {
    registry: Arc<CapabilityRegistry>,
    tools: HashMap<ToolName, (Tool, ToolSource, ToolState)>,
    cache: ToolSetCache,
    registry_version: u32,

    pub fn add_group(group: ToolGroup) -> Result<()> // projection-local add group
    pub fn add_tool(tool: Tool) -> Result<()> // projection-local add; never writes to CapabilityRegistry
    pub fn remove_tool(name: impl IntoIterator<Item = ToolName>) -> Result<()> // projection-local hide; central registry remains unchanged
    pub fn enable_tool(name: impl IntoIterator<Item = ToolName>) -> Result<()> // clears projection-local disable; cannot override central disable
    pub fn disable_tool(name: impl IntoIterator<Item = ToolName>) -> Result<()> // projection-local disable; only this ToolSet is affected
    pub fn temporarily_enable_tools(names: impl IntoIterator<Item = ToolName>) // soft tools; affects reminders only
    pub fn temporarily_disable_tools(names: impl IntoIterator<Item = ToolName>) // proposed convenience; not in current code
    pub fn clear_temporary_tools() // clears soft-tool phase gating
    pub fn begin() -> Result<ToolSetHandle> // RAII boundary; auto-refreshes then freezes a snapshot

    fn rebuild() -> ToolSetCache
}

struct ToolSetHandle {
    pub fn schemas_json() -> Option<&str> // stable during this handle
    pub fn tool_context() -> Option<&str> // stable during this handle
    pub fn extra_tool_context() -> Option<&str> // current code name; dynamic soft-tool reminder text
    pub async fn invoke(call: ToolInvocation<'_>) -> Result<ToolOutput> // async dispatch; no live registry read
}

trait ToolGate {
    fn decide(&self, action: &Action) -> PermissionDecision
}

struct ApprovalNeeded {
    pub summary: String // human-facing permission summary
    pub signature: String // permission grant/deny key
}

enum ToolRunOutcome {
    Ran { content: String, ok: bool } // tool executed, or a dispatch/tool error was rendered as model-facing content
    Blocked { content: String } // soft tools refused the call before permission
    ApprovalNeeded { content: String, approval: ApprovalNeeded } // permission requested human approval
}

struct ToolRunner<'a> {
    pub fn new(tools: &'a ToolSetHandle, gate: Option<&'a dyn ToolGate>) -> ToolRunner<'a>
    pub async fn run(&self, call: &ToolInvocation<'_>) -> ToolRunOutcome // soft gate -> permission -> retry -> dispatch
}
```
