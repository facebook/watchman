# fb-watchman

`fb-watchman` is a filesystem watcher that uses the
[Watchman](https://facebook.github.io/watchman/) file watching service from
Facebook.

Watchman provides file change notification services using very
efficient recursive watches and also allows more advanced change matching and
filesystem tree querying operations using
[a powerful expression syntax](https://facebook.github.io/watchman/docs/file-query.html#expressions).

## Install

You should [install Watchman](https://facebook.github.io/watchman/docs/install.html) to make the most of this module.

Then simply:

```
$ npm install fb-watchman
```

## Key Concepts

- Watchman recursively watches directories.
- Each watched directory is called a `root`.
- You must initiate a `watch` on a `root` using the `watch-project` command prior to subscribing to changes
- Rather than separately watching many sibling directories, `watch-project` consolidates and re-uses existing watches relative to a project root (the location of your `.watchmanconfig` or source control repository root)
- change notifications are relative to the project root

## Usage

```js
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
client.command(['watch-project', process.cwd()], function(error, resp) {
  if (error) {
    console.error('Error initiating watch:', error);
    return;
  }

  // It is considered to be best practice to show any 'warning' or 'error'
  // information to the user, as it may suggest steps for remediation
  if ('warning' in resp) {
    console.log('warning: ', resp.warning);
  }

  // The default subscribe behavior is to deliver a list of all current files
  // when you first subscribe, so you don't need to walk the tree for yourself
  // on startup.  If you don't want this behavior, you should issue a `clock`
  // command and use it to give a logical time constraint on the subscription.
  // See further below for an example of this.

  // watch-project may re-use an existing watch at a higher level in the
  // filesystem.  It will tell us the relative path to the directory that
  // we expressed interest in, so we need to adjust for it in our results
  var path_prefix = '';
  var root = resp['watch'];
  if ('relative_path' in resp) {
    path_prefix = resp['relative_path'];
    console.log('(re)using project watch at ', root, ', our dir is relative: ',
        path_prefix);
  }

  function get_relative_name(proj_rel) {
    if (path_prefix.length == 0) {
      return proj_rel;
    }
    if (proj_rel.substr(0, path_prefix.length) == path_prefix) {
      return proj_rel.substr(path_prefix.length + 1);
    }
    return null;
  }

  // Subscribe to notifications about .js files
  // https://facebook.github.io/watchman/docs/cmd/subscribe.html
  client.command(['subscribe', root, 'mysubscription', {
      // Match any .js file under process.cwd()
      // https://facebook.github.io/watchman/docs/file-query.html#expressions
      // Has more on the supported expression syntax
      expression: ["allof",
          ["match", "*.js"],
          // focus on the relative path from the project to the path
          // of interest
          ['dirname', path_prefix]
      ],
      // Which fields we're interested in
      fields: ["name", "size", "exists", "type"]
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
  // Note that this emits for all subscriptions.  If you have
  // subscriptions with different `fields` you will need to check
  // the subscription name and handle the differing data accordingly
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
    console.log(resp.root, resp.subscription);
    for (var i in resp.files) {
      var f = resp.files[i];
      // Fixup name for watch-project offset
      if (resp.subscription == 'mysubscription') {
        // we requested a set of fields in this subscription
        f.name = get_relative_name(f.name);
      } else {
        // the other subscription we set up returns only the name
        f = get_relative_name(f);
      }
      console.log(f);
    }
  });

  // Here's an example of just subscribing for notifications after the
  // current point in time
  client.command(['clock', root], function(error, resp) {
    if (error) {
      console.error('Failed to query clock:', error);
      return;
    }

    client.command(['subscribe', root, 'sincesub', {
        expression: ['allof',
          ["match", "*.js"],
          // focus on the relative path from the project to the path
          // of interest
          ['dirname', path_prefix]
        ],
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
```

## Methods

### client.command(args [, done])

Sends a command to the watchman service.  `args` is an array that specifies
the command name and any optional arguments.  The command is queued and
dispatched asynchronously.  You may queue multiple commands to the service;
they will be dispatched in FIFO order once the client connection is established.

The `done` parameter is a callback that will be passed (error, result) when the
command completes.  You may omit it if you are not interested in the result of
the command.

```js
client.command(['watch-project', process.cwd()], function(error, resp) {
  if (error) {
    console.log('watch failed: ', error);
    return;
  }
  if ('warning' in resp) {
    console.log('warning: ', resp.warning);
  }
  if ('relative_path' in resp) {
    // We will need to remember and adjust for relative_path
    console.log('watching project ', resp.watch, ' relative path to cwd is ',
      resp.relative_path);
  } else {
    console.log('watching ', resp.watch);
  }
});
```

If a field named `warning` is present in `resp`, the watchman service is trying
to communicate an issue that the user should see and address.  For example, if
the system watch resources need adjustment, watchman will provide information
about this and how to remediate the issue.  It is suggested that tools that
build on top of this library bubble the warning message up to the user.

### client.end()

Terminates the connection to the watchman service.  Does not wait
for any queued commands to send.

## Events

The following events are emitted by the watchman client object:

### Event: 'connect'

Emitted when the client successfully connects to the watchman service

### Event: 'error'

Emitted when the socket to the watchman service encounters an error.

It may also be emitted prior to establishing a connection if we are unable
to successfully execute the watchman CLI binary to determine how to talk
to the server process.

It is passed a variable that encapsulates the error.

### Event: 'end'

Emitted when the socket to the watchman service is closed

### Event: 'log'

Emitted in response to a unilateral `log` PDU from the watchman service.
To enable these, you need to send a `log-level` command to the service:

```js
// This is very verbose, you probably don't want to do this
client.command(['log-level', 'debug']);
client.on('log', function(info) {
  console.log(info);
});
```

### Event: 'subscription'

Emitted in response to a unilateral `subscription` PDU from the watchman
service.  To enable these, you need to send a `subscribe` command to the service:

```js
  // Subscribe to notifications about .js files
  client.command(['subscribe', process.cwd(), 'mysubscription', {
      expression: ["match", "*.js"]
    }],
    function(error, resp) {
      if (error) {
        // Probably an error in the subscription criteria
        console.log('failed to subscribe: ', error);
        return;
      }
      console.log('subscription ' + resp.subscribe + ' established');
    }
  );

  // Subscription results are emitted via the subscription event.
  // Note that watchman will deliver a list of all current files
  // when you first subscribe, so you don't need to walk the tree
  // for yourself on startup
  client.on('subscription', function(resp) {
    console.log(resp.root, resp.subscription, resp.files);
  });
```

To cancel a subscription, use the `unsubscribe` command and pass in the name of
the subscription you want to cancel:

```js
  client.command(['unsubscribe', process.cwd(), 'mysubscription']);
```

Note that subscriptions names are scoped to your connection to the watchman
service; multiple different clients can use the same subscription name without
fear of colliding.


