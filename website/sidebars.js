/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * @format
 */

module.exports = {
  someSidebar: {
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
    Installation: ['install','release-notes'],
    Compatibility: ['compatibility', 'capabilities'],
    Queries: [
      'clockspec',
      'file-query',
      'simple-query',
      'scm-query'
    ],
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
    Expressions : [
      'allof',
      'anyof',
      'dirname',
      'empty',
      'exists',
      'match',
      'name',
      'not',
      'pcre',
      'since',
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
