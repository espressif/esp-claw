"""Tree/context-reconstruction tests using the spec's worked example."""

from __future__ import annotations

from claw_trace import build_forest, render_tree

# Compact same-task fixture for grouped-context inheritance and shadowing.
SPEC_EXAMPLE = """\
TRACE 2100 enter <span=1 parent=none task=main span-name=session target=claw_core::orchestrator> <context=run system=agent-system session=session-1>
TRACE 2105 enter <span=2 parent=1 task=main span-name=turn target=claw_core::orchestrator> <context=run turn=7> message_id=m1 cause=message
TRACE 2110 enter <span=3 parent=2 task=main span-name=agent target=claw_core::agent::registry> <context=run agent=agent-1> kind=conversation depth=0
TRACE 2112 enter <span=4 parent=3 task=main span-name=iteration_loop target=claw_core::iteration_loop> <context=run iteration=iteration-0>
TRACE 2120 enter <span=5 parent=4 task=main span-name=agent target=claw_core::agent::registry> <context=run agent=agent-2> kind=tool depth=1
TRACE 2121 event <span=5 task=main event-name=spawned target=claw_core::agent::registry> parent_agent=agent-1 child_agent=agent-2
TRACE 2130 exit <span=5 task=main>
TRACE 2150 event <span=4 task=main event-name=completion target=claw_core::iteration_loop> status=done 👋 Hello!
TRACE 2152 exit <span=4 task=main>
TRACE 2154 exit <span=3 task=main>
TRACE 2156 exit <span=2 task=main>
TRACE 2158 exit <span=1 task=main>
"""


def test_hierarchy_and_durations() -> None:
    forest = build_forest(SPEC_EXAMPLE)

    assert [root.id for root in forest.roots] == [1]
    session = forest.roots[0]
    assert session.name == 'session'
    assert session.duration_ms == 58  # 2158 - 2100

    turn = session.children[0]
    assert turn.id == 2 and turn.name == 'turn'
    agent1 = turn.children[0]
    assert agent1.id == 3 and agent1.name == 'agent'
    iteration = agent1.children[0]
    assert iteration.id == 4 and iteration.name == 'iteration_loop'
    agent2 = iteration.children[0]
    assert agent2.id == 5 and agent2.name == 'agent'
    assert agent2.duration_ms == 10  # 2130 - 2120


def test_inherited_context_is_prefix_closed_and_grouped() -> None:
    forest = build_forest(SPEC_EXAMPLE)
    iteration = forest.spans[4]
    # system -> session -> turn -> agent -> iteration, agent-1 in this subtree.
    assert iteration.context == {
        'run': {
            'system': 'agent-system',
            'session': 'session-1',
            'turn': '7',
            'agent': 'agent-1',
            'iteration': 'iteration-0',
        }
    }
    # The opened set on the iteration span is only the key it introduces.
    assert iteration.opened_context == {'run': {'iteration': 'iteration-0'}}


def test_subagent_shadow_overrides_agent() -> None:
    forest = build_forest(SPEC_EXAMPLE)
    # span 5 reopens `run.agent`, shadowing agent-1 with agent-2.
    spawned = next(e for e in forest.events if e.name == 'spawned')
    assert spawned.context == {
        'run': {
            'system': 'agent-system',
            'session': 'session-1',
            'turn': '7',
            'agent': 'agent-2',
            'iteration': 'iteration-0',
        }
    }
    # The completion event under the iteration span keeps agent-1.
    completion = next(e for e in forest.events if e.name == 'completion')
    assert completion.context == {
        'run': {
            'system': 'agent-system',
            'session': 'session-1',
            'turn': '7',
            'agent': 'agent-1',
            'iteration': 'iteration-0',
        }
    }


def test_events_anchor_to_their_span() -> None:
    forest = build_forest(SPEC_EXAMPLE)
    assert [e.name for e in forest.spans[5].events] == ['spawned']
    assert [e.name for e in forest.spans[4].events] == ['completion']
    assert forest.orphan_events == []


def test_orphan_event_when_span_absent() -> None:
    forest = build_forest(
        'TRACE 1 event <span=none task=main event-name=boot target=t> starting'
    )
    assert len(forest.orphan_events) == 1
    assert forest.orphan_events[0].name == 'boot'
    assert forest.roots == []


def test_render_tree_smoke() -> None:
    rendered = render_tree(build_forest(SPEC_EXAMPLE))
    assert '[session]' in rendered
    assert 'spawned' in rendered
    assert 'agent=agent-2' in rendered
