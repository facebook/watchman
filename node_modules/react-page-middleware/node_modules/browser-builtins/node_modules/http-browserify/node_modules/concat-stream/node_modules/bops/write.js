var Buffer = require('buffer').Buffer

var proto = {}
  , rex = /write.+/
  , fn

fn = function() {

}

module.exports = proto

for(var key in Buffer.prototype) {
  if(rex.test(key)) {
    proto[key] = fn.call.bind(Buffer.prototype[key])
  }
}
