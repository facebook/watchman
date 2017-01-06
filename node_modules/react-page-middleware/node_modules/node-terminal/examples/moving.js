var terminal = require('../terminal');

terminal.write('Let\'s begin').nl();

terminal.write('Loading...  ');
var state = 0;
var loadingInterval = setInterval(function() {
    terminal.left(1).write(['|', '/', '-', '\\'].splice(state, 1).pop());
    
    if (++state >= 4) {
        state = 0;
    }
}, 500);

setTimeout(function() {
    clearInterval(loadingInterval);
    terminal.left(1).write('done').nl();
}, 10000);