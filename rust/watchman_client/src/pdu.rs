//! This module defines the request and response PDU types used by the
//! watchman protocol.

use crate::expr::Expr;
use serde::{Deserialize, Serialize};
use serde_with_macros::*;
use std::path::PathBuf;

/// The `get-sockname` command response
#[derive(Deserialize, Debug)]
pub struct GetSockNameResponse {
    pub version: String,
    pub sockname: Option<PathBuf>,
    pub error: Option<String>,
}

/// The `clock` command response
#[derive(Deserialize, Debug)]
pub struct ClockResponse {
    pub version: String,
    pub clock: Option<ClockSpec>,
    pub error: Option<String>,
}

/// The `clock` command request.
#[derive(Serialize, Debug)]
pub struct ClockRequest(pub &'static str, pub PathBuf, pub ClockRequestParams);

#[derive(Serialize, Debug)]
pub struct ClockRequestParams {
    #[serde(skip_serializing_if = "Option::is_none")]
    pub sync_timeout: Option<i64>,
}

/// The `watch-project` command request.
/// You should use `Client::resolve_root` rather than directly
/// constructing this type.
#[derive(Serialize)]
pub struct WatchProjectRequest(pub &'static str, pub PathBuf);

/// The `watch-project` response
#[derive(Deserialize, Debug)]
pub struct WatchProjectResponse {
    /// The watchman server version
    pub version: String,
    /// The path relative to the root of the project; if not none,
    /// this must be passed to QueryRequestCommon::relative_root
    pub relative_path: Option<PathBuf>,
    /// The root of the watched project
    pub watch: PathBuf,
    /// The watcher that the server is using to monitor this path
    pub watcher: String,
}

/// When using the `path` generator, this specifies a path to be
/// examined.
/// <https://facebook.github.io/watchman/docs/file-query.html#path-generator>
#[derive(Serialize, Clone, Debug)]
#[serde(untagged)]
pub enum PathGeneratorElement {
    RecursivePath(PathBuf),
    ConstrainedDepth { path: PathBuf, depth: i64 },
}

/// The `query` request
#[derive(Serialize, Clone, Debug)]
pub struct QueryRequest(pub &'static str, pub PathBuf, pub QueryRequestCommon);

#[allow(clippy::trivially_copy_pass_by_ref)]
fn is_false(v: &bool) -> bool {
    !*v
}

/// The query parameters.
/// There are a large number of fields that influence the behavior.
///
/// A query consists of three phases:
/// 1. Candidate generation
/// 2. Result filtration (using the `expression` term)
/// 3. Result rendering
///
/// The generation phase is explained in detail here:
/// <https://facebook.github.io/watchman/docs/file-query.html#generators>
///
/// Note that it is legal to combine multiple generators but that it
/// is often undesirable to do so.
/// Not specifying a generator results in the default "all-files" generator
/// being used to iterate all known files.
///
/// The filtration and expression syntax is explained here:
/// <https://facebook.github.io/watchman/docs/file-query.html#expressions>
#[derive(Serialize, Default, Clone, Debug)]
pub struct QueryRequestCommon {
    /// If set, enables the glob generator and specifies a set of globs
    /// that will be expanded into a list of file names and then filtered
    /// according to the expression field.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub glob: Option<Vec<String>>,

    /// If using the glob generator and set to true, do not treat the backslash
    /// character as an escape sequence
    #[serde(default, skip_serializing_if = "is_false")]
    pub glob_noescape: bool,

    /// If using the glob generator and set to true, include files whose basename
    /// starts with `.` in the results. The default behavior for globs is to
    /// exclude those files from the results.
    #[serde(default, skip_serializing_if = "is_false")]
    pub glob_includedotfiles: bool,

    /// If set, enables the use of the `path` generator.
    /// <https://facebook.github.io/watchman/docs/file-query.html#path-generator>
    #[serde(skip_serializing_if = "Option::is_none")]
    pub path: Option<Vec<PathGeneratorElement>>,

    /// If set, enables the use of the `suffix` generator, and specifies the
    /// list of filename suffixes.
    /// In virtualized filesystems this can result in an expensive O(project)
    /// filesystem walk, so it is strongly recommended that you scope this to
    /// a relatively shallow subdirectory.
    ///
    /// <https://facebook.github.io/watchman/docs/file-query.html#suffix-generator>
    #[serde(skip_serializing_if = "Option::is_none")]
    pub suffix: Option<Vec<PathBuf>>,

    /// If set, enables the use of the `since` generator and specifies the last
    /// time you queried the server and for which you wish to receive a delta of
    /// changes.
    /// You will typically thread the QueryResult.clock field back to a subsequent
    /// since query to process the continuity of matching file changes.
    /// <https://facebook.github.io/watchman/docs/file-query.html#since-generator>
    #[serde(skip_serializing_if = "Option::is_none")]
    pub since: Option<Clock>,

    /// if set, indicates that all input paths are relative to this subdirectory
    /// in the project, and that all returned filenames will also be relative to
    /// this subdirectory.
    /// In large virtualized filesystems it is undesirable to leave this set to
    /// None as it makes it more likely that you will trigger an O(project)
    /// filesystem walk.
    /// This field is set automatically from the ResolvedRoot when you perform queries
    /// using Client::query.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub relative_root: Option<PathBuf>,

    /// If set, specifies the expression to use to filter the candidate matches
    /// produced by the selected query generator.
    /// Each candidate is visited in turn and has the expression applied.
    /// Candidates for which the expression evaluates as true will be included
    /// in the returned list of files.
    /// If left unspecified, the server will assume `Expr::True`.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub expression: Option<Expr>,

    /// Specifies the list of fields names returned by the server.
    /// The `name` field should be considered a required field and is the cheapest
    /// field to return.
    /// Depending on the watcher implementation, other metadata has varying cost.
    /// In general, avoid querying `size` and `mode` fields and instead prefer to
    /// query `content.sha1hex` and `type` instead to avoid materializing inodes
    /// in a virtualized filesystem.
    pub fields: Vec<&'static str>,

    /// If true you indicate that you know how to 100% correctly deal with a fresh
    /// instance result set.  It is strongly recommended that you leave this
    /// option alone as it is a common source of cache invalidation and divergence
    /// issues for clients.
    #[serde(default, skip_serializing_if = "is_false")]
    pub empty_on_fresh_instance: bool,

    /// If true, treat filenames as case sensitive even on filesystems that otherwise
    /// appear to be case insensitive.
    /// This can improve performance of directory traversal in queries by turning
    /// O(directory-size) operations into an O(1) hash lookup.
    /// <https://facebook.github.io/watchman/docs/cmd/query.html#case-sensitivity>
    #[serde(default, skip_serializing_if = "is_false")]
    pub case_sensitive: bool,

    /// If set, override the default synchronization timeout.
    /// The timeout controls how long the server will wait to observe a cookie
    /// file through the notification stream.
    /// If the timeout is reached, the query will fail.
    /// <https://facebook.github.io/watchman/docs/cookies.html>
    /// <https://facebook.github.io/watchman/docs/cmd/query.html#synchronization-timeout-since-21>
    #[serde(skip_serializing_if = "Option::is_none")]
    pub sync_timeout: Option<i64>,

    /// If set to true, when mixing generators (not recommended), dedup results by filename.
    /// This defaults to false.  When not enabled, if multiple generators match
    /// the same file, it will appear twice in the result set.
    /// Turning on dedup_results will increase the memory cost of processing a query
    /// and build an O(result-size) hash map to dedup the results.
    #[serde(default, skip_serializing_if = "is_false")]
    pub dedup_results: bool,

    /// Controls the duration that the server will wait to obtain a lock on the
    /// filesystem view.
    /// You should not normally need to change this.
    /// <https://facebook.github.io/watchman/docs/cmd/query.html#lock-timeout>
    #[serde(skip_serializing_if = "Option::is_none")]
    pub lock_timeout: Option<i64>,

    /// If set, records the request_id in internal performance sampling data.
    /// It is also exported through the environment as HGREQUESTID so that
    /// the context of the request can be passed down to any child mercurial
    /// processes that might be spawned as part of processing source control
    /// aware queries.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub request_id: Option<String>,
}

/// Holds the result of a query.
/// The result is generic over a `F` type that you define.
/// The `F` should deserialize the list of fields in your QueryRequestCommon
/// struct.
#[derive(Deserialize, Clone, Debug)]
pub struct QueryResult<F>
where
    F: std::fmt::Debug + Clone,
{
    /// The version of the watchman server
    pub version: String,
    /// If present and true, indicates that this result set represents the
    /// total set of possible matches.  Otherwise the results should be
    /// considered to be incremental since your last since query.
    /// If is_fresh_instance is true you MUST arrange to forget about
    /// any files not included in the list of files in this QueryResult
    /// otherwise you risk diverging your state.
    pub is_fresh_instance: Option<bool>,

    /// If present, indicates that the query failed.  Holds the error
    /// message.
    pub error: Option<String>,

    /// Holds the list of matching files from the query
    pub files: Option<Vec<F>>,
}

/// A `Clock` is used to refer to a logical point in time.
/// Internally, watchman maintains a monotonically increasing tick counter
/// along with some additional data to detect A-B-A style situations if
/// eg: the watchman server is restarted.
///
/// Clocks are important when using the recency index with the `since`
/// generator.
///
/// A clock can also encoded metadata describing the state of source
/// control to work with source control aware queries:
/// <https://facebook.github.io/watchman/docs/scm-query.html>
#[derive(Serialize, Deserialize, Debug, Clone)]
#[serde(untagged)]
pub enum Clock {
    /// Just a basic ClockSpec
    Spec(ClockSpec),
    /// A clock embedding additional source control information
    ScmAware(FatClockData),
}

/// The fundamental clock specifier string.
/// The contents of the string should be considered to be opaque to
/// the client as the server occasionally evolves the meaning of
/// the clockspec and its format is expressly not a stable API.
///
/// In particular, there is no defined way for a client to reason
/// about the relationship between any two ClockSpec's.
///
/// <https://facebook.github.io/watchman/docs/clockspec.html>
#[derive(Serialize, Deserialize, Debug, Clone)]
pub struct ClockSpec(String);

/// Construct a null clockspec
impl Default for ClockSpec {
    fn default() -> Self {
        Self::null()
    }
}

impl ClockSpec {
    /// Construct a null clockspec.
    /// This indicates a time before any changes occurred and will
    /// cause a `since` generator based query to emit a fresh instance
    /// result set that contains all possible matches.
    /// It is appropriate to use a null clock in cases where you are
    /// starting up from scratch and don't have a saved clock value
    /// to use as the basis for your query.
    pub fn null() -> Self {
        Self("c:0:0".to_string())
    }

    /// Construct a named cursor clockspec.
    ///
    /// Using a named cursor causes the server to maintain the name -> clock
    /// value mapping on the behalf of your tool.  This frees your client
    /// from the need to manage storing of the clock between queries but
    /// does require an exclusive lock for the duration of the query, which
    /// serializes the query with all other clients.
    ///
    /// The namespace is per watched project so your cursor name must be
    /// unique enough to not collide with other tools that use this same
    /// feature.
    ///
    /// There is no way to clear the value associated with a named cursor.
    ///
    /// The first time you use a named cursor, it has an effective value
    /// of the null clock.
    ///
    /// We do not recommend using named cursors because of the exclusive
    /// lock requirement.
    pub fn named_cursor(cursor: &str) -> Self {
        Self(format!("n:{}", cursor))
    }

    /// A clock specified as a unix timestamp.
    /// The watchman server will never generate a clock in this form,
    /// but will accept them in `since` generator based queries.
    /// Using UnixTimeStamp is discouraged as it has granularity of
    /// 1 second and will often result in over-reporting the same events
    /// when they happen in the same second.
    pub fn unix_timestamp(time_t: i64) -> Self {
        Self(time_t.to_string())
    }
}

/// Holds extended clock data that includes source control aware
/// query metadata.
/// <https://facebook.github.io/watchman/docs/scm-query.html>
#[skip_serializing_none]
#[derive(Serialize, Deserialize, Debug, Clone)]
pub struct FatClockData {
    pub clock: ClockSpec,
    pub scm: Option<ScmAwareClockData>,
}

/// Holds extended clock data that includes source control aware
/// query metadata.
/// <https://facebook.github.io/watchman/docs/scm-query.html>
#[skip_serializing_none]
#[derive(Serialize, Deserialize, Debug, Clone)]
pub struct ScmAwareClockData {
    pub mergebase: Option<String>,
    #[serde(rename = "mergebase-with")]
    pub mergebase_with: Option<String>,

    #[serde(rename = "saved-state")]
    pub saved_state: Option<SavedStateClockData>,
}

/// Holds extended clock data that includes source control aware
/// query metadata.
/// <https://facebook.github.io/watchman/docs/scm-query.html>
#[skip_serializing_none]
#[derive(Serialize, Deserialize, Debug, Clone)]
pub struct SavedStateClockData {
    pub storage: Option<String>,
    #[serde(rename = "commit-id")]
    pub commit: Option<String>,
    pub config: Option<serde_json::Value>,
}

/// Reports the content SHA1 hash for a file.
/// Since computing the hash can fail, this struct can also represent
/// the error that happened during hash computation.
#[derive(Deserialize, Debug, Clone)]
#[serde(untagged)]
pub enum ContentSha1Hex {
    /// The 40-hex-digit SHA1 content hash of the file contents
    Hash(String),
    /// The error that occured while trying to determine the hash
    Error { error: String },
}

/// Encodes the file type field returned in query results and
/// specified in expression terms.
///
/// <https://facebook.github.io/watchman/docs/expr/type.html>
///
/// Use this in your query file struct like this:
///
/// ```
/// use serde::Deserialize;
/// use watchman_client::prelude::*;
/// #[derive(Deserialize, Debug, Clone)]
/// struct NameAndType {
///    name: std::path::PathBuf,
///    #[serde(rename = "type")]
///    file_type: FileType,
/// }
/// ```
#[derive(Serialize, Deserialize, Debug, Clone, Copy)]
#[serde(from = "String", into = "String")]
pub enum FileType {
    BlockSpecial,
    CharSpecial,
    Directory,
    Regular,
    Fifo,
    Symlink,
    Socket,
    SolarisDoor,
}

impl std::string::ToString for FileType {
    fn to_string(&self) -> String {
        (*self).into()
    }
}

impl From<String> for FileType {
    fn from(s: String) -> Self {
        match s.as_ref() {
            "b" => Self::BlockSpecial,
            "c" => Self::CharSpecial,
            "d" => Self::Directory,
            "f" => Self::Regular,
            "p" => Self::Fifo,
            "l" => Self::Symlink,
            "s" => Self::Socket,
            "D" => Self::SolarisDoor,
            unknown => panic!("Watchman Server returned impossible file type {}", unknown),
        }
    }
}

impl Into<String> for FileType {
    fn into(self) -> String {
        match self {
            Self::BlockSpecial => "b",
            Self::CharSpecial => "c",
            Self::Directory => "d",
            Self::Regular => "f",
            Self::Fifo => "p",
            Self::Symlink => "l",
            Self::Socket => "s",
            Self::SolarisDoor => "D",
        }
        .to_string()
    }
}
