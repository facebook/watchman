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
"use strict";

var SourceMapGenerator = require('source-map').SourceMapGenerator;

/**
 * Stateful class, that has an odd "finalized" state, where no more resources
 * may be added to the package. The only reason for this, is that the
 * `ResourceMap` makes it difficult to prepend content to the package, or adjust
 * previously appended mappings. So this class accommodates that by forcing users
 * of a `Package` to organize their code in a way that multiple people may add
 * resources to a package, but only one should "seal" the package, and extract
 * the source maps/text from it.
 */
function Package() {
  this.items = [];
  this._isSealed = false;
  this._sourceMapGenerator = null;
  this._packageStr = null;
}

Package.prototype.push = function(originPath, originSource, transformedSource) {
  this._validateNotSealed();
  return this.items.push({
    originPath: originPath,
    originSource: originSource,
    transformedSource: transformedSource
  });
};

Package.prototype.resourceCount = function() {
  return this.items.length;
};

Package.prototype._validateSealed = function() {
  if (!this._isSealed) {
    throw new Error('Operation may only be invoked on finalized package.');
  }
};

Package.prototype._validateNotSealed = function() {
  if (this._isSealed) {
    throw new Error('Cannot continue to build up a package once generated.');
  }
};

/**
 * Only after invoking this can you access computed package text, and source
 * maps.
 */
Package.prototype._seal = function() {
  this._isSealed = true;
};

Package.prototype._updateSealedPackage = function(computeSourceMaps) {
  this._seal();
  if (computeSourceMaps) {
    this._sourceMapGenerator =
      new SourceMapGenerator({file: 'bundle.js', version: 3});
  }
  var lastCharNewLine = false;
  var packageStr = '';
  var packageLineCount = 0;
  for (var i = 0; i < this.items.length; i++) {
    var transformedLineCount = 0;
    var item = this.items[i];
    var transformedSource = item.transformedSource;
    if (computeSourceMaps) {
      for (var t = 0; t < transformedSource.length; t++) {
        if (t === 0 || lastCharNewLine) {
          this._sourceMapGenerator.addMapping({
            generated: {line: packageLineCount + 1, column: 0},
            original: {line: transformedLineCount + 1, column: 0},
            source: item.originPath
          });
        }
        lastCharNewLine = transformedSource[t] === '\n';
        if (lastCharNewLine) {
          transformedLineCount++;
          packageLineCount++;
        }
      }
      this._sourceMapGenerator.setSourceContent(
        item.originPath,
        item.originSource
      );
    }
    packageStr += transformedSource;
  }
  this._packageStr = packageStr;
};

/**
 * @param {boolean} computeSourceMaps Whether or not to also compute source
 * maps. Pass true if you would like to optimize the case when you will access
 * source maps anyways. (It's just a perf optimization).
 */
Package.prototype.getSealedText = function(computeSourceMaps) {
  if (!this._packageStr) {
    this._updateSealedPackage(computeSourceMaps);
  }
  this._validateSealed();
  return this._packageStr;
};

Package.prototype.getSealedSourceMaps = function() {
  if (!this._sourceMapGenerator) {
    this._updateSealedPackage(true);
  }
  this._validateSealed();
  return this._sourceMapGenerator;
};

Package.prototype.unshift =
  function(originPath, originSource, transformedSource) {
    this._validateNotSealed();
    return this.items.unshift({
      originPath: originPath,
      originSource: originSource,
      transformedSource: transformedSource
    });
  };

module.exports = Package;
