# Tool

```rust
struct ToolProjection {
    registry_version: ToolRegistryVersion
    tools: Vec<ToolProjectionEntry>
}

struct ToolRegistry {
    tools: HashMap<ToolName, ToolRegistryEntry>,
    started: bool,
    version: ToolRegistryVersion,

    pub fn register(tool: Tool) -> Result<()> // central tool catalog registration
    pub fn enable(name: &str) -> Result<()> // central visibility enable
    pub fn disable(name: &str) -> Result<()> // central visibility disable
    pub fn start_all() -> Result<()> // registered enabled tools become visible to projections
    pub fn stop_all() -> Result<()> // registry tools disappear from projections
    pub fn tool_set() -> ToolSet // creates a projection bound to this registry
    pub fn tool_version() -> ToolRegistryVersion // bumps on catalog/visibility/lifecycle mutation
}

struct ToolRegistryEntry {
    tool: Tool,
    enabled: bool,
}

struct RawToolInvocation<'a> {
    pub id: Option<&'a str> // raw model/tool-call id; transport metadata only
    pub name: &'a str // raw model-emitted name; not guaranteed to exist in the ToolSet
    pub arguments_json: &'a str // raw JSON object text; validated by ToolInvocation::try_from
}

struct ToolInvocation<'a> {
    id: Option<&'a str>
    name: &'a str
    arguments_json: &'a str

    pub fn try_from(raw: RawToolInvocation<'a>) -> Result<ToolInvocation<'a>> // validates arguments_json as a JSON object
    pub fn id() -> Option<&str>
    pub fn name() -> &str
    pub fn arguments_json() -> &str
    pub fn arguments_value() -> Result<Value>
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
}

struct RetryCount {
    pub fn none() -> RetryCount
    pub fn extra(extra_attempts: u32) -> RetryCount
}

trait ToolSpec {
    fn name(&self) -> &str // may return static text or borrow metadata owned by the handler
    fn schema(&self) -> &str // may return static text or borrow metadata owned by the handler
    fn usage(&self) -> Option<&str> // may return static text or borrow metadata owned by the handler
    fn concurrent(&self) -> bool // internal scheduling hint; runner uses it, callers do not
    fn retry_count(&self) -> RetryCount // tool-owned retry policy; runner does not know retry
    fn classify(&self, call: &ToolInvocation<'_>) -> Action // describes the call; permission decides the Action
}

trait SyncToolHandler: ToolSpec {
    fn invoke(&self, call: &ToolInvocation<'_>) -> Result<ToolOutput>
}

trait AsyncToolHandler: ToolSpec {
    fn invoke<'a>(&'a self, call: &'a ToolInvocation<'_>) -> ToolFuture<'a>
}

mod bake {
    pub fn validate_tools_dir(tools_dir: &Path) -> Result<usize> // build-time check for resources/tools
}

macro_rules! tool_metadata // generates name/schema/usage from resources/tools/<name>

struct Tool {
    pub fn from_sync(handler: impl SyncToolHandler + 'static) -> Tool // adapter for C/immediate tools
    pub fn from_async(handler: impl AsyncToolHandler + 'static) -> Tool // adapter for native async tools
    pub fn name(&self) -> &str // stable id used by Capability::Tool
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
    registry: Arc<ToolRegistry>,
    tools: HashMap<ToolName, (Tool, ToolSource, ToolState)>,
    cache: ToolSetCache,
    registry_version: ToolRegistryVersion,

    pub fn add_tool(tool: Tool) -> Result<()> // projection-local add; never writes to ToolRegistry
    pub fn remove_tool(name: ToolName) -> Result<()> // projection-local remove; local tools only
    pub fn enable_tool(name: ToolName) -> Result<()> // clears projection-local disable; cannot override central disable
    pub fn disable_tool(name: ToolName) -> Result<()> // projection-local disable; only this ToolSet is affected
    pub fn temporarily_enable_tool(name: ToolName) -> Result<()> // soft tools; affects reminders only
    pub fn temporarily_disable_tool(name: ToolName) -> Result<()> // soft tools; affects reminders only
    pub fn clear_temporary_tools() // clears soft-tool phase gating
    pub fn begin() -> Result<ToolSetHandle> // RAII boundary; auto-refreshes then freezes a snapshot

    fn rebuild()
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
