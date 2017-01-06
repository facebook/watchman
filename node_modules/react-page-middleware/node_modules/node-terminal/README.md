# Terminal

Terminal is a small package that can be used with [node.js](http://nodejs.org) to control terminal output. The package can move the cursor in the terminal and output colored text. It can colorize a message with a simple straightforward markup syntax.

# Examples

Some examples from the `examples` directory. 

## Colors

Control colored output. See also `examples/colors.js`. Colors may vary depending on the terminal settings.

Simple color changing:
    
    terminal.color('magenta').write('Unicorn');

This will output `Unicorn` in magenta (or purple). To change the background color to magenta:

    terminal.color('magenta', 'background').write('Unicorn');

### Color formatting

`Terminal` supports formatting strings with colors using a simple syntax. Outputting `Unicorn` in magenta (like the example above) would look like this:

    terminal.colorize('%mUnicorn');
    
    // And changing the background color to magenta
    terminal.colorize('%5Unicorn');

Using this syntax we can create `Rainbows` easily in one line:

    terminal.colorize('%rR%ma%ci%bn%yb%go%rw\n');
    
    // Or with background colors
    terminal.colorize('%w%1  R  %2  A  %3  I  %4  N  %5  B  %6  O  %7  W  %n ');

The colorize function accepts the following modifiers:

                      text      text            background
          ------------------------------------------------
          %k %K %0    black     dark grey       black
          %r %R %1    red       bold red        red
          %g %G %2    green     bold green      green
          %y %Y %3    yellow    bold yellow     yellow
          %b %B %4    blue      bold blue       blue
          %m %M %5    magenta   bold magenta    magenta
          %p %P       magenta (think: purple)
          %c %C %6    cyan      bold cyan       cyan
          %w %W %7    white     bold white      white
    
          %F     Blinking, Flashing
          %U     Underline
          %8     Reverse
          %_,%9  Bold
    
          %n,%N  Resets the color
          %%     A single %

Colored ouput can be reset with the `reset` function:

    terminal.color('red').write('This is red,').reset().write(' and this is not');
# License

Terminal is licensed under The MIT License

