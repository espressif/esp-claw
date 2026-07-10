//! ESP-IDF VFS-backed [`ClawFs`](claw_interface::ClawFs).
//!
//! ESP-IDF exposes mounted FATFS/SD paths through POSIX file APIs, and Rust's
//! `std::fs` on the espidf target is implemented over those APIs. `EspIdfFs`
//! therefore keeps the same byte-oriented semantics as the host `DiskFs`, while
//! staying device-only and rooted in paths already resolved by C `claw_paths`.

#[cfg(target_os = "espidf")]
mod espidf {
    use std::io::{Read, Seek, SeekFrom, Write};

    use claw_interface::{ClawFile, ClawFs, FsError};

    /// Device filesystem backend over ESP-IDF VFS paths.
    ///
    /// Paths are used verbatim. The caller is responsible for passing paths
    /// joined against the DATA root (for example via C `claw_paths_join`).
    #[derive(Debug, Clone, Copy, Default)]
    pub struct EspIdfFs;

    impl EspIdfFs {
        fn ensure_parent(path: &std::path::Path) -> Result<(), FsError> {
            if let Some(parent) = path.parent() {
                std::fs::create_dir_all(parent).map_err(FsError::from)?;
            }
            Ok(())
        }

        fn replace(from: &std::path::Path, to: &std::path::Path) -> Result<(), FsError> {
            Self::restore_backup_if_needed(to)?;
            match std::fs::rename(from, to) {
                Ok(()) => Ok(()),
                // ESP-IDF's FATFS VFS delegates to `f_rename`, which returns
                // EEXIST instead of replacing an occupied destination. Move
                // the old file aside first so a reset between the two renames
                // can restore it on the next open.
                Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => {
                    let backup = Self::backup_path(to);
                    Self::remove_file_if_exists(&backup)?;
                    std::fs::rename(to, &backup).map_err(FsError::from)?;
                    match std::fs::rename(from, to) {
                        Ok(()) => {
                            // The new file is committed. A failed best-effort
                            // cleanup only leaves a recoverable stale backup;
                            // it must not make the caller roll back live state.
                            let _ = Self::remove_file_if_exists(&backup);
                            Ok(())
                        }
                        Err(error) => {
                            let _ = std::fs::rename(&backup, to);
                            Err(FsError::from(error))
                        }
                    }
                }
                Err(error) => Err(FsError::from(error)),
            }
        }

        fn backup_path(path: &std::path::Path) -> std::path::PathBuf {
            let mut backup = path.as_os_str().to_owned();
            backup.push(".bak");
            std::path::PathBuf::from(backup)
        }

        fn remove_file_if_exists(path: &std::path::Path) -> Result<(), FsError> {
            match std::fs::remove_file(path) {
                Ok(()) => Ok(()),
                Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(()),
                Err(error) => Err(FsError::from(error)),
            }
        }

        fn restore_backup_if_needed(path: &std::path::Path) -> Result<(), FsError> {
            let backup = Self::backup_path(path);
            match std::fs::metadata(path) {
                Ok(_) => {
                    let _ = Self::remove_file_if_exists(&backup);
                    Ok(())
                }
                Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
                    match std::fs::rename(&backup, path) {
                        Ok(()) => Ok(()),
                        Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(()),
                        Err(error) => Err(FsError::from(error)),
                    }
                }
                Err(error) => Err(FsError::from(error)),
            }
        }
    }

    /// Open ESP-IDF VFS file handle.
    pub struct EspIdfFile {
        file: std::fs::File,
    }

    impl ClawFile for EspIdfFile {
        fn read_to_end(&mut self) -> Result<Vec<u8>, FsError> {
            let mut buffer = Vec::new();
            self.file.read_to_end(&mut buffer).map_err(FsError::from)?;
            Ok(buffer)
        }

        fn read_exact_at(&mut self, offset: u64, len: usize) -> Result<Vec<u8>, FsError> {
            self.file
                .seek(SeekFrom::Start(offset))
                .map_err(FsError::from)?;
            let mut buffer = vec![0u8; len];
            self.file.read_exact(&mut buffer).map_err(FsError::from)?;
            Ok(buffer)
        }

        fn size(&self) -> Result<u64, FsError> {
            self.file
                .metadata()
                .map(|metadata| metadata.len())
                .map_err(FsError::from)
        }

        fn write_all(&mut self, data: &[u8]) -> Result<(), FsError> {
            self.file.write_all(data).map_err(FsError::from)
        }
    }

    impl ClawFs for EspIdfFs {
        type File = EspIdfFile;

        fn open(path: &str) -> Result<Self::File, FsError> {
            let full = std::path::Path::new(path);
            Self::restore_backup_if_needed(full)?;
            std::fs::File::open(full)
                .map(|file| EspIdfFile { file })
                .map_err(FsError::from)
        }

        fn create(path: &str) -> Result<Self::File, FsError> {
            let full = std::path::Path::new(path);
            Self::ensure_parent(full)?;
            std::fs::File::create(full)
                .map(|file| EspIdfFile { file })
                .map_err(FsError::from)
        }

        fn open_append(path: &str) -> Result<Self::File, FsError> {
            let full = std::path::Path::new(path);
            Self::ensure_parent(full)?;
            Self::restore_backup_if_needed(full)?;
            std::fs::OpenOptions::new()
                .create(true)
                .append(true)
                .open(full)
                .map(|file| EspIdfFile { file })
                .map_err(FsError::from)
        }

        fn rename(from: &str, to: &str) -> Result<(), FsError> {
            Self::replace(std::path::Path::new(from), std::path::Path::new(to))
        }

        fn create_dir_all(path: &str) -> Result<(), FsError> {
            std::fs::create_dir_all(path).map_err(FsError::from)
        }

        fn exists(path: &str) -> bool {
            let full = std::path::Path::new(path);
            let _ = Self::restore_backup_if_needed(full);
            full.exists()
        }

        fn remove(path: &str) -> Result<(), FsError> {
            // ESP-IDF/FatFS has no symlink semantics to preserve, and Rust's
            // `symlink_metadata` pulls in the unsupported POSIX `lstat` symbol.
            let result = match std::fs::metadata(path) {
                Ok(metadata) if metadata.is_dir() => std::fs::remove_dir(path),
                Ok(_) => std::fs::remove_file(path),
                Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(()),
                Err(error) => return Err(FsError::from(error)),
            };
            match result {
                Ok(()) => Ok(()),
                Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(()),
                Err(error) => Err(FsError::from(error)),
            }?;
            Self::remove_file_if_exists(&Self::backup_path(std::path::Path::new(path)))
        }

        fn list_dir(path: &str) -> Result<Vec<String>, FsError> {
            let entries = std::fs::read_dir(path).map_err(FsError::from)?;
            let mut names = Vec::new();
            for entry in entries {
                let entry = entry.map_err(FsError::from)?;
                if let Some(name) = entry.file_name().to_str() {
                    names.push(name.to_string());
                }
            }
            Ok(names)
        }

        fn len(path: &str) -> Result<u64, FsError> {
            let full = std::path::Path::new(path);
            Self::restore_backup_if_needed(full)?;
            std::fs::metadata(full)
                .map(|metadata| metadata.len())
                .map_err(FsError::from)
        }

        fn write_atomic(path: &str, data: &[u8]) -> Result<(), FsError> {
            let full = std::path::Path::new(path);
            Self::ensure_parent(full)?;
            let tmp = format!("{path}.tmp");
            std::fs::write(&tmp, data).map_err(FsError::from)?;
            Self::replace(std::path::Path::new(&tmp), full)
        }
    }
}

#[cfg(target_os = "espidf")]
pub use espidf::{EspIdfFile, EspIdfFs};
