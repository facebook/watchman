fs.extra
===

Extra file utilities for node.js

**Includes**

* `copy`
* `copyRecursive`
* `mkdirp`
* `move`
* `walk`
* `rmrf`

**Install**

``` bash
npm install --save fs.extra
````

**Usage**

```javascript
// this will have all of a copy of the normal fs methods as well
var fs = require('fs.extra');
```

fs.copy
===

Creates an `fs.readStream` and `fs.writeStream` and uses `util.pump` to efficiently copy.

```javascript
fs.copy('foo.txt', 'bar.txt', { replace: false }, function (err) {
  if (err) {
    // i.e. file already exists or can't write to directory
    throw err;
  }

  console.log("Copied 'foo.txt' to 'bar.txt');
});
```

Options are optional. `replace` defaults to false, but will replace existing files if set to `true`.

fs.copyRecursive
===

Basically a local `rsync`, uses `fs.copy` to recursively copy files and folders (with correct permissions).

```javascript
fs.copyRecursive('./foo', './bar', function (err) {
  if (err) {
    throw err;
  }

  console.log("Copied './foo' to './bar');
});
```

fs.mkdirRecursive
===

Included from <https://github.com/substack/node-mkdirp>

```javascript
// fs.mkdirp(path, mode=(0777 & (~process.umask())), cb);

fs.mkdirp('/tmp/foo/bar/baz', function (err) {
  if (err) {
    console.error(err);
  } else {
    console.log('pow!')
  }
});
```

fs.mkdirRecursiveSync
===

Included from <https://github.com/substack/node-mkdirp>

```javascript
// fs.mkdirpSync(path, mode=(0777 & (~process.umask())));

try {
  fs.mkdirpSync('/tmp/foo/bar/baz');
} catch(e) {
  throw e;
}
```

fs.move
===

Attempts `fs.rename`, then tries `fs.copy` + `fs.unlink` before failing.

```javascript
fs.move('foo.txt', 'bar.txt', function (err) {
  if (err) {
    throw err;
  }

  console.log("Moved 'foo.txt' to 'bar.txt');
});
```

fs.rmRecursive
===

Included from <https://github.com/jprichardson/node-fs-extra>

Recursively deletes a directory (like `rm -rf`)

```javascript
// fs.rmrf(dir, callback);

fs.rmrf('/choose/me/carefully/', function (err) {
  if (err) {
    console.error(err);
  }
});
```

fs.rmRecursiveSync
===

Included from <https://github.com/jprichardson/node-fs-extra>

Recursively deletes a directory (like `rm -rf`)

```javascript
// fs.rmrfSync(dir);

fs.rmrfSync('/choose/me/carefully/');
```

fs.walk
===

See <https://github.com/coolaj86/node-walk>

```javascript
var walker = fs.walk(dir)
  ;

// file, files, directory, directories
walker.on("file", function (root, stat, next) {
  var filepath = path.join(root, stat.name)
    ;

  console.log(filepath);
});
```

Aliases and Backwards Compatibility
===

For the sake of backwards compatability, you can call the recursive functions with their names as such

    fs.remove <- fs.rmRecursive <- fs.rmrf
    fs.removeSync <- fs.rmRecursiveSync <- fs.rmrfSync
    fs.mkdirRecursive <- fs.mkdirp
    fs.mkdirRecursiveSync <- fs.mkdirpSync

License
===

Copyright AJ ONeal 2011-2015

This project is available under the MIT and Apache v2 licenses.

  * http://www.opensource.org/licenses/mit-license.php
  * http://www.apache.org/licenses/LICENSE-2.0.html
