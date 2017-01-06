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
 * Creates a callback that invokes the `next` function if an error occured
 * (first arg). Otherwise it will call `cb` with the single arg.
 * @param {function} cb Function to gracefully wrap.
 * @param {next} next Continues.
 * @return {function} Error handling wrapper.
 */
var guard = function(next, cb) {
  return function(err /*rest args*/) {
    if (err) {
      next(new Error(err));
    } else {
      cb.apply(null, Array.prototype.slice.call(arguments, 1));
    }
  };
};


module.exports = guard;
