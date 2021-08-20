#!/usr/bin/env python
# Copyright (c) Facebook, Inc. and its affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from __future__ import absolute_import, division, print_function, unicode_literals


"Facebook-specific additions to the fbcode_builder spec for watchman"

config = read_fbcode_builder_config("fbcode_builder_config.py")  # noqa: F821
config["legocastle_opts"] = {
    "alias": "watchman-oss-linux",
    "oncall": "eden",
    "build_name": "Open-source build for watchman",
    "legocastle_os": "ubuntu_16.04",
}
