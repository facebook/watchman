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

use serde::Deserialize;
use std::path::PathBuf;
use structopt::StructOpt;
use watchman_client::prelude::*;

#[derive(Debug, StructOpt)]
#[structopt(about = "Perform a glob query for a path, using watchman")]
struct Opt {
    #[structopt(default_value = ".")]
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

    println!("resolved watch to {:?}", resolved);

    // Basic globs -> names
    let files = client.glob(&resolved, &["**/*.rs"]).await?;
    println!("files: {:#?}", files);

    query_result_type! {
        struct NameAndHash {
            name: NameField,
            hash: ContentSha1HexField,
        }
    }

    let response: QueryResult<NameAndHash> = client
        .query(
            &resolved,
            QueryRequestCommon {
                glob: Some(vec!["**/*.rs".to_string()]),
                expression: Some(Expr::Not(Box::new(Expr::Empty))),
                ..Default::default()
            },
        )
        .await?;
    println!("response: {:#?}", response);

    Ok(())
}
