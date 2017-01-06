var spawn = require('child_process').spawn
var concat = require('./')

// real world example
var cmd = spawn('ls')
cmd.stdout.pipe(
  concat(function(out) {
    console.log('`ls`', out.toString())
  })
)

// array stream
var arrays = concat(function(out) {
  console.log('arrays', out)
})
arrays.write([1,2,3])
arrays.write([4,5,6])
arrays.end()

// buffer stream
var buffers = concat(function(out) {
  console.log('buffers', out.toString())
})
buffers.write(new Buffer('pizza Array is not a ', 'utf8'))
buffers.write(new Buffer('stringy cat'))
buffers.end()

// string stream
var strings = concat(function(out) {
  console.log('strings', out)
})
strings.write("nacho ")
strings.write("dogs")
strings.end()
