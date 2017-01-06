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

var nodeTerminal = require('node-terminal');


/**
 * Super hacky charing utilities to report timing data.
 */

var YELLOW = '%k%3';
var GREEN = '%k%2';
var RED = '%k%1';
var LOG_WIDTH = 130;

var renderChart = function(title, blocks, datas) {
  // Remove spaces from available blocks, and titles.
  var totalColorBlocks = blocks - (datas.length - 1) - title.length;
  var sum = 0;
  for (var d = 0; d < datas.length; d++) {
    sum += datas[d].value;
  }
  var totalColorBlocksRendered = 0;
  nodeTerminal.write(title);
  for (d = 0; d < datas.length; d++) {
    var val = datas[d].value;
    var pct = val / sum;
    var blocksToRender = Math.floor(pct * totalColorBlocks);
    var textLen = Math.min(datas[d].text.length, blocksToRender);
    var spacesToRender = blocksToRender - textLen;
    var color = val < datas[d].bad/2 ? GREEN :
      val < datas[d].bad ? YELLOW : RED;
    nodeTerminal.colorize(color).colorize(
      datas[d].text.substr(0, textLen)
    );
    for (var i = 0; i < spacesToRender; i++) {
      nodeTerminal.write(' ');
    }
    totalColorBlocksRendered += blocksToRender;
    if (d === datas.length - 1) {
      var padding = Math.max(totalColorBlocks - totalColorBlocksRendered, 0);
      for (var ii = 0; ii < padding; ii++) {
        nodeTerminal.write(' ');
      }
    }
    nodeTerminal.reset().write(' ');
  }
  nodeTerminal.reset().write('\n\n');
};

var firstBundle = false;
var badFind = function() {
  return !firstBundle ? 1100 : 100;
};
var findMsg = function(time) {
  var timeText = 'find/transform ' + time + 'ms';
  var disclaimer = !firstBundle ? ' (WARMING UP ON FIRST LOAD)' : '';
  return timeText + disclaimer;
};

var getChartColumns = function() {
  return typeof process.stdout.getWindowSize == 'function' &&
    process.stdout.getWindowSize()[0] - 2 || LOG_WIDTH;
};
var logPageServeTime = function(timingData) {
  var pageStart = timingData.pageStart;
  var findEnd = timingData.findEnd;
  var concatEnd = timingData.concatEnd;
  var markupEnd = timingData.markupEnd;
  var serveEnd = timingData.serveEnd;

  var columns = getChartColumns();
  var findTime = findEnd - pageStart;
  var concatTime = concatEnd - findEnd;
  var renderingTime = markupEnd - findEnd;
  var serveTime = serveEnd - markupEnd;
  Chart.renderChart('HTML Gen: ', columns, [
    {text: findMsg(findTime), value: findTime, bad: badFind()},
    // Concat time for HTML serving is very fast because we don't need to
    // compute source maps - they are computed lazily upon errors for stack traces.
    // JS bundles compute them at the same time as concatenation happens.
    {text: 'concat JS ' + concatTime + 'ms', value: Math.max(concatTime, 7), bad: 40},
    {text: 'render ' + renderingTime + 'ms', value: renderingTime, bad: 170},
    // Inflate the serve time to make a nicer chart
    {text: 'serve ' + serveTime + 'ms', value: serveTime + 15, bad: 20 + 15}
  ]);
  firstBundle = true;
};

var logBundleServeTime = function(timingData) {
  var pageStart = timingData.pageStart;
  var findEnd = timingData.findEnd;
  var concatEnd = timingData.concatEnd;
  var sourceMapEnd = timingData.sourceMapEnd;
  var serveEnd = timingData.serveEnd;

  var columns = getChartColumns();
  var findTime = findEnd - pageStart;
  var concatTime = concatEnd - findEnd;
  var sourceMapTime = sourceMapEnd - concatEnd;
  var serveTime = serveEnd - sourceMapEnd;
  var datas = [
    {text: findMsg(findTime), value: findTime, bad: badFind()},
    { text: (sourceMapTime ?  'concat JS and build srcmap ' : 'concat JS ') +
        concatTime + 'ms',
      value: concatTime,
      bad: sourceMapTime ? 100 : 10
    }
  ];
  if (sourceMapTime && sourceMapTime > 2) {
    datas.push({
      text: 'serialize srcmap ' + sourceMapTime + 'ms',
      value: sourceMapTime,
      bad: 210
    });
  }
  datas.push({text: 'serve ' + serveTime + 'ms', value: serveTime + 10, bad: 40});
  Chart.renderChart('  JS Gen: ', columns, datas);
  firstBundle = true;
};

var times = function(s, n) {
  var res = '';
  for (var i = 0; i < n; i++) {
    res += s;
  }
  return res;
};

/**
 * The bytes are an approximation - in JS, str.length and bytes are not equal.
 */
var logSummary = function(normalizedRequestPath, numModules) {
  var columns = getChartColumns();
  var msg =
    'Bundling ' + numModules + ' JS files for ' + normalizedRequestPath + '   -   ' +
    'HTML blocks UI, JS Gen does not';
  var padL = Math.floor((columns - msg.length) / 2);
  var padR = padL * 2 < columns ? padL : padL;
  var formattedMsg = times(' ', padL) + msg + times(' ', padR);
  nodeTerminal.reset().write('\n\n\n').write(formattedMsg).write('\n');
  nodeTerminal.write(times('-', columns)).write('\n');
};


var Chart = {
  renderChart: renderChart,
  logPageServeTime: logPageServeTime,
  logBundleServeTime: logBundleServeTime,
  logSummary: logSummary
};

module.exports = Chart;
