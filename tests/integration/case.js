var assert = require('assert');
var watchman = require('../../node');
var client = new watchman.Client();
const fs = require('fs');
const os = require('os');
const path = require('path');

var platform = os.platform();
if (platform == 'win32' || platform == 'darwin') {
  var tmp = fs.realpathSync(process.env.TMPDIR);
  var foo = path.join(tmp, 'foo');
  var FOO = path.join(tmp, 'FOO');

  fs.mkdir(FOO, (err_mk_dir_foo) => {
    assert.equal(err_mk_dir_foo, null, 'no errors');
    var bar = path.join(foo, 'bar');
    var BAR = path.join(FOO, 'bar');

    fs.mkdir(BAR, (err_mk_dir_bar) => {
      assert.equal(err_mk_dir_bar, null, 'no errors');
      client.command(['watch', bar], function (error, resp) {
        assert.equal('unable to resolve root ' + bar
                      + ': casing problem in \"'+ bar
                      + '\"'
                      , error.message);
        client.end();
      });
    });
  });
}
