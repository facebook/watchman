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
"use strict";

var fs = require('fs');
var path = require('path');
var url = require('url');

var Chart = require('./Chart');
var Modularizer = require('./Modularizer');
var TimingData = require('./TimingData');

var consts = require('./consts');
var renderReactPage = require('./renderReactPage');

var convertSourceMap = require('convert-source-map');


/**
 * What is a "Router" for `react-page-middleware`?
 *
 * A `react-page-middleware` `Router` object helps generate entire responses for URLs.
 * `react-page-middleware` understands dependencies between files, and
 * bundles/transforms your resources for you, but your `Router` fills in some
 * of the application specific details - such as how URLs map to a root JS
 * module, and how to render a page for a particular type of URL request.
 *
 * A `Router object must contain:
 *
 * `decideRoute` method:
 *
 * - Accepts a URL from a server request.
 * - Asynchronously determines a "route" object describing at least two things:
 * -- Path to the root JS module corresponding to the request.
 * -- The content type of the response.
 * -- The determined route may also include as many other things as your router
 *  would like.
 *
 * `routePackageHandler` method:
 *
 * - Accepts a `Package` object (automatically generated for you based on the
 *   `route` object you previously decided from `decideRoute`.
 * - Generates a page response text.
 * - Can make great use of the features in the `Package` object, including
 *   source maps etc.
 * - Can use that package to deliver a static resource to the client, or can
 *   run that package on the server to generate markup on the server.
 */


/**
 * More formally:
 * @typedef Config: Stringmap;
 *
 * @typedef RouteData {
 *  contentType: string,           // Content-type header field
 *  rootModulePath: string         // Path to "root" JS module
 *  // (whatever other fields you want: "additional props" etc.
 * };
 *
 * @typedef Router {
 *   decideRoute: (
 *     buildConfig:Config,
 *     reqURL:string,
 *     next:(err, RouteData)->void
 *   ) -> void,
 *   routePackageHandler: (
 *     buildConfig:Config,
 *     route:RouteData,
 *     rootModuleID:string,
 *     ppackage:Package,
 *     next:(err, bundleText:string)->void
 *   ) -> void
 * };
 *
 */

/**
 * The "DefaultRouter" contains some nice features worth emulating:
 *
 * - Looks at `buildConfig` for a field `sourceMapsType` (either 'inline' or
 *   'linked') that instructs page generation on how to link source maps -
 *   should they be sent in the bundle, or downloaded lazily when you open your
 *   debugger?
 * - Looks at `buildConfig.useBrowserBuiltins` to know whether or not to
 *   include common node.js functionality in the bundle even if ran in the
 *   browser.
 * - Server rendering of pages: Routes `mypage/path/to/component.html` to
 *   `mypage/path/to/component.js` and expects to find a React component there
 *   to render.
 * - Looks for "tags" in the path, which hint to include additional code in the
 *   generated bundle:
 * -- runModule: Puts `require('YourModule')` at the end of the bundle so it is
 *  ran automatically.
 * -- includeRequire: Controls whether or not to include the require runtime.
 *  If a previous bundle already defined it you know you don't have to.
 */

/**
 * Define your own custom router:
 * - Simply adhere to the above definition. If you use `react-page`, you should
 *   be able to simply provide your own router inside the `server.js` file
 *   (pass it to the `react-page-middleware` `provide` method using the object
 *   key `router`).
 */

var devBlock = function(buildConfig) {
  return '__VERSION__ = 0.44; __DEV__ = ' + (buildConfig.dev ? ' true;\n' : 'false;\n');
};

var keyMirror = function(obj) {
  var ret = {};
  var key;
  for (key in obj) {
    if (!obj.hasOwnProperty(key)) {
      continue;
    }
    ret[key] = key;
  }
  return ret;
};

var JS_TYPE = 'application/javascript; charset=utf-8';
var MAPS_TYPE = 'text; charset=utf-8';
var HTML_TYPE = 'text/html; charset=utf-8';

/**
 * Bundle the require implementation
 */
var REQUIRE_RUNTIME_PATH = path.resolve(__dirname, '..', 'polyfill/require.js');
var PROCESS_RUNTIME_PATH = path.resolve(
  __dirname,
  '..',
  'node_modules',
  'browser-builtins',
  'builtin',
  'process.js'
);
var REQUIRE_RUNTIME = fs.readFileSync(REQUIRE_RUNTIME_PATH, 'utf8');
var PROCESS_RUNTIME =
  Modularizer.modularize('process', fs.readFileSync(PROCESS_RUNTIME_PATH, 'utf8'));

var RouteTypes = keyMirror({
  fullPageRender: null,  // requests for `.html` files corresponding to components.
  jsBundle: null,        // requests for `.bundle` files corresponding to components.
  jsMaps: null           // requsts for `.map` files corresponding to components.
});

/**
 * The default router uses the `buildConfig.pageRouteRoot` as a way to prefix
 * all component lookups. For example, in the example `server.js`, the
 * `pageRouteRoot` is set to the `src/pages` directory, so
 * http://localhost:8080/index.html => src/pages/index.js
 * http://localhost:8080/about/index.html => src/pages/index.js
 *
 * The same convention is applied to bundle paths, since each page has exactly
 * one bundle. Each generated HTML page automatically pulls in its JS bundle
 * from the client (you don't worry about this).
 *
 * http://localhost:8080/index.bundle => src/pages/index.js(bundled)
 * http://localhost:8080/about/index.bundle => src/pages/index.js(bundled)
 *
 * If no `.bundle` or `.html` is found at the end of the URL, the default router
 * will append `index.html` to the end, before performing the routing convention
 * listed above.
 *
 * So http://localhost:8080/some/path
 * normalized => http://localhost:8080/some/path/index.html
 * rendered   => http://localhost:8080/some/path/index.js
 * bundled   => http://localhost:8080/some/path/index.bundle
 *
 * @return Route data or null if none applicable.
 */
var _getDefaultRouteData = function(buildConfig, reqURL) {
  var reqPath = url.parse(reqURL).pathname;
  var hasExtension = consts.HAS_EXT_RE.test(reqPath);
  var endsInHTML = consts.PAGE_EXT_RE.test(reqPath);
  var endsInBundle = consts.BUNDLE_EXT_RE.test(reqPath);
  var endsInMap = consts.MAP_EXT_RE.test(reqPath);
  var routeType = endsInHTML || !hasExtension ? RouteTypes.fullPageRender :
    endsInBundle ? RouteTypes.jsBundle :
    endsInMap ? RouteTypes.jsMaps : null;

  if (!routeType || reqPath.indexOf('..') !== -1) {
    return null;
  }

  var contentType = routeType === RouteTypes.fullPageRender ? HTML_TYPE :
    routeType === RouteTypes.jsBundle ? JS_TYPE :
    routeType === RouteTypes.jsMaps ? MAPS_TYPE : null;

  // Normalize localhost/myPage to localhost/myPage/index.html
  var indexNormalizedRequestPath =
    !hasExtension ? path.join(reqPath, '/index.html') : reqPath;

  var rootModulePath = path.join(
      // .html => .js, .bundle => js, .map => .js
      indexNormalizedRequestPath.replace(consts.ALL_TAGS_AND_EXT_RE, consts.JS_SRC_EXT)
        .replace(consts.BUNDLE_EXT_RE, consts.JS_SRC_EXT)
        .replace(consts.MAP_EXT_RE, consts.JS_SRC_EXT)
        .replace(consts.LEADING_SLASH_RE, '')
    );

  return {
    /**
     * The only "first class" routing fields that are expected to be returned
     * by all routers.
     */
    contentType: contentType,
    rootModulePath: rootModulePath,

    /**
     * The remaining fields are anticipated by `DefaultRouter`'s particular
     * `routePackageHandler`.
     */
    type: routeType,
    indexNormalizedRequestPath: indexNormalizedRequestPath,
    bundleTags: getBundleTagsForRequestPath(indexNormalizedRequestPath, routeType),
    additionalProps: {requestParams: url.parse(reqURL, true).query}
  };
};

var getBundleTagsForRequestPath = function(indexNormalizedRequestPath, routeType) {
  if (routeType === RouteTypes.fullPageRender) {
    // Make sure we include the same bundle tags when server rendering as
    // what will be downloaded after the initial page load.
    return renderReactPage.bundleTagsForFullPage();
  } else {
    var allTagsAndExtensionsMatch =
      indexNormalizedRequestPath.match(consts.ALL_TAGS_AND_EXT_RE);
    if (!allTagsAndExtensionsMatch) {
      return [];
    }
    var tagsAndExtension = allTagsAndExtensionsMatch && allTagsAndExtensionsMatch[1];
    var tagsAndExtensionSplit = tagsAndExtension && tagsAndExtension.split('.');
    return tagsAndExtensionSplit &&
      tagsAndExtensionSplit.slice(0, tagsAndExtensionSplit.length - 1);
  }
};


/**
 * @param {object} buildConfig Options for building.
 * @param {RouteData} route RouteData specifying root module etc.
 * @param {string} Root module ID: we use this with the `runModule` tag.
 * @param {Package} ppackage Appends module system etc.
 */
var preparePackage = function(buildConfig, route, rootModuleID, ppackage) {
  var devStr = devBlock(buildConfig);
  var processCode = buildConfig.useBrowserBuiltins ?
    (PROCESS_RUNTIME + '\nprocess = require("process");') : '\nprocess = {env:{}};';
  processCode +=
    '\nprocess.env.NODE_ENV = "' + (buildConfig.dev ? 'development' : 'production') + '";\n';
  ppackage.unshift(PROCESS_RUNTIME_PATH, processCode, processCode);
  if (route.bundleTags.indexOf(consts.INCLUDE_REQUIRE_TAG) !== -1) {
    ppackage.unshift(REQUIRE_RUNTIME_PATH, REQUIRE_RUNTIME, REQUIRE_RUNTIME);
  }
  ppackage.unshift('/dynamically-generated.js', devStr, devStr);
  if (route.bundleTags.indexOf(consts.RUN_MODULE_TAG) !== -1) {
    var moduleRunnerSource = "require('" + rootModuleID + "');null;";
    ppackage.push("PackageRun" + rootModuleID, moduleRunnerSource, moduleRunnerSource);
  }
};

var routePackageHandler = function(buildConfig, route, rootModuleID, ppackage, next) {
  preparePackage(buildConfig, route, rootModuleID, ppackage);
  if (route.type === RouteTypes.fullPageRender) {
    renderComponentPackage(buildConfig, route, rootModuleID, ppackage, next);
    TimingData.data.serveEnd = Date.now();
    if (buildConfig.logTiming) {
      Chart.logPageServeTime(TimingData.data);
    }
  } else if (route.type === RouteTypes.jsBundle) {
    var computedBundle = computeJSBundle(buildConfig, route, ppackage);
    next(null, computedBundle);
    TimingData.data.serveEnd = Date.now();
    if (buildConfig.logTiming) {
      Chart.logBundleServeTime(TimingData.data);
    }
  } else if (route.type === RouteTypes.jsMaps) {
    next(null, computeJSBundleMapsFile(buildConfig, route, ppackage));
    TimingData.data.serveEnd = Date.now();
    if (buildConfig.logTiming) {
      Chart.logBundleServeTime(TimingData.data);
    }
  } else {
    next(new Error('unrecognized route'));
  }
};

var decideRoute = function(buildConfig, reqURL, next) {
  if (!buildConfig.pageRouteRoot) {
    return next(new Error('Must specify default router root'));
  } else {
    try {
      var routerData =  _getDefaultRouteData(buildConfig, reqURL);
      return next(null, routerData);
    } catch (e) {
      return next(e, null);
    }
  }
};

/**
 * Handles generating "an entire page" of markup for a given route and
 * corresponding `Package`.
 *
 * - First, generates the markup and renders it server side. Will never generate
 *   source maps, unless an error occurs - then they are generated lazily.
 * - That markup is sent, then another request is made for the JS. If
 *   `useSourceMaps` is true in the `buildConfig`, then these will be generated
 *   at this point - it does not block initial page render since server
 *   rendering happens without them.
 *
 * @param {object} buildConfig Build config options.
 * @param {Route} route Contains information about the component to render
 * based on URL.
 * @param {String} rootModuleID, id of the root module for given route.
 * @param {Package} Contains information about all dependencies for given route.
 * @param {function} next When complete.
 */
var renderComponentPackage = function(buildConfig, route, rootModuleID, ppackage, next) {
  var jsBundleText = computeJSBundle(buildConfig, route, ppackage);
  var props =  route.additionalProps || {};
  renderReactPage({
    serverRender: buildConfig.serverRender,
    rootModulePath: route.rootModulePath,
    rootModuleID: rootModuleID,
    props: props,
    bundleText: jsBundleText,
    ppackage: ppackage,
    static: buildConfig['static'],
    /**
     * @param {Error} err Error that occured.
     * @param {string} markup Markup result.
     */
    done: function(err, markup) {
      TimingData.data.markupEnd = Date.now();
      next(err, markup);
      if (buildConfig.logTiming) {
        Chart.logSummary(route.indexNormalizedRequestPath, ppackage.resourceCount());
      }
    }
  });
};

var computeJSBundle = function(buildConfig, route, ppackage) {
  var useSourceMaps = buildConfig.useSourceMaps;
  var sourceMapsType = buildConfig.sourceMapsType;
  var inlineSourceMaps = sourceMapsType === 'inline';
  var footerContent = null;
  var packageText = ppackage.getSealedText(inlineSourceMaps);
  TimingData.data.concatEnd = Date.now();
  if (useSourceMaps) {
    footerContent = inlineSourceMaps ?
      // Inline them in a comment
      convertSourceMap.fromObject(ppackage.getSealedSourceMaps().toJSON()).toComment() :
      // Link them by replacing only the extension:
      // path/File.tag.bundle -> path/File.tag.map
      '\/\/@ sourceMappingURL=' + route.indexNormalizedRequestPath.replace(
        consts.BUNDLE_EXT_RE,
        consts.MAP_EXT
      );
  }
  TimingData.data.sourceMapEnd = Date.now();
  return (footerContent ? packageText + '\n' + footerContent : packageText) + ' ';
};

var computeJSBundleMapsFile = function(buildConfig, route, ppackage) {
  TimingData.data.concatEnd = Date.now();
  var bundleText = ppackage.getSealedSourceMaps().toString();
  TimingData.data.sourceMapEnd = Date.now();
  return bundleText;
};

var DefaultRouter = {
  decideRoute: decideRoute,
  routePackageHandler: routePackageHandler
};

module.exports = DefaultRouter;
