(function () {
  "use strict";

  var oldFs = require('fs')
    , extraFs = require('fs-extra')
    , fs = {}
    ;

  Object.keys(extraFs).forEach(function (key) {
    fs[key] = extraFs[key];
  });

  Object.keys(oldFs).forEach(function (key) {
    fs[key] = oldFs[key];
  });

  fs.copy = require('./fs.copy.js');
  fs.copyRecursive = require('./fs.copy-recursive.js');

  fs.move = require('./fs.move.js');

  fs.mkdirp = require('mkdirp');
  fs.mkdirpSync = fs.mkdirp.sync;
  // Alias
  fs.mkdirRecursive = fs.mkdirp;
  fs.mkdirRecursiveSync = fs.mkdirp.sync;

  fs.remove = extraFs.remove;
  fs.removeSync = extraFs.removeSync;
  // Alias
  fs.rmrf = extraFs.remove;
  fs.rmrfSync = extraFs.removeSync;
  fs.rmRecursive = extraFs.rmrf;
  fs.rmRecursiveSync = extraFs.rmrfSync;

  fs.walk = require('walk').walk;

  module.exports = fs;
}());
