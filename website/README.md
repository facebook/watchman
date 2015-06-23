# Run the server

The first time, get all the dependencies loaded via

```
npm install
```

Then, run the server via

```
npm start
Open http://localhost:8080/watchman/index.html
```

Anytime you change the a, just refresh the page and it's going to be updated

# Publish the website

First setup your environment by having two folders, one `watchman` and one `watchman-gh-pages`. The publish script expect those exact names.

```
cd ../../
git clone git@github.com:facebook/watchman.git watchman-gh-pages
cd watchman-gh-pages
git checkout origin/gh-pages
git checkout -b gh-pages
git push --set-upstream origin gh-pages
cd ../watchman/website
```

Then, after you've done changes, just run the command and it'll automatically build the static version of the site and publish it to gh-pages.

```
./publish.sh
```
