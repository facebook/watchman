/**
 * @generated
 * @jsx React.DOM
 */
var React = require("React");
var layout = require("DocsLayout");
module.exports = React.createClass({
  render: function() {
    return layout({metadata: {"id":"cmd.log","title":"log","layout":"docs","category":"Commands","permalink":"docs/cmd/log.html"}}, `
Generates a log line in the watchman log.

\`\`\`bash
$ watchman log debug "log this please"
\`\`\`
`);
  }
});
