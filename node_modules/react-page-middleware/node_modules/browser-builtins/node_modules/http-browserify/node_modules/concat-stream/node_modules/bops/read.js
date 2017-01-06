var proto = {}
  , rex = /read.+/
  , fn

fn = function() {

}

module.exports = proto

for(var key in Buffer.prototype) {
  if(rex.test(key)) {
    proto[key] = fn.call.bind(Buffer.prototype[key])
  }
}
