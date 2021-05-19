use structopt::{clap::AppSettings, StructOpt};

mod audit;

/// I'd like to replace this with anyhow, but we need to replace error-chain's use in watchman-client first. https://github.com/rust-lang-nursery/error-chain/pull/241
pub type Result<T> = std::result::Result<T, watchman_client::Error>;

#[derive(StructOpt, Debug)]
#[structopt(setting = AppSettings::DisableVersion,
    setting = AppSettings::VersionlessSubcommands)]
struct MainCommand {
    #[structopt(subcommand)]
    subcommand: TopLevelSubcommand,
}

#[derive(StructOpt, Debug)]
enum TopLevelSubcommand {
    Audit(audit::AuditCmd),
}

impl TopLevelSubcommand {
    async fn run(&self) -> Result<()> {
        use TopLevelSubcommand::*;
        match self {
            Audit(cmd) => cmd.run().await,
        }
    }
}

#[tokio::main]
async fn main() {
    let cmd = MainCommand::from_args();
    match cmd.subcommand.run().await {
        Ok(()) => {}
        Err(e) => {
            eprintln!("error: {}", e);
            std::process::exit(1);
        }
    }
}
