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

/**
 * #Super poor-man's require system. Other systems include larger feature sets
 * for asyncronously loading resources etc. Here, we completely resolve the
 * module load order statically - I'd be open to trying an existing solution if
 * it's lightweight.
 */
var DEFINE_MODULE_CODE =
  "__d(" +
    "'_moduleName_'," +
    "[/* deps */]," +
    "function(global, require, requireDynamic, requireLazy, module, exports) {" +
    "  _code_" +
    "}" +
  ");";

var DEFINE_MODULE_REPLACE_RE = /_moduleName_|_code_/g;

/**
 * Only do the lookup if the name has a dot in it - providesModule don't need
 * any static rewriting of module names. Handles require('./something/path.js');
 */
var REL_REQUIRE_STMT = /require\(['"]([\.\/0-9A-Z_$\-]*)['"]\)/gi;


var Modularizer = {
  /**
   * @param {pageComponent} pageComponent Either node-haste compatible module
   * description (either name or providesModule).
   * @param {string} code String of code to modularize.
   * @return {string} Modularized code.
   */
  modularize: function(pageComponent, code, resolveModule) {

    var relativizedCode = !resolveModule ? code :
      code.replace(REL_REQUIRE_STMT, function(codeMatch, moduleName) {
        return "require('" + resolveModule(moduleName) + "')";
      });
    var ret = DEFINE_MODULE_CODE.replace(DEFINE_MODULE_REPLACE_RE, function(key) {
      return {
        '_moduleName_': pageComponent,
        '_code_': relativizedCode
      }[key];
    });
    return ret;
  }
};

module.exports = Modularizer;
