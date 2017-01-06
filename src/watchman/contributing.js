/**
 * @generated
 * @jsx React.DOM
 */
var React = require("React");
var layout = require("DocsLayout");
module.exports = React.createClass({
  render: function() {
    return layout({metadata: {"id":"contributing","title":"Contributing","layout":"docs","permalink":"contributing.html","category":"Internals"}}, `
If you're thinking of hacking on watchman we'd love to hear from you!
Feel free to use the GitHub issue tracker and pull requests discuss and
submit code changes.

We (Facebook) have to ask for a "Contributor License Agreement" from someone
who sends in a patch or code that we want to include in the codebase.  This is
a legal requirement; a similar situation applies to Apache and other ASF
projects.

If we ask you to fill out a CLA we'll direct you to [our online CLA
page](https://code.facebook.com/cla) where you can complete it
easily.  We use the same form as the Apache CLA so that friction is minimal.

### Tools

We use a tool called \`arc\` to run tests and perform lint checks.  \`arc\` is part
of [Phabricator](http://www.phabricator.org) and can be installed by following
these steps:

\`\`\`bash
$ mkdir /somewhere
$ cd /somewhere
$ git clone git://github.com/facebook/libphutil.git
$ git clone git://github.com/facebook/arcanist.git
\`\`\`

Add \`arc\` to your path:

\`\`\`bash
$ export PATH="$PATH:/somewhere/arcanist/bin/"
\`\`\`

With \`arc\` in your path, re-running configure will detect it and adjust the
makefile so that \`arc lint\` will be run as part of \`make\`, but you can run it
yourself outside of make.

You can run the unit tests using:

    $ make integration

If you'd like to contribute a patch to watchman, we'll ask you to make sure
that \`make integration\` still passes successfully and we'd ideally like you to
augment the test suite to cover the functionality that you're adding or
changing.

Once you've installed \`arc\`, you can ask it to submit a diff for code review:

    $ arc diff

and it will run the tests and linters for you.  You will need to register on
our [Open Source Phabricator Instance](https://reviews.facebook.net); you
can sign in using either your Facebook or Gighub identity.
`);
  }
});
