var terminal = require('../terminal');

// Pink Unicorn
terminal.colorize('%pUnicorn\n');

// White unicor
terminal.color('white').write('Unicorn\n');

// Rainbows
// As a single line
terminal.colorize('%rR%ma%ci%bn%yb%go%rw\n');

// Multiple commands
terminal.color('red').write('R').color('magenta').write('a').color('cyan').write('i').color('blue').write('n').color('yellow').write('b').color('green').write('o').color('red').write('w').write('\n');

// With background colors
terminal.colorize('%w%1  R  %2  A  %3  I  %4  N  %5  B  %6  O  %7  W  %n ');