//! cpp-anything: rewrite your app in C++ (C++20/23), and prove it still behaves
//! the same.
//!
//! The loop is `analyze` → `bridge` → `record` → (write C++) → `build` → `diff` →
//! `gate`, with `matrix` answering what the target toolchain can compile and `rsi`
//! using the same loop to check cpp-anything itself.

mod analyze;
mod app;
mod appcase;
mod appfront;
mod bench;
mod build;
mod cli;
mod codegen;
mod diff;
mod docs;
mod doctor;
mod gate;
mod inventory;
mod json;
mod matrix;
mod mcp;
mod profile;
mod project;
mod python;
mod rsi;
mod shadow;
mod types;
mod util;

#[cfg(unix)]
fn restore_sigpipe() {
    // Rust ignores SIGPIPE, so `cpp-anything matrix features | head` would panic on
    // a closed stdout. Unix programs are expected to die quietly instead.
    unsafe extern "C" {
        fn signal(signum: i32, handler: usize) -> usize;
    }
    const SIGPIPE: i32 = 13;
    const SIG_DFL: usize = 0;
    // SAFETY: restoring the disposition the process was started with; no handler
    // is installed and no pointer outlives the call.
    unsafe {
        signal(SIGPIPE, SIG_DFL);
    }
}

fn main() {
    #[cfg(unix)]
    restore_sigpipe();
    let argv: Vec<String> = std::env::args().skip(1).collect();
    match cli::run(&argv) {
        Ok(code) => std::process::exit(code),
        Err(message) => {
            eprintln!("cpp-anything: {message}");
            std::process::exit(1);
        }
    }
}
