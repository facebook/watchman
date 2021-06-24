#!/usr/bin/env python
from __future__ import absolute_import, division, print_function, unicode_literals


"Facebook-specific additions to the fbcode_builder spec for watchman"

config = read_fbcode_builder_config("fbcode_builder_config.py")  # noqa: F821
config["legocastle_opts"] = {
    "alias": "watchman-oss-linux",
    "oncall": "eden",
    "build_name": "Open-source build for watchman",
    "legocastle_os": "ubuntu_16.04",
}
