/**
 * @generated
 * @jsx React.DOM
 */
var React = require("React");
var layout = require("DocsLayout");
module.exports = React.createClass({
  render: function() {
    return layout({metadata: {"id":"cmd.shutdown-server","title":"shutdown-server","layout":"docs","category":"Commands","permalink":"docs/cmd/shutdown-server.html"}}, `
This command causes your watchman service to exit with a normal status code.

\`\`\`bash
$ watchman shutdown-server
\`\`\`
`);
  }
});
