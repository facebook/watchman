/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

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
