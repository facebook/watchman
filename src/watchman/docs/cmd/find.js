/**
 * @generated
 * @jsx React.DOM
 */
var React = require("React");
var layout = require("DocsLayout");
module.exports = React.createClass({
  render: function() {
    return layout({metadata: {"id":"cmd.find","title":"find","layout":"docs","category":"Commands","permalink":"docs/cmd/find.html"}}, `
Finds all files that match the optional list of patterns under the
specified dir.  If no patterns were specified, all files are returned.

\`\`\`bash
$ watchman find /path/to/dir [patterns]
\`\`\`
`);
  }
});
