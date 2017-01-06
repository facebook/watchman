(function () {
  "use strict";

  var fs = require('fs')
    , fsCopy = require('./fs.copy')
    , fsMkdirp = require('mkdirp')
    , fsWalk = require('walk').walk
    , path = require('path')
    ;

  function run(src, dst, cb) {

    function syncDirs(cb, src, dst) {
      var walker = fsWalk(src)
        ;

      walker.on('directory', function (root, stat, next) {
        var newDir = path.join(dst, root.substr(src.length + 1), stat.name)
          ;

        fsMkdirp(newDir, stat.mode, next);
      });
      
      walker.on('end', function () {
        cb();
      });
    }

    function syncFiles(cb, src, dst) {
      var walker = fsWalk(src)
        ;

      walker.on('file', function (root, stat, next) {
        var curFile = path.join(root, stat.name)
          , newFile = path.join(dst, root.substr(src.length + 1), stat.name)
          ;

        fsCopy(curFile, newFile, function (err) {
          if (err) {
            cb(err);
            return;
          }
          next();
        });
      });
      
      walker.on('end', function () {
        cb();
      });
    }

    dst = path.resolve(process.cwd(), dst);

    fsMkdirp(path.join(dst), function () {
      fs.realpath(src, function (err, rsrc) {
        fs.realpath(dst, function (err, rdst) {
          syncDirs(function () {
            syncFiles(cb, rsrc, rdst);
          }, rsrc, rdst);
        });
      });
    });
  }

  function runCli() {
    var srcPath = process.argv[2]
      , dstPath = process.argv[3]
      , emitter
      ;

    function showHelp() {
      console.log('usage: rsync src-path dst-path');
    }

    if (!srcPath || !dstPath) {
      showHelp();
      return;
    }

    emitter = run(srcPath, dstPath, function (err) {
      if (err) {
        console.error(err);
      }
      console.log('All Done');
    });

    // TODO
    //emitter.on('fileCopied');
  }

  if (require.main === module) {
    runCli();
  }

  module.exports = run;
}());
