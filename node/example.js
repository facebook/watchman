var watchman = require('fb-watchman');
var client = new watchman.Client();

client.on('end', function() {
  // Called when the connection to watchman is terminated
  console.log('client ended');
});

client.on('error', function(error) {
  console.error('Error while talking to watchman: ', error);
});

client.command(['version'], function(error, resp) {
  if (error) {
    console.error('Error getting version:', error);
    return;
  }
  console.log('Talking to watchman version', resp.version);
});

// Example of the error case
client.command(['invalid-command-never-will-work'], function(error, resp) {
  if (error) {
    console.error('failed to subscribe: ', error);
    return;
  }
});

// Initiate a watch.  You can repeatedly ask to watch the same dir without
// error; Watchman will re-use an existing watch.
client.command(['watch', process.cwd()], function(error, resp) {
  if (error) {
    console.error('Error initiating watch:', error);
    return;
  }

  if ('warning' in resp) {
    console.log('warning: ', resp.warning);
  }

  // The default subscribe behavior is to deliver a list of all current files
  // when you first subscribe, so you don't need to walk the tree for yourself
  // on startup.  If you don't want this behavior, you should issue a `clock`
  // command and use it to give a logical time constraint on the subscription.
  // See further below for an example of this.

  // Subscribe to notifications about .js files
  // https://facebook.github.io/watchman/docs/cmd/subscribe.html
  client.command(['subscribe', process.cwd(), 'mysubscription', {
      // Match any .js file under process.cwd()
      // https://facebook.github.io/watchman/docs/file-query.html#expressions
      // Has more on the supported expression syntax
      expression: ["match", "*.js"],
      // Which fields we're interested in
      fields: ["name", "size", "exists", "mode"]
    }],
    function(error, resp) {
      if (error) {
        // Probably an error in the subscription criteria
        console.error('failed to subscribe: ', error);
        return;
      }
      console.log('subscription ' + resp.subscribe + ' established');
    }
  );

  // Subscription results are emitted via the subscription event.
  client.on('subscription', function(resp) {
    // Each entry in `resp.files` will have the fields you requested
    // in your subscription.  The default is:
    //  { name: 'example.js',
    //    size: 1680,
    //    new: true,
    //    exists: true,
    //    mode: 33188 }
    //
    // Names are relative to resp.root; join them together to
    // obtain a fully qualified path.
    //
    // `resp`  looks like this in practice:
    //
    // { root: '/private/tmp/foo',
    //   subscription: 'mysubscription',
    //   files: [ { name: 'node_modules/fb-watchman/index.js',
    //       size: 4768,
    //       exists: true,
    //       mode: 33188 } ] }
    console.log(resp.root, resp.subscription, resp.files);
  });

  // Here's an example of just subscribing for notifications after the
  // current point in time
  client.command(['clock', process.cwd()], function(error, resp) {
    if (error) {
      console.error('Failed to query clock:', error);
      return;
    }

    client.command(['subscribe', process.cwd(), 'sincesub', {
        expression: ["match", "*.js"],
        // Note: since we only request a single field, the `sincesub` subscription
        // response will just set files to an array of filenames, not an array of
        // objects with name properties
        // { root: '/private/tmp/foo',
        //   subscription: 'sincesub',
        //   files: [ 'node_modules/fb-watchman/index.js' ] }
        fields: ["name"],
        since: resp.clock // time constraint
      }],
      function(error, resp) {
        if (error) {
          console.error('failed to subscribe: ', error);
          return;
        }
        console.log('subscription ' + resp.subscribe + ' established');
      }
    );
  });
});
