(function () {
  "use strict";

  var fs = require('fs')
    ;

  function noop() {}

  function copy(src, dst, opts, cb) {
    if ('function' === typeof opts) {
      cb = opts;
      opts = null;
    }
    opts = opts || {};

    function copyHelper(err) {
      var is
        , os
        ;

      if (!err && !(opts.replace || opts.overwrite)) {
        return cb(new Error("File " + dst + " exists."));
      }

      fs.stat(src, function (err, stat) {
        if (err) {
          return cb(err);
        }

        is = fs.createReadStream(src);
        os = fs.createWriteStream(dst);

        is.pipe(os);
        os.on('close', function (err) {
          if (err) {
            return cb(err);
          }

          fs.utimes(dst, stat.atime, stat.mtime, cb);
        });
      });
    }

    cb = cb || noop;
    fs.stat(dst, copyHelper);
  }

  module.exports = copy;
}());
