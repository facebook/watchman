//! This example prints the effective privilege output from
//! `whoami` as well as our understanding of that level
use deelevate::Token;

fn whoami() {
    let output = std::process::Command::new("whoami.exe")
        .arg("/groups")
        .output()
        .expect("failed to run whoami");
    let stdout = String::from_utf8_lossy(&output.stdout);
    println!("{}", stdout);
}

fn main() {
    whoami();

    let token = Token::with_current_process().unwrap();
    let level = token.privilege_level().unwrap();
    println!("priv level is {:?}", level);
}
