/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

var assert = require('assert');
var watchman = require('fb-watchman');

function optional() {
  var client = new watchman.Client();
  client.capabilityCheck({optional: ['will-never-exist']},
      function (error, resp) {
        assert.equal(error, null, 'no errors');
        assert.equal(resp.capabilities['will-never-exist'], false);
        client.end();
      });
}
optional();

function required() {
  var client = new watchman.Client();
  client.capabilityCheck({required: ['will-never-exist']},
      function (error, resp) {
        assert.equal('client required capability `will-never-exist` is not' +
                     ' supported by this server', error.message);
        client.end();
      });
}
required();

function synth() {
  var client = new watchman.Client();

  resp = client._synthesizeCapabilityCheck({version: '1.0'},
      ['will-never-exist'], []);
  assert.equal(resp.capabilities['will-never-exist'], false);

  resp = client._synthesizeCapabilityCheck({version: '3.2'},
      ['relative_root'], []);
  assert.equal(resp.capabilities['relative_root'], false);

  resp = client._synthesizeCapabilityCheck({version: '3.3'},
      ['relative_root'], []);
  assert.equal(resp.capabilities['relative_root'], true);

  resp = client._synthesizeCapabilityCheck({version: '1.0'},
      [], ['will-never-exist']);
  assert.equal('client required capability `will-never-exist` is not' +
               ' supported by this server', resp.error);
}
synth();

