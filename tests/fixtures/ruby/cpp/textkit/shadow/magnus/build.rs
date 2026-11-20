fn main() {
    println!("cargo:rustc-link-search=native=/mnt/data/Projects/OpenSource/cpp-anything/tests/fixtures/ruby/cpp/textkit/build");
    println!("cargo:rustc-link-lib=dylib=cppan_shadow");
    println!("cargo:rustc-link-arg=-Wl,-rpath,/mnt/data/Projects/OpenSource/cpp-anything/tests/fixtures/ruby/cpp/textkit/build");
}
