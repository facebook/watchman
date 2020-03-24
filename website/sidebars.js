/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * @format
 */

module.exports = {
  sidebar: {
    Installation: ['install', 'release-notes'],
    Invocation: [
      'cli-options',
      'watchman-make',
      'watchman-wait',
      'watchman-replicate-subscription',
      'cppclient',
      'nodejs',
      'config',
      'socket-interface'
    ],
    Compatibility: ['compatibility', 'capabilities'],
    Commands: [
      'find',
      'flush-subscriptions',
      'get-config',
      'get-sockname',
      'list-capabilities',
      'log',
      'log-level',
      'query',
      'shutdown-server',
      'since',
      'state-enter',
      'state-leave',
      'subscribe',
      'trigger',
      'trigger-del',
      'trigger-list',
      'unsubscribe',
      'version',
      'watch',
      'watch-del',
      'watch-del-all',
      'watch-list',
      'watch-project'
    ],
    Queries: [
      'clockspec',
      'file-query',
      'simple-query',
      'scm-query'
    ],
    "Expressions Terms": [
      'allof',
      'anyof',
      'dirname',
      'empty',
      'exists',
      'match',
      'name',
      'not',
      'pcre',
      'exp-since',
      'size',
      'suffix',
      'type'
    ],
    Internals: [
      'bser',
      'casefolding',
      'contributing',
      'cookies'
    ],
    Troubleshooting: ['troubleshooting']
  },
};
