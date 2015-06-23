/**
 * @providesModule IndexLayout
 * @jsx React.DOM
 */

var React = require('React');
var Site = require('Site');
var Marked = require('Marked');
var IndexLayout = React.createClass({
  render: function() {
    var metadata = this.props.metadata;
    var content = this.props.children;
    return (
      <Site>
        <div className="hero">
          <div className="wrap">
            <div className="text"><strong>{metadata.title.split('|')[0]}</strong></div>
            <div className="minitext">
              {metadata.title.split('|')[1]}
            </div>
          </div>
        </div>

        <section className="content wrap">
          <section className="home-section home-getting-started">
            <Marked>{content}</Marked>
          </section>
        </section>
      </Site>
    );
  }
});

module.exports = IndexLayout;
