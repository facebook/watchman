/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

//! This example shows how to initiate a subscription and print out
//! file changes as they are reported
use clap::Parser;
use std::path::PathBuf;
use watchman_client::prelude::*;


/// Subscribe to watchman and stream file changes for a path
#[derive(Debug, Parser)]
struct Cli {
    #[arg(default_value = ".")]
    path: PathBuf,
}

#[tokio::main(flavor = "current_thread")]
async fn main() {
    if let Err(err) = run().await {
        // Print a prettier error than the default
        eprintln!("{}", err);
        std::process::exit(1);
    }
}

async fn run() -> Result<(), Box<dyn std::error::Error>> {
    let opt = Cli::parse();
    let client = Connector::new().connect().await?;
    let resolved = client
        .resolve_root(CanonicalPath::canonicalize(opt.path)?)
        .await?;

    let (mut sub, initial) = client
        .subscribe::<NameOnly>(&resolved, SubscribeRequest::default())
        .await?;

    println!("{} {:#?}", sub.name(), initial);
    loop {
        let item = sub.next().await?;
        println!("{:#?}", item);
    }
}
