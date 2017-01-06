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

var SourceMapConsumer = require('source-map').SourceMapConsumer;

var ERR_RE = /(<anonymous>):([^:]*):(.*)/g;
var SANDBOX_ERR_MSG = 'ERROR RENDERING PAGE ON SERVER:';


/**
 * @param {string} stack String representing call stack of error.
 * @return {string} Source mapped stack.
 */
var extractSourceMappedStack = function(ppackage, stack) {
  var sourceMap = ppackage.getSealedSourceMaps();
  if (!sourceMap) {
    return "ERROR Computing Source Maps: " + stack;
  }
  var sourceMapsObj = ppackage.getSealedSourceMaps().toJSON();
  var sourceMapConsumer = new SourceMapConsumer(sourceMapsObj);
  var mappedError =
    "\n<mapped error>\n" +
    stack.replace(ERR_RE, function(match, file, line, column) {
      var originalPosition =
        sourceMapConsumer.originalPositionFor({line: line, column: column});
      var origFile = originalPosition && originalPosition.source;
      var origLine = originalPosition && (originalPosition.line);
      return origFile + ':' + origLine + ':' + column;
    }) +
    '\n<\/mapped error>\n';
  return SANDBOX_ERR_MSG + mappedError;
};

module.exports = extractSourceMappedStack;
