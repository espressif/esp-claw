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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn remote_url_passthrough() {
        let asset = MediaAsset::remote_url("https://example.com/a.png");
        let p = prepare_asset(&asset, false, 1024).unwrap();
        assert_eq!(p.kind, PreparedKind::RemoteUrl);
        assert_eq!(p.payload, "https://example.com/a.png");
    }

    #[test]
    fn rejects_empty_remote_url() {
        let asset = MediaAsset::remote_url("");
        let e = prepare_asset(&asset, false, 1024).unwrap_err();
        assert!(matches!(e, InferMediaError::MediaUrlEmpty));
    }

    #[test]
    fn local_path_data_url() {
        let dir = std::env::temp_dir();
        let path = dir.join(format!("claw_media_test_{}.png", std::process::id()));
        std::fs::write(&path, b"\x89PNG\r\n\x1a\nABCDE").unwrap();
        let asset = MediaAsset::local_path(path.to_string_lossy().into_owned());
        let p = prepare_asset(&asset, false, 1024).unwrap();
        assert_eq!(p.kind, PreparedKind::DataUrl);
        assert!(p.payload.starts_with("data:image/png;base64,"));
        let _ = std::fs::remove_file(&path);
    }

    #[test]
    fn rejects_empty_path() {
        let asset = MediaAsset::local_path("");
        let e = prepare_asset(&asset, false, 1024).unwrap_err();
        assert!(matches!(e, InferMediaError::MediaPathEmpty));
    }

    #[test]
    fn rejects_relative_path() {
        let asset = MediaAsset::local_path("rel/a.png");
        let e = prepare_asset(&asset, false, 1024).unwrap_err();
        assert!(matches!(e, InferMediaError::MediaPathNotAbsolute));
    }

    #[test]
    fn rejects_unknown_extension() {
        let asset = MediaAsset::local_path("/tmp/a.bmp");
        let e = prepare_asset(&asset, false, 1024).unwrap_err();
        assert!(matches!(e, InferMediaError::UnsupportedMediaType));
    }

    #[test]
    fn local_path_mime_override_bypasses_extension() {
        let dir = std::env::temp_dir();
        let path = dir.join(format!("claw_media_override_{}.bmp", std::process::id()));
        std::fs::write(&path, b"bmpdata").unwrap();
        // `.bmp` is unsupported by extension, but an explicit override wins.
        let asset =
            MediaAsset::local_path(path.to_string_lossy().into_owned()).with_mime_type("image/png");
        let p = prepare_asset(&asset, false, 1024).unwrap();
        assert!(p.payload.starts_with("data:image/png;base64,"));
        let _ = std::fs::remove_file(&path);
    }

    #[test]
    fn inline_bytes_data_url() {
        let asset = MediaAsset::inline_bytes(b"\x89PNG\r\n\x1a\nABCDE".to_vec(), "image/png");
        let p = prepare_asset(&asset, false, 1024).unwrap();
        assert_eq!(p.kind, PreparedKind::DataUrl);
        assert!(p.payload.starts_with("data:image/png;base64,"));
    }

    #[test]
    fn inline_bytes_respects_size_limit() {
        let asset = MediaAsset::inline_bytes(vec![0u8; 100], "image/png");
        let e = prepare_asset(&asset, false, 50).unwrap_err();
        assert!(matches!(e, InferMediaError::MediaTooLarge));
    }

    #[test]
    fn inline_bytes_rejects_empty() {
        let asset = MediaAsset::inline_bytes(vec![], "image/png");
        let e = prepare_asset(&asset, false, 1024).unwrap_err();
        assert!(matches!(e, InferMediaError::MediaFileEmpty));
    }

    #[test]
    fn inline_bytes_rejects_remote_only_profile() {
        let asset = MediaAsset::inline_bytes(b"x".to_vec(), "image/png");
        let e = prepare_asset(&asset, true, 1024).unwrap_err();
        assert!(matches!(e, InferMediaError::RemoteOnlyProfile));
    }
}
