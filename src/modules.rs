//! C++20 modules: whether the target can build them, and whether this machine can
//! build them at all.
//!
//! CMake scans module interfaces only with Ninja (or Visual Studio generators), so
//! `--modules on` needs a Ninja on PATH. The compiler side is a curated minimum
//! ahead of the matrix's "partial" rows: the matrix records that GCC 11 ships
//! modules, and GCC 13 still fails on a two-file module, so the tool only claims
//! what it has verified.

use std::path::PathBuf;

use crate::project::Target;
use crate::util;

pub fn ninja() -> Option<PathBuf> {
    if let Some(path) = util::which("ninja") {
        return Some(path);
    }
    let home = std::env::var_os("HOME").map(PathBuf::from);
    let candidates = [
        home.map(|home| home.join(".local/bin/ninja")),
        Some(PathBuf::from("/usr/local/bin/ninja")),
        Some(PathBuf::from("/opt/homebrew/bin/ninja")),
    ];
    candidates.into_iter().flatten().find(|path| path.is_file())
}

pub fn compiler_ready(target: &Target) -> bool {
    let toolchain = target.toolchain.as_str();
    let minimum = match toolchain {
        "gcc" => "15",
        "clang" => "17",
        "aclang" => "16",
        "msvc" => "19.36",
        _ => return false,
    };
    util::version_at_least(&target.version, minimum)
}

/// Resolves `--modules auto|on|off` into the project's mode.
pub fn resolve(
    mode: &str,
    target: &Target,
    matrix: &crate::matrix::Matrix,
) -> Result<bool, String> {
    let std_number: u32 = target.std.trim_start_matches("c++").parse().unwrap_or(0);
    if std_number < 20 {
        if mode == "on" {
            return Err(format!(
                "--modules needs C++20 or later, not {}: pass `--std c++23`",
                target.std
            ));
        }
        return Ok(false);
    }
    let matrix_says = matrix.macro_available(
        "__cpp_modules",
        &target.toolchain,
        &target.version,
        &target.std,
    );
    let compiler = compiler_ready(target);
    let ninja = ninja();
    match mode {
        "off" => Ok(false),
        "on" => {
            {
                if !compiler {
                    return Err(format!(
                        "{} does not support C++20 modules well enough to generate them: the tool \
                         needs gcc 15+, clang 17+, apple-clang 16+ or msvc 19.36+ for a module unit \
                         (the matrix row says: {}). Pass `--modules off` to generate a header instead.",
                        target.toolchain,
                        match matrix_says {
                            crate::matrix::MacroStatus::TooOld(min, _) => format!("needs {min}"),
                            crate::matrix::MacroStatus::Partial(min, _) => format!("partial since {min}"),
                            _ => "not recorded".to_string(),
                        }
                    ));
                }
                if ninja.is_none() {
                    return Err(
                        "CMake scans C++20 module interfaces only with Ninja: install it \
                         (`python3 -m pip install --user ninja`, `apt install ninja-build`, \
                         `brew install ninja`) or pass `--modules off`"
                            .into(),
                    );
                }
            }
            Ok(true)
        }
        _ => Ok(compiler && ninja.is_some()),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::matrix::Matrix;

    fn target(toolchain: &str, version: &str, std: &str) -> Target {
        Target {
            toolchain: toolchain.into(),
            version: version.into(),
            std: std.into(),
        }
    }

    #[test]
    fn curated_minimums() {
        assert!(!compiler_ready(&target("gcc", "13", "c++23")));
        assert!(compiler_ready(&target("gcc", "15", "c++23")));
        assert!(!compiler_ready(&target("clang", "16", "c++23")));
        assert!(compiler_ready(&target("clang", "18", "c++23")));
        assert!(!compiler_ready(&target("msvc", "19.35", "c++23")));
        assert!(compiler_ready(&target("msvc", "19.40", "c++23")));
    }

    #[test]
    fn off_never_modules() {
        let matrix = Matrix::get();
        assert_eq!(
            resolve("off", &target("gcc", "15", "c++23"), matrix),
            Ok(false)
        );
    }

    #[test]
    fn old_standard_is_refused() {
        let matrix = Matrix::get();
        assert!(resolve("on", &target("gcc", "15", "c++17"), matrix).is_err());
    }

    #[test]
    fn old_compiler_is_refused() {
        let matrix = Matrix::get();
        let error = resolve("on", &target("gcc", "13", "c++23"), matrix).unwrap_err();
        assert!(error.contains("--modules off"), "{error}");
    }
}
