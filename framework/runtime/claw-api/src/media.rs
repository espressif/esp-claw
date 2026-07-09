//! Media preparation, port of `claw_media_pipeline.c`.
//!
//! Local image files are read with `std::fs` and base64-encoded with the
//! `base64` crate (replacing `fopen`/`mbedtls_base64_encode`).

use base64::engine::general_purpose::STANDARD;
use base64::Engine;

use super::errors::InferMediaError;
use super::types::MediaAsset;

/// How a prepared media payload is encoded (`claw_media_prepared_kind_t`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum PreparedKind {
    DataUrl,
    RemoteUrl,
}

/// Output of the media-prep pipeline (`claw_media_prepared_t`).
#[derive(Clone, Debug)]
pub(crate) struct Prepared {
    kind: PreparedKind,
    /// Data URL (for [`PreparedKind::DataUrl`]) or the remote URL.
    payload: String,
}

impl Prepared {
    pub(crate) fn is_data_url(&self) -> bool {
        self.kind == PreparedKind::DataUrl
    }

    pub(crate) fn payload(&self) -> &str {
        &self.payload
    }
}

/// Mirror of `image_mime_from_path`: extension-based MIME, case-insensitive.
fn image_mime_from_path(path: &str) -> Option<&'static str> {
    let dot = path.rfind('.')?;
    let ext = path[dot..].to_ascii_lowercase();
    match ext.as_str() {
        ".jpg" | ".jpeg" => Some("image/jpeg"),
        ".png" => Some("image/png"),
        ".gif" => Some("image/gif"),
        ".webp" => Some("image/webp"),
        _ => None,
    }
}

fn prepare_local_path_asset(
    path: &str,
    mime_override: Option<&str>,
    image_max_bytes: usize,
) -> Result<Prepared, InferMediaError> {
    if path.is_empty() {
        return Err(InferMediaError::MediaPathEmpty);
    }
    if !path.starts_with('/') {
        return Err(InferMediaError::MediaPathNotAbsolute);
    }

    let mime = mime_override
        .or_else(|| image_mime_from_path(path))
        .ok_or(InferMediaError::UnsupportedMediaType)?;

    let meta = std::fs::metadata(path).map_err(|_| InferMediaError::MediaNotFound)?;
    let size = meta.len() as usize;
    if size == 0 {
        return Err(InferMediaError::MediaFileEmpty);
    }
    if size > image_max_bytes {
        return Err(InferMediaError::MediaTooLarge);
    }

    let raw = std::fs::read(path).map_err(|_| InferMediaError::MediaReadFailed)?;
    if raw.len() != size {
        return Err(InferMediaError::MediaReadFailed);
    }

    let encoded = STANDARD.encode(&raw);
    let payload = format!("data:{mime};base64,{encoded}");

    Ok(Prepared {
        kind: PreparedKind::DataUrl,
        payload,
    })
}

fn prepare_inline_bytes_asset(
    bytes: &[u8],
    mime: &str,
    image_max_bytes: usize,
) -> Result<Prepared, InferMediaError> {
    if bytes.is_empty() {
        return Err(InferMediaError::MediaFileEmpty);
    }
    if bytes.len() > image_max_bytes {
        return Err(InferMediaError::MediaTooLarge);
    }

    let encoded = STANDARD.encode(bytes);
    let payload = format!("data:{mime};base64,{encoded}");

    Ok(Prepared {
        kind: PreparedKind::DataUrl,
        payload,
    })
}

/// `claw_media_prepare_asset`
pub(crate) fn prepare_asset(
    asset: &MediaAsset,
    image_remote_url_only: bool,
    image_max_bytes: usize,
) -> Result<Prepared, InferMediaError> {
    match asset {
        MediaAsset::RemoteUrl { url } => {
            if url.is_empty() {
                return Err(InferMediaError::MediaUrlEmpty);
            }
            Ok(Prepared {
                kind: PreparedKind::RemoteUrl,
                payload: url.clone(),
            })
        }
        MediaAsset::InlineBytes { bytes, mime_type } => {
            if image_remote_url_only {
                return Err(InferMediaError::RemoteOnlyProfile);
            }
            prepare_inline_bytes_asset(bytes, mime_type, image_max_bytes)
        }
        MediaAsset::LocalPath { path, mime_type } => {
            if image_remote_url_only {
                return Err(InferMediaError::RemoteOnlyProfile);
            }
            prepare_local_path_asset(path, mime_type.as_deref(), image_max_bytes)
        }
    }
}
