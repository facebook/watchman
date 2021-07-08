use ahash::AHashMap;
use jwalk::WalkDir;
use serde::Deserialize;
use std::io::ErrorKind;
use std::os::unix::fs::MetadataExt;
use std::os::unix::fs::PermissionsExt;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::{Duration, Instant};
use structopt::StructOpt;
use watchman_client::prelude::*;

#[derive(StructOpt, Debug)]
#[structopt(about = "Audit Watchman's in-memory database with the filesystem")]
pub(crate) struct AuditCmd {
    #[structopt(name = "path", parse(from_os_str))]
    path: PathBuf,

    #[structopt(
        long = "timeout",
        about = "seconds to wait for Watchman query result",
        default_value = "120"
    )]
    timeout_secs: u64,
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

// Skip cookies when diffing.
fn is_cookie<T: AsRef<Path>>(name: T) -> bool {
    return name
        .as_ref()
        .file_name()
        .and_then(|s| s.to_str())
        .map_or(false, |s| s.starts_with(".watchman-cookie-"));
}

impl AuditCmd {
    pub(crate) async fn run(&self) -> anyhow::Result<()> {
        let client = Connector::new().connect().await?;
        let resolved = Arc::new(
            client
                .resolve_root(CanonicalPath::canonicalize(&self.path)?)
                .await?,
        );

        if resolved.watcher() == "eden" {
            return Err(anyhow::anyhow!(
                "{} is an EdenFS mount - no need to audit",
                resolved.project_root().display()
            ));
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
                let resolved_path = Arc::new(resolved.path());

                let resolved_path_copy = resolved_path.clone();
                let walk_dir = WalkDir::new(&*resolved_path)
                    .skip_hidden(false)
                    .process_read_dir(move |_depth, path, _read_dir_state, children| {
                        let resolved_path: &Path = resolved_path_copy.as_ref();
                        let from_root = match path.strip_prefix(resolved_path) {
                            Ok(from_root) => from_root,
                            Err(_) => {
                                return;
                            }
                        };

                        if from_root == Path::new("") {
                            children.retain(|child| match child {
                                Ok(child) => {
                                    ignore_dirs.iter().all(|i| i.as_os_str() != child.file_name)
                                }
                                Err(_) => true,
                            });
                        }
                    });

                for entry in walk_dir {
                    let entry = match entry {
                        Ok(entry) => entry,
                        Err(err) => {
                            eprintln!("error while traversing directory: {}", err);
                            continue;
                        }
                    };

                    let entry_path = entry.path();
                    let relpath = match entry_path.strip_prefix(&*resolved_path) {
                        Ok(relpath) => relpath,
                        Err(err) => {
                            eprintln!(
                                "unable to form relative path from {} to {}: {}",
                                resolved_path.display(),
                                entry_path.display(),
                                err
                            );
                            continue;
                        }
                    };

                    let metadata = match entry.metadata() {
                        Ok(metadata) => metadata,
                        Err(err) => {
                            if err.io_error().map(|e| e.kind() == ErrorKind::NotFound) != Some(true)
                            {
                                eprintln!(
                                    "error fetching metadata for {}: {}",
                                    entry_path.display(),
                                    err
                                );
                            }
                            continue;
                        }
                    };
                    filesystem_state.insert(relpath.to_path_buf(), metadata);
                }
                // Watchman doesn't return information about the root, so remove it here.
                filesystem_state.remove(&PathBuf::new());
                eprintln!("Crawled filesystem in {:?}", start_crawl.elapsed());

                filesystem_state
            })
        };

        let start_query = Instant::now();

        // Do not ignore fresh instance results: the goal is to validate the
        // correctness of query results against the filesystem state, no
        // matter what happened in the crawler.

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
                    sync_timeout: SyncTimeout::Duration(Duration::new(self.timeout_secs, 0)),
                    ..Default::default()
                },
            )
            .await;

        let result = match result {
            Ok(result) => result,
            Err(err) => {
                eprintln!("Error during Watchman query: {}", err);
                // Use a different error code for Watchman query errors, including timeouts, so they can be differentiated by audit logging.
                std::process::exit(2);
            }
        };

        eprintln!(
            "Queried Watchman in {:?} (is_fresh_instance = {}, clock = {:?}, version = {})",
            start_query.elapsed(),
            result.is_fresh_instance,
            result.clock,
            result.version
        );

        let filesystem_state = filesystem_state_handle.await.unwrap();

        let diff_start = Instant::now();

        let watchman_files = match result.files {
            Some(files) => files,
            None => {
                return Err(anyhow::anyhow!("No files set in result"));
            }
        };

        let mut any_differences = false;
        let mut phantoms = vec![];
        let mut missing = vec![];

        let mut watchman_state: AHashMap<&Path, &AuditQueryResult> =
            AHashMap::with_capacity(watchman_files.len());
        for watchman_file in &watchman_files {
            let filename = &*watchman_file.name;
            // Skip cookies when diffing.
            if is_cookie(filename) {
                continue;
            }
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
                for diff in diffs {
                    println!("  {}", diff);
                }
                println!("  oclock is {:?}", *watchman_file.oclock);
                any_differences = true;
            }
        }

        for (path, val) in &filesystem_state {
            if is_cookie(path) {
                continue;
            }
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
