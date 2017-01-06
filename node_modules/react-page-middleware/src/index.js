/**
 * Copyright 2013 Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

var DefaultRouter = require('./DefaultRouter');
var Packager = require('./Packager');
var TimingData = require('./TimingData');

var fs = require('fs');
var guard = require('./guard');
var path = require('path');

var ES5_RUNTIME_DEPENDENCIES = [
  'es5-shim/es5-shim.js',
  'es5-shim/es5-sham.js'
];

var REACT_RUNTIME_DEPENDENCIES = [
  'React',
  'ReactMount'
];

var validateBuildConfig = function(buildConfig) {
  var middlewarePath =
    path.resolve(buildConfig.projectRoot, 'node_modules', 'react-page-middleware');
  var browserBuiltinsPath =
    path.resolve(middlewarePath, 'node_modules', 'browser-builtins');
  if (buildConfig.useBrowserBuiltins && buildConfig.blacklistRE &&
    buildConfig.blacklistRE.test(browserBuiltinsPath)) {
    throw new Error(
      'You have blacklisted the browser builtins directory (' +
      browserBuiltinsPath + ')' +
      ' but you have set `useBrowserBuiltins` to be true');
  }
  // Substring/indexOf is actually a flawed way to infer folder hierarchy!
  if (buildConfig.pageRouteRoot.indexOf(buildConfig.projectRoot) !== 0) {
    throw new Error('pageRouteRoot must be prefix of projectRoot');
  }
  if (!fs.existsSync(buildConfig.projectRoot)) {
    throw new Error('ERROR: No root:' + buildConfig.projectRoot);
  }
};

/**
 * TODO: We may need to call next here, if we want to allow something like a
 * gzip plugin.
 */
function send(type, res, str, mtime) {
  res.setHeader('Date', new Date().toUTCString());
  // Always assume we had compiled something that may have changed.
  res.setHeader('Last-Modified', mtime || (new Date()).toUTCString());
  // Would like to set the content length but it isn't clear how to do that
  // efficiently with JS strings (string length is not byte length!).
  res.setHeader('Content-Type', type);
  res.end(str);
}

exports.provide = function provide(buildConfig) {
  validateBuildConfig(buildConfig);
  /**
   * TODO: We can cache sign the module cache with a particular web request ID.
   * We generate a page with request ID x, and include a script
   * src="main.js?pageRenderedWithRequestID=x" so we know that we can somehow use
   * that old module version, saving a module cache invalidation.
   */
  return function provideImpl(req, res, next) {
    if (req.method !== 'GET') {
      return next();
    }
    var router = buildConfig.router || DefaultRouter;
    var decideRoute = router.decideRoute;
    var routePackageHandler = router.routePackageHandler;

    decideRoute(buildConfig, req.url, function(err, route) {
      TimingData.data = {pageStart: Date.now()};
      if (err || !route) {
        return next(err);
      }
      if (!route.contentType || !route.rootModulePath) {
        return next(new Error('Router must provide contentType and rootModulePath'));
      }
      var serveResult = guard(next, send.bind(null, route.contentType, res));
      var onOutputGenerated = function(err, resultText) {
        serveResult(err, resultText);
      };
      var onComputePackage = function(rootModuleID, ppackage) {
        TimingData.data.findEnd = Date.now();
        routePackageHandler(buildConfig, route, rootModuleID, ppackage, onOutputGenerated);
      };
      var packageOptions = {
        buildConfig: buildConfig,
        rootModuleAbsolutePath: path.join(buildConfig.pageRouteRoot || '', route.rootModulePath),
        runtimeDependencies: REACT_RUNTIME_DEPENDENCIES.concat(
          buildConfig.skipES5Shim ? [] : ES5_RUNTIME_DEPENDENCIES
        )
      };
      var onComputePackageGuarded = guard(next, onComputePackage);
      Packager.computePackageForAbsolutePath(packageOptions, onComputePackageGuarded);
    });
  };
};

/**
 * @param {object} buildConfig The same build config object used everywhere.
 * @return {function} Function that when invoked with the absolute path of a web
 * request, will return the response that would normally be served over the
 * network.
 *
 *   > require('react-page-middleware')
 *   >  .compute({buildConfigOptions})
 *   >    ('path/to/x.html', console.log.bind(console))
 *
 *   <  <html><body>...</body></html>
 *
 * Can also be used to compute JS bundles.
 */
exports.compute = function(buildConfig) {
  return function(requestedPath, onComputed) {
    if (!requestedPath) {
      throw new Error('Must supply file to compute build from:');
    }
    var mockRequestedURL = 'http://localhost:8080/' + requestedPath;
    var noop = function() {};
    exports.provide(buildConfig)(
      {url: mockRequestedURL, method: 'GET'}, // req
      {end: onComputed, setHeader: noop},     // res
      function(err) {                         // next()
        console.log('ERROR computing build:', err);
      }
    );
  };
};
