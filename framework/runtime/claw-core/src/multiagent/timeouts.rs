use core::future::Future;
use core::pin::Pin;
use core::task::{Context, Poll};
use std::collections::BTreeMap;

use claw_interface::{Cancel, ClawTimer};

use crate::protocol::AgentId;

use super::model::SubagentTimeout;

type TimeoutFuture = Pin<Box<dyn Future<Output = AgentId>>>;

struct TimeoutEntry {
    timeout: SubagentTimeout,
    future: TimeoutFuture,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) struct ExpiredTimeout {
    pub(super) agent: AgentId,
    pub(super) timeout: SubagentTimeout,
}

/// Process-local timers for every live non-root node.
///
/// The timeout configuration itself belongs to durable graph metadata. These
/// futures are deliberately disposable: restore re-arms a fresh full duration,
/// and removing a subtree drops every matching future atomically with its graph
/// nodes.
#[derive(Default)]
pub(super) struct AgentTimeouts {
    entries: BTreeMap<AgentId, TimeoutEntry>,
}

impl AgentTimeouts {
    pub(super) fn arm<Timer>(&mut self, agent: AgentId, timeout: SubagentTimeout)
    where
        Timer: ClawTimer + Default + 'static,
    {
        let future = Box::pin(async move {
            let mut timer = Timer::default();
            let _ = timer.sleep(timeout.duration(), Cancel::never()).await;
            agent
        });
        assert!(
            self.entries
                .insert(agent, TimeoutEntry { timeout, future })
                .is_none(),
            "subagent timeout already armed: {agent}"
        );
    }

    pub(super) fn remove(&mut self, agent: AgentId) -> bool {
        self.entries.remove(&agent).is_some()
    }

    pub(super) fn has_pending(&self) -> bool {
        !self.entries.is_empty()
    }

    pub(super) fn next_expired(&mut self) -> NextExpired<'_> {
        NextExpired { timeouts: self }
    }

    #[cfg(test)]
    fn contains(&self, agent: AgentId) -> bool {
        self.entries.contains_key(&agent)
    }
}

pub(super) struct NextExpired<'a> {
    timeouts: &'a mut AgentTimeouts,
}

impl Future for NextExpired<'_> {
    type Output = Vec<ExpiredTimeout>;

    fn poll(self: Pin<&mut Self>, context: &mut Context<'_>) -> Poll<Self::Output> {
        let this = self.get_mut();
        let mut expired = Vec::new();
        for (&agent, entry) in &mut this.timeouts.entries {
            if entry.future.as_mut().poll(context).is_ready() {
                expired.push(ExpiredTimeout {
                    agent,
                    timeout: entry.timeout,
                });
            }
        }
        if expired.is_empty() {
            return Poll::Pending;
        }
        for timeout in &expired {
            this.timeouts.entries.remove(&timeout.agent);
        }
        Poll::Ready(expired)
    }
}

#[cfg(test)]
mod tests {
    use claw_interface::ImmediateTimer;
    use futures_lite::future::block_on;

    use super::{AgentTimeouts, SubagentTimeout};
    use crate::protocol::AgentId;

    fn timeout(milliseconds: u32) -> SubagentTimeout {
        SubagentTimeout::from_millis(milliseconds).expect("non-zero timeout")
    }

    #[test]
    fn expiry_is_ordered_and_consumes_the_registered_timers() {
        let mut timeouts = AgentTimeouts::default();
        timeouts.arm::<ImmediateTimer>(AgentId(3), timeout(30));
        timeouts.arm::<ImmediateTimer>(AgentId(2), timeout(20));

        let expired = block_on(timeouts.next_expired());

        assert_eq!(
            expired
                .iter()
                .map(|expired| (expired.agent, expired.timeout.millis()))
                .collect::<Vec<_>>(),
            vec![(AgentId(2), 20), (AgentId(3), 30)]
        );
        assert!(!timeouts.has_pending());
    }

    #[test]
    fn removing_a_timer_drops_it_before_it_can_expire() {
        let mut timeouts = AgentTimeouts::default();
        let agent = AgentId(2);
        timeouts.arm::<ImmediateTimer>(agent, timeout(20));

        assert!(timeouts.remove(agent));
        assert!(!timeouts.contains(agent));
        assert!(!timeouts.has_pending());
    }
}
