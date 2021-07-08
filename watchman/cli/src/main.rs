use structopt::{clap::AppSettings, StructOpt};

mod audit;

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
    async fn run(&self) -> anyhow::Result<()> {
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
