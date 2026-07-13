use claw_context::{Block, BlockKind, Context, ContextItem};
use serde_json::Value;

#[test]
fn blocks_render_in_wire_order_regardless_of_declaration_order() {
    let mut context = Context::new();
    context
        .with(Block::new(BlockKind::RecentContext, "RECENT"))
        .with(Block::new(BlockKind::OutputContract, "OUTPUT"))
        .with(Block::new(BlockKind::AgentInstruction, "AGENT"))
        .with(Block::new(BlockKind::ConversationSummary, "SUMMARY"));

    assert_eq!(
        system_of(&mut context),
        "AGENT\n\nSUMMARY\n\nRECENT\n\nOUTPUT"
    );
}

#[test]
fn profile_blocks_render_before_global_memory() {
    let mut context = Context::new();
    context
        .with(Block::new(BlockKind::GlobalMemory, "MEMORY"))
        .with(Block::new(BlockKind::UserProfile, "USER"))
        .with(Block::new(BlockKind::Soul, "SOUL"))
        .with(Block::new(BlockKind::AssistantIdentity, "IDENTITY"));

    assert_eq!(
        system_of(&mut context),
        "SOUL\n\nIDENTITY\n\nUSER\n\nMEMORY"
    );
}

#[test]
fn reasoning_effort_follows_mode_and_precedes_conversation_summary() {
    let mut context = Context::new();
    context
        .with(Block::new(BlockKind::ConversationSummary, "SUMMARY"))
        .with(Block::new(BlockKind::ReasoningEffort, "EFFORT"))
        .with(Block::new(BlockKind::SkillList, "SKILLS"))
        .with(Block::new(BlockKind::ModeFraming, "MODE"));

    assert_eq!(
        system_of(&mut context),
        "SKILLS\n\nMODE\n\nEFFORT\n\nSUMMARY"
    );
}

#[test]
fn empty_content_is_absent_and_drops_the_key() {
    let mut context = Context::new();
    context
        .with(Block::new(BlockKind::AgentInstruction, "AGENT"))
        .with(Block::new(BlockKind::ToolPolicy, ""))
        .with(Block::new(BlockKind::RecentContext, "   \n  "));
    assert_eq!(system_of(&mut context), "AGENT");
}

#[test]
fn redeclaring_identical_content_does_not_bump_version() {
    let mut context = Context::new();
    context.with(Block::new(BlockKind::AgentInstruction, "PERSONA"));
    let version = context.version();
    context
        .with(Block::new(BlockKind::AgentInstruction, "PERSONA"))
        .with(Block::new(BlockKind::AgentInstruction, "PERSONA"));
    assert_eq!(context.version(), version);
}

#[test]
fn changing_content_bumps_version_and_rerenders() {
    let mut context = Context::new();
    context.with(Block::new(BlockKind::AgentInstruction, "OLD"));
    assert_eq!(system_of(&mut context), "OLD");
    let version = context.version();

    context.with(Block::new(BlockKind::AgentInstruction, "NEW"));
    assert!(context.version() > version);
    assert_eq!(system_of(&mut context), "NEW");
}

#[test]
fn undeclared_block_keeps_its_value() {
    let mut context = Context::new();
    context
        .with(Block::new(BlockKind::AgentInstruction, "PERSONA"))
        .with(Block::new(BlockKind::SkillList, "SKILL"));
    assert_eq!(system_of(&mut context), "PERSONA\n\nSKILL");

    context.with(Block::new(BlockKind::SkillList, "SKILL2"));
    assert_eq!(system_of(&mut context), "PERSONA\n\nSKILL2");
}

#[test]
fn setting_empty_removes_a_previously_set_block() {
    let mut context = Context::new();
    context
        .with(Block::new(BlockKind::AgentInstruction, "PERSONA"))
        .with(Block::new(BlockKind::SkillList, "SKILL"));
    assert_eq!(system_of(&mut context), "PERSONA\n\nSKILL");

    context.with(Block::new(BlockKind::SkillList, ""));
    assert_eq!(system_of(&mut context), "PERSONA");
}

#[test]
fn reminder_feeds_the_tail_without_touching_version() {
    let mut context = Context::new();
    context.with(Block::new(BlockKind::AgentInstruction, "PERSONA"));
    let version = context.version();

    context.reminder(Some("only these tools"));
    assert_eq!(context.version(), version);

    let history = Value::Array(vec![]);
    let request = context.request(&history);
    assert_eq!(request.reminders().len(), 1);
    assert_eq!(
        reminder_content(request.reminders().first()),
        Some("<system-reminder>\nonly these tools\n</system-reminder>")
    );
}

#[test]
fn reminders_render_in_wire_order_and_clear_by_kind() {
    let mut context = Context::new();
    context
        .with_reminder(BlockKind::OutputContract, Some("output"))
        .with_reminder(BlockKind::ToolReminder, Some("tools"));

    let history = Value::Array(vec![]);
    let request = context.request(&history);
    assert_eq!(request.reminders().len(), 2);
    assert_eq!(
        reminder_content(request.reminders().first()),
        Some("<system-reminder>\ntools\n</system-reminder>")
    );

    context.with_reminder(BlockKind::ToolReminder, None);
    let request = context.request(&history);
    assert_eq!(request.reminders().len(), 1);
    assert_eq!(
        reminder_content(request.reminders().first()),
        Some("<system-reminder>\noutput\n</system-reminder>")
    );
}

#[test]
fn sink_routes_items_to_their_request_channels() {
    let mut context = Context::new();
    let recent = serde_json::json!({ "role": "user", "content": "hello" });
    let summary = serde_json::json!({ "role": "assistant", "content": "summary" });

    let history = {
        let mut sink = context.sink();
        sink.item(ContextItem::block(BlockKind::AgentInstruction, "PERSONA"));
        sink.item(ContextItem::message(BlockKind::RecentContext, &recent));
        sink.item(ContextItem::message(
            BlockKind::ConversationSummary,
            &summary,
        ));
        sink.item(ContextItem::reminder(
            BlockKind::ToolReminder,
            "only these tools",
        ));
        sink.into_history()
    };

    let request = context.request(&history);
    assert_eq!(request.system(), "PERSONA");
    assert_eq!(request.history(), &serde_json::json!([summary, recent]));
    assert_eq!(request.reminders().len(), 1);
}

fn system_of(context: &mut Context) -> String {
    let history = Value::Array(vec![]);
    context.request(&history).system().to_string()
}

fn reminder_content(message: Option<&Value>) -> Option<&str> {
    message
        .and_then(|message| message.get("content"))
        .and_then(Value::as_str)
}
