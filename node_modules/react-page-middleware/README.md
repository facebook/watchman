- - -

**_This project is not actively maintained. Proceed at your own risk!_**

- - -

react-page-middleware
===============================================
Middleware for building full page apps using React, JSX, and CommonJS.

If you want to get started with server rendered React apps, go directly to
[react-page](http://www.github.com/facebook/react-page/). This project is the
  implementation of the router/server-side-page-assembler/packager.


###Features

  - Server-side JavaScript rendering of pages/apps using React.
  - Pages rendered on server, seamlessly brought to life in the browser.
  - No special glue code to write - "Just works" on client/server.
  - CommonJS + React + optional JSX.

<br>

###Requirements

    - node (a more recent version)
    - npm

###Install

> Let npm do all the installing - just create a directory structure anywhere as
> follows:

    yourProject/
     ├── package.json              # Add npm dependencies here.
     ├── server.js                 # Start web server with `node server.js`
     └── src                       # All your application JS.
         ├── index.js              # localhost:8080/index.html routed here
         └── pages                 # Configure the page root using pageRouteRoot
             └── about.js          # localhost:8080/about.html

> List your dependencies in `package.json`:

    // Shows how to depend on bleeding edge versions. One niceness of
    // `react-page-middleware`, is depending on the main React repo as
    // `require('React')` Not all JS packagers understand the pure git repo for
    // React.
    "dependencies": {
      "React": "git://github.com/facebook/react.git",
      "react-page-middleware": "git://github.com/facebook/react-page-middleware.git",
      "connect": "2.8.3"
    },

> Download your project's dependencies:

    cd yourProject
    npm install


> Create a `server.js` file that requires `react-page-middleware`, and set the
> proper directory search paths and routing paths.

    var reactMiddleware = require('react-page-middleware');
    var REACT_LOCATION = __dirname + '/node_modules/react-tools/src';
    var ROOT_DIR = __dirname;
    var app = connect()
      .use(reactMiddleware.provide({
        logTiming: true,
        pageRouteRoot: ROOT_DIR,           // URLs based in this directory
        useSourceMaps: true,                // Generate client source maps.
        projectRoot: ROOT_DIR,          // Search for sources from
        ignorePaths: function(p) {          // Additional filtering
          return p.indexOf('__tests__') !== -1;
        }
      }))
      .use(connect['static'](__dirname + '/src/static_files'));
    http.createServer(app).listen(8080);


> Run the server and open index.html:


   node server
   open http://localhost:8080/index.html


> The [react-page](http://www.github.com/facebook/react-page/) project has a
> much more thorough explanation of the motivation and features.


### JavaScript-centric Routing And Page Rendering For JavaScript.

The default router is JavaScript-centric. You simply specify the path to the JS
component you want to use to render the entire page.
[react-page](http://www.github.com/facebook/react-page/) for more information
about the routing.

### Source Maps

`react-page-middleware` has them.


### Run and Build on the Fly

>  Just hit your browser's refresh button to run an always-up-to-date version of
>  your app.

- Dynamically packages/compiles your app on each server request.

### Purpose

`react-page-middleware` is a rapid development environment where you can experiment with
entirely new ways of building production web apps powered by React. It provides
a common environment that allows sharing of modules client/server architecture
prototypes.

In order to use this technology in a production environment, you would need to
audit and verify that the server rendering strategy is safe and suitable for
your purposes.

- In particular, you would want to ensure that a proper server
sandbox is enforced. However, `react-page` _does_ run your UI rendering code
inside of contextify as a preliminary sandbox.

- The packaging/transforming features of `react-page` would not be needed in a
production environment where the packages can be prebuilt once, stored in a CDN
and not be repackaged on the fly, but the server rendering feature is very
compelling for production environments where page load performance is of great
concern.

- Among other things, additional connect middleware should be added to prevent
stack traces from showing up in the client.
