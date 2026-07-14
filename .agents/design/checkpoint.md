# Runtime Durable Tree

```text
Orchestrator
├── SessionStore
│   ├── persistent sessions
│   └── next session id
├── AgentIdAllocator
└── SessionActor[session_id]
    ├── SessionState
    │   ├── active turn / pending input
    │   ├── next turn id
    │   └── reasoning effort
    └── MultiagentRuntime
        ├── MultiagentState
        │   ├── graph topology
        │   ├── ready queue
        │   └── parked approvals
        └── AgentSlot[agent_id]
            ├── inbox
            └── BaseAgent durable parts
                ├── BaseAgentState
                └── ToolSetState
```

Running futures, event sinks, actor command channels, wakers, abort handles,
foreground completion senders, and inspection snapshots are process-local.

## Checkpoint batches

```text
Batch: orchestrator[1]
└── agent-id-allocator

Batch: session-runtime[session_id]       // atomic
├── session-state
└── multiagent-runtime
    └── AgentSlot[*] durable envelopes

Batch: session-registry[1]
└── session-store

Batch: tool-registry[1]
└── tool-registry
```

The atomic session unit is:

```text
session-state + multiagent-runtime = session-runtime[session_id]
```

A running agent tick cannot be serialized. The session actor defers checkpoint
publication until its stable slots are checkpoint-ready; close first stops all
work and then writes the final batch.

## External file-backed stores

```text
TranscriptStore       // owns append/load
LongTermMemory        // owns append/load
ProfileStore          // owns file read/write
Soul/Profile markdown // owns file read/write
SkillCatalog          // reloaded from filesystem
```

These payloads are not duplicated into the session checkpoint. Context adapters
are derived from these stores and are not durable parts.

## API Design

```rust

type SchemaVersion = u32

struct PartStateBlob<'a> {
    schema_version: SchemaVersion,
    bytes: Cow<'a, [u8]>,
}

struct PartStateSlice<'a> {
    schema_version: SchemaVersion,
    bytes: &'a [u8],
}

enum StorageSizeHint {
    Small, // Usually cheap to copy, hash, and keep inline with the batch.
    Large, // May be worth chunking, deduplicating, or writing out of line.
}

enum ChangePatternHint {
    Arbitrary,    // Changes may touch any byte; use the normal snapshot path.
    AppendLikely, // Often old bytes plus a tail; verify before storing a tail delta.
}

struct StorageHint {
    size: StorageSizeHint,
    change: ChangePatternHint,
}

type PartGeneration = u64
type PartName = &'static str

// minimal logical persistence unit
trait DurablePart {
    fn name(&self) -> PartName
    fn generation(&self) -> PartGeneration // part-local monotonic change generation
    fn export_state(&self) -> Result<PartStateBlob<'_>> // it is caller's responsibility to return a blob that can be parsed back
    fn restore_from_state(state: PartStateSlice<'_>) -> Result<Self>
    fn storage_hint(&self) -> StorageHint
    where
        Self: Sized
}

define_prefix_id!(BatchId, "batch-","batch")

type BatchGeneration = u64

type BatchName = &'static str

type BatchKey = (BatchName, BatchId)

struct BatchRef {
    key: BatchKey,
    generation: BatchGeneration,
}

enum DependencyRequirement {
    Equal,
    AtLeast,
    AtMost,
}

// minimal strong sync unit
trait DurableBatch {
    fn name(&self) -> BatchName
    fn id(&self) -> BatchId
    fn parts(&self) -> Vec<&dyn DurablePart> // immutable ref
    fn as_ref(&self) -> BatchRef
    fn depends_on(&self) -> Vec<(BatchRef, DependencyRequirement)>

    fn refresh_generation(&mut self)
}

type CheckpointStep = u64

struct PartWrite<'a> {
    name: PartName,
    state: PartStateBlob<'a>,
    hint: StorageHint,
}

struct BatchWrite<'a> {
    batch: BatchKey,
    writes: Vec<PartWrite<'a>>, // dirty part payloads only
}

struct CheckpointWrite<'a> {
    step: CheckpointStep,
    batches: Vec<BatchWrite<'a>>,
}

struct BatchCheckpoint<'a> {
    batch: BatchKey,
    parts: Vec<(PartName, PartStateSlice<'a>)>,
}

struct Checkpoint<'a> {
    step: CheckpointStep,
    batches: Vec<BatchCheckpoint<'a>>,
}

trait CheckpointStorage {
    pub fn new(path: String) -> CheckpointStorage

    fn latest_step(&self) -> Result<Option<CheckpointStep>>
    fn next_step(&mut self) -> Result<CheckpointStep>
    fn write_checkpoint(&mut self, checkpoint: CheckpointWrite<'_>) -> Result<()> {
        // Paths:
        // - manifest path: {storage}/manifest.json, written with ClawFs::write_atomic
        // - checkpoint path: {storage}/{checkpoint.step}/...
        //
        // Write logic:
        // 1. Create a staging directory for checkpoint.step.
        // 2. For each BatchWrite, write only its PartWrite entries.
        // 3. If there is no previous checkpoint, every PartWrite is stored as a full part.
        // 4. If there is a previous checkpoint, storage may encode each PartWrite as full or delta.
        // 5. Write a checkpoint-local index that records, for each written part:
        //    batch key, part name, schema version, encoding, hash, and physical object reference.
        // 6. fsync checkpoint data if required by the target platform.
        // 7. Atomically update manifest.json so checkpoint.step becomes the latest valid checkpoint.
        // 8. If any step fails before manifest update, the checkpoint is ignored.
        //
        // Storage hint optimization:
        // - Small + Arbitrary:
        //   Write full bytes. Diffing is usually more expensive than replacing.
        // - Small + AppendLikely:
        //   Write full bytes. Append optimization is not worth the extra bookkeeping.
        // - Large + Arbitrary:
        //   chunk delta if supported; otherwise write full bytes.
        // - Large + AppendLikely:
        //   Compare with previous materialized part. If old bytes are a prefix of new bytes,
        //   write an append delta. Otherwise fall back to Large + Arbitrary behavior.
    }
    fn load_checkpoint(&self, step: CheckpointStep) -> Result<Checkpoint<'_>, LoadCheckpointError>
}

struct CheckpointCoordinator<S: CheckpointStorage> {
    storage: S,
    batches: Vec<Box<dyn DurableBatch>>,

    checkpoint_interval: CheckpointStep,
    history_checkpoints: CheckpointStep,

    last_physical_checkpoint_tick: Option<CheckpointStep>,
    current_checkpoint_tick: Option<CheckpointStep>,

    clean_batches: HashMap<BatchKey, BatchGeneration>,
    clean_parts: HashMap<(BatchKey, PartName), PartGeneration>,

    heads: HashMap<BatchKey, BatchRef>,
}

impl<S: CheckpointStorage> CheckpointCoordinator<S> {
    // checkpoint_interval: how often physical checkpointing runs after logical changes
    // history_checkpoints: how many history checkpoints storage should keep
    pub fn new(storage: S, checkpoint_interval: CheckpointStep, history_checkpoints: CheckpointStep) -> Result<CheckpointCoordinator<S>, CheckpointCoordinatorInitError>{
        // 1. we need to verify history_checkpoints>=1
        // 2. we need to load from the storage and see how many history checkpoints there are in the storge, if it is more than history_checkpoints, keeping removing the oldest ones until satified
    }
    pub fn add_batch(&mut self, batch: Box<dyn DurableBatch>) -> &mut Self
    pub fn maybe_checkpoint(&mut self) -> Result<(), CheckpointError> {
        // 1. Ask every batch to refresh its batch generation.
        // 2. Collect current BatchRef for every managed batch.
        // 3. Compare current refs with clean_batches to find changed batches.
        // 4. If no batch changed, return.
        // 5. If batches changed but checkpoint interval is not reached, return.
        // 6. Build candidate_heads from current published heads.
        // 7. Add changed batches to the checkpoint plan and candidate_heads.
        // 8. Resolve depends_on against candidate_heads until the plan is stable.
        //    If a dependency needs a managed batch that is not planned yet, add it.
        //    If a dependency cannot be satisfied, return CheckpointError.
        // 9. Build BatchWrite for every planned batch.
        //    parts is the full current part index.
        //    writes contains dirty part payloads only.
        // 10. Build one CheckpointWrite from all planned BatchWrite records.
        // 11. Call storage.write_checkpoint() to atomically publish the checkpoint.
        // 12. Only after write_checkpoint succeeds, update clean_batches, clean_parts, and heads.
    }
}
```

## Rules

```rust
// Dirty signal lives in batch and part generations.
batch_business_mutation() -> batch.generation += 1
part_business_mutation() -> part.generation += 1

// DurablePart exposes only monotonic generation, not dirty/clean state.
DurablePart::is_dirty() // forbidden
DurablePart::clear_dirty() // forbidden

// DurableBatch is only the atomic grouping boundary.
SessionState + MultiagentRuntime -> SessionRuntime[session_id]

// DurableBatch exposes current generation, but does not own clean state or checkpoint state.
DurableBatch::mark_clean() // forbidden

// The checkpoint framework owns the clean line.
clean_generations[(batch, part)] = last durable generation

// Dirty check is generation comparison at checkpoint boundary.
part.generation() > clean_generations[(batch, part)] // dirty

// Normal checkpoint selection.
coordinator.maybe_checkpoint() -> build CheckpointWrite -> storage.write_checkpoint()

// After write_checkpoint succeeds, the framework records exported generations as clean.
clean_generations[(batch, part)] = exported_part.generation

// If mutation races with export, only the captured generation becomes clean.
newer_generation > exported_part.generation // remains dirty

// DurablePart has no step, version, file, checkpoint, or global restore context knowledge.
DurablePart -> DurableBatch -> Checkpoint

// Restore is owned by runtime objects that can provide parent resources.
SessionActor::restored(session, persistence, factory, agent_id_allocator, restore)
MultiagentRuntime::from_restored_state(session, factory, agent_id_allocator, restore)

// Checkpoint is the recovery anchor.
latest valid checkpoint -> restore cut
latest batch snapshot without checkpoint -> ignored
```
