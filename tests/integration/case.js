var assert = require('assert');
var watchman = require('../../node');
var client = new watchman.Client();
var fs = require('fs');
var path = require('path');

var bar = fs.mkdtempSync('/tmp/BAR-');
var foo = path.join(bar, 'FOO');
fs.mkdirSync(foo);

client.command(['watch', foo], function (error, resp) {
  assert.equal('unable to resolve root ' + foo
                + ': casing problem in \"'+ foo
                + '\"'
                , error.message);
  console.log(error.message);
  client.end();
});
