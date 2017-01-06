(function () {
  "use strict";

  var fs = require('./fs.extra')
    , sequence = require('sequence').create()
    , exec = require('child_process').exec
    , count = 0
    ;

  function fsop(op, a, b, shouldpass) {
    var c = count;
    count += 1;
    return function (n) {
      function log(err) {
        if (shouldpass && !err) {
          console.log(c, op, a, b, 'passed');
          n();
        } else if (!shouldpass && err) {
          console.log(c, op, a, b, 'passed');
          n();
        } else {
          console.log(c, op, a, b, 'failed');
        }
      }

      fs[op](
          'file.' + a + '.t.txt'
        , 'file.' + b + '.t.txt'
        , log
      );
    };
  }

  sequence
    .then(function (next) {
      exec('rm ./file.*.t.txt; touch file.0.t.txt', next);
    })                               // 0
    .then(fsop('copy', 0, 1, true))  // 0, 1
    .then(fsop('copy', 0, 2, true))  // 0, 1, 2
    .then(fsop('copy', 0, 1, false)) // 0, 1, 2
    .then(fsop('copy', 3, 4, false)) // 0, 1, 2
    .then(fsop('move', 0, 3, true))  //    1, 2, 3
    .then(fsop('move', 0, 4, false)) //    1, 2, 3
    .then(fsop('move', 4, 0, false)) //    1, 2, 3
    .then(fsop('move', 5, 6, false)) //    1, 2, 3
    .then(fsop('move', 3, 0, true))  // 0, 1, 2
    .then(fsop('move', 2, 0, false)) // 0, 1, 2
    .then(function () {
      console.log('All Done.');
    });
}());
