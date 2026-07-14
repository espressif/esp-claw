const TRANSCRIPT_DIR: &str = "transcript";
const PROFILE_DIR: &str = "profile";
const LONG_TERM_DIR: &str = "long_term";

pub(super) struct FsAgentFactoryLayout {
    pub(super) transcript_dir: String,
    pub(super) profile_dir: String,
    pub(super) long_term_dir: String,
}

impl FsAgentFactoryLayout {
    pub(super) fn new(root: String) -> Self {
        Self {
            transcript_dir: join_storage_path(&root, TRANSCRIPT_DIR),
            profile_dir: join_storage_path(&root, PROFILE_DIR),
            long_term_dir: join_storage_path(&root, LONG_TERM_DIR),
        }
    }
}

pub(super) fn join_storage_path(parent: &str, child: &str) -> String {
    if parent == "/" {
        return format!("/{child}");
    }
    let parent = parent.trim_end_matches('/');
    if parent.is_empty() {
        child.to_string()
    } else {
        format!("{parent}/{child}")
    }
}
