use ahash::AHashMap;
use serde::Deserialize;
use std::os::unix::fs::MetadataExt;
use std::os::unix::fs::PermissionsExt;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::Instant;
use structopt::StructOpt;
use walkdir::WalkDir;
use watchman_client::prelude::*;

#[derive(StructOpt, Debug)]
#[structopt(about = "Audit Watchman's in-memory database with the filesystem")]
pub(crate) struct AuditCmd {
    #[structopt(name = "path", parse(from_os_str))]
    path: PathBuf,
}

query_result_type! {
    struct AuditQueryResult {
        name: NameField,
        mode: ModeAndPermissionsField,
        size: SizeField,
        mtime: MTimeField,
        oclock: ObservedClockField,
        ino: InodeNumberField,
    }
}

impl AuditCmd {
    pub(crate) async fn run(&self) -> crate::Result<()> {
        let client = Connector::new().connect().await?;
        let resolved = Arc::new(
            client
                .resolve_root(CanonicalPath::canonicalize(&self.path)?)
                .await?,
        );

        if resolved.watcher() == "eden" {
            return Err(watchman_client::Error::Generic(format!(
                "{} is an EdenFS mount - no need to audit",
                resolved.project_root().display()
            )));
        }

        let config = client.get_config(&resolved).await?;
        let mut ignore_dirs = config.ignore_dirs.unwrap_or_default().clone();

        // TODO: This list is duplicated in the Watchman query below.
        ignore_dirs.push(".hg".into());
        ignore_dirs.push(".git".into());
        ignore_dirs.push(".svn".into());

        let filesystem_state_handle = {
            let resolved = resolved.clone();
            tokio::spawn(async move {
                let mut filesystem_state: AHashMap<PathBuf, std::fs::Metadata> = AHashMap::new();

                let start_crawl = Instant::now();

                // Allocate outside the loop to save time.
                let resolved_path = resolved.path();

                for entry in WalkDir::new(&resolved_path)
                    .into_iter()
                    .filter_entry(|entry| {
                        // It sucks we have to do this here and in the loop body below. It would be nice if traversal and filtering were folded into the same callback.
                        let from_root = match entry.path().strip_prefix(&resolved_path) {
                            Ok(from_root) => from_root,
                            Err(_) => return true,
                        };
                        let join_storage;
                        let path_from_root = match resolved.project_relative_path() {
                            Some(relpath) => {
                                join_storage = relpath.join(&from_root);
                                join_storage.as_ref()
                            }
                            None => from_root,
                        };
                        !ignore_dirs.iter().any(|i| i == path_from_root)
                    })
                {
                    let entry = match entry {
                        Ok(entry) => entry,
                        Err(err) => {
                            eprintln!("error while traversing directory: {}", err);
                            continue;
                        }
                    };

                    let relpath = match entry.path().strip_prefix(&resolved_path) {
                        Ok(relpath) => relpath,
                        Err(err) => {
                            eprintln!(
                                "unable to form relative path from {} to {}: {}",
                                resolved_path.display(),
                                entry.path().display(),
                                err
                            );
                            continue;
                        }
                    };

                    let metadata = match entry.metadata() {
                        Ok(metadata) => metadata,
                        Err(err) => {
                            eprintln!(
                                "error fetching metadata for {}: {}",
                                entry.path().display(),
                                err
                            );
                            continue;
                        }
                    };
                    filesystem_state.insert(relpath.to_path_buf(), metadata);
                }
                // Watchman doesn't return information about the root, so remove it here.
                filesystem_state.remove(&PathBuf::new());
                eprintln!("Crawled filesystem in {:#?}", start_crawl.elapsed());

                filesystem_state
            })
        };

        let start_query = Instant::now();

        // TODO: should we ignore fresh instance results?

        use Expr::*;
        let result = client
            .query::<AuditQueryResult>(
                &resolved,
                QueryRequestCommon {
                    expression: Some(All(vec![
                        Exists,
                        Not(Box::new(Any(vec![
                            DirName(DirNameTerm {
                                path: ".git".into(),
                                depth: None,
                            }),
                            DirName(DirNameTerm {
                                path: ".hg".into(),
                                depth: None,
                            }),
                            DirName(DirNameTerm {
                                path: ".svn".into(),
                                depth: None,
                            }),
                            Name(NameTerm {
                                paths: vec![".git".into()],
                                wholename: true,
                            }),
                            Name(NameTerm {
                                paths: vec![".svn".into()],
                                wholename: true,
                            }),
                            Name(NameTerm {
                                paths: vec![".hg".into()],
                                wholename: true,
                            }),
                        ]))),
                    ])),
                    ..Default::default()
                },
            )
            .await?;

        eprintln!("Queried Watchman in {:#?}", start_query.elapsed());

        let filesystem_state = filesystem_state_handle.await.unwrap();

        let diff_start = Instant::now();

        let watchman_files = match result.files {
            Some(files) => files,
            None => {
                return Err(watchman_client::Error::Generic(
                    "No files set in result {}".into(),
                ));
            }
        };

        let mut any_differences = false;
        let mut phantoms = vec![];
        let mut missing = vec![];

        let mut watchman_state: AHashMap<&Path, &AuditQueryResult> =
            AHashMap::with_capacity(watchman_files.len());
        for watchman_file in &watchman_files {
            let filename = &*watchman_file.name;
            watchman_state.insert(filename, watchman_file);

            let metadata = match filesystem_state.get(filename) {
                Some(metadata) => metadata,
                None => {
                    phantoms.push(watchman_file);
                    continue;
                }
            };

            let mut diffs = Vec::new();

            if *watchman_file.mode != u64::from(metadata.permissions().mode()) {
                diffs.push(format!(
                    "watchman mode is {} vs. fs {}",
                    *watchman_file.mode,
                    metadata.permissions().mode()
                ));
            }

            if metadata.is_file() && *watchman_file.size != metadata.size() {
                diffs.push(format!(
                    "watchman size is {} vs. fs {}",
                    *watchman_file.size,
                    metadata.len()
                ));
            }

            if metadata.is_file() && *watchman_file.mtime != metadata.mtime() {
                diffs.push(format!(
                    "watchman mtime is {} vs. fs {}",
                    *watchman_file.mtime,
                    metadata.mtime()
                ));
            }

            if *watchman_file.ino != metadata.ino() {
                diffs.push(format!(
                    "watchman ino is {} vs. fs {}",
                    *watchman_file.ino,
                    metadata.ino()
                ));
            }

            if !diffs.is_empty() {
                println!(
                    "Conflicting information for {}:",
                    watchman_file.name.display()
                );
                println!("  oclock is {:#?}", *watchman_file.oclock);
                for diff in diffs {
                    println!("  {}", diff);
                }
                any_differences = true;
            }
        }

        for (path, val) in &filesystem_state {
            if !watchman_state.contains_key(&path.as_path()) {
                missing.push((path, val));
            }
        }

        phantoms.sort_by(|x, y| x.name.cmp(&y.name));
        missing.sort_by(|x, y| x.0.cmp(&y.0));

        if !phantoms.is_empty() {
            println!(
                "There are {} items reported by watchman not on the filesystem:",
                phantoms.len()
            );
            for phantom in &phantoms {
                println!("  {}", phantom.name.display());
            }
            any_differences = true;
        }

        if !missing.is_empty() {
            println!(
                "There are {} items on the filesystem not reported by watchman:",
                missing.len()
            );
            for (path, _) in &missing {
                println!("  {}", path.display());
            }
            any_differences = true;
        }

        if any_differences {
            // This is dumb, but Rust doesn't have a standard way to return
            // nonzero exit codes yet.
            std::process::exit(1);
        }

        eprintln!("Diffed in {:#?}", diff_start.elapsed());

        Ok(())
    }
}
