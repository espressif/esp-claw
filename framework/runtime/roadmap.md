# Roadmap

## Stage 1 - Goal 1

- [x] keep everything else unchanged with master but not agent
- [ ] better context management
- [ ] cleaned up stale configuration
- [x] cleaned up sse bad designs
- [ ] cleaned up multiagent bugs
- [ ] efficient checkpointing system
- speeded up agent system perf
  - [x] max_token optimization (reasoning efforts), and per model
  - [x] SSE optimization
- [x] runtime agent config
- [x] tool search
- [x] subagent followup(agent-id, message) — cancel current task and retask
- [x] refactor (cleaned up runtime/)
- [ ] reduced memory use by 80%
- [ ] test plan
  - [ ] skills
  - [ ] tools
  - [ ] multiagent

## Stage 1 - Goal 2

- [ ] native multimodal agent

## Stage 2 - Goal 1

- [ ] DAG powered parallelized toolcalls
- [ ] SSE toocall scheduling
- [ ] evals

## Stage 2 - Goal 2

- [ ] event router refactor (c)
- [ ] event router re-design
- [ ] event router rust async stream rewrite
- [ ] rust wrapper
