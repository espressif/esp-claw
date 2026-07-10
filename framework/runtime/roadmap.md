# Roadmap

## Stage 1

- [x] keep everything else unchanged with master but not agent
- [ ] better context management
- [ ] cleaned up stale configuration
- [ ] native multimodal agent
- [ ] efficient checkpointing system
- speeded up agent system perf
  - [ ] max_token optimization (reasoning efforts), and per model
  - [x] SSE optimization
- [ ] runtime agent config
- [ ] tool search
- [x] subagent followup(agent-id, message) — cancel current task and retask
- [ ] refactor (cleaned up runtime/)

## Stage 2 - Goal 1

- [ ] DAG powered parallelized toolcalls
- [ ] SSE toocall scheduling
- [ ] evals

## Stage 2 - Goal 2

- [ ] event router refactor (c)
- [ ] event router re-design
- [ ] event router rust async stream rewrite
- [ ] rust wrapper
