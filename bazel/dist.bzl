load("@with_cfg.bzl", "with_cfg")

opt_filegroup, _opt_filegroup_internal = with_cfg(native.filegroup).set(
    "compilation_mode",
    "opt",
).build()
