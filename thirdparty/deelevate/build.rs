use vergen::{generate_cargo_keys, ConstantsFlags};
fn main() {
    let mut flags = ConstantsFlags::all();
    flags.remove(ConstantsFlags::SEMVER_FROM_CARGO_PKG);
    generate_cargo_keys(ConstantsFlags::all()).expect("Unable to generate the cargo keys!");
    println!("cargo:rerun-if-changed=resource.rc");

    // Obtain MSVC environment so that the rc compiler can find the right headers.
    // https://github.com/nabijaczleweli/rust-embed-resource/issues/11#issuecomment-603655972
    let target = std::env::var("TARGET").unwrap();
    if let Some(tool) = cc::windows_registry::find_tool(target.as_str(), "cl.exe") {
        for (key, value) in tool.env() {
            std::env::set_var(key, value);
        }
    }

    embed_resource::compile("resource.rc");
}
