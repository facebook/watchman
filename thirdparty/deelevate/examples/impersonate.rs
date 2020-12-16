//! This example demonstrates that impersonation isn't sufficient
//! to effectively reduce privileges.  It does this by running
//! `whoami` before and after an impersonation request and showing
//! that the effective prileges remain the same.

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

    let medium = token
        .as_medium_integrity_safer_token()
        .expect("failed to make medium token");

    medium
        .impersonate()
        .expect("failed to impersonate self with medium token");

    println!("impersonation successful, but note that it isn't effective in changing whoami!");

    whoami();
}
