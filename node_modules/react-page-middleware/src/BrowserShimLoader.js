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

var JS = require('node-haste/lib/resource/JS');
var ResourceLoader = require('node-haste/lib/loader/ResourceLoader');

var browserBuiltins = require('browser-builtins');
var extract = require('node-haste/lib/parse/extract');
var path = require('path');
var util = require('util');

var inherits = util.inherits;

function BrowserShimLoader(buildConfig) {
  this.buildConfig = buildConfig || {};
  this.browserBuiltinPaths = {};

  var realReactMiddlewarePath = path.resolve(__dirname, '..');
  /**
   * Store both the "real" path and the path relative to the `projectRoot`,
   * which would preserve any symlinks through node_modules.
   */
  for (var builtInName in browserBuiltins) {
    var realPath = browserBuiltins[builtInName];
    var relativeToMiddleware = path.relative(realReactMiddlewarePath, realPath);
    var inTermsOfProjectRoot = path.join(
      buildConfig.projectRoot,
      'node_modules',
      'react-page-middleware',
      relativeToMiddleware
    );
    this.browserBuiltinPaths[inTermsOfProjectRoot] = builtInName;
    this.browserBuiltinPaths[realPath] = builtInName;
  }

}
inherits(BrowserShimLoader, ResourceLoader);
BrowserShimLoader.prototype.path = __filename;

BrowserShimLoader.prototype.getResourceTypes = function() {
  return [JS];
};

BrowserShimLoader.prototype.getExtensions = function() {
  return ['.js'];
};

BrowserShimLoader.prototype.loadFromSource =
  function(path, configuration, sourceCode, messages, callback) {
    var shimJS = new JS(path);
    shimJS.id = this.browserBuiltinPaths[path];
    shimJS.isModule = true;
    shimJS.requiredModules = extract.requireCalls(sourceCode);
    if (!shimJS.id) {
      throw new Error('Cannot find shim for:' + path);
    }
    callback(messages, shimJS);
  };

BrowserShimLoader.prototype.matchPath = function(filePath) {
  return !!this.browserBuiltinPaths[filePath];
};

module.exports = BrowserShimLoader;
