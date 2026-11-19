//! Configuring and building a generated project with CMake.

use std::path::PathBuf;

use crate::project::Project;
use crate::util;

pub struct BuildOutcome {
    pub driver: PathBuf,
    pub warnings: usize,
}

pub fn driver_path(project: &Project) -> PathBuf {
    let name = if cfg!(windows) {
        "cppan_driver.exe"
    } else {
        "cppan_driver"
    };
    project.dir.join("build").join(name)
}

pub fn build(
    project: &Project,
    clean: bool,
    cxx: Option<&str>,
    jobs: usize,
) -> Result<BuildOutcome, String> {
    let build_dir = project.dir.join("build");
    if clean && build_dir.exists() {
        std::fs::remove_dir_all(&build_dir)
            .map_err(|error| format!("{}: {error}", build_dir.display()))?;
    }
    if util::which("cmake").is_none() {
        return Err(
            "cmake is not on PATH: install CMake 3.28 or newer (alternatives: `apt install cmake`, \
             `brew install cmake`, `winget install Kitware.CMake`)"
                .to_string(),
        );
    }

    let mut configure = vec![
        "-S".to_string(),
        project.dir.display().to_string(),
        "-B".to_string(),
        build_dir.display().to_string(),
        "-DCMAKE_BUILD_TYPE=Release".to_string(),
    ];
    if let Some(compiler) = cxx {
        configure.push(format!("-DCMAKE_CXX_COMPILER={compiler}"));
    }
    let run = util::run("cmake", &configure, Some(&project.dir));
    if !run.ok {
        return Err(format!(
            "cmake configure failed: {}\n{}",
            run.failure(),
            util::indent(&run.stderr, 2)
        ));
    }

    let mut make = vec![
        "--build".to_string(),
        build_dir.display().to_string(),
        "-j".to_string(),
        jobs.max(1).to_string(),
    ];
    if cxx.is_some() {
        // A fresh compiler argument invalidates the cache: re-generate cleanly.
        make.push("--".to_string());
    }
    let run = util::run("cmake", &make, Some(&project.dir));
    let warnings = run
        .stderr
        .lines()
        .filter(|line| line.contains("warning:"))
        .count()
        + run
            .stdout
            .lines()
            .filter(|line| line.contains("warning:"))
            .count();
    if !run.ok {
        return Err(format!(
            "build failed: {}\n{}",
            run.failure(),
            util::indent(&tail(&format!("{}{}", run.stdout, run.stderr), 40), 2)
        ));
    }
    let driver = driver_path(project);
    if !driver.exists() {
        return Err(format!(
            "build succeeded but {} is missing",
            driver.display()
        ));
    }
    Ok(BuildOutcome { driver, warnings })
}

fn tail(text: &str, lines: usize) -> String {
    let all: Vec<&str> = text.lines().collect();
    let start = all.len().saturating_sub(lines);
    all[start..].join("\n")
}
