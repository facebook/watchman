/**
 * @providesModule DocsSidebar
 * @jsx React.DOM
 */

var Metadata = require('Metadata');

var DocsSidebar = React.createClass({
  getCategories: function() {
    var metadatas = Metadata.files.filter(function(metadata) {
      return metadata.layout === 'docs';
    });

    // To prevent runaway craziness
    var total_links = 0;
    var link_limit = 1000;

    // metadata of first article in the sidebar
    var first = null;

    // a hashmap of category_id -> hashmap article_id -> linked state
    var state_by_cat = {};

    // hashmap of category -> category links and info
    var cat_by_name = {};
    // List of categories in desired order
    var categories = [];

    function getCategory(name) {
      if (!name) {
        return null;
      }
      if (!cat_by_name[name]) {
        cat = {
          name: name,
          links: [],
          previous: [],
          articles: [],
          linked_articles: {}
        }
        cat_by_name[name] = cat;
        categories.push(cat);
      }
      if (!state_by_cat[name]) {
        state_by_cat[name] = {}
      }
      return cat_by_name[name];
    }

    function addLink(cat, metadata) {
      if (total_links >= link_limit) {
        return;
      }
      cat.links.push(metadata);
      cat.linked_articles[metadata.id] = true;
    }

    // Pre-populate categories in desired order.
    // Create a file named sidebar.json to define this; it should contain
    // and array of category definition objects:
    // [ {category: "Cat name", first: "first_article_id"} ]
    if (Metadata.sidebar) {
      for (var i = 0; i < Metadata.sidebar.length; i++) {
        var ent = Metadata.sidebar[i];
        var cat = getCategory(ent.category);
        if (!cat) {
          console.log("sidebar entry " + i + " is missing a category");
          continue;
        }

        // Specifying the first article is optional: we'll automatically
        // collect all articles in this category and show them in sorted
        // order if you don't define the thread through them
        if (ent.first) {
          cat.first = ent.first;
        }
      }
    }

    // Figure out the set of categories that were not in any explicit order.
    // We'll add them to the list in sorted order
    var sorted_cat_names = [];
    var sorted_cat_hash = {};
    for (var i = 0; i < metadatas.length; ++i) {
      var metadata = metadatas[i];

      if (cat_by_name[metadata.category]) {
        continue;
      }
      if (sorted_cat_hash[metadata.category]) {
        continue;
      }
      sorted_cat_hash[metadata.category] = true;
      sorted_cat_names.push(metadata.category);
    }
    sorted_cat_names.sort();

    // Now create the remaining categories in the sorted order
    for (var i = 0; i < sorted_cat_names.length; ++i) {
      getCategory(sorted_cat_names[i]);
    }

    // Fill out the per-category map of article_id -> metadata
    for (var i = 0; i < metadatas.length; ++i) {
      var metadata = metadatas[i];
      if (!metadata.category) {
        console.log("article " + metadata.id + " has no category");
        continue;
      }
      var cat = getCategory(metadata.category);
      cat.linked_articles[metadata.id] = false;
      cat.articles[metadata.id] = metadata;
    }

    // For each category, produce a list of articles in an explicit order.
    // This order is set either by specifying the first article_id for
    // the category in the sidebar.json file, or if that is not present,
    // by walking the set of articles in the category and finding the
    // first article with a next and no previous element.
    // Any articles not explicitly threaded will be sorted by article_id
    // and shown after any explicitly ordered articles.

    for (var i = 0; i < categories.length; i++) {
      var cat = categories[i];
      console.log(i + ' ' + cat.name);

      // Generate the backwards mapping for any articles that have a `next`
      for (var j = 0; j < cat.articles.length; ++j) {
        var metadata = cat.articles[j];

        if (metadata.next) {
          var next_id = cat.articles[j].id;
          cat.previous[next_id] = metadata.id;
        }
      }

      if (cat.first && !cat.articles[cat.first]) {
        console.log("category " + cat.name + " wants " + cat.first +
            " to be first, but it doesn't exist");
        cat.first = null;
      }

      if (!cat.first) {
        // If there is no explicit first article for this category,
        // find the first element with a next which doesn't have any previous
        for (var j = 0; j < cat.articles.length; ++j) {
          var metadata = cat.articles[j];
          if (!metadata.next) {
            continue;
          }
          if (!cat.previous[metadata.id]) {
            cat.first = metadata.id;
            break;
          }
        }
      }

      if (cat.first) {
        // Walk the thread
        var id = cat.first;
        while (id) {
          var metadata = cat.articles[id];
          if (!metadata) {
            break;
          }
          addLink(cat, metadata);
          id = metadata.next;
        }
      }

      // Any unlinked articles are collected and sorted and tacked on the end
      var remainder = [];
      for (article_id in cat.linked_articles) {
        if (!cat.linked_articles[article_id]) {
          remainder.push(article_id);
        }
      }
      remainder.sort();
      for (var j = 0; j < remainder.length; ++j) {
        addLink(cat, cat.articles[remainder[j]]);
      }
    }

    return categories;
  },

  render: function() {
    return <div className="nav-docs">
      {this.getCategories().map((category) =>
        <div className="nav-docs-section" key={category.name}>
          <h3>{category.name}</h3>
          <ul>
            {category.links.map((metadata) =>
              <li key={metadata.id}>
                <a
                  style={{marginLeft: metadata.indent ? 20 : 0}}
                  className={metadata.id === this.props.metadata.id ? 'active' : ''}
                  href={'/watchman/' + metadata.permalink}>
                  {metadata.title}
                </a>
              </li>
            )}
          </ul>
        </div>
      )}
    </div>;
  }
});

module.exports = DocsSidebar;
