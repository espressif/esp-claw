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
            std::fs::File::open(path)
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
            std::fs::OpenOptions::new()
                .create(true)
                .append(true)
                .open(full)
                .map(|file| EspIdfFile { file })
                .map_err(FsError::from)
        }

        fn rename(from: &str, to: &str) -> Result<(), FsError> {
            std::fs::rename(from, to).map_err(FsError::from)
        }

        fn create_dir_all(path: &str) -> Result<(), FsError> {
            std::fs::create_dir_all(path).map_err(FsError::from)
        }

        fn exists(path: &str) -> bool {
            std::path::Path::new(path).exists()
        }

        fn remove(path: &str) -> Result<(), FsError> {
            match std::fs::remove_file(path) {
                Ok(()) => Ok(()),
                Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(()),
                Err(error) => Err(FsError::from(error)),
            }
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
            std::fs::metadata(path)
                .map(|metadata| metadata.len())
                .map_err(map_io)
        }

        fn write_atomic(path: &str, data: &[u8]) -> Result<(), FsError> {
            let full = std::path::Path::new(path);
            Self::ensure_parent(full)?;
            let tmp = format!("{path}.tmp");
            std::fs::write(&tmp, data).map_err(FsError::from)?;
            std::fs::rename(&tmp, full).map_err(FsError::from)
        }
    }
}

#[cfg(target_os = "espidf")]
pub use espidf::{EspIdfFile, EspIdfFs};
