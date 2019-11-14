use serde::Deserialize;
use watchman_client::*;

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
    let resolved = client
        .resolve_root(CanonicalPath::canonicalize(".")?)
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
