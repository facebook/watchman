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

//! This example shows how to send a query to watchman and print out the files
//! changed since the given timestamp.

use std::path::PathBuf;
use structopt::StructOpt;
use watchman_client::prelude::*;

#[derive(Debug, StructOpt)]
#[structopt(about = "Query files changed since a timestamp")]
struct Opt {
    #[structopt()]
    /// Specifies the clock. Use `watchman clock <PATH>` to retrieve the current clock of a watched
    /// directory
    clock: String,

    #[structopt(short, long)]
    /// [not recommended] Uses Unix timestamp as clock
    unix_timestamp: bool,

    #[structopt(short, long, default_value = ".")]
    /// Specifies the path to watched directory
    path: PathBuf,
}

#[tokio::main(flavor = "current_thread")]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    if let Err(err) = run().await {
        // Print a prettier error than the default
        eprintln!("{}", err);
        std::process::exit(1);
    }
    Ok(())
}

async fn run() -> Result<(), Box<dyn std::error::Error>> {
    let opt = Opt::from_args();
    let client = Connector::new().connect().await?;
    let resolved = client
        .resolve_root(CanonicalPath::canonicalize(opt.path)?)
        .await?;

    let clock_spec = if opt.unix_timestamp {
        // it is better to use watchman's clock rather than Unix timestamp.
        // see `watchman_client::pdu::ClockSpec::unix_timestamp` for details.
        ClockSpec::UnixTimestamp(opt.clock.parse()?)
    } else {
        ClockSpec::StringClock(opt.clock)
    };

    let result = client
        .query::<NameOnly>(
            &resolved,
            QueryRequestCommon {
                since: Some(Clock::Spec(clock_spec.clone())),
                ..Default::default()
            },
        )
        .await?;

    eprintln!("Clock is now: {:?}", result.clock);

    if let Some(files) = result.files {
        for file in files.iter() {
            println!("{}", file.name.display());
        }
    } else {
        eprintln!("no file changed since {:?}", clock_spec);
    }

    Ok(())
}
