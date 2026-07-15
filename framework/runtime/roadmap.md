# Roadmap

## Stage 1 - Goal 1

- [x] keep everything else unchanged with master but not agent
- [ ] better context management
- [x] cleaned up stale configuration
- [x] cleaned up sse bad designs
- [x] cleaned up multiagent bugs
- [ ] efficient checkpointing system
- speeded up agent system perf
  - [x] max_token optimization (reasoning efforts), and per model
  - [x] SSE optimization
- [x] runtime agent config
- [x] tool search
- [x] subagent followup(agent-id, message) — cancel current task and retask
- [x] refactor (cleaned up runtime/)
- [ ] reduced memory use by 80%
- [ ] plan mode
- [ ] test plan
  - [ ] skills
  - [ ] tools
  - [ ] multiagent

## Stage 1 - Goal 2

- [ ] native multimodal agent

## Stage 2 - Goal 1

- [ ] decouple agents and tools through a generic event bus
  - [ ] preserve baked `tool_groups` as the strict per-agent capability allowlist
  - [ ] apply the allowlist before tool discovery and loading
  - [ ] keep `default_visibility` solely for controlling tool-schema context size
- [ ] DAG powered parallelized toolcalls
- [ ] SSE toocall scheduling
- [ ] evals

## Stage 2 - Goal 2

- [ ] event router refactor (c)
- [ ] event router re-design
- [ ] event router rust async stream rewrite
- [ ] rust wrapper
