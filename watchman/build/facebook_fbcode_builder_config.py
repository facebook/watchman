#!/usr/bin/env python
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


"Facebook-specific additions to the fbcode_builder spec for watchman"

config = read_fbcode_builder_config("fbcode_builder_config.py")  # noqa: F821
config["legocastle_opts"] = {
    "alias": "watchman-oss-linux",
    "oncall": "eden",
    "build_name": "Open-source build for watchman",
    "legocastle_os": "ubuntu_16.04",
}
