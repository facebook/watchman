# BSER Binary Protocol

For more details about the protocol see [Watchman's docs](https://facebook.github.io/watchman/docs/bser.html).

## API

```js
var bser = require('bser');
```

### Buffer

#### Create

```js
var bunser = new bser.BunserBuf();
```

#### Append

```js
bunser.append(buf);
```

#### Read

```js
bunser.on('value', function(obj) {
  console.log(obj);
});
```

#### Dump

```js
bser.dumpToBuffer(obj);
```

## Example

Read BSER from socket:

```js
var bunser = new bser.BunserBuf();

bunser.on('value', function(obj) {
  console.log('data from socket', obj);		   
});

var socket = net.connect('/socket');

socket.on('data', function(buf) {
  bunser.append(buf);
});
```

Write BSER to socket:

```js
socket.write(bser.dumpToBuffer(obj));
```
