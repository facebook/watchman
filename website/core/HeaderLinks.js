/**
 * @providesModule HeaderLinks
 * @jsx React.DOM
 */

var Metadata = require('Metadata');

var HeaderLinks = React.createClass({
  render: function() {
    return (
      <ul className="nav-site">
        {Metadata.navigation.map(function(link) {
          return (
            <li key={link.section}>
              <a
                href={link.href}
                className={link.section === this.props.section ? 'active' : ''}>
                {link.text}
              </a>
            </li>
          );
        }, this)}
      </ul>
    );
  }
});

module.exports = HeaderLinks;
