var assert = require('assert');
var watchman = require('../../node');
var client = new watchman.Client();
const fs = require('fs');
const os = require('os');
const path = require('path');

var platform = os.platform();
if (platform == 'darwin') {
  var tmp = fs.realpathSync(process.env.TMPDIR);
  var foo = path.join(tmp, 'foo');
  var FOO = path.join(tmp, 'FOO');

  fs.mkdir(FOO, function(err_mk_dir_foo) {
    assert.equal(err_mk_dir_foo, null, 'no errors');
    var bar = path.join(foo, 'bar');
    var BAR = path.join(FOO, 'bar');

    fs.mkdir(BAR, function(err_mk_dir_bar) {
      assert.equal(err_mk_dir_bar, null, 'no errors');

      client.command(['watch', bar], function (error, resp) {
        assert.equal('unable to resolve root ' + bar
                      + ": \"" + bar + "\" resolved to \"" + BAR
                      + "\" but we were unable to examine \""
                      + bar + "\" using strict "
                      + "case sensitive rules.  Please check each component of the path and make "
                      + "sure that that path exactly matches the correct case of the files on your "
                      + "filesystem."
                      , error.message);
        client.end();
      });
    });
  });
}
