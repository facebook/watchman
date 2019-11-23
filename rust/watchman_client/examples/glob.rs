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

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    if let Err(err) = run().await {
        // Print a prettier error than the default
        eprintln!("{}", err);
        std::process::exit(1);
    }
    Ok(())
}

async fn run() -> Result<(), Box<dyn std::error::Error>> {
    let mut client = Connector::new().connect().await?;

    let opt = Opt::from_args();

    let resolved = client
        .resolve_root(CanonicalPath::canonicalize(opt.path)?)
        .await?;

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
