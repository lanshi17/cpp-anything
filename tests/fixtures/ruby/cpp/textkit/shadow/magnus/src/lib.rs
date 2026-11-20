use magnus::{function, prelude::*, Error, RString};

unsafe extern "C" {
    fn cppan_shadow_call(name: *const std::ffi::c_char, args: *const std::ffi::c_char) -> *mut std::ffi::c_char;
    fn cppan_shadow_free(value: *mut std::ffi::c_char);
}

fn call(name: RString, args: RString) -> Result<String, Error> {
    let name = std::ffi::CString::new(name.to_string()?)
        .map_err(|error| Error::new(magnus::exception::runtime_error(), error.to_string()))?;
    let args = std::ffi::CString::new(args.to_string()?)
        .map_err(|error| Error::new(magnus::exception::runtime_error(), error.to_string()))?;
    // SAFETY: both pointers are NUL-terminated and live for the call; the result is
    // the C ABI's allocation and is freed exactly once.
    unsafe {
        let out = cppan_shadow_call(name.as_ptr(), args.as_ptr());
        if out.is_null() {
            return Err(Error::new(magnus::exception::runtime_error(), "no result"));
        }
        let text = std::ffi::CStr::from_ptr(out).to_string_lossy().into_owned();
        cppan_shadow_free(out);
        Ok(text)
    }
}

#[magnus::init]
fn init() -> Result<(), Error> {
    let module = magnus::define_module("CppanShadowNative")?;
    module.define_singleton_method("call", function!(call, 2))?;
    Ok(())
}
