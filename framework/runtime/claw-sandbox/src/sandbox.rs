//! [`Sandbox`] — the concrete [`SandboxFs`] backed by a single [`ClawFs`].
//!
//! A `Sandbox` is a routing layer: it normalizes each virtual path, rejects
//! anything that is not under a visible root (or that escapes via `..`), and
//! maps the surviving paths onto **real** paths in the backing [`ClawFs`].
//!
//! The shared and system roots point at fixed, firmware-known locations passed
//! as `&'static str`; the private `/sandbox` root points at a per-instance host
//! directory whose `skills/` and `tmp/` subdirectories are materialized when the
//! sandbox is constructed.

use std::marker::PhantomData;

use claw_interface::ClawFs;

use crate::fs::{SandboxError, SandboxFs};

/// Real backing-store paths for the roots that live outside `/sandbox`.
///
/// Each field is the real path (in the backing [`ClawFs`]) that the
/// corresponding visible virtual root maps onto. They are `&'static str`
/// because these locations are fixed for the lifetime of the firmware — unlike
/// the per-instance `/sandbox` host directory, which is supplied separately.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct RealRoots {
    /// Real path backing `/shared/skills/`.
    pub shared_skills: &'static str,
    /// Real path backing `/shared/tmp/`.
    pub shared_tmp: &'static str,
    /// Real path backing `/shared/data/`.
    pub shared_data: &'static str,
    /// Real path backing `/system/skills/` (read-only).
    pub system_skills: &'static str,
}

/// One virtual-root → real-path mapping.
#[derive(Debug)]
struct Route {
    /// Visible virtual prefix, always ending in `/` (e.g. `"/sandbox/"`).
    virtual_prefix: &'static str,
    /// Real path in the backing store, with no trailing slash.
    real_prefix: String,
    /// Whether mutating operations are rejected on this route.
    read_only: bool,
}

/// A sandboxed filesystem over a single backing [`ClawFs`].
///
/// Construct with [`Sandbox::new`], then use it through the [`SandboxFs`] trait.
#[derive(Debug)]
pub struct Sandbox<F: ClawFs> {
    routes: Vec<Route>,
    _fs: PhantomData<fn() -> F>,
}

impl<F: ClawFs> Sandbox<F> {
    /// Build a sandbox over the backing filesystem type `F`.
    ///
    /// `sandbox_host_dir` is the real directory backing the private, ephemeral
    /// `/sandbox` root; `real` supplies the fixed real paths for the shared and
    /// system roots. The `/sandbox/skills/` and `/sandbox/tmp/` scratch
    /// directories are created eagerly so they are listable before anything is
    /// written into them.
    ///
    /// Returns [`SandboxError::Fs`] if materializing the scratch directories
    /// fails.
    pub fn new(sandbox_host_dir: impl Into<String>, real: RealRoots) -> Result<Self, SandboxError> {
        let host = trim_trailing_slash(&sandbox_host_dir.into());
        let routes = vec![
            Route {
                virtual_prefix: "/sandbox/",
                real_prefix: host,
                read_only: false,
            },
            Route {
                virtual_prefix: "/shared/skills/",
                real_prefix: trim_trailing_slash(real.shared_skills),
                read_only: false,
            },
            Route {
                virtual_prefix: "/shared/tmp/",
                real_prefix: trim_trailing_slash(real.shared_tmp),
                read_only: false,
            },
            Route {
                virtual_prefix: "/shared/data/",
                real_prefix: trim_trailing_slash(real.shared_data),
                read_only: false,
            },
            Route {
                virtual_prefix: "/system/skills/",
                real_prefix: trim_trailing_slash(real.system_skills),
                read_only: true,
            },
        ];
        let sandbox = Self {
            routes,
            _fs: PhantomData,
        };
        for scratch in ["/sandbox/skills", "/sandbox/tmp"] {
            let (_, real_path) = sandbox.route(scratch)?;
            F::create_dir_all(&real_path)?;
        }
        Ok(sandbox)
    }

    /// Resolve a virtual path to its route and real backing path.
    ///
    /// Returns [`SandboxError::OutsideSandbox`] when the (normalized) path is
    /// not under any visible root.
    fn route(&self, path: &str) -> Result<(&Route, String), SandboxError> {
        let virtual_path = normalize(path)?;
        for route in &self.routes {
            let dir = route.virtual_prefix.trim_end_matches('/');
            if virtual_path == dir {
                return Ok((route, route.real_prefix.clone()));
            }
            if let Some(rest) = virtual_path.strip_prefix(route.virtual_prefix) {
                return Ok((route, format!("{}/{}", route.real_prefix, rest)));
            }
        }
        Err(SandboxError::OutsideSandbox(path.to_string()))
    }

    /// Resolve a virtual path for a mutating operation, rejecting read-only roots.
    fn route_mut(&self, path: &str) -> Result<String, SandboxError> {
        let (route, real_path) = self.route(path)?;
        if route.read_only {
            return Err(SandboxError::ReadOnly(path.to_string()));
        }
        Ok(real_path)
    }
}

impl<F: ClawFs> SandboxFs for Sandbox<F> {
    fn read(&self, path: &str) -> Result<Vec<u8>, SandboxError> {
        let (_, real_path) = self.route(path)?;
        Ok(F::read(&real_path)?)
    }

    fn read_at(&self, path: &str, offset: u64, len: usize) -> Result<Vec<u8>, SandboxError> {
        let (_, real_path) = self.route(path)?;
        Ok(F::read_at(&real_path, offset, len)?)
    }

    fn len(&self, path: &str) -> Result<u64, SandboxError> {
        let (_, real_path) = self.route(path)?;
        Ok(F::len(&real_path)?)
    }

    fn write_atomic(&self, path: &str, data: &[u8]) -> Result<(), SandboxError> {
        let real_path = self.route_mut(path)?;
        Ok(F::write_atomic(&real_path, data)?)
    }

    fn append(&self, path: &str, data: &[u8]) -> Result<(), SandboxError> {
        let real_path = self.route_mut(path)?;
        Ok(F::append(&real_path, data)?)
    }

    fn exists(&self, path: &str) -> Result<bool, SandboxError> {
        let (_, real_path) = self.route(path)?;
        Ok(F::exists(&real_path))
    }

    fn remove(&self, path: &str) -> Result<(), SandboxError> {
        let real_path = self.route_mut(path)?;
        Ok(F::remove(&real_path)?)
    }

    fn list_dir(&self, path: &str) -> Result<Vec<String>, SandboxError> {
        let (_, real_path) = self.route(path)?;
        Ok(F::list_dir(&real_path)?)
    }
}

/// Drop a single trailing `/` (if any) and own the result.
fn trim_trailing_slash(path: &str) -> String {
    path.trim_end_matches('/').to_string()
}

/// Normalize an absolute virtual path: collapse `.`/empty components and apply
/// `..` against the accumulated stack.
///
/// Returns [`SandboxError::OutsideSandbox`] if the path is not absolute or if a
/// `..` would pop above the root (an escape attempt). The result is a clean
/// absolute path (`/a/b`), or `/` for the root.
fn normalize(path: &str) -> Result<String, SandboxError> {
    let outside = || SandboxError::OutsideSandbox(path.to_string());
    if !path.starts_with('/') {
        return Err(outside());
    }
    let mut components: Vec<&str> = Vec::new();
    for component in path.split('/') {
        match component {
            "" | "." => {}
            ".." => {
                components.pop().ok_or_else(outside)?;
            }
            name => components.push(name),
        }
    }
    Ok(format!("/{}", components.join("/")))
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use claw_interface::{ClawFs, MemFs};

    use super::*;

    const REAL: RealRoots = RealRoots {
        shared_skills: "/real/shared/skills",
        shared_tmp: "/real/shared/tmp",
        shared_data: "/real/shared/data",
        system_skills: "/real/system/skills",
    };

    fn sandbox() -> Sandbox<MemFs> {
        MemFs::new();
        Sandbox::<MemFs>::new("/real/sandbox/inst-1", REAL).unwrap()
    }

    #[test]
    fn routes_each_visible_root_to_its_real_path() {
        MemFs::new();
        let sb = Sandbox::<MemFs>::new("/host/sandbox", REAL).unwrap();

        sb.write_atomic("/sandbox/tmp/a", b"1").unwrap();
        sb.write_atomic("/shared/data/b", b"2").unwrap();

        // Writes land at the routed real paths in the backing store.
        assert_eq!(MemFs::read("/host/sandbox/tmp/a").unwrap(), b"1");
        assert_eq!(MemFs::read("/real/shared/data/b").unwrap(), b"2");
    }

    #[test]
    fn scratch_dirs_are_listable_after_construction() {
        let sb = sandbox();
        // Created eagerly, so they list as empty rather than erroring.
        assert_eq!(
            sb.list_dir("/sandbox/skills").unwrap(),
            Vec::<String>::new()
        );
        assert_eq!(sb.list_dir("/sandbox/tmp").unwrap(), Vec::<String>::new());
    }

    #[test]
    fn bare_shared_and_system_roots_are_rejected() {
        let sb = sandbox();
        assert!(matches!(
            sb.list_dir("/shared"),
            Err(SandboxError::OutsideSandbox(_))
        ));
        assert!(matches!(
            sb.read("/system/secret"),
            Err(SandboxError::OutsideSandbox(_))
        ));
        assert!(matches!(
            sb.read("/etc/passwd"),
            Err(SandboxError::OutsideSandbox(_))
        ));
    }

    #[test]
    fn sandbox_root_itself_is_accessible() {
        let sb = sandbox();
        assert!(sb.list_dir("/sandbox").is_ok());
    }

    #[test]
    fn dotdot_escape_is_rejected() {
        let sb = sandbox();
        assert!(matches!(
            sb.read("/sandbox/../../etc/passwd"),
            Err(SandboxError::OutsideSandbox(_))
        ));
    }

    #[test]
    fn dotdot_within_visible_area_is_allowed() {
        let sb = sandbox();
        // /sandbox/tmp/../skills normalizes to /sandbox/skills (still visible).
        sb.write_atomic("/sandbox/tmp/../skills/x", b"ok").unwrap();
        assert_eq!(sb.read("/sandbox/skills/x").unwrap(), b"ok");
    }

    #[test]
    fn system_root_is_read_only() {
        let sb = sandbox();
        assert!(matches!(
            sb.write_atomic("/system/skills/x", b"nope"),
            Err(SandboxError::ReadOnly(_))
        ));
        assert!(matches!(
            sb.remove("/system/skills/x"),
            Err(SandboxError::ReadOnly(_))
        ));
    }

    #[test]
    fn system_root_is_readable() {
        MemFs::new();
        MemFs::write_atomic("/real/system/skills/doc", b"hi").unwrap();
        let sb = Sandbox::<MemFs>::new("/host/sandbox", REAL).unwrap();
        assert_eq!(sb.read("/system/skills/doc").unwrap(), b"hi");
    }

    #[test]
    fn exists_distinguishes_absent_from_inaccessible() {
        let sb = sandbox();
        assert!(!sb.exists("/sandbox/tmp/missing").unwrap());
        assert!(matches!(
            sb.exists("/nope/x"),
            Err(SandboxError::OutsideSandbox(_))
        ));
    }
}
