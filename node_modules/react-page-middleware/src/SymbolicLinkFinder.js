/**
 * Copyright 2013 Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

var fs = require('fs');
var path = require('path');

/**
 * Scans directories for files with given extensions (async) Will follow
 * symlinks. Uses node.js native function to traverse, instead of shelling out
 * - for safety. Invokes callback with non-real path.
 *
 * @param  {Array.<String>} scanDirs   Directories to scan, ex: ['html/']
 * @param  {Array.<String>} extensions Extensions to searc for, ex: ['.js']
 * @param  {function|null}  ignore     Optional function to filter out paths
 * @param  {Function}       callback
 */
function find(scanDirs, extensions, ignore, callback) {
  var result = [];
  var activeCalls = 0;

  function readdirRecursive(curDir) {
    activeCalls++;
    fs.readdir(curDir, function(err, names) {
      activeCalls--;
      if (!names) {
        return;
      }

      for (var i = 0; i < names.length; i++) {
        names[i] = path.join(curDir, names[i]);
      }

      names.forEach(function(curFile) {
        if (ignore && ignore(curFile)) {
          return;
        }
        activeCalls++;

        fs.lstat(curFile, function(err, stat) {
          activeCalls--;

          if (!err && stat) {
            if (stat.isDirectory() || stat.isSymbolicLink()) {
              readdirRecursive(curFile);
            } else {
              var ext = path.extname(curFile);
              if (extensions.indexOf(ext) !== -1) {
                result.push([curFile, stat.mtime.getTime()]);
              }
            }
          }
          if (activeCalls === 0) {
            callback(result);
          }
        });
      });

      if (activeCalls === 0) {
        callback(result);
      }
    });
  }

  scanDirs.forEach(readdirRecursive);
}


/**
 * Wrapper for options for a find call
 * @class
 * @param {Object} options
 */
function SymbolicLinkFinder(options) {
  this.scanDirs = options && options.scanDirs || ['.'];
  this.extensions = options && options.extensions || ['.js'];
  this.ignore = options && options.ignore || null;
}

/**
 * @param  {Function} callback
 */
SymbolicLinkFinder.prototype.find = function(callback) {
  find(this.scanDirs, this.extensions, this.ignore, callback);
};


module.exports = SymbolicLinkFinder;
module.exports.find = find;
