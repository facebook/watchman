Watchman Java Library
====

This provides Java bindings to the Watchman service.

Building
===

First, you must make sure that you have all the required dependencies. You can
either download them manually (please read the individual README files in each
folder under third-party/), or you can run the auto-downloader:

```
bash deps.sh
```

Make sure that you have [buck](https://buckbuild.com/) installed. In this
folder, run:

```
buck build :watchman
```

The resulting JAR file is found in: 
`buck-out/gen/lib__watchman__output/watchman.jar`

To run the tests:

```
buck test :watchman
```

