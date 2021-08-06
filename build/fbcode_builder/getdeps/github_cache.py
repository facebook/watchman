# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

from __future__ import absolute_import, division, print_function, unicode_literals

import os
import shutil

from . import cache as cache_module


class GitHubActionArtifactCache(cache_module.ArtifactCache):
   ENVIRONMENT_VARIABLE = "GITHUB_ACTION_ARTIFACT_CACHE"

   def __init__(self, cache_dir):
      self.cache_dir = cache_dir

   def _path(self, name):
      return os.path.join(self.cache_dir, name)

   def download_to_file(self, name, dest_file_name):
      source = self._path(name)
      try:
         print("Copying {} => {}".format(source, dest_file_name))
         shutil.copyfile(source, dest_file_name)
         return True
      except Exception as e:
         print("Unable to download cache: {}".format(e))
         try:
            os.unlink(dest_file_name)
         except OSError:
            pass

      return False

   def upload_from_file(self, name, source_file_name):
      path = self._path(name)
      try:
         print("Copying {} => {}".format(source_file_name, path))
         shutil.copyfile(source_file_name, path)
      except Exception as e:
         print("Unable to upload cache: {}".format(e))
         pass

   @classmethod
   def create_cache(cls):
      if cls.ENVIRONMENT_VARIABLE in os.environ:
         cache_dir = os.environ[cls.ENVIRONMENT_VARIABLE]
         try:
            if not os.path.exists(cache_dir):
               os.mkdir(cache_dir)
            if not os.path.isdir(cache_dir):
               raise Exception("{} is not a directory".format(cache_dir))
            return cls(cache_dir)
         except Exception as e:
            print("Failed to create artifact cache: {}. Caching is disabled.".format(e))
      return None


cache_module.create_cache = GitHubActionArtifactCache.create_cache