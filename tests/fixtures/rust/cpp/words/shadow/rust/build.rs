fn main() {
    println!("cargo:rustc-link-search=native=/mnt/data/Projects/OpenSource/Admin/cpp-anything/tests/fixtures/rust/cpp/words/build");
    println!("cargo:rustc-link-lib=dylib=cppan_shadow");
#[cfg(target_os = "linux")]
    println!("cargo:rustc-link-arg=-Wl,-rpath,/mnt/data/Projects/OpenSource/Admin/cpp-anything/tests/fixtures/rust/cpp/words/build");
#[cfg(target_os = "macos")]
    println!("cargo:rustc-link-arg=-Wl,-rpath,/mnt/data/Projects/OpenSource/Admin/cpp-anything/tests/fixtures/rust/cpp/words/build");
}
