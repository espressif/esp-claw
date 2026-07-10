use strum::IntoStaticStr;

use super::approval::ApprovalResolverError;
use super::instance::{ApprovalResolutionError, InstanceDeliverError};

#[derive(Debug, IntoStaticStr, thiserror::Error)]
pub(super) enum DeliverError {
    #[strum(serialize = "agent")]
    #[error(transparent)]
    Instance(#[from] InstanceDeliverError),
    #[strum(serialize = "agent")]
    #[error(transparent)]
    ApprovalResolver(#[from] ApprovalResolverError),
    #[strum(serialize = "agent")]
    #[error(transparent)]
    ApprovalResolution(#[from] ApprovalResolutionError),
}
