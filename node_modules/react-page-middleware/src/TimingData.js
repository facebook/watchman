/**
 * @providesModule TimingData
 */

/**
 * Simple global timing data prevents having to pass around data as arguments.
 * This means if multiple, parallel requests are coming in simultaneously, then
 * results are not accurate (TODO: Fix this).
 */
var TimingData = {
  data: {}
};

module.exports = TimingData;
