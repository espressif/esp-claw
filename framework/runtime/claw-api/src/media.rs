//! Media preparation, port of `claw_media_pipeline.c`.
//!
//! Local image files are read with `std::fs` and base64-encoded with the
//! `base64` crate (replacing `fopen`/`mbedtls_base64_encode`).

use base64::engine::general_purpose::STANDARD;
use base64::Engine;

use super::errors::InferMediaError;
use super::types::{AssetKind, MediaAsset, ModelProfile, Prepared, PreparedKind};

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
    asset: &MediaAsset,
    image_max_bytes: usize,
) -> Result<Prepared, InferMediaError> {
    let path = match asset.path.as_deref() {
        Some(p) if !p.is_empty() => p,
        _ => return Err(InferMediaError::MediaPathEmpty),
    };
    if !path.starts_with('/') {
        return Err(InferMediaError::MediaPathNotAbsolute);
    }

    let mime = asset
        .mime_type
        .as_deref()
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
    asset: &MediaAsset,
    image_max_bytes: usize,
) -> Result<Prepared, InferMediaError> {
    let bytes = match asset.bytes.as_ref() {
        Some(b) if !b.is_empty() => b,
        _ => return Err(InferMediaError::MediaFileEmpty),
    };
    let mime = asset
        .mime_type
        .as_deref()
        .ok_or(InferMediaError::UnsupportedMediaType)?;

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
pub fn prepare_asset(
    asset: &MediaAsset,
    profile: &ModelProfile,
    image_max_bytes: usize,
) -> Result<Prepared, InferMediaError> {
    match asset.kind {
        AssetKind::RemoteUrl => {
            let url = match asset.url.as_deref() {
                Some(u) if !u.is_empty() => u,
                _ => return Err(InferMediaError::MediaUrlEmpty),
            };
            Ok(Prepared {
                kind: PreparedKind::RemoteUrl,
                payload: url.to_string(),
            })
        }
        AssetKind::InlineBytes => {
            if profile.image_remote_url_only {
                return Err(InferMediaError::RemoteOnlyProfile);
            }
            prepare_inline_bytes_asset(asset, image_max_bytes)
        }
        AssetKind::LocalPath => {
            if profile.image_remote_url_only {
                return Err(InferMediaError::RemoteOnlyProfile);
            }
            prepare_local_path_asset(asset, image_max_bytes)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn profile() -> ModelProfile {
        ModelProfile {
            supports_vision: true,
            ..Default::default()
        }
    }

    #[test]
    fn remote_url_passthrough() {
        let asset = MediaAsset {
            kind: AssetKind::RemoteUrl,
            path: None,
            url: Some("https://example.com/a.png".into()),
            bytes: None,
            mime_type: None,
        };
        let p = prepare_asset(&asset, &profile(), 1024).unwrap();
        assert_eq!(p.kind, PreparedKind::RemoteUrl);
        assert_eq!(p.payload, "https://example.com/a.png");
    }

    #[test]
    fn local_path_data_url() {
        let dir = std::env::temp_dir();
        let path = dir.join(format!("claw_media_test_{}.png", std::process::id()));
        std::fs::write(&path, b"\x89PNG\r\n\x1a\nABCDE").unwrap();
        let asset = MediaAsset {
            kind: AssetKind::LocalPath,
            path: Some(path.to_string_lossy().into_owned()),
            url: None,
            bytes: None,
            mime_type: None,
        };
        let p = prepare_asset(&asset, &profile(), 1024).unwrap();
        assert_eq!(p.kind, PreparedKind::DataUrl);
        assert!(p.payload.starts_with("data:image/png;base64,"));
        let _ = std::fs::remove_file(&path);
    }

    #[test]
    fn rejects_relative_path() {
        let asset = MediaAsset {
            kind: AssetKind::LocalPath,
            path: Some("rel/a.png".into()),
            url: None,
            bytes: None,
            mime_type: None,
        };
        let e = prepare_asset(&asset, &profile(), 1024).unwrap_err();
        assert!(matches!(e, InferMediaError::MediaPathNotAbsolute));
    }

    #[test]
    fn rejects_unknown_extension() {
        let asset = MediaAsset {
            kind: AssetKind::LocalPath,
            path: Some("/tmp/a.bmp".into()),
            url: None,
            bytes: None,
            mime_type: None,
        };
        let e = prepare_asset(&asset, &profile(), 1024).unwrap_err();
        assert!(matches!(e, InferMediaError::UnsupportedMediaType));
    }

    #[test]
    fn inline_bytes_data_url() {
        let png_bytes = b"\x89PNG\r\n\x1a\nABCDE";
        let asset = MediaAsset {
            kind: AssetKind::InlineBytes,
            path: None,
            url: None,
            bytes: Some(png_bytes.to_vec()),
            mime_type: Some("image/png".into()),
        };
        let p = prepare_asset(&asset, &profile(), 1024).unwrap();
        assert_eq!(p.kind, PreparedKind::DataUrl);
        assert!(p.payload.starts_with("data:image/png;base64,"));
    }

    #[test]
    fn inline_bytes_requires_mime() {
        let asset = MediaAsset {
            kind: AssetKind::InlineBytes,
            path: None,
            url: None,
            bytes: Some(b"hello".to_vec()),
            mime_type: None,
        };
        let e = prepare_asset(&asset, &profile(), 1024).unwrap_err();
        assert!(matches!(e, InferMediaError::UnsupportedMediaType));
    }

    #[test]
    fn inline_bytes_respects_size_limit() {
        let asset = MediaAsset {
            kind: AssetKind::InlineBytes,
            path: None,
            url: None,
            bytes: Some(vec![0u8; 100]),
            mime_type: Some("image/png".into()),
        };
        let e = prepare_asset(&asset, &profile(), 50).unwrap_err();
        assert!(matches!(e, InferMediaError::MediaTooLarge));
    }

    #[test]
    fn inline_bytes_rejects_empty() {
        let asset = MediaAsset {
            kind: AssetKind::InlineBytes,
            path: None,
            url: None,
            bytes: Some(vec![]),
            mime_type: Some("image/png".into()),
        };
        let e = prepare_asset(&asset, &profile(), 1024).unwrap_err();
        assert!(matches!(e, InferMediaError::MediaFileEmpty));
    }

    #[test]
    fn inline_bytes_rejects_remote_only_profile() {
        let mut p = profile();
        p.image_remote_url_only = true;
        let asset = MediaAsset {
            kind: AssetKind::InlineBytes,
            path: None,
            url: None,
            bytes: Some(b"x".to_vec()),
            mime_type: Some("image/png".into()),
        };
        let e = prepare_asset(&asset, &p, 1024).unwrap_err();
        assert!(matches!(e, InferMediaError::RemoteOnlyProfile));
    }
}
