use crate::pdu::*;
use serde::Serialize;
use serde_json::{json, Value};

/// An expression term used to filter candidate files from query results.
#[derive(Serialize, Debug, Clone)]
#[serde(into = "serde_json::Value")]
pub enum Expr {
    /// Always evaluates to true
    True,

    /// Always evaluates to false
    False,

    /// Inverts the match state of the child term
    Not(Box<Expr>),

    /// Evaluates to true IFF all child terms evaluate true
    All(Vec<Expr>),

    /// Evaluates to true if any child terms evaluate true
    Any(Vec<Expr>),

    /// Match on the parent directory structure
    /// https://facebook.github.io/watchman/docs/expr/dirname.html
    DirName(DirNameTerm),

    /// Evaluates as true if the file exists, has size 0 and is a regular
    /// file or directory.
    /// https://facebook.github.io/watchman/docs/expr/empty.html
    Empty,

    /// Evaluates as true if the file exists; this is useful for filtering
    /// out notifications for files that have been deleted.
    /// Note that this term doesn't add value for `path` and `glob` generators
    /// which implicitly add this constraint.
    /// https://facebook.github.io/watchman/docs/expr/exists.html
    Exists,

    /// Performs a glob-style match against the file name
    /// https://facebook.github.io/watchman/docs/expr/match.html
    Match(MatchTerm),

    /// Performs an exact match against the file name.
    /// https://facebook.github.io/watchman/docs/expr/name.html
    Name(NameTerm),

    /// Use PCRE to match the filename.
    /// Note that this is an optional server feature and using this term
    /// on a server that doesn't support this feature will generate an
    /// error in response to the query.
    /// https://facebook.github.io/watchman/docs/expr/pcre.html
    Pcre(PcreTerm),

    /// Evaluates as true if the specified time property of the file is
    /// greater than the since value.
    /// https://facebook.github.io/watchman/docs/expr/since.html
    Since(SinceTerm),

    /// Evaluate as true if the size of a file matches the specified constraint.
    /// Files that do not presently exist will evaluate as false.
    /// https://facebook.github.io/watchman/docs/expr/size.html
    Size(RelOp),

    /// Evaluate as true if the filename suffix (also known as extension)
    /// matches the provided set of suffixes.
    /// Suffix matches are always case insensitive.
    /// `php` matches `foo.php` and `foo.PHP` but not `foophp`.
    /// https://facebook.github.io/watchman/docs/expr/suffix.html
    // FIXME: this String should be PathBuf but we cannot guarantee
    // representation while we rely on serde_json::Value as an intermediate.
    Suffix(Vec<String>),

    /// Evaluate as true if the file type exactly matches the specified type.
    FileType(FileType),
}

impl Into<Value> for Expr {
    fn into(self) -> Value {
        match self {
            Self::True => "true".into(),
            Self::False => "false".into(),
            Self::Not(expr) => Value::Array(vec!["not".into(), (*expr).into()]),
            Self::All(expr) => {
                let mut expr: Vec<Value> = expr.into_iter().map(Into::into).collect();
                expr.insert(0, "allof".into());
                Value::Array(expr)
            }
            Self::Any(expr) => {
                let mut expr: Vec<Value> = expr.into_iter().map(Into::into).collect();
                expr.insert(0, "anyof".into());
                Value::Array(expr)
            }
            Self::DirName(term) => {
                let mut expr: Vec<Value> = vec!["dirname".into(), term.path.into()];
                if let Some(depth) = term.depth {
                    expr.push(depth.into_term("depth"));
                }
                expr.into()
            }
            Self::Empty => "empty".into(),
            Self::Exists => "exists".into(),
            Self::Match(term) => json!([
                "match",
                term.glob,
                if term.wholename {
                    "wholename"
                } else {
                    "basename"
                },
                {
                    "includedotfiles": term.include_dot_files,
                    "noescape": term.no_escape
                },
            ]),
            Self::Name(term) => json!([
                "name",
                Value::Array(term.paths.into_iter().map(Into::into).collect()),
                if term.wholename {
                    "wholename"
                } else {
                    "basename"
                }
            ]),
            Self::Pcre(term) => json!([
                "pcre",
                term.pattern,
                if term.wholename {
                    "wholename"
                } else {
                    "basename"
                }
            ]),
            Self::Since(term) => match term {
                SinceTerm::ObservedClock(c) => json!(["since", c, "oclock"]),
                SinceTerm::CreatedClock(c) => json!(["since", c, "cclock"]),
                SinceTerm::MTime(c) => json!(["since", c.to_string(), "mtime"]),
                SinceTerm::CTime(c) => json!(["since", c.to_string(), "ctime"]),
            },
            Self::Size(term) => term.into_term("size"),
            Self::Suffix(term) => json!([
                "suffix",
                Value::Array(term.into_iter().map(Into::into).collect())
            ]),
            Self::FileType(term) => json!(["type", term.to_string()]),
        }
    }
}

/// Performs an exact match against the file name.
/// https://facebook.github.io/watchman/docs/expr/name.html
#[derive(Clone, Debug)]
pub struct NameTerm {
    // FIXME: this String should be PathBuf but we cannot guarantee
    // representation while we rely on serde_json::Value as an intermediate.
    pub paths: Vec<String>,
    /// By default, the name is evaluated against the basename portion
    /// of the filename.  Set wholename=true to have it match against
    /// the path relative to the root of the project.
    pub wholename: bool,
}

/// Match on the parent directory structure
/// https://facebook.github.io/watchman/docs/expr/dirname.html
#[derive(Clone, Debug)]
pub struct DirNameTerm {
    /// The path to a directory
    // FIXME: this String should be PathBuf but we cannot guarantee
    // representation while we rely on serde_json::Value as an intermediate.
    pub path: String,
    /// Specifies the matching depth.  A file has depth == 0
    /// if it is contained directory within `path`, depth == 1 if
    /// it is in a direct child directory of `path`, depth == 2 if
    /// in a grand-child directory and so on.
    /// If None, the default is considered to GreaterOrEqual depth 0.
    pub depth: Option<RelOp>,
}

/// Use PCRE to match the filename.
/// Note that this is an optional server feature and using this term
/// on a server that doesn't support this feature will generate an
/// error in response to the query.
/// https://facebook.github.io/watchman/docs/expr/pcre.html
#[derive(Clone, Debug, Default)]
pub struct PcreTerm {
    /// The perl compatible regular expression
    pub pattern: String,

    /// By default, the name is evaluated against the basename portion
    /// of the filename.  Set wholename=true to have it match against
    /// the path relative to the root of the project.
    pub wholename: bool,
}

/// Encodes the match expression term
/// https://facebook.github.io/watchman/docs/expr/match.html
#[derive(Clone, Debug, Default)]
pub struct MatchTerm {
    /// The glob expression to evaluate
    pub glob: String,
    /// By default, the glob is evaluated against the basename portion
    /// of the filename.  Set wholename=true to have it match against
    /// the path relative to the root of the project.
    pub wholename: bool,
    /// By default, paths whose names start with a `.` are not matched.
    /// Set include_dot_files=true to include them
    pub include_dot_files: bool,
    /// By default, backslashes in the pattern escape the next character.
    /// To have `\` treated literally, set no_escape=true.
    pub no_escape: bool,
}

/// Specifies a relational comparison with an integer value
#[derive(Clone, Debug)]
pub enum RelOp {
    Equal(usize),
    NotEqual(usize),
    Greater(usize),
    GreaterOrEqual(usize),
    Less(usize),
    LessOrEqual(usize),
}

impl RelOp {
    fn into_term(self, field: &str) -> Value {
        let (op, value) = match self {
            Self::Equal(value) => ("eq", value),
            Self::NotEqual(value) => ("ne", value),
            Self::Greater(value) => ("gt", value),
            Self::GreaterOrEqual(value) => ("ge", value),
            Self::Less(value) => ("lt", value),
            Self::LessOrEqual(value) => ("le", value),
        };
        Value::Array(vec![field.into(), op.into(), value.into()])
    }
}

/// Evaluates as true if the specified time property of the file is greater
/// than the since value.
/// https://facebook.github.io/watchman/docs/expr/since.html
#[derive(Clone, Debug)]
pub enum SinceTerm {
    /// Yield true if the file was observed to be modified more recently than
    /// the specified clockspec
    ObservedClock(ClockSpec),

    /// Yield true if the file changed from !exists -> exists more recently
    /// than the specified clockspec
    CreatedClock(ClockSpec),

    /// Yield true if the mtime stat field is >= the provided timestamp.
    /// Note that this is >= because it has 1-second granularity.
    MTime(i64),

    /// Yield true if the ctime stat field is >= the provided timestamp.
    /// Note that this is >= because it has 1-second granularity.
    CTime(i64),
}

#[cfg(test)]
mod tests {
    use super::*;

    fn val(expr: Expr) -> Value {
        expr.into()
    }

    #[test]
    fn exprs() {
        assert_eq!(val(Expr::True), json!("true"));
        assert_eq!(val(Expr::False), json!("false"));
        assert_eq!(val(Expr::Empty), json!("empty"));
        assert_eq!(val(Expr::Exists), json!("exists"));
        assert_eq!(
            val(Expr::Not(Box::new(Expr::False))),
            json!(["not", "false"])
        );
        assert_eq!(
            val(Expr::All(vec![Expr::True, Expr::False])),
            json!(["allof", "true", "false"])
        );
        assert_eq!(
            val(Expr::Any(vec![Expr::True, Expr::False])),
            json!(["anyof", "true", "false"])
        );

        assert_eq!(
            val(Expr::DirName(DirNameTerm {
                path: "foo".into(),
                depth: None,
            })),
            json!(["dirname", "foo"])
        );
        assert_eq!(
            val(Expr::DirName(DirNameTerm {
                path: "foo".into(),
                depth: Some(RelOp::GreaterOrEqual(1)),
            })),
            json!(["dirname", "foo", ["depth", "ge", 1]])
        );

        assert_eq!(
            val(Expr::Match(MatchTerm {
                glob: "*.txt".into(),
                ..Default::default()
            })),
            json!(["match", "*.txt", "basename", {
                "includedotfiles": false,
                "noescape": false,
            }])
        );

        assert_eq!(
            val(Expr::Match(MatchTerm {
                glob: "*.txt".into(),
                wholename: true,
                include_dot_files: true,
                ..Default::default()
            })),
            json!(["match", "*.txt", "wholename", {
                "includedotfiles": true,
                "noescape": false,
            }])
        );

        assert_eq!(
            val(Expr::Name(NameTerm {
                paths: vec!["foo".into()],
                wholename: true,
            })),
            json!(["name", ["foo"], "wholename"])
        );

        assert_eq!(
            val(Expr::Pcre(PcreTerm {
                pattern: "foo$".into(),
                wholename: true,
            })),
            json!(["pcre", "foo$", "wholename"])
        );

        assert_eq!(val(Expr::FileType(FileType::Regular)), json!(["type", "f"]));

        assert_eq!(
            val(Expr::Suffix(vec!["php".into(), "js".into()])),
            json!(["suffix", ["php", "js"]])
        );

        assert_eq!(
            val(Expr::Since(SinceTerm::ObservedClock(ClockSpec::null()))),
            json!(["since", "c:0:0", "oclock"])
        );
    }
}
