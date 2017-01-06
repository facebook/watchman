/**
 * @generated
 * @jsx React.DOM
 */
var React = require("React");
var layout = require("DocsLayout");
module.exports = React.createClass({
  render: function() {
    return layout({metadata: {"id":"cmd.trigger-list","title":"trigger-list","layout":"docs","category":"Commands","permalink":"docs/cmd/trigger-list.html"}}, `
Returns the set of registered triggers associated with a root directory.

\`\`\`bash
$ watchman trigger-list /root
\`\`\`

Note that the format of the output from \`trigger-list\` changed in Watchman
version 2.9.7.  It will now output a list of trigger objects as defined
by the \`trigger\` command.
`);
  }
});
