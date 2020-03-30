/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * @format
 */

module.exports = {
  title: 'Watchman',
  tagline: 'A file watching service',
  url: 'https://facebook.github.io',
  baseUrl: '/watchman/',
  favicon: 'img/favicon.png',
  organizationName: 'facebook',
  projectName: 'watchman',
  themeConfig: {
    navbar: {
      title: 'Watchman',
      logo: {
        alt: 'Watchman Logo',
        src: 'img/logo.png',
      },
      links: [
        {
          to: 'docs/install',
          activeBasePath: 'docs',
          label: 'Docs',
        },
        {to: 'docs/support', label: 'Support', position: 'left'},
        {
          href: 'https://github.com/facebook/watchman',
          label: 'GitHub',
          position: 'right',
        },
      ],
    },
    footer: {
      style: 'dark',
      logo: {
        alt: 'Facebook Open Source Logo',
        src: 'img/oss_logo.png',
        href: 'https://opensource.facebook.com/',
      },
      // Please do not remove the credits, help to publicize Docusaurus :)
      copyright: `Copyright © ${new Date().getFullYear()} Facebook, Inc. Built with Docusaurus.`,
    },
  },
  presets: [
    [
      '@docusaurus/preset-classic',
      {
        docs: {
          sidebarPath: require.resolve('./sidebars.js'),
          editUrl:
            'https://github.com/facebook/watchman/edit/master/website/',
        },
        theme: {
          customCss: require.resolve('./src/css/custom.css'),
        },
      },
    ],
  ],
};
