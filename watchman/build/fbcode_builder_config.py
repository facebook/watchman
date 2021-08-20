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

import os

import specs.fbthrift as fbthrift
import specs.folly as folly
import specs.gmock as gmock
from shell_quoting import ShellQuoted, path_join


"fbcode_builder steps to build & test watchman"


def fbcode_builder_spec(builder):
    builder.add_option("watchman/_build:cmake_defines", {"BUILD_SHARED_LIBS": "OFF"})
    builder.add_option("watchman/_build:cmake_defines", {"USE_SYS_PYTHON": "ON"})

    projects = builder.option("projects_dir")

    return {
        "depends_on": [gmock, folly, fbthrift],
        "steps": [
            builder.fb_github_cmake_install("watchman/_build", ".."),
            builder.step(
                "Run watchman tests",
                [
                    builder.run(
                        ShellQuoted("ctest --output-on-failure -j {n}").format(
                            n=builder.option("make_parallelism")
                        )
                    ),
                    builder.run(
                        ShellQuoted(
                            "cd ../ && ./runtests.py --concurrency {n} "
                            "--watchman-path _build/watchman --pybuild-dir {p}"
                        ).format(
                            n=builder.option("make_parallelism"),
                            p=path_join(
                                projects, "../shipit_projects/watchman/_build/python"
                            ),
                        )
                    ),
                ],
            ),
        ],
    }


config = {
    "github_project": "facebook/watchman",
    "fbcode_builder_spec": fbcode_builder_spec,
}
