//! Strongly typed runtime identifiers and their local allocators.

crate::define_prefixed_id!(AgentId, "agent-", "agent");
crate::define_prefixed_id!(InputRequestId, "input-", "input request");
crate::define_prefixed_id!(IterationId, "iteration-", "iteration");
crate::define_prefixed_id!(SessionId, "session-", "session");
crate::define_prefixed_id!(TurnId, "turn-", "turn");

crate::define_id_allocator!(
    /// Hands out session-local input request ids.
    pub(crate) InputRequestIdAllocator(InputRequestId),
    InputRequestId(1)
);

crate::define_id_allocator!(
    /// Reset for each agent task.
    pub(crate) IterationIdAllocator(IterationId),
    IterationId(0)
);

crate::define_id_allocator!(
    /// Hands out process-unique session ids for the current runtime.
    pub(crate) SessionIdAllocator(SessionId),
    SessionId(1)
);

crate::define_id_allocator!(
    /// Hands out session-local turn ids.
    pub(crate) TurnIdAllocator(TurnId),
    TurnId(1)
);
