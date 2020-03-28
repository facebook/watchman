/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * @format
 */

import React from 'react';
import classnames from 'classnames';
import Layout from '@theme/Layout';
import Link from '@docusaurus/Link';
import useDocusaurusContext from '@docusaurus/useDocusaurusContext';
import useBaseUrl from '@docusaurus/useBaseUrl';
import styles from './styles.module.css';

function Home() {
  const context = useDocusaurusContext();
  const {siteConfig = {}} = context;

  return (
    <Layout title={siteConfig.tagline}>
      <header className={classnames('hero hero--primary', styles.heroBanner)}>
        <div className="container">
          <img
            src="img/logo.png"
            alt="logo"
            style={{
              borderRadius: '90%',
              maxWidth: 180,
            }}
          />
          <h1 className="hero__title">{siteConfig.title}</h1>
          <p className="hero__subtitle">{siteConfig.tagline}</p>
          <div className={styles.buttons}>
            <Link
              className={classnames(
                'button button--secondary button--lg',
                styles.getStarted,
              )}
              to={useBaseUrl('docs/install')}>
              Get Started →
            </Link>
          </div>
          <span className={styles.indexCtasGitHubButtonWrapper}>
            <iframe
              className={styles.indexCtasGitHubButton}
              src="https://ghbtns.com/github-btn.html?user=facebook&amp;repo=watchman&amp;type=star&amp;count=true&amp;size=large"
              width={160}
              height={30}
              title="GitHub Stars"
            />
          </span>
        </div>
      </header>
      <div className="container">
        <div className="row">
          <div className="col col--8 col--offset-2">
            <section className="margin-vert--lg">
              <h2>How does it work?</h2>
              <p>
                Watchman exists to watch files and record when they change. It
                can also trigger actions (such as rebuilding assets) when
                matching files change.
              </p>
            </section>
            <section className="margin-vert--lg">
              <h2>Concepts</h2>
              <ul>
                <li>
                  Watchman can recursively watch one or more directory trees
                  (we call them roots).
                </li>
                <li>
                  Watchman does not follow symlinks. It knows they exist, but
                  they show up the same as any other file in its reporting.
                </li>
                <li>
                  Watchman waits for a root to settle down before it will
                  start to trigger notifications or command execution.
                </li>
                <li>
                  Watchman is conservative, preferring to err on the side of
                  caution; it considers files to be freshly changed when you
                  start to watch them or when it is unsure.
                </li>
                <li>
                  You can query a root for file changes since you last
                  checked, or the current state of the tree
                </li>
                <li>
                  You can subscribe to file changes that occur in a root
                </li>
              </ul>
            </section>
            <section className="margin-vert--lg">
              <h2>Quick Starter</h2>
              <p>
                These two lines establish a watch on a source directory and
                then set up a trigger named buildme that will run a tool named
                minify-css whenever a CSS file is changed. The tool will be
                passed a list of the changed filenames.
              </p>
              <pre>
                $ watchman watch ~/src
                <br />
                # the single quotes around '*.css' are important!
                <br />
                $ watchman -- trigger ~/src buildme '*.css' -- minify-css
                <br />
              </pre>
              <p>
                The output for buildme will land in the Watchman log file
                unless you send it somewhere else.
              </p>
            </section>
          </div>
        </div>
      </div>
    </Layout>
  );
}

export default Home;
