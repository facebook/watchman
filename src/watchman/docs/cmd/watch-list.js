/**
 * @generated
 * @jsx React.DOM
 */
var React = require("React");
var layout = require("DocsLayout");
module.exports = React.createClass({
  render: function() {
    return layout({metadata: {"id":"cmd.watch-list","title":"watch-list","layout":"docs","category":"Commands","permalink":"docs/cmd/watch-list.html"}}, `
Returns a list of watched dirs.

From the command line:

\`\`\`bash
$ watchman watch-list
\`\`\`

JSON:

\`\`\`json
["watch-list"]
\`\`\`

Result:

\`\`\`json
{
    "version": "1.9",
    "roots": [
        "/home/wez/watchman"
    ]
}
\`\`\`
`);
  }
});
